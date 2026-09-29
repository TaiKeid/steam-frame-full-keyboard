# Architecture

Version 0.1.0 implements the configuration catalog, key-state engine, native renderer, host preview and manually launched OpenVR panel. The uinput backend passed an isolated kernel-event test. Steam takeover, target-keymap detection and independent stock recovery remain unimplemented.

## Components

| Component | Responsibility | Planned technology |
| --- | --- | --- |
| Configuration | Profile discovery, validation, selection, reload and persistence | Runtime JSON catalog with compiled fallback |
| Layout model | Physical key IDs, bounds, fallback labels and actions | File-defined geometry |
| Language model | Keymap, modifier-level legends, shaping and backend compatibility | Language profiles; XKB mapping candidate |
| Key state | Pointer capture, key transitions, modifiers, repeat and cancellation | C++20, independent of renderer/backend |
| Renderer | Draw key faces/sides/legends and animations | Cairo + font handling |
| VR panel | Own overlay, pose, size, pointer events and texture submission | OpenVR + Vulkan |
| Input backend | Translate physical actions into press/release events | Native Gamescope libei or Linux uinput; matching target-language declaration required |
| Steam bridge | Request lifecycle, target metadata, stock suppression/restoration | Minimal JS through the existing local CDP interface, if needed |
| Recovery | Release keys and restore stock UI after native/bridge failure | Independent watchdog or lease owned by a surviving component |

## Rendering choice

Earlier discussion suggested OpenGL. The existing Frame performance overlay instead uses Cairo plus persistent Vulkan textures submitted through OpenVR. Its source documents flicker from replacing raw overlay images. Use its rendering strategy as a reference, while keeping this project independent. Confirm any reused code's license and pin its revision before copying it.

The renderer draws an image, then updates the GPU texture. The compositor reuses the submitted texture between updates. Short press animations need a bounded redraw loop; idle keys do not require a continuous CPU paint loop. The VR event loop targets a 16.667 ms period while visible, including its processing time, and 250 ms while hidden. It reads the dashboard pose every visible iteration; hidden tracking retries remain limited to 4 Hz. Hidden panels skip native controller-component queries. Stationary, fully aligned panels skip transform updates. These intervals control polling, not compositor presentation; SteamVR presents the existing texture independently.

The HTML in `design/` is a design artifact only. It is not an embedded browser requirement for the native app.

## Configuration and selection

[Configuration](configuration.md) defines independent layout, language and theme profiles. Native rendering consumes the selected model, not hardcoded US keys or graphite colors. The VR settings panel uses the same catalog and validation as the host preview. Prepare a complete candidate before activation, release keys using the old mapping, and atomically replace geometry and hit regions. Persist the selection only after successful activation.

The language model validates required physical keys. v0.1 uses the explicit `--target-language` declaration; automatic negotiation with the receiving session remains future work. Derived legends must match the symbols actually delivered. Locale names alone do not establish language support. Keep IME/composition and UI translation separate from physical key geometry.

## Input and focus

The installed launcher uses libei through the running Gamescope socket. A keyboard seat is bound, output starts only after device resume, and a pause/removal/disconnect disables input until reopen. Existing evdev key mapping is shared with uinput; compositor repeat replaces synthetic EV_KEY repeat in the libei path. Development previews remain input-disabled. Apply/Reload releases held keys and restores typing only for a live backend, matching target language and an explicitly enabled launch; no pause/resume button is needed.

Use logical names such as `Enter`, `ControlLeft`, `KeyC`, and `NumpadEnter`. Every backend owns its mapping. Steam key-state values are not Linux evdev values. The native keyboard must not use Steam's contextual Enter callback for its dedicated Enter action.

uinput follows the compositor's input routing; it does not address a chosen window. A target overlay ID does not prove keyboard focus. Preserve focus while interacting with the panel and verify delivery in KDE and the outer floating-window session separately.

Copy/Paste are shortcut actions, initially Ctrl+C and Ctrl+V for browser targets. They do not read passwords or clipboard contents. Terminal shortcuts and clipboard sharing across sessions are separate requirements.

## Takeover lifecycle

