# Implementation plan

## Product target

A native ARM64 keyboard for Steam Frame that appears through the existing system keyboard control, displays our custom layout, and delivers real key events to local desktop and floating applications. Enter remains Enter on our panel. The receiving application still decides whether Enter submits, searches, or inserts a newline.

The baseline geometry is full-size ANSI, including a function row, navigation cluster, arrows and numpad, with Copy and Paste at the far left. The visual reference is `design/index.html`. Speech, swipe typing, predictive text, PC-streamed VR input, and cross-session clipboard synchronization are outside the first release.

Layouts, languages and themes must load from user-editable config files. Provide profile selection, reload and favorite combinations inside VR, with persistent selection. US English and German are the first language validation targets; additional languages must not require renderer changes. See [the configuration contract](configuration.md).

## Version 0.4.0 follow-up

Add selectable Japanese romaji and direct-kana composition, native Anthy conversion and in-panel candidate/segment controls. Add a JIS layout/profile for existing system IMEs. Deliver composed UTF-8 through the feature-checked Gamescope input-method protocol while keeping physical shortcuts on libei. Preserve explicit launch opt-in, lazy engine loading and adaptive idle polling. Verify composition with fake sinks and native text with a dedicated receiver; real application coverage remains an acceptance task.

## Version 0.3.8 follow-up

Keep Settings accessible when a saved language mismatches the launch target. Freeze the target XKB definition across profile reloads. Release physically held modifiers immediately even when a chord key remains down. Reduce idle and hidden VR polling while retaining the interaction cadence and event-driven rendering.

## Version 0.3.7 follow-up

Remove Move / align and Pause typing from the toolbar. Add native minus/plus zoom icons for 5 cm width adjustments in the main view. Preserve explicit typing opt-in and target-language checks when applying or reloading profiles.

## Version 0.3.6 follow-up

Reduce horizon easing to 500 ms. Route haptics explicitly to the originating controller with output-only actions, retain press ownership for release, tune release strength independently, and prevent hover from extending clicks.

## Version 0.3.5 follow-up

Ease sideways lean to the horizon over one second when within 5 degrees. Preserve forward/back tilt and position, assist during grab and after release, and capture/save the displayed pose. Keep the subtle hover pulse from 0.3.4.

## Version 0.3.3 follow-up

Add thumbstick depth control on the grabbing controller, for either hand. Native component calibration recovers the vertical stick axis in the dashboard. Up moves along the laser away from the controller; down brings the keyboard closer. Dead zone, proportional speed and depth/time bounds protect against drift and stalled frames. Existing saved placement, relaunch recovery and haptics remain.

## Version 0.3.2 follow-up

Persist standing-space position, rotation, width and tracking-universe ID in a separate placement file on orderly close. Cold launch restores; an existing instance still responds to relaunch by recentering. Validate saved transforms and replace the file atomically. Brief tracking loss preserves placement. Add haptic clicks on accepted key press/release and a lighter pulse when entering a key through the native overlay API.

## Version 0.3.1 follow-up

Replaced the trigger handle with whole-keyboard grip movement. Frame's dashboard masks both legacy controller polling and modern grip actions, including the tested global-priority route. Native Frame render-component travel provides squeeze state without changing SteamVR settings. The user confirmed following the controller and staying in place on release. Added hysteresis and startup/reconnect release gating; retained independent tracked poses and key cancellation. Other controller models and future Valve model revisions require separate validation.

## Version 0.3.0 follow-up

Added native libei compositor input after the outer SteamVR session failed to discover the new uinput keyboard. The installed launcher now enables typing; development preview remains input-disabled. Added controller-laser dragging with preserved grab offset and cancellation. Dedicated Frame Xwayland receiver verified A, Enter and Ctrl+A. Physical controller dragging and both Brave modes still require user checks.

## Version 0.2.0 follow-up

Added per-user VR instance ownership and acknowledged relaunch-to-recenter requests before OpenVR initialization. Added Move / align controls for position, tilt/yaw/roll, size and Face me. Pure placement/IPC tests run in CTest; the opt-in VR probe checks actual overlay transforms. Recenter removes old position/angle offsets and preserves the running panel's width. Placement persistence remains future work.

## Version 0.1.0 implementation status

Implemented: runtime profile loading and persistence, native preview and PNG export, full-size US/international layouts, English/German legends, two themes, profile controls, key state, uinput, and a manually launched Vulkan/OpenVR panel. Host and Frame core tests, isolated Frame kernel-input tests and a short visible-overlay smoke test passed. Live controller interaction, Brave focus/delivery and stock takeover remain pending. See README for the supported launch path and limits.

