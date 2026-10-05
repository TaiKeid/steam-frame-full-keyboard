# Architecture

Full Keyboard for Steam Frame is a native, manually launched overlay for dashboard and local-app input. It does not intercept the system keyboard button, suppress the stock keyboard, or route events to streamed VR apps. Internal namespaces, executable names, paths, and overlay IDs remain `framekeyboard`.

## Components

| Component | Responsibility | Implementation |
| --- | --- | --- |
| Configuration | Profile discovery, validation, selection, reload and persistence | Runtime JSON catalog with compiled fallback |
| Layout model | Physical key IDs, bounds, fallback labels and actions | File-defined geometry |
| Language model | Keymap, modifier-level legends, shaping and backend compatibility | Language profiles and XKB legends |
| Key state | Pointer capture, key transitions, modifiers, repeat and cancellation | C++20, independent of renderer/backend |
| Renderer | Draw key faces/sides/legends and animations | Cairo/Pango |
| VR panel | Own overlay, pose, size, pointer events and texture submission | OpenVR + Vulkan |
| Input backend | Translate physical actions into press/release events | Gamescope Unicode characters plus native libei controls; physical uinput remains available |

## Rendering choice

Cairo/Pango draw into a CPU image, and the vendored Vulkan texture transport keeps a persistent OpenVR-compatible texture. Provenance and licenses are recorded in [third-party sources](../third_party/README.md).

The renderer draws an image, then updates the GPU texture. The compositor reuses the submitted texture between updates. Short press animations need a bounded redraw loop; idle keys do not require a continuous CPU paint loop. The VR event loop targets a 16.667 ms period while visible, including its processing time, and 250 ms while hidden. It reads the dashboard pose every visible iteration; hidden tracking retries remain limited to 4 Hz. Hidden panels skip native controller-component queries. Stationary, fully aligned panels skip transform updates. These intervals control polling, not compositor presentation; SteamVR presents the existing texture independently.

The image is retained between paints. Hover/press/release updates clear and redraw only damaged key regions, including overlapping neighboring ink in the original drawing order. Cairo recording surfaces measure ink bounds when the visual model changes; the bounds cover the entire press travel and include text outside the face rectangle. Profile snapshots compare values rather than pointer addresses so Reload cannot reuse stale geometry. Modifier/lock legends, composition/candidates, settings, status, and profile changes use full redraws. Pixel-comparison tests run the same interaction sequence against an always-full renderer.

`PanelTexture` prefers Cairo's native premultiplied BGRA bytes on little-endian systems, using `VK_FORMAT_B8G8R8A8_UNORM` and `VROverlayFlags_IsPremultiplied`. It checks Vulkan format support and OpenVR flag acceptance, and falls back to straight RGBA conversion if setup or submission fails. Rejected native textures stay alive until VR shutdown. Both GPU images still receive full uploads; partial CPU painting does not introduce stale alternate textures. The visible 60 Hz pose loop and 80 ms Graphite animation duration are unchanged.

The HTML in `design/` is a design artifact only. It is not an embedded browser requirement for the native app.

## Configuration and selection

[Configuration](configuration.md) defines independent layout, language and theme profiles. Native rendering consumes the selected model, not hardcoded US keys or graphite colors. The VR settings panel uses the same catalog and validation as the host preview. Prepare a complete candidate before activation, release keys using the old mapping, and atomically replace geometry and hit regions. Persist the selection only after successful activation.

The language model validates required physical keys. The normal Frame path composes characters locally and sends Unicode. Only physical uinput and external JIS use an explicit `--target-language` declaration. Derived legends must match the symbols actually delivered. Locale names alone do not establish language support. Keep IME/composition and UI translation separate from physical key geometry.

## Input and focus

The installed launcher uses libei through the running Gamescope socket. A keyboard seat is bound, output starts only after device resume, and a pause or removed device clears held state. Resuming or receiving a replacement keyboard can restore typing; a disconnected socket requires reopening. An interruption flag survives a pause/resume pair within one dispatch, and blocks backend output until the app clears its old UI holds. Connection state is tracked even while the dashboard disables typing. Existing evdev key mapping is shared with uinput; compositor repeat replaces synthetic EV_KEY repeat in the libei path. Development previews remain input-disabled. Apply/Reload releases held keys and restores typing only for a live backend, available text transport and an explicitly enabled launch. Physical-only modes additionally check their target declaration; no pause/resume button is needed.

