#!/usr/bin/env python3
"""App-scoped CDP transport. Only localhost Steam is contacted; no listeners or files are created."""
import base64
from collections import deque
import fcntl
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, build_opener

MAX_PACKET = 32768
GLOBAL = "__fullKeyboardNativeV1"
INSTALL_TIMEOUT = 2.0
# After an in-app update, adopt a native keyboard the outgoing version left open.
ADOPT_OPEN_MS = 3000
ACTIVE_POLL = 0.035
IDLE_POLL = 0.075
# A poll can take 0.4 s, so this keeps replies inside the parent's 0.8 s
# freshness limit and renewals inside the 0.9 s Steam lease.
STANDBY_POLL = 0.25
# Steam's UI can stall for about half a second while it filters search
# results, and a typed piece is often sent into that stall. Its confirmation
# may take that long, but must still come before the 0.9 s Steam lease ends.
DELIVERY_TIMEOUT = 0.8
# While a call waits on Steam, the parent hears from the helper this often,
# so a slow Steam is not mistaken for a dead helper.
WAIT_HEARTBEAT = 0.2


def poll_interval(state, standby=False):
    # Missing/new/inconsistent metadata must retain the ordinary fast cadence.
    handover = ('eligible', 'active', 'external', 'externalCandidate', 'externalRestore')
    # Nobody can type in standby, even with Steam's keyboard left open, but a
    # handover or restoration keeps its pace.
    if standby and type(state.get('idle')) is bool and not any(state.get(k) for k in handover):
        return STANDBY_POLL
    if state.get('idle') is True and not state.get('open') and not any(state.get(k) for k in handover):
        return IDLE_POLL
    return ACTIVE_POLL


def install_expression(source, token, external_supported, expires, adopt_open=False):
    reference = f"globalThis.{GLOBAL}"
    # Python and CEF run on the same device. A command queued beyond its local
    # deadline must not install hooks later, even if CDP cleanup cannot get through.
    return (f"(() => {{ if (Date.now() >= {expires}) return null; "
            f"if ({reference}) {{ if (!{reference}.bridge.expired()) return false; "
            f"{reference}.bridge.dispose(); delete {reference}; }} "
            f"const bridge = ({source})(globalThis, undefined, {str(external_supported).lower()}, "
            f"{ADOPT_OPEN_MS if adopt_open else 0}); "
            f"if (Date.now() >= {expires}) {{ bridge.dispose(); return null; }} "
            f"{reference} = {{token: {json.dumps(token)}, bridge}}; return true; }})()")


def cleanup_expression(token, restore=True):
    reference = f"globalThis.{GLOBAL}"
    argument = "" if restore else "false"
    return (f"(() => {{ const b = {reference}; if (b?.token === {json.dumps(token)}) {{"
            f"b.bridge.dispose({argument}); delete {reference}; }} }})()")


class BridgeUnavailable(ValueError):
    """Only fixed, non-sensitive codes may cross the private status channel."""
    def __init__(self, reason):
        self.reason = reason if reason in {"debugger-blocked", "steam-incompatible",
                                          "replacement-busy", "reply-timeout"} else "helper-unavailable"
        super().__init__(self.reason)


def require_loopback_debugger():
    """Require the debugger endpoint we use to be bound to loopback."""
    for table in ("tcp", "tcp6"):
        try:
            lines = Path("/proc/net", table).read_text().splitlines()[1:]
        except FileNotFoundError:
            if table == "tcp6":
                continue  # IPv6 can be disabled by the device.
            raise BridgeUnavailable("debugger-blocked") from None
        except OSError:
            raise BridgeUnavailable("debugger-blocked") from None
        for line in lines:
            fields = line.split()
            if fields[3] != "0A":
                continue
            host, port = fields[1].split(":")
            if int(port, 16) != 8080:
                continue
            raw = bytes.fromhex(host)
            if sys.byteorder == "little":
                raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
            address = ipaddress.ip_address(raw)
            mapped = getattr(address, "ipv4_mapped", None)
            if not (mapped or address).is_loopback:
                raise BridgeUnavailable("debugger-blocked")


