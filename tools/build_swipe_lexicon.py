#!/usr/bin/env python3
"""Build a swipe lexicon TSV from a word frequency list.

Run on the development host. Output lines are "word<TAB>f", where f is a 0-255
log-frequency, most frequent first. Words keep their case so German nouns stay
capitalised. Only letters and apostrophes are kept; the runtime maps letters to
keys, folds accents without their own key and skips apostrophes in the path.

Formats:
  combined      AOSP .combined list (" word=X,f=N" lines, f already 0-255)
  leipzig       Leipzig Corpora Collection *-words.txt ("id<TAB>word<TAB>count")
  jieba-pinyin  jieba dict.txt ("word count tag"), converted to toneless Pinyin
                with pypinyin; the "word" is the Pinyin the composer receives
"""

import argparse
import math
import re
import sys

WORD = re.compile(r"^ word=([^,]+),f=(\d+)")
# Natural-log units per 0-255 step, matching the runtime's interpretation.
FREQUENCY_SCALE = 13.0


def letters_only(word):
    letters = [ch for ch in word if ch not in "'’"]
    return len(letters) >= 2 and all(ch.isalpha() for ch in letters)


def from_combined(path):
    entries = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            match = WORD.match(line)
            if match and letters_only(match.group(1)):
                word, frequency = match.group(1), int(match.group(2))
                entries[word] = max(frequency, entries.get(word, 0))
    return entries


def scaled(counts, minimum):
    """Relative corpus frequency p becomes 255 + 13 ln p, as AOSP lists roughly do."""
    total = sum(counts.values())
    return {
        word: max(0, min(255, round(255 + FREQUENCY_SCALE * math.log(count / total))))
        for word, count in counts.items()
        if count >= minimum
    }


def from_leipzig(path, minimum):
    # Sentence-initial capitals inflate capitalised forms. Keep one form per
    # lowercase word: the most frequent spelling, with the counts combined.
    forms, totals = {}, {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3 or not letters_only(parts[1]):
                continue
            word, count = parts[1], int(parts[2])
            key = word.lower()
            totals[key] = totals.get(key, 0) + count
            best = forms.get(key)
            if best is None or count > best[1]:
                forms[key] = (word, count)
    return scaled({forms[key][0]: total for key, total in totals.items()}, minimum)


def pinyin_of(word):
    from pypinyin import lazy_pinyin

    syllables = lazy_pinyin(word, errors=lambda _: None)  # "v" stands for ü, as typed
    if not syllables or len(syllables) != len(word) or not all(s.isascii() and s.isalpha() for s in syllables):
        return None
    # An apostrophe separates a vowel-initial syllable (xi'an), as Pinyin input does.
    text = syllables[0]
    for syllable in syllables[1:]:
        text += ("'" if syllable[0] in "aoe" else "") + syllable
    return text


def from_jieba(path, minimum):
    counts = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            parts = line.split()
            if len(parts) < 2 or not 1 <= len(parts[0]) <= 4:
                continue
            if not all("一" <= ch <= "鿿" for ch in parts[0]):
                continue
            pinyin = pinyin_of(parts[0])
            if pinyin and len(pinyin.replace("'", "")) >= 2:
                counts[pinyin] = counts.get(pinyin, 0) + int(parts[1])
    return scaled(counts, minimum)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--format", choices=("combined", "leipzig", "jieba-pinyin"), default="combined")
    parser.add_argument("--limit", type=int, default=100000)
    parser.add_argument("--minimum-count", type=int, default=3)
    parser.add_argument("--header", action="append", default=[], help="comment line to record")
    args = parser.parse_args()

    if args.format == "combined":
        entries = from_combined(args.source)
    elif args.format == "leipzig":
        entries = from_leipzig(args.source, args.minimum_count)
    else:
        entries = from_jieba(args.source, args.minimum_count)
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
