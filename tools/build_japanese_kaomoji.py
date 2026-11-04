#!/usr/bin/env python3
"""Builds japanese/kaomoji.tsv, the kaomoji Japanese input offers for a
reading (docs/japanese.md, Prediction), from Mozc's emoticon data.

    build_japanese_kaomoji.py MOZC_EMOTICON_DIR OUTPUT

MOZC_EMOTICON_DIR is src/data/emoticon of a Mozc checkout at the commit named
below. Each kaomoji is listed under each of its readings, and all of them under
かお and かおもじ, as phones offer them, as reading<TAB>kaomoji<TAB>order lines
sorted by reading, then by Mozc's own order.
"""
import argparse
import pathlib
import sys

MOZC_COMMIT = "74f8ee66c5546d1f3ce4fadbef23a3b402239eed"
EVERY = ["かお", "かおもじ"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("emoticon", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    faces = []  # (kaomoji, readings) in Mozc's order
    # emoticon.tsv: kaomoji<TAB>readings<TAB>categories, after a header line.
    for line in (args.emoticon / "emoticon.tsv").read_text(encoding="utf-8").splitlines()[1:]:
        fields = line.split("\t")
        if len(fields) >= 2 and fields[0]:
            faces.append((fields[0], fields[1].split()))
    # categorized.tsv: kaomoji<TAB>category<TAB>readings; # starts a comment.
    for line in (args.emoticon / "categorized.tsv").read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if not line.startswith("#") and len(fields) >= 3 and fields[0]:
            faces.append((fields[0], fields[2].split()))

    # A kaomoji in both files keeps its first place and all its readings.
    merged = {}  # kaomoji -> (order, readings)
    for order, (face, readings) in enumerate(faces):
        if "\t" not in face:
            first, known = merged.setdefault(face, (order, []))
            known.extend(readings)
    seen = merged.keys()
    rows = []
    for face, (order, readings) in merged.items():
        for reading in dict.fromkeys(readings + EVERY):
            rows.append((reading, order, face))
    rows.sort()
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("# Kaomoji for Japanese readings: reading<TAB>kaomoji<TAB>order.\n")
        output.write(f"# Built by tools/build_japanese_kaomoji.py from Mozc's src/data/emoticon at\n"
                     f"# google/mozc {MOZC_COMMIT}; license in third_party/mozc/LICENSE.\n")
        for reading, order, face in rows:
            output.write(f"{reading}\t{face}\t{order}\n")
    print(f"{len(seen)} kaomoji, {len(rows)} lines", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
