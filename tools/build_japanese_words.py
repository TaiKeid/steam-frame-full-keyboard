#!/usr/bin/env python3
"""Builds japanese/words.tsv, the common words Japanese input completes a
reading to, from Mozc's open-source dictionary (docs/japanese.md, Prediction).

    build_japanese_words.py MOZC_DICTIONARY_OSS_DIR OUTPUT [--count N]

MOZC_DICTIONARY_OSS_DIR is src/data/dictionary_oss of a Mozc checkout at the
commit named below; nothing else of Mozc is used. The output keeps the N
commonest words (lowest Mozc cost) with an all-hiragana reading, as
reading<TAB>word<TAB>cost lines sorted by reading, then cost.
"""
import argparse
import pathlib
import re
import sys

MOZC_COMMIT = "74f8ee66c5546d1f3ce4fadbef23a3b402239eed"
# Parts of speech that complete a reading well: words, not names, particles,
# affixes or symbols.
KEPT = ("名詞,一般", "名詞,代名詞", "名詞,サ変接続", "名詞,形容動詞語幹", "名詞,副詞可能",
        "名詞,ナイ形容詞語幹", "動詞,自立", "形容詞,自立", "副詞", "感動詞", "連体詞", "接続詞")
READING = re.compile(r"^[ぁ-ゖー]{2,12}$")
# A word written in Latin letters or digits completes nothing typed in kana.
LATIN = re.compile(r"[0-9A-Za-z０-９Ａ-Ｚａ-ｚ]")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dictionary", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--count", type=int, default=20000)
    args = parser.parse_args()

    kinds = {}
    for line in (args.dictionary / "id.def").read_text(encoding="utf-8").splitlines():
        number, _, kind = line.partition(" ")
        kinds[int(number)] = kind
    # Dictionary forms only: an inflected stem such as 興味深かっ completes
    # nothing anyone would choose.
    kept_ids = {number for number, kind in kinds.items()
                if kind.startswith(KEPT) and kind.split(",")[5] in ("*", "基本形")}
    # A word Mozc compounded from one of its words (顔全体, 今日の給食, 本剤)
    # shares that word's id, whose last field names it: keep only the word.
    base_of = {number: kind.split(",")[6] for number, kind in kinds.items()}
    hidden = set()
    for line in (args.dictionary / "suggestion_filter.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            hidden.add(line.strip())

    best = {}  # (reading, word) -> cost
    for path in sorted(args.dictionary.glob("dictionary0[0-9].txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) != 5:
                continue
            reading, left, _, cost, word = fields
            base = base_of.get(int(left), "*")
            if (int(left) not in kept_ids or not READING.match(reading) or LATIN.search(word) or
                    word in hidden or "\t" in word or (base != "*" and word != base)):
                continue
            key = (reading, word)
            best[key] = min(best.get(key, 1 << 30), int(cost))
    common = sorted(best.items(), key=lambda item: (item[1], item[0]))[:args.count]
    common.sort(key=lambda item: (item[0][0], item[1]))
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("# Common Japanese words: reading<TAB>word<TAB>cost, lower is commoner.\n")
        output.write(f"# Built by tools/build_japanese_words.py from Mozc's dictionary_oss at\n"
                     f"# google/mozc {MOZC_COMMIT};\n"
                     "# licenses in third_party/mozc/LICENSE (Mozc, IPAdic, ICOT, Okinawa dictionary).\n")
        for (reading, word), cost in common:
            output.write(f"{reading}\t{word}\t{cost}\n")
    print(f"{len(common)} words, costs up to {max(cost for _, cost in common)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
