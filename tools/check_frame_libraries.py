#!/usr/bin/env python3
"""Reject a prebuilt binary that needs a library, or a symbol version, the Frame lacks.

Our own binaries link against the Frame sysroot, so the linker already checks
them. A prebuilt one, such as Mozc's server, is checked here instead: every
NEEDED library must be in the sysroot and define each version the binary needs
from it (GLIBC_2.38, GLIBCXX_3.4.30 and the like).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

LIBRARY_DIRS = ("usr/lib", "lib", "usr/lib64", "lib64")


def readelf(*args):
    environment = dict(os.environ, LC_ALL="C")
    return subprocess.check_output(["readelf", "-W", *args], text=True, env=environment)


def resolve(sysroot, path):
    """`path` with its symlinks followed inside the sysroot."""
    for _ in range(16):
        if not path.is_symlink():
            break
        # A copied sysroot keeps absolute links, which point into the sysroot.
        target = Path(os.readlink(path))
        path = sysroot / target.relative_to("/") if target.is_absolute() else path.parent / target
    return path


def soname(library):
    match = re.search(r"\(SONAME\).*\[(.+)\]", readelf("-d", str(library)))
    return match.group(1) if match else None


def in_sysroot(sysroot, name):
    """The library loaded as `name`. The curated sysroot keeps some libraries
    only under their link-time name (libjson-c.so for libjson-c.so.5), which
    counts when its soname is `name`."""
    for directory in LIBRARY_DIRS:
        path = resolve(sysroot, sysroot / directory / name)
        if path.is_file():
            return path
        path = resolve(sysroot, sysroot / directory / (name.split(".so")[0] + ".so"))
        if path.is_file() and soname(path) == name:
            return path
    return None


def needed(binary):
    """The binary's NEEDED libraries, each with the versions it needs from it."""
    names = re.findall(r"\(NEEDED\).*\[(.+)\]", readelf("-d", str(binary)))
    libraries = {name: set() for name in names}
    current = None
    for line in readelf("-V", str(binary)).splitlines():
        if match := re.search(r"File: (\S+)", line):
            current = libraries.setdefault(match.group(1), set())
        elif current is not None and (match := re.search(r"Name: (\S+)\s+Flags", line)):
            current.add(match.group(1))
    return libraries


def defined(library):
    return set(re.findall(r"Name: (\S+)$", readelf("-V", str(library)), re.MULTILINE))


def validate(binary, sysroot):
    problems = []
    for name, versions in sorted(needed(binary).items()):
        library = in_sysroot(sysroot, name)
        if library is None:
            problems.append(f"{name} is not in the sysroot")
            continue
        missing = sorted(versions - defined(library))
        if missing:
            problems.append(f"{name} lacks {', '.join(missing)}")
    if problems:
        raise ValueError(f"{binary.name} needs what the Frame lacks: " + "; ".join(problems))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_frame_libraries.py BINARY SYSROOT")
    try:
        validate(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
    print("Prebuilt executable: every library and symbol version it needs is in the sysroot.")
