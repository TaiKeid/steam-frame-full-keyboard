# Streamed PC games and apps

## Through Moonlight

Moonlight runs on the Frame as an ordinary window and sends the keys it gets to the PC that Sunshine streams, so Full Keyboard types into PC apps and games through it. Install Moonlight on the Frame from Flathub (`com.moonlight_stream.Moonlight`) and Sunshine on the PC.

Moonlight passes on keys, as the physical keys they are, and no text. So the keyboard sends:
- letters, digits and symbols, Enter, Backspace, the arrows and shortcuts as keys, as anywhere else. The PC reads them with its own layout, so symbols match when both use the same one; only US layouts on both were measured;
- other text, such as é, Cyrillic, emoji, and Japanese, Chinese and Korean, a character at a time through the Frame's clipboard and Moonlight's own Ctrl+Alt+Shift+V, which types the clipboard's text on the PC. Typed through Gamescope's keys, it arrived as digits, and a whole word pasted at once lost characters on a Linux PC. ASCII between such characters still goes as keys. The keyboard writes Gamescope's clipboard and never reads it; the last character stays there afterwards, and SteamOS's Desktop keeps its own clipboard. Gamescope reads every new clipboard itself, so the shortcut waits until no read has come for 30 ms, and only a reader that hadn't read before it counts: Moonlight. A character takes about 60 ms. One Moonlight doesn't read within 2 s, as in its own menus, which ignore the shortcut, is typed the usual way instead;
- a swipe alternative as Backspace taps, 20 ms apart, and then the word: Moonlight never sees Gamescope's own deletions, and takes a key pressed again at once for its repeat, which it drops;
- dictation's Send and Type as I speak's phrases typed, as above, even with Send set to Paste, since the PC types a paste itself.

Cut, Copy and Paste press Ctrl+X, Ctrl+C and Ctrl+V on the PC, so Paste pastes the PC's own clipboard. Moonlight keeps its own shortcuts, Ctrl+Alt+Shift with Q, Z, X and other letters, for itself.

The PC types pasted text itself. Sunshine on Windows types Unicode with the system's own Unicode input, which is expected to take whole words but wasn't tested. Sunshine on Linux types each character as Ctrl+Shift+U, its code and Enter, which only GTK apps and input methods such as IBus take, and which lost characters in a long paste into Firefox. Measured in November 2026 with Moonlight 6.2 on the Frame and Sunshine 2026.1103 on a Linux PC, with US layouts on both.

## Steam Link streams: not supported

Games streamed from a PC to the Frame through Steam Link (for example Path of Exile 2 or VRChat) keep using Steam's own keyboard. Full Keyboard leaves their keyboard requests native. The rest of this page records what was measured on a Steam Frame in October 2026, so the question can be revisited when SteamVR, Steam or Gamescope change.

## How Steam's keyboard reaches a streamed game

- The game runs on the PC. Libei and Gamescope input on the Frame never reach it.
- Its keyboard request arrives in Steam's VR keyboard status with overlay key `steamlink_openvr-overlay`, `bDispatchEventsToSteamVR` set and flags 13 (minimal, arrow keys, no Done key). Steam Link's client on the Frame owns that overlay and forwards keyboard events to the PC.
- For such a request, Steam's keyboard sends each key with `SteamClient.OpenVR.Keyboard.SendText`. It maps Backspace to `\b`, Enter to `\n`, Tab to `\t` and the arrows to `ESC[D`, `ESC[C`, `ESC[A`, `ESC[B`. Requests for Gamescope windows instead use `SteamClient.Input.ControllerKeyboardSendText`.
- Steam's UI exposes no other way to address the stream: `Keyboard.Show()` takes no target, and there is no public OpenVR call that sends events to another client's overlay.

## Why another keyboard cannot use that route

`SendText` can only be used through SteamVR's single keyboard session, and both states of that session break a separate keyboard. A dedicated test overlay opened Steam's keyboard for itself to measure them.

| Session | Where `SendText` text goes | Effect on the Frame |
| --- | --- | --- |
| Open | Only to the overlay that requested it | Correct, but any SteamVR laser click on another overlay moves the session to that overlay |
| Closed (after `Keyboard.Hide`) | To the last requesting overlay, and also as a global `VREvent_KeyboardCharInput` | Gamescope types every such event into its focused window |

- **Gamescope:** `src/Backends/OpenVRBackend.cpp` (`COpenVRBackend::ProcessVRInput`) reads keyboard character events from its global event loop. It ignores the target overlay and types the text through its always-present input method. A closed session therefore also typed the stream's text into a pinned Kate window.
- **Session moves on click:** with the session open, a laser click on our panel moved it from `steamlink_openvr-overlay` to `org.framekeyboard.panel`; the game's request then ended. The move appears to follow SteamVR's overlay input focus (Gamescope refers to a private `VREvent_OverlayInputFocusChanged`, 564). No public overlay flag prevents it.
- **Invisible Steam keyboard as a shield:** clicking Steam's own keyboard overlay, made invisible with its page ignoring clicks, also moved the session.
- **Laser passing through our panel:** clicks then landed on the theater window behind it. In those tests the session stayed, apparently because that window belongs to the same Steam Link overlay. The laser and every trigger pull then also reach the game, which is not usable.

## What another OpenVR client may do to Steam's keyboard overlay

`valve.steam.gamepadui.keyboard` has input method Mouse, `MakeOverlaysInteractiveIfVisible` set, and transform type Mountable. SteamVR draws its own frame and drag line around Mountable overlays.

From a separate OpenVR client:
- `HideOverlay` is refused with permission denied (12).
- `SetOverlayInputMethod`, `SetOverlayFlag`, `SetOverlayAlpha`, `SetOverlayWidthInMeters` and `SetOverlayTransformAbsolute` succeed.
- An absolute transform removes SteamVR's frame, and Steam makes the overlay Mountable again the next time its keyboard opens.

Whether Steam also resets alpha, width, input, flags and texel aspect on reopen was not measured. Any future use must restore them itself, including after a crash.

## What would make support possible

- **SteamVR:** a public way to send keyboard text to one overlay, or an overlay that can be clicked without taking keyboard focus.
- **Gamescope:** ignore keyboard character events addressed to other overlays (`data.keyboard.overlayHandle`). That would make the closed-session route safe.
- **PC helper:** a small companion on the PC that receives keys over the network. This needs an installation on the PC, which this project avoids.
- **Private interfaces or Steam Link's protocol:** undocumented and expected to break with updates.

## If this is revisited

Repeat the measurements above first, with a dedicated test overlay as the only recipient, never a real game or window. Check in this order:
1. whether a closed session still broadcasts text globally;
2. whether Gamescope still types global character events;
3. whether a laser click still moves an open session.
