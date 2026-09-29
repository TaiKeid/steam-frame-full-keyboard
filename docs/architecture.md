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
| Input backend | Translate physical actions into press/release events | Linux uinput; explicitly armed, matching target-language declaration required |
| Steam bridge | Request lifecycle, target metadata, stock suppression/restoration | Minimal JS through the existing local CDP interface, if needed |
| Recovery | Release keys and restore stock UI after native/bridge failure | Independent watchdog or lease owned by a surviving component |

## Rendering choice

Earlier discussion suggested OpenGL. The existing Frame performance overlay instead uses Cairo plus persistent Vulkan textures submitted through OpenVR. Its source documents flicker from replacing raw overlay images. Use its rendering strategy as a reference, while keeping this project independent. Confirm any reused code's license and pin its revision before copying it.

The renderer draws an image, then updates the GPU texture. The compositor reuses the submitted texture between updates. Short press animations need a bounded redraw loop; idle keys do not require a continuous CPU paint loop. Target measurements must determine the final render resolution and animation cadence.

The HTML in `design/` is a design artifact only. It is not an embedded browser requirement for the native app.

## Configuration and selection

[Configuration](configuration.md) defines independent layout, language and theme profiles. Native rendering consumes the selected model, not hardcoded US keys or graphite colors. The VR settings panel uses the same catalog and validation as the host preview. Prepare a complete candidate before activation, release keys using the old mapping, and atomically replace geometry and hit regions. Persist the selection only after successful activation.

The language model validates required physical keys. v0.1 uses the explicit `--target-language` declaration; automatic negotiation with the receiving session remains future work. Derived legends must match the symbols actually delivered. Locale names alone do not establish language support. Keep IME/composition and UI translation separate from physical key geometry.

## Input and focus

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

`PanelPlacement` contains pure transform math with no VR calls. The UI queues bounded position/rotation/size actions. The VR loop consumes them, updates the transform/width, and checks compositor visibility independently of its cached state. Tracking loss clears held keys; a valid pose after loss triggers recentering. Absolute room-space positions are not persisted.
