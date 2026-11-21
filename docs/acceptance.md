# Acceptance checks

Use this checklist for a release candidate. Automated core tests and earlier headset checks do not complete every application combination. Fill the matrix for the specific artifact under review; do not infer untested results from the intended use case. Keep private device logs and machine-specific deployment notes outside the public repository.

## Visuals

- Full-size US arrangement, left Cut, Copy and Paste, separated navigation and numpad.
- Flat dark gray surface, white legends, rounded gray gradient keycaps.
- Shallow sides; fixed-size caps translate down on press without stretching or exposing a top side.
- Hover, press and modifier state remain distinguishable at actual headset size.
- Displayed legends and pointer hit regions align across supported scaling.
- An edge light along each key's top edge, keys that type no character a tone darker, the Shift character quieter than the key's own, a held key sinking into shade and a shaded latched fill; High Contrast keeps plain keys and Neon its tubes.
- With Hover fade on, a key the laser leaves dims over a moment; turned off in General, it goes dark at once, and High Contrast keys never fade. The swipe trail is a quiet path with a comet and fades out at release.
- With the laser on a toolbar icon button, or on Back, Apply and save or Reload config, a tooltip above the case names it and goes as soon as the laser leaves; the language button has none, and only one tooltip shows at a time.

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
- The corner resizers change width about the panel's center; the movement line and grabbing handle position and rotation.
- Launching repeatedly retains one owner, restores visibility and recenters it. With replacement disabled, closing and reopening creates a fresh owner; abrupt exit leaves no permanent launch lock.
- Recenter clears position/angle offsets while preserving width, including after a tracking-space change.
- Tracking loss and the 30-second grab timeout restore typing.
- Grabbing with either hand disables horizon correction until release.
- Two controllers holding Caps Lock or Num Lock produce one toggle and one backend down/up pair.
- Ctrl, Alt or Super held with one controller stays down over several keys from the other: Alt+Tab steps through windows in a local app and in a stream from a PC. Shift and AltGr go down with each key.
- Holding Shift, Ctrl, Alt, AltGr or the left Frame icon: a tap shows no ring; the ring appears after about 0.15 s, fully by 0.25 s, and goes round clockwise from 12 o'clock, the key locks at 0.5 s with a double tick in that hand, and the ring closes into a border at 0.6 s. A double tap within 0.3 s locks at once. A locked Alt with Tab tapped steps through windows with one hand; a tap on any locked key unlocks them all and lets them up. Hiding the keyboard ends every lock and lets the keys up. The ring and border read on Graphite, Porcelain, Neon and High Contrast.
- Closing the dashboard hides Full Keyboard and stops local typing without claiming streamed-app controls.
- Canceled pointer presses, tracking loss, focus loss and close release keys.
- A crash leaves the stock keyboard usable and does not retain a stale instance lock.
- Input connection loss, runtime restart, sleep/wake, and reboot fail or recover predictably.
- Install/update/rollback/uninstall follow the documented steps, with `install.sh` and with the [installer](installer.md), and preserve user profiles.
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

