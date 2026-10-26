"""Dictation helper tests with synthetic audio, a fake recorder and a local fake server.

No microphone, network or real speech server is used; servers listen on
127.0.0.1 only and every file lives in a temporary directory.
"""
import contextlib
import hashlib
import http.server
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import random
import shutil
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave

HELPER = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("dictation_helper", HELPER)
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)

FRAME = d.FRAME_SAMPLES


def zeros(seconds):
    return b"\0\0" * int(seconds * d.RATE)


def noise(seconds, db, seed=1):
    rng = random.Random(seed)
    sigma = 32768 * 10 ** (db / 20)
    samples = [max(-32768, min(32767, int(rng.gauss(0, sigma)))) for _ in range(int(seconds * d.RATE))]
    return b"".join(x.to_bytes(2, "little", signed=True) for x in samples)


def speech(seconds, db=-24, background=None, seed=2):
    """A syllable-like tone: 200 Hz bursts with short dips, plus optional background noise."""
    rng = random.Random(seed)
    amplitude = 32768 * 10 ** (db / 20) * math.sqrt(2)
    out = []
    for i in range(int(seconds * d.RATE)):
        t = i / d.RATE
        envelope = 1.0 if (t % 0.25) < 0.2 else 0.05  # dips between syllables
        value = amplitude * envelope * math.sin(2 * math.pi * 200 * t)
        if background is not None:
            value += rng.gauss(0, 32768 * 10 ** (background / 20))
        out.append(max(-32768, min(32767, int(value))))
    return b"".join(x.to_bytes(2, "little", signed=True) for x in out)


def run_detector(pcm):
    detector = d.PhraseDetector()
    events = []
    for offset in range(0, len(pcm) - d.FRAME_BYTES + 1, d.FRAME_BYTES):
        events.extend(detector.feed(pcm[offset:offset + d.FRAME_BYTES]))
    return detector, events


def phrases(events):
    return [e[1] for e in events if e[0] == "end"]


