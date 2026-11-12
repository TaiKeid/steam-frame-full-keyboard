"""Isolated helper lifecycle tests. No Steam endpoint or focused application is used."""
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("native_bridge", Path(sys.argv.pop(1)))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class FakeCDP:
    def __init__(self):
        self.installed = self.disposed = self.closed = False
        self.dismissed = threading.Event()
        self.sent = []
        self.restored = False

    def evaluate(self, expression, timeout=.4, waiting=None):
        if "const bridge =" in expression:
            self.installed = True
            return True
        if ".bridge.dispose(" in expression:
            self.disposed = True
            self.restored = ".bridge.dispose(false)" not in expression
            return None
        if ".bridge.step(" in expression:
            return dict(version=1, request=2, open=True, eligible=True, active=False)
        if ".bridge.send(" in expression:
            arguments = expression.split(".bridge.send(", 1)[1].rsplit(") === true", 1)[0]
            self.sent.append(json.loads("[" + arguments + "]"))
            return True
        if ".bridge.dismiss(" in expression:
            self.dismissed.set()
            return True
        raise AssertionError("unexpected operation")

    def close(self):
        self.closed = True


class FakeGuard(bridge.NativeGuard):
    def __init__(self):
        self.armed = self.closed = False
        self.native_armed = False
        self.prepared = self.cancelled = self.restored = 0
        self.renewals = []  # Guard lease state at each renewal.
        self.reject = set()

    def command(self, op, **fields):
        if op in self.reject:
            return False
        if op == "prepare":
            self.prepared += 1
            assert fields["key"] == "gamescope.gamescope-0.window.62" and fields["ready"]
            if self.native_armed:
                return False  # Match the real guard's single-lease invariant.
            self.native_armed = True
        elif op == "cancel":
            self.cancelled += 1
            self.native_armed = False
        elif op == "restore":
            self.restored += 1
            self.native_armed = False
        elif op == "renew":
            self.renewals.append(self.native_armed)
        return True

    def close(self):
        self.closed = True
        if self.native_armed:
            self.restored += 1  # The real independent guard restores on EOF.
            self.native_armed = self.armed = False


class ExternalCDP(FakeCDP):
    def __init__(self, guard):
        super().__init__()
        self.guard = guard
        self.external = False
        self.fallback = False

    def evaluate(self, expression, timeout=.4, waiting=None):
        if ".bridge.prepareExternal(" in expression:
            assert self.guard.armed, "native may close only after independent recovery is armed"
            self.external = True
        if ".bridge.step(" in expression or ".bridge.prepareExternal(" in expression:
            result = dict(version=1, request=2, open=True, eligible=self.external,
                          active=self.external, local=self.external)
            if not self.external and '"ready": true' in expression:
                result["externalCandidate"] = dict(request=2, key="gamescope.gamescope-0.window.62", flags=13)
            elif self.external:
                result["external"] = dict(request=2, key="gamescope.gamescope-0.window.62")
            if self.dismissed.is_set():
                result = dict(version=1, request=3, open=False, eligible=False, active=False)
            elif self.fallback:
                result = dict(version=1, request=3, open=False, eligible=False, active=False,
                              externalRestore=dict(request=3, key="gamescope.gamescope-0.window.62"))
                self.fallback = False
            return result
        return super().evaluate(expression, timeout)


