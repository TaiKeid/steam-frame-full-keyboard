#!/usr/bin/env python3
"""Embed the shipped resources so damaged user files cannot remove the safe defaults.

embed_defaults.py ROOT OUTPUT.cpp

Every bundled layout, typing language, theme and interface language, and the emoji table, is
compressed with zlib and written into OUTPUT.cpp as a byte array, with the size it unzips to; the
keyboard unzips one when it needs it (src/bundled.cpp, R436). No bundled text is a string literal,
so none can pass a compiler's limit for one.

An interface language also gets an uncompressed head: its fields without its strings, from which
the loader learns every language's tag, names and system codes without unzipping its texts. The
`sources` section, the translation check's fingerprints, is left out: nothing reads it at run time.
"""
import json
from pathlib import Path
import sys
import zlib

root, output = map(Path, sys.argv[1:])


def bytes_array(name, data):
    rows = [', '.join(str(b) for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    return [f'const unsigned char {name}[] = {{'] + ['    ' + row + ',' for row in rows] + ['};']


def literal(text):
    """A short UTF-8 text as a C++ string_view, its length given."""
    return f'std::string_view({json.dumps(text, ensure_ascii=False)}, {len(text.encode())})'


arrays, entries, heads = [], [], []
resources = []
for kind in ('layouts', 'languages', 'themes', 'locales'):
    for path in sorted((root / kind).glob('*.json')):
        value = json.loads(path.read_text(encoding='utf-8'))
        if kind == 'locales':
            value.pop('sources', None)
            head = {key: field for key, field in value.items() if key != 'strings'}
            heads.append((len(resources), json.dumps(head, ensure_ascii=False)))
        # The same text the keyboard parsed when it was a literal: json.dumps of the file.
        resources.append((kind, path.stem, json.dumps(value, ensure_ascii=False).encode()))
# The emoji picker's table, see tools/emoji_table.py.
resources.append(('emoji', 'emoji', (root / 'emoji' / 'emoji.tsv').read_bytes()))

for index, (kind, name, text) in enumerate(resources):
    packed = zlib.compress(text, 9)
    arrays += bytes_array(f'resource_{index}', packed)
    entries.append(f'    {{{json.dumps(kind)}, {json.dumps(name)}, resource_{index}, {len(packed)}, '
                   f'{len(text)}}},')

lines = ['// Generated from the bundled resources by tools/embed_defaults.py; do not edit.',
         '#include "framekeyboard/bundled.hpp"', '#include <string_view>',
         'namespace framekeyboard::bundled {', 'namespace {'] + arrays + ['} // namespace',
         'extern const Resource resource_table[] = {'] + entries + ['};',
         f'extern const std::size_t resource_count = {len(resources)};',
         'extern const LocaleHead locale_head_table[] = {']
for index, head in heads:
    lines.append(f'    {{{literal(head)}, {index}}},')
lines += ['};', f'extern const std::size_t locale_head_count = {len(heads)};', '} // namespace framekeyboard::bundled']
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
