# Languages and system keyboard settings

The typing language is an app setting, separate from SteamOS's interface locale.
A fresh installation starts in English US. After Apply and save, the keyboard
reopens with the last saved language, layout and theme. A Russian system locale
does not automatically select Russian. Updating the app preserves your selection.

## Included profiles

| Language | Profile ID | Layout | Character mapping |
| --- | --- | --- | --- |
| English US | `en-us` | US full-size | US XKB |
| German | `de-de` | International full-size | German XKB |
| French | `fr-fr` | International full-size | French AZERTY, France |
| Spanish | `es-es` | International full-size | Spanish, Spain |
| Italian | `it-it` | International full-size | Italian XKB |
| Portuguese, Brazil | `pt-br` | Brazilian ABNT2 full-size | Brazilian XKB, including extra slash and keypad separator |
| Russian | `ru-ru` | US full-size | Russian ЙЦУКЕН |
| Ukrainian | `uk-ua` | US full-size | Ukrainian ЙЦУКЕН |
| French, Canada | `fr-ca` | International full-size | Canadian French, accents on dead keys |
| Arabic | `ar` | International full-size | Arabic XKB, legends right to left |
| Croatian | `hr-hr` | International full-size | Croatian QWERTZ |
| Czech | `cs-cz` | International full-size | Czech QWERTZ, accented letters on the number row |
| Danish | `da-dk` | International full-size | Danish XKB |
| Dutch | `nl-nl` | US full-size | US international with AltGr dead keys; ' and " stay plain |
| Estonian | `et-ee` | International full-size | Estonian XKB |
| Finnish | `fi-fi` | International full-size | Finnish XKB |
| Greek | `el-gr` | US full-size | Greek XKB, tonos and dialytika dead keys |
| Hebrew | `he-il` | US full-size | Hebrew SI-1452, legends right to left |
| Hindi | `hi-in` | International full-size | Devanagari InScript |
| Hungarian | `hu-hu` | International full-size | Hungarian QWERTZ, 0 left of 1 |
| Indonesian | `id-id` | US full-size | US XKB, the standard layout there |
| Latvian | `lv-lv` | US full-size | Latvian, letters on AltGr |
| Lithuanian | `lt-lt` | US full-size | Lithuanian, letters on the number row |
| Malay | `ms-my` | US full-size | US XKB, the standard layout there |
| Norwegian | `nb-no` | International full-size | Norwegian XKB |
| Polish | `pl-pl` | US full-size | Polish programmer, letters on AltGr |
| Romanian | `ro-ro` | International full-size | Romanian standard, ș and ț with comma below |
| Slovak | `sk-sk` | International full-size | Slovak QWERTZ |
| Slovenian | `sl-si` | International full-size | Slovenian QWERTZ |
| Swedish | `sv-se` | International full-size | Swedish XKB |
| Thai | `th-th` | International full-size | Thai Kedmanee |
| Turkish | `tr-tr` | International full-size | Turkish Q |
| Vietnamese | `vi-vn` | US full-size | Vietnamese XKB, tone keys on the number row |
| Simplified Chinese | `zh-cn-pinyin` | US full-size | Local Pinyin with candidates |
| Traditional Chinese | `zh-tw-pinyin` | US full-size | Local Pinyin with Traditional output |
| Korean | `ko-kr`, `ko-cheonjiin` | US full-size, or 12-key 천지인 (`ko-cheonjiin` only) | Local two-set Hangul, or 천지인 strokes |
| Japanese | `ja-romaji`, `ja-kana`, `ja-jis`, `ja-flick` | US, Japanese JIS, or flick 12-key (`ja-flick` only) | See [Japanese input](japanese.md) |

Open Settings, select Layout, then Language, then Apply and save. Languages
the selected layout cannot type are dimmed, and the open list says so on the
status line; choose a layout with the needed keys first
(for example International or UK for German). Choosing a layout that cannot
type the current language switches to the most likely one for it: a favorite
pair, another variant of the same language, the layout's own language, or US
English. Theme selection is unchanged.

Languages listed with International full-size also work on US full-size,
except those with characters only on the extra key left of Z: Arabic, Danish,
French (Canada), Hindi, Norwegian, Swedish and Thai, which Settings dims there.
All of them work on International and UK full-size. AltGr is the right Alt
key; its legend reads AltGr whenever the language uses it.

Accents on dead keys, such as the Czech caron or the Greek tonos, are typed before the letter,
and Shift may be pressed after the accent for a capital. On the Vietnamese
layout the number-row tone keys add a combining mark after the letter; AltGr
with the same key is a dead tone typed before the letter, which gives the
single precomposed letter that some apps handle better. Indonesian and Malay
use the US layout, which is standard there, and differ in their dictation
language. Dutch uses US international with AltGr dead keys, so an apostrophe
before n or s stays an apostrophe.

Arabic and Hebrew keys show their legends as they appear in right-to-left text,
so a bracket key shows the bracket it adds to the text. In the dictation
editor, right-to-left text aligns to the right, and the arrow keys move the
cursor the way they point on screen, stepping over a letter together with its
marks, as in Thai or Hindi. The headset's own Noto fonts draw the Arabic,
Hebrew, Devanagari, Thai and Greek legends.