Use logical names such as `Enter`, `ControlLeft`, `KeyC`, and `NumpadEnter`. Every backend owns its mapping. Steam key-state values are not Linux evdev values. The native keyboard must not use Steam's contextual Enter callback for its dedicated Enter action.

uinput follows the compositor's input routing; it does not address a chosen window. A target overlay ID does not prove keyboard focus. Preserve focus while interacting with the panel and verify delivery in KDE and the outer floating-window session separately.

Copy/Paste are shortcut actions, initially Ctrl+C and Ctrl+V for browser targets. They do not read passwords or clipboard contents. Terminal shortcuts and clipboard sharing across sessions are separate requirements.

## Packaging and failure cleanup

Releases live under `~/.local/share/framekeyboard/releases`, with a `current` symlink and a launcher in `~/.local/bin`. Config lives under `~/.config/framekeyboard`. The package installs no service, browser bridge, startup hook, kernel module, or Steam patch.

Every held key has a cancellation path. Disconnect, focus loss, hiding, close, and drag capture release input. Overlapping controller presses share backend key references, including lock-key toggles. After a drag timeout or tracking loss, cleanup clears the application's dragging state even if the motion tracker already stopped. Cleanup is idempotent.

The stock keyboard remains untouched, so no stock-UI watchdog is needed. Per-user instance IPC accepts only a recenter request, not arbitrary input or text.

## Instance ownership and placement

VR launches acquire `VrInstance` before touching OpenVR. A later launch connects to the owner's private local socket, sends only a recenter request, waits for acknowledgment, and exits. The render loop handles requests on its own thread; it releases keys, returns to the keyboard view, resets placement beneath the current dashboard, and shows its own overlay. This avoids duplicate overlay keys and avoids reassigning SteamVR's generated application PID through a second VR connection.

`PanelPlacement` contains pure transform math with no VR calls. Its initial width is 0.95 m; recenter keeps the chosen width and applies a 50-degree backward pitch from upright, independent of headset pitch/roll. Recenter reads the live bottom center of `valve.steam.gamepadui.main` through `GetTransformForOverlayCoordinates`, normalizes its scaled basis, and puts the keyboard's top edge 6 cm below it with its center 24 cm toward the viewer. The hidden stock keyboard is deliberately ignored because its transform can lag behind dashboard movement. If the dashboard pose is unavailable, placement falls back to 0.85 m ahead and 0.65 m below the headset. While visible, dashboard movement updates the panel's relative pose at a target 60 Hz without repainting the texture. During a grab the hand owns the pose and updates its offset. The last dashboard anchor is saved with placement, so close/reopen follows subsequent dashboard movement. `PanelPlacement` stores the world transform directly; only recenter constructs a default rotation. The main-view resize icons queue bounded width changes without rebuilding the pose; controller grabbing handles positioning. The VR loop consumes them, updates the transform/width, and checks compositor visibility independently of its cached state. Tracking loss clears held keys; a valid pose after a brief loss restores visibility without moving the panel. Tracking-origin reset events still request recentering. `placement_store.cpp` validates and atomically persists the standing-space transform, width and tracking-universe ID on orderly shutdown. A cold launch restores the saved placement once tracking is valid. A different known universe recenters the pose while preserving saved width. An invalid file falls back to defaults. Legacy placement files without a dashboard anchor retain their world-space pose until reset. This is not a cross-room spatial anchor.

`IsDashboardVisible` gates both rendering and interaction. The loop checks it before pointer events and before repeat, releases held keys before disabling `InputGate`, cancels preedit/grabs and drains hidden pointer events. The gate also covers Apply, Reload and relaunch, so they cannot enable hidden output. The overlay does not set `MakeOverlaysInteractiveIfVisible`; it relies on dashboard interaction only and cannot deliberately claim scene inputs after closure. No active-VR-app backend is provided, so the panel remains hidden outside the dashboard.

Grip dragging captures a controller-to-panel transform at grab time, then composes each tracked controller pose with that fixed offset. Frame's dashboard masks modern grip actions and legacy controller-state polling. `GrabInput` instead reads the native `button_grip` render component for the known Frame controller models. A synthetic all-released controller state supplies its neutral component pose; the live component's relative rotation measures physical squeeze. `GripLatch` applies press/release hysteresis and requires a released sample after startup or a missing sample. This model-specific path must be rechecked when Valve changes controller models. It does not alter runtime input settings.

