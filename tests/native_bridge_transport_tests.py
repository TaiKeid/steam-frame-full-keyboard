"""Isolated helper lifecycle tests. No Steam endpoint or focused application is used."""
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import socket
import struct
import sys
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

    def evaluate(self, expression):
        if "const bridge =" in expression:
            self.installed = True
            return True
        if ".bridge.dispose()" in expression:
            self.disposed = True
            return None
        if ".bridge.step(" in expression:
            return dict(version=1, request=2, open=True, eligible=True, active=False)
        if ".bridge.send(" in expression:
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
        return True

    def close(self):
        self.closed = True


class ExternalCDP(FakeCDP):
    def __init__(self, guard):
        super().__init__()
        self.guard = guard
        self.external = False
        self.fallback = False

    def evaluate(self, expression):
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
        return super().evaluate(expression)


class Lifecycle(unittest.TestCase):
    def receive(self, parent, operation):
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            message = json.loads(parent.recv(32768))
            if message.get("op") == operation:
                return message
            self.assertEqual(message.get("op"), "state", "only status may precede a reply")
        self.fail("helper reply timed out")

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
                        with self.assertRaises(ValueError):
                            bridge.CDP()
                        opener.assert_not_called()
        for address in ("127.0.0.1", "127.0.0.2", "::1", "::ffff:127.0.0.1"):
            rows = tables(address, 8080)
            with patch.object(bridge.Path, "read_text", lambda path: rows[path.name]):
                bridge.require_loopback_debugger()
        with patch.object(bridge.Path, "read_text", side_effect=PermissionError()):
            with self.assertRaises(PermissionError):
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

    def test_unavailable_debugger_disables_replacement_before_installing_hooks(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(1)
        def unavailable():
            raise ConnectionRefusedError()
        guard_factory = unittest.mock.Mock()
        worker = threading.Thread(target=bridge.run, args=(child, unavailable, None, guard_factory))
        worker.start()
        try:
            self.assertEqual(self.receive(parent, "unavailable"), dict(op="unavailable"))
            worker.join(1)
            self.assertFalse(worker.is_alive())
            guard_factory.assert_not_called()
        finally:
            parent.close()

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

    def test_independent_health_pipe_does_not_renew_render_progress(self):
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        health = bridge.WatchdogProgress(read_fd)
        try:
            with patch.object(bridge.time, "monotonic", return_value=10), \
                    patch.object(bridge.os, "getppid", return_value=77), \
                    patch.object(bridge.os, "kill") as kill:
                recovery = bridge.ParentRecovery()
                # Include a split record, stale ticks and impossible timestamps.
                packet = struct.pack("=dd", 5, 35)
                os.write(write_fd, packet[:5])
                health.poll()
                self.assertIsNone(health.sent)
                os.write(write_fd, packet[5:])
                for tick in (9.9, 9.8, float("nan"), 99):
                    os.write(write_fd, struct.pack("=dd", tick, 35))
                health.poll()
                self.assertEqual(health.sent, 9.9, "only fresh increasing watchdog progress counts")
                self.assertEqual(health.slow_until, 35)
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
                    os.write(write_fd, struct.pack("=dd", tick, deadline))
                    health.poll()
                    self.assertEqual(health.sent, tick)
                    self.assertEqual(health.slow_until, 0)
                os.write(write_fd, struct.pack("=dd", 5, 35))
                health.poll()
                self.assertEqual(health.slow_until, 35)
                os.write(write_fd, struct.pack("=dd", 6, 0))
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

                    def evaluate(self, expression):
                        if ".bridge.send(" in expression:
                            arguments = expression.split(".bridge.send(", 1)[1].rsplit(") === true", 1)[0]
                            request, text = json.loads("[" + arguments + "]")
                            if text == "fixture barrier":
                                self.blocked.set()
                                assert self.resume.wait(1), "fixture dispatch unblocked"
                            if request != 2:
                                return False
                            self.deliveries.append(text)
                            return True
                        return super().evaluate(expression)

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
                    expected = [2, 3, 4] if release else [1, 2, 3, 4]
                    for key_id in expected:
                        result = self.receive(parent, "key-result")
                        self.assertEqual(result, dict(op="key-result", id=key_id, ok=key_id != 4))
                    self.assertEqual(client.deliveries,
                                     ["Backspace", "fixture barrier"] +
                                     ([] if release else ["Backspace"]) + ["ArrowLeft", "ArrowLeft"],
                                     "release drops repeats, retains distinct clicks and rejects stale targets")
                finally:
                    client.resume.set()
                    parent.close()
                    worker.join(2)
                self.assertFalse(worker.is_alive())

    def test_external_guard_restores_on_eof(self):
        self.external_lifecycle(True)

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

    def test_external_resummon_between_polls_rearms_guard(self):
        guard = FakeGuard()

        class ResummonCDP(ExternalCDP):
            generation = 2
            resummon = threading.Event()

            def evaluate(self, expression):
                if self.resummon.is_set() and ".bridge.step(" in expression:
                    self.resummon.clear()
                    self.external = False
                    self.generation += 2
                result = super().evaluate(expression)
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
            self.assertEqual(message, dict(op="unavailable"))
        finally:
            parent.close()
            worker.join(2)
        self.assertTrue(client.disposed and client.closed)


if __name__ == "__main__":
    unittest.main()
