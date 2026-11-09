# Acceptance checks

Use this checklist for a release candidate. Automated core tests and earlier headset checks do not complete every application combination. Fill the matrix for the specific artifact under review; do not infer untested results from the intended use case. Keep private device logs and machine-specific deployment notes outside the public repository.

## Visuals

- Full-size US arrangement, left Cut, Copy and Paste, separated navigation and numpad.
- Flat dark grey surface, white legends, rounded grey gradient keycaps.
- Shallow sides; fixed-size caps translate down on press without stretching or exposing a top side.
- Hover, press and modifier state remain distinguishable at actual headset size.
- Displayed legends and pointer hit regions align across supported scaling.

## Input matrix

Run against harmless test content, first in a dedicated receiver, then in Brave. Open the bundled [input check page](input-check.html) in each Brave mode. Its checklist exercises native controls and focus without logging typed characters or reading the clipboard.

Version 0.5.0: automated ARM64 engine tests cover Korean spaces/digits/punctuation and Chinese punctuation/digits, including failed commits. Missing-module/dependency recovery is tested separately. These automated checks do not substitute for the manual application rows below.

On-device report, 2026-09-29, version 0.5.0: the tester completed all six steps of the bundled input check page using the controllers in both KDE Brave and standalone floating Brave. The passes below are user-reported headset results, not inferred from automation.

| Check | KDE Brave | Standalone floating Brave |
| --- | --- | --- |
| Main Enter and numpad Enter deliver key-down/up | Passed | Passed |
| Tab and Shift+Tab change focus correctly | Passed | Passed |
| Ctrl+A, Copy, Paste | Passed | Passed |
| Ctrl/Alt combined with letters on the same backend | Passed | Passed |
| Arrow/navigation/numpad keys | Passed | Passed |
| Focus survives a keyboard pointer click | Passed | Passed |
| No doubled characters from stock keyboard | Passed | Passed |
| Switching apps releases modifiers | Passed | Passed |

The same tester also confirmed typing, Backspace, arrow navigation and Enter in Steam search and KDE Konsole on 0.5.0. These are basic application checks, not a full language-quality or long-session test.

Test a form that submits on Enter and a multiline field that inserts a newline. The keyboard must emit Enter consistently; it cannot force arbitrary websites to implement form submission.

## Configuration, languages and VR switching

- Add a differently sized layout and a custom theme through user JSON files. Reload and select both in VR without rebuilding or restarting.
- Switch layout, language and theme independently, and switch to an additional language in one step.
- Theme changes affect colors, gradients, geometry of keycaps and state styles while hit regions remain aligned. The approved default remains unchanged.
- Save selection across app restart and an application update; preserve user profile files.
- Reject malformed JSON, duplicate IDs, unsupported schema versions and incompatible language/layout/backend combinations. Keep the last working selection and display a useful error in VR.
- Switch while a modifier is latched or a key is repeating. Release the old mapping and deliver no unintended character to the app.
- Verify English and German output in both Brave modes, including Y/Z, umlauts, Shift, AltGr and shortcuts. Legends must match actual input.
- Check non-Latin font fallback and shaping in the renderer separately from actual language input support.
- Test dead keys, Compose, right-to-left text and IME composition for every profile that advertises them. Unsupported methods must be identified before activation.
- Reload edited profiles and simulate an unwritable settings directory. Preserve the usable runtime state and report failed persistence.

## Lifecycle and recovery

- Full Keyboard launches independently; the existing system keyboard button still opens the stock keyboard.
- Main-view minus/plus icons change width by 5 cm without moving the panel; grabbing handles position and rotation.
- Launching repeatedly retains one owner, restores visibility and recenters it. With replacement disabled, closing and reopening creates a fresh owner; abrupt exit leaves no permanent launch lock.
- Recenter clears position/angle offsets while preserving width, including after a tracking-space change.
- Tracking loss and the 30-second grab timeout restore typing.
- Grabbing with either hand disables horizon correction until release.
- Two controllers holding Caps Lock or Num Lock produce one toggle and one backend down/up pair.
- Closing the dashboard hides Full Keyboard and stops local typing without claiming streamed-app controls.
- Cancelled pointer presses, tracking loss, focus loss and close release keys.
- A crash leaves the stock keyboard usable and does not retain a stale instance lock.
- Input connection loss, runtime restart, sleep/wake, and reboot fail or recover predictably.
- Install/update/rollback/uninstall follow the documented steps and preserve user profiles.
- Reinstall the same production version and manually install an older package. Confirm `current` changes only after validation, the replaced marked release is removed, and unmarked/development directories survive. Refuse manual installation while Full Keyboard holds its instance lock. An acknowledged in-app update removes the replaced marked release and its snapshot; a failed start keeps the old release as `current`. Manual rollback reinstalls a package rather than switching a `previous` link.
- Unsupported runtime interfaces fail without changing Steam files or global input settings.

