#!/usr/bin/env python3
"""Builds chinese/char-ranks.txt, the order Cangjie and Quick list characters
of one code in (docs/languages.md, Chinese and Korean composition): one
character a line, commonest first. It holds exactly the CJK ideographs of
Big5-HKSCS, the repertoire Taiwan and Hong Kong input methods offer; a
character outside it (Simplified-only and Japanese forms, most of Extensions
A and B) counts as rare and is offered after it.

    build_chinese_ranks.py MCBOPOMOFO_DATA ESSAY_CANTONESE CANTONESE_WORDS OUTPUT

MCBOPOMOFO_DATA is data/data.txt of fcitx5-mcbopomofo; ESSAY_CANTONESE and
CANTONESE_WORDS are essay-cantonese.txt and jyut6ping3.words.dict.yaml of
rime-cantonese, each at the commit named below, which the input's SHA-256
confirms. Taiwan usage comes from McBopomofo: a
character's best single-character score over its readings (one reading's
score, not their sum, as McBopomofo already discounts other readings). Hong
Kong usage comes from rime-cantonese's counts of single characters; characters
its counts lack (𨋢, 冧, 冚) follow them, by how many of its Cantonese
dictionary words hold them, if at least two do. Each source
orders its CJK ideographs, and a character takes the better of its two places,
so one common in either region is common. After one empty line, the rest of
Big5-HKSCS follows by code point: characters neither source uses, which count
as rare too. Ties go by code point, so the output never changes for the
same inputs. Big5-HKSCS is Python's big5hkscs codec.
"""
import hashlib
import pathlib
import sys

MCBOPOMOFO_COMMIT = "a84bdd5c741e1696b270159ebda675946dc73d49"  # fcitx5-mcbopomofo 3.1.1
MCBOPOMOFO_SHA256 = "0deae7b7c1dcde1d7a30d139e7068543e0c0e7112e944b63eb52947bca1db7ac"
RIME_CANTONESE_COMMIT = "259f0e48bba840c3a2e0d117539e96937f3d89bc"
RIME_CANTONESE_SHA256 = "d08836175f598219f43c2f2f9e12e12711212dbefe08d57b2eddb7a9d9f22a5d"
RIME_CANTONESE_WORDS_SHA256 = "54d174ad2bb997e4a678b7b076b84e4dc5914481b32467cdea3b43e88b7d5474"
# Fewer words than this is too little evidence that a character is in use.
MINIMUM_WORDS = 2


def ideograph(character):
    """A CJK Unified Ideograph, of any extension; not a compatibility one."""
    point = ord(character)
    return (0x3400 <= point <= 0x4DBF or 0x4E00 <= point <= 0x9FFF or 0x20000 <= point <= 0x3FFFF) and not (
        0x2F800 <= point <= 0x2FA1F)


def traditional(character):
    """In Big5-HKSCS."""
    try:
        character.encode("big5hkscs")
        return True
    except UnicodeEncodeError:
        return False


def checked(path, digest):
    data = pathlib.Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f"{path} is not the pinned file (SHA-256 {digest})")
    return data.decode("utf-8")


def taiwan(text):
    scores = {}
    for line in text.splitlines():
        fields = line.split()
        # Readings starting with _ are McBopomofo's punctuation and macros.
        if len(fields) == 3 and not fields[0].startswith("_") and len(fields[1]) == 1 and ideograph(fields[1]):
            scores[fields[1]] = max(scores.get(fields[1], float("-inf")), float(fields[2]))
    return sorted(scores, key=lambda c: (-scores[c], ord(c)))


def hong_kong(text, words):
    counts = {}
    for line in text.splitlines():
        fields = line.split("\t")
        if len(fields) == 2 and len(fields[0]) == 1 and ideograph(fields[0]):
            counts[fields[0]] = max(counts.get(fields[0], 0), int(fields[1]))
    order = sorted(counts, key=lambda c: (-counts[c], ord(c)))
    # Words after the dictionary's header, one a line before a tab.
    used = {}
    for line in words.split("\n...\n", 1)[1].splitlines():
        word = line.split("\t")[0]
        if len(word) > 1:
            for character in set(word):
                if ideograph(character) and character not in counts:
                    used[character] = used.get(character, 0) + 1
    return order + sorted((c for c in used if used[c] >= MINIMUM_WORDS), key=lambda c: (-used[c], ord(c)))


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    orders = [taiwan(checked(sys.argv[1], MCBOPOMOFO_SHA256)),
              hong_kong(checked(sys.argv[2], RIME_CANTONESE_SHA256),
                        checked(sys.argv[3], RIME_CANTONESE_WORDS_SHA256))]
    place = {}
    for order in orders:
        for index, character in enumerate(order):
            place[character] = min(place.get(character, index), index)
    ranked = sorted((c for c in place if traditional(c)), key=lambda c: (place[c], ord(c)))
    known = set(ranked)
    rest = [chr(point) for point in [*range(0x3400, 0xA000), *range(0x20000, 0x32000)]
            if ideograph(chr(point)) and chr(point) not in known and traditional(chr(point))]
    pathlib.Path(sys.argv[4]).write_text("".join(c + "\n" for c in ranked) + "\n" + "".join(c + "\n" for c in rest),
                                         encoding="utf-8")
    print(f"{len(ranked) + len(rest)} characters: {len(ranked)} ranked ({len(orders[0])} from Taiwan, "
          f"{len(orders[1])} from Hong Kong, the others outside Big5-HKSCS), then {len(rest)} by code point")


if __name__ == "__main__":
    main()
