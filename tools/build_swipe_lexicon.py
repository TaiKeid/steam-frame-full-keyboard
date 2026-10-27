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
  tsv           a lexicon this script wrote, to blend another list into it

--blend adds a FrequencyWords list ("word count", lowercase, from film
subtitles), so everyday chat words that news text rarely uses rank as they
are spoken.
"""

import argparse
import math
import re
import statistics
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


# A capitalised form at most this many steps (about 4.6x) above its lowercase
# form is taken as sentence-initial; further above, it is a name (Warszawa 129,
# warszawa 75).
CAPITAL_FOLD_STEPS = 20


def fold_initial_capitals(entries):
    """Fold "Po" into "po" when both exist, keeping the higher frequency.

    Sentence-initial capitals can outrank the lowercase word (Polish Po 199,
    po 183), and the runtime offers one case variant per word, so a swipe
    mid-sentence would type "Po". Names stay capitalised. Not for German,
    whose nouns are capitalised (Zeit 187, zeit 86).
    """
    folded = dict(entries)
    for word, frequency in entries.items():
        lower = word[:1].lower() + word[1:]
        if lower != word and lower in entries and frequency <= entries[lower] + CAPITAL_FOLD_STEPS:
            folded[lower] = max(folded[lower], frequency)
            del folded[word]
    return folded


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


def from_tsv(path):
    entries = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("#") or "\t" not in line:
                continue
            word, frequency = line.rstrip("\n").split("\t")
            entries[word] = int(frequency)
    return entries


# Subtitle words whose calibration is measured: the most frequent ones, where
# both lists count reliably.
BLEND_CALIBRATION_WORDS = 5000


def blend_subtitles(entries, path, lowercase_capitals=False):
    """Raise words to their film-subtitle frequency; add the missing ones.

    Subtitle counts go on the same 0-255 log scale, shifted by the median
    difference on words both lists share, so only a word spoken more often
    than written rises. A word already listed keeps its spelling (German Zeit);
    a new one is added as the subtitle list spells it, in lowercase. With
    lowercase_capitals, a word listed only with an initial capital (Dutch Hoi,
    from sentence starts) takes the lowercase spelling when the subtitles use
    it about as often (see fold_initial_capitals).
    """
    counts = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            parts = line.split()
            if len(parts) == 2 and letters_only(parts[0]) and parts[1].isdigit():
                counts[parts[0]] = counts.get(parts[0], 0) + int(parts[1])
    subtitle = scaled(counts, 1)
    spellings = {}
    for word, frequency in entries.items():
        key = word.casefold()
        if key not in spellings or frequency > entries[spellings[key]]:
            spellings[key] = word
    ranked = sorted(subtitle, key=lambda word: -subtitle[word])[:BLEND_CALIBRATION_WORDS]
    shared = [entries[spellings[w.casefold()]] - subtitle[w] for w in ranked if w.casefold() in spellings]
    offset = statistics.median(shared) if shared else 0
    blended = dict(entries)
    added = raised = 0
    for word, frequency in subtitle.items():
        frequency = max(0, min(255, round(frequency + offset)))
        listed = spellings.get(word.casefold())
        if listed is None:
            blended[word] = frequency
            added += 1
        elif (
            lowercase_capitals
            and listed != word
            and listed[:1].lower() + listed[1:] == word
            and frequency + CAPITAL_FOLD_STEPS >= blended[listed]
        ):
            blended[word] = max(frequency, blended.pop(listed))
            raised += 1
        elif frequency > blended[listed]:
            blended[listed] = frequency
            raised += 1
    print(f"{path}: offset {offset:+.0f}, {raised} raised, {added} added", file=sys.stderr)
    return blended


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
    parser.add_argument("--format", choices=("combined", "leipzig", "jieba-pinyin", "tsv"), default="combined")
    parser.add_argument("--blend", help="FrequencyWords subtitle list to blend in")
    parser.add_argument("--limit", type=int, default=100000)
    parser.add_argument("--minimum-count", type=int, default=3)
    parser.add_argument("--header", action="append", default=[], help="comment line to record")
    parser.add_argument(
        "--fold-initial-capitals",
        action="store_true",
        help="fold a capitalised word into its lowercase form when both exist",
    )
    args = parser.parse_args()

    if args.format == "combined":
        entries = from_combined(args.source)
    elif args.format == "leipzig":
        entries = from_leipzig(args.source, args.minimum_count)
    elif args.format == "tsv":
        entries = from_tsv(args.source)
    else:
        entries = from_jieba(args.source, args.minimum_count)
    if args.blend:
        entries = blend_subtitles(entries, args.blend, args.fold_initial_capitals)
    if args.fold_initial_capitals:
        entries = fold_initial_capitals(entries)
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
