# Languages and system keyboard settings

The typing language is an app setting, separate from SteamOS's interface locale.
A fresh installation starts in English US. After Apply and save, the keyboard
reopens with the last saved language, layout and theme. A Russian system locale
does not automatically select Russian. Updating the app preserves your selection.

## Included profiles

| Language | Profile ID | Layout | Character mapping |
| --- | --- | --- | --- |
| English US | `en-us` | US QWERTY | US XKB |
| English UK | `en-gb` | UK QWERTY | UK XKB |
| English US, Dvorak | `en-us-dvorak` | US QWERTY | US Dvorak XKB; legends and swipe follow it |
| English US, Colemak | `en-us-colemak` | US QWERTY | US Colemak XKB; legends and swipe follow it |
| English US, Colemak-DH | `en-us-colemak-dh` | US QWERTY | US Colemak-DH XKB (ANSI); legends and swipe follow it |
| English US, International | `en-us-intl` | US QWERTY | US international, accents on dead keys |
| Bulgarian | `bg-bg` | US QWERTY | Bulgarian BDS |
| Bulgarian, phonetic | `bg-bg-phonetic` | US QWERTY | Bulgarian new phonetic |
| Bulgarian, traditional phonetic | `bg-bg-traditional` | US QWERTY | Bulgarian traditional phonetic |
| Portuguese, Portugal | `pt-pt` | International | Portuguese XKB |
| German | `de-de` | International | German XKB |
| French | `fr-fr` | International | French AZERTY, France |
| French, QWERTY | `fr-fr-qwerty` | US QWERTY | US international, accents on dead keys |
| French, Belgium | `fr-be` | International | Belgian AZERTY |
| Dutch, Belgium | `nl-be` | International | Belgian AZERTY |
| German, QWERTY | `de-de-qwerty` | US QWERTY | German letters on AltGr |
| Czech, QWERTY | `cs-cz-qwerty` | International | Czech with Y and Z swapped |
| Slovak, QWERTY | `sk-sk-qwerty` | International | Slovak with Y and Z swapped |
| Hungarian, QWERTY | `hu-hu-qwerty` | International | Hungarian with Y and Z swapped |
| Croatian, QWERTY | `hr-hr-qwerty` | US QWERTY | Croatian letters on AltGr |
| Polish, QWERTZ | `pl-pl-qwertz` | International | Polish typewriter arrangement |
| Turkish, F | `tr-tr-f` | International | Turkish F |
| Spanish | `es-es` | International | Spanish, Spain |
| Italian | `it-it` | International | Italian XKB |
| Spanish, Latin America | `es-419` | International | Latin American XKB |
| German, Switzerland | `de-ch` | International | Swiss German |
| French, Switzerland | `fr-ch` | International | Swiss French |
| Catalan | `ca-es` | International | Spanish with Catalan L·L |
| Serbian, Cyrillic | `sr-rs` | US QWERTY | Serbian Cyrillic |
| Serbian, Latin | `sr-rs-latin` | US QWERTY | Serbian Latin |
| Persian | `fa-ir` | US QWERTY | Persian XKB, legends right to left |
| Filipino | `fil-ph` | US QWERTY | US with AltGr accents, ñ on AltGr+N |
| Icelandic | `is-is` | International | Icelandic XKB |
| Georgian | `ka-ge` | US QWERTY | Georgian XKB |
| Armenian | `hy-am` | International | Armenian XKB |
| Kazakh | `kk-kz` | International | Kazakh XKB |
| Azerbaijani | `az-az` | International | Azerbaijani XKB |
| Macedonian | `mk-mk` | International | Macedonian XKB |
| Albanian | `sq-al` | US QWERTY | Albanian QWERTZ |
| Portuguese, Brazil | `pt-br` | Brazilian ABNT2 | Brazilian XKB, including extra slash and keypad separator |
| Russian | `ru-ru` | US QWERTY | Russian ЙЦУКЕН |
| Ukrainian | `uk-ua` | US QWERTY | Ukrainian ЙЦУКЕН |
| French, Canada | `fr-ca` | International | Canadian French, accents on dead keys |
| Arabic | `ar` | International | Arabic XKB, legends right to left |
| Croatian | `hr-hr` | International | Croatian QWERTZ |
| Czech | `cs-cz` | International | Czech QWERTZ, accented letters on the number row |
| Danish | `da-dk` | International | Danish XKB |
| Dutch | `nl-nl` | US QWERTY | US international with AltGr dead keys; ' and " stay plain |
| Estonian | `et-ee` | International | Estonian XKB |
| Finnish | `fi-fi` | International | Finnish XKB |
| Greek | `el-gr` | US QWERTY | Greek XKB, tonos and dialytika dead keys |
| Hebrew | `he-il` | US QWERTY | Hebrew SI-1452, legends right to left |
| Hindi | `hi-in` | International | Devanagari InScript |
| Hungarian | `hu-hu` | International | Hungarian QWERTZ, 0 left of 1 |
| Indonesian | `id-id` | US QWERTY | US XKB, the standard layout there |
| Latvian | `lv-lv` | US QWERTY | Latvian, letters on AltGr |
| Lithuanian | `lt-lt` | US QWERTY | Lithuanian, letters on the number row |
| Malay | `ms-my` | US QWERTY | US XKB, the standard layout there |
| Norwegian | `nb-no` | International | Norwegian XKB |
| Polish | `pl-pl` | US QWERTY | Polish programmer, letters on AltGr |
| Romanian | `ro-ro` | International | Romanian standard, ș and ț with comma below |
| Slovak | `sk-sk` | International | Slovak QWERTZ |
| Slovenian | `sl-si` | International | Slovenian QWERTZ |
| Swedish | `sv-se` | International | Swedish XKB |
| Thai | `th-th` | International | Thai Kedmanee |
| Turkish | `tr-tr` | International | Turkish Q |
| Vietnamese | `vi-vn` | US QWERTY | Vietnamese XKB, tone keys on the number row |
| Simplified Chinese | `zh-cn-pinyin` | US QWERTY | Local Pinyin with candidates |
| Traditional Chinese | `zh-tw-pinyin` | US QWERTY | Local Pinyin with Traditional output |
| Traditional Chinese, Zhuyin | `zh-tw-zhuyin` | US QWERTY | Local Zhuyin with candidates |
| Traditional Chinese, Cangjie | `zh-tw-cangjie` | US QWERTY | Local Cangjie table |
| Traditional Chinese, Quick | `zh-tw-quick` | US QWERTY | Local Quick table |
| Korean | `ko-kr` | US QWERTY | Local two-set Hangul |
| Japanese | `ja-romaji`, `ja-kana`, `ja-jis`, `ja-flick` | US, Japanese JIS, or flick 12-key (`ja-flick` only) | See [Japanese input](japanese.md) |

