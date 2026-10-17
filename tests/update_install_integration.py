"""Actual ARM64 package validation, app handoff, startup and rollback in temporary roots."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

if platform.machine() != "aarch64":
    raise SystemExit(77)
archive, fixture = [Path(arg).resolve() for arg in sys.argv[1:]]
match = re.fullmatch(r"framekeyboard-([0-9]+\.[0-9]+\.[0-9]+)-aarch64\.tar\.gz", archive.name)
assert match, archive.name
number = match[1]
with tempfile.TemporaryDirectory(prefix="full-keyboard-update-integration-") as directory:
    base = Path(directory)
    with tarfile.open(archive) as package:
        package.extractall(base, filter="data")
    package = base / f"framekeyboard-{number}-aarch64"
    helper = package / "share/framekeyboard/update/update_helper.py"
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    name = archive.name
    selected = dict(tag=number, number=number, parts=list(map(int, number.split("."))), archive=name,
                    assets={name:dict(browser_download_url="https://fixture/archive", size=archive.stat().st_size,
                                     digest="sha256:"+digest),
                            "SHA256SUMS":dict(browser_download_url="https://fixture/sums")})
    shim = base / "offline-helper.py"
    shim.write_text(f'''import hashlib,importlib.util,json,shutil
spec=importlib.util.spec_from_file_location("updates",{str(helper)!r})
u=importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
selected=json.loads({json.dumps(selected)!r})
selected["parts"]=tuple(selected["parts"])
u.release=lambda tag=None: selected
u.read_url=lambda url,limit: {(digest+'  '+name+chr(10)).encode()!r}
def download(url,output,limit,expected_size=None):
    with open({str(archive)!r},"rb") as source: shutil.copyfileobj(source,output)
    return {digest!r}
u.download=download
raise SystemExit(u.main())
''')
    for mode in ("success", "rollback"):
        install = base / mode
        root = install / ".local/share/framekeyboard"
        old = root / "releases/old"
        shutil.copytree(package, old)
        shutil.copy2(fixture, old / "bin/framekeyboard")
        (root / "current").symlink_to("releases/old")
        (root / "previous").symlink_to("releases/older")
        config = install / ".config/framekeyboard"
        config.mkdir(parents=True)
        runtime = install / "runtime"
        runtime.mkdir(mode=0o700)
        sentinel = config / "custom-profile.txt"
        sentinel.write_text("preserve this file")
        launcher = install / ".local/bin/framekeyboard"
        launcher.parent.mkdir(parents=True)
        launcher.write_text("preserve custom launcher")
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                   SDL_VIDEODRIVER="dummy" if mode == "success" else "unavailable-test-driver")
        parent = subprocess.Popen([str(old / "bin/framekeyboard"), "--handoff-fixture", str(shim), str(config)],
                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = parent.communicate(timeout=40)[0].decode()
        assert parent.returncode == 0, output
        result = root / "update-status.log"
        deadline = time.monotonic() + 15
        while not result.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert result.exists(), output + " no update result"
        if mode == "success":
            assert "installed" in result.read_text()
            assert (root / "current").resolve() != old
            assert os.readlink(root / "previous") == "releases/old"
        else:
            assert "failed" in result.read_text()
            assert os.readlink(root / "current") == "releases/old"
            assert os.readlink(root / "previous") == "releases/older"
            assert (config / "rollback-started").exists()
        assert sentinel.read_text() == "preserve this file"
        assert launcher.read_text() == "preserve custom launcher"
        assert not list((root / "releases").glob(".update-*"))
        # Both children use finite previews; wait before deleting their disposable root.
        time.sleep(4)
        print(f"ARM64 {mode}: real package validation, Settings click, cleanup, handoff and startup passed.")
