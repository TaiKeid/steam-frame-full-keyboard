"""Isolated helper lifecycle tests. No Steam endpoint or focused application is used."""
import importlib.util
import json
from pathlib import Path
import socket
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
            if not self.external:
                result["externalCandidate"] = dict(request=2, key="gamescope.gamescope-0.window.62", flags=13)
            else:
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

    def test_recovery_is_scoped_to_a_frozen_original_parent(self):
        with patch.object(bridge.os, "getppid", return_value=77), \
                patch.object(bridge.os, "kill") as kill, \
                patch.object(bridge.time, "monotonic", return_value=10):
            self.assertFalse(bridge.recover_stalled_parent(77, False, 5))
            self.assertFalse(bridge.recover_stalled_parent(78, True, 5))
            self.assertFalse(bridge.recover_stalled_parent(77, True, 8))
            kill.assert_not_called()
            self.assertTrue(bridge.recover_stalled_parent(77, True, 5))
            kill.assert_called_once_with(77, bridge.signal.SIGKILL)

    def test_external_guard_arms_before_takeover_and_cancels_on_x(self):
        self.external_lifecycle(False)

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