Open Settings, select Layout, then Language, then Apply and save. Languages
the selected layout cannot type are dimmed, and the open list says so on the
status line; choose a layout with the needed keys first
(for example International or UK for German). Choosing a layout that cannot
type the current language switches to the most likely one for it: an additional
language on that layout, another variant of the same language, the layout's own language, or US
English. Theme selection is unchanged.

Settings dims a language on any layout without a key it needs. Many listed with
International need the extra key left of Z and are dimmed on US QWERTY; the rest,
such as Czech or Finnish, work there too, without that key's characters. AltGr is the right Alt
key; its legend reads AltGr whenever the language uses it.

Accents on dead keys, such as the Czech caron or the Greek tonos, are typed before the letter,
and Shift may be pressed after the accent for a capital. On the Vietnamese
layout the number-row tone keys add a combining mark after the letter; AltGr
with the same key is a dead tone typed before the letter, which gives the
single precomposed letter that some apps handle better. Indonesian and Malay
use the US layout, which is standard there, and differ in their dictation
language. Dutch uses US international with AltGr dead keys, so an apostrophe
before n or s stays an apostrophe.

Arabic, Persian and Hebrew keys show their legends as they appear in right-to-left text,
so a bracket key shows the bracket it adds to the text. In the dictation
editor, right-to-left text aligns to the right, and the arrow keys move the
cursor the way they point on screen, stepping over a letter together with its
marks, as in Thai or Hindi. The headset's own Noto fonts draw the Arabic,
Hebrew, Devanagari, Thai and Greek legends.

Chinese Wubi, Korean Hanja conversion, Vietnamese Telex and
other Indic scripts are not included.

## Independent layout selection

The normal Frame launcher sends characters as Unicode from the profile selected
in Full Keyboard. Switch languages in Settings and Apply and save. You do not
need to install matching system layouts, change SteamOS's active layout, pass a
launch flag, or restart the app. Shift, Caps Lock, AltGr, dead-key accents and
numpad digits are resolved locally. Legend overrides remain display-only; the
profile's XKB mapping determines the characters.

