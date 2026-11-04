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
type the current language switches to the most likely one for it: a favorite
pair, another variant of the same language, the layout's own language, or US
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
punctuation keys, and converts phrases; finish a syllable with a tone key, and Space
chooses the highlighted candidate. Cangjie takes up to five keys and Quick two. Each
shows the keys as radicals, lists matching characters, longer codes after exact ones,
and types one character at a time. Cangjie and Quick read the tables SteamOS installs
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
sit at the line's left, with a hint while nothing is typed, and the case fits
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
for syllable separation where needed. Arrow Up/Down change the highlighted
candidate. Space, number keys 1–5, or tapping a candidate selects it; the five
around the highlighted one are numbered (Zhuyin, whose digits are tones, has no
numbers). If it covers the whole reading, selection commits the text. Otherwise
the remaining reading stays available for the next selection. Enter submits the
current conversion; Escape discards it. A second Enter sends an ordinary Enter.
Both script profiles use Pinyin; Traditional here does not mean a Zhuyin layout.

For Korean, type the displayed two-set jamo. Shift selects doubled consonants and
the shifted vowels. Caps Lock does not double jamo. Backspace undoes one typed
jamo, including changes across syllable boundaries. Enter submits the
composition. Space and other ordinary keys commit the word before sending the
key. Chinese and Korean type their own script only, as their keyboards do; for
English, add an English layout as an additional language and switch with the
language button. Punctuation uses ordinary US keys.

Pinyin, Zhuyin and Korean keep a bounded preedit. Commit before the
30-input-character limit; Cangjie and Quick use the retained-text bound above.
Shortcuts and navigation that leave composition commit first; if output refuses
the composition before admission, the preedit stays visible and the forwarding
action is blocked. An admitted long batch uses the output retry behavior above.
Hiding, changing profiles, and closing discard unfinished preedit, as with Japanese; moving,
resizing, pinning and recentring the panel keep it. A held Backspace repeats on the preedit after 0.5 s, every 40 ms, and stops once it is empty; the press stays local, so it never goes on to delete the target's text. Other composition keys do not repeat.

Chinese uses the Frame's PyZy library and dictionary; Korean uses libhangul.
Both are loaded only when their language is selected. Missing libraries disable
that composition profile, not the whole keyboard.
No IBus daemon configuration, system language setting, clipboard access, network
service, or persistent typing history is used. PyZy's conversion/selection
learning APIs are deliberately avoided. Its startup database uses private
RAM-backed storage that is removed before typing; the active user database is
in memory. If private memory-backed storage is unavailable, the profile reports
an error rather than using persistent storage. A build without an engine rejects its profile on Apply while retaining the
previous profile. A saved unsupported composition profile falls back to English
at launch with an error message.

Automated tests cover profile legends, sample letters, AltGr levels and dead
keys of every national keymap, Chinese script conversion/candidates,
Korean syllables/editing, and commit ordering. A dedicated receiver checks Unicode
transport separately from composition. Fluent-speaker review and per-application
acceptance are still needed; passing a local conversion test does not establish
that every browser or desktop session accepts it.