class Detection(unittest.TestCase):
    def test_speech_between_gate_zeros_is_one_phrase_with_preroll(self):
        _, events = run_detector(zeros(1) + speech(1.0) + zeros(1.5))
        ended = phrases(events)
        self.assertEqual([e[0] for e in events if e[0] in ("start", "end")], ["start", "end"])
        self.assertIsNotNone(ended[0])
        seconds = len(ended[0]) / (2 * d.RATE)
        self.assertGreater(seconds, 1.0)
        self.assertLess(seconds, 1.0 + 0.3 + d.TAIL_FRAMES / d.FRAMES_PER_SECOND + 0.05)

    def test_background_noise_while_the_gate_is_open_is_not_speech(self):
        # A dishwasher at -40 dBFS around speech at -24 dBFS, as on the Frame.
        pcm = zeros(0.5) + speech(1.5, background=-40) + noise(1.0, -40) + zeros(1)
        _, events = run_detector(pcm)
        ended = [p for p in phrases(events) if p]
        self.assertEqual(len(ended), 1)
        # The phrase ends during the open-gate noise, not only once the gate closes.
        self.assertLess(len(ended[0]) / (2 * d.RATE), 0.3 + 1.5 + 0.8)

    def test_steady_noise_without_speech_starts_no_lasting_phrase(self):
        _, events = run_detector(noise(6, -40))
        self.assertFalse([p for p in phrases(events) if p])

    def test_a_click_is_dropped(self):
        _, events = run_detector(zeros(0.5) + speech(0.1) + zeros(1))
        self.assertEqual(phrases(events), [None])

    def test_long_speech_is_split_without_shared_audio(self):
        pcm = speech(30)
        _, events = run_detector(pcm + zeros(1))
        ended = [p for p in phrases(events) if p]
        self.assertEqual(len(ended), 2)
        self.assertLessEqual(len(ended[0]), d.MAX_PHRASE_FRAMES * d.FRAME_BYTES)
        self.assertLessEqual(sum(len(p) for p in ended), len(pcm) + 0.3 * 2 * d.RATE)

    def test_sixty_seconds_without_speech_stops(self):
        _, events = run_detector(zeros(61))
        self.assertIn(("silence",), events)

    def test_pause_finishes_the_phrase_being_spoken(self):
        detector, _ = run_detector(zeros(0.5) + speech(1.0))
        self.assertIsNotNone(detector.finish())
        self.assertIsNone(detector.finish())

    def test_levels_are_flat_in_silence_and_rate_limited(self):
        _, events = run_detector(zeros(1) + speech(1))
        levels = [e[1] for e in events if e[0] == "level"]
        self.assertEqual(len(levels), 2 * d.FRAMES_PER_SECOND // d.LEVEL_EVERY)
        self.assertEqual(set(levels[:10]), {0})
        self.assertGreater(max(levels), 100)


class Cleanup(unittest.TestCase):
    def test_trims_annotations_and_controls(self):
        self.assertEqual(d.clean(" [BLANK_AUDIO] Hello\x07 (music) world ♪\n", "en", 3), ("Hello world", None))

    def test_collapses_runs_of_four_or_more(self):
        self.assertEqual(d.clean("no no no no no stop", "en", 5)[0], "no stop")
        self.assertEqual(d.clean("no no no stop", "en", 5)[0], "no no no stop")
        self.assertEqual(d.clean("ありがとうありがとうありがとうありがとう", "ja", 5)[0], "ありがとう")

    def test_rejects_empty_invalid_and_too_long(self):
        self.assertEqual(d.clean(" [BLANK_AUDIO] ", "en", 1), (None, "empty"))
        self.assertEqual(d.clean("bad \ud800", "en", 1), (None, "invalid"))
        self.assertEqual(d.clean(None, "en", 1), (None, "invalid"))
        self.assertEqual(d.clean(" ".join(f"w{i}" for i in range(40)), "en", 1)[1], "too-long")


class Targets(unittest.TestCase):
    def test_server_and_url_forms(self):
        own = d.Target(server="192.168.1.20")
        self.assertEqual((own.https, own.host, own.port), (False, "192.168.1.20", 8031))
        self.assertEqual(own.paths, ["/v1/audio/transcriptions", "/inference"])
        self.assertEqual(d.Target(server="[fd00::1]:9000").host, "fd00::1")
        hosted = d.Target(url="https://api.example.com/openai/v1/", model="m")
        self.assertEqual((hosted.https, hosted.port, hosted.paths), (True, 443, ["/openai/v1/audio/transcriptions"]))
        for bad in [dict(), dict(server="a b"), dict(server="host:99999"), dict(url="ftp://x/v1"),
                    dict(url="https://user:pw@x/v1"), dict(url="https://x/v1?k=1"),
                    dict(server="a", url="https://x"), dict(server="a", language="EN")]:
            with self.subTest(bad=bad), self.assertRaises(d.Failure):
                d.Target(**bad)

    def test_key_file_must_be_private(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dictation-key"
            path.write_text("sk-test-123\n")
            path.chmod(0o600)
            self.assertEqual(d.read_key(str(path)), "sk-test-123")
            path.chmod(0o644)
            self.assertIsNone(d.read_key(str(path)))
            self.assertIsNone(d.read_key(str(Path(directory) / "missing")))


class FakeServer:
    """Records each request; `reply` decides the response for every path."""

    def __init__(self, tls_context=None):
        self.requests = []
        self.reply = lambda path, fields: (200, json.dumps({"text": " hello\n"}).encode())
        self.delay = 0
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers["Content-Length"])
                body = self.rfile.read(length)
                fields = parse_multipart(self.headers["Content-Type"], body)
                outer.requests.append((self.path, dict(self.headers), fields))
                if outer.delay:
                    time.sleep(outer.delay)
                status, data = outer.reply(self.path, fields)
                self.send_response(status)
                if status == 302:
                    self.send_header("Location", "https://example.invalid/steal")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                with contextlib.suppress(OSError):
                    self.wfile.write(data)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        if tls_context:
            self.server.socket = tls_context.wrap_socket(self.server.socket, server_side=True)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def parse_multipart(content_type, body):
    boundary = content_type.split("boundary=")[1].encode()
    fields = {}
    for part in body.split(b"--" + boundary)[1:-1]:
        head, _, value = part.strip(b"\r\n").partition(b"\r\n\r\n")
        name = head.split(b'name="')[1].split(b'"')[0].decode()
        if b"filename=" in head:
            fields["filename"] = head.split(b'filename="')[1].split(b'"')[0].decode()
        fields[name] = value
    return fields


def make_ca(directory):
    """A throwaway CA and a 127.0.0.1 certificate, or None without openssl."""
    if not shutil.which("openssl"):
        return None
    run = lambda *a: subprocess.run(["openssl", *a], cwd=directory, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem",
        "-days", "1", "-subj", "/CN=Full Keyboard test CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-out", "server.csr",
        "-subj", "/CN=127.0.0.1")
    # Strict X.509 checks in newer Python versions need these extensions.
    Path(directory, "ext").write_text("subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
                                      "keyUsage=critical,digitalSignature,keyEncipherment\n"
                                      "extendedKeyUsage=serverAuth\n"
                                      "authorityKeyIdentifier=keyid,issuer\nsubjectKeyIdentifier=hash\n")
    run("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
        "-out", "server.pem", "-days", "1", "-extfile", "ext")
    return Path(directory)


PCM = speech(1.0)


class Requests(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.addCleanup(self.server.close)
        self.target = d.Target(server=f"127.0.0.1:{self.server.port}", language="en")

    def recognize(self, target=None, key=None, prompt="Earlier text.", ca_file=None):
        return d.recognize(target or self.target, key, PCM, prompt, d.Connection(), ca_file)

    def test_request_fields_and_upload(self):
        self.assertEqual(self.recognize(), ("text", "hello"))
        path, headers, fields = self.server.requests[0]
        self.assertEqual(path, "/v1/audio/transcriptions")
        self.assertEqual(fields["filename"], "phrase.wav")
        self.assertEqual((fields["language"], fields["prompt"], fields["temperature"], fields["response_format"]),
                         (b"en", b"Earlier text.", b"0", b"json"))
        self.assertNotIn("model", fields)
        with wave.open(io.BytesIO(fields["file"])) as wav:
            self.assertEqual((wav.getframerate(), wav.getnchannels(), wav.getsampwidth()), (16000, 1, 2))
        self.assertNotIn("Authorization", headers)

    def test_model_only_when_configured_and_no_key_over_http(self):
        target = d.Target(url=f"http://127.0.0.1:{self.server.port}/v1", model="whisper-large-v3-turbo")
        self.recognize(target, key="sk-secret")
        _, headers, fields = self.server.requests[0]
        self.assertEqual(fields["model"], b"whisper-large-v3-turbo")
        self.assertNotIn("Authorization", headers)

    def test_inference_fallback_is_remembered(self):
        self.server.reply = lambda path, f: ((200, b'{"text":"ok"}') if path == "/inference" else (404, b""))
        self.assertEqual(self.recognize(), ("text", "ok"))
        self.assertEqual(self.recognize(), ("text", "ok"))
        self.assertEqual([r[0] for r in self.server.requests],
                         ["/v1/audio/transcriptions", "/inference", "/inference"])

    def test_status_codes_and_bad_replies(self):
        cases = [((401, b""), ("error", "key-rejected")), ((403, b""), ("error", "key-rejected")),
                 ((429, b""), ("error", "rate-limited")), ((500, b"{}"), ("error", "server-error")),
                 ((302, b""), ("error", "server-error")), ((200, b"not json"), ("error", "bad-reply")),
                 ((200, b'{"no":"text"}'), ("error", "bad-reply")),
                 ((200, b'{"text":"' + b"a" * (70 * 1024) + b'"}'), ("error", "bad-reply")),
                 ((200, b'{"text":"\xff"}'), ("dropped", "invalid")),
                 ((200, b'{"text":" [BLANK_AUDIO]"}'), ("dropped", "empty"))]
        for reply, expected in cases:
            with self.subTest(reply=reply[0]):
                self.server.reply = lambda path, f, reply=reply: reply
                self.assertEqual(self.recognize(), expected)

    def test_redirects_are_not_followed(self):
        self.server.reply = lambda path, f: (302, b"")
        self.recognize()
        self.assertEqual(len(self.server.requests), 1)

    def test_unreachable_and_timeout(self):
        closed = socket.socket()
        closed.bind(("127.0.0.1", 0))
        port = closed.getsockname()[1]
        closed.close()
        self.assertEqual(self.recognize(d.Target(server=f"127.0.0.1:{port}")), ("error", "unreachable"))
        self.server.delay = 1.5
        previous, d.REQUEST_TIMEOUT = d.REQUEST_TIMEOUT, 0.5
        try:
            self.assertEqual(self.recognize(), ("error", "timeout"))
        finally:
            d.REQUEST_TIMEOUT = previous

    def test_https_sends_the_key_and_verifies_the_certificate(self):
        with tempfile.TemporaryDirectory() as directory:
            ca = make_ca(directory)
            if ca is None:
                self.skipTest("openssl is not installed")
            context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
            context.load_cert_chain(ca / "server.pem", ca / "server.key")
            server = FakeServer(context)
            self.addCleanup(server.close)
            target = d.Target(url=f"https://127.0.0.1:{server.port}/v1")
            self.assertEqual(d.recognize(target, "sk-secret", PCM, "", d.Connection(), str(ca / "ca.pem")),
                             ("text", "hello"))
            self.assertEqual(server.requests[-1][1].get("Authorization"), "Bearer sk-secret")
            # The system store does not trust the test CA, so nothing is sent.
            count = len(server.requests)
            self.assertEqual(d.recognize(target, "sk-secret", PCM, "", d.Connection(), None),
                             ("error", "unreachable"))
            self.assertEqual(len(server.requests), count)


class Downloads(unittest.TestCase):
    """Model downloads from a local HTTPS server with a test CA, redirecting
    once like Hugging Face's CDN."""

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory, True)
        ca = make_ca(self.directory)
        if ca is None:
            self.skipTest("openssl is not installed")
        self.ca = str(ca / "ca.pem")
        self.content = os.urandom(3 << 20)
        self.served = {"ggml-small-q8_0.bin": self.content, "ggml-small.bin": self.content}
        self.redirect = None
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                name = self.path.rsplit("/", 1)[-1]
                if "/resolve/" in self.path:
                    self.send_response(302)
                    self.send_header("Location", outer.redirect or f"https://127.0.0.1:{outer.port}/cdn/{name}")
                    self.end_headers()
                    return
                data = outer.served.get(name)
                if data is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                with contextlib.suppress(OSError):
                    self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        context.load_cert_chain(ca / "server.pem", ca / "server.key")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        self.port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        digest = hashlib.sha256(self.content).hexdigest()
        self.models = dict(d.MODELS)
        d.MODELS["small-q8_0"] = ("ggml-small-q8_0.bin", len(self.content), digest)
        d.MODELS["small-q4_0"] = ("ggml-small.bin", len(self.content), digest)
        self.addCleanup(lambda: (d.MODELS.clear(), d.MODELS.update(self.models)))
        self.models_dir = self.directory / "models"

    def download(self, name, quantize=None, cancel=False):
        events = []
        job = d.Download(name, str(self.models_dir), quantize, self.ca,
                         lambda state, **values: events.append(dict(state=state, **values)),
                         origin=("127.0.0.1", self.port))
        if cancel:
            job.cancelled = True
        job.start()
        job.join(30)
        return events

    def test_a_verified_model_is_renamed_into_place(self):
        events = self.download("small-q8_0")
        self.assertEqual(events[-1], {"state": "ready"})
        progress = [e for e in events if e["state"] == "progress"]
        self.assertTrue(progress and progress[-1]["done"] == len(self.content))
        self.assertEqual((self.models_dir / "ggml-small-q8_0.bin").read_bytes(), self.content)
        self.assertEqual(stat.S_IMODE(os.stat(self.models_dir).st_mode), 0o700)
        self.assertEqual(sorted(p.name for p in self.models_dir.iterdir()), ["ggml-small-q8_0.bin"])

    def test_a_wrong_file_is_never_kept(self):
        self.served["ggml-small-q8_0.bin"] = self.content[:-1] + b"x"
        self.assertEqual(self.download("small-q8_0")[-1], {"state": "failed", "reason": "checksum"})
        self.assertEqual(list(self.models_dir.iterdir()), [])

    def test_redirects_only_to_https_model_hosts(self):
        for location in (f"http://127.0.0.1:{self.port}/cdn/ggml-small-q8_0.bin",
                         "https://example.invalid/ggml-small-q8_0.bin"):
            self.redirect = location
            self.assertEqual(self.download("small-q8_0")[-1], {"state": "failed", "reason": "unreachable"})
        self.assertTrue(d.model_host("us.aws.cdn.hf.co") and d.model_host("cdn-lfs.huggingface.co"))
        self.assertFalse(d.model_host("huggingface.co.example.com") or d.model_host("evilhf.co"))

    def test_the_4_bit_model_is_made_from_the_f16_file(self):
        tool = self.directory / "whisper-quantize"
        tool.write_text(f"#!/bin/sh\n[ \"$3\" = q4_0 ] && cp \"$1\" \"$2\"\n")
        tool.chmod(0o755)
        events = self.download("small-q4_0", str(tool))
        self.assertEqual([e["state"] for e in events][-2:], ["quantizing", "ready"])
        self.assertEqual(sorted(p.name for p in self.models_dir.iterdir()), ["ggml-small-q4_0.bin"])
        tool.write_text("#!/bin/sh\nexit 1\n")
        (self.models_dir / "ggml-small-q4_0.bin").unlink()
        self.assertEqual(self.download("small-q4_0", str(tool))[-1], {"state": "failed", "reason": "quantize"})
        self.assertEqual(list(self.models_dir.iterdir()), [])

    def test_cancel(self):
        self.assertEqual(self.download("small-q8_0", cancel=True)[-1], {"state": "failed", "reason": "cancelled"})
        self.assertFalse((self.models_dir / "ggml-small-q8_0.bin").exists())


RECORDER = r'''
import os, sys, time
path, mode = sys.argv[1], sys.argv[2]
with open(path, "rb") as f:
    data = f.read()
with open(os.environ["FK_RECORDER_PID"], "w") as f:
    f.write(str(os.getpid()))
try:
    if mode == "loop":  # someone who keeps talking, at twice real time
        while True:
            for offset in range(0, len(data), 3200):
                sys.stdout.buffer.write(data[offset:offset + 3200])
                sys.stdout.buffer.flush()
                time.sleep(0.05)
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    if mode == "exit":
        sys.exit(0)
    while True:  # an open microphone delivers gate zeros until stopped
        sys.stdout.buffer.write(b"\0" * 6400)
        sys.stdout.buffer.flush()
        time.sleep(0.05)
except BrokenPipeError:
    sys.exit(0)
'''


FAKE_ENGINE = r'''
import json, os, sys
# Stands in for framekeyboard-whisper: counts its starts, answers each request
# with how many samples it got.
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
with open(os.environ["FK_ENGINE_LOG"], "a") as log:
    log.write("start " + args["--model"] + "\n")
if not os.path.isfile(args["--model"]):
    print(json.dumps({"error": "model"}), flush=True)
    sys.exit(1)
print(json.dumps({"ready": True}), flush=True)
while True:
    line = sys.stdin.buffer.readline()
    if not line:
        break
    header = json.loads(line)
    sys.stdin.buffer.read(header["samples"] * 2)
    with open(os.environ["FK_ENGINE_LOG"], "a") as log:
        log.write("prompt " + header["prompt"] + "\n")
    print(json.dumps({"text": " on device %d" % header["samples"]}), flush=True)
'''


def fake_engine(test):
    directory = Path(tempfile.mkdtemp())
    test.addCleanup(shutil.rmtree, directory, True)
    (directory / "engine.py").write_text(FAKE_ENGINE)
    wrapper = directory / "engine"
    wrapper.write_text(f"#!/bin/sh\nexec {sys.executable} -I {directory / 'engine.py'} \"$@\"\n")
    wrapper.chmod(0o755)
    model = directory / "ggml-small-q8_0.bin"
    model.write_bytes(b"model")
    log = directory / "engine.log"
    os.environ["FK_ENGINE_LOG"] = str(log)
    return wrapper, model, log


class Device(unittest.TestCase):
    def test_engine_loads_once_and_unloads_when_idle(self):
        engine, model, log = fake_engine(self)
        e = d.Engine(str(engine))
        self.addCleanup(e.close)
        self.assertEqual(e.recognize(str(model), "en", "Before.", PCM, d.Connection()), ("text", " on device %d" % (len(PCM) // 2)))
        self.assertEqual(e.recognize(str(model), "en", "", PCM, d.Connection())[0], "text")
        self.assertEqual(log.read_text().count("start"), 1)
        process = e.process
        e.unload_if_idle(time.monotonic())
        self.assertIs(e.process, process)
        e.unload_if_idle(time.monotonic() + d.DEVICE_IDLE + 1)
        self.assertIsNone(e.process)
        self.assertIsNotNone(process.poll())

    def test_missing_model_and_engine(self):
        engine, model, _ = fake_engine(self)
        self.assertEqual(d.Engine(str(engine)).recognize(str(model) + ".gone", "en", "", PCM, d.Connection()),
                         ("error", "model-missing"))
        self.assertEqual(d.Engine(None).recognize(str(model), "en", "", PCM, d.Connection()),
                         ("error", "device-failed"))

    def test_a_slow_engine_times_out_and_a_dead_one_failed(self):
        engine, model, _ = fake_engine(self)
        slow = engine.parent / "slow"
        slow.write_text("#!/bin/sh\necho '{\"ready\":true}'\nexec sleep 30\n")
        slow.chmod(0o755)
        dead = engine.parent / "dead"
        dead.write_text("#!/bin/sh\necho '{\"ready\":true}'\nread line\nexit 1\n")
        dead.chmod(0o755)
        limit = d.DEVICE_TIMEOUT
        d.DEVICE_TIMEOUT = 0.5
        self.addCleanup(setattr, d, "DEVICE_TIMEOUT", limit)
        for path, reason in ((slow, "timeout"), (dead, "device-failed")):
            e = d.Engine(str(path))
            self.addCleanup(e.close)
            self.assertEqual(e.recognize(str(model), "en", "", PCM, d.Connection()), ("error", reason))

    def test_cancel_kills_the_engine(self):
        engine, model, _ = fake_engine(self)
        e = d.Engine(str(engine))
        self.addCleanup(e.close)
        e.recognize(str(model), "en", "", PCM, d.Connection())
        connection = d.Connection()
        connection.abort()
        self.assertEqual(e.recognize(str(model), "en", "", PCM, connection), ("error", "cancelled"))


class Process:
    """The real helper on a private socket, with a fake recorder streaming `audio`."""

    def __init__(self, test, audio, mode="stay", key=None, ca=None, engine=None):
        self.directory = Path(tempfile.mkdtemp())
        test.addCleanup(shutil.rmtree, self.directory, True)
        (self.directory / "audio.raw").write_bytes(audio)
        script = self.directory / "recorder.py"
        script.write_text(RECORDER)
        wrapper = self.directory / "recorder"
        wrapper.write_text(f"#!/bin/sh\nexec {sys.executable} -I {script} {self.directory / 'audio.raw'} {mode}\n")
        wrapper.chmod(0o755)
        self.pid_file = self.directory / "recorder.pid"
        key_file = self.directory / "dictation-key"
        if key:
            key_file.write_text(key)
            key_file.chmod(0o600)
        self.sock, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        args = [sys.executable, "-I", str(HELPER), "--fd", str(child.fileno()), "--key-file", str(key_file),
                "--recorder", str(wrapper)] + (["--ca-file", ca] if ca else []) + \
               (["--engine", str(engine)] if engine else [])
        env = dict(os.environ, FK_RECORDER_PID=str(self.pid_file))
        self.process = subprocess.Popen(args, pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        child.close()
        self.sock.settimeout(10)
        test.addCleanup(self.close)

    def send(self, **message):
        self.sock.send(json.dumps(message).encode())

    def receive(self, op, timeout=10, keep_alive=True):
        """The next packet with this op, skipping levels; keeps the helper alive meanwhile."""
        skipped = ("level",) if op == "phrase" else ("level", "phrase")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.sock.settimeout(0.3)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                if keep_alive:
                    self.send(op="alive")
                continue
            if not data:
                raise EOFError
            message = json.loads(data)
            if message["op"] == op:
                return message
            if message["op"] not in skipped:
                self.seen.append(message)
        raise TimeoutError(op)

    seen = []

    def recorder_alive(self):
        if not self.pid_file.exists():
            return False
        pid = int(self.pid_file.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        # A zombie child of the helper has exited too.
        with contextlib.suppress(OSError):
            if Path(f"/proc/{pid}/stat").read_text().split(") ")[1].startswith("Z"):
                return False
        return True

    def close(self):
        with contextlib.suppress(OSError):
            self.sock.close()
        try:
            self.output = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.output = self.process.communicate()


TWO_PHRASES = zeros(0.5) + speech(1.0) + zeros(1.0) + speech(1.0, seed=3) + zeros(1.0)


class Helper(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.addCleanup(self.server.close)
        Process.seen = []

    def start(self, audio=TWO_PHRASES, **kwargs):
        helper = Process(self, audio, **kwargs)
        helper.send(op="configure", server=f"127.0.0.1:{self.server.port}", language="en")
        return helper

    def test_phrases_are_recognized_one_at_a_time_with_the_keyboards_context(self):
        replies = iter(["first", "second"])
        self.server.reply = lambda path, f: (200, json.dumps({"text": next(replies)}).encode())
        self.server.delay = 0.3
        helper = self.start()
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("ready")["id"], 1)
        helper.send(op="prompt", id=1, text="Before.")
        self.assertEqual(helper.receive("text"), {"op": "text", "id": 1, "text": "first"})
        # The second phrase ended long ago, but waits for the first text.
        self.assertEqual(helper.receive("ready")["id"], 2)
        helper.send(op="prompt", id=2, text="Before. first")
        self.assertEqual(helper.receive("text")["text"], "second")
        self.assertEqual([r[2]["prompt"] for r in self.server.requests], [b"Before.", b"Before. first"])

    def test_pause_recognizes_the_phrase_being_spoken_and_releases_the_microphone(self):
        helper = self.start(speech(1.0), mode="loop")
        helper.send(op="capture", on=True)
        helper.receive("phrase")
        time.sleep(0.3)
        self.assertTrue(helper.recorder_alive())
        helper.send(op="capture", on=False)
        ready = helper.receive("ready")
        self.assertFalse(helper.recorder_alive())
        helper.send(op="prompt", id=ready["id"], text="")
        self.assertEqual(helper.receive("text")["text"], "hello")

    def test_live_words_recognize_the_phrase_as_it_grows(self):
        helper = Process(self, speech(1.0), mode="loop")
        helper.send(op="configure", server=f"127.0.0.1:{self.server.port}", language="en", live=True)
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("phrase")["id"], 1)
        # The prompt is asked for once, while the phrase is still being spoken.
        self.assertEqual(helper.receive("ready")["id"], 1)
        self.assertTrue(helper.recorder_alive())
        helper.send(op="prompt", id=1, text="Before.")
        self.assertEqual(helper.receive("partial"), {"op": "partial", "id": 1, "text": "hello"})
        helper.receive("partial")
        helper.send(op="capture", on=False)
        self.assertEqual(helper.receive("text"), {"op": "text", "id": 1, "text": "hello"})
        self.assertNotIn("ready", [m["op"] for m in helper.seen])
        # Every request, partial or final, carries the same prompt; each
        # partial uploads the phrase so far, so later ones are longer.
        prompts = [r[2]["prompt"] for r in self.server.requests]
        sizes = [len(r[2]["file"]) for r in self.server.requests]
        self.assertGreaterEqual(len(prompts), 3)
        self.assertEqual(set(prompts), {b"Before."})
        self.assertEqual(sizes, sorted(sizes))
        self.assertGreater(sizes[1], sizes[0])

    def test_without_live_words_nothing_is_recognized_early(self):
        helper = self.start(speech(1.0), mode="loop")
        helper.send(op="capture", on=True)
        helper.receive("phrase")
        time.sleep(2.0)
        helper.send(op="capture", on=False)
        ready = helper.receive("ready")
        helper.send(op="prompt", id=ready["id"], text="")
        helper.receive("text")
        self.assertEqual(len(self.server.requests), 1)
        self.assertNotIn("partial", [m["op"] for m in helper.seen])

    def test_on_device_phrases_use_the_engine_with_the_keyboards_prompt(self):
        engine, model, log = fake_engine(self)
        helper = Process(self, TWO_PHRASES, engine=engine)
        helper.send(op="configure", device=str(model), language="en")
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("ready")["id"], 1)
        helper.send(op="prompt", id=1, text="Before.")
        first = helper.receive("text")
        self.assertTrue(first["text"].startswith("on device") and "device" not in first)
        ready = helper.receive("ready")
        helper.send(op="prompt", id=ready["id"], text="Before. first")
        helper.receive("text")
        self.assertEqual(self.server.requests, [])
        self.assertEqual(log.read_text().count("start"), 1)
        self.assertIn("prompt Before.\n", log.read_text())

    def test_fallback_recognizes_on_device_when_the_server_cannot(self):
        engine, model, _ = fake_engine(self)
        helper = Process(self, speech(1.0) + zeros(1.0), engine=engine)
        with socket.socket() as closed:
            closed.bind(("127.0.0.1", 0))
            port = closed.getsockname()[1]
        helper.send(op="configure", server=f"127.0.0.1:{port}", language="en", device=str(model),
                    fallback=True)
        helper.send(op="capture", on=True)
        helper.send(op="prompt", id=helper.receive("ready")["id"], text="")
        text = helper.receive("text")
        self.assertTrue(text["text"].startswith("on device") and text["device"] is True)

    def test_without_fallback_an_unreachable_server_is_an_error(self):
        engine, model, log = fake_engine(self)
        helper = Process(self, speech(1.0) + zeros(1.0), engine=engine)
        with socket.socket() as closed:
            closed.bind(("127.0.0.1", 0))
            port = closed.getsockname()[1]
        helper.send(op="configure", server=f"127.0.0.1:{port}", language="en", device=str(model))
        helper.send(op="capture", on=True)
        helper.send(op="prompt", id=helper.receive("ready")["id"], text="")
        self.assertEqual(helper.receive("error")["reason"], "unreachable")
        self.assertFalse(log.exists())

    def test_every_pause_is_confirmed(self):
        helper = self.start(speech(1.0), mode="loop")
        helper.send(op="capture", on=False)
        helper.receive("paused")  # even when nothing was listening
        helper.send(op="capture", on=True)
        phrase = helper.receive("phrase")
        time.sleep(0.3)
        helper.send(op="capture", on=False)
        helper.receive("paused")  # after the phrase it ends was announced
        self.assertEqual(helper.receive("ready")["id"], phrase["id"])

    def test_cancel_drops_pending_phrases(self):
        self.server.delay = 2
        helper = self.start()
        helper.send(op="capture", on=True)
        helper.send(op="prompt", id=helper.receive("ready")["id"], text="")
        time.sleep(0.3)
        helper.send(op="cancel")
        helper.receive("cancelled")  # ends everything that belongs to the cancelled phrases
        with self.assertRaises(TimeoutError):
            helper.receive("text", timeout=3)
        self.assertFalse(helper.recorder_alive())

    def test_recorder_end_is_reported(self):
        helper = self.start(zeros(0.2), mode="exit")
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("error")["reason"], "capture-stopped")

    def test_capture_needs_configuration(self):
        helper = Process(self, zeros(1))
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("error")["reason"], "not-configured")
        helper.send(op="test")
        self.assertEqual(helper.receive("test"), {"op": "test", "ok": False, "reason": "not-configured"})

    def test_test_sends_half_a_second_of_silence(self):
        self.server.reply = lambda path, f: (200, b'{"text":" you"}')
        helper = self.start()
        helper.send(op="test")
        self.assertEqual(helper.receive("test"), {"op": "test", "ok": True})
        with wave.open(io.BytesIO(self.server.requests[0][2]["file"])) as wav:
            self.assertEqual(wav.getnframes(), 8000)

    def test_server_failure_stops_capture(self):
        self.server.reply = lambda path, f: (429, b"")
        helper = self.start()
        helper.send(op="capture", on=True)
        helper.send(op="prompt", id=helper.receive("ready")["id"], text="")
        self.assertEqual(helper.receive("error")["reason"], "rate-limited")
        self.assertFalse(helper.recorder_alive())

    def test_backlog_is_bounded(self):
        self.server.delay = 5
        bursts = zeros(0.5) + b"".join(speech(0.5, seed=i) + zeros(0.8) for i in range(10))
        helper = self.start(bursts)
        helper.send(op="capture", on=True)
        helper.send(op="prompt", id=helper.receive("ready")["id"], text="")
        self.assertEqual(helper.receive("error")["reason"], "backlog-full")

    def test_a_stalled_keyboard_stops_capture(self):
        helper = self.start(zeros(1))
        helper.send(op="capture", on=True)
        self.assertEqual(helper.receive("error", timeout=6, keep_alive=False)["reason"], "keyboard-stalled")
        self.assertFalse(helper.recorder_alive())

    def test_end_of_file_stops_the_helper_and_recorder(self):
        helper = self.start(zeros(1))
        helper.send(op="capture", on=True)
        deadline = time.monotonic() + 5
        while not helper.recorder_alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(helper.recorder_alive())
        helper.close()
        self.assertEqual(helper.process.returncode, 0)
        self.assertFalse(helper.recorder_alive())

    def test_a_killed_helper_leaves_no_recorder(self):
        helper = self.start(zeros(1))
        helper.send(op="capture", on=True)
        deadline = time.monotonic() + 5
        while not helper.recorder_alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        helper.process.kill()
        helper.process.wait()
        deadline = time.monotonic() + 3
        while helper.recorder_alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(helper.recorder_alive())

    def test_invalid_commands_are_reported(self):
        helper = self.start()
        helper.sock.send(b"not json")
        self.assertEqual(helper.receive("error")["reason"], "invalid-command")
        helper.send(op="unknown")
        self.assertEqual(helper.receive("error")["reason"], "invalid-command")

    def test_nothing_is_printed_and_core_dumps_are_off(self):
        marker = "zebra marker 4217"
        self.server.reply = lambda path, f: (200, json.dumps({"text": marker}).encode())
        helper = self.start()
        helper.send(op="capture", on=True)
        ready = helper.receive("ready")
        limits = Path(f"/proc/{helper.process.pid}/limits").read_text()
        self.assertRegex(limits, r"Max core file size\s+0\s+0")
        helper.send(op="prompt", id=ready["id"], text="secret context 9931")
        self.assertEqual(helper.receive("text")["text"], marker)
        helper.close()
        self.assertEqual(helper.output, (b"", b""))

    def test_key_goes_only_to_https(self):
        with tempfile.TemporaryDirectory() as directory:
            ca = make_ca(directory)
            if ca is None:
                self.skipTest("openssl is not installed")
            context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
            context.load_cert_chain(ca / "server.pem", ca / "server.key")
            secure = FakeServer(context)
            self.addCleanup(secure.close)
            helper = Process(self, zeros(1), key="sk-file-key", ca=str(ca / "ca.pem"))
            helper.send(op="configure", url=f"https://127.0.0.1:{secure.port}/v1")
            helper.send(op="test")
            self.assertTrue(helper.receive("test")["ok"])
            self.assertEqual(secure.requests[0][1].get("Authorization"), "Bearer sk-file-key")
            helper.send(op="configure", server=f"127.0.0.1:{self.server.port}")
            helper.send(op="test")
            self.assertTrue(helper.receive("test")["ok"])
            self.assertNotIn("Authorization", self.server.requests[0][1])


if __name__ == "__main__":
    unittest.main()