A Russian system locale still does not choose the initial app language. A fresh
install starts in English; subsequent launches restore your saved selection.
With several system layouts installed, Full Keyboard continues using its own
selected language. System layout-switch hotkeys do not change our selection.

Enter, arrows, Backspace, Tab and shortcuts remain native key events. For Latin
shortcuts, the app reads the compositor's advertised keymap and group to find the
right physical key. For example, the A shown on an AZERTY layout sends Ctrl+A,
not Ctrl+Q. Copy/Paste use the same lookup. A non-Latin target group uses its
Latin physical counterpart when the compositor exposes one. Applications still
define shortcut behavior; a compositor that supplies no usable keymap falls back
to conventional physical Latin positions.

Unicode output needs the Gamescope text connection. If it is unavailable, typing
is disabled with an error instead of falling back to a potentially wrong physical
layout. A failed single-character commit remains queued; release other held keys
and press Enter to retry without submitting a form. Hiding or cancellation clears
queued text. Character repeat starts after 500 ms, repeats at 25 Hz, and stops on
release/cancellation without replaying a backlog.

The developer `--input uinput` backend and Japanese external JIS/system-IME mode
remain physical-key modes. They still require the receiving keymap to match
`--target-language`. Integrated Japanese, Chinese, Korean and the ordinary XKB
profiles use independent Unicode output through the normal Frame launcher.

## Chinese and Korean composition

These profiles compose inside Full Keyboard, then submit Unicode text through
the same Gamescope text connection as the other layouts. English and the local
Japanese, Chinese and Korean modes can be selected in VR without changing the
system keymap or relaunching. The receiving app does not need its own IME enabled.
Shortcut and navigation actions remain native. This still requires a local app
that accepts Gamescope's text delivery.

Traditional Chinese also offers Zhuyin, Cangjie and Quick, whose keys show their
symbols or radicals. Zhuyin uses the standard keyboard, including the digit and
punctuation keys, and converts phrases as you type; finish a syllable with a tone
key, or Space for the first tone. With McBopomofo (below) it works as McBopomofo's
defaults do:

- The reading has a cursor, which starts at the end. ←/→ move it a syllable,
  Home and End to the ends, and none of them leave the reading.
- The candidates are for the phrase before the cursor, which the reading line
  highlights. Space or ↓ starts choosing: the five around the highlighted
  candidate are numbered, Space and ↓/↑ move on, 1–5 or Enter pick, and Esc
  stops. A tap picks at any time.
- A pick replaces that phrase and the reading stays open, with the cursor where
  it was, to fix others. Space never sends: only Enter, outside choosing, sends
  the whole line.
- New syllables and punctuation (Shift+`,` for ，) go in at the cursor and
  convert with the rest. Backspace takes the syllable before the cursor and
  Delete the one after it. Esc takes one step at a time: choosing, then a
  syllable still open, then the whole reading. Cangjie takes up to five keys and Quick two; a third
Quick key types the highlighted character and starts the next code, as Microsoft's
Quick does. Each shows the keys as radicals, lists matching characters, longer codes after exact ones,
and types one character at a time. A code's characters come in the table's own
frequency order, which also puts a character's main codes before its second ones.
Where the table gives characters the same frequency, the commonest in Taiwan or
Hong Kong usage (`chinese/char-ranks.txt`) come first. Characters neither
source uses, those outside Big5-HKSCS (Simplified and Japanese forms, most of
Extensions A and B, which fonts often lack), compatibility forms and symbols
come after a code's common completions, together with rare completions, the
rest of Big5-HKSCS first and exact matches before completions, so every
character stays typable but none is offered before a common one. Each character is listed once. SteamOS's
tables already carry frequencies, so there the list only breaks ties; the fuller
tables the desktop simulator bundles have nearly none, and the list orders them:
Quick `hi` offers 我 and 的 first, `ru` 嘅. The table is read once and kept while the language is added. Cangjie and Quick read the tables SteamOS installs
in `/usr/share/ibus-table/tables`; without them those languages stay listed, and choosing
one reports the error. A reading that matches nothing stays to be corrected: other keys
are refused, and the status line says so, until Backspace fixes it or Esc cancels it.

If Cangjie or Quick cannot send a choice, the accepted characters stay in the
local composition, up to 1,024 Unicode code points. After delivery is available,
Enter submits that text in smaller pieces. A full composition refuses
more input without discarding existing text. Backspace removes one reading key,
or one retained Unicode code point when there is no reading. If submission has
already started and a piece fails, the status asks for Enter to retry only the
unsent remainder. Hiding or changing the typing context cancels queued output;
text that was already submitted is never replayed automatically.

