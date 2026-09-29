#!/usr/bin/env python3
"""Exercise the real installer with a host binary and an isolated install root."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
binary = Path(sys.argv[2]).resolve()

with tempfile.TemporaryDirectory(prefix="full-keyboard-installer-") as directory:
    root = Path(directory)
    package = root / "package"
    (package / "bin").mkdir(parents=True)
    shutil.copy2(source / "scripts/install-local.sh", package / "install.sh")
    for kind in ("layouts", "languages", "themes"):
        shutil.copytree(source / kind, package / "share/framekeyboard" / kind)
    # Keep --check real. Only version and target architecture are test fixtures.
    wrapper = package / "bin/framekeyboard"
    wrapper.write_text('''#!/bin/sh
if [ "$1" = --version ]; then
    echo "framekeyboard $(cat "$(dirname "$0")/version")"
else
    exec "$TEST_BINARY" "$@"
fi
''')
    wrapper.chmod(0o755)
    version = package / "bin/version"
    version.write_text("9.0.1")
    commands = root / "commands"
    commands.mkdir()
    (commands / "uname").write_text('#!/bin/sh\necho aarch64\n')
    real_mv = shutil.which("mv")
    real_cp = shutil.which("cp")
    (commands / "cp").write_text('''#!/bin/sh
"$REAL_CP" "$@" || exit $?
[ "${FAIL_COPY:-}" != 1 ]
''')
    (commands / "mv").write_text('''#!/bin/sh
for last do :; done
if [ "${FAIL_ACTIVATION:-}" = 1 ] && [ "$last" = "$FRAMEKEYBOARD_INSTALL_HOME/.local/share/framekeyboard/current" ]; then
    exit 1
fi
exec "$REAL_MV" "$@"
''')
    for path in commands.iterdir():
        path.chmod(0o755)
    install_home = root / "install"
    config = install_home / ".config/framekeyboard/config.json"
    config.parent.mkdir(parents=True)
    config.write_text('user configuration must remain untouched')
    env = dict(
        os.environ,
        FRAMEKEYBOARD_INSTALL_HOME=str(install_home),
        TEST_BINARY=str(binary),
        REAL_MV=real_mv,
        REAL_CP=real_cp,
        PATH=f"{commands}:{os.environ['PATH']}",
    )
    releases = install_home / ".local/share/framekeyboard/releases"
    current = releases.parent / "current"

    def install(success, **extra):
        result = subprocess.run(
            ["bash", str(package / "install.sh")],
            env=dict(env, **extra), capture_output=True, text=True,
        )
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        assert config.read_text() == 'user configuration must remain untouched'
        assert not list(releases.glob(".install-*")), "failed install left staging directory"
        assert not list(releases.parent.glob(".links.*")), "temporary links leaked"

    theme = package / "share/framekeyboard/themes/graphite.json"
    good_theme = theme.read_bytes()
    theme.write_text('{broken')
    install(False)
    assert not (releases / "9.0.1").exists() and not current.exists()
    theme.write_bytes(good_theme)
    install(True)
    assert os.readlink(current) == "releases/9.0.1"
    install(False)  # A completed release must still be protected from overwrite.
    version.write_text("9.0.2")
    theme.write_text('{broken')
    install(False)
    assert os.readlink(current) == "releases/9.0.1" and not (releases / "9.0.2").exists()
    theme.write_bytes(good_theme)
    install(False, FAIL_COPY="1")
    assert os.readlink(current) == "releases/9.0.1" and not (releases / "9.0.2").exists()
    install(False, FAIL_ACTIVATION="1")
    assert os.readlink(current) == "releases/9.0.1" and not (releases / "9.0.2").exists()
    install(True)
    assert os.readlink(current) == "releases/9.0.2"
    assert os.readlink(releases.parent / "previous") == "releases/9.0.1"
    assert "Name=Full Keyboard" in (install_home / ".local/share/applications/framekeyboard.desktop").read_text()
    print("Copied-profile validation, failure cleanup, retry, update and settings preservation passed.")
