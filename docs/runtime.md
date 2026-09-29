# Runtime, tests and removal

## Frame launch

The installed launcher and menu entry default to `--vr --input ei --target-language en-us --start-enabled`. Typing is ready immediately; there is no pause toggle. The native libei connection uses Gamescope's existing `/run/user/<uid>/gamescope-0-ei` socket. No SteamVR restart, browser bridge or kernel device is needed. `--ei-socket` overrides the socket path. The launcher reads `FRAMEKEYBOARD_TARGET_LANGUAGE` if a different default target language is needed; it does not change the receiving session's keymap.

Explicit backend launches stay input-disabled unless `--start-enabled` is included. Development preview/PNG operations and direct binary `--vr` never type by default. Use `framekeyboard --vr --input none` for an installed input-disabled VR preview. The optional `--input uinput` backend requires matching `--target-language`, but newly created devices were not discovered by the current outer SteamVR session.

libei owns only a keyboard-capable device. It sends evdev down/up transitions; repeat belongs to the compositor. A paused, removed or disconnected device disables typing, clears held state, and requires reopening the app. No events or typed text are logged.

Without a saved placement, the panel starts world-fixed, 1.15 meters wide, 85 cm ahead and 25 cm below the headset. Recenter uses the current horizontal heading, removing head pitch and roll. A valid tracked headset pose is required before showing it. The panel hides and releases keys when tracking is lost, then returns to its position when tracking resumes. Tracking-origin reset events also request recentering.

Point the laser anywhere on the keyboard and squeeze the controller grab/grip button to move and rotate it. Release grab to place it. There is no move handle and trigger clicks remain typing actions. Frame's dashboard masks both legacy controller input and modern grip actions. The native render-model API still exposes physical grip travel through the `button_grip` component. This version supports the named left/right Frame controller models, with 1-degree press and 0.5-degree release thresholds across their 9.5-degree travel. Touch alone does not grab. Startup or reconnect while squeezed requires a release before grabbing. Unknown models or missing component state refuse capture; Recenter remains available. This path does not change SteamVR's global input settings. Tracked poses are read independently; the initial controller-to-panel transform preserves the grabbed offset. Input/tracking loss, hide, recenter and a 30-second timeout end capture.

The minus/plus zoom icons in the main toolbar change width by 5 cm per click, within 45 cm to 2 m, without changing position. Recenter resets position and angle offsets while preserving width. Closing saves the current placement and width.

VR mode uses a private per-user directory under `/run/user/<uid>/framekeyboard`. `vr.lock` stays on disk while the kernel releases its flock when the owner exits. The socket path is removed on normal exit or recovered under that lock after a crash. The next launch sends a bounded, acknowledged recenter request before loading profiles, creating input devices or initializing OpenVR. No launch depends on a saved PID.

A repeated launch returns to the keyboard and releases held keys while keeping the input connection unchanged. It preserves the existing process's input-backend options. Subsequent arguments do not reconfigure that process. Closing the existing panel allows the next launch to use different options.

SIGINT, SIGTERM, Close, focus loss, hide and cancellation release held keys. A ten-second hold limit also covers lost pointer-up events. A killed process loses its uinput device when the kernel closes the descriptor. The program never suppresses the stock keyboard, so no separate stock-UI recovery service is needed in this version.

Copy and Paste are browser-style Ctrl+C and Ctrl+V. They do not read clipboard data, synchronize clipboards between Frame sessions, or implement terminal Ctrl+Shift shortcuts. A shortcut press is ignored while another native chord is held, preventing an unintended combination from a second pointer.

Keyboard lock indicators track this panel's actions. They are not synchronized with a physical keyboard or an already enabled session Caps/Num Lock. Language composition and actual delivered text depend on the receiving session's keymap. Start tests with harmless text, including Enter in a form and in a multiline field.

## Automated tests

On CachyOS:

```sh
./scripts/build.sh host
ctest --test-dir build/host --output-on-failure
```

Cross-build and copy only test artifacts to Frame:

```sh
./scripts/build.sh frame-arm64
scp build/frame-arm64/keyboard-tests build/frame-arm64/uinput-smoke-test steamos@steam-frame:/tmp/
ssh steamos@steam-frame /tmp/keyboard-tests
ssh steamos@steam-frame /tmp/uinput-smoke-test
```

The second test opens only the event node of its own newly created virtual keyboard. It obtains an exclusive EVIOCGRAB before sending Enter, Ctrl+C and Alt+Tab sequences, compares the received kernel events, and destroys the device while it is still grabbed. If the grab fails, it sends nothing. It is deliberately not included in ordinary CTest runs.

For an input-disabled VR smoke test:

