"""Offline updater tests in disposable installs; never download or touch a real keyboard."""
import copy
import errno
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("update_helper", Path(sys.argv.pop(1)))
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)


def metadata(tag="9.0.0"):
    name = f"framekeyboard-{tag}-aarch64.tar.gz"
    return dict(tag_name=tag, draft=False, prerelease=False, assets=[
        dict(name=n, size=100, browser_download_url=f"https://github.com/{u.REPOSITORY}/releases/download/{tag}/{n}")
        for n in (name, "SHA256SUMS")])


def archive_bytes(members):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for item, data in members:
            archive.addfile(item, io.BytesIO(data))
    return output.getvalue()


def member(name, contents=b"hello", mode=0o644):
    info = tarfile.TarInfo(name)
    info.mode, info.size = mode, len(contents)
    return info, contents


class Metadata(unittest.TestCase):
    def test_numeric_versions_and_rejected_tags(self):
        self.assertGreater(u.version("0.10.0"), u.version("0.9.9"))
        self.assertEqual(u.version("v1.2.3"), u.version("1.2.3"))
        for invalid in (None, {}, "01.2.3", "1.2", "1.2.3-rc1", "1.2.3/evil", "1.2.3\n"):
            with self.subTest(invalid=invalid), self.assertRaises(u.UpdateError):
                u.version(invalid)

    def test_release_assets_and_official_urls(self):
        data = metadata()
        with patch.object(u, "read_url", return_value=json.dumps(data).encode()):
            self.assertEqual(u.release()["number"], "9.0.0")
        for change in ("draft", "prerelease", "missing", "url", "duplicate", "size"):
            bad = copy.deepcopy(data)
            if change in ("draft", "prerelease"):
                bad[change] = True
            elif change == "missing":
                bad["assets"].pop()
            elif change == "url":
                bad["assets"][0]["browser_download_url"] = "https://example.com/update.tar.gz"
            elif change == "duplicate":
                bad["assets"].append(bad["assets"][0])
            else:
                bad["assets"][0]["size"] = u.MAX_ARCHIVE + 1
            with self.subTest(change=change), patch.object(u, "read_url", return_value=json.dumps(bad).encode()), self.assertRaises(u.UpdateError):
                u.release()

    def test_exact_unambiguous_checksum(self):
        line = "a" * 64 + "  package.tar.gz\n"
        self.assertEqual(u.expected_checksum(line.encode(), "package.tar.gz"), "a" * 64)
        for bad in (line.replace("package.tar.gz", "other.tar.gz"), line + line, "bad  package.tar.gz"):
            with self.assertRaises(u.UpdateError):
                u.expected_checksum(bad.encode(), "package.tar.gz")

    def test_redirects_reject_non_https_and_unrelated_hosts(self):
        handler = u.ReleaseRedirects()
        for url in ("http://github.com/file", "https://example.com/file", "https://github.com.evil.org/file", "https://user@github.com/file"):
            with self.subTest(url=url), self.assertRaises(u.UpdateError):
                handler.redirect_request(None, None, 302, "", {}, url)


