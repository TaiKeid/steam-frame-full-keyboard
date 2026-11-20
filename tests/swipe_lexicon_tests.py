#!/usr/bin/env python3
"""The swipe lexicon builder's sentence-initial folds and the shipped Spanish
and English lists: what folds at each threshold, that the blend uses the same
threshold, that English folds before the blend so the subtitle count raises
the lowercase word, and that articles, pronouns and names come out as
intended."""
import importlib.util
import subprocess
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

# English folds on the written list, before the blend: the subtitle lists are
# lowercase, and the blend raises whichever case variant is listed higher.
expect(tool.fold_written_capitals({'He': 193, 'he': 187, 'John': 147, 'john': 58}, 26)
       == {'he': 193, 'John': 147, 'john': 58},
       'a capital a few steps above its lowercase folds; a name far above stays')
expect(tool.fold_written_capitals({'House': 140, 'house': 152}, 26) == {'House': 140, 'house': 152},
       'a capital below its lowercase stays listed: the raise already goes to the lowercase')
expect(tool.fold_written_capitals({'Our': 167, 'our': 141}, 25) == {'Our': 167, 'our': 141},
       'a capital one step beyond the threshold stays')


def build(folder, *options):
    """Runs the builder on a small written list and subtitle list."""
    source, subtitles, output = (Path(folder) / name for name in ('written.tsv', 'subtitles.txt', 'out.tsv'))
    source.write_text('He\t193\nhe\t187\nJohn\t147\njohn\t58\nthe\t222\n', encoding='utf-8')
    subtitles.write_text('the 1000000\nhe 400000\njohn 2000\n', encoding='utf-8')
    subprocess.run([sys.executable, '-I', sys.argv[1], str(source), str(output), '--format', 'tsv',
                    '--blend', str(subtitles), *options], check=True, capture_output=True)
    return tool.from_tsv(output)


with tempfile.TemporaryDirectory() as folder:
    plain = build(folder)
    expect(plain.get('He', 0) > 193 and plain['he'] == 187 and 'John' in plain,
           'by default the subtitle count for he raises He, as the other lists are built')
    folded = build(folder, '--fold-written-capitals', '26')
    expect('He' not in folded and folded['he'] == plain['He'],
           'with the fold, he takes the subtitle raise He used to get')
    expect(folded['John'] == plain['John'] and folded['john'] == 58,
           'a name keeps the raise and its lowercase twin stays as it was')

# Rare words crowd at the bottom of the written scale, so a rare name can sit
# close to a lowercase twin that is only noise. With the floors, a capital folds
# only if its lowercase word is common in its own right: listed high enough in
# the written list, or spoken often enough in the subtitles.
expect(tool.fold_written_capitals({'Wikipedia': 98, 'wikipedia': 78, 'He': 193, 'he': 187}, 26,
                                  lambda lower: lower == 'he')
       == {'Wikipedia': 98, 'wikipedia': 78, 'he': 193},
       'a capital whose lowercase twin is not common stays')
with tempfile.TemporaryDirectory() as folder:
    source, subtitles, output = (Path(folder) / name for name in ('written.tsv', 'subtitles.txt', 'out.tsv'))
    # Written: "he" common, "hi" rare but spoken, "bianca" rare and rarely spoken.
    source.write_text('the\t222\nHe\t193\nhe\t187\nHi\t93\nhi\t77\nBianca\t74\nbianca\t50\n', encoding='utf-8')
    subtitles.write_text('the 1000000\nhe 400000\nhi 300000\nbianca 20\n', encoding='utf-8')

    def floors(*options):
        subprocess.run([sys.executable, '-I', sys.argv[1], str(source), str(output), '--format', 'tsv',
                        '--blend', str(subtitles), '--fold-written-capitals', '26', *options],
                       check=True, capture_output=True)
        return tool.from_tsv(output)
    common = floors('--fold-written-floor', '80', '--fold-written-spoken-floor', '200')
    expect('He' not in common and 'Hi' in common and 'Bianca' in common,
           'a lowercase word listed above the written floor folds its capital; others stay')
    spoken = floors('--fold-written-floor', '80', '--fold-written-spoken-floor', '150')
    expect('He' not in spoken and 'Hi' not in spoken and spoken['hi'] > 93 and 'Bianca' in spoken,
           'a lowercase word the subtitles give the spoken floor folds too, and takes the raise')
    everything = floors()
    expect(not {'He', 'Hi', 'Bianca'} & set(everything), 'without floors every capital in the window folds')

# A list's always-fold words fold however far their capital is above them;
# without the option nothing changes.
expect(tool.always_fold({'Hi': 158, 'hi': 77, 'Hill': 138}, ('Hi',)) == {'hi': 158, 'Hill': 138},
       'a named capital folds into its lowercase word, which takes its frequency')
expect(tool.always_fold({'Sir': 92, 'sir': 167}, ('Sir',)) == {'sir': 167}, 'the lowercase word keeps a higher one')
expect(tool.always_fold({'Hi': 158}, ()) == {'Hi': 158}, 'nothing named, nothing folded')
expect(set(tool.ALWAYS_FOLD) == {'en-us', 'en-gb'}, 'only the English lists use it so far')

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

lexicons = Path(sys.argv[1]).resolve().parent.parent / 'swipe/lexicons'


def offered(name):
    """Each key's spelling as the runtime offers it: the commonest."""
    best = {}
    for word, frequency in tool.from_tsv(lexicons / name).items():
        key = word.casefold()
        if key not in best or frequency > best[key][1]:
            best[key] = (word, frequency)
    return {key: word for key, (word, _) in best.items()}


# The shipped English lists offer common words in lowercase mid-sentence,
# and names capitalized.
for name, lowercase, names in (
        ('en-us.tsv', "he no so he's here's new may hi sir",
         'John York American English August Trump Wikipedia Bianca Hanuman Seine Scrabble Petra Pasteur'),
        ('en-gb.tsv', 'how our new may hi sir',
         'John York American English August Wikipedia Bianca Lakshmi Scrabble Petra Pasteur')):
    spellings = offered(name)
    for word in lowercase.split():
        expect(spellings.get(word) == word, f'{name} offers {word} in lowercase, not {spellings.get(word)}')
    for word in names.split():
        expect(spellings.get(word.casefold()) == word, f'{name} keeps {word} capitalized')

# tools/build_offensive_words.py lists each word as its word list spells it. A
# word list rebuilt with a fold needs its offensive list rebuilt too: the fold
# changes spellings and lets new words in at the end of the list.
for offensive in sorted(lexicons.glob('*-offensive.tsv')):
    words = tool.from_tsv(lexicons / offensive.name.replace('-offensive', ''))
    stale = [line for line in offensive.read_text(encoding='utf-8').splitlines()
             if line and not line.startswith('#') and line not in words]
    expect(not stale, f'{offensive.name} spells every word as its word list does: {stale[:5]}')

print('swipe lexicon tool tests passed')
