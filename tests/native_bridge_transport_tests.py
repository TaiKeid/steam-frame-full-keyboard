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


class FakeGuard:
    def __init__(self):
        self.armed = self.closed = False
        self.prepared = self.cancelled = self.restored = 0

    def prepare(self, candidate, frame):
        assert candidate["key"] == "gamescope.gamescope-0.window.62" and frame["ready"]
        self.prepared += 1
        self.armed = True
        return True

    def renew(self, frame):
        pass

    def cancel(self):
        self.cancelled += 1
        self.armed = False

    def restore(self):
        self.restored += 1
        self.armed = False

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
