# Changelog

## Unreleased

- Resize the keyboard from its corners, like a Steam window, instead of with the zoom buttons. Aim either controller's laser just outside a bottom corner: a resizer that follows the keyboard's rounded corner fades in there, and fades out as the laser moves away. Hold the trigger on it and drag; the keyboard grows or shrinks around its center. Pinning the keyboard hides the resizers. With the numpad hidden, only Stay open over apps moves to the toolbar's second row.

- Added an emoji picker. The new smiley key at the top of the left column replaces the keys with a scrollable grid of emoji, with a category row above it, a space bar, and Backspace, Enter and the arrows at the right; its arrow key returns. Release the trigger on an emoji to type it; drag or use the thumbstick to scroll. Recent emoji come first, with a Clear button at the end. Hold an emoji with skin tones to choose one; the choice applies to every emoji after it. With the numpad hidden, the picker stays narrow. Only emoji the headset's emoji font can draw are listed. The picker needs Unicode text input, so it is unavailable with the uinput backend and external JIS. General → **Close emoji picker after choosing** returns to the keys after each emoji.
- Copy and Paste are now equal-height keys below the emoji key, leaving room for a dictation key later.

## 0.9.2: Swiped words type correctly in X11 apps

- Fixed swiped words and swipe alternatives sometimes arriving scrambled or partly as digits in Brave and other X11 apps, such as `word235` or `htree` for *three*, especially while the Frame was busy. Words of plain ASCII characters are now typed one acknowledged character at a time, the way single key presses already were, with a short gap between characters. This avoids Gamescope's temporary keymap, and keeps the input method from reordering keys. Other text, such as accented words, Japanese or Chinese, is typed as before.

## 0.9.1: Full Keyboard returns after in-app updates

- After an in-app update, Full Keyboard comes back by itself. The old version hands the open keyboard request back to Steam, and the new one takes over Steam's keyboard about a second later, instead of leaving it open until you close it. Updating into this version already takes over a Steam keyboard that opened during the handoff; the automatic return on every update starts with updates from this version onward.

## 0.9.0: Swipe typing, flick input and typing over pinned windows

- With the numpad hidden or on the flick layout, the toolbar uses two rows: Settings, Recenter, Pin and the language on top, the zoom buttons and Stay open over apps below. Swipe suggestions no longer cover the language button.
- Auto leveling now lines the keyboard's long edge up properly. It removes both one end being higher than the other and a slight turn away, and keeps the desk tilt. Near the dashboard the keyboard lines up with the dashboard, which Steam often leaves slightly rolled. Elsewhere it ends level and facing you. Once it has been shown over apps with the dashboard closed, it stays level with the room, even when the dashboard opens again, until you move or recenter it, so opening the dashboard no longer turns it. When the dashboard carries the keyboard to a new heading, the room leveling turns along. Before, it only turned the keyboard around its own face, which levelled the edge but left a hand tilt as a visible twist, and it ignored the dashboard's own roll.
- Added an uninstaller, `uninstall.sh`, in the package and in the installed release. It stops the keyboard and its helpers, then removes the app, the Start with SteamOS service, the launcher and the menu entry. Settings stay unless `--purge` is given.
- Games streamed from the PC keep using Steam's own keyboard. A separate keyboard cannot deliver their text without leaking it into the focused Frame window or losing the request to a laser click; [streamed games](docs/streamed-games.md) records the findings.
- Fixed the keyboard button of games running locally on the Frame, such as Deltarune, opening Steam's keyboard instead of Full Keyboard with Replace native keyboard on, whether the game is docked, pinned or in theater mode. Their window is named after the game's app ID, which the replacement did not recognize as a Frame app.
- Fixed windows pinned in the world: with Replace native keyboard on and the dashboard closed, their keyboard button opened Steam's keyboard, because Full Keyboard could only appear inside the dashboard. It now opens Full Keyboard on the first press, in a mode that stays open outside the dashboard and types into that window. Outside the dashboard the keyboard reads the controllers itself instead of taking SteamVR's laser, so other apps keep all of their input; pulling the trigger on a key also reaches them. A dot shows where each controller points. The new **Stay open over apps** button after Pin turns this mode on or off; it is not saved. Pulling the trigger outside the keyboard, with either hand, removes swipe suggestions, since that click may have moved the cursor. When a pinned window's button turned it on, it stays on after that window's request ends.
- Added the **Japanese flick 12-key** layout with the 日本語 — Flick language: the 12-key kana grid used on Japanese phones, with flick and toggle (repeated tap) input, a cross-shaped preview of the flick kana while a key is held, ゛゜小 for voiced and small kana, and the usual conversion. It keeps Esc, the F keys and the number row, and leaves out Ctrl, Alt, Super and Caps Lock.
- Settings now dims languages the selected layout cannot type, and says so while the Language list is open. Choosing a layout that cannot type the current language switches to the most likely language for it, such as Romaji when leaving the flick layout.
- The Backspace key now shows a ⇤-style icon instead of the word, distinct from the left arrow.
- The Japanese Romaji and Kana composition hint is now shown in Japanese.
- Switching to the external JIS profile while Full Keyboard is replacing Steam's keyboard no longer hands the request back to Steam's keyboard, which looked like a crash. Full Keyboard stays open, explains that external JIS cannot type there, and blocks typing until another language is chosen.
- The log now records frames slower than 0.5 s and why native replacement paused, with no typed content.
- Added experimental swipe typing, off by default, for English (US, UK), German, French, Spanish, Italian, Brazilian Portuguese, Ukrainian, Russian and Chinese Pinyin (the swiped Pinyin goes to the composer, which offers characters). Enable **Swipe typing** in Settings → Languages and Layouts; it is disabled for languages without a word list. Hold the trigger on a letter, trace the word and release. The best match is typed, with a space between consecutive swiped words, and up to five alternatives appear in the toolbar; choosing one replaces the swiped word. Taps still type single letters, on release while swipe is enabled. Swipe typing is powered by "FUTO Swipe" technology.

