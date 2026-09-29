# Japanese input

Choose a Japanese preset at the top of Settings, then Apply and save. It preserves the theme and chooses the required keyboard geometry. Existing English/German profiles remain available in the language selector.

| Preset | Entry | Conversion/output |
| --- | --- | --- |
| 日本語 Romaji | Latin letters, such as `nihon` | Local kana composition and Anthy kanji candidates |
| 日本語 Kana | Direct JIS-position kana keys | Same local conversion and candidate controls |
| JIS (system IME) | Physical JIS keys, including 変換/無変換/かな | Conversion handled by an already configured target IME |

## Built-in composition

Romaji is the default Japanese choice for someone unfamiliar with Japanese keyboard layouts. It supports common Hepburn and Japanese IME spellings, doubled consonants, `n'` disambiguation, `nn`, and small kana with `x`/`l` prefixes. Direct Kana has Shift variants for small kana and punctuation; ゛ and ゜ combine with the preceding kana.

- Space or 変換 starts conversion, then advances candidates. Shift+Space or ↑ goes back. Click a candidate to select it.
- ←/→ select the phrase segment. The active segment is marked with 【 】 in the preedit. The selected candidate stays highlighted; ↑/↓ move through pages as needed.
- Enter or 確定 / Commit sends the completed text. It does **not** submit the target form. Enter with no composition sends a normal physical Enter.
- ひらがな/F6 and カタカナ/F7 change the reading's script. 無変換 restores the hiragana reading.
- Backspace removes a kana/pending roman letter; during candidate selection it first returns to the reading. Escape first cancels conversion, then clears the reading. Cancel clears it immediately.
- あ / A toggles local composition and Latin keys. The JIS 半/全 and かな keys also toggle composition in the integrated modes.
- Starting another word while candidates are active commits the chosen phrase first. Ctrl/Alt/Meta shortcuts and Copy/Paste remain physical shortcuts and discard an unfinished composition.

The preedit stays in the keyboard until committed. Moving the laser off the keyboard keeps it. Release all, opening Settings, dragging, hiding, recentering, changing profiles or losing input focus discards it. This avoids committing an old phrase after a focus transition. Input routing follows compositor focus, just like the ordinary keyboard.

## Runtime requirements

The integrated modes use `libanthy.so.0`, its installed dictionary and Japanese fonts. These were already present on the tested Steam Frame. No daemon, browser, online converter, clipboard fallback or dictionary training is added. Conversion runs only when requested; ordinary polling/rendering stays adaptive. The library/dictionary stay loaded after first conversion until the keyboard exits.

The normal launcher with `--input ei --target-language en-us --start-enabled` supports the bundled integrated profiles. Their physical XKB definition remains US for shortcut routing; the selected language ID differs because Japanese characters are consumed locally. The text backend checks the private [Gamescope input-method protocol](https://github.com/ValveSoftware/gamescope/blob/6867f509874f9bc52e12d6f4c4596cdf0d5be6b4/protocol/gamescope-input-method.xml) before enabling output. Missing support leaves a conversion preview and a visible error. The uinput backend cannot deliver composed Japanese text.

External JIS is for a session already using the Japanese physical keymap and a working system IME. Close the running keyboard before changing launch options:

```sh
~/.local/bin/framekeyboard --vr --input ei --target-language ja-jis --start-enabled
```

Then select JIS (system IME) in Settings. Selecting JIS while still declaring a US target disables output; it never changes the system layout/IME silently. The geometry uses a tall rectangular Enter key.

## Current limits and validation

Reading is limited to roughly 30 Unicode characters per phrase, with at most 32 in one native commit. Candidate lists expose up to 128 alternatives per segment. Full paragraph editing, segment-boundary resizing, prediction, custom learned dictionaries, half-width kana conversion and repeat while holding composition keys are not implemented. The desktop preview composes locally and never sends OS input.

Host and ARM64 tests cover romaji, voiced/small kana, candidate selection, scripts, failed commit retention, cancellation, opt-in and modifier routing. The dedicated on-device Xwayland receiver verified 日本 via the native UTF-8 connection. That does not establish every nested desktop/browser or external system-IME combination; test a disposable field in the desired application.