class Archives(unittest.TestCase):
    def test_traversal_links_devices_duplicates_and_limits(self):
        prefix = "framekeyboard-9.0.0-aarch64/"
        link, _ = member(prefix + "link")
        link.type, link.linkname, link.size = tarfile.SYMTYPE, "/tmp", 0
        hard = copy.copy(link)
        hard.type = tarfile.LNKTYPE
        device = copy.copy(link)
        device.type = tarfile.CHRTYPE
        oversized, _ = member(prefix + "huge")
        oversized.size = u.MAX_UNPACKED + 1
        cases = [
            [member("../escape")], [member("/tmp/escape")], [member(prefix + "../escape")],
            [member(prefix + "a//b")], [(link, b"")], [(hard, b"")], [(device, b"")],
            [member(prefix + "dup"), member(prefix + "dup")], [member(prefix + "setuid", mode=0o4755)],
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, members in enumerate(cases):
                archive = root / f"{index}.tar.gz"
                archive.write_bytes(archive_bytes(members))
                with self.subTest(index=index), self.assertRaises(u.UpdateError):
                    u.extract(archive, root / "output", "9.0.0")
            archive = root / "valid.tar.gz"
            archive.write_bytes(archive_bytes([member(prefix + "file")]))
            package = u.extract(archive, root / "valid", "9.0.0")
            self.assertEqual((package / "file").read_bytes(), b"hello")
            with patch.object(u, "MAX_UNPACKED", 1), self.assertRaises(u.UpdateError):
                u.extract(archive, root / "limited", "9.0.0")

    def test_elf_architecture_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            for name in ("framekeyboard", "framekeyboard-native-guard"):
                path = root / "bin" / name
                path.write_bytes(b"\x7fELF\x02\x01" + b"\0" * 12 + (62).to_bytes(2, "little"))
                path.chmod(0o755)
            with self.assertRaises(u.UpdateError):
                u.validate(root, "9.0.0")


class Transaction(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name) / "install"
        self.old = self.root / "releases/old"
        (self.old / "bin").mkdir(parents=True)
        (self.old / "bin/framekeyboard").write_text("old binary")
        (self.root / "current").symlink_to("releases/old")
        (self.root / "previous").symlink_to("releases/older")
        self.config = self.root / "user-config"
        self.config.write_text("preserve preferences")
        self.installation = u.Installation(self.old / "bin/framekeyboard")
        name = "framekeyboard-9.0.0-aarch64"
        self.archive = archive_bytes([member(name + "/bin/framekeyboard", b"new binary", 0o755)])
        self.sha = hashlib.sha256(self.archive).hexdigest()
        data = metadata()
        data["assets"][0]["size"] = len(self.archive)
        with patch.object(u, "read_url", return_value=json.dumps(data).encode()):
            self.selected = u.release()
        self.frames = []

    def tearDown(self):
        self.installation.close()
        self.assertEqual(self.config.read_text(), "preserve preferences")
        self.directory.cleanup()

    def prepare(self, checksum=None, validation_error=None):
        def download(url, output, limit, expected_size=None):
            output.write(self.archive)
            return self.sha
        sums = f"{checksum or self.sha}  {self.selected['archive']}\n".encode()
        with patch.object(u.platform, "machine", return_value="aarch64"), patch.object(u, "read_url", return_value=sums), patch.object(u, "download", side_effect=download), patch.object(u, "validate", side_effect=validation_error):
            self.installation.prepare(self.selected, self.frames.append)

    def test_prepare_only_and_cleanup_on_cancel(self):
        self.prepare()
        self.assertEqual(os.readlink(self.root / "current"), "releases/old")
        self.assertEqual(self.frames, ["verify", "prepare"])
        self.assertTrue(self.installation.package.is_dir())
        self.installation.close()
        self.assertFalse(list((self.root / "releases").glob(".update-*")))

    def test_bad_checksum_or_validation_does_not_switch(self):
        for checksum, error in (("0" * 64, None), (None, u.UpdateError("profiles"))):
            with self.assertRaises(u.UpdateError):
                self.prepare(checksum, error)
            self.installation.close()
            self.assertEqual(os.readlink(self.root / "current"), "releases/old")
            self.assertFalse(list((self.root / "releases").glob(".update-*")))

    def test_lock_and_stale_installation(self):
        lock = (self.root / "install.lock").open("a")
        try:
            u.fcntl.flock(lock, u.fcntl.LOCK_EX | u.fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.prepare()
        finally:
            lock.close()
        self.installation.close()
        u.replace_link(self.root, "current", "releases/another")
        with self.assertRaises(u.UpdateError):
            self.prepare()

    def test_activate_preserves_previous_and_restart_arguments(self):
        self.prepare()
        def launch(package, args):
            self.assertEqual((self.root / "current").resolve(), package)
            self.assertEqual(os.readlink(self.root / "previous"), "releases/old")
            self.assertEqual(args, ["--vr", "--input", "ei", "--start-enabled", "--config-dir", str(Path(self.directory.name)/"prefs")])
        with patch.object(u, "launch", side_effect=launch):
            self.installation.activate(["--vr", "--input", "ei", "--start-enabled", "--config-dir", str(Path(self.directory.name)/"prefs")])
        self.assertEqual((self.root / "current").resolve(), self.installation.target)
        self.assertTrue(self.old.exists())
        self.assertEqual(u.restart_arguments(["--data-dir", "old", "--vr", "--config-dir", "custom"], self.installation.target), ["--data-dir", str(self.installation.target / "share/framekeyboard"), "--vr", "--config-dir", "custom"])

    def test_startup_failure_restores_post_close_preferences(self):
        config = Path(self.directory.name) / "preferences"
        config.mkdir()
        (config / "config.json").write_text("saved before restart")
        self.prepare()
        def restart(package, arguments):
            if package == self.old:
                self.assertEqual((config / "config.json").read_text(), "saved before restart")
                self.assertFalse((config / "placement.json").exists())
            else:
                (config / "config.json").write_text("failed migration")
                (config / "placement.json").write_text("new placement")
                raise u.UpdateError("startup")
        with patch.object(u, "launch", side_effect=restart), self.assertRaises(u.UpdateError):
            self.installation.activate(["--config-dir", str(config)])
        backups = list((self.root / "update-backups").glob("*/config.json"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "saved before restart")

    def test_rollback_file_errors_cannot_skip_old_restart_or_leave_dangling_current(self):
        for failing_step in ("current", "previous", "preferences"):
            with self.subTest(step=failing_step):
                self.prepare()
                config = Path(self.directory.name) / "prefs"
                real_replace = u.replace_link
                def replace(root, name, target):
                    if (name == failing_step and
                            target == ("releases/old" if name == "current" else "releases/older")):
                        self.assertFalse((Path(self.installation.temporary.name) / "update.tar.gz").exists())
                        raise OSError(errno.EIO, "injected rollback failure")
                    return real_replace(root, name, target)
                def restore(snapshot):
                    if failing_step == "preferences":
                        raise OSError(errno.ENOSPC, "injected restore failure")
                with patch.object(u, "replace_link", side_effect=replace), patch.object(u.SettingsSnapshot, "restore", autospec=True, side_effect=restore) as restored, patch.object(u, "launch", side_effect=[u.UpdateError("startup"), None]) as launched:
                    with self.assertRaises(u.UpdateError):
                        self.installation.activate(["--vr", "--config-dir", str(config)])
                    self.assertEqual(launched.call_args_list[1].args[0], self.old)
                    restored.assert_called_once()
                self.assertTrue((self.root / "current").exists())
                self.assertTrue((self.old / "bin/framekeyboard").exists())
                if failing_step == "current":
                    self.assertTrue(self.installation.target.exists())
                else:
                    self.assertEqual((self.root / "current").resolve(), self.old)
                    self.assertFalse(self.installation.target.exists())
                self.assertIn("recovery writes incomplete", (self.root / "update-status.log").read_text())
                self.installation.close()
                # Each subcase starts from the original link state; all paths are private.
                real_replace(self.root, "current", "releases/old")
                real_replace(self.root, "previous", "releases/older")

    def test_snapshot_attempts_placement_after_config_restore_error(self):
        config = Path(self.directory.name) / "preferences"
        config.mkdir()
        (config / "config.json").write_text("original config")
        (config / "placement.json").write_text("original placement")
        snapshot = u.SettingsSnapshot(self.root, "snapshot-test", ["--config-dir", str(config)])
        (config / "config.json").write_text("changed config")
        (config / "placement.json").write_text("changed placement")
        create = u.tempfile.NamedTemporaryFile
        calls = 0
        def temporary(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise OSError(errno.ENOSPC, "injected config write failure")
            return create(*args, **kwargs)
        with patch.object(u.tempfile, "NamedTemporaryFile", side_effect=temporary), self.assertRaises(OSError):
            snapshot.restore()
        self.assertEqual((config / "placement.json").read_text(), "original placement")
        self.assertEqual((snapshot.backup / "config.json").read_text(), "original config")

    def test_stop_after_acknowledgement_keeps_the_live_release(self):
        for phase in ("before-return", "status-write"):
            with self.subTest(phase=phase):
                self.prepare()
                lock = self.root / (phase + "-instance.lock")
                script = ("#!/usr/bin/env python3\nimport fcntl,os,socket,time\n"
                          f"lock=open({str(lock)!r},'a')\n"
                          "fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)\n"
                          "peer=socket.socket(fileno=int(os.environ.pop('FRAMEKEYBOARD_UPDATE_READY_FD')))\n"
                          "peer.send(b'1');peer.close()\ntime.sleep(10)\n")
                for binary in (self.old / "bin/framekeyboard", self.installation.package / "bin/framekeyboard"):
                    binary.write_text(script)
                    binary.chmod(0o755)
                children = []
                original_launch = u.launch
                original_status = u.Installation.write_status
                def interrupted(number, frame):
                    raise SystemExit(0)
                def launch(package, arguments):
                    self.assertEqual(package, self.installation.target, "a committed child must not trigger old relaunch")
                    child = original_launch(package, arguments, timeout=2)
                    children.append(child)
                    if phase == "before-return":
                        signal.raise_signal(signal.SIGTERM)
                    return child
                def status(installation, text):
                    if phase == "status-write":
                        signal.raise_signal(signal.SIGTERM)
                    original_status(installation, text)
                previous = signal.signal(signal.SIGTERM, interrupted)
                try:
                    with patch.object(u, "launch", side_effect=launch), patch.object(u.Installation, "write_status", status), self.assertRaises(SystemExit):
                        self.installation.activate(["--config-dir", str(Path(self.directory.name) / "prefs")])
                    self.assertEqual(len(children), 1)
                    self.assertIsNone(children[0].poll())
                    self.assertEqual((self.root / "current").resolve(), self.installation.target)
                    self.assertTrue((self.installation.target / "bin/framekeyboard").exists())
                    self.assertIn("startup acknowledged", (self.root / "update-status.log").read_text())
                    self.assertIs(signal.getsignal(signal.SIGTERM), interrupted)
                finally:
                    signal.signal(signal.SIGTERM, previous)
                    for child in children:
                        if child.poll() is None:
                            child.terminate()
                        child.wait(timeout=3)
                    self.installation.close()
                    u.replace_link(self.root, "current", "releases/old")
                    u.replace_link(self.root, "previous", "releases/older")

    def test_stop_during_rollback_is_replayed_after_old_restart(self):
        self.prepare()
        replace = u.replace_link
        def interrupted(number, frame):
            raise SystemExit(0)
        def repair(root, name, target):
            if name == "current" and target == "releases/old":
                signal.raise_signal(signal.SIGTERM)
            return replace(root, name, target)
        previous = signal.signal(signal.SIGTERM, interrupted)
        try:
            with patch.object(u, "replace_link", side_effect=repair), patch.object(u, "launch", side_effect=[u.UpdateError("startup"), None]) as launched, self.assertRaises(SystemExit):
                self.installation.activate(["--config-dir", str(Path(self.directory.name) / "prefs")])
            self.assertEqual(launched.call_args_list[1].args[0], self.old)
            self.assertEqual((self.root / "current").resolve(), self.old)
            self.assertFalse(self.installation.target.exists())
        finally:
            signal.signal(signal.SIGTERM, previous)

    def test_startup_failure_restores_links_and_relaunches_old(self):
        self.prepare()
        with patch.object(u, "launch", side_effect=[u.UpdateError("startup"), None]) as launch:
            with self.assertRaises(u.UpdateError):
                self.installation.activate(["--vr", "--config-dir", str(Path(self.directory.name)/"prefs")])
            self.assertEqual(launch.call_args_list[1].args[0], self.old)
        self.assertEqual(os.readlink(self.root / "current"), "releases/old")
        self.assertEqual(os.readlink(self.root / "previous"), "releases/older")


class Startup(unittest.TestCase):
    def test_private_ack_and_failure_reaping(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            (package / "bin").mkdir()
            binary = package / "bin/framekeyboard"
            binary.write_text('#!/usr/bin/env python3\nimport os,socket,time\ns=socket.socket(fileno=int(os.environ.pop("FRAMEKEYBOARD_UPDATE_READY_FD")))\ns.send(b"1")\ns.close()\ntime.sleep(10)\n')
            binary.chmod(0o755)
            child = u.launch(package, ["--preview"], timeout=2)
            try:
                self.assertIsNone(child.poll())
            finally:
                child.terminate()
                child.wait(timeout=3)
            binary.write_text('#!/usr/bin/env python3\nraise SystemExit(1)\n')
            with self.assertRaises(u.UpdateError):
                u.launch(package, [], timeout=2)
            binary.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(10)\n')
            with self.assertRaises(u.UpdateError):
                u.launch(package, [], timeout=.1)


    def test_activation_deferral_does_not_block_child_termination(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            (package / "bin").mkdir()
            binary = package / "bin/framekeyboard"
            binary.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(10)\n')
            binary.chmod(0o755)
            started = time.monotonic()
            with u.activation_signals(), self.assertRaises(u.UpdateError):
                u.launch(package, [], timeout=.1)
            self.assertLess(time.monotonic() - started, 2, "child must not inherit a blocked SIGTERM")



class Protocol(unittest.TestCase):
    def test_closing_channel_cancels_prepared_install_without_activation(self):
        client, server = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        selected = {"tag": "9.0.0", "parts": (9, 0, 0)}
        class FakeInstallation:
            closed = False
            activated = False
            def __init__(self, executable):
                pass
            def prepare(self, release, progress):
                pass
            def close(self):
                FakeInstallation.closed = True
            def activate(self, arguments):
                FakeInstallation.activated = True
        errors = []
        def run():
            try:
                u.serve(server, "/unused", "0.6.0", ["--vr"])
            except BaseException as error:
                errors.append(error)
        with patch.object(u, "release", return_value=selected), patch.object(u, "Installation", FakeInstallation):
            thread = threading.Thread(target=run)
            thread.start()
            client.settimeout(2)
            client.send(b'{"op":"check"}')
            self.assertEqual(json.loads(client.recv(2048))["state"], "available")
            client.send(b'{"op":"install","version":"9.0.0"}')
            self.assertEqual(json.loads(client.recv(2048))["state"], "prepared")
            client.close()
            thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertTrue(FakeInstallation.closed)
        self.assertFalse(FakeInstallation.activated)


if __name__ == "__main__":
    unittest.main()
