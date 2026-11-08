#!/usr/bin/env python3
"""tools/check_locales.py on small made-up trees: what it accepts, what it reports."""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('check_locales', sys.argv[1])
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

ENGLISH = {'locale': 'en', 'plural': {'one': 'i = 1 and v = 0'}, 'strings': {
    'a.hello': 'Hello {name}',
    'a.count': {'by': 'count', 'one': '{count} item', 'other': '{count} items'},
}}
CONTEXT = {'a.hello': {'where': 'status line', 'about': 'a greeting'},
           'a.count': {'where': 'Settings', 'about': 'how many'}}
TESTISH = {'locale': 'xt', 'plural': {'one': 'i = 1', 'few': 'i = 2..4'}, 'strings': {
    'a.hello': 'Hej {name}',
    'a.count': {'by': 'count', 'one': 'one item', 'few': '{count} itemy', 'other': '{count} itemow'},
}}
CODE = 'status_ = tr("a.hello", {{"name", n}});\nreturn ui("a.count", {{"count", 2LL}});\n'


def tree(folder, english=ENGLISH, context=CONTEXT, translations=(TESTISH,), code=CODE):
    """A repository in folder/repo, and its translator notes beside it in
    folder/context.json, as they are kept outside the repository."""
    root = Path(folder) / 'repo'
    (root / 'locales').mkdir(parents=True)
    (root / 'src').mkdir()
    (root / 'locales' / 'en.json').write_text(json.dumps(english))
    (Path(folder) / 'context.json').write_text(json.dumps(context))
    for data in translations:
        (root / 'locales' / f'{data["locale"].lower()}.json').write_text(json.dumps(data, ensure_ascii=False))
    (root / 'src' / 'app.cpp').write_text(code)
    return root


def expect(condition, message):
    if not condition:
        raise SystemExit(message)


def has(messages, text):
    return any(text in message for message in messages)


with tempfile.TemporaryDirectory() as folder:
    root = tree(folder)
    errors, warnings = tool.check(root)
    expect(not errors, f'a complete tree has no errors: {errors}')
    expect(not tool.check(root, context_path=Path(folder) / 'context.json')[0],
           'and its notes cover every key')
    expect(has(warnings, 'never accepted'), 'a translation not yet accepted is a warning')
    expect(tool.check(root, release=True)[0], 'and an error for a release')
    tool.accept(root, 'xt')
    errors, warnings = tool.check(root, release=True)
    expect(not errors and not warnings, f'accepted, it is clean: {errors} {warnings}')
    english = json.loads((root / 'locales' / 'en.json').read_text())
    english['strings']['a.hello'] = 'Hi there, {name}'
    (root / 'locales' / 'en.json').write_text(json.dumps(english))
    errors, warnings = tool.check(root)
    expect(not errors and has(warnings, 'a.hello was translated from other English text'),
           'changed English makes the translation stale')

with tempfile.TemporaryDirectory() as folder:
    broken = json.loads(json.dumps(TESTISH))
    broken['strings']['a.hello'] = 'Hej {nmae}'
    broken['strings']['a.count'] = {'by': 'count', 'one': 'one', 'other': 'many'}
    broken['strings']['a.extra'] = 'x'
    context = dict(CONTEXT, **{'a.gone': {'where': 'x', 'about': 'y'}})
    del context['a.count']
    root = tree(folder, context=context, translations=(broken,),
                code=CODE + 'tr("a.missing");\n')
    errors, _ = tool.check(root)
    expect(not has(errors, 'context.json'), f'without --context the notes are not checked: {errors}')
    errors, _ = tool.check(root, context_path=Path(folder) / 'context.json')
    for text in ('en.json lacks a.missing', 'context.json: a.count needs', 'context.json: a.gone is no English key',
                 'xt.json: a.extra is no English key', 'a.hello uses nmae', 'a.hello leaves out name',
                 'a.count: plural forms missing: few'):
        expect(has(errors, text), f'reported: {text} in {errors}')
    expect(not has(errors, 'a.count leaves out count'), 'a count branch may leave the count out')

with tempfile.TemporaryDirectory() as folder:
    wrong = dict(TESTISH, locale='xu')
    shapeless = dict(TESTISH, locale='xv', strings=dict(TESTISH['strings'], **{'a.count': {'one': 'x'}}))
    missing = dict(TESTISH, locale='xw', strings={'a.hello': 'Hej {name}'})
    root = tree(folder, translations=(shapeless, missing))
    (root / 'locales' / 'xt.json').write_text(json.dumps(wrong))
    (root / 'locales' / 'damaged.json').write_text('{ no')
    errors, _ = tool.check(root)
    for text in ('xt.json: "locale" must be', 'xv.json: a.count: a branch needs', 'xw.json: missing a.count',
                 'damaged.json'):
        expect(has(errors, text), f'reported: {text} in {errors}')

print('check_locales: accepted trees, stale translations, missing keys, placeholders, plurals and damage checked')
