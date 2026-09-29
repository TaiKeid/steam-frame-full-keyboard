# Implementation plan

## Product target

A native ARM64 keyboard for Steam Frame that appears through the existing system keyboard control, displays our custom layout, and delivers real key events to local desktop and floating applications. Enter remains Enter on our panel. The receiving application still decides whether Enter submits, searches, or inserts a newline.

The first supported layout is full-size US English, including a function row, navigation cluster, arrows and numpad, with Copy and Paste at the far left. The visual reference is `design/index.html`. Speech, swipe typing, predictive text, PC-streamed VR input, and cross-session clipboard synchronization are outside the first release.

## Milestones

### 0. Repository and build foundation

- [x] Local repository structure, build presets, project documentation.
- [x] Preserve the approved HTML mockup.
- [x] Define 106-key layout and graphite theme.
- [x] Validate layout bounds, IDs and non-overlapping hit areas during build.
- [x] Verify host executable and ARM64 cross-build; record the result in parent project notes.

### 1. Native rendering reference

- [ ] Implement layout-to-canvas geometry and hit testing using the same coordinates.
- [ ] Render the full keyboard to an image using Cairo and a bundled or verified target font.
- [ ] Reproduce case, gradients, rounded caps, 3.5px sides and fixed-size 3px press travel.
- [ ] Render normal, hovered, pressed, latched and disabled states; no exposed upper side during a press.
- [ ] Add a host preview mode and image export without any OS input injection.
- [ ] Compare the native image against the approved HTML before VR interaction work.

Exit condition: the native renderer matches the approved design and key hit regions match the visible caps. Redraw only on state changes and while animations are active.

### 2. Manual VR panel and input proof

This is the first device milestone. Prove routing with a small set of keys before implementing the full keyboard behavior.

- [ ] Add a standalone OpenVR overlay with a Vulkan texture and pointer hit testing.
- [ ] Keep the stock keyboard available; initially launch ours manually.
- [ ] Add an input backend interface and a receiver that records only synthetic test events.
- [ ] Implement a uinput candidate and investigate input-device discovery/startup timing.
- [ ] Compare Steam's direct key-state API through an optional bridge if focus/routing requires it.
- [ ] Verify real Enter, Tab, Ctrl+A, Copy and Paste in both Brave modes with disposable text.
- [ ] Verify keyboard clicks preserve app focus and rapid press/release produces no duplicate events.

Exit condition: both target modes receive the intended press/release sequences through a selected backend. If this fails, resolve it before takeover and full-layout behavior.

### 3. Existing system button and stock takeover

- [ ] Observe keyboard open/close requests and target changes without inspecting text.
- [ ] Establish whether OpenVR global keyboard events cover Frame's current keyboard.
- [ ] Add the smallest Steam runtime bridge needed for lifecycle and presentation control.
- [ ] Keep the stock keyboard session alive if closing it loses the intended target.
- [ ] Align our panel with the stock keyboard and suppress stock rendering and pointer input.
- [ ] Verify that covering the panel creates no duplicate clicks or hidden stock actions.
- [ ] Restore stock UI on disable, native crash, bridge loss, and unsupported Steam interfaces.
- [ ] Avoid close/reopen loops when our panel closes or the target changes.

Exit condition: the normal system keyboard control opens ours in both target modes, with verified stock input blocking and recovery. No promise of replacement is made before this passes.

### 4. Complete interaction behavior

- [ ] Implement all US layout mappings, Shift/Caps/Num Lock, navigation, numpad and repeat.
- [ ] One-shot Ctrl/Alt/Shift, visible state, deliberate lock behavior and a release-all action.
- [ ] Send modifiers and their modified key through the same backend.
- [ ] Handle multiple controller pointers without duplicate presses or lost releases.
- [ ] Add size/placement settings and decide the location of close/settings controls with the user.
- [ ] Load validated user layout/theme files at runtime with a built-in fallback.
- [ ] Decide Unicode/composition support explicitly; first-release US key events are not universal text insertion.

Exit condition: interaction matrix passes, including long press, cancellation, window switching and all modifier release paths.

### 5. User-local packaging and recovery

- [ ] Versioned ARM64 artifact, dependency/architecture verification and local staging.
- [ ] Reversible installer/uninstaller with backups and settings preservation.
- [ ] Choose service boundaries and ordering based on measured input-device discovery behavior.
- [ ] Start the input device before SteamVR only if testing demonstrates it is required; do not invent service dependencies or introduce cycles.
- [ ] Enable autostart only after explicit manual runtime and recovery checks pass.
- [ ] Check private interfaces on startup and restore stock behavior when unsupported.
- [ ] Verify normal restart, reboot, sleep/wake, Steam restart and a supported update transition.
- [ ] Record installation, verification and rollback in the top-level SteamFrame notes on both machines.

Exit condition: the user can install, disable, update and remove the keyboard without unlocking SteamOS, modifying Steam files, or getting stuck without a working keyboard.

## Main uncertainties

| Question | Evidence so far | Required proof |
| --- | --- | --- |
| Can the stock panel be hidden and blocked while preserving its target? | Popup and control interfaces exist. Closing can clear target state. | Real takeover and click/focus tests |
| Will a virtual device reach both Brave modes while our overlay is clicked? | uinput access verified; external project reports success | Dedicated receiver and two-mode validation |
| Does direct Steam key-state output provide better routing? | Live API exists; stock Paste calls it | Explicit code mapping and key delivery tests |
| Will keyboard events identify the right overlay? | OpenVR header defines target-bearing events | Observe current Frame runtime behavior |
| How are stuck keys and a hidden stock keyboard recovered after a crash? | No implementation yet | Independent recovery and crash tests |

The critical path is rendering a small native panel, proving input/focus, then proving takeover. A completed visual layout does not establish any of those integration results.
