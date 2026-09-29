# Runtime, tests and removal

## Frame launch

The `framekeyboard` launcher defaults to `--vr`. With no input argument, it creates no virtual input device. Use `--vr --input uinput --target-language en-us` for a US target session. The panel still starts with input off until the user enables it.

The panel starts world-fixed, 1.15 meters wide, 85 cm ahead and 25 cm below the headset. Recenter uses the current horizontal heading, removing head pitch and roll. A valid tracked headset pose is required before showing it. The panel hides and releases keys when tracking is lost, then recenters after tracking returns. Tracking-origin reset events also request recentering.

Move / align provides 2.5 cm position steps, 5-degree tilt/turn/roll steps, and 5 cm width steps. Face me changes orientation while preserving position. Recenter resets position and angle offsets, preserving the width of the running instance. Session adjustments are not written to disk.

VR mode uses a private per-user directory under `/run/user/<uid>/framekeyboard`. `vr.lock` stays on disk while the kernel releases its flock when the owner exits. The socket path is removed on normal exit or recovered under that lock after a crash. The next launch sends a bounded, acknowledged recenter request before loading profiles, creating input devices or initializing OpenVR. No launch depends on a saved PID.

A repeated launch returns to the keyboard, releases held keys and turns input off. It preserves the existing process's input-backend options. Subsequent arguments do not reconfigure that process. Closing the existing panel allows the next launch to use different options.

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
framekeyboard --vr --duration 4 --config-dir /tmp/framekeyboard-smoke
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

`vr-panel-probe` is an opt-in on-device check. It requires the exact PID of an input-disabled test instance and confirms overlay ownership before posting UI events. `--exercise PID` clicks only Move / align controls and checks the resulting overlay transform/width. After relaunching the keyboard, `--check-centered PID` verifies the same owner remains visible with level rotation. Do not run it on a user typing session or a different keyboard version.