- Confirm the toolbar order: Settings, Recenter, Pin, Stay open over apps, Focus mode with replacement on, swipe with Swipe typing on, haptic sounds with Haptic sounds on, Pull for Shift with Pull for Shift on, the shortcut bar button with Shortcut bar on, then language, always last. On a narrow keyboard confirm nothing covers the dictation editor. A pinned button changes icon without an active background.
- Set Haptic sounds to each mode and confirm effect and volume clicks preview it, keys pop, the toolbar button mutes without changing the setting, and pops stop while the microphone listens.
- Uncheck Key vibration in the Typing card, apply, and confirm hovering keys, releasing them and a Pull for Shift flick no longer pulse and a press leaves only the controller's own trigger click, while key sounds still pop and dictation's listening cues still tick; re-check it and confirm the pulses return.
- Turn on Pull for Shift. Pulling a key down types its Shift character, which slides into the key's middle while pulled; Space, Backspace, Enter and the arrows still act on press. The toolbar button pauses Pull for Shift and hides its characters, and keeps its state across a restart.
- With Extended key collision on, a tap in the gap between two keys types the nearer one; with it off, it types nothing. The flick layout's kana and digit pages keep their gaps.
- Corner resizers: aiming either laser just outside either bottom corner fades in a bracket there that follows the case's rounded corner; aiming away fades it out, and the pin hides both. Dragging either corner scales the keyboard about its center, release keeps the size, and keys at the corner still type while the resizer shows. Repeat over apps with the dashboard closed and at the smallest and largest sizes.
- Turn Num Lock on, close/reopen, and confirm its legends and mapping. Turn it off and repeat.
- In General check Hide numpad and Apply. Confirm the keys disappear and the case becomes narrower, with no laser/grab hits in the trimmed space. Change layouts and switch additional languages; visibility and Num Lock remain global. Show the numpad again and confirm its prior lock state.
- In General set Background to Invisible and Apply. Confirm the case behind the keys disappears while keycaps, legends and Settings itself stay opaque, with no stale case pixels after Back and Apply, and that pointing anywhere on the case, gaps included, still takes the laser. Change layouts and switch additional languages; the preference is global and survives restart. Set Semi-transparent and confirm the case is half see-through, without stale pixels after key presses, and takes the laser everywhere. Set Visible and confirm the case fills again.
- In General set Background to Disabled and Apply. No case is drawn, as with Invisible. Over a window floating behind the keyboard, a click between Esc and F1, between the main block and the arrows, beside ↑ and in the case's corners reaches the window; a click between two keys of a block, on a toolbar button, the bar and the X does not. A swipe that leaves a block still swipes, and a grip move holds the keyboard. Settings and the shortcut editor have a full case under the laser. With the Profiles menu or the candidate list open, a click in a case corner only closes it and reaches no window; closed, the same click reaches the window again. In dictation mode, a status too long for its line, such as a server error, scrolls under the laser, and a click over a short one reaches the window. Repeat with the numpad hidden, the emoji picker, dictation mode and the flick layout, over the dashboard, over an app and over a stream. A `config.json` saved before 0.15.0 with Background Disabled loads as Disabled.
- With Background Semi-transparent or Invisible, over a bright and a dark window: the message card above the keyboard, a Japanese reading line and its hint, the dictation counter and the emoji picker's hints stay readable, and the flick preview's caps stand apart from the keys beneath. Repeat with Stay open over apps.
- Toggle pin, close/reopen, and confirm the saved icon/state. Neither movement method can move a pinned keyboard; moving/tilting the dashboard still carries it. Unpin and confirm the selected movement method returns.
- Confirm the settings toolbar order is Back, Apply and save, Reload config. Production shows the Languages and Layouts, General and Typing cards in full and the start of Dictation, cut off at the right edge to show the row scrolls; Integration is reached by scrolling sideways.
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

- In General, confirm Hover fade, Hide numpad, Grab to move and Auto leveling labels sit at the left edge and their checkboxes share one column at the right edge. Auto leveling starts checked on a new or older config. Toggle it and use Back to discard; Apply and restart must retain it. With it off, place the keyboard at a small sideways angle and confirm it stays there after release in laser, AUX and grip modes. Re-enable it and confirm the existing 5°/500 ms correction resumes without a pose jump or pitch change. Reload must refresh the checkbox and behavior.

- With Grab to move off, hold the line with a controller trigger and leave grip released. That same hand's stick up must move the keyboard farther along the ray; down must bring it closer with the existing speed, dead zone and bounds. The other hand's stick must have no effect. Repeat with each controller, release, pin, hide the dashboard and lose tracking to verify depth stops with capture. Ordinary key hover/click must not move depth. An AUX head-cursor capture must ignore both controllers' sticks. Re-enable grip mode and verify its same-hand depth still works.

- Quickly click/release the movement line, then move the controller. A missed overlay mouse-up must not leave the keyboard following for the 30-second timeout. Initial stale rest samples must not cancel a genuine hold; release after an observed hold must stay immediate. Missing/invalid component samples must not count toward a released-sample window. Automated placement tests cover these timing sequences, and the isolated runtime harness verifies the production line-capture wiring.

## Resident native replacement

- Invoke the native keyboard button, then repeatedly click/type on our panel. Native keys, collision and move line must remain absent; neither panel may loop open/closed. X must dismiss ours and keep both panels dismissed. The native button must then reopen ours, and another press must dismiss it. Repeat with automatic Steam field focus and field Enter/close handling. External requests lacking an exact restorable owner must remain native.
- Enable replacement and Apply. Hold keys with both pointers, then X. All owned input releases, our panel hides, the process remains and native does not reappear for that dismissed session.
- Invoke a native keyboard request again. The same resident process shows our panel and completes a new readiness handshake. Repeat after closing/reopening the dashboard.
- Relaunch the app while its resident panel is hidden. The existing process reopens/recenters and Settings is reachable.
- Quit Full Keyboard is present only in Settings while replacement is enabled. Explicit Quit ends the process and restores native fallback; native summons remain usable afterward.
- Uncheck replacement and Apply. The Quit action disappears, normal X exits, and the original Steam button behavior is restored.
- On a Steam client with and without the module registry, replacement takes Steam field and dashboard requests, and the journal shows `status-store` or `status-notification` once per bridge install. On the newer interface, a Steam keyboard already open when Full Keyboard starts stays visible, and the next request in its field opens ours.