## 0.8.2: Fewer background wakeups

- Reduced background CPU wakeups while **Replace native keyboard** is enabled and no keyboard is open. The native recovery guard now checks for events every 250 ms instead of every 25 ms while idle, and the replacement helper no longer contacts it on every frame unless a local-app request needs recovery.
- The internal safety thread no longer wakes up periodically while native replacement is disabled.
- Native keyboard recovery, request deadlines and Steam-side polling are unchanged. Battery-life impact has not been measured.

## 0.8.1: Update handoff fix

- Fixed in-app update shutdown reopening Steam's native keyboard. The old typing session and queued input now end before the updated keyboard starts, while Quit and failure recovery still restore native.
- Kept background replacement updates hidden until a fresh keyboard request, even if Start with SteamOS was disabled during the session. Manual updates and updates with replacement disabled still show the panel normally.

When updating from 0.7.0 or 0.8.0, Steam's native keyboard may still open once because the old version handles its own shutdown. Dismiss it; the next keyboard request can open Full Keyboard.

## 0.8.0: Typing reliability and Steam dashboard fixes

- Added an ordered output queue so rapid two-controller typing, Backspace and arrow keys keep their press order while delivery is pending. Hiding, dismissal, target changes and native handoff discard old queued input and ignore late results.
- Made local-app text delivery asynchronous so a slow compositor does not block the keyboard. Input connections recover in the background without replaying old keys or text. Bundled the fixed libei 1.6.0 client to prevent crashes during interrupted input negotiation, without changing system libraries.
- Fixed Steam search typing reaching a local app behind the dashboard. Text stays bound to the selected Steam field, follows switches between supported Steam and local-app targets, and stops when the field loses focus or disappears.
- Improved native keyboard handoff and cleanup when owners change, the helper fails, or Steam's UI stalls. Abandoned installation attempts are cleaned up, native restoration is checked, and repeated failures use bounded retries.
- Fixed the first keyboard request after background startup and SteamVR session restart, and restored replacement readiness after headset sleep or tracking loss without requiring an app relaunch.
- Fixed Escape dismissal in Steam fields and dashboard sessions. Releasing a queued Backspace repeat now completes its delivery without another deletion, preventing a missing acknowledgement from forcing native fallback.
- Removed the routine pending-text notice that flashed during typing. Reduced bridge polling and inspection work while idle, retaining independent recovery checks and bounded diagnostics that never include typed text.
- Preserved saved profile selections when startup temporarily falls back to another supported profile. Missing or incompatible profiles now produce a useful fallback reason, and custom key IDs no longer collide with toolbar controls.
- Fixed Japanese Romaji input depending on cosmetic key labels instead of resolved key symbols, and stopped idle Space from incorrectly starting composition.
- Made Reload wait for pending settings work while the VR loop stays responsive. Unrelated preference changes can still save when startup configuration fails, and storage errors remain visible.
- Manual reinstalls and downgrades now use fresh release directories. Replacement removes only the old marked production release after successful activation, or startup confirmation for in-app updates, while preserving settings and unmarked development releases.
- Refactored VR lifecycle coordination and expanded requirements-based tests for routing, queued cancellation, dismissal, native fallback, recovery and controller ownership.

## 0.7.0: Movement, native replacement and updates