Laser hover gates a grab, and releasing grip, losing input/tracking, hiding or recentering cancels it. Only the captured controller owns movement; leaving the hover target during a drag does not release it. The UI cancels held keys and suppresses typing while dragging. The keyboard opens one OpenVR connection after checking that the VR server is already running.

`App::down` and `App::up` report accepted key press/release transitions. `App::move` reports entering a different key when that pointer is not holding a key. Only the VR layer sends feedback: 25 ms/full amplitude for press and 8 ms/0.35 amplitude for release, both at 150 Hz, 4 ms at 240 Hz/0.1 amplitude for hover. Clicks take priority over hover in the same frame. This keeps haptics out of rendering and input delivery; duplicate/rejected events, stationary hover, dragging and repeat do not pulse.

During a grab, `GrabInput` also reads the native thumbstick render component. `ComponentAxis` calibrates neutral/full-up rotations using synthetic axis states. It extracts the signed twist around that axis, excluding sideways tilt. The controller model’s tip supplies the laser direction. `PanelDrag` integrates only the captured controller’s vertical stick value into its relative translation with a dead zone, elapsed-time cap and depth bounds. This keeps rotation, size and the lateral grab offset intact. Grab input uses neither action manifests nor global input overrides.

`HorizonAlignment` corrects only local roll within 5 degrees of the standing-space horizon, using 500 ms smoothstep easing. It preserves the panel normal and center. The VR loop keeps raw placement separate from displayed placement so corrections never accumulate in the grab transform. Grabs capture the displayed pose and bypass horizon correction entirely. Normal grip release starts a fresh 500 ms animation, including when the hand was stationary before release. Leaving the range eases the correction out. Cancellation freezes the visible pose, recenter resets assistance, and shutdown saves the displayed pose. Panels facing straight up/down skip alignment because horizon roll is undefined.

`KeyHaptics` retains the controller owning each accepted pointer press and sends release feedback to that owner. Each controller has its own feedback queue; clicks take priority over hover, and hover is suppressed until a click ends. Release uses a shorter pulse because equal API values felt stronger on release on Frame. `VrHaptics` uses an output-only action manifest and `TriggerHapticVibrationAction` restricted to the event controller’s hand source. Missing/unknown controllers receive no pulse; there is no fallback to the overlay-wide laser haptic API. The haptic action set binds no controller inputs and changes no global input settings.

When an app tab such as Brave is active, Steam's hidden main dashboard overlay retains a stale transform. `DashboardAnchor` uses the main tab only while visible; the visible dashboard bar then carries that mount with its full translation and rotation across all tabs. Overlay scale is removed without flattening pitch or roll. The bar reference remains continuous until recentering to avoid orientation jumps on tab switches. Both anchor and bar pose are saved so reopening directly into an app preserves the offset. Without a prior calibration, initial placement falls back beneath the live bar. Invisible overlays are never accepted as current poses.

## Japanese composition

`JapaneseComposer` owns a bounded in-memory preedit, romaji parsing and UTF-8 kana editing. Anthy is loaded dynamically on first conversion. Its public C API provides segment candidates; no dictionary learning/commit API is called. Candidate controls, script changes and Enter update local state before committing. Composition keys use `KeyboardState` for pointer ownership, visuals, modifier consumption and haptics, with native emission/repeat suppressed for those presses. Raw Ctrl/Alt/Meta shortcuts retain their normal key path, but unfinished composition is committed before any native key-down. A failed commit keeps the preedit and blocks the forwarding action. Gamescope text commit uses a bounded synchronization roundtrip before a following key can be sent over the separate libei socket.

`EiSink` derives the Japanese text socket only from the documented `gamescope-N-ei` naming convention, preserving its directory and compositor number. Nonstandard key sockets require an explicit `--text-socket`; there is no fallback to gamescope-0 or the default Wayland display.

`GamescopeText` uses the pinned, generated version-1 input-method protocol. It feature-checks the manager/seat and handles unavailable connections. Connection handshakes have bounded waits; the running loop only flushes/dispatches readable events. `EiSink` creates this connection lazily, refuses text while native keys are held, and does not read target fields or clipboard data. Text and physical output share explicit launch gating. Commits are limited to 32 Unicode characters to stay below the compositor's temporary keymap capacity; local reading is limited to roughly 30 characters.

