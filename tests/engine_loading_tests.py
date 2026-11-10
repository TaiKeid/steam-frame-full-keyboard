#!/usr/bin/env python3
"""Run a copied test executable without modules, then with broken dependencies.

All fixtures live in a temporary directory. Installed engines are never moved.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="full-keyboard-engines-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    probe = root / "bin/language-tests"
    shutil.copy2(binary, probe)
    subprocess.run([str(probe), "--without-engines"], check=True)
    modules = binary.parent / "engines"
    if modules.is_dir():
        shutil.copytree(modules, probe.parent / "engines")
        # Stale aliases must not restore a removed fallback or accept a
        # retired override. Use real retained modules, so the fixtures would
        # actually load if runtime selection still admitted those names.
        copied = probe.parent / "engines"
        held = root / "retained"
        held.mkdir()
        for source, retired in [("libime", "pinyin"), ("mcbopomofo", "chewing")]:
            module = copied / f"framekeyboard-{source}.so"
            if module.exists():
                shutil.copy2(module, copied / f"framekeyboard-{retired}.so")
        for name in ("hangul", "libime", "mcbopomofo"):
            module = copied / f"framekeyboard-{name}.so"
            if module.exists():
                module.rename(held / module.name)
        subprocess.run([str(probe), "--without-engines"], check=True)
        for module in held.iterdir():
            module.rename(copied / module.name)
        # Force dlopen's dependency resolution to fail without altering the OS.
        libraries = root / "unavailable"
        libraries.mkdir()
        # LD_LIBRARY_PATH comes before the Chinese modules' run path.
        for name in ("libhangul.so.1", "libIMEPinyin.so.0", "libIMECore.so.0",
                     "libFcitx5Utils.so.2"):
            (libraries / name).write_bytes(b"unavailable library test fixture")
        subprocess.run([str(probe), "--without-engines"], check=True,
                       env=dict(os.environ, LD_LIBRARY_PATH=str(libraries)))
        print("Missing modules and unavailable runtime dependencies passed.")
    else:
        print("Missing modules passed; runtime dependency case needs engine modules.")