```sh
framekeyboard --vr --input none --duration 4 --config-dir /tmp/framekeyboard-smoke
```

With an instance already running, this command only recenters that instance; the duration argument does not close it. For an isolated smoke test, close the existing panel first. The log reports whether the overlay became visible. That is not a controller-interaction or application-focus test. Successful kernel delivery also does not establish SteamVR hotplug discovery or Brave delivery. Test both Brave modes manually before enabling any stock replacement work.

## Packaging and rollback

The installer checks architecture and validates the embedded profiles before changing the `current` symlink. Release directories are immutable by convention. It refuses to overwrite an existing release number. Keep any prior `current` link as `previous`; launching the new version does not migrate or erase user profiles.

To roll back while the keyboard is closed, point `~/.local/share/framekeyboard/current` at the previous release. The wrapper and desktop entry follow that link. No system files or services need restoring.

To remove, close FrameKeyboard, then delete its launcher, desktop entry and release directory:

```sh
rm ~/.local/bin/framekeyboard
rm ~/.local/share/applications/framekeyboard.desktop
rm -r ~/.local/share/framekeyboard
```

Keep `~/.config/framekeyboard` to preserve custom profiles. Removing the application never requires deleting those files. No Steam assets or system services are modified by the installer.

## Relaunch and placement regression tests

`placement-instance-tests` is part of CTest. It covers consecutive launches, normal cleanup, stale-socket recovery after abrupt exit, leveling/recentering, translation/rotation/size bounds, and the panel's control dispatch.

`vr-panel-probe` is an opt-in on-device check. It requires the exact PID of a direct-binary test instance with no `--input` or `--start-enabled` flags and confirms overlay ownership before posting UI events. `--exercise PID` clicks the main-view resize icons and verifies width changes without position changes. After relaunching the keyboard, `--check-centered PID` verifies the same owner remains visible with level rotation. Do not run it on a user typing session or a different keyboard version.

`ei-target-probe` is an opt-in Frame test, excluded from CTest. It maps its own disposable Xwayland window, verifies that exact window has keyboard focus before every emitted event, and checks A, Enter and Ctrl+A down/up delivery through Gamescope. It does not read other windows, clipboard data or browser fields. Core tests also cover enabled launch, pause on backend loss, typing preservation across relaunch, drag key suppression, no-jump grabs, controller translation/rotation and recenter after dragging.

## Placement persistence and key feedback

Closing the keyboard atomically saves its position, rotation and size in the selected config directory’s `placement.json`. Opening it from a stopped state restores that pose. Opening the app while its process already runs requests recentering and keeps its size. Brief tracking loss preserves placement; a known tracking-space mismatch or origin reset recenters it. Corrupt saved data is ignored. SIGKILL cannot save the latest movement.

Key presses use 25 ms/full amplitude and releases use 8 ms/0.35 amplitude, both at 150 Hz, through `TriggerHapticVibrationAction`, restricted to the controller that owns the key press. Entering a different key uses a 4 ms, 240 Hz pulse at 0.1 amplitude. Preview keys also provide feedback. Stationary hover, repeat, toolbar controls, canceled/duplicate events and dragging do not pulse. Hover from a pointer holding a key is suppressed, and clicks take priority per controller within a frame. Hover cannot extend a click into a later frame. Haptic failure logs once and does not prevent typing.

The live probe also supports `--snapshot PID FILE` to capture pose/width read-only, `--check-snapshot PID FILE` to compare a reopened input-disabled instance, and `--watch-close PID FILE` to observe its final live pose while the user closes it. The latter times out after 60 seconds. Synthetic clicks still require an input-disabled instance.

## Thumbstick depth while grabbing

Use the capturing controller’s stick: up moves farther along its laser, down moves closer. A 20% dead zone prevents drift; movement ramps to 0.65 m/s at full deflection. Normal depth limits are 0.2–3 m from the controller along its ray. An already out-of-range placement is preserved at capture and can move back toward that range. Elapsed time is capped at 50 ms per update to prevent jumps after a stall. Stick input never affects an ungrabbed panel, and the other hand cannot change depth. Rotation and size stay unchanged. Closing saves the resulting position as usual.

`stick-component-probe` validates signed vertical-axis extraction against synthetic stick positions on both installed Frame render models. It performs model calculations only, without sending input or changing overlays.

The packaged `vr/actions.json` and `vr/frame-controller.json` bind only haptic output. No global input-priority change is needed. The opt-in `haptics-probe ACTION_MANIFEST` sends a press/release pair to the left controller, then a pair to the right, and reports API results. It sends no key input and is excluded from CTest. API success must be distinguished from confirmation of the physical vibration.
