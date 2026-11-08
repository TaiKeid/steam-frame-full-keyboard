#!/usr/bin/env python3
"""Check the interface language files (docs/localization-design.md).

check_locales.py ROOT [--release] [--accept TAG]

Errors (exit 1): a key the code uses that en.json lacks; an English key
without translator notes in context.json, or notes for no key; a bundled
translation missing a key, having one English doesn't, using placeholders
English doesn't, or missing a plural form its rules can pick; a damaged file.

Warnings: translations made from English text that has changed since
(`sources`), or never accepted; keys no code uses. --release turns stale
translations into errors.

--accept TAG records, for every key TAG translates, which English text it
was made from, after the translation was checked.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

PLURAL = ('zero', 'one', 'two', 'few', 'many', 'other')
KEY_USE = re.compile(r'\b(?:tr|ui)\(\s*"([a-z][a-z0-9.-]*)"')
PLACEHOLDER = re.compile(r'(?<!\{)\{([A-Za-z_][A-Za-z0-9_]*)\}(?!\})')
TAG = re.compile(r'^[A-Za-z][A-Za-z0-9-]{1,15}$')


def texts(node):
    """Every text of a string or branch."""
    if isinstance(node, str):
        yield node
    else:
        for name, option in node.items():
            if name != 'by':
                yield from texts(option)


def placeholders(node):
    return {name for text in texts(node) for name in PLACEHOLDER.findall(text.replace('{{', '').replace('}}', ''))}


def branch_args(node):
    if isinstance(node, str):
        return set()
    result = {node['by']}
    for name, option in node.items():
        if name != 'by':
            result |= branch_args(option)
    return result


def shape_errors(node, where):
    if isinstance(node, str):
        return []
    if not isinstance(node, dict) or not isinstance(node.get('by'), str) or 'other' not in node:
        return [f'{where}: a branch needs "by" and "other"']
    errors = []
    for name, option in node.items():
        if name != 'by':
            errors += shape_errors(option, f'{where}/{name}')
    return errors


def plural_errors(node, needed, where):
    """Branches over a count need every category the language's rules pick."""
    if isinstance(node, str):
        return []
    errors = []
    names = set(node) - {'by'}
    if names & set(PLURAL[:-1]) and not needed <= names:
        errors.append(f'{where}: plural forms missing: {", ".join(sorted(needed - names))}')
    for name in names:
        errors += plural_errors(node[name], needed, f'{where}/{name}')
    return errors


def source_hash(node):
    text = node if isinstance(node, str) else json.dumps(node, sort_keys=True, ensure_ascii=False)
    return hashlib.sha256(text.encode()).hexdigest()[:8]


def load(path, errors):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        errors.append(f'{path.name}: {error}')
        return None
    if not isinstance(data, dict):
        errors.append(f'{path.name}: expected an object')
        return None
    return data


def check(root, release=False):
    errors, warnings = [], []
    folder = root / 'locales'
    english = load(folder / 'en.json', errors) or {}
    source = english.get('strings', {})
    for key, node in source.items():
        errors += shape_errors(node, f'en.json: {key}')

    used = set()
    for path in sorted([*root.glob('src/*.cpp'), *root.glob('include/framekeyboard/*.hpp')]):
        used |= set(KEY_USE.findall(path.read_text(encoding='utf-8', errors='replace')))
    errors += [f'en.json lacks {key}, used in the code' for key in sorted(used - set(source))]
    warnings += [f'{key} is in en.json but no code uses it' for key in sorted(set(source) - used)]

    context = load(folder / 'context.json', errors) or {}
    for key in sorted(source):
        note = context.get(key)
        if not isinstance(note, dict) or not note.get('where') or not note.get('about'):
            errors.append(f'context.json: {key} needs "where" and "about"')
    errors += [f'context.json: {key} is no English key' for key in sorted(set(context) - set(source))]

    for path in sorted(folder.glob('*.json')):
        if path.name in ('en.json', 'context.json'):
            continue
        data = load(path, errors)
        if data is None:
            continue
        name = path.name
        tag = data.get('locale', '')
        if not TAG.match(tag) or path.stem != tag.lower():
            errors.append(f'{name}: "locale" must be the file name\'s tag')
        strings = data.get('strings', {})
        needed = set(data.get('plural', {})) | {'other'}
        sources = data.get('sources', {})
        errors += [f'{name}: missing {key}' for key in sorted(set(source) - set(strings))]
        errors += [f'{name}: {key} is no English key' for key in sorted(set(strings) - set(source))]
        for key in sorted(set(strings) & set(source)):
            node = strings[key]
            shape = shape_errors(node, f'{name}: {key}')
            errors += shape
            if shape:
                continue
            english_args = placeholders(source[key]) | branch_args(source[key])
            extra = (placeholders(node) | branch_args(node)) - english_args
            missing = placeholders(source[key]) - placeholders(node) - branch_args(node)
            if extra:
                errors.append(f'{name}: {key} uses {", ".join(sorted(extra))}, which English doesn\'t give')
            if missing:
                errors.append(f'{name}: {key} leaves out {", ".join(sorted(missing))}')
            errors += plural_errors(node, needed, f'{name}: {key}')
            if sources.get(key) != source_hash(source[key]):
                message = (f'{name}: {key} was translated from other English text' if key in sources
                           else f'{name}: {key} was never accepted')
                (errors if release else warnings).append(message)
    return errors, warnings


def accept(root, tag):
    path = root / 'locales' / f'{tag.lower()}.json'
    english = json.loads((root / 'locales' / 'en.json').read_text(encoding='utf-8'))['strings']
    data = json.loads(path.read_text(encoding='utf-8'))
    data['sources'] = {key: source_hash(english[key]) for key in sorted(data.get('strings', {}))
                       if key in english}
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main(argv):
    args = argv[1:]
    release = '--release' in args
    tag = None
    if '--accept' in args:
        tag = args[args.index('--accept') + 1]
        args = [a for a in args if a not in ('--accept', tag)]
    args = [a for a in args if a != '--release']
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(args[0])
    errors, warnings = check(root, release)
    if tag and not errors:
        accept(root, tag)
    for message in warnings:
        print('warning:', message)
    for message in errors:
        print('error:', message)
    print(f'{len(errors)} errors, {len(warnings)} warnings')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
