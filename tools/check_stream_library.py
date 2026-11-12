#!/usr/bin/env python3
"""Hold the Steam Link pass-through library to what the streaming client may load.

It stands in for libopenvr_api.so inside the streaming client, so it must:
- name the real library as a dependency without a path, with no search path but its own folder
  (RUNPATH exactly $ORIGIN, never RPATH), where the gate keeps a link to Steam's copy: after the
  client's LD_LIBRARY_PATH it binds that copy, never another one on the system, and nothing of the
  build machine is in it;
- depend on nothing else but the C library;
- offer only the one call it wraps, so every other OpenVR symbol resolves to the real library.
"""
import os
import re
import subprocess
import sys

REAL_LIBRARY = "libopenvr_api.so"
ALLOWED_DEPENDENCIES = {REAL_LIBRARY, "libc.so.6", "ld-linux-aarch64.so.1"}
WRAPPED = {"VR_GetGenericInterface"}
# Section marks some linkers define in every shared object.
LINKER_SYMBOLS = {"_init", "_fini", "_edata", "_end", "__bss_start", "__bss_start__", "__bss_end__",
                  "__end__", "_bss_end__"}


def readelf(*arguments):
    return subprocess.check_output(["readelf", "-W", *arguments], text=True,
                                   env=dict(os.environ, LC_ALL="C"))


def validate(library):
    dynamic = readelf("-d", library)
    needed = set(re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic))
    if REAL_LIBRARY not in needed:
        raise ValueError(f"stream library does not depend on {REAL_LIBRARY} by name")
    if needed - ALLOWED_DEPENDENCIES:
        raise ValueError(f"stream library has other dependencies: {sorted(needed - ALLOWED_DEPENDENCIES)}")
    if re.search(r"\(RPATH\)", dynamic):
        raise ValueError("stream library carries an RPATH")
    runpaths = re.findall(r"\(RUNPATH\)\s+Library runpath: \[([^\]]*)\]", dynamic)
    if runpaths != ["$ORIGIN"]:
        raise ValueError(f"stream library searches {runpaths}, expected only its own folder ($ORIGIN)")
    defined = set()
    for line in readelf("--dyn-syms", library).splitlines():
        fields = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(fields) >= 8 and fields[0].endswith(":") and fields[4] in ("GLOBAL", "WEAK") and fields[6] != "UND":
            defined.add(fields[7].split("@")[0])
    if defined - LINKER_SYMBOLS != WRAPPED:
        raise ValueError(f"stream library offers {sorted(defined - LINKER_SYMBOLS)}, expected {sorted(WRAPPED)}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_stream_library.py LIBRARY")
    try:
        validate(sys.argv[1])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
    print(f"Stream library: depends on {REAL_LIBRARY} by name, searches only its own folder, one wrapped call.")
