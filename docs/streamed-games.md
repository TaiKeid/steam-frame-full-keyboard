# Streamed PC games and apps

## Through Moonlight

Moonlight runs on the Frame as an ordinary window and sends the keys it gets to the PC that Sunshine streams, so Full Keyboard types into PC apps and games through it. Install Moonlight on the Frame from Flathub (`com.moonlight_stream.Moonlight`) and Sunshine on the PC.

Moonlight passes on keys, as the physical keys they are, and no text. So the keyboard sends:
- letters, digits and symbols, Enter, Backspace, the arrows and shortcuts as keys, as anywhere else. The PC reads them with its own layout, so symbols match when both use the same one; only US layouts on both were measured. Gamescope types them with the layout Steam gives it, US by default, and doesn't tell the keyboard which: a symbol that needs AltGr there goes through Gamescope's temporary keymap and arrives as digits;
- other text, such as é, Cyrillic, emoji, and Japanese, Chinese and Korean, a character at a time through the Frame's clipboard and Moonlight's own Ctrl+Alt+Shift+V, which types the clipboard's text on the PC. Typed through Gamescope's keys, it arrived as digits, and a whole word pasted at once lost characters on a Linux PC. ASCII between such characters still goes as keys. The keyboard writes Gamescope's clipboard and never reads it; the last character stays there afterwards, and SteamOS's Desktop keeps its own clipboard. Gamescope reads every new clipboard itself, so the shortcut waits until no read has come for 30 ms, and only a reader that hadn't read before it counts: Moonlight. Moonlight reads twice, so the next character waits until its reads stop. A character takes under 0.1 s. One that no one reads within 2 s, as in Moonlight's own menus, which ignore the shortcut, goes through Gamescope's keys instead; one that only something else read is reported as not sent;
- a swipe alternative as Backspace taps, 20 ms apart, and then the word: Moonlight never sees Gamescope's own deletions, and takes a key pressed again at once for its repeat, which it drops;
- dictation's Send and Type as I speak's phrases typed, as above, even with Send set to Paste, since the PC types a paste itself.

Cut, Copy and Paste press Ctrl+X, Ctrl+C and Ctrl+V on the PC, so Paste pastes the PC's own clipboard. Moonlight keeps its own shortcuts, Ctrl+Alt+Shift with Q, Z, X and other letters, for itself.

The PC types pasted text itself. Sunshine on Windows types Unicode with the system's own Unicode input, which is expected to take whole words but wasn't tested. Sunshine on Linux types each character as Ctrl+Shift+U, its code and Enter, which GTK apps and Chromium-based browsers take, and which lost characters in a long paste into Firefox. Qt and KDE apps, such as Kate, show the code instead (`1f605` for 😅) unless an input method such as IBus or Fcitx5 runs on the PC. Measured in November 2026 with Moonlight 6.2 on the Frame and Sunshine 2026.1103 on a Linux PC, with US layouts on both.

## Through Steam Link: Streaming support

Games and apps streamed from a PC to the Frame through Steam Link take Full Keyboard with **Streaming support** on, in Settings → Integration. It is off by default and needs Replace native keyboard; the [installer](installer.md) asks about it on its last page. Without it, such a game keeps Steam's own keyboard, which types only basic Latin text into a stream.

Turning it on asks first, because it is the one feature that changes how Steam starts. It takes effect after SteamOS restarts once: Steam reads the setting only when it starts. Settings says so under the switch, and the installer offers the restart on its Finish page. Turning it off, or on again later, needs no restart and applies to the next stream; off, a stream that is running goes back to Steam's keyboard at once.

With it on:
- press SteamOS's keyboard button with the stream in front, as for any other window. With Full Keyboard already showing, click the game: Steam sends no keyboard request for a click on a stream, so the click itself moves typing there;
- letters, digits and symbols arrive as the keys a US layout types them with, and the PC reads them with its own layout;
- every key and shortcut goes through as a real press and release with its modifiers: Ctrl+C, Shift with the arrows, Esc, the function keys, and Ctrl or Alt held for a game's own bindings. Paste presses Ctrl+V on the PC, so it pastes the PC's clipboard. A key held on the keyboard is sent again as a release and a new press, because the streaming client drops a second press of a key it holds;
- other text, such as é, Cyrillic or Japanese, is sent as text. **A Windows PC types it**: Russian and whole Japanese phrases were measured. **A Linux PC does not**: there Steam ignores text sent to it, also from the Steam Link app on a phone, so only what keys type arrives. A Mac was not measured.