## Streaming support

- Off, a stream from a PC keeps Steam's keyboard, which opens at once.
- Turning it on asks first, and the line under the switch says it needs a SteamOS restart; after the restart that line is gone.
- In a stream from a Windows PC and from a Linux PC: the keyboard button opens Full Keyboard; letters, Enter, Backspace, Esc, arrows, Ctrl+C, Shift with arrows and a held key arrive; Cyrillic and Japanese arrive from the Windows PC. A key press with the dashboard closed does not click on the PC, and a click on the stream with the panel showing moves typing there.
- Closing the panel mid-text or while Shift is held still types what was sent and leaves no key down on the PC.
- A held Backspace or arrow stops on the PC as soon as you let go, and a letter typed right after it arrives at once; a held letter still repeats at the usual speed. While you type into the stream, a trigger pull with the laser off the panel does not click on the PC.
- Turning it off returns a running stream to Steam's keyboard with no restart, and turning it back on before a restart works without one.
- `uninstall.sh`, the installer's Uninstall and its Downgrade below 0.18.0 remove its drop-in.

## Shortcut bar

Run these in order on a release candidate, with a fresh `shortcuts.json` unless a step says otherwise, Replace native keyboard on, and a harmless text editor in SteamOS's Desktop as the target. See [the shortcut bar](shortcut-bar.md).