class NoDebuggerRedirects(HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        raise BridgeUnavailable("debugger-blocked")


# Steam Link's streaming client asks for a keyboard through this overlay.
STREAM_KEY = "steamlink_openvr-overlay"
STREAM_SOCKET = "framekeyboard-stream.sock"
STREAM_PROTOCOL = 1
STREAM_TEXT_BYTES = 2000
# SDL scancodes (USB HID usages) of the named keys a remote session can press, and whether Shift is
# held. They are the streaming client's codes: never Linux evdev or Steam key codes.
STREAM_KEYS = {"Backspace": (42, False), "Enter": (40, False), "ShiftEnter": (40, True), "Tab": (43, False),
               "ArrowRight": (79, False), "ArrowLeft": (80, False), "ArrowDown": (81, False),
               "ArrowUp": (82, False)}
STREAM_LEFT_SHIFT = 225
STREAM_MOD_LEFT_SHIFT = 0x0001
# Linux key codes (the app's) to SDL scancodes (the streaming client's), for keys sent as real
# presses. The two numberings are unrelated; a code missing here is not sent.
STREAM_SCANCODES = {
    1: 41, 2: 30, 3: 31, 4: 32, 5: 33, 6: 34, 7: 35, 8: 36, 9: 37, 10: 38, 11: 39, 12: 45, 13: 46, 14: 42,
    15: 43, 16: 20, 17: 26, 18: 8, 19: 21, 20: 23, 21: 28, 22: 24, 23: 12, 24: 18, 25: 19, 26: 47, 27: 48,
    28: 40, 29: 224, 30: 4, 31: 22, 32: 7, 33: 9, 34: 10, 35: 11, 36: 13, 37: 14, 38: 15, 39: 51, 40: 52,
    41: 53, 42: 225, 43: 49, 44: 29, 45: 27, 46: 6, 47: 25, 48: 5, 49: 17, 50: 16, 51: 54, 52: 55, 53: 56,
    54: 229, 55: 85, 56: 226, 57: 44, 58: 57, 59: 58, 60: 59, 61: 60, 62: 61, 63: 62, 64: 63, 65: 64,
    66: 65, 67: 66, 68: 67, 69: 83, 70: 71, 71: 95, 72: 96, 73: 97, 74: 86, 75: 92, 76: 93, 77: 94, 78: 87,
    79: 89, 80: 90, 81: 91, 82: 98, 83: 99, 86: 100, 87: 68, 88: 69, 96: 88, 97: 228, 98: 84, 99: 70,
    100: 230, 102: 74, 103: 82, 104: 75, 105: 80, 106: 79, 107: 77, 108: 81, 109: 78, 110: 73, 111: 76,
    119: 72, 125: 227, 126: 231, 127: 101}
# SDL modifier bits of the modifier keys, by Linux key code: both Shifts, Ctrls, Alts and Metas.
STREAM_MODIFIERS = {42: 0x0001, 54: 0x0002, 29: 0x0040, 97: 0x0080, 56: 0x0100, 100: 0x0200,
                    125: 0x0400, 126: 0x0800}


# Steam's streaming client, the program that loads the pass-through library, by executable name.
STREAM_PROGRAMS = ("streaming_client",)


class StreamChannel:
    """Private socket the streaming client's pass-through library connects to.

    The library types what arrives here into the stream. One packet is one message: b"T" + UTF-8 for a
    commit, b"K" + scancode, modifiers and down flag for a key event. Closing the socket makes the
    library release whatever it still holds. The library sends b"F" when the user clicks the stream.
    """

    def __init__(self, directory=None, programs=STREAM_PROGRAMS):
        self.listener = None
        self.client = None
        self.path = None
        # Only these programs of this user may connect: typed text goes to whoever does. Tests pass None.
        self.programs = programs
        self.clicked = False
        self.modifiers = 0  # SDL modifier bits of the keys pressed with press()
        self.client_pid = None
        # A text of this session found no room: its later keys must not arrive without it.
        self.text_failed = False
        directory = directory or os.environ.get("XDG_RUNTIME_DIR")
        if not directory:
            return
        path = os.path.join(directory, STREAM_SOCKET)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_NONBLOCK |
                                 socket.SOCK_CLOEXEC)
        try:
            try:
                os.unlink(path)  # Left by a helper that did not exit cleanly.
            except FileNotFoundError:
                pass
            listener.bind(path)
            os.chmod(path, 0o600)
            listener.listen(2)
        except OSError:
            listener.close()
            return
        self.listener = listener
        self.path = path

    def connected(self):
        return self.client is not None

    def _drop(self):
        self.modifiers = 0
        if self.client is not None:
            self.client.close()
            self.client = None
            self.client_pid = None

    @staticmethod
    def _started(pid):
        """When a process started, in clock ticks since boot; None when it is gone."""
        try:
            with open(f"/proc/{pid}/stat") as stat:
                data = stat.read()
            return int(data[data.rindex(")") + 2:].split()[19])  # field 22, starttime
        except (OSError, ValueError, IndexError):
            return None

    def _takes_over(self, pid):
        """Whether a new client replaces the current one: only a newer stream's.

        A client hung up on reconnects within a second; were the newest always taken, two live
        streaming clients would take the channel back and forth. A process that started later is a
        newer stream, and the same process is the current client coming back.
        """
        if self.client is None or pid == self.client_pid:
            return True
        current = self._started(self.client_pid)
        incoming = self._started(pid)
        return current is None or (incoming is not None and incoming > current)

    def _allowed(self, pid, uid, hello):
        if uid != os.getuid() or len(hello) != 6 or hello[:2] != bytes((ord("H"), STREAM_PROTOCOL)):
            return False
        if self.programs is None:
            return True
        try:
            name = os.path.basename(os.readlink(f"/proc/{pid}/exe"))
        except OSError:
            return False
        # A program whose file was replaced since it started, as by a Steam update, reads back so.
        return name.removesuffix(" (deleted)") in self.programs

    def _read(self):
        try:
            while self.client is not None and select.select([self.client], [], [], 0)[0]:
                packet = self.client.recv(64)
                if not packet:
                    self._drop()
                elif packet == b"F":
                    self.clicked = True
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            self._drop()

    def service(self):
        """Accept the newest streaming client and notice when the current one went away.

        Each stream is a streaming client of its own, so a new one replaces the previous one: the
        program check above, not who came first, decides who may type.
        """
        if self.listener is None:
            return
        self._read()
        while True:
            try:
                incoming, _ = self.listener.accept()
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                break
            incoming.setblocking(False)
            try:
                pid, uid, _ = struct.unpack("3i", incoming.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                                      struct.calcsize("3i")))
                ready, _, _ = select.select([incoming], [], [], 0.2)
                hello = incoming.recv(64) if ready else b""
            except OSError:
                pid, uid, hello = 0, -1, b""
            if not self._allowed(pid, uid, hello) or not self._takes_over(pid):
                incoming.close()
                continue
            self._drop()  # A newer stream replaces the previous client.
            try:
                # Room for a client that falls behind for a while (see _send).
                incoming.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
            except OSError:
                pass
            self.client = incoming
            self.client_pid = pid
            self.text_failed = False
        self._read()

    def sockets(self):
        return [s for s in (self.listener, self.client) if s is not None]

    def take_click(self):
        clicked, self.clicked = self.clicked, False
        return clicked

    def _send(self, packet):
        if self.client is None:
            return False
        try:
            self.client.send(packet)
            return True
        except BlockingIOError:
            # A streaming client that falls behind: wait a moment for room rather than hang up on it,
            # which would drop everything it still has to type. A full AF_UNIX socket reads as
            # writable only once three quarters of it is free, so the send itself is retried. Still
            # full, a text alone fails; a key event or a release cannot be left out, or a key could
            # stay down on the PC, so then the helper hangs up and the library lets go of
            # everything, as it always did.
            deadline = time.monotonic() + .1
            while time.monotonic() < deadline:
                time.sleep(.005)
                try:
                    self.client.send(packet)
                    return True
                except BlockingIOError:
                    continue
                except OSError:
                    self._drop()
                    return False
            if packet[:1] != b"T":
                self._drop()
            return False
        except OSError:
            self._drop()
            return False

    def send_text(self, text):
        data = text.encode()
        ok = bool(data)
        while data and ok:
            end = min(len(data), STREAM_TEXT_BYTES)
            while end < len(data) and (data[end] & 0xC0) == 0x80:
                end -= 1  # Never cut inside a character.
            ok = self._send(b"T" + data[:end])
            data = data[end:]
        if not ok:
            self.text_failed = True
        return ok

    def send_key(self, name):
        key = STREAM_KEYS.get(name)
        if key is None or self.text_failed:
            return False  # After a text that failed, no key of the session: no Enter without its text.
        scancode, shift = key
        mod = STREAM_MOD_LEFT_SHIFT if shift else 0
        events = [(scancode, mod, 1), (scancode, mod, 0)]
        if shift:
            events = [(STREAM_LEFT_SHIFT, mod, 1), *events, (STREAM_LEFT_SHIFT, 0, 0)]
        return all(self._send(b"K" + struct.pack("<HHB", *event)) for event in events)

    def press(self, code, value):
        """One real key event by Linux key code: 1 down, 0 up, 2 repeat. Unknown codes are skipped."""
        scancode = STREAM_SCANCODES.get(code)
        if scancode is None:
            return True
        if value != 0 and self.text_failed:
            return False  # After a text that failed, no press of the session; releases still go.
        bit = STREAM_MODIFIERS.get(code, 0)
        if value == 1:
            self.modifiers |= bit
        elif value == 0:
            self.modifiers &= ~bit
        # A repeat is a fresh press: the client drops a second down of a key it holds.
        events = [(scancode, self.modifiers, 0), (scancode, self.modifiers, 1)] if value == 2 else [
            (scancode, self.modifiers, 1 if value else 0)]
        return all(self._send(b"K" + struct.pack("<HHB", *event)) for event in events)

    def release(self):
        """Let go of every key pressed here: the session that held them is over."""
        self.modifiers = 0
        self.text_failed = False
        self._send(b"R")

    def close(self):
        self._drop()
        if self.listener is not None:
            self.listener.close()
            self.listener = None
            try:
                os.unlink(self.path)
            except OSError:
                pass


