#!/usr/bin/env python3
"""List a swipe lexicon's offensive words, for typing suggestions to hold back.

Run on the development host. Typing suggestions rank these words below every
other completion until the user has used them; swipe typing ranks them as the
lexicon does, since a swipe spells the whole word.

A word is listed when the lexicon has it and either:
  - a "bad words" list (LDNOOBW: one entry per line, lower-case; entries with a
    space are phrases and skipped) has it,
  - it begins with one of the language's ROOTS, which every word built on is
    offensive in (fuckface, scheißegal), reviewed by hand, or
  - an AOSP .combined list marks it possibly_offensive and it begins with such
    a bad word of four letters or more, which catches inflections the bad
    words list lacks (fags, fucking). The AOSP marks alone are too broad to use:
    English marks "AI" and "MP", German "Club" and "frisch".

Words in KEEP are never listed: everyday words the bad words lists include
(Italian regina, Portuguese comer, Swedish sås, English hardcore), reviewed by
hand from each language's output. Their inflections are not caught either.

Languages without an LDNOOBW list take --words instead: a file of their own
in tools/offensive_words/, one entry per line, "word", "root*" or "!kept word",
drafted with an AI model, matched against the word list and reviewed for
everyday words.

Output: comment lines, then one word per line as the lexicon spells it.
"""

import argparse
import re
import sys

FLAGGED = re.compile(r"^ ?word=([^,]+), ?f=\d+.*possibly_offensive=true")

ROOTS = {
    "cs": "kurv zkurv prdel hovn zmrd kokot čurák píč",
    "da": "fisse kneppe røvhul",
    "fi": "vittu vitun perkele kusipää runkk mulkk huora",
    "hu": "basz kibasz bazd geci kurv",
    "nb": "fitte faen jævl knull",
    "tr": "siktir sikiş siker sikim sikt amcık orospu yarra",
    "de": "fick fotz wichs scheiß arschloch hurensohn schlampe nutte",
    "en": "fuck cunt nigger nigga motherf shit bitch whore slut wanker wankin twat fagg fags jizz dildo porn "
    "orgasm masturb cocksuck asshole bullshit dickhead blowjob handjob",
    "es": "mierd gilipoll hijoput joder jodid cabron cabrón",
    "fr": "encul connard connass salope salopard saloperie putain merd",
    "it": "cazz stronz vaffancul puttan minchi coglion merd fottut fotter",
    "nl": "klootzak godverdom tering kutwijf",
    "pl": "kurw jeb pierdol chuj pizd skurwy",
    "pt": "caralh fod merd",
    "ru": "хуй хуё хуя бляд пизд ебан ебат ёбан",
    "sv": "knull fitt kuk jävl",
}

KEEP = {
    "cs": "bordel debil",
    "da": "anus",
    "fi": "jätkä muna panna pistää reikä tavara paljaalla hitto piru",
    "fil": "bobo tanga",
    "hu": "marha barom barmok pofa franc francot francokat",
    "nb": "fan sinnsykt pokker fanken skitt forbanna forbannet",
    "tr": "çingene",
    "de": "nackt fratze rosette pinkeln popel lümmel dödel möpse",
    "en": "hardcore fingering snowballing swinger swingers suck sucks sucking sucker suckers cocked "
    "cocking twinkie hooker hooker's escort domination undressing xx butt snatch shiitake",
    "es": "asesinato asno drogas heroína infierno martillo nazi racista trio semental concha orina "
    "sádico idiota maldito maciza caca",
    "fr": "baiser bander bordel bourré bourrée folle gerbe gerber gueule jouir meuf pipi péter trique "
    "zizi caca déconne déconner salopette",
    "it": "regina pesce tirare montare monta battere sbattere sbattersi cadavere quaglia spagnola pompa "
    "palle balle biga fava cozza vacca topa mona patacca pippa pisello finocchio femminuccia cesso porco "
    "porca bagnarsi lecchino ruffiano guardone mannaggia palloso pomiciare ingoio pipì sega bordello cagna stronzio",
    "nl": "anita asbak balen bedonderen bekken belazeren beurt dombo gat griet hol knor matje muts naakt "
    "naakte naaktheid nicht paal palen poot pot remsporen schatje spuiten standje standjes stootje toeter "
    "vergallen watje zaadje zuigen wippen opzouten potverdorie gadverdamme sufferd klojo mafketel lummel "
    "boerenpummel engerd flikken klooien knor viespeuk zuiplap",
    "pl": "jajko syf",
    "pt": "aborto amador amadoras aranha ariano bissexual burro camisinha cerveja cocaína comer consolo "
    "heroína homosexual inferno lolita lésbica mama saco torneira pinto pau gozar chupar bumbum",
    "ru": "голый офигеть фига хрен",
    "sv": "hård stake sås tusan fittja kuka",
}


def lexicon_words(path):
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("#") or "\t" not in line:
                continue
            yield line.split("\t", 1)[0]


def bad_words(paths):
    words = set()
    for path in paths:
        with open(path, encoding="utf-8") as source:
            for line in source:
                word = line.strip().lower()
                if word and " " not in word:
                    words.add(word)
    return words


def drafted_words(path):
    """Words, roots and kept words from a tools/offensive_words/ file."""
    words, roots, keep = set(), [], set()
    with open(path, encoding="utf-8") as source:
        for line in source:
            entry = line.strip().lower()
            if not entry or entry.startswith("#"):
                continue
            if entry.startswith("!"):
                keep.add(entry[1:])
            elif entry.endswith("*"):
                roots.append(entry[:-1])
            else:
                words.add(entry)
    return words, roots, keep


def flagged_words(path):
    words = set()
    with open(path, encoding="utf-8") as source:
        for line in source:
            match = FLAGGED.match(line)
            if match:
                words.add(match.group(1).lower())
    return words


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("lexicon")
    parser.add_argument("output")
    parser.add_argument("--bad", action="append", default=[], help="LDNOOBW list")
    parser.add_argument("--words", help="tools/offensive_words/ list: word, root* or !kept word")
    parser.add_argument("--flags", help="AOSP .combined list with possibly_offensive marks")
    parser.add_argument(
        "--language", choices=sorted(KEEP.keys() | ROOTS.keys()), help="language whose roots and kept words apply"
    )
    parser.add_argument("--header", action="append", default=[], help="comment line to record")
    args = parser.parse_args()

    keep = set(KEEP.get(args.language, "").split())
    roots = ROOTS.get(args.language, "").split()
    bad = bad_words(args.bad)
    if args.words:
        drafted, drafted_roots, drafted_keep = drafted_words(args.words)
        bad |= drafted
        roots += drafted_roots
        keep |= drafted_keep
    roots = tuple(roots)
    bad -= keep
    # Short stems would catch unrelated words the AOSP marks are loose about.
    stems = sorted((word for word in bad if len(word) >= 4), key=len)
    flagged = flagged_words(args.flags) if args.flags else set()
    listed = []
    seen = set()
    for word in lexicon_words(args.lexicon):
        key = word.lower()
        if key in seen or key in keep:
            continue
        if (key in bad or (roots and key.startswith(roots)) or
                (key in flagged and any(key.startswith(stem) for stem in stems))):
            listed.append(word)
            seen.add(key)
    with open(args.output, "w", encoding="utf-8") as output:
        output.write("# Full Keyboard offensive words: typing suggestions hold them back until used\n")
        for header in args.header:
            output.write(f"# {header}\n")
        for word in sorted(listed, key=str.lower):
            output.write(f"{word}\n")
    print(f"{args.output}: {len(listed)} words", file=sys.stderr)


if __name__ == "__main__":
    main()