- Added a movement line below the keyboard with native-style colors and hover feedback. Hold it with the controller laser or AUX head cursor to move the keyboard; the same controller's thumbstick adjusts distance.
- Renamed Appearance to General and added **Use Drag to Move** to select grip movement instead of the default laser line. Pinning blocks both movement methods and hides the line.
- Added **Auto leveling** in General, enabled by default, to level small sideways tilts after releasing movement. Aligned the General checkboxes with their labels on the right.
- Added experimental **Replace native keyboard** for supported dashboard, Steam-field and local-app requests. It is off by default and requires Steam Developer Mode with an available local debugger connection. Independent recovery restores the native keyboard when Full Keyboard becomes unavailable or stops progressing.
- In replacement mode, × dismisses the panel while the app stays ready for the next request. **Quit Full Keyboard** appears in Settings, and **Use Steam keyboard** returns the current request to Steam.
- Added optional **Start with SteamOS**, available only with replacement enabled. It starts the keyboard and recovery helpers in the background without showing the panel, ready for native keyboard requests. Startup after a device reboot has been manually verified.
- Added **Check for updates** at the top of Settings. Checks run only when requested, report the latest-version result briefly, and keep a newer release available to download and install until the app exits.
- Updates verify release checksums, archive contents, ARM64 architecture, version and profiles before installation. They restart the keyboard, preserve settings and startup preferences, and roll back if the new app fails to complete its startup checks. Failures show a short reason and retain normal logging.
- Improved service recovery across SteamVR restarts and update handoffs. Saved startup preferences and owned service files stay consistent on Apply, Reload and launch, including after upgrades. Deliberate Quit pauses service restart, while ordinary manual launches retain successful exit codes.
- Improved movement release detection, cancellation of native key repeats, and recovery from stalled or signal-stopped keyboard processes, with bounded allowances for slow work and SteamVR waits.

## 0.6.0: Settings and keyboard preferences

- Rebuilt Settings as separate cards for languages/layouts and appearance, with horizontal card scrolling and independent vertical scrolling within each card.
- Added Layout, Language, and Theme dropdowns with five visible choices and scrolling. Menus can extend beyond the keyboard case and dismiss when clicked outside.
- Moved Back, Apply and save, and Reload config into the Settings toolbar.
- Added a Favorite checkbox for layout/language pairs. When favorites exist, a main-toolbar language button cycles the saved default and unique favorites while retaining the active theme and saved default.
- Added a persistent pin button after the zoom controls to prevent accidental grip movement while retaining dashboard following, resizing, and recentering.
- Added global Num Lock persistence across restarts and profile changes, plus a Hide numpad preference that narrows the case while keeping the remaining key sizes.
- Fixed native keypad output disagreeing with restored Num Lock in physical uinput and external JIS modes. Digits and decimal wait for confirmed target lock state; keypad Enter and operators remain usable without lock feedback.
- Fixed toolbar buttons overlapping Close when hiding the numpad in custom layouts.
- Made favorite cycling faster and moved preference writes off the input thread, with ordered saves, shutdown flushing, and save-error reporting.
- Fixed fractional dropdown scrolling and preserved precise wheel input in the desktop preview.
- Added a British English layout and language profile, contributed by [Cr0ss0vr](https://github.com/cr0ss0vr).

## 0.5.0: First release

Full Keyboard for Steam Frame is a separately launched alternative keyboard for the Frame dashboard and local apps. It does not replace the stock keyboard or send input to streamed VR applications.

- Composition commits before ordinary spaces, digits and punctuation, with failed-delivery protection.
- Chinese/Korean engines load on demand; missing libraries leave the rest of the keyboard usable.
- Added French, Spanish, Italian, Brazilian ABNT2, Russian and Ukrainian profiles, Simplified/Traditional Pinyin candidates, and Korean two-set Hangul composition. Language settings and system-layout limitations are documented.
- Sticky left Super and momentary native right Super, with monochrome Frame and SteamOS icons.
- Full-size keyboard with F1–F12, arrow and navigation keys, numpad, Ctrl/Alt/Shift, and dedicated Copy/Paste buttons.
- Layout-independent Unicode character delivery, local Shift/Caps/AltGr and accents, plus native navigation, shortcuts and key repeat.
- Controller laser typing, per-controller press/release haptics, and subtle hover feedback.
- Grip movement and rotation, same-hand thumbstick depth adjustment, size controls, and horizon alignment after release.
- Dashboard position and rotation following, saved placement, and relaunch-to-recenter recovery.
- Dashboard-only visibility and input, with cancellation on hiding, tracking loss, or disconnect.
- Editable JSON layouts, languages, and themes, with in-VR selection, favorites, and profile reload.
- English and German profiles, plus Japanese romaji/kana composition, kanji candidates, and an external JIS IME mode.
- Graphite and Midnight themes, a native desktop preview, and image export.
- Incremental key redraws and native pixel uploads where supported, with an RGBA compatibility fallback and event-driven idle rendering.
- User-local ARM64 installation, preserved settings, versioned rollback, release checksums, and manual installation instructions.
- MIT-licensed project code, build documentation, and automated regression tests.

See the [README](README.md) for installation and controls, and [runtime documentation](docs/runtime.md) for language requirements and known limitations.