Proposed states: unavailable, stock available, replacement preparing, replacement active, restoring.

1. Observe a stock keyboard request and retain non-text target identity.
2. Prepare our overlay and establish that the input route is usable.
3. Suppress stock presentation and pointer interaction only when ours is ready.
4. Preserve Steam's logical keyboard session if required to retain routing.
5. On close, target loss or failure, release keys and restore stock presentation before discarding ownership state.

Do not equate opacity with pointer blocking. OpenVR restricts who can render into an existing overlay. Cross-process visibility/input setters and current keyboard event behavior require live verification. Prefer our own overlay with an owner-context bridge where necessary.

Independent recovery is required because a killed native process cannot restore a stock popup it previously suppressed. An expiring heartbeat lease in the bridge is a candidate. If a local communication endpoint is necessary, limit it to the current user, authenticate the bridge and validate messages; arbitrary webpages must not be able to request keystrokes.

## Packaging

Proposed install root: `~/.local/share/framekeyboard`, launcher under `~/.local/bin`, config under `~/.config/framekeyboard`, and user services under `~/.config/systemd/user`. Keep dependencies user-local where practical. Do not patch Steam's packaged assets or unlock SteamOS.

A persistent input device might need a service separate from the VR renderer so it can exist before SteamVR starts without requiring a live VR runtime. Decide after measuring discovery behavior. A systemd descriptor store is one candidate for surviving input-service restarts, not a settled design.

## Evidence boundary

The parent research note `../notes/keyboard-replacement-research.md` relative to the repository root records the inspected Steam bundle, live API checks, upstream references, and unverified items. It belongs to shared SteamFrame records and is intentionally not copied into this repository as session memory.

## Instance ownership and placement

VR launches acquire `VrInstance` before touching OpenVR. A later launch connects to the owner's private local socket, sends only a recenter request, waits for acknowledgment, and exits. The render loop handles requests on its own thread; it releases keys, returns to the keyboard view, resets placement from the current headset heading, and shows its own overlay. This avoids duplicate overlay keys and avoids reassigning SteamVR's generated application PID through a second VR connection.

`PanelPlacement` contains pure transform math with no VR calls. Its initial width is 0.95 m; recenter keeps the chosen width and applies a 50-degree backward pitch from upright, independent of headset pitch/roll. Recenter reads the live bottom center of `valve.steam.gamepadui.main` through `GetTransformForOverlayCoordinates`, normalizes its scaled basis, and puts the keyboard's top edge 6 cm below it with its center 24 cm toward the viewer. The hidden stock keyboard is deliberately ignored because its transform can lag behind dashboard movement. If the dashboard pose is unavailable, placement falls back to 0.85 m ahead and 0.65 m below the headset. While visible, dashboard movement updates the panel's relative pose at a target 60 Hz without repainting the texture. During a grab the hand owns the pose and updates its offset. The last dashboard anchor is saved with placement, so close/reopen follows subsequent dashboard movement. `PanelPlacement` stores the world transform directly; only recenter constructs a default rotation. The main-view resize icons queue bounded width changes without rebuilding the pose; controller grabbing handles positioning. The VR loop consumes them, updates the transform/width, and checks compositor visibility independently of its cached state. Tracking loss clears held keys; a valid pose after a brief loss restores visibility without moving the panel. Tracking-origin reset events still request recentering. `placement_store.cpp` validates and atomically persists the standing-space transform, width and tracking-universe ID on orderly shutdown. A cold launch restores the saved placement once tracking is valid. A different known universe or invalid file falls back to recentering. Legacy placement files without a dashboard anchor retain their world-space pose until reset. This is not a cross-room spatial anchor.

`IsDashboardVisible` gates both rendering and interaction. The loop checks it before pointer events and before repeat, releases held keys before disabling `InputGate`, cancels preedit/grabs and drains hidden pointer events. The gate also covers Apply, Reload and relaunch, so they cannot enable hidden output. The overlay does not set `MakeOverlaysInteractiveIfVisible`; it relies on dashboard interaction only and cannot deliberately claim scene inputs after closure. No active-VR-app backend is provided, so the panel remains hidden outside the dashboard.

