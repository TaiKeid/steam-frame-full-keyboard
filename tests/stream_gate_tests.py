#!/usr/bin/env python3
"""Run the gate (stream/stream_gate.c) against a private runtime folder.

Arguments: the gate, the pass-through library, and the folder holding the stand-in for Steam's
libopenvr_api.so (tests/stream_fake_openvr.c).
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

gate, library, steam = (Path(argument).resolve() for argument in sys.argv[1:4])


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def run(runtime, command, library=library, steam=steam):
    return subprocess.run([str(gate), command, "--runtime", str(runtime), "--library", str(library),
                           "--steam", str(steam)], timeout=30).returncode


with tempfile.TemporaryDirectory(prefix="fk-gate-") as directory:
    base = Path(directory)
    runtime = base / "runtime"
    answer, link, copy, off, failed = (runtime / name for name in
                                       ("steam.env", "openvr.so", "framekeyboard-stream.so", "off",
                                        "failed"))

    # Before Steam starts: a checked copy, the link to it and the variable that names the link.
    require(run(runtime, "start") == 0, "start succeeds with a library that loads")
    require(answer.read_text() == f"SDL_OPENVR_LIBRARY={link}\n", "the answer names the link")
    require(os.readlink(link) == copy.name and copy.read_bytes() == library.read_bytes(),
            "the link leads to a copy of the library")
    require(not off.exists() and runtime.stat().st_mode & 0o777 == 0o700, "private folder, no idle mark")
    # The copy's dependency resolves beside it, to Steam's library, with or without LD_LIBRARY_PATH.
    require(os.readlink(runtime / "libopenvr_api.so") == str(steam / "libopenvr_api.so"),
            "a link to Steam's library sits beside the copy")
    plain = {name: value for name, value in os.environ.items() if name != "LD_LIBRARY_PATH"}
    require(subprocess.run([str(gate), "check", str(copy), str(steam / "libopenvr_api.so")], env=plain,
                           timeout=30).returncode == 0,
            "the copy binds Steam's library through its own folder")
    other = base / "other-openvr.so"
    shutil.copy(steam / "libopenvr_api.so", other)
    require(subprocess.run([str(gate), "check", str(copy), str(other)], env=plain, timeout=30).returncode != 0,
            "a copy that binds another library fails the check")

    # Turned off: Steam's own library behind the link, a loaded library told to idle, no answer.
    require(run(runtime, "stock") == 0, "stock succeeds")
    require(os.readlink(link) == str(steam / "libopenvr_api.so") and off.exists() and not answer.exists(),
            "stock points at Steam's library")

    # Turned on again while Steam already carries the variable: no answer is written.
    require(run(runtime, "prepare") == 0, "prepare succeeds")
    require(os.readlink(link) == copy.name and not off.exists() and not answer.exists(),
            "prepare points at the copy again")

    # A library that does not load: no answer, even if an old one was there, and Steam's library is used.
    broken = base / "broken.so"
    broken.write_text("not a library")
    answer.write_text("SDL_OPENVR_LIBRARY=/old\n")
    require(run(runtime, "start", library=broken) != 0, "a broken library fails the check")
    require(not answer.exists() and os.readlink(link) == str(steam / "libopenvr_api.so") and off.exists(),
            "a failed start leaves Steam stock")
    require(failed.exists(), "a failed start leaves a mark for Settings")

    # The library is missing, or Steam's own library is not where it is expected.
    require(run(runtime, "start", library=base / "missing.so") != 0 and not answer.exists(),
            "a missing library fails")
    require(run(runtime, "start", steam=base / "no-steam") != 0 and not answer.exists(),
            "a missing Steam library fails")

    # The environment file is not quoted: a path it could not carry is refused.
    spaced = base / "run time"
    require(run(spaced, "start") != 0 and not (spaced / "steam.env").exists(),
            "a runtime path with a space gets no answer")

    # A folder that is not ours alone is refused.
    elsewhere = base / "elsewhere"
    elsewhere.mkdir()
    linked = base / "linked"
    linked.symlink_to(elsewhere)
    require(run(linked, "start") != 0 and not (elsewhere / "steam.env").exists() and
            not (elsewhere / "failed").exists(), "a linked runtime folder is refused and left alone")

    # It works again after all that.
    require(run(runtime, "start") == 0 and answer.exists() and not off.exists(), "start recovers")
    require(not failed.exists(), "a start that succeeds takes the mark away")
    require(subprocess.run([str(gate)]).returncode == 2 and
            subprocess.run([str(gate), "nonsense"]).returncode == 2, "usage errors exit with 2")
print("The gate's start, prepare and stock passed.")
