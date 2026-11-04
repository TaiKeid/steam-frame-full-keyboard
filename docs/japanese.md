# Japanese input

Open the Language dropdown in Settings, choose Japanese Romaji, Kana, or JIS, then use Apply and save. For flick input, choose the **Japanese flick 12-key** layout instead; it selects 日本語 — Flick. Kana and JIS need the JIS layout and Flick needs the flick layout: choose the layout first, since Settings dims languages the selected layout cannot type (the open list says so). Choosing a layout that cannot type the current language switches to the closest one, such as Romaji when leaving the flick layout. Existing English/German profiles remain available in the language selector.

| Preset | Entry | Conversion/output |
| --- | --- | --- |
| 日本語 Romaji | Latin letters, such as `nihon` | Local kana composition and Anthy kanji candidates |
| 日本語 Kana | Direct JIS-position kana keys | Same local conversion and candidate controls |
| JIS (system IME) | Physical JIS keys, including 変換/無変換/かな | Conversion handled by an already configured target IME |
| 日本語 Flick | 12-key flick grid, as on Japanese phones | Same local conversion and candidate controls |

## Built-in composition

Romaji is the default Japanese choice for someone unfamiliar with Japanese keyboard layouts. It supports common Hepburn and Japanese IME spellings, doubled consonants, `n'` disambiguation, `nn`, and small kana with `x`/`l` prefixes. Additional aliases include `ca/ci/cu/ce/co`, `fya/fyu/fyo`, `tha/thi/thu/the/tho`, `dha/dhi/dhu/dhe/dho`, and `wu`, following the corresponding entries in [Mozc's romaji table](https://github.com/google/mozc/blob/master/src/data/preedit/romanji-hiragana.tsv). Direct Kana has Shift variants for small kana and punctuation; ゛ and ゜ combine with the preceding kana.

On full layouts the reading shows centred in a line of its own between the toolbar and the keys, and the candidates in the toolbar row, after its buttons, where swipe suggestions go: centred while they all fit, or filling the row with ∨ after them while there are more. ∨ drops all of them down in a menu over the keys, with a scrollbar when they run longer than it; while it is open, the keys take no presses, and a press outside it closes it without choosing. The flick layout instead has a band with a row for the reading and one for the candidates, which ends with ∨ and opens them over the keys. Either stays in every mode, so switching modes never changes the size of the keys, and the case fits the keys with no empty space at the sides. Before anything is typed, a hint shows where the reading goes. The keys do the rest: there are no separate convert, commit, script or cancel buttons.

- **Live candidates.** After a short pause in typing (0.15 s), candidates for the reading appear without pressing 変換: the whole phrase converted, then the candidates of its first segment, then the reading in hiragana and in katakana. While you keep typing, the old list stays dimmed and takes no clicks until the next one is ready. Tapping the whole phrase commits it. Tapping a first segment's candidate chooses it and moves on to the next segment, as in conversion below; tapping hiragana or katakana commits the reading in that script.
- **Emoji.** Live candidates end with up to four emoji for the reading or its likeliest conversions, as on a phone: ねこ offers 🐈 🐱 🐈‍⬛ 😺. They come from the system's IBus emoji dictionary (`/usr/share/ibus/dicts/emoji-ja.dict`, read through libibus, both already on the Steam Frame), loaded on the first live candidates; without them there are none. Tapping one types it in place of the reading.
- **Conversion.** Space or 変換 converts the reading, then moves through the active segment's candidates. On layouts with Shift and arrow keys, Shift+Space or ↑ goes back and ↓ forward. Tapping a candidate chooses it for the active segment and moves on to the next; in the last segment it commits the whole phrase. The active segment is highlighted in the reading. Each segment's list ends with its reading in hiragana and katakana, if Anthy did not already offer them.
- **The cursor.** Before conversion, ← and → move a cursor through the reading, as in a phone's IME: kana, romaji, ⌫, ↶ and ゛゜小 act at it, and the band and the dictation editor show it as a caret. It stays inside the reading. Conversion still takes the whole reading.
- **Segment boundaries.** On the flick layout, as on phones, → makes the active segment one character longer and ← one character shorter; Anthy splits the rest of the reading again, and earlier segments keep their choices. Shift+←/→ moves to the previous or next segment instead. On other layouts, as in PC IMEs, ←/→ move between segments and Shift+←/→ change the active segment's length.
- Enter or 確定 commits the whole phrase as shown. It does **not** submit the target form. Enter with no composition sends a normal physical Enter.
- F6 and F7 change the reading's script to hiragana and katakana. 無変換 restores the hiragana reading.
- Backspace removes a kana/pending roman letter; during conversion it first returns to the reading. Escape first cancels conversion, then clears the reading.
- Full layouts type kana only, as Japanese keyboards do: for English, add an English layout as an additional language and switch with the language button. On a JIS keyboard, 半/全 and かな still switch between kana composition and Latin keys, typing any reading first. The flick layout has its own mode key instead; see below.
- Starting another word while converting commits the chosen phrase first. Tab, Delete, Home/End, Copy/Paste, and Ctrl/Alt/Meta shortcuts first commit unfinished composition, then send their physical key events. If committing fails, the text remains and the key or shortcut is not sent. Escape still cancels deliberately.

The preedit stays in the keyboard until committed. Moving the laser off the keyboard keeps it. Opening Settings, dragging, hiding, recentering, changing profiles or losing input focus discards it. This avoids committing an old phrase after a focus transition. Input routing follows compositor focus, just like the ordinary keyboard.

## Flick input

The flick layout is four rows, as on a phone, laid out like Gboard's 12-key keyboard: the kana grid (あ か さ / た な は / ま や ら / ゛゜小 わ 、。?!) with ↶, ←, 😊 and 「あA1」 on its left, and ⌫, →, 空白 (Space) and 改行 (Enter) on its right. Space and Enter show ␣ and ↵; while a reading is open they show 変換 (次候補 while converting) and 確定. It has no function keys, number row, Esc, Tab or Shift; the English page has those, and digits have a mode of their own. Cut, Copy and Paste stay in the side column. Each kana key shows its tap kana with the four flick kana as small hints. The band's candidate row ends with ∨, which opens every candidate over the keys, as a phone's does: the row keeps the ones that fit, and the rest wrap into lines below it that scroll up and down with a drag. Tapping one chooses it and closes the list; ∧, in ∨'s place, or a press anywhere else closes it without choosing, and the keys take no presses meanwhile. The kana page has no ↑ and ↓ for the field's cursor; the English page has them.

- Tap a key for its first kana. Hold the trigger, move left, up, right or down by about 40% of a key's height and release for that direction's kana (あ: い う え お). While held, a cross-shaped preview over the key shows the tap kana in the middle and the four flick kana around it, highlighting the one release will type; moving back to the centre returns to the tap kana. The preview lies above everything else, may extend past the keyboard edge, and the laser keeps tracking over it.
- Toggle input: tapping the same key again within one second replaces the kana with the next in its row, then small kana (あ い う え お ぁ … ). → ends the cycle, so the next tap types the first kana again.
- ↶ works as Gboard's 逆トグル／戻す key. While typing, it steps the last kana back through its key's taps (う い あ, then on to ぉ), and a tap after it goes on from there. While converting, it returns to the reading. Straight after a phrase is committed, it takes the phrase back out of the field (with Backspace, one per character) and opens it as a reading again; any other key, the laser leaving the keyboard or the keyboard hiding ends that chance, since the field's cursor might no longer be after the phrase. While the phrase is still being typed, ↶ says so and keeps the chance for a tap once it is in. The reading appears once the field has taken every Backspace; until then keys, A/あ, the language button and Settings wait. A field that goes three seconds without answering is given up on, with a message, and the keys work again; a slow field that keeps answering is waited for. It is not the dictation editor's Undo.
- ゛゜小 cycles the previous kana through its small, voiced and semi-voiced forms (つ っ づ, は ば ぱ); flick left for ゛, up for 小 and right for ゜. や flicks to 「 and 」, わ to を, ん and ー, and 、。 to 。, ？, ！ and ….
- 「あA1」 switches between kana, English and digits; its lit character shows which is on. A tap moves on to the next mode; a flick picks one: left for kana, up for English, right for digits. The three share the reading, as on a phone: きょうは, then English, then 3じに still converts to 今日は3時に.
- **English** shows the layout's English page: a QWERTY keyboard with Esc, Tab, Shift, ⌫, ↵ and the arrows, in the same case as the kana page, so nothing moves or resizes. Its letters type at once, or with a reading open, join it as typed: Space converts it and Enter commits it, as on the kana page. Swipe typing is not available on it.
- **Digits** turns the grid into a number pad, 1–9 with ( 0 . on the bottom row. A tap adds the digit to the reading and a flick its symbols, as on a phone's number mode, so 3 and then じに on the kana page convert to 3時に, with ３時に and 三時に among the candidates; Enter commits digits typed alone. The symbols: 2 flicks to ¥ $ €, 5 to + × ÷, 0 to 〜 …, and so on, as each key's hints show. The down flicks, and the free sides of ( 0 ., hold the ASCII symbols the English page has no keys for: @ & _ ~ ; ' " ` { } ! ?.
- In dictation mode the kana page gets Esc in the side column's top slot and Send in 😊's place.

Flick and toggle typing happen on release; other keys still act on press. A copy of the flick layout without the 「あA1」 key keeps the toolbar's あ / A button, which switches between kana and English.

## Learning

Learning is on by default, as on Steam's own keyboard, which always learns. With Japanese as the main language or an additional one, Settings → Languages and Layouts offers **Learn from my typing** to turn it off. While it is on:

- A phrase committed from a conversion teaches Anthy which candidate each segment took, so they come first next time.
- Live candidates also offer words committed before that begin with the reading, right after the whole phrase: once ありがとう was committed as 有難う, typing あり offers 有難う. These are only words you committed; Anthy has no dictionary of its own for predictions.
- Nothing is learned in a field that says it is for a password, a PIN or a terminal, as a [shell](integration.md)'s fields can. Steam's own fields don't say, so there, as on Steam's keyboard, phrases are learned.

Anthy keeps what it learns in plain text, in `~/.anthy/last-record1_framekeyboard.utf8` and `last-record2_framekeyboard.utf8`: under a name of Full Keyboard's own, apart from what Steam's keyboard learns there. A phone IME's learned words sit in storage other apps can't open, but Steam Frame's desktop is an unlocked Linux system, where any program you run can read the files in your home folder. Steam's own keyboard keeps its learning the same way.

**Clear learned words**, under the setting, deletes those files after asking. It can't be pressed while there is nothing to clear. Anthy restarts to forget what it had loaded; meanwhile a ring circles the button, which can't be pressed again, and live candidates wait. Turning learning off stops both learning and using what was learned; the files stay until cleared.

With learning off, Anthy reads and writes no record at all, Steam's keyboard's included, so candidates keep Anthy's own order. Anthy still keeps an empty lock file in `~/.anthy`.

## Runtime requirements

The integrated modes use `libanthy.so.0`, its installed dictionary and Japanese fonts. These were already present on the tested Steam Frame. No daemon, browser, online converter, clipboard fallback or dictionary training is added. With [learning](#learning) turned off, Anthy learns nothing from your choices, so nothing you type is written to disk. Live candidates convert the reading once after each pause in typing, never while you type; ordinary polling/rendering stays adaptive. The library/dictionary load with the first conversion, live or requested, and stay loaded until the keyboard exits.

The normal launcher with `--input ei --start-enabled` supports the bundled integrated profiles. Japanese characters are composed locally. The normal launcher does not require a matching system keymap; native shortcuts use the compositor keymap where available. The text backend checks the private [Gamescope input-method protocol](https://github.com/ValveSoftware/gamescope/blob/6867f509874f9bc52e12d6f4c4596cdf0d5be6b4/protocol/gamescope-input-method.xml) before enabling output. Missing support leaves a conversion preview and a visible error. The uinput backend cannot deliver composed Japanese text. A numbered `--ei-socket` selects the matching text compositor; custom names need `--text-socket`. See [runtime routing](runtime.md).

External JIS is for a session already using the Japanese physical keymap and a working system IME. Close the running keyboard before changing launch options:

```sh
~/.local/bin/framekeyboard --vr --input ei --target-language ja-jis --start-enabled
```

Then select JIS (system IME) in Settings. Selecting JIS while still declaring a US target disables output; it never changes the system layout/IME silently. The geometry uses a tall rectangular Enter key.

## Current limits and validation

Reading is limited to roughly 30 Unicode characters per phrase, with at most 32 in one native commit. Candidate lists expose up to 128 alternatives per segment, plus the two scripts. Drag the candidate row sideways to see more; a quick drag glides on, like the emoji picker, and the thumbstick scrolls it too. Full paragraph editing, predictions from earlier choices (they need Anthy to learn, which Full Keyboard does not do), custom learned dictionaries, half-width kana conversion and repeat while holding composition keys other than Backspace are not implemented. A held Backspace repeats on the reading, in the keyboard and in the dictation editor, and stops once the reading is empty. The desktop preview composes locally and never sends OS input.

Host and ARM64 tests cover romaji, voiced/small kana, candidate selection, scripts, failed commit retention, cancellation, opt-in and modifier routing. The dedicated on-device Xwayland receiver verified 日本 via the native UTF-8 connection. That does not establish every nested desktop/browser or external system-IME combination; test a disposable field in the desired application.