Grip dragging captures a controller-to-panel transform at grab time, then composes each tracked controller pose with that fixed offset. Frame's dashboard masks modern grip actions and legacy controller-state polling. `GrabInput` instead reads the native `button_grip` render component for the known Frame controller models. A synthetic all-released controller state supplies its neutral component pose; the live component's relative rotation measures physical squeeze. `GripLatch` applies press/release hysteresis and requires a released sample after startup or a missing sample. This model-specific path must be rechecked when Valve changes controller models. It does not alter runtime input settings.

Laser hover gates a grab, and releasing grip, losing input/tracking, hiding or recentering cancels it. Only the captured controller owns movement; leaving the hover target during a drag does not release it. The UI cancels held keys and suppresses typing while dragging. The keyboard opens one OpenVR connection after checking that the VR server is already running.

`App::down` and `App::up` report accepted key press/release transitions. `App::move` reports entering a different key when that pointer is not holding a key. Only the VR layer sends feedback: 25 ms/full amplitude for press and 8 ms/0.35 amplitude for release, both at 150 Hz, 4 ms at 240 Hz/0.1 amplitude for hover. Clicks take priority over hover in the same frame. This keeps haptics out of rendering and input delivery; duplicate/rejected events, stationary hover, dragging and repeat do not pulse.

During a grab, `GrabInput` also reads the native thumbstick render component. `ComponentAxis` calibrates neutral/full-up rotations using synthetic axis states. It extracts the signed twist around that axis, excluding sideways tilt. The controller model’s tip supplies the laser direction. `PanelDrag` integrates only the captured controller’s vertical stick value into its relative translation with a dead zone, elapsed-time cap and depth bounds. This keeps rotation, size and the lateral grab offset intact. Grab input uses neither action manifests nor global input overrides.

`HorizonAlignment` corrects only local roll within 5 degrees of the standing-space horizon, using 500 ms smoothstep easing. It preserves the panel normal and center. The VR loop keeps raw placement separate from displayed placement so corrections never accumulate in the grab transform. Grabs capture the displayed pose; normal grip release lets the animation finish. Leaving the range eases the correction out. Cancellation freezes the visible pose, recenter resets assistance, and shutdown saves the displayed pose. Panels facing straight up/down skip alignment because horizon roll is undefined.

`KeyHaptics` retains the controller owning each accepted pointer press and sends release feedback to that owner. Each controller has its own feedback queue; clicks take priority over hover, and hover is suppressed until a click ends. Release uses a shorter pulse because equal API values felt stronger on release on Frame. `VrHaptics` uses an output-only action manifest and `TriggerHapticVibrationAction` restricted to the event controller’s hand source. Missing/unknown controllers receive no pulse; there is no fallback to the overlay-wide laser haptic API. The haptic action set binds no controller inputs and changes no global input settings.

## Japanese composition

`JapaneseComposer` owns a bounded in-memory preedit, romaji parsing and UTF-8 kana editing. Anthy is loaded dynamically on first conversion. Its public C API provides segment candidates; no dictionary learning/commit API is called. Candidate controls, script changes and Enter update local state before committing. Composition keys use `KeyboardState` for pointer ownership, visuals, modifier consumption and haptics, with native emission/repeat suppressed for those presses. Raw Ctrl/Alt/Meta shortcuts retain their normal key path.

`GamescopeText` uses the pinned, generated version-1 input-method protocol. It feature-checks the manager/seat and handles unavailable connections. Connection handshakes have bounded waits; the running loop only flushes/dispatches readable events. `EiSink` creates this connection lazily, refuses text while native keys are held, and does not read target fields or clipboard data. Text and physical output share explicit launch gating. Commits are limited to 32 Unicode characters to stay below the compositor's temporary keymap capacity; local reading is limited to roughly 30 characters.

The renderer reserves space for preedit/candidates only during Japanese composition. Kana/shift labels come from the selected language profile. Settings, profile changes, target-focus loss, hiding, dragging and recentering discard unfinished composition. Laser FocusLeave releases held keys but retains composition because it does not change application focus. Failed text submission keeps the preedit for recovery. The protocol reports submission availability, not application acceptance; application coverage needs separate tests.
