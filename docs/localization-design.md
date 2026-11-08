# Interface localization design

Status: design, not built. It covers the keyboard's own interface text: Settings, popups, status messages, tooltips and dialogs. Keyboard layouts and key legends already follow the typing language and stay as they are ([configuration](configuration.md#languages-and-actual-input): the interface language is separate from the typing language).

## Goals

- No interface text in the code. Every string lives in a per-language JSON file, English included, loaded like layouts and themes and editable the same way.
- The interface follows the system language. A language without a file falls back to English, and a string missing from a file falls back to its English text.
- Development builds can switch the interface language from the Debug card for the current run, without saving it.
- Plurals, placeholders and number formats are data in each language's file, so adding a language needs no code.
- Light on the Frame: two small tables in memory (the chosen language and English), no per-frame work.

First languages, by Steam's language survey of October 2026 (about 97% of Steam users): English, Simplified Chinese, Russian, Spanish, Brazilian Portuguese, German, Japanese, French, Polish, Korean, Turkish, Traditional Chinese, Ukrainian and Italian. Thai and Czech next; Latin American Spanish and European Portuguese later, as variants.

## Files

```
<data dir>/locales/en.json         bundled, the source of every key
<data dir>/locales/ru.json         bundled translations
<data dir>/locales/context.json    translator notes per key; not loaded by the app
<config dir>/locales/*.json        a user's own files: added languages, or overrides
```

A user file with the same `locale` as a bundled one replaces it key by key. Reload config reloads them.

```json
{
  "locale": "ru",
  "name": "Russian",
  "native_name": "Русский",
  "steam": ["russian"],
  "posix": ["ru"],
  "font_families": ["Noto Sans", "sans-serif"],
  "plural": {
    "one": "v = 0 and i % 10 = 1 and i % 100 != 11",
    "few": "v = 0 and i % 10 = 2..4 and i % 100 != 12..14",
    "many": "v = 0 and i % 10 = 0 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 11..14"
  },
  "number": {"group": " ", "decimal": ","},
  "strings": {
    "setting.typing-suggestions": "Подсказки при вводе",
    "status.wait-for-typing": "Дождитесь окончания ввода и выберите снова.",
    "learned.count": {"by": "count", "one": "{count} слово", "few": "{count} слова", "many": "{count} слов", "other": "{count} слова"},
    "status.feature-switched": {"by": "state", "on": "{feature}: включено", "off": "{feature}: выключено"}
  },
  "sources": {"setting.typing-suggestions": "3f9a01c2"}
}
```

- `locale`: a BCP 47 tag (`en`, `de`, `pt-BR`, `zh-Hans`, `zh-Hant`). File names are the tag in lower case.
- `steam`: Steam's own language names that select this file (`schinese`, `tchinese`, `koreana`, `brazilian`, `latam`).
- `posix`: language prefixes of `LANG` that select it (`zh_CN`, `zh_TW`, `pt_BR`, `ru`). The longest match wins.
- `font_families`: the fonts for interface text in this language (below).
- `plural`: CLDR plural rules, one per category the language uses; `other` needs none. Only integer counts appear in the interface, so the operands are `n`, `i` and `v`, with `v = 0`.
- `number`: digit grouping and decimal marks for counts such as 5,000.
- `strings`: key to text, or key to a branch (below).
- `sources`: for each translated key, the first 8 hex digits of the SHA-256 of the English text it was translated from, written by the check tool when a translation is accepted. A mismatch marks the translation stale.

## Keys

Dotted, lower case, area first: `setting.typing-suggestions`, `setting.typing-suggestions.note`, `status.wait-for-typing`, `update.network-unavailable`, `tooltip.focus-mode`, `popup.languages.title`. Settings rows already carry stable IDs such as `typing-suggestions`; their keys reuse them. A key keeps one meaning for life: a new meaning gets a new key, so no stale translation shows under a changed sentence.

Every string is a whole sentence or label. Text built from pieces in code (`"Steam dashboard (" + name + ")"`) becomes one string with a placeholder, since word order differs between languages. Manual line breaks are removed where the panel can wrap.

## Templates and branches

Strings are templates with named placeholders: `"{feature} is on for {language}"`. Each language puts them where its word order needs them; `{{` and `}}` are literal braces. Every translation uses exactly the English placeholders. Numbers are formatted with the language's `number` marks.

Where one template can't serve every case, a string branches on one argument:

```json
"learned.count": {"by": "count", "one": "{count} word", "other": "{count} words"},
"status.feature-switched": {"by": "state", "on": "{feature} is on", "off": "{feature} is off"}
```

- `by` names the argument. A number chooses by the language's plural categories (`zero`, `one`, `two`, `few`, `many`, `other`); any other value chooses the option with that name.
- `other` is the fallback for both, and a number branch must have every category the language's `plural` rules can select.
- An option can itself be a branch, for text that depends on two arguments.
- A language may branch where English doesn't: a translation can be an object where English has plain text, with the English placeholders as its `by` arguments.
- Inserted names, such as a language or a feature, keep their dictionary form, since Russian, Polish or Ukrainian would decline them in a sentence. Phrasing such as "{feature}: on" avoids that, and the style guide says so.