The renderer reserves space for preedit/candidates during Japanese, Chinese and Korean composition. Kana/shift labels come from the selected language profile. Settings, profile changes, target-focus loss, hiding, dragging and recentering discard unfinished composition. Laser FocusLeave releases only the identified controller's keys and retains composition because it does not change application focus. Failed text submission keeps the preedit for recovery. The protocol reports submission availability, not application acceptance; application coverage needs separate tests.

## Code map

| Area | Files |
| --- | --- |
| Configuration and validation | `src/config.cpp`, `include/framekeyboard/config.hpp` |
| Pointer/key ownership, XKB legends, uinput | `src/input.cpp` |
| Compositor key and text delivery | `src/ei_input.cpp`, `src/text_input.cpp` |
| Native rendering and hit regions | `src/panel.cpp` |
| Settings, profile changes, and input gating | `src/app.cpp` |
| Japanese reading and conversion | `src/japanese.cpp` |
| Preview and command-line entry | `src/preview.cpp`, `src/main.cpp` |
| Instance IPC | `src/instance.cpp` |
| Placement math and persistence | `src/placement.cpp`, `src/placement_store.cpp` |
| Grip model and haptic state | `src/grip.cpp`, `include/framekeyboard/feedback.hpp`, `include/framekeyboard/vr_haptics.hpp` |
| OpenVR lifecycle and event loop | `src/vr.cpp` |
| Offline speech recognition | `src/dictation.cpp`, `src/dictation_main.cpp`, `include/framekeyboard/dictation.hpp` |
| Tests and opt-in receiver probes | `tests/` |

## Offline speech-to-text dictation

`Dictation` supervises an independent helper executable (`framekeyboard-dictate`).
The helper links the vendored `whisper.cpp` engine with ARM NEON/SVE vectorization
and loads the bundled English `small.en` GGML model. Microphone audio is recorded
over PipeWire (`pw-record`) or ALSA (`arecord`) in raw 16 kHz 16-bit mono.

Communication uses an anonymous Unix domain socket pair. The parent sends
`stop\n` when dictation finishes; the child streams state events (`listening`,
`hearing`, `transcribing`, `text <result>`, `error <reason>`). When any cancellation
occurs (closing the dashboard, grabbing/dragging, switching windows, or pressing
another key), `Dictation::cancel` terminates the child and immediately releases
the recording device. Audio is streamed in memory and never written to disk; speech
transcripts and field values are never logged. Recognized text is delivered directly
as physical keystrokes with Shift management to bypass XWayland keymap synchronization races,
with multi-byte characters falling back to `GamescopeText`.

## Chinese and Korean composition

`CjkComposer` loads a bundled engine adapter on demand with `dlopen`. The
main executable has no PyZy/libhangul startup dependency. Each adapter links its
own system library and exposes a versioned factory through a private interface.
Adapters and host are built together; engine objects are destroyed before their
module is unloaded. A missing engine rejects Apply, or falls back to English at
startup without overwriting the saved profile.

The engine keeps bounded Pinyin or two-set Hangul state. PyZy supplies Chinese
candidates and script conversion; libhangul supplies Korean syllable assembly.
The app releases physical holds and gates commits using the same transport rules
as Japanese. Failed commits retain preedit. Space or a candidate selection commits
a completed Pinyin reading; Hangul word boundaries commit before forwarding keys.
Profile Apply prepares engines before changing the active model. Missing build
support or dictionaries leaves the prior profile usable.

PyZy's learning methods are not called. Candidate selection reads its conversion
and remaining phonetic text, saves local undo state, and rebuilds the remaining
reading. Its user-cache/config paths are non-directories, so the library cannot
read or persist personal history. Korean Backspace rebuilds from the bounded key
sequence, preserving jamo-level undo across resyllabification. Neither engine
changes the system IBus selection. See [language setup](languages.md).

## Independent Unicode character output

`LanguageMap::symbol` resolves the selected XKB profile without legend overrides.
Its compose state handles dead accents locally. `App` sends printable characters
through the Gamescope text connection and retains a failed commit for retry.
Local Caps/Num toggles share pointer ownership without changing compositor locks.
Text repeat is bounded and canceled with its pointer; native controls keep the
existing backend repeat path. Numpad navigation is sent as explicit Home/arrows/etc.

`EiSink` reads the advertised keymap once per keyboard device and tracks group
updates. Ctrl/Alt letter shortcuts and dedicated Copy/Paste search that map for
the intended Latin symbol, with a conventional physical fallback where metadata
is absent. This lookup neither changes the system keymap nor reads app text.
