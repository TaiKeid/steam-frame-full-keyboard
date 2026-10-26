"""Dictation capture and recognition over a private socket.

Opens no listeners and writes no files. Audio and text live only in memory,
nothing is printed, and the keyboard learns only fixed status codes.
"""
import argparse
import array
import collections
import contextlib
import ctypes
import http.client
import io
import json
import math
import os
import queue
import re
import resource
import secrets
import select
import signal
import socket
import ssl
import stat
import subprocess
import threading
import time
import unicodedata
import wave
from urllib.parse import urlsplit

RATE = 16000
FRAME_SAMPLES = 320  # 20 ms
FRAME_BYTES = FRAME_SAMPLES * 2
FRAMES_PER_SECOND = RATE // FRAME_SAMPLES
PREROLL_FRAMES = 15  # 0.3 s before the first speech frame
START_FRAMES = 2  # consecutive speech frames that start a phrase
END_FRAMES = 30  # 0.6 s without speech ends a phrase
TAIL_FRAMES = 10  # quiet kept after the last speech frame
MIN_SPEECH_FRAMES = 12  # shorter bursts are clicks, not speech
MAX_PHRASE_FRAMES = 25 * FRAMES_PER_SECOND
# Live words: while a phrase is spoken, its audio so far is recognized once
# 1.5 s are in and again after every further second.
LIVE_FIRST_FRAMES = 3 * FRAMES_PER_SECOND // 2
LIVE_EVERY_FRAMES = FRAMES_PER_SECOND
SILENCE_STOP_FRAMES = 60 * FRAMES_PER_SECOND
LEVEL_EVERY = 4  # 12.5 level packets a second
ZERO_DB = -90.0  # exact zeros: the Frame's noise gate is closed
FLOOR_FRAMES = 2 * FRAMES_PER_SECOND  # the floor is the quietest open-gate frame of the last 2 s
FLOOR_MIN_FRAMES = FRAMES_PER_SECOND // 2  # fewer open-gate frames than this use the default
FLOOR_DEFAULT_DB = -60.0
SPEECH_MARGIN_DB = 10.0
SPEECH_MIN_DB = -55.0
LEVEL_RANGE_DB = 40.0

MAX_PACKET = 16384
MAX_REPLY = 64 * 1024
MAX_PROMPT = 200
REQUEST_TIMEOUT = 10.0
ALIVE_TIMEOUT = 2.0
MAX_QUEUED = 8
MAX_QUEUED_FRAMES = 120 * FRAMES_PER_SECOND
CHARS_PER_SECOND = 25
DEFAULT_PORT = 8031
UPLOAD_NAME = "phrase.wav"
RECORDER = ["pw-record", "--raw", "--rate", str(RATE), "--channels", "1", "--format", "s16",
            "--media-role", "Communication", "-"]


class Failure(Exception):
    """A fixed status code; the only failure detail that leaves the helper."""


def harden():
    # Audio and text sit in this process's memory; keep them out of core dumps.
    with contextlib.suppress(OSError, ValueError):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with contextlib.suppress(OSError, AttributeError):
        ctypes.CDLL(None, use_errno=True).prctl(4, 0, 0, 0, 0)  # PR_SET_DUMPABLE


def level_db(frame):
    samples = array.array("h")
    samples.frombytes(frame)
    energy = sum(x * x for x in samples)
    if energy == 0:
        return -96.0
    return 10 * math.log10(energy / len(samples) / (32768.0 * 32768.0))


