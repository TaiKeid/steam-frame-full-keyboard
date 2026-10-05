# Changelog

## 0.6.1: Dictation safety and speed

- Dictated text is typed a few characters per frame instead of in one blocking loop. Closing the dashboard or changing focus stops the remaining text.
- Dictation waits while any key is held, so a held Ctrl or Super can no longer turn dictated letters into shortcuts.
- Pressing a keyboard key cancels active dictation.
- Held keys are released before typing is disabled. Previously a key could stay down if Gamescope text input became unavailable.
- Transcripts keep text after an unmatched "(", "[" or "*". Only closed annotations such as "[music]" are removed.
- Language profiles accept only plain XKB names, so a profile can't make the keymap library read other files.
- Dictation is much faster: the speech engine is built for the Frame's CPU features, and short recordings no longer pay for a full 30-second window. A 3-second clip with small.en now transcribes in under a second (was 17 s).

## 0.6.0: Offline Speech-to-Text Dictation

- Integrated on-device speech-to-text dictation button in the top toolbar with a dedicated microphone icon.
- Fully offline inference using whisper.cpp with ARM64 NEON/SVE hardware acceleration.
- Audio recording directly from the native PipeWire subsystem (`pw-record`) with an ALSA fallback (`arecord`).
- Isolated helper architecture (`framekeyboard-dictate`): microphone capture and model evaluation run in an independent child process.
- Cancellation safety: closing the dashboard, moving/grabbing the keyboard, switching windows, or pressing any key immediately stops recording and frees the microphone.
- Privacy guarantee: audio is streamed directly in memory and never written to disk; speech transcripts are never logged.
- Gamescope Unicode text delivery: recognized sentences are cleanly committed into any focused local application or desktop window.

## 0.5.0: First release

Full Keyboard for Steam Frame is a separately launched alternative keyboard for the Frame dashboard and local apps. It does not replace the stock keyboard or send input to streamed VR applications.

- Composition commits before ordinary spaces, digits and punctuation, with failed-delivery protection.
- Chinese/Korean engines load on demand; missing libraries leave the rest of the keyboard usable.
- Added French, Spanish, Italian, Brazilian ABNT2, Russian and Ukrainian profiles, Simplified/Traditional Pinyin candidates, and Korean two-set Hangul composition. Language settings and system-layout limitations are documented.
- Sticky left Super and momentary native right Super, with monochrome Frame and SteamOS icons.
- Full-size keyboard with F1–F12, arrow and navigation keys, numpad, Ctrl/Alt/Shift, and dedicated Copy/Paste buttons.
- Layout-independent Unicode character delivery, local Shift/Caps/AltGr and accents, plus native navigation, shortcuts and key repeat.
- Controller laser typing, per-controller press/release haptics, and subtle hover feedback.
- Grip movement and rotation, same-hand thumbstick depth adjustment, size controls, and horizon alignment after release.
- Dashboard position and rotation following, saved placement, and relaunch-to-recenter recovery.
- Dashboard-only visibility and input, with cancellation on hiding, tracking loss, or disconnect.
- Editable JSON layouts, languages, and themes, with in-VR selection, favorites, and profile reload.
- English and German profiles, plus Japanese romaji/kana composition, kanji candidates, and an external JIS IME mode.
- Graphite and Midnight themes, a native desktop preview, and image export.
- Incremental key redraws and native pixel uploads where supported, with an RGBA compatibility fallback and event-driven idle rendering.
- User-local ARM64 installation, preserved settings, versioned rollback, release checksums, and manual installation instructions.
- MIT-licensed project code, build documentation, and automated regression tests.

See the [README](README.md) for installation and controls, and [runtime documentation](docs/runtime.md) for language requirements and known limitations.
