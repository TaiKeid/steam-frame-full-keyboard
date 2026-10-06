# Changelog

## 0.6.0: Settings and keyboard preferences

- Rebuilt Settings as separate cards for languages/layouts and appearance, with horizontal card scrolling and independent vertical scrolling within each card.
- Added Layout, Language, and Theme dropdowns with five visible choices and scrolling. Menus can extend beyond the keyboard case and dismiss when clicked outside.
- Moved Back, Apply and save, and Reload config into the Settings toolbar.
- Added a Favorite checkbox for layout/language pairs. When favorites exist, a main-toolbar language button cycles the saved default and unique favorites while retaining the active theme and saved default.
- Added a persistent pin button after the zoom controls to prevent accidental grip movement while retaining dashboard following, resizing, and recentering.
- Added global Num Lock persistence across restarts and profile changes, plus a Hide numpad preference that narrows the case while keeping the remaining key sizes.
- Fixed native keypad output disagreeing with restored Num Lock in physical uinput and external JIS modes. Digits and decimal wait for confirmed target lock state; keypad Enter and operators remain usable without lock feedback.
- Fixed toolbar buttons overlapping Close when hiding the numpad in custom layouts.
- Made favorite cycling faster and moved preference writes off the input thread, with ordered saves, shutdown flushing, and save-error reporting.
- Fixed fractional dropdown scrolling and preserved precise wheel input in the desktop preview.
- Added a British English layout and language profile, contributed by [Cr0ss0vr](https://github.com/cr0ss0vr).

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