Chinese and Korean show the reading centred in a line of its own under the
toolbar, in the place of a narrow keyboard's second toolbar row, whose buttons
sit at the line's left, with a hint for Chinese while nothing is typed, and the case fits
the keys with no empty space at the sides. Other languages on the same layout
among your languages keep the keys at that size and place, the second toolbar
row where the line would be, so switching between them moves nothing.
The candidates show in the toolbar row,
where swipe suggestions go: from its left, a clear gap after its buttons, so
the first is always in the same place, with ∨ after them while there are more; a still row ends on a
whole candidate, none cut before ∨. Drag them sideways to see more, or tap ∨ to open them all in a menu under the reading line, over the keys,
as tall as its lines up to five; a press outside the menu closes it, choosing
nothing, and the keys wait while it is open. The keys do the
rest, with no Commit or Cancel buttons.

For Chinese, type Pinyin without tone numbers. Use `v` for ü and an apostrophe
for syllable separation where needed; the reading line shows the syllables
apart, as `ni hao`. Arrow Up/Down change the highlighted candidate, and `-` and
`=` move five back or on (not in Zhuyin, whose `-` is ㄦ). Space, number keys 1–5, or tapping a candidate selects it; the five
around the highlighted one are numbered (in Zhuyin, whose digits are tones, only
while choosing). If it covers the whole reading, selection commits the text (not in Zhuyin, below). Otherwise
the remaining reading stays available for the next selection. In Pinyin, Enter
types what is left of the reading as the letters typed, after any characters
chosen, as on a PC (`steam`, `gg`); in Zhuyin, Cangjie and Quick it submits the
current conversion. Escape discards it. A second Enter sends an ordinary Enter.
Letters typed with Shift or Caps Lock type themselves, a capital at once, after
any reading. A reading longer than 30 keys (Pinyin) or syllables (Zhuyin) sends
its converted front ahead and goes on, so no key is lost. Both script profiles
use Pinyin; Traditional here does not mean a Zhuyin layout.

Chinese types Chinese punctuation, as a PC input method does in its Chinese
mode, from the `punctuation` tables of its language file
([configuration](configuration.md)), after any reading; in Zhuyin it goes into
the reading instead:

| Keys | Simplified Pinyin | Traditional Pinyin, Cangjie, Quick | Zhuyin |
|---|---|---|---|
| `,` `.` `?` `!` | ，。？！ | ，。？！ | Shift+`,` `.` `/` `1`: ，。？！ |
| `;` `:` | ；： | ；： | Shift+`'` `;`: ；： |
| `\` | 、 | 、 | `'`: 、 |
| `'` `"` | ‘ ’, “ ” in turn | ‘ ’, “ ” in turn | |
| `(` `)` | （） | （） | （） |
| `[` `]` | 【】 | 「」 | 「」 |
| `{` `}` | | 『』 | 『』 |
| `<` `>` | 《》 | 《》 | |
| `$` `^` `_` `~` | ￥ …… —— ～ | `$` …… —— ～ | ～ |

After a digit, `.`, `,` and `:` type themselves, for 3.5, 1,000 and 10:30.
Other keys, such as `@`, `#`, `%` and `/`, type themselves too; for English
punctuation, add an English layout. The keys keep their usual legends.

