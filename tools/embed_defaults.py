#!/usr/bin/env python3
"""Embed shipped JSON profiles so damaged user files cannot remove the safe defaults."""
import json
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])


def literal(text):
    # With its byte length given, Clang needn't count the characters while
    # evaluating the table, which exceeds its constexpr step limit.
    return f'std::string_view({json.dumps(text, ensure_ascii=False)}, {len(text.encode())})'
lines = ['// Generated from bundled JSON; do not edit.', '#pragma once',
         '#include <string_view>', 'namespace framekeyboard::bundled {',
         'struct Resource { std::string_view kind, json; };',
         'inline constexpr Resource profiles[] = {']
for kind in ('layouts', 'languages', 'themes', 'locales'):
    for path in sorted((root / kind).glob('*.json')):
        # Encode as a C++ string, preserving UTF-8 while escaping quotes and controls.
        content = json.dumps(json.loads(path.read_text()), ensure_ascii=False)
        lines.append('{' + json.dumps(kind) + ', ' + literal(content) + '},')
lines.append('};')
# The emoji picker's table, see tools/emoji_table.py.
table = (root / 'emoji' / 'emoji.tsv').read_text(encoding='utf-8')
lines.append('inline constexpr std::string_view emoji_table = ' + literal(table) + ';')
lines.append('}')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(lines) + '\n')