Spanish regional variants beyond Spain, French beyond France and Canada,
Chinese Zhuyin, Cangjie and Wubi, Korean Hanja conversion, Vietnamese Telex and
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

For Chinese, type Pinyin without tone numbers. Use `v` for ü and an apostrophe
for syllable separation where needed. The candidate row shows five entries at a
time. Arrow Up/Down or the candidate-row arrows change the highlighted entry.
Space, number keys 1–5, or clicking a candidate selects it. If it covers the
whole reading, selection commits the text. Otherwise the remaining reading stays
available for the next selection. Enter or Commit submits the current conversion;
Escape or Cancel discards it. A second Enter sends an ordinary Enter. Both script
profiles use Pinyin; Traditional here does not mean a Zhuyin layout.

For Korean, type the displayed two-set jamo. Shift selects doubled consonants and
the shifted vowels. Caps Lock does not double jamo. Backspace undoes one typed
jamo, including changes across syllable boundaries. Enter or Commit submits the
composition. Space and other ordinary keys commit the word before sending the
key. Use the `한 / A` control to switch between Hangul and Latin typing. Chinese
has a corresponding `中文 / A` control. Punctuation uses ordinary US keys.

For 12-key Korean, choose the **Korean 12-key (천지인)** layout; it selects
한국어 — 천지인. As with Japanese flick, it keeps Esc, the F keys and the number
row, with Tab, ←, ↑/↓ and Shift on the left and Space, → and a two-row Enter on
the right. Its 3×4 grid is ㅣ ㆍ ㅡ / ㄱㅋ ㄴㄹ ㄷㅌ / ㅂㅍ ㅅㅎ ㅈㅊ / .,?! ㅇㅁ ~^-.

- Vowels are built from the strokes ㅣ, ㆍ and ㅡ: ㅣㆍ ㅏ, ㆍㅣ ㅓ, ㆍㅡ ㅗ,
  ㅡㆍ ㅜ, ㅣㆍㆍ ㅑ, ㆍㆍㅣ ㅕ, ㆍㆍㅡ ㅛ, ㅡㆍㆍ ㅠ. Another ㅣ adds to the vowel
  before it: ㅐ ㅔ ㅒ ㅖ ㅢ ㅚ ㅟ, ㅝ after ㅠ, then ㅙ ㅞ; ㆍ after ㅚ gives ㅘ.
  A lone ㆍ or ㆍㆍ shows in the reading until a stroke finishes it, and is
  typed as it shows if committed.
- Tapping a consonant key again within one second changes the consonant:
  ㄱ ㅋ ㄲ, ㄴ ㄹ, ㄷ ㅌ ㄸ, ㅂ ㅍ ㅃ, ㅅ ㅎ ㅆ, ㅈ ㅊ ㅉ, ㅇ ㅁ. To type the same
  consonant twice, as in 각기, wait a second or press → in between.
- Flicks are shortcuts: a consonant key flicks right for its second consonant
  and up for its double one, ㅣ flicks right for ㅏ and left for ㅓ, and ㅡ up for
  ㅗ and down for ㅜ, the side the dot goes on. .,?! cycles . , ? ! and flicks to
  , ? ! …; ~^- cycles ~ ^ - and flicks to ( " ) '.
- Syllables, final consonants and clusters such as ㄺ form as in two-set
  typing, and a final moves to the next syllable when a vowel follows.
  Backspace removes the last stroke or consonant. Space, Enter, Commit, Esc,
  Cancel and other keys behave as for two-set Korean, and held Backspace
  repeats on the reading the same way.

천지인 composes inside Full Keyboard and needs no libhangul. Leaving the
12-key layout switches to two-set Korean, which does.

Both modes keep a bounded preedit. Commit before the 30-input-character limit.
Shortcuts and navigation that leave composition commit first; if delivery fails,
the preedit stays visible and the forwarding action is blocked. Hiding, changing
profiles, and closing discard unfinished preedit, as with Japanese; moving,
resizing, pinning and recentring the panel keep it. A held Backspace repeats on the preedit after 0.5 s, every 40 ms, and stops once it is empty; the press stays local, so it never goes on to delete the target's text. Other composition keys do not repeat.

Chinese uses the Frame's PyZy library and dictionary; Korean uses libhangul.
Both are loaded only when their language is selected. Missing libraries disable
that composition profile, not the whole keyboard.
No IBus daemon configuration, system language setting, clipboard access, network
service, or persistent typing history is used. PyZy's conversion/selection
learning APIs are deliberately avoided, and its user database has no writable
path. A build without an engine rejects its profile on Apply while retaining the
previous profile. A saved unsupported composition profile falls back to English
at launch with an error message.

Automated tests cover profile legends, sample letters, AltGr levels and dead
keys of every national keymap, Chinese script conversion/candidates,
Korean syllables/editing including 천지인 strokes, toggles and flicks, and
commit ordering. A dedicated receiver checks Unicode
transport separately from composition. Fluent-speaker review and per-application
acceptance are still needed; passing a local conversion test does not establish
that every browser or desktop session accepts it.
