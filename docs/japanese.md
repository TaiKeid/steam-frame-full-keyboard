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

- Space or 変換 starts conversion, then advances candidates. Shift+Space or ↑ goes back. Click a candidate to select it.
- ←/→ select the phrase segment. The active segment is marked with 【 】 in the preedit. The selected candidate stays highlighted; ↑/↓ move through pages as needed.
- Enter or 確定 / Commit sends the completed text. It does **not** submit the target form. Enter with no composition sends a normal physical Enter.
- ひらがな/F6 and カタカナ/F7 change the reading's script. 無変換 restores the hiragana reading.
- Backspace removes a kana/pending roman letter; during candidate selection it first returns to the reading. Escape first cancels conversion, then clears the reading. Cancel clears it immediately.
- あ / A toggles local composition and Latin keys. The JIS 半/全 and かな keys also toggle composition in the integrated modes.
- Starting another word while candidates are active commits the chosen phrase first. Tab, Delete, Home/End, Copy/Paste, and Ctrl/Alt/Meta shortcuts first commit unfinished composition, then send their physical key events. If committing fails, the text remains and the key or shortcut is not sent. Escape and the Cancel control still cancel deliberately.

The preedit stays in the keyboard until committed. Moving the laser off the keyboard keeps it. Opening Settings, dragging, hiding, recentering, changing profiles or losing input focus discards it. This avoids committing an old phrase after a focus transition. Input routing follows compositor focus, just like the ordinary keyboard.

## Flick input

The flick layout keeps Esc, the F keys and the number row, and replaces the letter keys with a 12-key kana grid (あ か さ / た な は / ま や ら / ゛゜小 わ 、。?!). Tab, ←, ↑/↓ and Shift sit on its left; 空白 (Space, which shows 変換 while composing), → and a two-row 改行 (Enter, 確定 while composing) on its right. Ctrl, Alt, Super and Caps Lock are not on this layout. Each kana key shows its tap kana with the four flick kana as small hints.

- Tap a key for its first kana. Hold the trigger, move left, up, right or down by about 40% of a key's height and release for that direction's kana (あ: い う え お). While held, a cross-shaped preview over the key shows the tap kana in the middle and the four flick kana around it, highlighting the one release will type; moving back to the centre returns to the tap kana. The preview may extend past the keyboard edge, and the laser keeps tracking over it.
- Toggle input: tapping the same key again within one second replaces the kana with the next in its row, then small kana (あ い う え お ぁ … ). → ends the cycle, so the next tap types the first kana again.
- ゛゜小 cycles the previous kana through its small, voiced and semi-voiced forms (つ っ づ, は ば ぱ); flick left for ゛, up for 小 and right for ゜. や flicks to 「 and 」, わ to を, ん and ー, and 、。 to 。, ？, ！ and ….
- Digits and Shift symbols from the number row join the reading, as on phones. あ / A switches the number row to direct output; pressing a kana key returns to kana composition.

Flick and toggle typing happen on release; other keys still act on press. The first version's look is provisional.

## Runtime requirements

The integrated modes use `libanthy.so.0`, its installed dictionary and Japanese fonts. These were already present on the tested Steam Frame. No daemon, browser, online converter, clipboard fallback or dictionary training is added. Conversion runs only when requested; ordinary polling/rendering stays adaptive. The library/dictionary stay loaded after first conversion until the keyboard exits.

The normal launcher with `--input ei --start-enabled` supports the bundled integrated profiles. Japanese characters are composed locally. The normal launcher does not require a matching system keymap; native shortcuts use the compositor keymap where available. The text backend checks the private [Gamescope input-method protocol](https://github.com/ValveSoftware/gamescope/blob/6867f509874f9bc52e12d6f4c4596cdf0d5be6b4/protocol/gamescope-input-method.xml) before enabling output. Missing support leaves a conversion preview and a visible error. The uinput backend cannot deliver composed Japanese text. A numbered `--ei-socket` selects the matching text compositor; custom names need `--text-socket`. See [runtime routing](runtime.md).

External JIS is for a session already using the Japanese physical keymap and a working system IME. Close the running keyboard before changing launch options:

```sh
~/.local/bin/framekeyboard --vr --input ei --target-language ja-jis --start-enabled
```

Then select JIS (system IME) in Settings. Selecting JIS while still declaring a US target disables output; it never changes the system layout/IME silently. The geometry uses a tall rectangular Enter key.

## Current limits and validation

Reading is limited to roughly 30 Unicode characters per phrase, with at most 32 in one native commit. Candidate lists expose up to 128 alternatives per segment. Full paragraph editing, segment-boundary resizing, prediction, custom learned dictionaries, half-width kana conversion and repeat while holding composition keys other than Backspace are not implemented. A held Backspace repeats on the reading, in the keyboard and in the dictation editor, and stops once the reading is empty. The desktop preview composes locally and never sends OS input.

Host and ARM64 tests cover romaji, voiced/small kana, candidate selection, scripts, failed commit retention, cancellation, opt-in and modifier routing. The dedicated on-device Xwayland receiver verified 日本 via the native UTF-8 connection. That does not establish every nested desktop/browser or external system-IME combination; test a disposable field in the desired application.
