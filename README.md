# FrameKeyboard

A native C++20 virtual keyboard for Steam Frame. Version 0.2.0 provides a manually launched VR panel, a desktop preview, configurable layouts/languages/themes, and an optional Linux uinput backend. It uses Cairo/Pango for drawing and Vulkan/OpenVR for the VR panel. It does not embed a browser.

The full-size default includes real Enter, Ctrl/Alt, and left-side Copy/Paste. Keycaps have shallow raised sides and move down without stretching. The original approved HTML remains in `design/index.html` as a design reference.

## What works in this version

- Native rendering and controller mouse-event handling, with two pointer IDs.
- Launch again to show and recenter the existing panel. One VR process owns the keyboard.
- Move / align controls for position, tilt, yaw, roll and size, plus Face me.
- US and international full-size layouts, English/German XKB legends, Graphite/Midnight themes.
- VR settings for layout, language and theme selection, favorites, reload and persistent selection.
- One-shot modifier taps, held chords, repeat, release-all, cancellation and a ten-second missing-release timeout.
- Optional native key events. Copy/Paste send Ctrl+C/Ctrl+V without reading the clipboard.
- Host preview, PNG export, profile validation and an ARM64 package.

The ARM64 core tests, offscreen rendering, isolated kernel-input test and VR overlay smoke test passed on Frame. Version 0.2.0 also tests repeated-launch handling and placement controls. **Live controller typing and focus in KDE Brave and standalone floating Brave remain unverified.** This version does not replace the stock keyboard or intercept its summon button. It does not change Steam files, start SteamVR, or register autostart.

## Run on the development host

Build dependencies: CMake 3.24+, Ninja, Python 3, a C++20 compiler, pkg-config, Cairo, Pango, libxkbcommon, json-c, SDL2, Vulkan and the SteamVR OpenVR library. Python is used only during the build. Pass `-DOPENVR_LIBRARY=/path/to/libopenvr_api.so` if SteamVR is installed elsewhere.

```sh
./scripts/build.sh host
ctest --test-dir build/host --output-on-failure
./build/host/framekeyboard --preview
./build/host/framekeyboard --render /tmp/framekeyboard.png
./build/host/framekeyboard --render-settings /tmp/framekeyboard-settings.png
./build/host/framekeyboard --check
```

The desktop preview never injects OS input. It exercises the same native renderer, key state and settings controls as VR. Use the Settings button to switch profiles; a preview is not a desktop replacement keyboard.

## Build and install on Frame

Build on CachyOS with the shared sibling ARM64 sysroot:

```sh
./scripts/package.sh
scp out/framekeyboard-0.2.0-aarch64.tar.gz steamos@steam-frame:/tmp/
```

On Frame, extract the package into a temporary directory and run its installer:

```sh
mkdir -p /tmp/framekeyboard-install
cd /tmp/framekeyboard-install
tar -xzf /tmp/framekeyboard-0.2.0-aarch64.tar.gz
./framekeyboard-0.2.0-aarch64/install.sh
```

The installer keeps releases under `~/.local/share/framekeyboard/releases`, provides `~/.local/bin/framekeyboard`, and adds a desktop menu entry. The menu entry opens a VR preview with input disabled. It preserves user profiles and does not enable autostart. See [runtime and removal](docs/runtime.md).

## Try native input on Frame

With SteamVR already running and the receiving session using a US keymap:

```sh
~/.local/bin/framekeyboard --vr --input uinput --target-language en-us
```

The panel starts with input off. Focus a disposable text field in the receiving app, then select **Input off** on the panel to enable typing. Tap Ctrl/Alt/Shift to latch it for the next key; tap it again to clear it. Release all clears held and latched keys. Close destroys the panel and virtual device.

`--target-language` is an explicit statement about the target session's existing keymap. The program does not detect or change that session's language. For German, the target must already use German; launch with `--target-language de-de`, then choose the Deutsch favorite in Settings. That favorite includes the extra physical language key.

The selected profile must match the declared target language before input can be enabled. Editing or reloading a language file turns input off. IME composition, automatic target-keymap synchronization, lock-state synchronization with other keyboards and stock takeover are not implemented. uinput hotplug/routing may require further Frame integration; do not restart SteamVR as part of an automated test.

## Move, align and recover the keyboard

Select **Move / align** in the top row. Position buttons move 2.5 cm per tap; rotation buttons turn 5 degrees; size buttons change width by 5 cm. **Face me** keeps the current position and size while turning the panel toward your head and leveling it. **Done** returns to the keys.

**Recenter**, **Reset position**, or launching the app again brings the keyboard in front of your current horizontal viewing direction. Position and angle adjustments reset, while the running instance keeps your chosen size. A new process starts at the default size. Placement adjustments are session-only in this release.

Launching again sends a request to the existing process before connecting to SteamVR. It returns to the keys, releases held modifiers, and turns input off. The first process keeps its language/backend launch options; later launch flags do not change them. Close the existing panel first if you need to change its input backend or target language.

## Customize inside VR

Copy a bundled JSON file into one of these directories, edit it, and select Settings → Reload profiles:

```text
~/.config/framekeyboard/config.json
~/.config/framekeyboard/layouts/*.json
~/.config/framekeyboard/languages/*.json
~/.config/framekeyboard/themes/*.json
```

Use a new `id` for an additional profile, or the same `id` to override a bundled one. Select the profiles with the arrow controls, then Apply and save. Invalid files leave the last valid version available and report an error. Deleting an active custom profile leaves the current session unchanged until another valid profile is selected.

[Configuration details](docs/configuration.md) cover fields, input-language constraints and fallback behavior. `config/default.json` is an example settings file; installation never overwrites an existing user config.

## Code map

| File | Responsibility |
| --- | --- |
| `src/config.cpp` | JSON validation, profile discovery and settings persistence |
| `src/input.cpp` | Physical key mapping, XKB legends, key state and uinput |
| `src/panel.cpp` | Cairo/Pango painting, press animation and shared hit testing |
| `src/app.cpp` | Profile controls, selection transactions and input arming |
| `src/preview.cpp` | SDL host preview |
| `src/instance.cpp` | Per-user instance lock and acknowledged recenter requests |
| `src/placement.cpp` | Head-relative recentering and world-space placement math |
| `src/vr.cpp` | OpenVR lifecycle, world placement, controller events and texture submission |
| `tests/keyboard_tests.cpp` | Input sequences, recovery, profiles, language legends and rendering |
| `tests/uinput_smoke.cpp` | Opt-in test against an exclusively grabbed, newly created device |

Comments explain the non-obvious boundaries: XKB versus evdev codes, modifier ownership, error cleanup, press geometry, pixel formats and compositor texture lifetime. `.clang-format` defines formatting for our C++ code. Third-party transport code retains its upstream style and license, with provenance in `third_party/README.md`.

[Implementation plan](docs/plan.md) · [Architecture](docs/architecture.md) · [Acceptance checks](docs/acceptance.md)