class PhraseDetector:
    """Energy phrase detection with an adaptive floor.

    Exact zeros mean the microphone's noise gate is closed, which is certain
    silence and never moves the floor. Background noise passes only while the
    gate is open, so the floor is the quietest open-gate frame of the last 2 s:
    the dips between words reach it, and speech must stand out from it.
    """

    def __init__(self):
        self.recent = collections.deque(maxlen=FLOOR_FRAMES)
        self.history = collections.deque(maxlen=PREROLL_FRAMES)
        self.history_levels = collections.deque(maxlen=PREROLL_FRAMES)
        self.reset()
        self.idle = 0  # frames since the last speech, for the 60 s stop
        self.counter = 0

    def reset(self):
        self.phrase = None
        self.levels = []  # each phrase frame's level, re-checked against the final floor
        self.speech = self.quiet = self.run = self.frames = 0
        self.history.clear()
        self.history_levels.clear()

    def feed(self, frame):
        """Returns ("level", n), ("start",), ("end", pcm or None) and ("silence",) events."""
        db = level_db(frame)
        if db > ZERO_DB:
            self.recent.append(db)
        floor = self.floor()
        speech = db > ZERO_DB and db > max(floor + SPEECH_MARGIN_DB, SPEECH_MIN_DB)
        events = []
        self.idle = 0 if speech else self.idle + 1
        if self.phrase is None:
            self.history.append(frame)
            self.history_levels.append(db)
            self.run = self.run + 1 if speech else 0
            if self.run >= START_FRAMES:
                self.phrase = bytearray(b"".join(self.history))
                self.levels = list(self.history_levels)
                self.history.clear()
                self.history_levels.clear()
                self.speech, self.quiet, self.frames = self.run, 0, len(self.phrase) // FRAME_BYTES
                events.append(("start",))
            elif self.idle >= SILENCE_STOP_FRAMES:
                events.append(("silence",))
        else:
            self.phrase += frame
            self.levels.append(db)
            self.frames += 1
            if speech:
                self.speech += 1
                self.quiet = 0
            else:
                self.quiet += 1
            if self.quiet >= END_FRAMES:
                events.append(("end", self.finish()))
            elif self.frames >= MAX_PHRASE_FRAMES:
                # A forced split shares no audio with the next phrase, which
                # would make Whisper repeat the words at the cut.
                events.append(("end", self.finish()))
                if speech:
                    self.phrase = bytearray()
                    self.levels = []
                    self.speech, self.quiet, self.frames = 0, 0, 0
                    events.append(("start",))
        self.counter += 1
        if self.counter % LEVEL_EVERY == 0:
            value = 0 if not speech else round(min(1.0, (db - floor) / LEVEL_RANGE_DB) * 255)
            events.append(("level", max(0, value)))
        return events

    def floor(self):
        # Until the window has seen enough open-gate audio, its minimum may be
        # the first word itself, which must not mask that word.
        if len(self.recent) < FLOOR_MIN_FRAMES:
            return min([FLOOR_DEFAULT_DB, *self.recent])
        return min(self.recent)

    def finish(self):
        """Ends the phrase in progress; None when it held too little speech."""
        if self.phrase is None:
            return None
        extra = max(0, self.quiet - TAIL_FRAMES) * FRAME_BYTES
        pcm = bytes(self.phrase[:len(self.phrase) - extra] if extra else self.phrase)
        # Frames counted as speech while the floor was still unknown may have
        # been steady noise; only frames above the floor as it is now count.
        threshold = max(self.floor() + SPEECH_MARGIN_DB, SPEECH_MIN_DB)
        enough = sum(1 for db in self.levels if db > threshold) >= MIN_SPEECH_FRAMES
        self.phrase = None
        self.levels = []
        self.speech = self.quiet = self.run = self.frames = 0
        return pcm if enough else None


def wav_bytes(pcm):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(pcm)
    return output.getvalue()


class Target:
    """Where phrases go: the user's own server (host:port) or a configured URL."""

    def __init__(self, server=None, url=None, model=None, language=None):
        if bool(server) == bool(url):
            raise Failure("not-configured")
        if server:
            match = re.fullmatch(r"(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(?::([0-9]{1,5}))?", server)
            if not match:
                raise Failure("not-configured")
            self.https, self.host = False, match.group(1).strip("[]")
            self.port = int(match.group(2) or DEFAULT_PORT)
            self.paths = ["/v1/audio/transcriptions", "/inference"]
        else:
            parts = urlsplit(url)
            if (parts.scheme not in ("http", "https") or not parts.hostname or parts.username or
                    parts.password or parts.query or parts.fragment):
                raise Failure("not-configured")
            self.https, self.host = parts.scheme == "https", parts.hostname
            self.port = parts.port or (443 if self.https else 80)
            self.paths = [parts.path.rstrip("/") + "/audio/transcriptions"]
        if not 0 < self.port < 65536:
            raise Failure("not-configured")
        if model is not None and (not isinstance(model, str) or len(model) > 128):
            raise Failure("not-configured")
        if language is not None and (not isinstance(language, str) or
                                     not re.fullmatch(r"[a-z]{2,3}", language)):
            raise Failure("not-configured")
        self.model, self.language = model, language
        self.path_index = 0  # advanced, by the worker only, once a fallback path worked


