#!/usr/bin/env python3
"""The swipe lexicon builder's sentence-initial fold and the shipped Spanish
list: what folds at each threshold, that the blend uses the same threshold, and
that the article, the accented pronoun and names come out as intended."""
import importlib.util
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('build_swipe_lexicon', sys.argv[1])
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def expect(condition, message):
    if not condition:
        raise SystemExit(message)


def fold(entries, steps):
    return tool.fold_initial_capitals(entries, steps)


# Spanish: "El" is one step above "el", so one step folds it while names stay.
expect(fold({'El': 213, 'el': 212, 'él': 171}, 1) == {'el': 213, 'él': 171},
       'a one-step threshold folds El into el and keeps the accented él')
expect(fold({'Cruz': 145, 'cruz': 130}, 1) == {'Cruz': 145, 'cruz': 130},
       'a one-step threshold keeps a name that is 15 steps above its lowercase')
expect(fold({'Cruz': 145, 'cruz': 130}, 20) == {'cruz': 145},
       'the default threshold folds the same name, as the shipped lists do')

# Sentence-initial capitals below their lowercase word fold at any threshold.
expect(fold({'La': 208, 'la': 216}, 0) == {'la': 216},
       'a capital below its lowercase form folds')
# Names far above their lowercase form stay capitalised.
expect(fold({'Warszawa': 129, 'warszawa': 75}, 20) == {'Warszawa': 129, 'warszawa': 75},
       'a name far above its lowercase form stays capitalised')
# A capital with no lowercase entry has nothing to fold into.
expect(fold({'María': 100}, 20) == {'María': 100},
       'a word without a lowercase form stays as it is')
# Accented initials fold too.
expect(fold({'Él': 100, 'él': 99}, 1) == {'él': 100},
       'an accented initial folds')

# The blend applies the same threshold to a listed sentence-initial capital.
with tempfile.TemporaryDirectory() as folder:
    subtitles = Path(folder) / 'subtitles.txt'
    subtitles.write_text('hoi 1000\nzwei 1000\n', encoding='utf-8')
    entries = {'Hoi': 160, 'Zwei': 145, 'zwei': 80}
    near = tool.blend_subtitles(dict(entries), subtitles, True, None, 8)
    expect('hoi' in near and 'Hoi' not in near,
           'the blend folds a capital within the threshold it is given')
    far = tool.blend_subtitles(dict(entries), subtitles, True, None, 7)
    expect('Hoi' in far and 'hoi' not in far,
           'the blend keeps a capital outside the threshold it is given')

# The shipped Spanish list carries the fold, and keeps the accented pronoun and
# names. The runtime keeps one spelling per key, the commonest, so "Cruz" wins
# over "cruz" only while it is still listed.
spanish = Path(sys.argv[1]).resolve().parent.parent / 'swipe/lexicons/es-es.tsv'
words = {}
for line in spanish.read_text(encoding='utf-8').splitlines():
    if line and not line.startswith('#'):
        word, frequency = line.split('\t')
        words[word] = int(frequency)
expect('El' not in words, 'the shipped Spanish list has no sentence-initial El')
expect(words.get('el', 0) > words.get('él', 0),
       'the article el is listed and outranks the accented pronoun')
expect('él' in words, 'the accented pronoun stays in the list')
expect(max(('Cruz', 'cruz'), key=lambda word: words.get(word, -1)) == 'Cruz',
       'the name Cruz stays the spelling its key offers')

print('swipe lexicon tool tests passed')