class NativeGuard:
    """Separate native lease. EOF restores an armed exact-overlay request without CDP."""
    def __init__(self):
        self.socket = self.process = None
        self.armed = False
        executable = Path(__file__).resolve().parents[3] / "bin/framekeyboard-native-guard"
        if not executable.is_file():
            executable = Path(__file__).resolve().parent.parent / "framekeyboard-native-guard"
            if not executable.is_file():
                raise FileNotFoundError()
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket = parent
        parent.settimeout(.5)
        try:
            self.process = subprocess.Popen([str(executable), "--fd", str(child.fileno())],
                                            pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if json.loads(parent.recv(2048)).get("ok") is not True:
                raise ValueError("native-guard-unavailable")
        except BaseException:
            self.close()
            raise
        finally:
            child.close()

    def command(self, op, **fields):
        data = json.dumps(dict(op=op, **fields), separators=(",", ":")).encode()
        if self.socket.send(data) != len(data):
            raise ValueError("native-guard-backpressure")
        return json.loads(self.socket.recv(2048)).get("ok") is True

    def prepare(self, candidate, frame):
        ok = self.command("prepare", key=candidate["key"], flags=candidate["flags"],
                          requireActive=candidate.get("requireActive") is True,
                          progress=frame["progress"], sent=frame["sent"], ready=frame["ready"])
        if ok:
            self.armed = True
        return ok

    def renew(self, frame):
        self.command("renew", progress=frame["progress"], sent=frame["sent"], ready=frame["ready"])

    def cancel(self):
        if not self.command("cancel"):
            raise ValueError("native-guard-cancel-failed")
        self.armed = False

    def restore(self):
        if not self.command("restore"):
            raise ValueError("native-guard-restore-failed")
        self.armed = False

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        # An active native fallback belongs to this client. Leave the guard alive
        # until Steam reports its closure, or VR shutdown would close it immediately.
        if self.process:
            try:
                self.process.wait(timeout=.1)
            except subprocess.TimeoutExpired:
                pass


class CDP:
    def __init__(self):
        self.socket = None
        self.pending = b""
        self.sequence = 0
        try:
            require_loopback_debugger()
            self.network_checked = time.monotonic()
            opener = build_opener(ProxyHandler({}), NoDebuggerRedirects())
            with opener.open("http://127.0.0.1:8080/json/list", timeout=0.4) as response:
                data = response.read(1024 * 1024 + 1)
            if len(data) > 1024 * 1024:
                raise ValueError("target-list-too-large")
            targets = json.loads(data)
            target = next((t for t in targets if t.get("title") == "SharedJSContext"), None)
            if target is None:
                raise BridgeUnavailable("steam-incompatible")
            url = urlsplit(target["webSocketDebuggerUrl"])
            if url.scheme != "ws" or url.hostname not in ("localhost", "127.0.0.1") or url.port != 8080:
                raise BridgeUnavailable("debugger-blocked")
            self.socket = socket.create_connection(("127.0.0.1", 8080), timeout=0.3)
            key = base64.b64encode(os.urandom(16)).decode()
            path = url.path + ("?" + url.query if url.query else "")
            self.socket.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
                                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                 f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
            while b"\r\n\r\n" not in self.pending:
                chunk = self.socket.recv(4096)
                if not chunk:
                    raise EOFError()
                self.pending += chunk
                if len(self.pending) > 16384:
                    raise ValueError("invalid-upgrade")
            header, self.pending = self.pending.split(b"\r\n\r\n", 1)
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            if not header.startswith(b"HTTP/1.1 101 ") or accept.lower() not in header.lower():
                raise ValueError("upgrade-refused")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None

    def peek(self, count, deadline):
        while len(self.pending) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            self.socket.settimeout(remaining)
            data = self.socket.recv(min(65536, count - len(self.pending)))
            if not data:
                raise EOFError()
            self.pending += data
        return self.pending[:count]

    def receive_frame(self, deadline):
        # Do not consume a partial frame on timeout. The next call must be able
        # to drain a late reply, including one interrupted inside its payload.
        first, second = self.peek(2, deadline)
        length, header = second & 127, 2
        if length == 126:
            length, header = struct.unpack("!H", self.peek(4, deadline)[2:4])[0], 4
        elif length == 127:
            length, header = struct.unpack("!Q", self.peek(10, deadline)[2:10])[0], 10
        if length > 1024 * 1024 or second & 128 or not first & 0x80 or first & 0x70:
            raise ValueError("invalid-debug-frame")
        data = self.peek(header + length, deadline)[header:]
        self.pending = self.pending[header + length:]
        return first & 15, data

    def frame(self, opcode, data, deadline):
        size = len(data)
        header = bytes([0x80 | opcode])
        if size < 126:
            header += bytes([0x80 | size])
        elif size < 65536:
            header += bytes([0xfe]) + struct.pack("!H", size)
        else:
            header += bytes([0xff]) + struct.pack("!Q", size)
        mask = os.urandom(4)
        self.socket.settimeout(max(0.001, deadline - time.monotonic()))
        try:
            self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        except OSError as error:
            # A partial write cannot safely be retried on the same stream.
            self.close()
            raise ConnectionError("debugger-write-failed") from error

    def evaluate(self, expression, timeout=0.4, waiting=None):
        if time.monotonic() - self.network_checked >= 1:
            require_loopback_debugger()
            self.network_checked = time.monotonic()
        self.sequence += 1
        command = {"id": self.sequence, "method": "Runtime.evaluate", "params": {
            "expression": expression, "returnByValue": True, "silent": True}}
        deadline = time.monotonic() + timeout
        self.frame(1, json.dumps(command, ensure_ascii=True).encode(), deadline)
        while time.monotonic() < deadline:
            # Wake every WAIT_HEARTBEAT for the caller; a partial reply stays
            # buffered across these short reads.
            try:
                opcode, payload = self.receive_frame(
                    min(deadline, time.monotonic() + WAIT_HEARTBEAT) if waiting else deadline)
            except TimeoutError:
                if waiting and time.monotonic() < deadline:
                    waiting()
                    continue
                raise
            if opcode == 9:
                self.frame(10, payload, deadline)
                continue
            if opcode == 8:
                raise EOFError()
            if opcode != 1:
                continue
            message = json.loads(payload)
            if message.get("id") != self.sequence:
                continue
            if "exceptionDetails" in message.get("result", {}) or "error" in message:
                raise BridgeUnavailable("steam-incompatible")
            return message.get("result", {}).get("result", {}).get("value")
        raise TimeoutError()


def encode(value):
    data = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode()
    if len(data) > MAX_PACKET:
        raise ValueError("oversize-reply")
    return data


def queued_bytes(parent):
    """Bytes this end has sent that the parent has not read yet."""
    try:
        return struct.unpack("i", fcntl.ioctl(parent.fileno(), termios.TIOCOUTQ, b"\0" * 4))[0]
    except OSError:
        return 0


class ParentChannel:
    """Replies to the keyboard without ever blocking on it.

    Results answer a request and must arrive, so they wait in a bounded outbox
    until the socket takes them. Status is idempotent: a new one follows every
    poll, so one is skipped rather than queued while results wait or the
    parent is not reading. A parent stalled in a long runtime call then finds a
    few recent states, never a full socket that refuses its results."""
    STATE_QUEUE_LIMIT = 16384
    OUTBOX_LIMIT = 256
    # An unchanged idle state only shows that the helper is alive. Like the
    # standby cadence, this leaves a slow 0.4 s poll inside the parent's 0.8 s
    # freshness limit; any change, and every non-idle state, goes out at once.
    IDLE_HEARTBEAT = 0.25

    def __init__(self, parent):
        self.parent = parent
        self.outbox = deque()
        self.last_state, self.last_state_at = None, 0.0

    def send_result(self, value):
        if len(self.outbox) >= self.OUTBOX_LIMIT:
            raise ValueError("parent-backpressure")
        self.outbox.append(encode(value))
        self.flush()

    def flush(self):
        while self.outbox:
            try:
                sent = self.parent.send(self.outbox[0], socket.MSG_DONTWAIT)
            except BlockingIOError:
                return
            if sent != len(self.outbox[0]):
                raise ValueError("parent-backpressure")
            self.outbox.popleft()

    def send_state(self, value):
        """Returns whether the state went out."""
        self.flush()
        if self.outbox or queued_bytes(self.parent) >= self.STATE_QUEUE_LIMIT:
            return False
        data = encode(value)
        now = time.monotonic()
        if value.get("idle") is True and data == self.last_state and \
                now - self.last_state_at < self.IDLE_HEARTBEAT:
            return False
        try:
            sent = self.parent.send(data, socket.MSG_DONTWAIT)
        except BlockingIOError:
            return False
        if sent != len(data):
            raise ValueError("parent-backpressure")
        self.last_state, self.last_state_at = data, now
        return True

    def waiting(self):
        return bool(self.outbox)

    def heartbeat(self):
        """While a call waits on a slow Steam: the last state again, so the
        parent sees a live helper, and any results still queued."""
        self.flush()
        if self.last_state is None or self.outbox or queued_bytes(self.parent) >= self.STATE_QUEUE_LIMIT:
            return
        try:
            sent = self.parent.send(self.last_state, socket.MSG_DONTWAIT)
        except BlockingIOError:
            return
        if sent != len(self.last_state):
            raise ValueError("parent-backpressure")
        self.last_state_at = time.monotonic()


def send(parent, value):
    data = encode(value)
    if parent.send(data) != len(data):
        raise ValueError("parent-backpressure")


def report_unavailable(parent, reason, channel=None):
    # Closing a seqpacket peer with unread frames discards its final reply with
    # ECONNRESET. Refuse new writes and drain the bounded queue before reporting.
    parent.shutdown(socket.SHUT_RD)
    for _ in range(1024):
        if not parent.recv(MAX_PACKET + 1, socket.MSG_DONTWAIT):
            break
    # Results still waiting go first. A parent that is not reading gets EOF
    # instead; waiting longer would only delay the cleanup that restores native.
    channel = channel or ParentChannel(parent)
    deadline = time.monotonic() + 0.2
    channel.outbox.append(encode({"op": "unavailable", "reason": reason}))
    while channel.waiting():
        channel.flush()
        remaining = deadline - time.monotonic()
        if not channel.waiting() or remaining <= 0:
            break
        select.select([], [parent], [], remaining)


class WatchdogProgress:
    """Private pipe from the overlay watchdog, independent of the VR/main loop."""
    def __init__(self, fd):
        self.fd = fd
        self.pending = b""
        self.sent = None
        self.slow_until = 0
        self.runtime_until = 0

    def poll(self):
        if self.fd is None:
            return
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            if not data:
                break
            self.pending += data
            while len(self.pending) >= 24:
                sent, slow_until, runtime_until = struct.unpack("=ddd", self.pending[:24])
                self.pending = self.pending[24:]
                if math.isfinite(sent) and 0 < sent <= time.monotonic() and \
                        (self.sent is None or sent > self.sent):
                    self.sent = sent
                    self.slow_until = slow_until if math.isfinite(slow_until) and \
                        0 < slow_until <= sent + 30 else 0
                    self.runtime_until = runtime_until if math.isfinite(runtime_until) and \
                        0 < runtime_until <= sent + 30 else 0

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class ParentRecovery:
    def __init__(self):
        self.terminated_at = None
        self.boot_gap = None
        self.resume_until = 0
        self.resume_at = 0
        self.resume_frame = None

    @staticmethod
    def parent_stopped(owner):
        try:
            # comm can contain spaces and parentheses. The state follows its
            # final closing parenthesis; only a signal stop uses uppercase T.
            fields = Path(f"/proc/{owner}/stat").read_text().rsplit(") ", 1)[1].split()
            return fields[0] == "T"
        except (OSError, IndexError):
            return False  # An unreadable state leaves the normal deadlines in place.

    def observe_clock(self, now, boot_now, frame_sent):
        # BOOTTIME includes system suspend; MONOTONIC and the frame clock do not.
        gap = boot_now - now
        if self.boot_gap is not None and gap - self.boot_gap > .5 and self.resume_frame != frame_sent:
            self.resume_at, self.resume_until, self.resume_frame = now, now + 30, frame_sent
        self.boot_gap = gap
        if frame_sent > self.resume_at:
            self.resume_until = 0

    def check(self, owner, watch, frame_sent, watchdog_sent, slow_until=0, runtime_until=0):
        now = time.monotonic()
        if not owner or not watch or os.getppid() != owner:
            self.terminated_at = None
            return False
        stopped = self.parent_stopped(owner)
        if os.getppid() != owner:
            self.terminated_at = None
            return False
        if stopped:
            # Neither the main loop nor its watchdog can run after SIGSTOP, and
            # SIGTERM would remain pending. Remove the frozen panel's owner now.
            os.kill(owner, signal.SIGKILL)
            return True
        if now - frame_sent < 3:
            self.terminated_at = None
            return False
        if self.terminated_at is None and (now < self.resume_until or
                                          now < min(runtime_until, frame_sent + 30)):
            return False
        # A live overlay watchdog does not prove main-loop progress. Only an
        # explicit slow operation may delay recovery, never beyond 30 seconds
        # from the last completed frame. It cannot cancel a termination begun.
        if self.terminated_at is None and watchdog_sent is not None and \
                0 <= now - watchdog_sent < 3 and now < min(slow_until, frame_sent + 30):
            return False
        if self.terminated_at is None:
            os.kill(owner, signal.SIGTERM)
            self.terminated_at = now
        elif now - self.terminated_at >= 1:
            os.kill(owner, signal.SIGKILL)
            return True
        return False


def run(parent, client_factory=CDP, parent_pid=None, guard_factory=NativeGuard, health_fd=None,
        adopt_open=False, stream_factory=None):
    client = None
    guard = None
    stream = None
    install_attempted = False
    watchdog = WatchdogProgress(health_fd)
    recovery_watchdog = ParentRecovery()
    token = os.urandom(16).hex()
    reference = f"globalThis.{GLOBAL}"
    failure_reason = "debugger-unavailable"
    restore_native = True
    channel = ParentChannel(parent)
    try:
        client = client_factory()
        try:
            guard = guard_factory()
        except (OSError, ValueError, EOFError):
            pass  # Steam targets remain supported when the native adapter is unavailable.
        failure_reason = "helper-unavailable"
        source = Path(__file__).with_name("steam_bridge.js").read_text()
        failure_reason = "steam-incompatible"
        # Never displace another live instance. A dead owner's lease must expire first.
        install_attempted = True
        result = client.evaluate(install_expression(source, token, guard is not None,
                                                   int((time.time() + INSTALL_TIMEOUT) * 1000),
                                                   adopt_open),
                                 timeout=INSTALL_TIMEOUT)
        if result is None:
            raise TimeoutError()  # The expression itself rejected a late installation.
        if result is not True:
            raise BridgeUnavailable("replacement-busy")
        failure_reason = "connection-lost"
        if stream_factory:
            try:
                stream = stream_factory()  # Only the installed, live bridge owns the socket.
            except OSError:
                stream = None
        stream_request = None
        stream_click = False  # The user clicked the stream; reported to the bridge with the next poll.
        # The stream session whose streaming client went away, reported once with the next poll.
        stream_gone = None
        gone_request = None
        guarded_key = None  # the overlay the guard was last armed for
        panel_shown = False
        frame = {"progress": 0, "ready": False, "ack": 0, "fallback": False,
                 "sent": time.monotonic()}
        frame_sent = time.monotonic()
        last_parent = time.monotonic()
        watch_main_loop = False
        standby = False
        fallback_request = None
        external_ack = 0
        next_poll = 0.0
        pending = deque()
        held_keys = {}
        pending_repeats = set()
        stopping = False
        consecutive_timeouts = 0
        announced = False

        def evaluate(expression, timeout=0.4):
            nonlocal consecutive_timeouts
            try:
                result = client.evaluate(expression, timeout, waiting=channel.heartbeat)
            except TimeoutError:
                consecutive_timeouts += 1
                if consecutive_timeouts >= 3:
                    raise BridgeUnavailable("reply-timeout") from None
                raise
            consecutive_timeouts = 0
            return result

        def receive(readable):
            nonlocal stopping, restore_native, frame, fallback_request, next_poll, last_parent
            nonlocal frame_sent, watch_main_loop, standby, pending, panel_shown
            # Drain releases before dispatching more work. CDP can be slower than
            # repeat generation; initial clicks stay ordered, repeats do not pile up.
            received = 0
            while readable:
                received += 1
                if received > 256:
                    raise ValueError("parent-backlog")
                packet = parent.recv(MAX_PACKET + 1)
                if not packet:
                    stopping = True
                    break
                if len(packet) > MAX_PACKET:
                    raise ValueError("oversize-packet")
                message = json.loads(packet)
                operation = message.get("op")
                if operation == "stop":
                    # Only an explicit restore:false stop may suppress recovery;
                    # prepared updates now restore too. EOF, signals and
                    # malformed packets still restore.
                    restore_native = message.get("restore") is not False
                    stopping = True
                    break
                if operation == "frame":
                    if type(message.get("progress")) is not int or type(message.get("ready")) is not bool:
                        raise ValueError("invalid-frame")
                    frame = {k: message[k] for k in ("progress", "ready", "ack", "sent")}
                    panel_shown = message.get("shown") is True
                    # Restoration acknowledgements must survive subsequent frame packets.
                    frame["externalRestored"] = external_ack
                    # An unarmed guard ignores renewals; prepare carries the
                    # current frame, so skip the per-frame round trip until then.
                    if guard and guard.armed:
                        guard.renew(frame)
                    if message.get("fallback") is True:
                        fallback_request = message.get("fallbackRequest", 0)
                        next_poll = 0
                    last_parent = time.monotonic()
                    frame_sent = float(message["sent"])
                    watch_main_loop = message.get("watch") is True
                    if standby and message.get("standby") is not True:
                        next_poll = 0  # Awake: fast again now, not after the slow wait.
                    standby = message.get("standby") is True
                elif operation == "key-up":
                    if type(message.get("id")) is not int or type(message.get("request")) is not int:
                        raise ValueError("invalid-release")
                    key_id = message["id"]
                    held = held_keys.get(key_id)
                    if held and held[0] == message["request"]:
                        del held_keys[key_id]
                        cancelled_repeat = key_id in pending_repeats
                        pending_repeats.discard(key_id)
                        pending = deque(m for m in pending if not (
                            m["op"] == "key" and m["value"] == 2 and m["id"] == key_id))
                        if cancelled_repeat:
                            # No deletion after release, but the parent still
                            # needs completion for the repeat it submitted.
                            channel.send_result({"op": "key-result", "id": key_id, "ok": True})
                elif operation == "stream-key":
                    if (type(message.get("request")) is not int or type(message.get("code")) is not int or
                            message.get("value") not in (0, 1, 2)):
                        raise ValueError("invalid-stream-key")
                    pending.append(message)
                elif operation == "dismiss":
                    if type(message.get("request")) is not int:
                        raise ValueError("invalid-dismissal")
                    pending.append(message)
                elif operation in ("text", "key"):
                    if type(message.get("request")) is not int or not isinstance(message.get("text"), str):
                        raise ValueError("invalid-input")
                    if len(message["text"].encode()) > 8192:
                        raise ValueError("oversize-input")
                    if operation == "key":
                        if type(message.get("id")) is not int or message["id"] <= 0 or \
                                type(message.get("value")) is not int or message["value"] not in (1, 2):
                            raise ValueError("invalid-key")
                        key_id = message["id"]
                        identity = (message["request"], message["text"])
                        if message["value"] == 1:
                            if key_id in held_keys or len(held_keys) >= 128:
                                raise ValueError("invalid-key-hold")
                            held_keys[key_id] = identity
                            pending.append(message)
                        elif held_keys.get(key_id) == identity and key_id not in pending_repeats:
                            pending_repeats.add(key_id)
                            pending.append(message)
                    else:
                        pending.append(message)
                else:
                    raise ValueError("unknown-operation")
                if len(pending) > 128:
                    raise ValueError("input-backlog")
                readable, _, _ = select.select([parent], [], [], 0)

        def judge_parent():
            # Judge the parent by the frames just read: frames that queued
            # while the helper waited on Steam are not a stalled parent, and a
            # parent that did stall cannot renew the lease with an old frame.
            if time.monotonic() - last_parent > 0.7 or not 0 <= time.monotonic() - frame_sent < 0.7:
                frame["ready"] = False

        while True:
            if not Path(__file__).is_file() or not Path(__file__).with_name("steam_bridge.js").is_file():
                break  # Removing an installed app also withdraws a still-running bridge.
            readable, _, _ = select.select([parent] + (stream.sockets() if stream else []),
                                           [parent] if channel.waiting() else [], [],
                                           0 if pending else max(0, next_poll - time.monotonic()))
            channel.flush()
            receive([parent] if parent in readable else [])
            if stopping:
                break
            judge_parent()
            if stream:
                stream.service()
                if stream.take_click():
                    if panel_shown:
                        stream_click = True
                        next_poll = 0
                # The streaming client went away during its session, as when the stream ends or the
                # library is told to idle: the bridge ends the session as a cleared target.
                if stream_request is not None and not stream.connected() and gone_request != stream_request:
                    stream_gone = gone_request = stream_request
                    next_poll = 0
            if pending:
                message = pending.popleft()
                operation = message["op"]
                # Typing into a stream whose client went away fails, but there is nothing to fall back
                # to: the session ends as a cleared target with the next poll instead.
                lost_stream = (bool(stream) and stream_request is not None and
                               message.get("request") == stream_request and not stream.connected())
                try:
                    if operation == "stream-key":
                        # A release goes out even after the session ended; a press only into it.
                        ok = bool(stream) and (stream_request == message["request"] or message["value"] == 0) \
                            and stream.press(message["code"], message["value"])
                        if message["value"] == 0:
                            ok = True  # A release that found no one has nothing left to undo.
                    elif operation == "dismiss":
                        ok = evaluate(f"{reference}?.bridge.dismiss({message['request']}) === true",
                                      DELIVERY_TIMEOUT)
                        next_poll = 0
                    else:
                        if operation == "key" and message["value"] == 2:
                            pending_repeats.discard(message["id"])
                        if (stream and stream_request == message["request"] and
                                not (operation == "key" and message["text"] == "VKClose")):
                            # A streamed game: the streaming client types it. Steam is not involved,
                            # so nothing depends on Steam's keyboard or SteamVR's keyboard session.
                            ok = (stream.send_key(message["text"]) if operation == "key"
                                  else stream.send_text(message["text"]))
                        else:
                            # The JS side checks the target and lease again at delivery.
                            ok = evaluate(f"{reference}?.bridge.send({message['request']},"
                                          f"{json.dumps(message['text'], ensure_ascii=True)},"
                                          f"{str(operation == 'key').lower()}) === true", DELIVERY_TIMEOUT)
                except TimeoutError:
                    ok = False  # Unknown delivery fails this request closed.
                if operation not in ("dismiss", "stream-key"):
                    channel.send_result({"op": f"{operation}-result", "id": message["id"],
                                         "ok": ok is True})
                if ok is not True and not lost_stream:
                    fallback_request = message["request"]
                # A delivery can wait on Steam for up to DELIVERY_TIMEOUT. Read what
                # the parent sent meanwhile before this loop's poll may renew the lease.
                receive(select.select([parent], [], [], 0)[0])
                if stopping:
                    break
                judge_parent()
            watchdog.poll()
            recovery_watchdog.observe_clock(time.monotonic(), time.clock_gettime(time.CLOCK_BOOTTIME), frame_sent)
            if not select.select([parent], [], [], 0)[0] and recovery_watchdog.check(
                    parent_pid, watch_main_loop, frame_sent, watchdog.sent, watchdog.slow_until,
                    watchdog.runtime_until):
                break
            if time.monotonic() >= next_poll:
                poll_frame = {**frame, "fallback": fallback_request is not None,
                              "fallbackRequest": fallback_request, "shown": panel_shown,
                              "streamClick": stream_click, "streamGone": stream_gone}
                try:
                    result = evaluate(f"{reference}?.bridge.step({json.dumps(poll_frame)})")
                    if not isinstance(result, dict) or result.get("version") != 1 or result.get("unavailable"):
                        raise BridgeUnavailable("steam-incompatible")
                    stream_click = False
                    stream_gone = None
                    if guard and guard.armed and result.get("stream") and guarded_key != STREAM_KEY:
                        # A click moved typing from the window the guard was armed for to the stream.
                        # Only once the step says so: a click the bridge did not follow keeps the guard.
                        guard.cancel()
                    if guard:
                        recovery = result.get("externalRestore")
                        if recovery and recovery["request"] != external_ack:
                            guard.restore()
                            external_ack = frame["externalRestored"] = recovery["request"]
                        candidate = result.get("externalCandidate")
                        if candidate:
                            # Dismissal and resummon can both occur between polls. A
                            # candidate has no logical owner, so any old lease must
                            # be cancelled before the next exact target is armed.
                            if guard.armed:
                                guard.cancel()
                            # Without the streaming client's channel there is no way to type into the
                            # stream, so the request stays with Steam's keyboard.
                            guarded = ((candidate.get("key") != STREAM_KEY or
                                        bool(stream and stream.connected())) and
                                       guard.prepare(candidate, frame))
                            if guarded:
                                guarded_key = candidate.get("key")
                            result = evaluate(f"{reference}?.bridge.prepareExternal("
                                              f"{candidate['request']},{json.dumps(candidate['key'])},"
                                              f"{str(guarded).lower()})")
                            if not isinstance(result, dict):
                                raise BridgeUnavailable("steam-incompatible")
                        recovery = result.get("externalRestore")
                        if recovery and recovery["request"] != external_ack:
                            guard.restore()
                            external_ack = frame["externalRestored"] = recovery["request"]
                        elif not result.get("external") and guard.armed:
                            guard.cancel()
                except TimeoutError:
                    # step is idempotent. Keep the last state/fallback request;
                    # the JS lease and C++ freshness timeout remain independent.
                    next_poll = time.monotonic() + ACTIVE_POLL
                    continue
                fallback_request = None
                previous_stream = stream_request
                stream_request = result.get("request") if result.get("stream") and result.get("active") else None
                if stream and previous_stream is not None and stream_request != previous_stream:
                    stream.release()  # Nothing stays held on the PC after its session.
                state = {"op": "state", **result}
                if not announced:
                    # The parent's startup deadline counts when this was sent,
                    # not when a slow frame of its own reads it.
                    state["at"] = time.monotonic()
                announced |= channel.send_state(state)
                next_poll = time.monotonic() + poll_interval(result, standby)
    except (Exception, SystemExit) as error:
        # Errors are fixed status codes. Never echo CDP expressions, input packets or field values.
        try:
            reason = error.reason if isinstance(error, BridgeUnavailable) else failure_reason
            if isinstance(error, TimeoutError) and failure_reason == "steam-incompatible":
                reason = "install-timeout"
            report_unavailable(parent, reason, channel)
        except Exception:
            pass
    finally:
        if client:
            if install_attempted:
                try:
                    # A lost reply does not prove the install failed. The token
                    # prevents this attempt's cleanup from removing a successor.
                    client.evaluate(cleanup_expression(token, restore_native))
                    if guard and guard.armed:
                        if restore_native:
                            guard.restore()
                        else:
                            guard.cancel()
                except Exception:
                    pass  # Steam-side watchdog restores presentation even if CDP disappeared.
            client.close()
        if guard:
            guard.close()
        if stream:
            stream.close()
        watchdog.close()
        parent.close()


if __name__ == "__main__":
    # SIGTERM follows the same cleanup path as EOF from the owning application.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    if (len(sys.argv) not in (5, 6) or sys.argv[1] != "--fd" or sys.argv[3] != "--health-fd" or
            sys.argv[5:] not in ([], ["--adopt-open"])):
        sys.exit(2)
    run(socket.socket(fileno=int(sys.argv[2])), stream_factory=StreamChannel, parent_pid=os.getppid(),
        health_fd=int(sys.argv[4]),
        adopt_open=sys.argv[5:] == ["--adopt-open"])