## Milestones

### 0. Repository and build foundation

- [x] Local repository structure, build presets, project documentation.
- [x] Preserve the approved HTML mockup.
- [x] Define 106-key layout and graphite theme.
- [x] Validate layout bounds, IDs and non-overlapping hit areas during build.
- [x] Verify host executable and ARM64 cross-build; record the result in parent project notes.

### 1. Configuration foundation

- [x] Implement versioned runtime JSON loading for settings, layouts, languages and themes.
- [x] Discover bundled and user profiles, with stable IDs and validated overrides.
- [x] Provide a profile catalog and selection model shared by preview and VR UI.
- [x] Keep geometry, language mapping and appearance independent; no fixed key count.
- [x] Prepare transactional switching, release-all handling and atomic persistence.
- [x] Retain the working configuration on invalid files and provide a built-in fallback.

Exit condition: host checks load an added layout, language and theme without rebuilding, reject invalid profiles, and preserve selection across restart. No input injection is needed for this milestone.

### 2. Native rendering reference

- [x] Implement layout-to-canvas geometry and hit testing using the same coordinates.
- [x] Render the full keyboard to an image using Cairo and a bundled or verified target font.
- [x] Reproduce case, gradients, rounded caps, 3.5px sides and fixed-size 3px press travel.
- [ ] Render normal, hovered, pressed, latched and disabled states; no exposed upper side during a press.
- [x] Add a host preview mode and image export without any OS input injection.
- [ ] Compare the native image against the approved HTML before VR interaction work.

Exit condition: the native renderer matches the approved design and key hit regions match the visible caps. Redraw only on state changes and while animations are active.

### 3. Manual VR panel and input proof

This is the first device milestone. Prove routing with a small set of keys before implementing the full keyboard behavior.

- [x] Add a standalone OpenVR overlay with a Vulkan texture and pointer hit testing.
- [x] Keep the stock keyboard available; initially launch ours manually.
- [x] Add an input backend interface and a receiver that records only synthetic test events.
- [ ] Implement a uinput candidate and investigate input-device discovery/startup timing.
- [ ] Compare Steam's direct key-state API through an optional bridge if focus/routing requires it.
- [ ] Verify real Enter, Tab, Ctrl+A, Copy and Paste in both Brave modes with disposable text.
- [ ] Verify keyboard clicks preserve app focus and rapid press/release produces no duplicate events.

Exit condition: both target modes receive the intended press/release sequences through a selected backend. If this fails, resolve it before takeover and full-layout behavior.

### 4. Existing system button and stock takeover

- [ ] Observe keyboard open/close requests and target changes without inspecting text.
- [ ] Establish whether OpenVR global keyboard events cover Frame's current keyboard.
- [ ] Add the smallest Steam runtime bridge needed for lifecycle and presentation control.
- [ ] Keep the stock keyboard session alive if closing it loses the intended target.
- [ ] Align our panel with the stock keyboard and suppress stock rendering and pointer input.
- [ ] Verify that covering the panel creates no duplicate clicks or hidden stock actions.
- [ ] Restore stock UI on disable, native crash, bridge loss, and unsupported Steam interfaces.
- [ ] Avoid close/reopen loops when our panel closes or the target changes.

Exit condition: the normal system keyboard control opens ours in both target modes, with verified stock input blocking and recovery. No promise of replacement is made before this passes.

### 5. Complete interaction behavior

- [ ] Implement all US layout mappings, Shift/Caps/Num Lock, navigation, numpad and repeat.
- [ ] One-shot Ctrl/Alt/Shift, visible state, deliberate lock behavior and a release-all action.
- [ ] Send modifiers and their modified key through the same backend.
- [ ] Handle multiple controller pointers without duplicate presses or lost releases.
- [ ] Add size/placement settings and decide the location of close/settings controls with the user.
- [ ] Add VR selectors for layout, language and theme, favorites, preview and Reload profiles.
- [ ] Switch and persist profiles without restart; release held keys before mapping changes.
- [ ] Prove English and German output, Shift/AltGr legends and target keymap agreement in both Brave modes.
- [ ] Support font fallback and shaped legends; identify unsupported language/input-method capabilities in VR.
- [ ] Define and test dead-key, Compose, Unicode and IME support explicitly before advertising dependent languages.

Exit condition: interaction matrix passes, including long press, cancellation, window switching and all modifier release paths.

### 6. User-local packaging and recovery

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

The critical path is establishing configurable profiles, rendering a small native panel, proving input/focus, then proving takeover. A completed visual layout does not establish any of those integration results.
