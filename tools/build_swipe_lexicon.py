#!/usr/bin/env python3
"""Build a swipe lexicon TSV from an AOSP-format .combined word list.

Run on the development host. Output lines are "word<TAB>f", where f is the
list's 0-255 log-frequency, most frequent first. Words keep their case so
German nouns stay capitalised. Only letters and apostrophes are kept; the
runtime maps letters to keys, folds accents without their own key and skips
apostrophes in the swipe path.
"""

import argparse
import re
import sys

WORD = re.compile(r"^ word=([^,]+),f=(\d+)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", help="AOSP .combined word list")
    parser.add_argument("output")
    parser.add_argument("--limit", type=int, default=100000)
    parser.add_argument("--header", action="append", default=[], help="comment line to record")
    args = parser.parse_args()

    entries = {}
    with open(args.source, encoding="utf-8") as source:
        for line in source:
            match = WORD.match(line)
            if not match:
                continue  # header, bigram and shortcut lines
            word, frequency = match.group(1), int(match.group(2))
            letters = [ch for ch in word if ch not in "'’"]
            if len(letters) < 2 or not all(ch.isalpha() for ch in letters):
                continue
            entries[word] = max(frequency, entries.get(word, 0))
    ranked = sorted(entries.items(), key=lambda item: (-item[1], item[0]))[: args.limit]
    with open(args.output, "w", encoding="utf-8") as output:
        output.write("# Full Keyboard swipe lexicon: word<TAB>log-frequency 0-255\n")
        for header in args.header:
            output.write(f"# {header}\n")
        for word, frequency in ranked:
            output.write(f"{word}\t{frequency}\n")
    print(f"{args.output}: {len(ranked)} words", file=sys.stderr)


if __name__ == "__main__":
    main()
