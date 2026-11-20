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

--fold-initial-capitals folds a sentence-initial capital into its lowercase
word; --fold-initial-capitals-steps sets how many frequency steps above the
lowercase still count as sentence-initial (default 20).

--fold-written-capitals N folds, before the blend, a capitalized word at most N
steps above its lowercase word in the written list, so the subtitle count
raises the lowercase word (English "He" over "he"). --fold-written-floor F and
--fold-written-spoken-floor S fold it only if the lowercase word is common in
its own right: listed at F or above, or given S or above by the subtitles on
the written scale. Without them every such capital folds.

--always-fold LIST folds, before the blend, the capitals named for that list in
ALWAYS_FOLD into their lowercase words, however far above them they are.

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
    # Letters only: a word with harakat (U+064B on) is left out, as no key
    # types them; tatweel (U+0640, inside the range) is dropped below.
    "arabic": ("\u0621", "\u064a"),
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
# warszawa 75). Lists tune it with --fold-initial-capitals-steps.
DEFAULT_CAPITAL_FOLD_STEPS = 20


def fold_initial_capitals(entries, steps=DEFAULT_CAPITAL_FOLD_STEPS):
    """Fold "Po" into "po" when both exist, keeping the higher frequency.

    Sentence-initial capitals can outrank the lowercase word (Polish Po 199,
    po 183), and the runtime offers one case variant per word, so a swipe
    mid-sentence would type "Po". A capitalised form more than `steps` above
    its lowercase is a name and stays capitalised. Not for German, whose nouns
    are capitalised (Zeit 187, zeit 86).
    """
    folded = dict(entries)
    for word, frequency in entries.items():
        lower = lower_initial(word)
        if lower != word and lower in entries and frequency <= entries[lower] + steps:
            folded[lower] = max(folded[lower], frequency)
            del folded[word]
    return folded


def fold_written_capitals(entries, steps, common=None):
    """Fold "He" into "he" when "He" is listed at most `steps` above it.

    For a list about to be blended. Subtitle lists are lowercase, so the blend
    raises whichever case variant is listed higher; sentence starts put
    English "He" (193) a little above "he" (187), and the subtitle count for
    "he" would then lift "He" further. A capital far above its lowercase word
    is a name (John 147, john 58) and keeps the raise. A capital below its
    lowercase word is left listed: the raise already goes to the lowercase
    word, and the runtime offers only the commonest spelling.

    Rare words crowd together at the bottom of the scale, so a rare name can
    sit within `steps` of a lowercase twin that is only noise (Wikipedia 98,
    wikipedia 78). `common`, given the lowercase word, says whether it is a
    word in its own right; only then does the capital fold.
    """
    folded = dict(entries)
    for word, frequency in entries.items():
        lower = lower_initial(word)
        if lower != word and lower in entries and entries[lower] <= frequency <= entries[lower] + steps and \
                (common is None or common(lower)):
            folded[lower] = frequency
            del folded[word]
    return folded


# Per list, capitals that always fold into their lowercase words: ordinary
# words their capital outranks by far, beyond any window (British "Hi" 158,
# "hi" 77; American "Sir" 167, "sir" 92), and only those no rule reaches.
ALWAYS_FOLD = {
    "en-gb": ("Hi",),
    "en-us": ("Sir",),
}


def always_fold(entries, words):
    """Fold each named capital into its lowercase word, which takes the higher frequency."""
    folded = dict(entries)
    for word in words:
        lower = lower_initial(word)
        if word in folded and lower != word:
            folded[lower] = max(folded.pop(word), folded.get(lower, 0))
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


def blend_subtitles(entries, path, lowercase_capitals=False, transliterate=None,
                    steps=DEFAULT_CAPITAL_FOLD_STEPS, written_fold=None):
    """Raise words to their film-subtitle frequency; add the missing ones.

    Subtitle counts go on the same 0-255 log scale, shifted by the median
    difference on words both lists share, so only a word spoken more often
    than written rises. A word already listed keeps its spelling (German Zeit);
    a new one is added as the subtitle list spells it, in lowercase. With
    lowercase_capitals, a word listed only with an initial capital (Dutch Hoi,
    from sentence starts) takes the lowercase spelling when the subtitles use
    it about as often (see fold_initial_capitals). transliterate converts
    each subtitle word first, or returns None to skip it. written_fold, as
    (steps, written floor, spoken floor), first folds capitals as
    fold_written_capitals does, a lowercase word counting as common when it is
    listed at the written floor or above, or its subtitle frequency on the
    written scale reaches the spoken floor; a floor of None is no condition.
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
    if written_fold:
        # Folding keeps each key's frequency, so the offset is the same either way.
        fold_steps, written_floor, spoken_floor = written_fold
        conditional = written_floor is not None or spoken_floor is not None

        def common(lower):
            spoken = subtitle.get(lower)
            return (written_floor is not None and entries[lower] >= written_floor or
                    spoken_floor is not None and spoken is not None and
                    round(spoken + offset) >= spoken_floor)
        entries = fold_written_capitals(entries, fold_steps, common if conditional else None)
        spellings = {}
        for word, frequency in entries.items():
            key = fold_case(word)
            if key not in spellings or frequency > entries[spellings[key]]:
                spellings[key] = word
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
            and frequency + steps >= blended[listed]
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
    parser.add_argument(
        "--fold-initial-capitals-steps",
        type=int,
        default=DEFAULT_CAPITAL_FOLD_STEPS,
        metavar="N",
        help="frequency steps above the lowercase that still count as sentence-initial (default 20)",
    )
    parser.add_argument(
        "--fold-written-capitals",
        type=int,
        metavar="N",
        help="before the blend, fold a capitalized word at most N steps above its lowercase word",
    )
    parser.add_argument(
        "--fold-written-floor",
        type=int,
        metavar="F",
        help="with --fold-written-capitals: the lowercase word is common if listed at F or above",
    )
    parser.add_argument(
        "--fold-written-spoken-floor",
        type=int,
        metavar="S",
        help="with --fold-written-capitals: or if the subtitles give it S or above on the written scale",
    )
    parser.add_argument(
        "--always-fold",
        choices=sorted(ALWAYS_FOLD),
        metavar="LIST",
        help="fold the capitals ALWAYS_FOLD names for this list into their lowercase words",
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
    if args.fold_initial_capitals_steps < 0:
        parser.error("--fold-initial-capitals-steps must be zero or more")
    if args.fold_written_capitals is not None and (args.fold_written_capitals < 0 or not args.blend):
        parser.error("--fold-written-capitals needs --blend and zero or more steps")
    if (args.fold_written_floor is not None or args.fold_written_spoken_floor is not None) and \
            args.fold_written_capitals is None:
        parser.error("--fold-written-floor and --fold-written-spoken-floor need --fold-written-capitals")
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
    if args.always_fold:
        entries = always_fold(entries, ALWAYS_FOLD[args.always_fold])
    if args.blend:
        transliterate = serbian_cyrillic if args.serbian_cyrillic else None
        written_fold = None if args.fold_written_capitals is None else (
            args.fold_written_capitals, args.fold_written_floor, args.fold_written_spoken_floor)
        entries = blend_subtitles(
            entries, args.blend, args.fold_initial_capitals, transliterate,
            args.fold_initial_capitals_steps, written_fold
        )
    if args.respell:
        entries = respelled(entries, args.respell)
    if script:
        entries = {word: f for word, f in entries.items() if in_script(word, script)}
    if args.fold_initial_capitals:
        entries = fold_initial_capitals(entries, args.fold_initial_capitals_steps)
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