## Performance and privacy

Measure idle CPU, process memory, redraw time and pointer-to-feedback latency on Frame before setting budgets. Confirm no idle full-frame repaint loop and no texture-replacement flicker.

Typed text and clipboard contents must not appear in logs. Recovery must not paste or replay queued characters into a newly focused application.

## Emoji picker

See [emoji picker](emoji.md#acceptance) for the full list. In the headset:

- The emoji key opens the picker and Back returns; hiding, Settings and a lost Unicode connection return to the keys. Latched modifiers never reach an emoji.
- A single emoji, a toned emoji, a ZWJ family and a flag each arrive whole in a Steam field, a Wayland field and an X11 field.
- Dragging the grid and both stick paths (dashboard scroll events, and Stay open over apps) scroll without typing; holding never repeats.
- The first open never stalls the VR loop, cells fill in as images arrive, and an idle picker does not redraw.

## Settings and pin

- Confirm the toolbar order: Settings, Recenter, Pin, Stay open over apps, Focus mode with replacement on, swipe with Swipe typing on, haptic sounds with Haptic sounds on, Pull for Shift with Pull for Shift on, then language, always last. On a narrow keyboard confirm nothing covers the dictation editor. A pinned button changes icon without an active background.
- Set Haptic sounds to each mode and confirm effect and volume clicks preview it, keys pop, the toolbar button mutes without changing the setting, and pops stop while the microphone listens.
- Uncheck Key vibration in the Typing card, apply, and confirm hovering keys, releasing them and a Pull for Shift flick no longer pulse and a press leaves only the controller's own trigger click, while key sounds still pop and dictation's listening cues still tick; re-check it and confirm the pulses return.
- Turn on Pull for Shift. Pulling a key down types its Shift character, which slides into the key's middle while pulled; Space, Backspace, Enter and the arrows still act on press. The toolbar button pauses Pull for Shift and hides its characters, and keeps its state across a restart.
- With Extended key collision on, a tap in the gap between two keys types the nearer one; with it off, it types nothing. The flick layout's kana and digit pages keep their gaps.
- Corner resizers: aiming either laser just outside either bottom corner fades in a bracket there that follows the case's rounded corner; aiming away fades it out, and the pin hides both. Dragging either corner scales the keyboard about its center, release keeps the size, and keys at the corner still type while the resizer shows. Repeat over apps with the dashboard closed and at the smallest and largest sizes.
- Turn Num Lock on, close/reopen, and confirm its legends and mapping. Turn it off and repeat.
- In General check Hide numpad and Apply. Confirm the keys disappear and the case becomes narrower, with no laser/grab hits in the trimmed space. Change layouts and switch additional languages; visibility and Num Lock remain global. Show the numpad again and confirm its prior lock state.
- In General set Background to Invisible and Apply. Confirm the case behind the keys disappears while keycaps, legends and Settings itself stay opaque, with no stale case pixels after Back and Apply, and that pointing anywhere on the case, gaps included, still takes the laser. Change layouts and switch additional languages; the preference is global and survives restart. Set Semi-transparent and confirm the case is half see-through, without stale pixels after key presses, and takes the laser everywhere. Set Visible and confirm the case fills again.
- With Background Semi-transparent or Invisible, over a bright and a dark window: the message card above the keyboard, a Japanese reading line and its hint, the dictation counter and the emoji picker's hints stay readable, and the flick preview's caps stand apart from the keys beneath. Repeat with Stay open over apps.
- Toggle pin, close/reopen, and confirm the saved icon/state. Neither movement method can move a pinned keyboard; moving/tilting the dashboard still carries it. Unpin and confirm the selected movement method returns.
- Confirm the settings toolbar order is Back, Apply and save, Reload config. Production shows the Languages and Layouts, General, Typing and Dictation cards, with Integration reached by scrolling sideways.
- Open each dropdown, scroll through all choices, also by pressing and dragging the list, select one, and verify at most five rows are visible. Click outside a list and confirm only dismissal occurs. Verify laser hit positions match menu rows, including any rows beyond the case border.
- Edit several additional languages, leave Settings and choose Discard. Reopen and confirm edits were discarded. Repeat with Save and confirm persistence after restart. Drop a language from the list on Main and confirm the old main language becomes the first additional one.
- Cycle the main language plus additional languages, including saved legacy favorites that repeat a pair or match the main language. Confirm the active theme stays unchanged and cycling does not replace the saved main language.
- Hide/reopen the dashboard or disconnect a controller while a menu/scrollbar is held. Confirm captures clear and no stale release selects a new row.
- Run the `settings` test for extra-card horizontal scrolling, independent vertical scrolling, clipping, popup overflow and cancellation. Extra test cards do not ship in the UI.

## Movement behavior

- With Grab to move off, confirm that a white line appears below the unpinned keyboard. Hold the trigger over it, move and release. Verify release after pointing away from the line, two-hand ownership, cancellation on tracking loss and dashboard closure, and saved placement. Grip should not move the keyboard in this mode.
- In General, enable Grab to move and Apply. The line must disappear and grip movement must work as before, including thumbstick depth. Restart and Reload must preserve the preference; Back must discard a pending edit.
- Pin in each mode. Neither trigger-handle nor grip movement should work, and no line should remain visible or intercept the laser. Unpinning must restore only the selected movement method. Repeat with the numpad hidden and while settings is open.

- Compare the movement line with Steam: opaque #3d4450 idle, white while hovered/held, 10% horizontal growth with an 80 ms ease-out transition, and no grown pixels after leaving. Confirm the hit region stays stable and animation stops when idle or pinned.
- With Grab to move off and the keyboard unpinned, show the AUX head cursor, aim at the line, hold AUX, move your head, and release. Confirm that click/typing still works, another controller cannot steal/release capture, and head tracking loss or dashboard closure cancels movement. No haptic pulse should be sent for the head cursor.

- In General, confirm Hide numpad, Grab to move and Auto leveling labels sit at the left edge and their checkboxes share one column at the right edge. Auto leveling starts checked on a new or older config. Toggle it and use Back to discard; Apply and restart must retain it. With it off, place the keyboard at a small sideways angle and confirm it stays there after release in laser, AUX and grip modes. Re-enable it and confirm the existing 5°/500 ms correction resumes without a pose jump or pitch change. Reload must refresh the checkbox and behavior.

- With Grab to move off, hold the line with a controller trigger and leave grip released. That same hand's stick up must move the keyboard farther along the ray; down must bring it closer with the existing speed, dead zone and bounds. The other hand's stick must have no effect. Repeat with each controller, release, pin, hide the dashboard and lose tracking to verify depth stops with capture. Ordinary key hover/click must not move depth. An AUX head-cursor capture must ignore both controllers' sticks. Re-enable grip mode and verify its same-hand depth still works.

- Quickly click/release the movement line, then move the controller. A missed overlay mouse-up must not leave the keyboard following for the 30-second timeout. Initial stale rest samples must not cancel a genuine hold; release after an observed hold must stay immediate. Missing/invalid component samples must not count toward a released-sample window. Automated placement tests cover these timing sequences, and the isolated runtime harness verifies the production line-capture wiring.

## Resident native replacement

- Invoke the native keyboard button, then repeatedly click/type on our panel. Native keys, collision and move line must remain absent; neither panel may loop open/closed. X must dismiss ours and keep both panels dismissed. The native button must then reopen ours, and another press must dismiss it. Repeat with automatic Steam field focus and field Enter/close handling. External requests lacking an exact restorable owner must remain native.
- Enable replacement and Apply. Hold keys with both pointers, then X. All owned input releases, our panel hides, the process remains and native does not reappear for that dismissed session.
- Invoke a native keyboard request again. The same resident process shows our panel and completes a new readiness handshake. Repeat after closing/reopening the dashboard.
- Relaunch the app while its resident panel is hidden. The existing process reopens/recenters and Settings is reachable.
- Quit Full Keyboard is present only in Settings while replacement is enabled. Explicit Quit ends the process and restores native fallback; native summons remain usable afterward.
- Uncheck replacement and Apply. The Quit action disappears, normal X exits, and the original Steam button behavior is restored.
