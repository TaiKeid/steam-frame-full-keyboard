# FrameKeyboard

A native C++20 virtual keyboard for Steam Frame. Version 0.3.4 provides a manually launched VR panel, a desktop preview, configurable layouts/languages/themes, and native Gamescope input through libei, plus an optional Linux uinput backend. It uses Cairo/Pango for drawing and Vulkan/OpenVR for the VR panel. It does not embed a browser.

The full-size default includes real Enter, Ctrl/Alt, and left-side Copy/Paste. Keycaps have shallow raised sides and move down without stretching. The original approved HTML remains in `design/index.html` as a design reference.

## What works in this version

- Native rendering and controller mouse-event handling, with two pointer IDs.
- Close and reopen to restore position, rotation and size. Launch while running to recenter the existing panel. One VR process owns the keyboard.
- Grip-to-move anywhere on the keyboard: point the laser at it, hold grab, move/turn your hand, then release.
- Move / align controls for position, tilt, yaw, roll and size, plus Face me.
- US and international full-size layouts, English/German XKB legends, Graphite/Midnight themes.
- VR settings for layout, language and theme selection, favorites, reload and persistent selection.
- One-shot modifier taps, held chords, repeat, release-all, cancellation and a ten-second missing-release timeout.
- Firm haptic clicks on key press/release, with lighter feedback when entering a key.
- Native key events. Copy/Paste send Ctrl+C/Ctrl+V without reading the clipboard.
- Host preview, PNG export, profile validation and an ARM64 package.

The ARM64 core tests, offscreen rendering, isolated kernel-input test and VR overlay smoke test passed on Frame. Repeated-launch handling and placement controls passed on-device checks. Whole-keyboard grip movement and release were confirmed in the headset. The libei backend delivered A, Enter and Ctrl+A to a dedicated Frame Xwayland receiver. **Live controller typing and focus in KDE Brave and standalone floating Brave remain unverified.** This version does not replace the stock keyboard or intercept its summon button. It does not change Steam files, start SteamVR, or register autostart.

## Run on the development host

Build dependencies: CMake 3.24+, Ninja, Python 3, a C++20 compiler, pkg-config, libei, Cairo, Pango, libxkbcommon, json-c, SDL2, Vulkan and the SteamVR OpenVR library. Python is used only during the build. Pass `-DOPENVR_LIBRARY=/path/to/libopenvr_api.so` if SteamVR is installed elsewhere.

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
scp out/framekeyboard-0.3.4-aarch64.tar.gz steamos@steam-frame:/tmp/
```

On Frame, extract the package into a temporary directory and run its installer:

```sh
mkdir -p /tmp/framekeyboard-install
cd /tmp/framekeyboard-install
tar -xzf /tmp/framekeyboard-0.3.4-aarch64.tar.gz
./framekeyboard-0.3.4-aarch64/install.sh
```

The installer keeps releases under `~/.local/share/framekeyboard/releases`, provides `~/.local/bin/framekeyboard`, and adds a desktop menu entry. The menu entry opens the keyboard with native compositor input enabled, assuming the receiving session uses US English. Pause typing disables output. It preserves user profiles and does not enable autostart. See [runtime and removal](docs/runtime.md).

## Try native input on Frame

With SteamVR already running and the receiving session using a US keymap:

```sh
~/.local/bin/framekeyboard
```

Focus a disposable text field in the receiving app, then type on the panel. The normal launcher uses `--input ei --target-language en-us --start-enabled`. **Pause typing** disables output; **Resume typing** enables it again. Tap Ctrl/Alt/Shift to latch it for the next key; tap it again to clear it. Release all clears held and latched keys. Close destroys the panel and input connection.

`--target-language` is an explicit statement about the target session's existing keymap. The program does not detect or change that session's language. For German, the target must already use German; launch explicitly with `--vr --input ei --target-language de-de`, then choose the Deutsch favorite in Settings. That favorite includes the extra physical language key.

The selected profile must match the declared target language before input can be enabled. Editing or reloading a language file turns input off. IME composition, automatic target-keymap synchronization, lock-state synchronization with other keyboards and stock takeover are not implemented. The compositor backend connects to `/run/user/<uid>/gamescope-0-ei`; `--ei-socket` overrides that path. It works without restarting SteamVR. The optional uinput backend remains available, but the current outer VR session did not discover a newly created device. Development `--preview` and direct binary `--vr` remain input-disabled. For an installed VR preview, pass `--vr --input none`.

## Move, align and recover the keyboard

To move the keyboard, point the controller laser anywhere on it and hold the **grab/grip** button. Move or rotate your hand, then release grab to place it. The trigger keeps typing normally; there is no move button. The grab offset is preserved so the panel does not jump. Losing tracking/input, hiding, recentering, or holding longer than 30 seconds cancels capture. Only the grabbing controller moves it, and typing is suppressed during a grab. Release the grip first if you launched or reconnected while squeezing it. Native Frame controller models are supported; the app leaves SteamVR input settings unchanged.

While grabbing, push that controller’s **thumbstick up** to move the keyboard farther along the laser, or **down** to bring it closer. Either hand works; the other controller’s stick has no effect. Release the stick to stop changing depth, then release grab to place the keyboard.

Select **Move / align** in the top row. Position buttons move 2.5 cm per tap; rotation buttons turn 5 degrees; size buttons change width by 5 cm. **Face me** keeps the current position and size while turning the panel toward your head and leveling it. **Done** returns to the keys.

**Recenter**, **Reset position**, or launching the app while it is already running brings the keyboard in front of your current horizontal viewing direction, preserving its size. Closing saves position, rotation and size to `~/.config/framekeyboard/placement.json`; opening a new instance restores them. First launch, invalid saved data, or a different known tracking space falls back to recentering. Brief tracking loss hides the panel and preserves its position. A tracking-origin reset recenters it.

Accepted key presses and releases produce a firm haptic click through the controller laser. Entering a different key produces a lighter, higher-frequency pulse. Staying over the same key, dragging, toolbar controls, duplicate events and key repeat do not produce pulses.

Launching again sends a request to the existing process before connecting to SteamVR. It returns to the keys, releases held modifiers, and preserves your typing/pause choice. The first process keeps its language/backend launch options; later launch flags do not change them. Close the existing panel first if you need to change its input backend or target language.

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
| `src/ei_input.cpp` | Native compositor keyboard connection, lifecycle and release handling |
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
