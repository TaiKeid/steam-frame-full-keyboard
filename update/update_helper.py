"""Manual release updates over a private socket. No listeners or background checks."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import select
import shutil
import signal
import socket
import subprocess
import tarfile
import tempfile
import time
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener

REPOSITORY = "TaiKeid/steam-frame-full-keyboard"
API = f"https://api.github.com/repos/{REPOSITORY}/releases"
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_UNPACKED = 256 * 1024 * 1024


class UpdateError(Exception):
    pass


def version(value):
    if not isinstance(value, str):
        raise UpdateError("invalid-version")
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value)
    if not match or len(value) > 40:
        raise UpdateError("invalid-version")
    return tuple(int(item) for item in match.groups())


class ReleaseRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parsed = urlsplit(newurl)
        if (parsed.scheme != "https" or parsed.hostname not in
                {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
                or parsed.username or parsed.password or parsed.port not in (None, 443)):
            raise UpdateError("invalid-redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


@contextlib.contextmanager
def network_deadline():
    def expired(signum, frame):
        raise UpdateError("download-timeout")
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, 120)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def download(url, output, limit, expected_size=None):
    with network_deadline():
        return download_response(url, output, limit, expected_size)


def download_response(url, output, limit, expected_size=None):
    # Only callers' fixed API or validated repository asset URLs reach this function.
    opener = build_opener(ProxyHandler({}), ReleaseRedirects())
    request = Request(url, headers={"User-Agent": "FullKeyboard-Updater/1",
                                   "Accept": "application/vnd.github+json"})
    started = time.monotonic()
    digest = hashlib.sha256()
    size = 0
    with opener.open(request, timeout=10) as response:
        length = response.headers.get("Content-Length")
        if length and (int(length) > limit or
                       (expected_size is not None and int(length) != expected_size)):
            raise UpdateError("invalid-download-size")
        while True:
            chunk = response.read(64 * 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > limit or time.monotonic() - started > 120:
                raise UpdateError("download-limit")
            output.write(chunk)
            digest.update(chunk)
    if expected_size is not None and size != expected_size:
        raise UpdateError("incomplete-download")
    return digest.hexdigest()


def read_url(url, limit):
    import io
    output = io.BytesIO()
    download(url, output, limit)
    return output.getvalue()


def release(tag=None):
    data = json.loads(read_url(API + ("/tags/" + quote(tag, safe="") if tag else "/latest"), 1024 * 1024))
    if not isinstance(data, dict) or data.get("draft") is not False or data.get("prerelease") is not False:
        raise UpdateError("not-stable-release")
    tag = data.get("tag_name", "")
    parts = version(tag)
    number = ".".join(map(str, parts))
    archive = f"framekeyboard-{number}-aarch64.tar.gz"
    assets = {}
    for asset in data.get("assets", []):
        if asset.get("name") not in (archive, "SHA256SUMS"):
            continue
        name = asset["name"]
        url = f"https://github.com/{REPOSITORY}/releases/download/{quote(tag, safe='')}/{name}"
        if name in assets or asset.get("browser_download_url") != url:
            raise UpdateError("invalid-asset")
        size = asset.get("size")
        if type(size) is not int or size <= 0 or size > (MAX_ARCHIVE if name == archive else 65536):
            raise UpdateError("invalid-asset-size")
        assets[name] = asset
    if set(assets) != {archive, "SHA256SUMS"}:
        raise UpdateError("missing-assets")
    return {"tag": tag, "number": number, "parts": parts, "archive": archive, "assets": assets}


def expected_checksum(contents, name):
    matches = []
    for line in contents.decode("ascii").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if match and match[2] == name:
            matches.append(match[1].lower())
    if len(matches) != 1:
        raise UpdateError("missing-or-duplicate-checksum")
    return matches[0]


def extract(archive, destination, number):
    prefix = f"framekeyboard-{number}-aarch64"
    names = set()
    total = 0
    with tarfile.open(archive, "r:gz") as source:
        members = []
        for item in source:
            parts = PurePosixPath(item.name).parts
            if (not parts or parts[0] != prefix or item.name.startswith("/") or
                    any(p in ("..", ".") for p in item.name.split("/")) or
                    "\\" in item.name or str(PurePosixPath(item.name)) in names or
                    item.name.rstrip("/") != str(PurePosixPath(item.name)) or
                    not (item.isdir() or item.isfile()) or item.mode & 0o7000):
                raise UpdateError("unsafe-archive")
            names.add(str(PurePosixPath(item.name)))
            total += item.size
            if item.size < 0 or total > MAX_UNPACKED or len(names) > 10000:
                raise UpdateError("archive-limit")
            members.append(item)
        # Reject links, devices, duplicate paths and traversal before writing any member.
        source.extractall(destination, members=members, filter="data")
    return destination / prefix


def validate(package, number):
    for filename in ("bin/framekeyboard", "bin/framekeyboard-native-guard"):
        path = package / filename
        if not path.is_file() or not os.access(path, os.X_OK):
            raise UpdateError("missing-binary")
        with path.open("rb") as binary:
            header = binary.read(20)
        if (header[:6] != b"\x7fELF\x02\x01" or len(header) != 20 or
                int.from_bytes(header[18:20], "little") != 183):
            raise UpdateError("wrong-architecture")
    for kind in ("layouts", "languages", "themes"):
        if not list((package / "share/framekeyboard" / kind).glob("*.json")):
            raise UpdateError("missing-profiles")
    binary = str(package / "bin/framekeyboard")
    result = subprocess.run([binary, "--version"], capture_output=True, timeout=20, check=True)
    if result.stdout.strip() != f"framekeyboard {number}".encode():
        raise UpdateError("version-mismatch")
    with tempfile.TemporaryDirectory(prefix=".validation-", dir=package.parent) as config:
        subprocess.run([binary, "--check", "--data-dir", str(package / "share/framekeyboard"),
                        "--config-dir", config], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30, check=True)


def parent_identity(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]
    except (OSError, IndexError):
        return None


def replace_link(root, name, target):
    if target is None:
        (root / name).unlink(missing_ok=True)
        return
    temporary = root / ("." + name + "-update-" + str(os.getpid()))
    try:
        temporary.symlink_to(target)
        temporary.replace(root / name)
    finally:
        temporary.unlink(missing_ok=True)


def restart_arguments(arguments, package):
    result = []
    skip = False
    for arg in arguments:
        if skip:
            skip = False
        elif arg == "--data-dir":
            skip = True
        else:
            result.append(arg)
    return ["--data-dir", str(package / "share/framekeyboard"), *result]


def launch(package, arguments, timeout=45):
    parent, child = socket.socketpair()
    process = None
    try:
        env = dict(os.environ, FRAMEKEYBOARD_UPDATE_READY_FD=str(child.fileno()))
        process = subprocess.Popen([str(package / "bin/framekeyboard"),
                                    *restart_arguments(arguments, package)], env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, pass_fds=(child.fileno(),),
                                   start_new_session=True)
        child.close()
        readable, _, _ = select.select([parent], [], [], timeout)
        if readable and parent.recv(1) == b"1" and process.poll() is None:
            return process
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        raise UpdateError("startup-failed")
    except BaseException:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        raise
    finally:
        parent.close()
        child.close()


@contextlib.contextmanager
def activation_signals():
    # Cancellation is immediate while downloading/preparing. Once the original
    # app exits, finish activation or rollback before honoring a stop request.
    signals = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
    previous = {number: signal.getsignal(number) for number in signals}
    pending = None

    def defer(number, frame):
        nonlocal pending
        if pending is None:
            pending = number

    try:
        for number in signals:
            if previous[number] != signal.SIG_IGN:
                signal.signal(number, defer)
        yield
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)
        if pending is not None:
            # Replayed outside the transaction's rollback handler. Caught signal
            # dispositions reset on exec, so child startup/termination is normal.
            signal.raise_signal(pending)


class SettingsSnapshot:
    def __init__(self, root, name, arguments):
        base = os.environ.get("XDG_CONFIG_HOME", "")
        self.config = Path(base) / "framekeyboard" if base and Path(base).is_absolute() else Path.home() / ".config/framekeyboard"
        for i, arg in enumerate(arguments[:-1]):
            if arg == "--config-dir":
                self.config = Path(arguments[i + 1]).resolve()
        backup_root = root / "update-backups"
        backup_root.mkdir(mode=0o700, exist_ok=True)
        self.backup = backup_root / name
        self.backup.mkdir(mode=0o700)
        self.saved = {}
        for filename in ("config.json", "placement.json"):
            path = self.config / filename
            self.saved[filename] = path.exists()
            if self.saved[filename]:
                shutil.copy2(path, self.backup / filename)

    def restore(self):
        errors = []
        for filename, existed in self.saved.items():
            path = self.config / filename
            try:
                if existed:
                    with tempfile.NamedTemporaryFile(dir=self.config, delete=False) as temporary:
                        name = Path(temporary.name)
                    try:
                        shutil.copy2(self.backup / filename, name)
                        name.replace(path)
                    finally:
                        name.unlink(missing_ok=True)
                else:
                    path.unlink(missing_ok=True)
            except OSError as error:
                errors.append(error)
        if errors:
            raise errors[0]


class Installation:
    def __init__(self, executable):
        self.executable = Path(executable).resolve(strict=True)
        self.old = self.executable.parent.parent
        self.root = self.old.parent.parent
        if (self.executable.name != "framekeyboard" or self.old.parent.name != "releases" or
                not (self.root / "current").is_symlink() or
                (self.root / "current").resolve() != self.old):
            raise UpdateError("not-installed-release")
        self.old_link = os.readlink(self.root / "current")
        self.previous = None
        self.lock = None
        self.temporary = None
        self.package = None
        self.target = None

    def close(self):
        temporary, self.temporary = self.temporary, None
        lock, self.lock = self.lock, None
        try:
            if temporary:
                temporary.cleanup()
        finally:
            if lock:
                lock.close()

    def prepare(self, selected, progress):
        if platform.machine() != "aarch64":
            raise UpdateError("wrong-platform")
        self.lock = (self.root / "install.lock").open("a")
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if os.readlink(self.root / "current") != self.old_link:
            raise UpdateError("installation-changed")
        self.previous = os.readlink(self.root / "previous") if (self.root / "previous").is_symlink() else None
        self.temporary = tempfile.TemporaryDirectory(prefix=".update-", dir=self.root / "releases")
        work = Path(self.temporary.name)
        archive_asset = selected["assets"][selected["archive"]]
        checksum_asset = selected["assets"]["SHA256SUMS"]
        sums = read_url(checksum_asset["browser_download_url"], 65536)
        expected = expected_checksum(sums, selected["archive"])
        api_digest = archive_asset.get("digest")
        if api_digest and api_digest != "sha256:" + expected:
            raise UpdateError("inconsistent-checksum")
        archive = work / "update.tar.gz"
        with archive.open("wb") as output:
            actual = download(archive_asset["browser_download_url"], output, MAX_ARCHIVE,
                              archive_asset["size"])
        progress("verify")
        if actual != expected:
            raise UpdateError("checksum-mismatch")
        self.package = extract(archive, work, selected["number"])
        progress("prepare")
        validate(self.package, selected["number"])
        # Unique names retain any earlier installation of the same release for rollback.
        self.target = self.root / "releases" / (selected["number"] + "-update-" + work.name[8:])
        if self.target.exists():
            raise UpdateError("release-exists")

    def activate(self, arguments):
        with activation_signals():
            try:
                return self.activate_transaction(arguments)
            finally:
                # Cleanup and unlock before replaying a default-action signal too.
                self.close()

    def activate_transaction(self, arguments):
        switched = False
        promoted = False
        snapshot = None
        try:
            if os.readlink(self.root / "current") != self.old_link:
                raise UpdateError("installation-changed")
            snapshot = SettingsSnapshot(self.root, self.target.name, arguments)
            self.package.rename(self.target)
            promoted = True
            replace_link(self.root, "previous", self.old_link)
            switched = True
            replace_link(self.root, "current", str(self.target.relative_to(self.root)))
            process = launch(self.target, arguments)
        except BaseException:
            self.rollback(arguments, snapshot, promoted, switched)
            raise
        # Acknowledged startup commits the new release. Optional bookkeeping can
        # no longer enter rollback or remove resources from this live process.
        self.write_status("Update installed and startup acknowledged.\n")
        return process

    def write_status(self, text):
        with contextlib.suppress(OSError):
            (self.root / "update-status.log").write_text(text)

    def rollback(self, arguments, snapshot, promoted, switched):
        errors = []

        def attempt(operation):
            try:
                operation()
            except OSError as error:
                errors.append(error)

        try:
            # Free the completed download first, without deleting the release that
            # current might still name. Link repair may itself need disk space.
            if self.temporary:
                attempt(lambda: (Path(self.temporary.name) / "update.tar.gz").unlink(missing_ok=True))
            if switched:
                attempt(lambda: replace_link(self.root, "current", self.old_link))
                attempt(lambda: replace_link(self.root, "previous", self.previous))
            if snapshot:
                attempt(snapshot.restore)
            if promoted:
                try:
                    old_current = (self.root / "current").resolve(strict=True) == self.old
                except OSError:
                    old_current = False
                # A persistent write failure must never leave current dangling.
                # Retain the new target until the old link is confirmed restored.
                if old_current:
                    attempt(lambda: shutil.rmtree(self.target))
        finally:
            # Repair failures are independent. Always attempt the known old binary
            # directly, even when current or a saved preference cannot be restored.
            try:
                launch(self.old, arguments)
            except BaseException:
                self.write_status("Update failed; previous release could not restart.\n")
                raise
            status = "Update failed; previous release relaunched"
            self.write_status(status + ("; recovery writes incomplete.\n" if errors else ".\n"))


def serve(peer, executable, current, arguments):
    parent = os.getppid()
    identity = parent_identity(parent)
    installation = None
    prepared_at = None

    def send(state, **values):
        peer.sendall(json.dumps(dict(state=state, **values)).encode())

    try:
        peer.settimeout(1)
        while True:
            try:
                packet, _, flags, _ = peer.recvmsg(2048)
            except socket.timeout:
                if prepared_at and time.monotonic() - prepared_at > 120:
                    return
                continue
            if not packet:
                return
            if flags & socket.MSG_TRUNC:
                raise UpdateError("oversized-command")
            command = json.loads(packet)
            try:
                if command["op"] == "check":
                    selected = release()
                    if selected["parts"] > version(current):
                        send("available", version=selected["tag"])
                    else:
                        send("latest")
                elif command["op"] == "install" and installation is None:
                    tag = command["version"]
                    version(tag)
                    selected = release(tag)
                    if selected["tag"] != tag or selected["parts"] <= version(current):
                        raise UpdateError("not-newer-release")
                    installation = Installation(executable)
                    installation.prepare(selected, lambda phase: send("progress", phase=phase))
                    prepared_at = time.monotonic()
                    send("prepared")
                elif command["op"] == "activate" and installation and prepared_at:
                    # Explicit handoff is sent only after VR/input/settings cleanup. Parent
                    # death by itself is cancellation, never authorization to install.
                    deadline = time.monotonic() + 15
                    while parent_identity(parent) == identity and time.monotonic() < deadline:
                        time.sleep(.05)
                    if parent_identity(parent) == identity:
                        return
                    peer.close()
                    installation.activate(arguments)
                    return
                else:
                    raise UpdateError("invalid-command")
            except (OSError, ValueError, TypeError, KeyError, UpdateError, subprocess.SubprocessError, tarfile.TarError):
                if installation:
                    installation.close()
                    installation = None
                    prepared_at = None
                send("error")
    finally:
        if installation:
            installation.close()
        peer.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", type=int, required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    version(args.version)
    peer = socket.socket(fileno=args.fd)
    if peer.family != socket.AF_UNIX or peer.type != socket.SOCK_SEQPACKET:
        raise UpdateError("not-private-channel")
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    # SIGTERM cancels downloads and cleans staging on an ordinary app shutdown.
    def interrupted(signum, frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        serve(peer, args.executable, args.version, arguments)
    except (OSError, ValueError, UpdateError):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
