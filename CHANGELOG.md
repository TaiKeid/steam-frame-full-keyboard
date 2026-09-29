# Changelog

## 0.4.13

- Adopt **Full Keyboard for Steam Frame** as the project name and **Full Keyboard** as the app label. Internal `framekeyboard` paths remain compatible.
- Prepare public documentation, manual installation instructions, a native keyboard image, contribution guidance, and release checks.
- Make the ARM64 toolchain part of the repository, with an explicit external sysroot path.
- Package release checksums and project documentation.

## 0.4.12

- Restore typing after grab timeout or tracking loss.
- Preserve saved keyboard width when the tracking space changes.
- Toggle Caps Lock and Num Lock once for overlapping controller holds.

## 0.4.4–0.4.11

- Follow the dashboard at 60 Hz, including app tabs and full dashboard rotation.
- Align to the horizon only after releasing a grab.
- Refine the Graphite palette, toolbar and clipboard icons, and Japanese settings visibility.
- Remove Meta keys from bundled layouts while retaining custom-layout support.

## 0.4.0–0.4.3

- Add integrated Japanese romaji/kana composition, kanji conversion, and external JIS mode.
- Set the default width to 95 cm and desk-like tilt to 50° from upright.
- Follow the current dashboard and hide/release input when it closes.

## 0.1.0–0.3.8

- Add native rendering, configurable profiles, a desktop preview, and ARM64 packaging.
- Add compositor input, grip movement, same-hand thumbstick depth, and size controls.
- Add saved placement, relaunch-to-recenter, horizon assistance, and per-controller haptics.
- Add input cancellation, profile validation, and regression tests.