def read_key(path):
    """The API key, only from a regular file the user alone can read."""
    if not path:
        return None
    try:
        with open(path, "rb") as file:
            info = os.fstat(file.fileno())
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    info.st_mode & 0o077 or info.st_size > 4096):
                return None
            key = file.read(4097).decode("ascii").strip()
    except (OSError, UnicodeDecodeError):
        return None
    return key if key and re.fullmatch(r"[\x21-\x7e]+", key) else None


class Connection:
    """The request in flight, which the main loop can abort from another thread."""

    def __init__(self):
        self.lock = threading.Lock()
        self.socket = None
        self.aborted = False

    def attach(self, connection):
        with self.lock:
            if self.aborted:
                raise Failure("cancelled")
            self.socket = connection.sock

    def abort(self):
        with self.lock:
            self.aborted = True
            if self.socket:
                with contextlib.suppress(OSError):
                    self.socket.shutdown(socket.SHUT_RDWR)


def multipart(fields, audio):
    boundary = "fk" + secrets.token_hex(12)
    parts = []
    for name, value in fields:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'.encode()
                     + value.encode("utf-8") + b"\r\n")
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{UPLOAD_NAME}"'
                 f'\r\nContent-Type: audio/wav\r\n\r\n'.encode() + audio + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def post(target, path, body, headers, connection, ca_file):
    def connect():
        if target.https:
            context = ssl.create_default_context(cafile=ca_file)
            return http.client.HTTPSConnection(target.host, target.port, timeout=REQUEST_TIMEOUT,
                                               context=context)
        return http.client.HTTPConnection(target.host, target.port, timeout=REQUEST_TIMEOUT)

    for attempt in range(2):
        client = connect()
        try:
            client.connect()
        except ssl.SSLError:
            client.close()
            raise Failure("unreachable")
        except OSError:
            client.close()
            # Nothing was sent yet, so one retry cannot type a phrase twice.
            if attempt or connection.aborted:
                raise Failure("unreachable")
            time.sleep(0.2)
            continue
        try:
            connection.attach(client)
            client.request("POST", path, body=body, headers=headers)
            response = client.getresponse()
            data = response.read(MAX_REPLY + 1)
            return response.status, data
        except socket.timeout:
            raise Failure("timeout")
        except (OSError, http.client.HTTPException):
            raise Failure("cancelled" if connection.aborted else "unreachable")
        finally:
            client.close()


ANNOTATION = re.compile(r"\[[^\]\n]{0,40}\]|\((?:[A-Za-z]+(?: [A-Za-z]+){0,2})\)|[♪♫]")
LOOP_CHARACTERS = re.compile(r"(.{1,20}?)\1{3,}", re.S)


def word_key(word):
    return word.strip(".,!?;:").lower()


def clean(text, language, seconds):
    """Returns (text, None) or (None, reason) for a recognizer's reply text."""
    if not isinstance(text, str):
        return None, "invalid"
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:  # lone surrogates from JSON escapes
        return None, "invalid"
    text = ANNOTATION.sub(" ", text)
    text = "".join(" " if unicodedata.category(c) in ("Cc", "Cs", "Co", "Cn") else c for c in text)
    words = text.split()
    if language in ("ja", "zh"):
        text = LOOP_CHARACTERS.sub(r"\1", " ".join(words))
    else:
        # A word repeated four or more times in a row is Whisper looping; keep one.
        kept, run = [], 0
        for index, word in enumerate(words):
            run = run + 1 if index and word_key(word) == word_key(words[index - 1]) else 1
            if run == 4:
                del kept[-2:]
            elif run < 4:
                kept.append(word)
        text = " ".join(kept)
    text = text.strip()
    if not text:
        return None, "empty"
    if len(text) > CHARS_PER_SECOND * seconds + 20:
        return None, "too-long"
    return text, None


