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

--turkic-case lowercases I to ı and İ to i (Turkish, Azerbaijani).
--script keeps only words in one script, as Bulgarian, Macedonian, Greek,
Arabic and Hebrew sources carry Latin words. --serbian-cyrillic implies --script cyrillic and
converts Latin subtitle words to Cyrillic, as Serbian subtitles are mostly
written in Latin.
--respell replaces letters, merging the words that then match: Romanian ş and
ţ with a cedilla become ș and ț with a comma, as Romanian keyboards type them.
"""

import argparse
import math
import re
import statistics
import sys

# Some lists drop the leading space or add one after the comma.
WORD = re.compile(r"^ ?word=([^,]+), ?f=(\d+)")
# Natural-log units per 0-255 step, matching the runtime's interpretation.
FREQUENCY_SCALE = 13.0


# Set by --turkic-case: dotted and dotless I are separate letters.
TURKIC_CASE = False


def lower(text):
    if TURKIC_CASE:
        text = text.replace("I", "ı").replace("İ", "i")
    return text.lower()


def lower_initial(word):
    return lower(word[:1]) + word[1:]


def fold_case(text):
    return lower(text) if TURKIC_CASE else text.casefold()


# Serbian Latin to Cyrillic, digraphs first. A few loanwords split lj, nj or dž
# across morphemes (injekcija); they come out wrong and rank low.
SERBIAN_DIGRAPHS = {"lj": "љ", "nj": "њ", "dž": "џ"}
SERBIAN_LETTERS = dict(zip("abvgdđežzijklmnoprstćufhcčš", "абвгдђежзијклмнопрстћуфхцчш"))


def serbian_cyrillic(word):
    """Cyrillic for a Serbian Latin word, None for other Latin text."""
    out, i, lowered = [], 0, word.lower()
    while i < len(lowered):
        pair = lowered[i : i + 2]
        if pair in SERBIAN_DIGRAPHS:
            letter, step = SERBIAN_DIGRAPHS[pair], 2
        elif lowered[i] in SERBIAN_LETTERS:
            letter, step = SERBIAN_LETTERS[lowered[i]], 1
        elif lowered[i] in "'’":
            letter, step = lowered[i], 1
        else:
            return None  # q, w, x, y and accents are not Serbian Latin.
        out.append(letter.upper() if word[i].isupper() else letter)
        i += step
    return "".join(out)


SCRIPTS = {
    "arabic": ("\u0621", "\u064a"),  # Letters, harakat and digits after it; tatweel is dropped below.
    "cyrillic": ("\u0400", "\u04ff"),
    "greek": ("\u0370", "\u03ff"),
    "hebrew": ("\u05d0", "\u05ea"),  # Letters, final forms included; no points.
}


def in_script(word, script):
    first, last = SCRIPTS[script]
    # Greek accents beyond the monotonic ones sit in Greek Extended.
    extended = script == "greek"
    return "\u0640" not in word and all(  # Arabic tatweel only stretches a letter.
        first <= ch <= last or (extended and "\u1f00" <= ch <= "\u1fff") or ch in "'’" for ch in word
    )


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
        lower = lower_initial(word)
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
            key = lower(word)
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


def blend_subtitles(entries, path, lowercase_capitals=False, transliterate=None):
    """Raise words to their film-subtitle frequency; add the missing ones.

    Subtitle counts go on the same 0-255 log scale, shifted by the median
    difference on words both lists share, so only a word spoken more often
    than written rises. A word already listed keeps its spelling (German Zeit);
    a new one is added as the subtitle list spells it, in lowercase. With
    lowercase_capitals, a word listed only with an initial capital (Dutch Hoi,
    from sentence starts) takes the lowercase spelling when the subtitles use
    it about as often (see fold_initial_capitals). transliterate converts
    each subtitle word first, or returns None to skip it.
    """
    counts = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            parts = line.split()
            if len(parts) == 2 and letters_only(parts[0]) and parts[1].isdigit():
                word = transliterate(parts[0]) if transliterate else parts[0]
                if word:
                    counts[word] = counts.get(word, 0) + int(parts[1])
    subtitle = scaled(counts, 1)
    spellings = {}
    for word, frequency in entries.items():
        key = fold_case(word)
        if key not in spellings or frequency > entries[spellings[key]]:
            spellings[key] = word
    ranked = sorted(subtitle, key=lambda word: -subtitle[word])[:BLEND_CALIBRATION_WORDS]
    shared = [entries[spellings[fold_case(w)]] - subtitle[w] for w in ranked if fold_case(w) in spellings]
    offset = statistics.median(shared) if shared else 0
    blended = dict(entries)
    added = raised = 0
    for word, frequency in subtitle.items():
        frequency = max(0, min(255, round(frequency + offset)))
        listed = spellings.get(fold_case(word))
        if listed is None:
            blended[word] = frequency
            added += 1
        elif (
            lowercase_capitals
            and listed != word
            and lower_initial(listed) == word
            and frequency + CAPITAL_FOLD_STEPS >= blended[listed]
        ):
            blended[word] = max(frequency, blended.pop(listed))
            spellings[fold_case(word)] = word  # Greek ας and ασ fold alike.
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


def respelled(entries, pairs):
    table = str.maketrans(dict(pair.split("=", 1) for pair in pairs))
    merged = {}
    for word, frequency in entries.items():
        word = word.translate(table)
        merged[word] = max(frequency, merged.get(word, 0))
    return merged


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
    parser.add_argument("--turkic-case", action="store_true", help="lowercase I to ı and İ to i")
    parser.add_argument("--respell", action="append", default=[], metavar="FROM=TO", help="replace a letter")
    parser.add_argument("--script", choices=sorted(SCRIPTS), help="keep only words in this script")
    parser.add_argument(
        "--serbian-cyrillic",
        action="store_true",
        help="keep Cyrillic words and convert Serbian Latin subtitle words to Cyrillic",
    )
    args = parser.parse_args()
    global TURKIC_CASE
    TURKIC_CASE = args.turkic_case

    if args.format == "combined":
        entries = from_combined(args.source)
    elif args.format == "leipzig":
        entries = from_leipzig(args.source, args.minimum_count)
    elif args.format == "tsv":
        entries = from_tsv(args.source)
    else:
        entries = from_jieba(args.source, args.minimum_count)
    script = "cyrillic" if args.serbian_cyrillic else args.script
    if args.blend:
        transliterate = serbian_cyrillic if args.serbian_cyrillic else None
        entries = blend_subtitles(entries, args.blend, args.fold_initial_capitals, transliterate)
    if args.respell:
        entries = respelled(entries, args.respell)
    if script:
        entries = {word: f for word, f in entries.items() if in_script(word, script)}
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