## Choosing the language

```
Debug card dropdown (development builds, this run only, never saved; languages by English name)
  └─ else the system language:
       Steam's own language (SteamOS's system language), from ~/.steam/registry.vdf
       └─ else LC_ALL, LC_MESSAGES, LANG
  └─ a file whose `steam` or `posix` names it
       └─ else its base language (pt-BR → pt, es-419 → es)
            └─ else English
Each key: the chosen file → its base language's file → English
```

On SteamOS the system language is Steam's own language setting; the session's `LANG` may stay English. The keyboard reads Steam's value when it starts and again whenever Settings opens (one `stat`, and a read only when the file changed), so a language change shows up without a restart. Hosted mode uses the same detection; a shell protocol field for the shell's own language can come later.

Bundled files must be complete: the check fails on a missing key. User files may be partial; their missing keys show in English.

## Fonts

Interface text uses the interface language's `font_families`, while key legends and candidates keep their keyboard language's fonts. Today Settings and status text use the keyboard language's fonts. With Chinese interface text while typing English, that font list has no Han font, so fontconfig picks one, often a Japanese one, and Chinese readers get Japanese character forms. So `zh-Hans` names Noto Sans CJK SC, `zh-Hant` TC, `ja` JP and `ko` KR. Thai, when added, needs Noto Sans Thai and Pango's Thai line breaking, to be checked on the Frame first.

## In the code

- `Localization` (`include/framekeyboard/localization.hpp`) loads files, resolves the chain, evaluates plural rules, formats placeholders and numbers, and reports the interface fonts. App owns it; it is read on the VR thread only.
- App resolves text when it builds what the panel shows. The panel draws strings and never looks up keys.
- Modules that produce messages for the user (updates, output, dictation, native replacement) return a key and its arguments, not text. App localizes them when it shows them, so worker threads never touch the tables.
- Switching language rebuilds everything that holds text: Settings cards, popups, tooltips and the status line. Chip and label measurements are keyed on the text and follow by themselves.

Only text the user sees in the interface is translated. Never translated: log lines, command-line help and errors, and other technical output; the Debug card, whose language dropdown lists languages by English name; product and protocol names (Full Keyboard, Steam, Steam Frame, SteamOS, Moonlight, Mozc, libei); key legends, which follow the layout; the typing-language list in Settings, which keeps each language's English and native names, since speakers of a language recognize them; error text that comes from a server, shown after a translated lead-in.

## Fitting the text

German and Russian run about 30% longer than English; Chinese, Japanese and Korean are shorter but need taller lines. Text that doesn't fit shrinks to a smaller size, or wraps onto another line where its element has room and it still looks right. It is never cut off with "…". The order and limits (how far to shrink before wrapping, the smallest size) are settled on screenshots with the owner during step 2.

Development builds record every interface label the renderer shrank or wrapped, and any it had to cut, with its key. The simulator tour renders every screen in every bundled language. It fails on a cut label, and reports shrunk and wrapped ones with screenshots for review. A fix is a shorter translation, more room or a layout change.

Before any translation, a generated pseudo-language (`qps`: accented, 40% longer, in brackets) runs through the same tour. Any interface text without brackets did not go through `Localization`, and any overflow shows where English has no slack. The Debug card's dropdown offers it too, for a look in the headset.

## Checks

- Unit tests: loading and merging, plural rules (including Russian, Polish and Ukrainian 1, 2, 5, 11, 21, 22), placeholders, number marks, every step of the fallback chain, the font choice.
- `tools/check_locales.py`, run by a host test: every key used in the code exists in `en.json` and `context.json`; every bundled file has every key, the same placeholders as English, and the plural forms its rules need; it lists stale translations (`sources`) and keys no code uses.
- The tour in `qps` and in every bundled language: no cut interface label; shrunk and wrapped labels listed with screenshots; in `qps` no interface text without brackets.
- Before each release: no stale or missing translation in bundled files.

## Translator context

`locales/context.json` has, for every key: where it shows (screen and element), what it is about (the feature, and when the user sees it), roughly how much room it has, and terms that stay as they are. Each language also gets a short style guide (formal or informal address, how the interface addresses the user, punctuation) and a glossary. The glossary keeps terms consistent, in line with the terms Steam itself uses in that language, without copying Steam's sentences. Simulator screenshots of each screen go with the notes, and each language is reviewed again on screenshots of its own text in place.

## Steps

1. The framework: `Localization`, file format and loader, plural rules, placeholders, numbers, fonts, language detection, the Debug card dropdown, live switching, the overflow recorder, `qps`, the check tool and their tests. The interface still shows only English.
2. English into `en.json`: every interface string gets a key and a note in `context.json`; pieced-together text becomes templates. Rendering in English stays as it was (tour screenshots compared before and after). Then the `qps` tour, with its screenshots reviewed with the owner to settle shrinking and wrapping.
3. Translations, one language at a time, in batches with their notes and screenshots; each checked by the tool and the tour, reviewed in place, and committed on its own.
4. Docs: the format in [configuration](configuration.md), a short guide for contributing a language, the changelog, the README's list of languages, and a note in the README's AI-generated section that the translations are AI-made and corrections are welcome (README.ja.md too).