For Korean, type the displayed two-set jamo. As on a PC, each syllable goes into
the field as soon as the next one starts, so only the syllable being typed waits
in the reading line, and there is no length limit. A final consonant still moves
on: 간 then ㅏ types 가 and leaves 나 open. Shift selects doubled consonants and
the shifted vowels. Caps Lock does not double jamo. With
[Pull for Shift](configuration.md#pull-for-shift) on, pulling a key down types its Shift
jamo, which the key shows, such as ㅃ over ㅂ. Backspace takes back one jamo
of the open syllable (값 → 갑), then deletes whole syllables in the field as the
app does. Enter types the open syllable and sends Enter in the same press. Esc
goes to the app at once and drops the open syllable; syllables the app refused
still go in before it, and while Enter waits to resend refused text, Enter
resends it and leaves the open syllable as it is. Space and other ordinary
keys type it before sending the key. The 한/영 key, in right Alt's place as on a
Korean PC keyboard, types the open syllable and switches the keys between Hangul
and the layout's Latin letters, whether or not English is added. A latched
Shift waits through it for the next letter. The language button still shows
한국어 and moves through your languages. Korean starts in Hangul, and returns to
it when you change language or layout or reload the config. Punctuation uses
ordinary US keys. A saved selection of the former Korean 12-key (천지인) keyboard,
including an additional language, opens as two-set Korean on US QWERTY.

Chinese types its own script, its punctuation, and the letters typed with Shift
or Caps Lock; for English words, Pinyin's Enter types the letters, or add an
English layout as an additional language and switch with the language button.

Pinyin and Zhuyin keep a bounded preedit, sending its front ahead at the
limit; Cangjie and Quick use the retained-text bound above.
Korean's only waits while the app can't take its syllables.
Shortcuts and navigation that leave composition commit first; if output refuses
the composition before admission, the preedit stays visible and the forwarding
action is blocked. An admitted long batch uses the output retry behavior above.
Hiding, changing profiles, and closing discard unfinished preedit, as with Japanese; moving,
resizing, pinning and recentring the panel, and opening Settings, keep it. A held Backspace repeats on the preedit after 0.5 s, every 40 ms. With Chinese it stops once the preedit is empty; the press stays local, so it never goes on to delete the target's text. Korean's goes on, as on a PC: at the open syllable's end it pauses for a fresh 0.5 s, then deletes in the field every 40 ms until you let go. Other composition keys do not repeat.

Chinese uses the Frame's PyZy library and dictionary; Korean uses libhangul.
Both are loaded only when their language is selected. Missing libraries disable
that composition profile, not the whole keyboard.
No IBus daemon configuration, system language setting, clipboard access or network
service is used, and PyZy and libhangul keep no typing history. PyZy's conversion/selection
learning APIs are deliberately avoided. Its startup database uses private
RAM-backed storage that is removed before typing; the active user database is
in memory. If private memory-backed storage is unavailable, the profile reports
an error rather than using persistent storage. A build without an engine rejects its profile on Apply while retaining the
previous profile. A saved unsupported composition profile falls back to English
at launch with an error message.

### Chinese engines

PyZy is being replaced, and its candidates for Traditional Chinese come from
the mainland dictionary. A blind benchmark chose these engines, built from
the revisions pinned in `tools/chinese-pins.sh` ([building](building.md#dependencies)):

| Engine | Module | Input methods |
|---|---|---|
| libime, with an n-gram model, as in Fcitx 5 | `framekeyboard-libime` | Pinyin; Traditional through OpenCC's Taiwan phrases (`s2twp`) |
| McBopomofo's engine, with its Taiwan phrase data | `framekeyboard-mcbopomofo` | Zhuyin |

Each method takes its engine when that module is installed, else PyZy.
`FRAMEKEYBOARD_CHINESE_ENGINE` names the engine to use instead, for comparing
them: `pyzy`, `libime` or `mcbopomofo`. Hanyu Pinyin through McBopomofo isn't
offered, because it needs tone numbers, and 1–5 choose candidates here.

Zhuyin converts a syllable as soon as its tone is typed, and takes the tones
as written in the dictionary or as spoken (不客氣 with ㄅㄨˋ or ㄅㄨˊ). The
first candidate is the conversion as it stands, so Space keeps it; the others
are the phrases starting there, longest first, with characters from
Extension A and later last. The engines' dictionaries stay
loaded while a Chinese language is added, main or additional, and are freed
when it is removed, as Mozc's server is. Traditional Pinyin's OpenCC needs
about 50 MB more than Simplified while it is added.

Both engines learn, as on Steam's keyboard. They learn from text once it
reaches the field, never in a password, PIN or terminal field, and never in a
preview. libime learns the words you choose and the sentences you commit.
McBopomofo learns the candidates you pick over its conversion, as McBopomofo
does, and keeps them across restarts, each fading over weeks. What they learn
stays in Full Keyboard's config folder, in `chinese/libime` and
`chinese/mcbopomofo`, owner-only. **Clear learned words** lists it as
Chinese and deletes it.

Automated tests cover profile legends, sample letters, AltGr levels and dead
keys of every national keymap, Chinese script conversion/candidates,
Korean syllables/editing, and commit ordering. A dedicated receiver checks Unicode
transport separately from composition. Fluent-speaker review and per-application
acceptance are still needed; passing a local conversion test does not establish
that every browser or desktop session accepts it.