def recognize(target, key, pcm, prompt, connection, ca_file):
    """Runs on the worker thread. Returns ("text", text), ("dropped", reason) or ("error", code)."""
    fields = [("response_format", "json"), ("temperature", "0")]
    if target.language:
        fields.append(("language", target.language))
    if prompt:
        fields.append(("prompt", prompt))
    if target.model:
        fields.append(("model", target.model))
    body, content_type = multipart(fields, wav_bytes(pcm))
    headers = {"Content-Type": content_type, "User-Agent": "FullKeyboard-Dictation/1"}
    if key and target.https:
        headers["Authorization"] = f"Bearer {key}"
    first = target.path_index
    try:
        for index in range(first, len(target.paths)):
            status, data = post(target, target.paths[index], body, headers, connection, ca_file)
            if status == 404 and index + 1 < len(target.paths):
                continue
            if status == 200:
                target.path_index = index
            break
    except Failure as failure:
        return "error", str(failure)
    if status in (401, 403):
        return "error", "key-rejected"
    if status == 429:
        return "error", "rate-limited"
    if status != 200:
        return "error", "server-error"
    if len(data) > MAX_REPLY:
        return "error", "bad-reply"
    try:
        reply = json.loads(data.decode("utf-8"))
    except UnicodeDecodeError:
        return "dropped", "invalid"
    except ValueError:
        return "error", "bad-reply"
    if not isinstance(reply, dict) or not isinstance(reply.get("text"), str):
        return "error", "bad-reply"
    text, reason = clean(reply["text"], target.language, len(pcm) / (2 * RATE))
    return ("text", text) if text is not None else ("dropped", reason)


class Phrase:
    def __init__(self, ident, pcm):
        self.id, self.pcm, self.prompt, self.ready_sent = ident, pcm, None, False


