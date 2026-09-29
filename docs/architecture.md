# Architecture

Only the layout model and bootstrap executable exist today. The runtime components below are the implementation target.

## Components

| Component | Responsibility | Planned technology |
| --- | --- | --- |
| Layout model | Logical key IDs, bounds, labels and actions | JSON compiled to C++ initially; runtime loading later |
| Key state | Pointer capture, key transitions, modifiers, repeat and cancellation | C++20, independent of renderer/backend |
| Renderer | Draw key faces/sides/legends and animations | Cairo + font handling |
| VR panel | Own overlay, pose, size, pointer events and texture submission | OpenVR + Vulkan |
| Input backend | Translate logical actions into actual press/release events | Linux uinput candidate; direct Steam key-state alternative |
| Steam bridge | Request lifecycle, target metadata, stock suppression/restoration | Minimal JS through the existing local CDP interface, if needed |
| Recovery | Release keys and restore stock UI after native/bridge failure | Independent watchdog or lease owned by a surviving component |

## Rendering choice

Earlier discussion suggested OpenGL. The existing Frame performance overlay instead uses Cairo plus persistent Vulkan textures submitted through OpenVR. Its source documents flicker from replacing raw overlay images. Use its rendering strategy as a reference, while keeping this project independent. Confirm any reused code's license and pin its revision before copying it.

The renderer draws an image, then updates the GPU texture. The compositor reuses the submitted texture between updates. Short press animations need a bounded redraw loop; idle keys do not require a continuous CPU paint loop. Target measurements must determine the final render resolution and animation cadence.

The HTML in `design/` is a design artifact only. It is not an embedded browser requirement for the native app.

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
