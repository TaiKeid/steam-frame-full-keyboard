#!/usr/bin/env python3
"""Exercise the real uninstaller against an isolated home, runtime directory and fake app."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

script = Path(sys.argv[1]).resolve() / "scripts/uninstall-local.sh"
MARKER = "# Managed by Full Keyboard\n"


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def install(home, runtime):
    root = home / ".local/share/framekeyboard"
    release = root / "releases/9.0.1-install-abc"
    (release / "bin").mkdir(parents=True)
    # A real executable copied into the release, so /proc/<pid>/exe names it.
    shutil.copy2(shutil.which("sleep"), release / "bin/framekeyboard")
    (root / "current").symlink_to("releases/9.0.1-install-abc")
    (home / ".local/bin").mkdir(parents=True)
    (home / ".local/bin/framekeyboard").write_text(
        '#!/usr/bin/env bash\ninstall_root="$HOME/.local/share/framekeyboard/current"\n')
    (home / ".local/share/applications").mkdir(parents=True)
    (home / ".local/share/applications/framekeyboard.desktop").write_text(
        "[Desktop Entry]\nName=Full Keyboard\n")
    units = home / ".config/systemd/user"
    (units / "steamvr.service.wants").mkdir(parents=True)
    (units / "gamescope-session.target.wants").mkdir()
    (units / "framekeyboard-autostart.service").write_text(MARKER + "[Unit]\n")
    for wants in ("steamvr.service.wants", "gamescope-session.target.wants"):
        (units / wants / "framekeyboard-autostart.service").symlink_to("../framekeyboard-autostart.service")
    (home / ".config/framekeyboard").mkdir()
    (home / ".config/framekeyboard/config.json").write_text("{}")
    runtime.mkdir(mode=0o700)
    (runtime / "vr.lock").touch()
    return root, release


def run(home, runtime, *arguments, stdin=subprocess.DEVNULL):
    environment = dict(os.environ, FRAMEKEYBOARD_INSTALL_HOME=str(home),
                       FRAMEKEYBOARD_INSTALL_RUNTIME_DIR=str(runtime), FRAMEKEYBOARD_UNINSTALL_SYSTEMCTL="0")
    return subprocess.run(["bash", str(script), *arguments], env=environment, stdin=stdin,
                          capture_output=True, text=True, timeout=60)


with tempfile.TemporaryDirectory(prefix="full-keyboard-uninstall-") as directory:
    home, runtime = Path(directory) / "home", Path(directory) / "runtime"
    root, release = install(home, runtime)
    units = home / ".config/systemd/user"

    # Without a terminal, nothing happens unless --yes is given.
    result = run(home, runtime)
    require(result.returncode == 1 and "--yes" in result.stderr, "a non-interactive run must require --yes")
    require(root.exists() and (units / "framekeyboard-autostart.service").exists(),
            "a declined run removes nothing")

    # A running copy holds the instance lock, like the app, and is stopped first.
    app = subprocess.Popen(["flock", str(runtime / "vr.lock"), str(release / "bin/framekeyboard"), "60"])
    time.sleep(0.3)
    result = run(home, runtime, "--yes")
    require(result.returncode == 0, "uninstall failed: " + result.stderr)
    require(app.wait(timeout=5) is not None, "the running app must be stopped")
    for path in (root, home / ".local/bin/framekeyboard", home / ".local/share/applications/framekeyboard.desktop",
                 units / "framekeyboard-autostart.service",
                 units / "steamvr.service.wants/framekeyboard-autostart.service",
                 units / "gamescope-session.target.wants/framekeyboard-autostart.service", runtime):
        require(not path.exists() and not path.is_symlink(), f"{path} must be removed")
    require((home / ".config/framekeyboard/config.json").exists(), "settings stay without --purge")
    require("--purge" in result.stdout, "the user is told how to delete settings")

    # Running again finds nothing; --purge then removes the settings.
    result = run(home, runtime, "--yes")
    require(result.returncode == 0 and "Nothing to remove" in result.stdout, "a second run is harmless")
    result = run(home, runtime, "--purge", "--yes")
    require(result.returncode == 0 and not (home / ".config/framekeyboard").exists(),
            "--purge deletes the settings")

with tempfile.TemporaryDirectory(prefix="full-keyboard-uninstall-") as directory:
    home, runtime = Path(directory) / "home", Path(directory) / "runtime"
    install(home, runtime)
    units = home / ".config/systemd/user"
    # Same-named files of another program are not ours and stay.
    (home / ".local/bin/framekeyboard").write_text("#!/bin/sh\necho another program\n")
    (home / ".local/share/applications/framekeyboard.desktop").write_text("[Desktop Entry]\nName=Other\n")
    (units / "framekeyboard-autostart.service").write_text("[Unit]\nDescription=Not ours\n")
    result = run(home, runtime, "--yes")
    require(result.returncode == 0, "uninstall failed: " + result.stderr)
    for path in (home / ".local/bin/framekeyboard", home / ".local/share/applications/framekeyboard.desktop",
                 units / "framekeyboard-autostart.service"):
        require(path.exists(), f"foreign {path} must stay")
    require(not (home / ".local/share/framekeyboard").exists(), "the installation itself is removed")

with tempfile.TemporaryDirectory(prefix="full-keyboard-uninstall-") as directory:
    home, runtime = Path(directory) / "home", Path(directory) / "runtime"
    root, _ = install(home, runtime)
    # A symlinked installation is left for manual removal, and nothing else is touched.
    moved = Path(directory) / "elsewhere"
    root.rename(moved)
    root.symlink_to(moved)
    result = run(home, runtime, "--yes")
    require(result.returncode == 1 and "symbolic link" in result.stderr, "a symlinked install must be refused")
    require(moved.exists() and (home / ".local/bin/framekeyboard").exists(), "a refused run removes nothing")

with tempfile.TemporaryDirectory(prefix="full-keyboard-uninstall-") as directory:
    # The app keeps downloaded speech models under XDG_DATA_HOME when it is set.
    # Run the real (non-test) path, with HOME and every XDG directory inside
    # this temporary tree, so nothing outside it can be touched.
    home, runtime = Path(directory) / "home", Path(directory) / "runtime"
    install(home, runtime)
    data = Path(directory) / "data"
    models = data / "framekeyboard/models"
    models.mkdir(parents=True)
    (models / "ggml-small.bin").write_bytes(b"model")
    (data / "other-app").mkdir()
    environment = {key: value for key, value in os.environ.items() if not key.startswith(("XDG_", "FRAMEKEYBOARD_"))}
    environment.update(HOME=str(home), XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(home / ".config"),
                       FRAMEKEYBOARD_INSTALL_RUNTIME_DIR=str(runtime), FRAMEKEYBOARD_UNINSTALL_SYSTEMCTL="0")
    result = subprocess.run(["bash", str(script), "--yes"], env=environment, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=60)
    require(result.returncode == 0, "uninstall failed: " + result.stderr)
    require(str(models) in result.stdout, "the models are listed with what is removed")
    require(not (data / "framekeyboard").exists(), "models under XDG_DATA_HOME are removed too")
    require((data / "other-app").exists(), "other programs' data stays")
    require(not (home / ".local/share/framekeyboard").exists(), "the installation is removed")

with tempfile.TemporaryDirectory(prefix="full-keyboard-uninstall-") as directory:
    # Run from SteamOS's Desktop, systemctl --user can't reach the manager
    # until it is pointed at the main session bus. This stand-in fails while
    # XDG_RUNTIME_DIR is set, as there, and records every call.
    home, runtime = Path(directory) / "home", Path(directory) / "runtime"
    install(home, runtime)
    commands, calls = Path(directory) / "commands", Path(directory) / "manager-calls"
    commands.mkdir()
    (commands / "systemctl").write_text(
        '#!/bin/sh\necho "${XDG_RUNTIME_DIR-unset} ${DBUS_SESSION_BUS_ADDRESS-unset} $*" >> "%s"\n'
        '[ -z "${XDG_RUNTIME_DIR+set}" ]\n' % calls)
    (commands / "systemctl").chmod(0o700)
    environment = dict(os.environ, FRAMEKEYBOARD_INSTALL_HOME=str(home),
                       FRAMEKEYBOARD_INSTALL_RUNTIME_DIR=str(runtime),
                       PATH=str(commands) + os.pathsep + os.environ.get("PATH", ""),
                       XDG_RUNTIME_DIR=str(Path(directory) / "desktop"),
                       DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(Path(directory) / "desktop-bus"))
    environment.pop("FRAMEKEYBOARD_UNINSTALL_SYSTEMCTL", None)
    # Only run the uninstaller once the stand-in is what bash finds and runs,
    # so the real manager is never asked to stop anything.
    subprocess.run(["bash", "-c", "systemctl probe"], env=environment, capture_output=True, text=True,
                   timeout=10)
    require(calls.exists() and calls.read_text().endswith(" probe\n"), "the systemctl stand-in did not run")
    calls.unlink()
    result = subprocess.run(["bash", str(script), "--yes"], env=environment, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=60)
    require(result.returncode == 0, "uninstall from the Desktop failed: " + result.stderr)
    bus = "unset unix:path=/run/user/%d/bus --user " % os.getuid()
    made = calls.read_text().splitlines()
    for action in ("stop framekeyboard-autostart.service", "daemon-reload",
                   "reset-failed framekeyboard-autostart.service"):
        require(bus + action in made, "the manager was not reached through the main bus for " + action)
    require(not (home / ".config/systemd/user/framekeyboard-autostart.service").exists(),
            "the startup unit is removed")

print("Uninstaller: confirmation, stopping a running app, removal, foreign files, purge, refusal, "
      "models under XDG_DATA_HOME and the Desktop's service manager passed.")