class Lifecycle(unittest.TestCase):
    def setUp(self):
        # Unit recovery tests use a synthetic PID; never depend on host PID 77.
        self.stopped_state = patch.object(bridge.ParentRecovery, "parent_stopped", return_value=False)
        self.stopped_state.start()
        self.addCleanup(self.stopped_state.stop)

    def receive(self, parent, operation):
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            message = json.loads(parent.recv(32768))
            if message.get("op") == operation:
                return message
            self.assertEqual(message.get("op"), "state", "only status may precede a reply")
        self.fail("helper reply timed out")

    def test_idle_cadence_requires_explicit_consistent_idle_metadata(self):
        self.assertEqual(bridge.poll_interval(dict(idle=True)), .075)
        for state in ({}, dict(idle=False), dict(idle=1), dict(idle="true")):
            self.assertEqual(bridge.poll_interval(state), .035)
        for field in ('open', 'eligible', 'active', 'external', 'externalCandidate', 'externalRestore'):
            self.assertEqual(bridge.poll_interval(dict(idle=True, **{field: True})), .035)

    def test_standby_cadence_slows_only_what_nobody_can_use(self):
        for state in (dict(idle=True), dict(idle=False, open=True)):
            self.assertEqual(bridge.poll_interval(state, True), .25)
        # A handover or restoration keeps its pace, as does unclear metadata.
        for field in ('eligible', 'active', 'external', 'externalCandidate', 'externalRestore'):
            self.assertEqual(bridge.poll_interval(dict(idle=False, open=True, **{field: True}), True), .035)
        for state in ({}, dict(idle=1), dict(idle="true")):
            self.assertEqual(bridge.poll_interval(state, True), .035)
        self.assertEqual(bridge.poll_interval(dict(idle=False, open=True)), .035)
        # A slow reply on top of the wait must stay inside the parent's 0.8 s limit.
        self.assertLess(bridge.STANDBY_POLL + .4, .8)

    def test_waking_from_standby_interrupts_a_slow_poll_wait(self):
        polled = threading.Event()
        class IdleCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if '.bridge.step(' in expression:
                    polled.set()
                    return dict(version=1, request=2, idle=True)
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        def frame(standby):
            return json.dumps(dict(op='frame', progress=1, ready=False, ack=0, sent=time.monotonic(),
                                   standby=standby)).encode()
        seen = []
        def interval(state, standby=False):
            seen.append(standby)
            return 60 if standby else .035  # A wakeup test, not a timing comparison.
        with patch.object(bridge, 'poll_interval', side_effect=interval):
            worker = threading.Thread(target=bridge.run, args=(child, IdleCDP, None, lambda: None))
            worker.start()
            try:
                parent.send(frame(True))
                deadline = time.monotonic() + 2
                while True not in seen and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertIn(True, seen, "a standby frame must reach the poll choice")
                polled.clear()
                time.sleep(.1)
                self.assertFalse(polled.is_set(), "standby keeps the slow wait")
                parent.send(frame(False))
                self.assertTrue(polled.wait(1), "waking must poll at once")
                parent.send(b'{"op":"stop"}')
                worker.join(2)
                self.assertFalse(worker.is_alive())
            finally:
                parent.close()
                worker.join(2)

    def test_run_selects_idle_and_busy_polling_from_each_state(self):
        states = [dict(idle=True), dict(idle=False, open=True),
                  dict(idle=False, externalRestore={"request": 2}), dict(idle=True)]
        class ChangingCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if '.bridge.step(' in expression:
                    return dict(version=1, request=2, **states.pop(0)) if states else dict(version=1)
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        selected = []
        original = bridge.poll_interval
        def interval(state, standby=False):
            value = original(state, standby)
            selected.append(value)
            return value
        with patch.object(bridge, 'poll_interval', side_effect=interval):
            worker = threading.Thread(target=bridge.run, args=(child, ChangingCDP, None, lambda: None))
            worker.start()
            try:
                for _ in range(4):
                    self.receive(parent, 'state')
                parent.send(b'{"op":"stop"}')
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(selected[:4], [.075, .035, .035, .075])
            finally:
                parent.close()
                worker.join(2)

    def test_explicit_fallback_interrupts_an_idle_poll_wait(self):
        fallback_seen = threading.Event()
        class IdleCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if '.bridge.step(' in expression:
                    if '"fallback": true' in expression:
                        fallback_seen.set()
                    return dict(version=1, request=2, idle=True)
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        # A long test-only wait makes this a wakeup test, not a timing comparison.
        with patch.object(bridge, 'poll_interval', return_value=60):
            worker = threading.Thread(target=bridge.run, args=(child, IdleCDP, None, lambda: None))
            worker.start()
            try:
                self.receive(parent, 'state')
                parent.send(json.dumps(dict(op='frame', progress=1, ready=False, ack=0,
                                            sent=time.monotonic(), fallback=True,
                                            fallbackRequest=2)).encode())
                self.assertTrue(fallback_seen.wait(.5), 'explicit fallback bypasses the idle timer')
            finally:
                parent.close()
                worker.join(2)
                self.assertFalse(worker.is_alive())

    def test_slow_step_and_send_do_not_end_helper(self):
        class SlowCDP(FakeCDP):
            step_calls = 0
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    self.step_calls += 1
                    if self.step_calls == 1:
                        raise TimeoutError()
                if ".bridge.send(" in expression or ".bridge.dismiss(" in expression:
                    raise TimeoutError()
                return super().evaluate(expression, timeout)

        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = SlowCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client))
        worker.start()
        try:
            self.receive(parent, "state")
            self.assertGreaterEqual(client.step_calls, 2)
            for operation in ("text", "key"):
                parent.send(json.dumps(dict(op=operation, request=2, id=7, text="fixture", value=1)).encode())
                self.assertEqual(self.receive(parent, operation + "-result"),
                                 dict(op=operation + "-result", id=7, ok=False))
                self.receive(parent, "state")
                self.assertTrue(worker.is_alive())
            parent.send(json.dumps(dict(op="dismiss", request=2)).encode())
            self.receive(parent, "state")
            self.assertTrue(worker.is_alive(), "dismiss timeout uses request fallback")
        finally:
            parent.close()
            worker.join(2)
        self.assertTrue(client.disposed and client.closed)

    def test_a_parent_that_stops_reading_keeps_its_helper_and_results(self):
        # The keyboard's socket is nonblocking, as in production, and each state
        # repeats a full event window. A main loop stalled in a long runtime call
        # stops reading for seconds; the helper must neither fill the socket
        # with states nor lose the result of the key it was asked to send.
        events = [dict(id=1000 + i, request=500 + i, code="release-dismissed") for i in range(32)]
        class EventCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    return dict(version=1, request=2, open=True, eligible=True, active=False,
                                idle=True, events=events)
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_NONBLOCK)
        client = EventCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
        worker.start()
        try:
            parent.send(json.dumps(dict(op="key", request=2, id=9, text="fixture", value=1)).encode())
            time.sleep(5)  # well past the old failure at about 3.7 s of 75 ms polls
            self.assertTrue(worker.is_alive(), "a parent that is not reading does not end the helper")
            self.assertLessEqual(bridge.queued_bytes(child), 2 * bridge.ParentChannel.STATE_QUEUE_LIMIT,
                                 "states stop once a few wait unread")
            received = []
            parent.setblocking(True)
            parent.settimeout(1)
            result_at = None
            while result_at is None or len(received) < result_at + 4:
                received.append(json.loads(parent.recv(32768)))
                if result_at is None and received[-1].get("op") == "key-result":
                    result_at = len(received)
            self.assertIn(dict(op="key-result", id=9, ok=True), received,
                          "the result waits for the parent instead of being dropped")
            self.assertTrue(all(m["op"] in ("state", "key-result") for m in received))
            self.assertEqual(received[-1]["op"], "state", "states continue once it reads again")
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertTrue(client.disposed and client.closed)

    def test_unchanged_idle_states_are_a_quarter_second_heartbeat(self):
        # Hidden and idle, the bridge polls Steam several times a second but
        # nothing changes: the keyboard needs only a heartbeat, not every poll.
        # A change goes out at once.
        state = dict(version=1, request=2, open=False, eligible=False, active=False, idle=True)
        class IdleCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    return dict(state)
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        worker = threading.Thread(target=bridge.run, args=(child, IdleCDP, None, lambda: None))
        worker.start()
        try:
            self.receive(parent, "state")
            started, count = time.monotonic(), 0
            while time.monotonic() - started < 1.0:
                self.receive(parent, "state")
                count += 1
            self.assertLessEqual(count, 6, "about four heartbeats a second, not one per 75 ms poll")
            self.assertGreaterEqual(count, 2, "the heartbeat keeps the keyboard's freshness limit")
            state["request"] = 3
            changed = time.monotonic()
            while self.receive(parent, "state")["request"] != 3:
                pass
            self.assertLess(time.monotonic() - changed, 0.2, "a changed state is not held back")
            parent.send(b'{"op":"stop"}')
            worker.join(2)
            self.assertFalse(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)

    def test_a_delivery_through_a_half_second_steam_stall_succeeds(self):
        # Steam's UI stalls about half a second while it filters search
        # results. A piece typed into such a stall, here one just longer than
        # the 0.7 s parent-freshness limit, is confirmed late but in time: the
        # parent keeps hearing from the helper, nothing falls back, and the
        # parent frames that queued meanwhile still count as fresh.
        steps = []
        class StallingCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    steps.append(json.loads(expression.split(".bridge.step(", 1)[1].rsplit(")", 1)[0]))
                    return dict(version=1, request=2, open=True, eligible=True, active=True)
                if ".bridge.send(" in expression:
                    self.assertLessEqual(.75, timeout)
                    stalled = time.monotonic() + .75
                    while time.monotonic() < stalled:
                        time.sleep(min(bridge.WAIT_HEARTBEAT, stalled - time.monotonic()))
                        waiting()
                return super().evaluate(expression, timeout)
            assertLessEqual = unittest.TestCase.assertLessEqual
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = StallingCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
        worker.start()
        try:
            progress = 0
            def frame():
                nonlocal progress
                progress += 1
                parent.send(json.dumps(dict(op="frame", progress=progress, ready=True, ack=2,
                                            sent=time.monotonic())).encode())
            frame()
            self.receive(parent, "state")
            parent.send(json.dumps(dict(op="text", request=2, id=7, text="fixture")).encode())
            sent, last, gap, result = time.monotonic(), time.monotonic(), 0.0, None
            while result is None and time.monotonic() - sent < 2:
                frame()
                message = json.loads(parent.recv(32768))
                gap, last = max(gap, time.monotonic() - last), time.monotonic()
                if message.get("op") == "text-result":
                    result = message
            self.assertEqual(result, dict(op="text-result", id=7, ok=True), "the stalled delivery is confirmed")
            self.assertLess(gap, .5, "the parent hears from the helper throughout the stall")
            frame()
            self.receive(parent, "state")
            after = [step for step in steps if step["progress"] >= 2]
            self.assertTrue(after and all(step["ready"] for step in after),
                            "frames that queued during the stall are still fresh")
            self.assertFalse(any(step["fallback"] for step in steps), "nothing falls back")
            parent.send(b'{"op":"stop"}')
            worker.join(2)
            self.assertFalse(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)

    def test_a_parent_that_stalls_during_a_slow_delivery_cannot_renew_the_lease(self):
        # The same 0.76 s Steam wait, but the parent sends nothing during it,
        # as when its main loop has stalled. The next poll must not hand Steam
        # its old frame as ready, or the lease would outlive the parent.
        steps = []
        class StallingCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    steps.append(json.loads(expression.split(".bridge.step(", 1)[1].rsplit(")", 1)[0]))
                    return dict(version=1, request=2, open=True, eligible=True, active=True)
                if ".bridge.send(" in expression:
                    stalled = time.monotonic() + .76
                    while time.monotonic() < stalled:
                        time.sleep(min(bridge.WAIT_HEARTBEAT, stalled - time.monotonic()))
                        waiting()
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = StallingCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
        worker.start()
        try:
            parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=2, sent=time.monotonic())).encode())
            self.receive(parent, "state")
            parent.send(json.dumps(dict(op="frame", progress=2, ready=True, ack=2, sent=time.monotonic())).encode())
            parent.send(json.dumps(dict(op="text", request=2, id=7, text="fixture")).encode())
            polled = len(steps)
            self.assertEqual(self.receive(parent, "text-result"), dict(op="text-result", id=7, ok=True))
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline and len(steps) == polled:
                time.sleep(.02)
            self.assertGreater(len(steps), polled, "the helper polls again after the delivery")
            self.assertFalse(steps[-1]["ready"], "the parent's 0.76 s old frame is not ready")
            parent.send(b'{"op":"stop"}')
            worker.join(2)
            self.assertFalse(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)

    def test_only_the_first_state_carries_its_send_time(self):
        # The keyboard's startup deadline counts when the first state was sent.
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        worker = threading.Thread(target=bridge.run, args=(child, FakeCDP, None, lambda: None))
        worker.start()
        try:
            first = self.receive(parent, "state")
            self.assertLess(abs(first["at"] - time.monotonic()), 1, "stamped on the shared monotonic clock")
            self.assertNotIn("at", self.receive(parent, "state"), "later states are not stamped")
            parent.send(b'{"op":"stop"}')
            worker.join(2)
            self.assertFalse(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)

    def test_three_consecutive_poll_timeouts_end_helper(self):
        class SlowCDP(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    raise TimeoutError()
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = SlowCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client))
        worker.start()
        try:
            self.assertEqual(self.receive(parent, "unavailable"),
                             dict(op="unavailable", reason="reply-timeout"))
            worker.join(1)
            self.assertFalse(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)

    def test_install_timeout_has_a_fixed_phase_specific_reason(self):
        class SlowInstall(FakeCDP):
            def evaluate(self, expression, timeout=.4, waiting=None):
                if "const bridge =" in expression:
                    raise TimeoutError("private field text must never be reported")
                return super().evaluate(expression, timeout)
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = SlowInstall()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
        worker.start()
        try:
            self.assertEqual(self.receive(parent, "unavailable"),
                             dict(op="unavailable", reason="install-timeout"))
        finally:
            parent.close()
            worker.join(1)
        self.assertTrue(client.closed and client.disposed, "unconfirmed installation is cleaned up")

    def test_cdp_reply_deadline_and_late_partial_frame(self):
        for partial in (False, True):
            with self.subTest(partial=partial):
                local, peer = socket.socketpair()
                local.settimeout(.3)  # The old inherited connect timeout.
                peer.settimeout(2)
                client = bridge.CDP.__new__(bridge.CDP)
                client.socket, client.pending, client.sequence = local, b"", 0
                client.network_checked = time.monotonic()

                def request():
                    def exact(count):
                        data = b""
                        while len(data) < count:
                            chunk = peer.recv(count - len(data))
                            if not chunk:
                                raise EOFError()
                            data += chunk
                        return data
                    first, second = exact(2)
                    length = second & 127
                    if length == 126:
                        length = struct.unpack("!H", exact(2))[0]
                    elif length == 127:
                        length = struct.unpack("!Q", exact(8))[0]
                    mask, payload = exact(4), exact(length)
                    return json.loads(bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

                def reply(identity):
                    payload = json.dumps(dict(id=identity, result=dict(result=dict(value=True)))).encode()
                    return bytes([0x81, len(payload)]) + payload

                errors = []
                def server():
                    try:
                        first = reply(request()["id"])
                        if partial:
                            peer.sendall(first[:7])
                            time.sleep(.45)
                            peer.sendall(first[7:])
                            peer.sendall(reply(request()["id"]))
                        else:
                            time.sleep(.35)
                            peer.sendall(first)
                    except Exception as error:
                        errors.append(error)
                worker = threading.Thread(target=server)
                worker.start()
                try:
                    if partial:
                        with self.assertRaises(TimeoutError):
                            client.evaluate("fixture")
                        self.assertTrue(client.evaluate("next fixture"), "late id is drained")
                    else:
                        self.assertTrue(client.evaluate("fixture"), "reply after 300 ms is accepted")
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(errors, [])
                finally:
                    client.close()
                    peer.close()
                    worker.join(2)

    def test_cdp_wakes_its_caller_while_a_reply_is_slow(self):
        local, peer = socket.socketpair()
        peer.settimeout(2)
        client = bridge.CDP.__new__(bridge.CDP)
        client.socket, client.pending, client.sequence = local, b"", 0
        client.network_checked = time.monotonic()
        errors = []
        def server():
            # Answer the first command only after a half-second stall; leave
            # the second unanswered.
            try:
                peer.recv(4096)
                time.sleep(.55)
                payload = json.dumps(dict(id=1, result=dict(result=dict(value=True)))).encode()
                peer.sendall(bytes([0x81, len(payload)]) + payload)
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=server)
        worker.start()
        wakes = []
        try:
            self.assertTrue(client.evaluate("fixture", timeout=bridge.DELIVERY_TIMEOUT,
                                            waiting=lambda: wakes.append(time.monotonic())))
            self.assertGreaterEqual(len(wakes), 2, "the caller is woken while Steam is slow")
            with self.assertRaises(TimeoutError):
                client.evaluate("unanswered", timeout=.3, waiting=lambda: None)
            worker.join(2)
            self.assertEqual(errors, [])
        finally:
            client.close()
            peer.close()
            worker.join(2)

    def test_slow_install_gets_its_own_budget_and_stop_never_renews_it(self):
        for stop_early in (False, True):
            with self.subTest(stop_early=stop_early):
                class SlowInstall(FakeCDP):
                    def __init__(self):
                        super().__init__()
                        self.started = threading.Event()
                        self.calls = []

                    def evaluate(self, expression, timeout=.4, waiting=None):
                        self.calls.append((expression, timeout))
                        if "const bridge =" in expression:
                            self.started.set()
                            time.sleep(.6)
                        return super().evaluate(expression, timeout)

                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                parent.settimeout(2)
                client = SlowInstall()
                worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
                worker.start()
                try:
                    self.assertTrue(client.started.wait(1))
                    if not stop_early:
                        self.receive(parent, "state")
                    parent.send(b'{"op":"stop"}')
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                    self.assertTrue(client.installed and client.disposed and client.closed)
                    install = [limit for expression, limit in client.calls if "const bridge =" in expression]
                    self.assertEqual(install, [2.0])
                    ordinary = [limit for expression, limit in client.calls if "const bridge =" not in expression]
                    self.assertTrue(ordinary and all(limit == .4 for limit in ordinary))
                    if stop_early:
                        self.assertFalse(any(".bridge.step(" in expression for expression, _ in client.calls))
                finally:
                    parent.close()
                    worker.join(2)

    def test_cdp_install_deadline_is_longer_but_ordinary_deadline_is_unchanged(self):
        for timeout, delay, succeeds in ((2.0, .6, True), (.4, .6, False), (2.0, 2.2, False)):
            with self.subTest(timeout=timeout):
                local, peer = socket.socketpair()
                client = bridge.CDP.__new__(bridge.CDP)
                client.socket, client.pending, client.sequence = local, b"", 0
                client.network_checked = time.monotonic()
                peer.settimeout(2)
                errors = []

                def server():
                    try:
                        peer.recv(65536)  # One small masked CDP request.
                        time.sleep(delay)
                        payload = json.dumps(dict(id=1, result=dict(result=dict(value=True)))).encode()
                        peer.sendall(bytes([0x81, len(payload)]) + payload)
                    except (BrokenPipeError, ConnectionResetError):
                        pass  # The bounded client may close before the late response.
                    except Exception as error:
                        errors.append(error)
                worker = threading.Thread(target=server)
                worker.start()
                started = time.monotonic()
                try:
                    if succeeds:
                        self.assertTrue(client.evaluate("fixture install", timeout=timeout))
                    else:
                        with self.assertRaises(TimeoutError):
                            if timeout == .4:
                                client.evaluate("ordinary fixture")
                            else:
                                client.evaluate("fixture install", timeout=timeout)
                        self.assertLess(time.monotonic() - started, timeout + .15)
                finally:
                    client.close()
                    worker.join(2)
                    peer.close()
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors, [])

    def test_partial_write_failure_closes_cdp_before_install_cleanup(self):
        class PartialWrite:
            closed = False
            def settimeout(self, timeout):
                pass
            def sendall(self, data):
                raise TimeoutError()
            def close(self):
                self.closed = True
        client = bridge.CDP.__new__(bridge.CDP)
        broken = client.socket = PartialWrite()
        with self.assertRaises(ConnectionError):
            client.frame(1, b"fixture install", time.monotonic() + 2)
        self.assertTrue(broken.closed)
        self.assertIsNone(client.socket, "token cleanup cannot reuse a partially written stream")

    @unittest.skipUnless(shutil.which("node"), "Node required for production install-expression regression")
    def test_install_expressions_reject_late_attempts_and_cleanup_only_their_token(self):
        source = """(function(root) {
            ++root.created; root.hooks++;
            root.clock += root.cost;
            let disposed = false;
            return {expired: () => root.expired, dispose() {
                if (!disposed) { disposed = true; root.hooks--; root.disposed++; }
            }};
        })"""
        payload = {
            "first": bridge.install_expression(source, "first", True, 2000),
            "second": bridge.install_expression(source, "second", True, 4000),
            "cleanupFirst": bridge.cleanup_expression("first"),
            "cleanupSecond": bridge.cleanup_expression("second"),
            "global": bridge.GLOBAL,
        }
        script = r"""
            const assert = require('node:assert/strict'), vm = require('node:vm');
            const p = JSON.parse(require('node:fs').readFileSync(0, 'utf8'));
            const root = {clock: 0, cost: 0, created: 0, disposed: 0, hooks: 0, expired: false};
            root.Date = {now: () => root.clock};
            const context = vm.createContext(root), run = s => vm.runInContext(s, context);
            assert.equal(run(p.first), true); // Reply could be lost after hooks were installed.
            assert.equal(root.hooks, 1);
            run(p.cleanupFirst); assert.equal(root.hooks, 0);
            assert.equal(root[p.global], undefined);
            assert.equal(run(p.first), true);
            assert.equal(run(p.second), false, 'a live owner stays intact');
            root.expired = true;
            assert.equal(run(p.second), true);
            root.expired = false;
            run(p.cleanupFirst);
            assert.equal(root.hooks, 1); assert.equal(root[p.global].token, 'second');
            root.clock = 2500;
            const created = root.created, disposed = root.disposed;
            assert.equal(run(p.first), null, 'queued expired attempt never touches a newer owner');
            assert.equal(root.created, created); assert.equal(root.disposed, disposed);
            run(p.cleanupSecond); assert.equal(root.hooks, 0);
            root.clock = 0; root.cost = 2100;
            assert.equal(run(p.first), null, 'an installation that finishes late disposes itself');
            assert.equal(root.hooks, 0); assert.equal(root[p.global], undefined);
        """
        subprocess.run([shutil.which("node"), "-e", script], input=json.dumps(payload), text=True,
                       check=True, timeout=5)

    @unittest.skipUnless(shutil.which("node"), "Node required for production install-expression regression")
    def test_only_an_update_handoff_install_adopts_an_open_native_keyboard(self):
        source = "(function(root, store, external, adoptOpenMs) { root.adopt.push(adoptOpenMs); " \
                 "return {expired: () => true, dispose() {}}; })"
        payload = [bridge.install_expression(source, "plain", True, 2000),
                   bridge.install_expression(source, "update", True, 2000, adopt_open=True)]
        script = r"""
            const assert = require('node:assert/strict'), vm = require('node:vm');
            const p = JSON.parse(require('node:fs').readFileSync(0, 'utf8'));
            const root = {adopt: []}; root.Date = {now: () => 0};
            const context = vm.createContext(root);
            for (const expression of p) assert.equal(vm.runInContext(expression, context), true);
            assert.deepEqual(root.adopt, [0, 3000]);
        """
        subprocess.run([shutil.which("node"), "-e", script], input=json.dumps(payload), text=True,
                       check=True, timeout=5)

    def test_helper_accepts_only_the_adopt_flag_as_an_extra_argument(self):
        for extra, accepted in (([], True), (["--adopt-open"], True), (["--adopt"], False),
                                (["--adopt-open", "--adopt-open"], False)):
            with patch.object(bridge, "run") as run, patch.object(sys, "argv",
                                                                  ["native_bridge.py", "--fd", "3",
                                                                   "--health-fd", "4", *extra]):
                main = Path(spec.origin).read_text().split('if __name__ == "__main__":', 1)[1]
                namespace = {**vars(bridge), "socket": type("S", (), {"socket": staticmethod(lambda **_: None)}),
                             "signal": type("G", (), {"signal": staticmethod(lambda *_: None), "SIGTERM": 15})}
                if accepted:
                    exec(compile("if True:" + main, spec.origin, "exec"), namespace)
                    self.assertEqual(run.call_args.kwargs["adopt_open"], extra == ["--adopt-open"])
                else:
                    with self.assertRaises(SystemExit) as stop:
                        exec(compile("if True:" + main, spec.origin, "exec"), namespace)
                    self.assertEqual(stop.exception.code, 2)
                    run.assert_not_called()

    def test_debugger_must_be_bound_to_loopback_before_any_connection(self):
        def tables(address, port):
            value = ipaddress.ip_address(address)
            raw = value.packed
            if sys.byteorder == "little":
                raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
            row = f"0: {raw.hex().upper()}:{port:04X} 00000000:0000 0A"
            return {"tcp": "header\n" + (row if value.version == 4 else ""),
                    "tcp6": "header\n" + (row if value.version == 6 else "")}
        for address in ("0.0.0.0", "192.0.2.1", "::", "2001:db8::1", "::ffff:192.0.2.1"):
            for port in (8080, 8081):
                with self.subTest(address=address, port=port):
                    rows = tables(address, port)
                    with patch.object(bridge.Path, "read_text", lambda path: rows[path.name]), \
                            patch.object(bridge, "build_opener") as opener:
                        if port == 8081:
                            bridge.require_loopback_debugger()
                        else:
                            with self.assertRaises(ValueError):
                                bridge.CDP()
                        opener.assert_not_called()
        for address in ("127.0.0.1", "127.0.0.2", "::1", "::ffff:127.0.0.1"):
            rows = tables(address, 8080)
            with patch.object(bridge.Path, "read_text", lambda path: rows[path.name]):
                bridge.require_loopback_debugger()
        with patch.object(bridge.Path, "read_text", side_effect=PermissionError()):
            with self.assertRaises(bridge.BridgeUnavailable):
                bridge.require_loopback_debugger()
        with self.assertRaises(ValueError):
            bridge.NoDebuggerRedirects().redirect_request(None, None, None, None, None,
                                                        "http://192.0.2.1:8080/json/list")

    def test_new_network_exposure_stops_an_existing_debugger_connection(self):
        client = bridge.CDP.__new__(bridge.CDP)
        client.network_checked = 0
        with patch.object(bridge.time, "monotonic", return_value=2), \
                patch.object(bridge, "require_loopback_debugger", side_effect=ValueError()), \
                patch.object(client, "frame") as frame:
            with self.assertRaises(ValueError):
                client.evaluate("fixture metadata")
            frame.assert_not_called()

    def test_missing_steam_shared_context_reports_incompatible_interfaces(self):
        with patch.object(bridge, "require_loopback_debugger"), \
                patch.object(bridge, "build_opener") as opener:
            opener.return_value.open.return_value.__enter__.return_value.read.return_value = b"[]"
            with self.assertRaises(bridge.BridgeUnavailable) as error:
                bridge.CDP()
            self.assertEqual(error.exception.reason, "steam-incompatible")

    def test_unavailable_debugger_disables_replacement_before_installing_hooks(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        def unavailable():
            raise ConnectionRefusedError()
        guard_factory = unittest.mock.Mock()
        worker = threading.Thread(target=bridge.run, args=(child, unavailable, None, guard_factory))
        worker.start()
        try:
            self.assertEqual(self.receive(parent, "unavailable"),
                             dict(op="unavailable", reason="debugger-unavailable"))
            worker.join(1)
            self.assertFalse(worker.is_alive())
            guard_factory.assert_not_called()
        finally:
            parent.close()

    def test_failure_reasons_are_fixed_codes_and_do_not_echo_exceptions(self):
        for reason in ("debugger-blocked", "steam-incompatible", "replacement-busy", "private fixture"):
            with self.subTest(reason=reason):
                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                parent.settimeout(1)
                def unavailable():
                    raise bridge.BridgeUnavailable(reason)
                worker = threading.Thread(target=bridge.run, args=(child, unavailable))
                worker.start()
                try:
                    expected = reason if reason != "private fixture" else "helper-unavailable"
                    self.assertEqual(self.receive(parent, "unavailable"),
                                     dict(op="unavailable", reason=expected))
                finally:
                    parent.close()
                    worker.join(1)
                self.assertFalse(worker.is_alive())

    def test_signal_stopped_original_parent_bypasses_all_graces(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10), \
                patch.object(bridge.ParentRecovery, "parent_stopped", return_value=True) as stopped:
            recovery = bridge.ParentRecovery()
            recovery.resume_until = 40
            self.assertFalse(recovery.check(77, False, 9.9, 9.9, 39, 39))
            self.assertFalse(recovery.check(78, True, 9.9, 9.9, 39, 39))
            stopped.assert_not_called()
            kill.assert_not_called()
            self.assertTrue(recovery.check(77, True, 9.9, 9.9, 39, 39))
            stopped.assert_called_once_with(77)
            kill.assert_called_once_with(77, bridge.signal.SIGKILL)

    def test_proc_state_parser_distinguishes_signal_stop_from_wait_and_trace(self):
        self.stopped_state.stop()
        for state in ("T", "S", "D", "R", "t", "Z", "I"):
            with self.subTest(state=state), patch.object(bridge.Path, "read_text",
                    return_value=f"77 (a tricky ) command) {state} 1 2 3") as read:
                self.assertEqual(bridge.ParentRecovery.parent_stopped(77), state == "T")
                read.assert_called_once()
        for malformed in ("", "77 (unfinished", "77 (empty) "):
            with patch.object(bridge.Path, "read_text", return_value=malformed):
                self.assertFalse(bridge.ParentRecovery.parent_stopped(77))
        for error in (FileNotFoundError(), PermissionError()):
            with patch.object(bridge.Path, "read_text", side_effect=error):
                self.assertFalse(bridge.ParentRecovery.parent_stopped(77))

    def test_changed_parent_is_not_killed_after_state_read(self):
        with patch.object(bridge.os, "getppid", side_effect=(77, 1)), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10), \
                patch.object(bridge.ParentRecovery, "parent_stopped", return_value=True):
            self.assertFalse(bridge.ParentRecovery().check(77, True, 5, 5))
            kill.assert_not_called()

    def test_main_loop_recovery_is_scoped_and_escalates_with_a_live_watchdog(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10):
            recovery = bridge.ParentRecovery()
            self.assertFalse(recovery.check(77, False, 5, 5))
            self.assertFalse(recovery.check(78, True, 5, 5))
            self.assertFalse(recovery.check(77, True, 8, 5))
            kill.assert_not_called()
            self.assertFalse(recovery.check(77, True, 5, 9.9),
                             "a live watchdog cannot excuse a deadlocked main loop")
            kill.assert_called_once_with(77, bridge.signal.SIGTERM)
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10.5):
            self.assertFalse(recovery.check(77, True, 5, 10.5))
            kill.assert_not_called()
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=11):
            self.assertTrue(recovery.check(77, True, 5, 11, 35),
                            "a late slow-work allowance cannot cancel termination")
            kill.assert_called_once_with(77, bridge.signal.SIGKILL)

    def test_explicit_slow_work_is_bounded_and_does_not_protect_a_frozen_watchdog(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill:
            recovery = bridge.ParentRecovery()
            for now in (10, 20, 34.99):
                with patch.object(bridge.time, "monotonic", return_value=now):
                    self.assertFalse(recovery.check(77, True, 5, now, 35))
            kill.assert_not_called()
            with patch.object(bridge.time, "monotonic", return_value=35):
                self.assertFalse(recovery.check(77, True, 5, 35, 1000),
                                 "even a growing allowance cannot extend the frame's hard limit")
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)
            for watchdog in (5, None):
                kill.reset_mock()
                with patch.object(bridge.time, "monotonic", return_value=10):
                    self.assertFalse(bridge.ParentRecovery().check(77, True, 5, watchdog, 35))
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)

    def test_resumed_main_loop_clears_recovery_and_starts_a_new_grace_period(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10):
            recovery = bridge.ParentRecovery()
            recovery.check(77, True, 5, 9.9)
            self.assertIsNotNone(recovery.terminated_at)
            recovery.check(77, True, 9.9, 9.9)
            self.assertIsNone(recovery.terminated_at)
            kill.assert_called_once_with(77, bridge.signal.SIGTERM)
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=14):
            self.assertFalse(recovery.check(77, True, 9.9, 14))
            kill.assert_called_once_with(77, bridge.signal.SIGTERM)

    def test_runtime_wait_is_bounded_even_if_the_overlay_watchdog_is_blocked(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill:
            recovery = bridge.ParentRecovery()
            for now in (10, 20, 34.99):
                with patch.object(bridge.time, "monotonic", return_value=now):
                    self.assertFalse(recovery.check(77, True, 5, 5, 0, 35))
            kill.assert_not_called()
            with patch.object(bridge.time, "monotonic", return_value=35):
                self.assertFalse(recovery.check(77, True, 5, 5, 0, 1000))
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)
            with patch.object(bridge.time, "monotonic", return_value=36):
                self.assertTrue(recovery.check(77, True, 5, 36, 0, 1000))
                self.assertEqual(kill.call_args.args, (77, bridge.signal.SIGKILL))

    def test_suspend_resume_grace_is_fixed_until_main_progress(self):
        recovery = bridge.ParentRecovery()
        recovery.observe_clock(10, 10, 9)
        recovery.observe_clock(11, 111, 9)
        self.assertEqual(recovery.resume_until, 41)
        recovery.observe_clock(20, 220, 9)
        self.assertEqual(recovery.resume_until, 41, "repeat wakes cannot extend a stuck frame's grace")
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill:
            with patch.object(bridge.time, "monotonic", return_value=40.99):
                self.assertFalse(recovery.check(77, True, 9, None))
                kill.assert_not_called()
            with patch.object(bridge.time, "monotonic", return_value=41):
                self.assertFalse(recovery.check(77, True, 9, None))
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)
            with patch.object(bridge.time, "monotonic", return_value=42):
                self.assertTrue(recovery.check(77, True, 9, None))

    def test_completed_frame_ends_resume_grace(self):
        recovery = bridge.ParentRecovery()
        recovery.observe_clock(10, 10, 9)
        recovery.observe_clock(11, 111, 9)
        recovery.observe_clock(14, 114, 13.9)
        self.assertEqual(recovery.resume_until, 0)
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=18):
            self.assertFalse(recovery.check(77, True, 13.9, 18))
            kill.assert_called_once_with(77, bridge.signal.SIGTERM)

    def test_resume_does_not_cancel_termination_already_started(self):
        recovery = bridge.ParentRecovery()
        recovery.observe_clock(10, 10, 5)
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill:
            with patch.object(bridge.time, "monotonic", return_value=10):
                recovery.check(77, True, 5, 10)
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)
            recovery.observe_clock(10.5, 110.5, 5)
            with patch.object(bridge.time, "monotonic", return_value=11):
                self.assertTrue(recovery.check(77, True, 5, 11))
                self.assertEqual(kill.call_args.args, (77, bridge.signal.SIGKILL))

    def test_independent_health_pipe_does_not_renew_render_progress(self):
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        health = bridge.WatchdogProgress(read_fd)
        try:
            with patch.object(bridge.time, "monotonic", return_value=10), \
                    patch.object(bridge.os, "getppid", return_value=77), \
                    patch.object(bridge.os, "kill") as kill:
                recovery = bridge.ParentRecovery()
                # Include a split record, stale ticks and impossible timestamps.
                packet = struct.pack("=ddd", 5, 35, 35)
                os.write(write_fd, packet[:5])
                health.poll()
                self.assertIsNone(health.sent)
                os.write(write_fd, packet[5:])
                for tick in (9.9, 9.8, float("nan"), 99):
                    os.write(write_fd, struct.pack("=ddd", tick, 35, 35))
                health.poll()
                self.assertEqual(health.sent, 9.9, "only fresh increasing watchdog progress counts")
                self.assertEqual(health.slow_until, 35)
                self.assertEqual(health.runtime_until, 35)
                self.assertFalse(recovery.check(77, True, 5, health.sent, health.slow_until))
                kill.assert_not_called()
                self.assertEqual(health.pending, b"")
            with patch.object(bridge.time, "monotonic", return_value=14), \
                    patch.object(bridge.os, "getppid", return_value=77), \
                    patch.object(bridge.os, "kill") as kill:
                health.poll()
                self.assertFalse(recovery.check(77, True, 5, health.sent, health.slow_until))
                kill.assert_called_once_with(77, bridge.signal.SIGTERM)
        finally:
            os.close(write_fd)
            health.close()

    def test_health_pipe_rejects_invalid_slow_work_deadlines(self):
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        health = bridge.WatchdogProgress(read_fd)
        try:
            with patch.object(bridge.time, "monotonic", return_value=20):
                for tick, deadline in ((1, float("nan")), (2, float("inf")), (3, -5), (4, 35)):
                    os.write(write_fd, struct.pack("=ddd", tick, deadline, deadline))
                    health.poll()
                    self.assertEqual(health.sent, tick)
                    self.assertEqual(health.slow_until, 0)
                    self.assertEqual(health.runtime_until, 0)
                os.write(write_fd, struct.pack("=ddd", 5, 35, 35))
                health.poll()
                self.assertEqual(health.slow_until, 35)
                os.write(write_fd, struct.pack("=ddd", 6, 0, 0))
                health.poll()
                self.assertEqual(health.slow_until, 0, "completed frames clear the allowance")
        finally:
            os.close(write_fd)
            health.close()

    def test_external_guard_arms_before_takeover_and_cancels_on_x(self):
        self.external_lifecycle(False)

    def test_slow_dispatch_coalesces_repeats_and_release_preserves_distinct_clicks(self):
        for release in (False, True):
            with self.subTest(release=release):
                class DelayedCDP(FakeCDP):
                    def __init__(self):
                        super().__init__()
                        self.blocked = threading.Event()
                        self.resume = threading.Event()
                        self.deliveries = []

                    def evaluate(self, expression, timeout=.4, waiting=None):
                        if ".bridge.send(" in expression:
                            arguments = expression.split(".bridge.send(", 1)[1].rsplit(") === true", 1)[0]
                            request, text, key = json.loads("[" + arguments + "]")
                            assert type(key) is bool
                            if text == "fixture barrier":
                                self.blocked.set()
                                assert self.resume.wait(1), "fixture dispatch unblocked"
                            if request != 2:
                                return False
                            self.deliveries.append((text, key))
                            return True
                        return super().evaluate(expression, timeout)

                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                parent.settimeout(1)
                client = DelayedCDP()
                worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None))
                worker.start()

                def packet(**fields):
                    parent.send(json.dumps(fields).encode())

                try:
                    packet(op="frame", progress=1, ready=True, ack=2, sent=time.monotonic())
                    self.receive(parent, "state")
                    packet(op="key", request=2, id=1, value=1, text="Backspace")
                    self.assertTrue(self.receive(parent, "key-result")["ok"])
                    packet(op="text", request=2, id=90, text="fixture barrier")
                    self.assertTrue(client.blocked.wait(1))
                    # Model queued input while Steam's preceding call is delayed.
                    for _ in range(6):
                        packet(op="key", request=2, id=1, value=2, text="Backspace")
                    if release:
                        packet(op="key-up", request=2, id=1)
                    for key_id, request in ((2, 2), (3, 2), (4, 1)):
                        packet(op="key", request=request, id=key_id, value=1, text="ArrowLeft")
                        packet(op="key-up", request=request, id=key_id)
                    client.resume.set()
                    self.assertEqual(self.receive(parent, "text-result")["id"], 90)
                    # Releasing a queued repeat must stop deletion AND settle
                    # its outstanding delivery. Otherwise the parent times out
                    # and replaces a healthy keyboard with native.
                    expected = [1, 2, 3, 4]
                    for key_id in expected:
                        result = self.receive(parent, "key-result")
                        self.assertEqual(result, dict(op="key-result", id=key_id, ok=key_id != 4))
                    self.assertEqual(client.deliveries,
                                     [("Backspace", True), ("fixture barrier", False)] +
                                     ([] if release else [("Backspace", True)]) +
                                     [("ArrowLeft", True), ("ArrowLeft", True)],
                                     "release drops repeats, retains distinct clicks and rejects stale targets")
                finally:
                    client.resume.set()
                    parent.close()
                    worker.join(2)
                self.assertFalse(worker.is_alive())

    def test_stream_request_is_typed_through_the_private_channel(self):
        stream_key = "steamlink_openvr-overlay"

        class StreamGuard(FakeGuard):
            def command(self, op, **fields):
                if op == "prepare":
                    self.prepared += 1
                    assert fields["key"] == stream_key and fields["ready"]
                    self.native_armed = True
                    return True
                return super().command(op, **fields)

        class StreamCDP(FakeCDP):
            def __init__(self, guard):
                super().__init__()
                self.guard = guard
                self.taken = False
                self.refused = 0
                self.request = 2  # Each new session has the next request number, as in the bridge.
                self.gone = []
                self.fallbacks = []

            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    frame = json.loads(expression.split(".bridge.step(", 1)[1].rsplit(")", 1)[0])
                    if frame.get("streamGone") is not None:
                        self.gone.append(frame["streamGone"])
                    if frame.get("fallback"):
                        self.fallbacks.append(frame.get("fallbackRequest"))
                if ".bridge.prepareExternal(" in expression:
                    if expression.endswith(",false)"):
                        self.refused += 1
                    else:
                        assert self.guard.armed, "native may close only after independent recovery is armed"
                        self.taken = True
                if ".bridge.step(" in expression or ".bridge.prepareExternal(" in expression:
                    result = dict(version=1, request=self.request, open=True, eligible=self.taken,
                                  active=self.taken, local=False, stream=self.taken)
                    if self.taken:
                        result["external"] = dict(request=self.request, key=stream_key)
                    elif '"ready": true' in expression:
                        result["externalCandidate"] = dict(request=self.request, key=stream_key, flags=13)
                    return result
                return super().evaluate(expression, timeout)

        # The socket path has to be short, so the directory sits in the runtime directory when there is one.
        with tempfile.TemporaryDirectory(dir=os.environ.get("XDG_RUNTIME_DIR")) as directory:
            guard = StreamGuard()
            client = StreamCDP(guard)
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            parent.settimeout(1)
            worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard),
                                      kwargs=dict(stream_factory=lambda: bridge.StreamChannel(directory, programs=None)))
            worker.start()

            def packet(**fields):
                parent.send(json.dumps(fields).encode())

            def frames_until(condition):
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline and not condition():
                    packet(op="frame", progress=1, ready=True, ack=2, sent=time.monotonic())
                    time.sleep(.05)
                self.assertTrue(condition())

            def drain_states():
                parent.setblocking(False)
                try:
                    while True:
                        self.assertEqual(json.loads(parent.recv(32768)).get("op"), "state")
                except BlockingIOError:
                    pass
                finally:
                    parent.settimeout(1)

            library = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            library.settimeout(1)
            try:
                # No streaming client has connected: the request is refused and stays native.
                frames_until(lambda: client.refused >= 2)
                self.assertEqual((guard.prepared, client.taken), (0, False))
                # The streaming client's library connects and says hello; the next request is taken.
                library.connect(os.path.join(directory, bridge.STREAM_SOCKET))
                library.send(b"H" + bytes((bridge.STREAM_PROTOCOL,)) + struct.pack("<I", os.getpid()))
                frames_until(lambda: client.taken)
                self.assertEqual(guard.prepared, 1)
                packet(op="frame", progress=2, ready=True, ack=2, sent=time.monotonic())
                time.sleep(.1)  # One poll reports the stream session back to the helper.
                drain_states()
                packet(op="text", request=2, id=1, text="a\u0416")
                self.assertTrue(self.receive(parent, "text-result")["ok"])
                self.assertEqual(library.recv(4096), b"T" + "a\u0416".encode())
                packet(op="key", request=2, id=2, value=1, text="Backspace")
                self.assertTrue(self.receive(parent, "key-result")["ok"])
                self.assertEqual([library.recv(64) for _ in range(2)],
                                 [b"K" + struct.pack("<HHB", 42, 0, 1), b"K" + struct.pack("<HHB", 42, 0, 0)])
                packet(op="key-up", request=2, id=2)
                packet(op="key", request=2, id=3, value=1, text="ShiftEnter")
                self.assertTrue(self.receive(parent, "key-result")["ok"])
                self.assertEqual([library.recv(64) for _ in range(4)],
                                 [b"K" + struct.pack("<HHB", *event)
                                  for event in ((225, 1, 1), (40, 1, 1), (40, 1, 0), (225, 0, 0))])
                packet(op="key-up", request=2, id=3)
                # Real key presses by Linux key code: Ctrl+C, a repeat, and an unknown code.
                for code, value in ((29, 1), (46, 1), (46, 0), (29, 0), (30, 1), (30, 2), (30, 0), (600, 1)):
                    packet(op="stream-key", request=2, code=code, value=value)
                self.assertEqual([library.recv(64) for _ in range(8)],
                                 [b"K" + struct.pack("<HHB", *event)
                                  for event in ((224, 0x40, 1), (6, 0x40, 1), (6, 0x40, 0), (224, 0, 0),
                                                (4, 0, 1), (4, 0, 0), (4, 0, 1), (4, 0, 0))])
                # The session ends with Shift still down: the library is told to let go of everything.
                packet(op="stream-key", request=2, code=42, value=1)
                self.assertEqual(library.recv(64), b"K" + struct.pack("<HHB", 225, 1, 1))
                client.request = 3
                client.taken = False
                frames_until(lambda: client.taken)
                self.assertEqual(library.recv(64), b"R")
                packet(op="frame", progress=4, ready=True, ack=3, sent=time.monotonic())
                time.sleep(.1)
                drain_states()
                self.assertEqual(client.sent, [], "nothing typed into a stream goes through Steam")
                packet(op="key", request=3, id=4, value=1, text="VKClose")
                self.assertTrue(self.receive(parent, "key-result")["ok"])
                self.assertEqual(client.sent, [[3, "VKClose", True]], "only the close key reaches the bridge")
                packet(op="key-up", request=3, id=4)
                # The streaming client goes away: typing fails closed, and the bridge is told once to end
                # the session as a cleared target, with no fallback to a keyboard for a stream that is gone.
                library.close()
                packet(op="frame", progress=5, ready=True, ack=3, sent=time.monotonic())
                packet(op="text", request=3, id=5, text="b")
                self.assertFalse(self.receive(parent, "text-result")["ok"])
                packet(op="stream-key", request=3, code=30, value=1)
                frames_until(lambda: client.gone == [3])
                time.sleep(.1)
                self.assertEqual((client.gone, client.fallbacks), ([3], []))
            finally:
                library.close()
                parent.close()
                worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertFalse(os.path.exists(os.path.join(directory, bridge.STREAM_SOCKET)),
                             "the helper removes its socket on exit")

    def test_stream_click_cancels_the_window_guard_only_when_the_bridge_follows_it(self):
        class ClickCDP(ExternalCDP):
            def __init__(self, guard):
                super().__init__(guard)
                self.follow = False
                self.clicks = 0

            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression and '"streamClick": true' in expression:
                    self.clicks += 1
                    if self.follow:
                        self.stream = True
                if getattr(self, "stream", False) and ".bridge.step(" in expression:
                    return dict(version=1, request=3, open=True, eligible=True, active=True, local=False,
                                stream=True, external=dict(request=3, key="steamlink_openvr-overlay"))
                return super().evaluate(expression, timeout)

        with tempfile.TemporaryDirectory(dir=os.environ.get("XDG_RUNTIME_DIR")) as directory:
            guard = FakeGuard()
            client = ClickCDP(guard)
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            parent.settimeout(1)
            worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard),
                                      kwargs=dict(stream_factory=lambda: bridge.StreamChannel(directory, programs=None)))
            worker.start()
            library = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)

            def frames_until(condition):
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline and not condition():
                    parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=2, shown=True,
                                                sent=time.monotonic())).encode())
                    time.sleep(.03)
                    parent.setblocking(False)
                    try:
                        while True:
                            parent.recv(32768)
                    except BlockingIOError:
                        pass
                    finally:
                        parent.settimeout(1)
                self.assertTrue(condition())

            try:
                frames_until(lambda: client.external and guard.armed)
                library.connect(os.path.join(directory, bridge.STREAM_SOCKET))
                library.send(b"H" + bytes((bridge.STREAM_PROTOCOL,)) + struct.pack("<I", os.getpid()))
                # The bridge does not follow the click (Steam's keyboard was open, say): the window's
                # session goes on, and so does its guard.
                library.send(b"F")
                frames_until(lambda: client.clicks == 1)
                later = time.monotonic() + .2
                frames_until(lambda: time.monotonic() >= later)
                self.assertEqual((guard.armed, guard.cancelled), (True, 0))
                # It follows the next one: typing is in the stream, and the window's guard goes.
                client.follow = True
                library.send(b"F")
                frames_until(lambda: guard.cancelled == 1)
                self.assertFalse(guard.armed)
            finally:
                library.close()
                parent.close()
                worker.join(2)
            self.assertFalse(worker.is_alive())

    def test_only_the_streaming_client_may_connect_and_it_is_never_replaced(self):
        def connect(directory):
            library = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            library.connect(os.path.join(directory, bridge.STREAM_SOCKET))
            library.send(b"H" + bytes((bridge.STREAM_PROTOCOL,)) + struct.pack("<I", os.getpid()))
            return library

        with tempfile.TemporaryDirectory(dir=os.environ.get("XDG_RUNTIME_DIR")) as directory:
            # This test's own program is not Steam's streaming client: refused.
            channel = bridge.StreamChannel(directory)
            library = connect(directory)
            channel.service()
            self.assertFalse(channel.connected(), "another program of the same user is refused")
            library.close()
            channel.close()
            # Named as an allowed program it connects; a second client is refused while it is there,
            # and taken once it has gone.
            channel = bridge.StreamChannel(directory, programs=(os.path.basename(os.readlink("/proc/self/exe")),))
            first = connect(directory)
            channel.service()
            self.assertTrue(channel.connected(), "the allowed program connects")
            second = connect(directory)
            channel.service()
            self.assertTrue(channel.send_text("a"))
            self.assertEqual(first.recv(64), b"Ta", "a live client is never replaced")
            first.close()
            third = connect(directory)
            channel.service()
            self.assertTrue(channel.send_text("b"))
            self.assertEqual(third.recv(64), b"Tb", "a new client is taken once the old one has gone")
            for library in (second, third):
                library.close()
            channel.close()

    def test_click_on_the_stream_is_reported_only_while_the_panel_shows(self):
        class ClickCDP(FakeCDP):
            def __init__(self):
                super().__init__()
                self.clicks = []

            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.step(" in expression:
                    frame = json.loads(expression.split(".bridge.step(", 1)[1].rsplit(")", 1)[0])
                    if frame.get("streamClick"):
                        self.clicks.append(frame.get("shown"))
                return super().evaluate(expression, timeout)

        with tempfile.TemporaryDirectory(dir=os.environ.get("XDG_RUNTIME_DIR")) as directory:
            client = ClickCDP()
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            parent.settimeout(1)
            worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: None),
                                      kwargs=dict(stream_factory=lambda: bridge.StreamChannel(directory, programs=None)))
            worker.start()
            library = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)

            def frames(shown, seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=2, shown=shown,
                                                sent=time.monotonic())).encode())
                    time.sleep(.03)
                    parent.setblocking(False)
                    try:
                        while True:
                            parent.recv(32768)
                    except BlockingIOError:
                        pass
                    finally:
                        parent.settimeout(1)

            try:
                frames(False, .2)
                library.connect(os.path.join(directory, bridge.STREAM_SOCKET))
                library.send(b"H" + bytes((bridge.STREAM_PROTOCOL,)) + struct.pack("<I", os.getpid()))
                frames(False, .2)
                library.send(b"F")
                frames(False, .3)
                self.assertEqual(client.clicks, [], "a click with the panel hidden is not reported")
                frames(True, .2)
                library.send(b"F")
                frames(True, .3)
                self.assertEqual(client.clicks, [True], "a click with the panel showing is reported once")
            finally:
                library.close()
                parent.close()
                worker.join(2)
            self.assertFalse(worker.is_alive())

    def test_external_guard_restores_on_eof(self):
        self.external_lifecycle(True)

    def test_update_handoff_reports_guard_refusal_without_acquiring_input(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        guard = FakeGuard()
        guard.reject.add("prepare")

        class RefusedCDP(ExternalCDP):
            refused = False

            def evaluate(self, expression, timeout=.4, waiting=None):
                if ".bridge.prepareExternal(" in expression:
                    self.assert_refused = expression.endswith(",false)")
                    assert self.assert_refused and not guard.armed
                    self.refused = True
                if self.refused and (".bridge.step(" in expression or ".bridge.prepareExternal(" in expression):
                    return dict(version=1, request=2, open=False, eligible=False, active=False)
                return super().evaluate(expression, timeout)

        client = RefusedCDP(guard)
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard),
                                  kwargs={"adopt_open": True})
        worker.start()
        try:
            parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=0,
                                        sent=time.monotonic())).encode())
            while self.receive(parent, "state").get("open"):
                pass
            self.assertTrue(client.refused)
            self.assertFalse(guard.armed)
            self.assertFalse(client.external)
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())

    def test_guard_renews_only_while_armed(self):
        # An unarmed guard ignores renewals, so frames must not cost a guard
        # round trip until a local request arms it, nor after it is cancelled.
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        guard = FakeGuard()
        client = ExternalCDP(guard)
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard))
        worker.start()

        def frames(first, ready, barrier):
            for progress in range(first, first + 3):
                parent.send(json.dumps(dict(op="frame", progress=progress, ready=ready, ack=0,
                                            sent=time.monotonic())).encode())
            # Packets are handled in order; the result proves the frames were read.
            parent.send(json.dumps(dict(op="text", request=2, id=barrier, text="barrier")).encode())
            self.assertEqual(self.receive(parent, "text-result")["id"], barrier)

        try:
            frames(1, False, 1)
            self.assertEqual(guard.renewals, [], "unarmed guard receives no per-frame renewals")
            parent.send(json.dumps(dict(op="frame", progress=4, ready=True, ack=0,
                                        sent=time.monotonic())).encode())
            while not self.receive(parent, "state").get("local"):
                pass
            self.assertEqual(guard.prepared, 1)
            frames(5, True, 2)
            self.assertEqual(guard.renewals, [True] * 3, "armed guard renews on every frame")
            parent.send(json.dumps(dict(op="dismiss", request=2)).encode())
            while self.receive(parent, "state")["open"]:
                pass
            self.assertEqual(guard.cancelled, 1)
            frames(8, True, 3)
            self.assertEqual(guard.renewals, [True] * 3, "cancelled guard receives no renewals")
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(guard.restored, 0)

    def test_update_shutdown_discards_queued_input_without_summoning_native(self):
        for local in (False, True):
            with self.subTest(local=local):
                guard = FakeGuard()
                base = ExternalCDP if local else FakeCDP

                class PausedCDP(base):
                    def __init__(self):
                        super().__init__(guard) if local else super().__init__()
                        self.pause = False
                        self.waiting, self.resume = threading.Event(), threading.Event()

                    def evaluate(self, expression, timeout=.4, waiting=None):
                        if self.pause and '.bridge.step(' in expression:
                            self.waiting.set()
                            assert self.resume.wait(2), 'fixture poll was not released'
                        return super().evaluate(expression, timeout)

                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                parent.settimeout(1)
                client = PausedCDP()
                worker = threading.Thread(target=bridge.run,
                    args=(child, lambda: client, None, lambda: guard))
                worker.start()
                try:
                    parent.send(json.dumps(dict(op='frame', progress=1, ready=True, ack=2,
                                                sent=time.monotonic())).encode())
                    while not self.receive(parent, 'state').get('local', not local):
                        pass
                    client.pause = True
                    self.assertTrue(client.waiting.wait(1))
                    for message in (dict(op='dismiss', request=2),
                                    dict(op='text', request=2, id=1, text='retired input'),
                                    dict(op='stop', restore=False)):
                        parent.send(json.dumps(message).encode())
                    client.resume.set()
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(client.sent, [], 'update cannot deliver queued input')
                    self.assertFalse(client.restored, 'update cannot reopen Steam native')
                    self.assertEqual(guard.restored, 0, 'update cannot reopen local native')
                    self.assertFalse(guard.native_armed, 'guard EOF must not reopen native later')
                    self.assertTrue(client.disposed and client.closed and guard.closed)
                finally:
                    client.resume.set()
                    parent.close()
                    worker.join(2)

    def test_only_explicit_update_shutdown_suppresses_native_recovery(self):
        for stop in (dict(op='stop'), dict(op='stop', restore=True),
                     dict(op='stop', restore=0), dict(op='stop', restore='false')):
            with self.subTest(stop=stop):
                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                client = FakeCDP()
                parent.send(json.dumps(stop).encode())
                worker = threading.Thread(target=bridge.run,
                    args=(child, lambda: client, None, FakeGuard))
                worker.start()
                worker.join(2)
                parent.close()
                self.assertFalse(worker.is_alive())
                self.assertTrue(client.disposed and client.closed and client.restored,
                                'ordinary stop and invalid dispositions retain safe recovery')

    def test_failed_update_cleanup_retains_independent_native_recovery(self):
        for failure in ('steam-disconnected', 'guard-cancel-failed'):
            with self.subTest(failure=failure):
                guard = FakeGuard()
                if failure == 'guard-cancel-failed':
                    guard.reject.add('cancel')

                class FailingCDP(ExternalCDP):
                    def evaluate(self, expression, timeout=.4, waiting=None):
                        if failure == 'steam-disconnected' and '.bridge.dispose(false)' in expression:
                            raise ConnectionError('fixture cleanup failure')
                        return super().evaluate(expression, timeout)

                parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                parent.settimeout(1)
                client = FailingCDP(guard)
                worker = threading.Thread(target=bridge.run,
                    args=(child, lambda: client, None, lambda: guard))
                worker.start()
                try:
                    parent.send(json.dumps(dict(op='frame', progress=1, ready=True, ack=2,
                                                sent=time.monotonic())).encode())
                    while not self.receive(parent, 'state').get('local'):
                        pass
                    parent.send(b'{"op":"stop","restore":false}')
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(guard.restored, 1,
                                     'failed update cleanup must leave native usable')
                    self.assertTrue(client.closed and guard.closed)
                finally:
                    parent.close()
                    worker.join(2)

    def test_guard_state_changes_only_after_acknowledged_commands(self):
        guard = FakeGuard()
        candidate = dict(key="gamescope.gamescope-0.window.62", flags=13)
        frame = dict(progress=1, sent=time.monotonic(), ready=True)
        self.assertTrue(guard.prepare(candidate, frame))
        self.assertFalse(guard.prepare(candidate, frame))
        self.assertTrue(guard.armed and guard.native_armed,
                        "rejected preparation cannot erase the previous lease")
        for command in ("cancel", "restore"):
            guard.reject.add(command)
            with self.assertRaises(ValueError):
                getattr(guard, command)()
            self.assertTrue(guard.armed and guard.native_armed)
            guard.reject.clear()

    def test_selection_handoff_requires_native_active_overlay_check(self):
        guard = FakeGuard()
        frame = dict(progress=1, sent=time.monotonic(), ready=True)
        for active in (False, True):
            guard.command = unittest.mock.Mock(return_value=True)
            self.assertTrue(guard.prepare(dict(key="gamescope.gamescope-0.window.62", flags=13,
                                              requireActive=active), frame))
            self.assertIs(guard.command.call_args.kwargs["requireActive"], active,
                          "selection candidates must retain their independent active-target requirement")

    def test_external_resummon_between_polls_rearms_guard(self):
        guard = FakeGuard()

        class ResummonCDP(ExternalCDP):
            generation = 2
            resummon = threading.Event()

            def evaluate(self, expression, timeout=.4, waiting=None):
                if self.resummon.is_set() and ".bridge.step(" in expression:
                    self.resummon.clear()
                    self.external = False
                    self.generation += 2
                result = super().evaluate(expression, timeout)
                if isinstance(result, dict):
                    result["request"] = self.generation
                    for name in ("external", "externalCandidate"):
                        if result.get(name):
                            result[name]["request"] = self.generation
                return result

        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = ResummonCDP(guard)
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard))
        worker.start()
        try:
            parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=0,
                                        sent=time.monotonic())).encode())
            while not self.receive(parent, "state").get("local"):
                pass
            for generation in (4, 6):
                client.resummon.set()
                while True:
                    state = self.receive(parent, "state")
                    if state["request"] == generation:
                        break
                self.assertTrue(state.get("local"), "the next request acquires a new guard")
                self.assertTrue(guard.armed and guard.native_armed)
            self.assertEqual((guard.prepared, guard.cancelled), (3, 2))
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(guard.restored, 1)

    def external_lifecycle(self, leave_open):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        guard = FakeGuard()
        client = ExternalCDP(guard)
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client, None, lambda: guard))
        worker.start()
        try:
            parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=0,
                                        sent=time.monotonic())).encode())
            while not self.receive(parent, "state").get("local"):
                pass
            self.assertEqual(guard.prepared, 1)
            if not leave_open:
                parent.send(json.dumps(dict(op="dismiss", request=2)).encode())
                while self.receive(parent, "state")["open"]:
                    pass
                self.assertEqual(guard.cancelled, 1)
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertTrue(guard.closed and client.disposed)
        self.assertEqual(guard.restored, int(leave_open))

    def test_parent_eof_restores_and_exits(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = FakeCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client))
        worker.start()
        try:
            parent.send(json.dumps(dict(op="frame", progress=1, ready=True, ack=0, fallback=False,
                                        sent=time.monotonic())).encode())
            self.assertEqual(self.receive(parent, "state")["op"], "state")
            parent.send(json.dumps(dict(op="text", request=2, id=7, text="café 世界")).encode())
            result = self.receive(parent, "text-result")
            self.assertEqual(result, dict(op="text-result", id=7, ok=True))
            for operation in ("text", "key"):
                parent.send(json.dumps(dict(op=operation, request=2, id=8, text="Enter", value=1)).encode())
                self.assertTrue(self.receive(parent, operation + "-result")["ok"])
            self.assertEqual(client.sent, [[2, "café 世界", False], [2, "Enter", False], [2, "Enter", True]],
                             "literal key names remain text; controls carry an explicit key flag")
            parent.send(json.dumps(dict(op="dismiss", request=2)).encode())
            self.assertTrue(client.dismissed.wait(1), "dismissal reaches Steam without ending the helper")
            self.assertTrue(worker.is_alive())
        finally:
            parent.close()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertTrue(client.installed and client.disposed and client.closed)

    def test_invalid_packet_restores_without_echoing_input(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        client = FakeCDP()
        worker = threading.Thread(target=bridge.run, args=(child, lambda: client))
        worker.start()
        try:
            parent.send(b'{"op":"unknown","text":"private fixture"}')
            message = self.receive(parent, "unavailable")
            self.assertEqual(message, dict(op="unavailable", reason="connection-lost"))
        finally:
            parent.close()
            worker.join(2)
        self.assertTrue(client.disposed and client.closed)


if __name__ == "__main__":
    unittest.main()