1. **Turning it on.** In Settings → General, turn on Shortcut bar and press ✓. An empty bar of two slots with the menu's tab shows above the case, centered on it, and a shortcut bar button joins the toolbar after Pull for Shift. Turn it off and press ✓: the bar and the toolbar button go.
2. **First profile.** Open the tab, then Manage. Add a profile with New profile, type its name on the mini keyboard, press +, type a label, choose Shortcut and record Ctrl+A. Press ✓: the editor closes and the bar shows the new profile with its button.
3. **A shortcut.** In the text editor, type a line, then press the Ctrl+A button: the line is selected. The button presses like a key, with hover, travel, vibration and its sound, and acts on release over it; letting go off it sends nothing.
4. **The toolbar button.** Press it: the bar hides and its icon is crossed out, also after changing apps. Press it again: the bar is back.
5. **Scrolling.** Add buttons until the bar is full and more. Scroll it with the stick while the laser is on it, and by pressing on it and dragging past a short distance: one button at a time, eased; a chevron and a fade show the clipped side. A drag that started on a button doesn't press it. Each profile keeps its scroll position.
6. **The menu.** Make a second profile. Open the tab: the menu drops over the case under the bar, the profile showing is filled, and the tab stays lit. Choose the other profile: it shows and the menu closes. A press outside the menu closes it and does nothing else.
7. **Assigning, Desktop app.** With the text editor in front, open the menu and press the assign button of the second profile: it lights, its tooltip names the profile and the app, and the menu stays open. Bring another Desktop app to the front: the bar follows the rule for apps without a profile. Bring the editor back: the second profile shows by itself. Press the lit button again: it goes dark, and the bar stays as it is until another app comes to the front.
8. **Assigning, local game.** Start a Steam game on the headset, assign a profile to it, quit and start it again: its profile shows. A non-Steam shortcut gets the same.
9. **Assigning, program.** With another program in front through Gamescope, such as a browser started on its own, assign a profile, switch away and back: it shows. The editor's Used for lists the app by a readable name.
10. **Assigning, streamed game.** With Streaming support on, stream a Steam game from a Windows PC and open the keyboard with SteamOS's keyboard button. Until the stream's app is known, assign buttons are dimmed with a reason; then assign a profile. End the stream and run the same game on the headset, if installed there: the same profile shows. Used for names the game, also when it isn't installed on the headset.
11. **Assigning, desktop stream.** Stream the PC's desktop and assign a profile: Used for lists PC desktop stream, and the profile shows for every desktop stream after that.
12. **Where nothing can be assigned.** With Steam's dashboard in front, in one of Steam's own text fields, and in Steam's own windows, every assign button is dimmed and its tooltip says why.
13. **A macro.** Make a Macro: a Keys step recording End, a Wait of 0.5 s, and a Keys step recording Shift+Home. Press it in the text editor: the cursor goes to the line's end, then after half a second the line is selected. The button stays lit while it runs.
14. **Repeat a count.** Set the macro to Times, 5, with a Keys step recording ↓ and a Wait of 0.3 s, and press it on a list of lines: the cursor moves five lines, a step every 0.3 s.
15. **While held.** Set it to While held. Hold the trigger on it: passes repeat while you hold, also with the laser drifting off the button; let go: the pass under way finishes and no other starts. Quick taps on it each run a pass.
16. **The progress stroke.** Press the Times macro of step 14: a stroke runs clockwise around the button's border from the top, steadily, and is full as the run ends, then goes. A While held macro's stroke goes round once per pass. A shortcut, and a macro planned for half a second or less, show none.
17. **What stops a run.** Start the Times macro again and, each time in a new run: press its button again; press a key; press another bar button; open the menu's tab; bring another window to the front, a dialog of the same app included; hide the keyboard; open Settings; open dictation mode; let the headset sleep (one sleep in the round). Each time the run stops at once, the button goes dark and nothing more arrives in the editor.
18. **Where keys can't go.** In one of Steam's own text fields the shortcut and macro buttons are dimmed and a press does what a key press does there; Run buttons stay usable.
19. **Run, script.** Put an executable script that writes a marker file into `~/.config/framekeyboard/scripts/`, make a Run button choosing it, and press it: the file appears. The button shows the `>_` mark. Remove the script: the editor marks it missing, and a press shows a message naming the button.
20. **Run, command.** Make a Run button with a command that writes a marker file, typed on the mini keyboard: a press writes it, with no typing target needed. Quit the keyboard while a long command runs: the command goes on.
21. **The editor at both widths.** Open the editor with the numpad shown and hidden: everything fits, the strip's hint is left out on the narrow one, and the case does not change width or height as the editor opens and closes.
22. **Editor drags.** Reorder profiles by dragging; reorder buttons in the strip; drop a button outside the strip to delete it; drop one on another profile's row and choose Move, then Copy: it lands at that profile's end. Reorder macro steps and delete one with ✕.
23. **The mini keyboard.** Type a label with Shift and Caps Lock, two lines with Shift+Enter; Esc cancels and Enter saves. Record a shortcut: modifiers stay lit after a tap, Num Lock and Caps Lock are dimmed, the strip scrolls with the newest chip, the icon at its end takes back the last combination, and a fifth combination is refused. Nothing typed or recorded reaches the text editor.
24. **Leaving the editor.** Change something and press ←: the question offers Discard and Save, and each does what it says. Delete a profile with buttons: it asks first.
25. **An invalid file.** Quit the keyboard, break `shortcuts.json` by hand (an unknown key name in one button), and start it: Settings says under Shortcut bar that the file can't be read and where, without quoting your text, and no profile loads. Fix the file and use Reload config: the profiles load. Break it again and Reload: the loaded profiles stay and a message says why. Save in the editor: the broken file is kept as `shortcuts.json.bad`.
26. **Sleep and wake.** With the bar showing and a profile assigned to the app in front, let the headset sleep and wake it: the same profile shows, nothing was sent while asleep. Before that sleep, open the editor with a label half typed and Shift latched, or a recording under way, press X and summon the keyboard: the editor and its popup are as they were, and nothing reached the app; they stay so across the sleep.
27. **Settings open.** Open Settings: the bar hides, and comes back when Settings closes.
28. **Dictation mode.** Open dictation mode: the bar and its toolbar button hide, the keys stay exactly where they were, and the bar returns when the mode ends.
29. **The stream lock.** In a stream from a Windows PC with Streaming support, swipe a non-Latin word (Russian), change it to another candidate, choose typing suggestions back to back, and press a few shortcut buttons between them: all text arrives in order, and afterwards a click on the PC still clicks, with no mouse button left held.
30. **Replacement off.** Turn Replace native keyboard off and start the keyboard from the launcher: the bar works with profiles picked in the menu, assigning works in SteamOS's Desktop, and nothing else changes.
31. **A run in a stream.** In a stream from a Windows PC with Streaming support, start a Times macro of ↓ on a list and open the dashboard: the run stops at once, its button goes dark, and at most one more ↓ arrives. With the dashboard closed, each ↓ arrives as its own step.
32. **The size cap.** With a `shortcuts.json` near 512 KB, add a long macro and press ✓: "Shortcuts not saved: they would take more than 512 KB…" shows, the editor keeps its draft and the file is unchanged; an assignment from the menu that would pass it is refused the same way. Edit a valid file by hand while the keyboard runs, then save in the editor: your version is kept as `shortcuts.json.old`.
33. **The installer's page.** On a first install of 0.20.0 or newer, "Enable the shortcut bar?" comes after Curved keyboard and before Streaming support, answered No. Yes turns Shortcut bar on; No leaves it off, and turns off a bar a kept configuration had on. Installing 0.19.x skips the page and leaves the setting alone. Installing 0.17.x over a kept configuration with Curved keyboard, Shortcut bar and Streaming support on leaves all three on in `config.json`, with no drop-in for Steam's start, and updating to 0.20.0 then sets Streaming support up again.
