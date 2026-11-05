#!/usr/bin/env python3
"""Offline checks for AppImage input pins and Mozc dictionary upkeep.

    python3 tools/simulator_appimage_tests.py
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    def test_mismatched_payload_stops_before_configure(self):
        inputs = {
            "ANTHY": ["usr/lib/libanthy.so.0", "usr/lib/libanthydic.so.0",
                      "usr/share/anthy/anthy.dic"],
            "IBUS": ["usr/lib/libibus-1.0.so.5", "usr/share/ibus/dicts/emoji-ja.dict"],
            "IBUS_TABLE": ["usr/share/ibus-table/tables/cangjie5.db",
                           "usr/share/ibus-table/tables/quick5.db"],
        }
        for kind, files in inputs.items():
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                scratch = Path(directory)
                payload = scratch / "payload"
                for name in files:
                    path = payload / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b"different package payload")
                marker = scratch / "configured"
                command = scratch / "cmake"
                command.write_text('#!/bin/sh\ntouch "$TEST_CONFIGURE_MARKER"\nexit 1\n')
                command.chmod(0o755)
                env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory,
                           XDG_DATA_HOME=directory,
                           PATH=f"{directory}:{os.environ['PATH']}",
                           FRAMEKEYBOARD_APPIMAGETOOL="/bin/false",
                           FRAMEKEYBOARD_APPIMAGE_RUNTIME="/dev/null",
                           TEST_CONFIGURE_MARKER=str(marker))
                for other in inputs:
                    env[f"FRAMEKEYBOARD_{other}_ROOT"] = ""
                env[f"FRAMEKEYBOARD_{kind}_ROOT"] = str(payload)
                result = subprocess.run(["bash", "scripts/simulator-appimage.sh"],
                                        cwd=ROOT, env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(marker.exists(), "unverified inputs reached CMake")

    def test_mozc_dictionary_upstream_state(self):
        pin = re.search(r'MOZC_COMMIT = "([0-9a-f]{40})"',
                        (ROOT / "tools/build_japanese_words.py").read_text()).group(1)
        with tempfile.TemporaryDirectory() as directory:
            scratch = Path(directory)
            git = scratch / "git"
            git.write_text('#!/bin/sh\ncase "$*" in *google/mozc.git*) '
                           'printf "%s\\tHEAD\\n" "$TEST_MOZC_HEAD" ;; esac\n')
            git.chmod(0o755)
            curl = scratch / "curl"
            curl.write_text("#!/bin/sh\nprintf '[]\\n'\n")
            curl.chmod(0o755)
            for upstream, expected in [(pin, "current"), ("0" * 40, "CHECK"), ("", "CHECK")]:
                with self.subTest(upstream=upstream):
                    env = dict(os.environ, HOME=directory,
                               PATH=f"{directory}:{os.environ['PATH']}",
                               TEST_MOZC_HEAD=upstream)
                    result = subprocess.run(["bash", "tools/check-upstream.sh"], cwd=ROOT,
                                            env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    row = next((line for line in result.stdout.splitlines()
                                if line.startswith("Mozc dictionary and kaomoji")), "")
                    self.assertTrue(row, "the dictionary pin is missing from upkeep checks")
                    self.assertEqual(row.split()[-1], expected)


if __name__ == "__main__":
    unittest.main()