class Helper:
    def __init__(self, peer, recorder, key_file, ca_file):
        self.peer, self.recorder_command, self.key_file, self.ca_file = peer, recorder, key_file, ca_file
        self.target = None
        self.key = None
        self.capturing = False
        self.recorder = None
        self.pending_audio = b""
        self.detector = PhraseDetector()
        self.next_id = 1
        self.current = None  # id of the phrase being spoken
        self.live = False  # recognize the phrase being spoken as it grows
        self.live_prompt = None  # its prompt, asked for once and kept for the final request
        self.live_asked = False
        self.live_frames = 0  # its length at the last partial request
        self.queue = collections.deque()
        self.inflight = None  # (kind, phrase or None, Connection)
        self.results = queue.Queue()
        self.jobs = queue.Queue()
        self.wake_read, self.wake_write = os.pipe()
        os.set_blocking(self.wake_read, False)
        self.last_packet = time.monotonic()
        self.started = 0.0
        self.worker = threading.Thread(target=self.work, daemon=True)
        self.worker.start()

    # Packets
    def send(self, op, **values):
        data = json.dumps(dict(op=op, **values), ensure_ascii=False).encode()
        if len(data) > MAX_PACKET:
            raise Failure("oversized-packet")
        try:
            self.peer.sendall(data)
        except socket.timeout:
            # A keyboard that stops reading must not keep the microphone open.
            self.drop_all()

    def send_level(self, value):
        # Levels are only for display: drop them rather than wait for the keyboard.
        with contextlib.suppress(BlockingIOError, InterruptedError):
            self.peer.send(json.dumps({"op": "level", "value": value}).encode(), socket.MSG_DONTWAIT)

    def command(self, message):
        op = message.get("op")
        if op == "alive":
            return
        if op == "configure":
            self.configure(message)
        elif op == "capture" and isinstance(message.get("on"), bool):
            self.set_capture(message["on"])
        elif op == "prompt" and isinstance(message.get("id"), int) and isinstance(message.get("text"), str):
            head = self.queue[0] if self.queue else None
            if (self.live_asked and self.live_prompt is None and message["id"] == self.current and
                    not self.queue):
                self.live_prompt = message["text"][-MAX_PROMPT:]
            elif head and head.id == message["id"] and head.prompt is None and self.inflight is None:
                head.prompt = message["text"][-MAX_PROMPT:]
        elif op == "cancel":
            self.drop_all()
            # Everything sent before this belongs to the cancelled phrases.
            self.send("cancelled")
        elif op == "test":
            self.start_test()
        else:
            raise Failure("invalid-command")

    def configure(self, message):
        try:
            self.target = Target(message.get("server"), message.get("url"), message.get("model"),
                                 message.get("language"))
        except Failure:
            self.target = None
            raise
        self.key = read_key(self.key_file) if self.target.https else None
        self.live = message.get("live") is True

    # Capture
    def set_capture(self, on):
        if on and not self.capturing:
            if self.target is None:
                raise Failure("not-configured")
            self.start_recorder()
            self.capturing = True
            self.detector.idle = 0
        elif not on:
            if self.capturing:
                self.stop_recorder()
                self.capturing = False
                self.end_phrase(self.detector.finish())  # pausing ends the phrase being spoken
            # Every phrase spoken before the pause has now been announced.
            self.send("paused")

    def start_recorder(self):
        try:
            self.recorder = subprocess.Popen(self.recorder_command, stdin=subprocess.DEVNULL,
                                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                             close_fds=True)
        except OSError:
            raise Failure("no-microphone")
        os.set_blocking(self.recorder.stdout.fileno(), False)
        self.pending_audio = b""

    def stop_recorder(self):
        recorder, self.recorder = self.recorder, None
        if recorder is None:
            return
        with contextlib.suppress(OSError):
            recorder.terminate()
        try:
            recorder.wait(0.5)
        except subprocess.TimeoutExpired:
            recorder.kill()
            with contextlib.suppress(subprocess.TimeoutExpired):
                recorder.wait(0.5)
        with contextlib.suppress(OSError):
            recorder.stdout.close()

    def read_audio(self):
        try:
            data = os.read(self.recorder.stdout.fileno(), FRAME_BYTES * 50)
        except BlockingIOError:
            return
        if not data:
            self.stop_recorder()
            self.capturing = False
            self.drop_all()
            self.send("error", reason="capture-stopped")
            return
        audio = self.pending_audio + data
        whole = len(audio) - len(audio) % FRAME_BYTES
        self.pending_audio = audio[whole:]
        for offset in range(0, whole, FRAME_BYTES):
            for event in self.detector.feed(audio[offset:offset + FRAME_BYTES]):
                self.handle(event)
                if not self.capturing:
                    return

    def handle(self, event):
        kind = event[0]
        if kind == "level":
            self.send_level(event[1])
        elif kind == "start":
            self.current = self.next_id
            self.next_id += 1
            self.live_prompt, self.live_asked, self.live_frames = None, False, 0
            self.send("phrase", id=self.current)
        elif kind == "end":
            self.end_phrase(event[1])
        elif kind == "silence":
            self.stop_recorder()
            self.capturing = False
            self.send("capture", on=False, reason="silence")

    def end_phrase(self, pcm):
        ident, self.current = self.current, None
        prompt, self.live_prompt, self.live_asked = self.live_prompt, None, False
        if ident is None:
            return
        if pcm is None:
            self.send("dropped", id=ident, reason="empty")
            return
        queued = sum(len(p.pcm) for p in self.queue) // FRAME_BYTES + len(pcm) // FRAME_BYTES
        if len(self.queue) >= MAX_QUEUED or queued > MAX_QUEUED_FRAMES:
            self.fail("backlog-full")
            return
        phrase = Phrase(ident, pcm)
        if prompt is not None:
            # Live words asked for this phrase's prompt before its first
            # partial; the final request uses the same one.
            phrase.prompt, phrase.ready_sent = prompt, True
        self.queue.append(phrase)

    # Recognition
    def pump(self):
        if self.inflight:
            return
        if not self.queue:
            self.pump_live()
            return
        head = self.queue[0]
        if head.prompt is None:
            if not head.ready_sent:
                head.ready_sent = True
                self.send("ready", id=head.id)
            return
        connection = Connection()
        self.inflight = ("phrase", head, connection)
        self.started = time.monotonic()
        self.jobs.put((self.target, self.key, head.pcm, head.prompt, connection))

    def pump_live(self):
        """Recognizes the phrase being spoken so far, once nothing else waits."""
        phrase = self.detector.phrase
        if not self.live or not self.capturing or self.current is None or phrase is None:
            return
        frames = len(phrase) // FRAME_BYTES
        if frames < LIVE_FIRST_FRAMES or frames - self.live_frames < LIVE_EVERY_FRAMES:
            return
        if self.live_prompt is None:
            if not self.live_asked:
                self.live_asked = True
                self.send("ready", id=self.current)
            return
        connection = Connection()
        self.inflight = ("partial", self.current, connection)
        self.started = time.monotonic()
        self.live_frames = frames
        self.jobs.put((self.target, self.key, bytes(phrase), self.live_prompt, connection))

    def start_test(self):
        if self.target is None:
            self.send("test", ok=False, reason="not-configured")
            return
        if self.inflight:
            self.send("test", ok=False, reason="busy")
            return
        connection = Connection()
        self.inflight = ("test", None, connection)
        self.started = time.monotonic()
        self.jobs.put((self.target, self.key, b"\0\0" * (RATE // 2), "", connection))

    def work(self):
        while True:
            job = self.jobs.get()
            if job is None:
                return
            target, key, pcm, prompt, connection = job
            try:
                result = recognize(target, key, pcm, prompt, connection, self.ca_file)
            except Exception:  # never let a worker failure leave text or traces behind
                result = ("error", "bad-reply")
            self.results.put((connection, result))
            with contextlib.suppress(OSError):
                os.write(self.wake_write, b"x")

    def finish_request(self):
        with contextlib.suppress(BlockingIOError):
            os.read(self.wake_read, 64)
        while True:
            try:
                connection, (kind, value) = self.results.get_nowait()
            except queue.Empty:
                return
            if not self.inflight or self.inflight[2] is not connection:
                continue  # cancelled or timed out before it returned
            job, phrase, _ = self.inflight
            self.inflight = None
            if job == "test":
                # Any valid reply passes, including empty or dropped text.
                if kind == "error":
                    self.send("test", ok=False, reason=value)
                else:
                    self.send("test", ok=True)
            elif job == "partial":
                # The words so far; one that returned nothing is simply skipped.
                if kind == "text":
                    self.send("partial", id=phrase, text=value)
                elif kind == "error":
                    self.fail(value)
            elif kind == "text":
                self.queue.popleft()
                self.send("text", id=phrase.id, text=value)
            elif kind == "dropped":
                self.queue.popleft()
                self.send("dropped", id=phrase.id, reason=value)
            else:
                self.fail(value)

    def check_timeout(self):
        if self.inflight and time.monotonic() - self.started > REQUEST_TIMEOUT + 1:
            job = self.inflight[0]
            self.inflight[2].abort()
            self.inflight = None
            if job == "test":
                self.send("test", ok=False, reason="timeout")
            else:
                self.fail("timeout")

    def fail(self, reason):
        self.drop_all()
        self.send("error", reason=reason)

    def drop_all(self):
        if self.capturing:
            self.stop_recorder()
            self.capturing = False
        self.detector.reset()
        self.current = None
        self.live_prompt, self.live_asked, self.live_frames = None, False, 0
        self.queue.clear()
        if self.inflight:
            self.inflight[2].abort()
            self.inflight = None

    # Main loop
    def run(self):
        try:
            while True:
                watched = [self.peer, self.wake_read]
                if self.recorder:
                    watched.append(self.recorder.stdout)
                readable, _, _ = select.select(watched, [], [], 0.25)
                if self.peer in readable:
                    data, _, flags, _ = self.peer.recvmsg(MAX_PACKET)
                    if not data:
                        return
                    self.last_packet = time.monotonic()
                    try:
                        if flags & socket.MSG_TRUNC:
                            raise Failure("invalid-command")
                        message = json.loads(data)
                        if not isinstance(message, dict):
                            raise Failure("invalid-command")
                        self.command(message)
                    except (ValueError, Failure) as failure:
                        reason = str(failure) if isinstance(failure, Failure) else "invalid-command"
                        self.fail(reason)
                if self.wake_read in readable:
                    self.finish_request()
                if self.recorder and self.recorder.stdout in readable:
                    try:
                        self.read_audio()
                    except Failure as failure:
                        self.fail(str(failure))
                self.check_timeout()
                if (self.capturing or self.queue or self.inflight) and \
                        time.monotonic() - self.last_packet > ALIVE_TIMEOUT:
                    self.fail("keyboard-stalled")
                    self.last_packet = time.monotonic()
                self.pump()
        except (BrokenPipeError, ConnectionResetError):
            return
        finally:
            self.drop_all()
            self.jobs.put(None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", type=int, required=True)
    parser.add_argument("--key-file", default="")
    # Test-only seams; the keyboard never passes them.
    parser.add_argument("--recorder", default="")
    parser.add_argument("--ca-file", default=None)
    args = parser.parse_args()
    harden()
    peer = socket.socket(fileno=args.fd)
    if peer.family != socket.AF_UNIX or peer.type != socket.SOCK_SEQPACKET:
        return 1
    peer.settimeout(ALIVE_TIMEOUT)
    recorder = [args.recorder] if args.recorder else RECORDER

    def interrupted(signum, frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, interrupted)
    Helper(peer, recorder, args.key_file, args.ca_file).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