VR apps streamed from a PC stay outside: their link has no keyboard channel at all.

### Why Steam's keyboard types only Latin there

Steam's streaming client on the Frame is an SDL program that shows the stream through SDL's OpenVR video driver. For a character from SteamVR's keyboard that driver passes on only its first byte, as a key. The client itself and the Remote Play protocol carry any text; the PC never gets to see it. Measured in November 2026 with Steam's SDL 3.4.0 builds 1576 and 1616; SDL's own source had the same line.

### What Streaming support adds

- **A library**, `framekeyboard-stream.so` ([`stream/openvr_passthrough.c`](../stream/openvr_passthrough.c)), which the streaming client loads in place of Steam's OpenVR library. It names that library as its dependency, so every OpenVR call but one goes straight to Steam's. The one it wraps hands the client's event poll through unchanged, and after each poll gives the client what Full Keyboard sent: text as SDL text events, keys as SDL key events. Characters from Steam's own keyboard, or any other, pass as before.
- **A private socket**, `framekeyboard-stream.sock` in the user's runtime folder, between the keyboard's replacement helper and the library. Only a program of the same user is accepted. When the keyboard goes away, the library drops what is queued and releases every key it holds down.
- **A setting for Steam's start.** The client loads the library only when Steam was started with `SDL_OPENVR_LIBRARY` naming it. A drop-in for Steam's user service, `~/.config/systemd/user/steam.service.d/50-framekeyboard-stream.conf`, in the folder that service's own file names for user additions, runs the gate before each start and reads the answer it writes.
- **The gate**, `framekeyboard-stream-gate` ([`stream/stream_gate.c`](../stream/stream_gate.c)), which keeps `/run/user/<uid>/framekeyboard-stream/`:

| File | What it is |
| --- | --- |
| `steam.env` | The variable, written only when the library passed its check |
| `framekeyboard-stream.so` | A copy of the library, so removing the keyboard cannot take it from a running Steam |
| `openvr.so` | What the variable names: a link to the copy, or to Steam's own library while Streaming support is off |
| `off` | Tells a library that is already loaded to stay idle |

No file of Steam or SteamOS is changed. The folder is in the runtime directory, which the system empties at shutdown.

### What keeps it from breaking streaming

- **Steam's start never depends on it.** The drop-in first removes the last answer with a system tool, then runs the gate with a five-second limit and ignores how it ends. A missing, failing or slow gate leaves Steam exactly as it comes.
- **The library is checked before every Steam start**, after a Steam update too, since Steam restarts through its service: the gate loads it beside Steam's own OpenVR library in a process of its own, and writes the variable only if that works.
- **The file Steam is told about always exists while Steam runs.** It is a link in the runtime folder. Off, uninstalled or after a failed check, it leads to Steam's own library, so a stream starts as without Full Keyboard.
- **Without the keyboard the library does nothing.** Closed or crashed, it types nothing and lets go of held keys.
- **An interface it does not know is passed through untouched.** It wraps only the overlay interface version it was written for.

What remains: the library runs inside the streaming client, so a fault in it could end a stream. It is not expected to break by itself: the risk is a SteamOS or Steam update that changes what the library relies on. If streaming stops working after one, turn Streaming support off, which takes effect with the next stream, and wait for a Full Keyboard update. `uninstall.sh` and the installer's Uninstall turn it off the same way before removing the keyboard. To remove it by hand, delete the drop-in and restart SteamOS.

Measured on a Steam Frame in November 2026 with a Windows PC and a Linux PC as hosts.

## Steam's own keyboard route

The rest of this page records why Full Keyboard does not type into a stream the way Steam's keyboard does. It was measured on a Steam Frame in October 2026, before Streaming support, and explains why that needed its own way in.

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

## What would make the library unnecessary

- **SDL:** an OpenVR driver that passes on whole characters. That would let Steam's keyboard type any text into a stream, though not shortcuts.
- **SteamVR:** a public way to send keyboard text to one overlay, or an overlay that can be clicked without taking keyboard focus.
- **Gamescope:** ignore keyboard character events addressed to other overlays (`data.keyboard.overlayHandle`). That would make the closed-session route safe.

## If this is revisited

Repeat the measurements above first, with a dedicated test overlay as the only recipient, never a real game or window. Check in this order:
1. whether a closed session still broadcasts text globally;
2. whether Gamescope still types global character events;
3. whether a laser click still moves an open session.
