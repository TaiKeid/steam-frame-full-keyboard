# Changelog

## Unreleased

- Added **Focus mode** to the toolbar, beside Stay open over apps, while Replace native keyboard is on. The keyboard then stays where it is when a text field closes or loses focus, instead of closing, and types into the next field you select. With no field selected, it types into the app that has keyboard focus, as with replacement off. × or the keyboard button still hides it. Its window icon turns amber when that app's window is out of sight, and a tooltip above it names the window; Settings → Integration → **Hidden window tooltip** turns the tooltip off. The setting is saved.
- Nothing is typed while Steam's dashboard has focus and no Steam field is selected. Keys used to reach the app behind the dashboard, so a shortcut such as Ctrl+T could open a tab in a browser you could not see. Select a text field, or the window you want to type into, to type again. Typing with nowhere to go now shakes the Focus mode icon instead of showing a "Text not sent" message.
- Added **Background** to Settings → General, to see through the keyboard's case. **Visible** keeps it as before. **Semi-transparent** dims what is behind it, and **Invisible** leaves it clear; with both, the whole case still takes the laser. **Disabled** leaves it clear and lets the laser pass through the gaps between keys to the window behind, while a swipe that crosses a gap still continues. Keys, the toolbar, the emoji picker, the dictation editor and Settings keep their own look, and the status line and hints sit on a small plate so they stay readable over any background.
- **Stay open over apps** is now remembered when Full Keyboard restarts, as Pin and Focus mode already were.
- With **Stay open over apps** and the dashboard closed, pointing at a window floating in the world now brings SteamVR's laser, so you can click that window to type into it. The laser shows only while you point at a window.
- Themes can set an `alert` colour for toolbar icons that need attention, such as Focus mode's warning. By default it is amber or orange, depending on the theme.
- Swipe suggestions and the automatic space before the next swiped word now stay when you point away from the keyboard. They are removed once you pull the trigger outside it or point with the other hand, since that click may have moved the cursor.

## 0.11.1: Speech model download no longer closes the keyboard

- Fixed Full Keyboard closing as soon as the on-device speech model started downloading from Settings → Dictation. The download itself was fine; the keyboard crashed while drawing the mic key's progress under Settings. It now finishes with Settings open and the mic key shows the right state when Settings closes.

## 0.11.0: Dictation, 25 more languages and ten themes

- Added **dictation**: speak instead of typing. The new mic key, below Copy and Paste, opens dictation mode and starts listening. An editor replaces the function-key row; each phrase appears in it word by word as soon as it is recognized, and you can correct it with the keys before sending. **Send** types the text into the focused app and presses Enter; line breaks between paragraphs go out as Shift+Enter, so chats don't send early. **Undo last phrase** removes the last phrase. Esc stops listening, then asks before discarding the text. While listening, the mic key shows the speech level, and a segment circles its border while phrases are being recognized. A small icon in its corner shows where speech is recognized: on the headset or on your computer. The controller gives two light ticks when listening starts and one soft pulse when it stops. Phrases use the text before the cursor as context, which improves punctuation, casing and names. Listening stops after 60 s of silence, at 900 characters, and as soon as the headset is taken off or wakes from sleep, saying why.
- Dictation works in every bundled language except Japanese JIS. English and Spanish are the most accurate on the headset. Japanese and Chinese readings compose in the editor as on the keyboard, with **Convert** beside it.
- Settings has a new **Dictation** card. Choose **On device**, the default, to recognize speech on the Steam Frame after a one-time download of the speech model (264 MB, after you confirm), or **Self-hosted** to use your own Whisper speech server on your network, which is faster and more accurate. [Speech server setup](docs/speech-server.md) covers Linux, macOS and Windows; only the Linux steps have been verified. The server's address is typed in a popup with its own small keyboard, where Backspace and the arrows repeat while held, and a clear key beside the field empties it. **Test** checks the server, and **Fall back to on device** uses the headset when the server cannot be reached.
- **Type as I speak**, a switch on the Dictation card, skips the editor: the mic key listens on the normal keyboard, and each phrase is typed straight into the focused app. With a speech server, words are typed while you are still speaking, once Whisper is sure of them.
- **Send** types at 20 ms per character by default, or 10, 30 or 40 ms, or uses **Paste**. Paste writes the text to the clipboard and pastes it with Ctrl+V; Send and Type as I speak use it for local apps, while Steam's own fields still receive the text directly. Full Keyboard writes the clipboard but never reads it.
- Dictation records the microphone directly, so the voice-chat noise suppression SteamOS turns on while a game runs does not cut words out. While a game runs, game sound may be picked up, and the status line says so while listening.
- 25 new typing languages on their standard national layouts, each with dictation: Arabic, Croatian, Czech, Danish, Dutch, Estonian, Finnish, French (Canada), Greek, Hebrew, Hindi, Hungarian, Indonesian, Latvian, Lithuanian, Malay, Norwegian, Polish, Romanian, Slovak, Slovenian, Swedish, Thai, Turkish and Vietnamese. Arabic and Hebrew legends read right to left, and the right Alt key shows AltGr where a language uses it.
- Ten new themes: Ember, Plum, Crimson, Navy and Mono in dark tones with their own accent colors; Neon, whose latched and pressed keys light up in neon tubes; Cinematic, dark blue with orange; and the light Porcelain, Blossom and Lilac.
- Added a Korean 12-key **천지인** keyboard, as on Korean phones. Choose the **Korean 12-key (천지인)** layout in Settings. Vowels are built from the strokes ㅣ ㆍ ㅡ (ㅣㆍ for ㅏ, ㆍㅡ for ㅗ, ㅏㅣ for ㅐ), and tapping a consonant key again changes ㄱ to ㅋ and ㄲ; → ends the change so the same consonant can be typed twice. Flicks are shortcuts: right on a consonant key for its second consonant, up for its double, and ㅣ and ㅡ flick to ㅏ ㅓ ㅗ ㅜ. Syllables and final consonants form as in two-set Korean, Backspace removes one stroke at a time, and it works without libhangul.
- Swipe typing knows everyday chat words: its word lists, built from news text, are now blended with how often words are spoken in film subtitles, so words such as "okay", "thanks", "dziękuję" or "спасибо" are recognized and ranked as in conversation. The swipe word lists are now licensed CC BY-SA 4.0.
- Swipe typing now works in Polish, Dutch and Swedish. Accented letters are swiped through their plain letter, as on phones: Polish ą through A and ź through Z, Dutch ë through E. Polish ł is swiped on L.
- Settings has a new look that matches the keyboard: its cards are raised panels instead of outlined boxes, buttons and dropdowns look like keycaps, checkboxes are switches, and dropdown lists open as a raised menu.
- In Settings, the cards now glide when scrolled with the thumbstick, and can also be dragged with the laser, like a touch screen. Scrollbars light up under the laser.
- Choosing a theme in Settings shows it at once; Back returns to the saved theme.
- The keyboard fades in while rising into place when it opens, and fades out when it closes.
- The flick input's preview and the emoji skin tone menu are now raised keycaps and panels with soft shadows, instead of shapes outlined in bright white.
- In Settings, **Manage** on the Favorites row opens a list of your favorite layout and language pairs over the Languages and Layouts card. A bin button removes one pair, and **Clear all** removes them all after asking. Like every Settings change, removals are saved by Apply, and Back keeps your favorites.
- A layout or language name too long for the dropdown list scrolls while the laser is on it.
- The emoji picker has **Favorites**, after Recent. Hold an emoji in Recent, then drag it into Favorites to keep it there. Drag a favorite to move it, or drop it outside Favorites to remove it. A toned emoji's hold still opens its skin tones; keep holding and move to drag it instead.
- Skin tones stay open after you release the hold: click a tone to type it, or click anywhere else to close them.
- The emoji picker waits for a larger movement before it scrolls, so a slightly unsteady hand still picks the emoji it pressed.
- Holding Shift for half a second locks it, like a phone keyboard: every key gets Shift until you press Shift again, which helps when selecting text with the arrows. Shift keys show an arrow before their name, filled while locked.
- Holding Backspace, an arrow or another repeating key now keeps it repeating when the laser drifts off the keyboard, until you release the trigger, so you can watch the text instead of aiming. This needs a Steam Frame controller; other controllers release the key as before.
- Holding Backspace now repeats on an unfinished Japanese, Chinese or Korean reading, including flick input, instead of deleting one character per press. It stops once the reading is empty, so it never goes on to erase text already in the app.
- Moving, resizing, pinning or recentering the keyboard no longer erases an unfinished Japanese, Chinese or Korean reading; it stays ready to convert or commit.
- A tap that trembles just past a key's edge no longer becomes a swipe: a swipe now starts only once the laser has clearly left the key.
- Moving or resizing the keyboard no longer shows a release instruction in the status line.
- In SteamOS's Desktop, Japanese, Chinese, Korean, emoji and other non-ASCII text no longer arrives as digits. The Desktop runs its own compositor, which misread the keys Steam's input method used, so this text is now typed through the Desktop's own input method instead. While it is typed, its Unicode codes show briefly at the cursor. General → **KDE Desktop Unicode** can paste it instead, which is fast but replaces what is in the clipboard. Paste for **Send** and **Type as I speak** also reaches apps in the Desktop now, through its own clipboard.
- Shift+Enter now starts a new line in Steam's friends chat and other multi-line Steam text boxes when Full Keyboard replaces Steam's keyboard; Enter still sends. It used to show "Control unavailable in Steam fields". Other shortcuts in Steam fields remain unavailable.
- Native keyboard replacement is no longer marked experimental.
- The Experimental settings card is now called **Integration**.
- Alternative Steam Frame shells can host Full Keyboard through an experimental integration protocol: a shell launches it with `--hosted FD` and controls it over that inherited connection, showing and placing it, styling it, setting contexts and settings, controlling dictation, forwarding its own pointer, receiving only committed text, and updating it silently. A keyboard you start yourself has no such connection and behaves as before. See [shell integration](docs/integration.md).
- The **Stay open over apps** button now shows its state only by its icon, like Pin, instead of also lighting up when on.
- Fixed **Stay open over apps** turning on by itself when the dashboard closed while the keyboard was open for a field in the dashboard. It turns on by itself only for a keyboard request from a window pinned in the world while the dashboard is closed.
- Fixed Shift or AltGr after an accent key dropping the accent: ´ then Shift+E now gives É instead of E. Every accent key, such as caron, ogonek or ring, now shows its own legend instead of the US one.
- Fixed a retry after a stalled app typing the start of a word twice. Plain-ASCII words go to local apps one character at a time. If the app stopped acknowledging partway through, pressing Enter to retry typed the whole word again, so *hello* could become *hehello*. The retry now types only the characters that had not been acknowledged.

## 0.10.0: Emoji picker and corner resizing

- Added an emoji picker. The new smiley key at the top of the left column replaces the keys with a scrollable grid of emoji, with a category row above it, a space bar, and Backspace, Enter and the arrows at the right; its arrow key returns. Release the trigger on an emoji to type it; drag or use the thumbstick to scroll. A quick laser drag keeps gliding after release and bounces back from the ends; pressing the grid stops it. Emoji give no haptic clicks, so sweeping the laser across them stays quiet. Recent emoji come first, in a fixed six-column area with a Clear button: an emoji chosen elsewhere moves to the front at once, while choosing from Recent leaves its order alone. Hold an emoji with skin tones to choose one; the choice applies to every emoji after it. With the numpad hidden, the picker stays narrow. Only emoji the headset's emoji font can draw are listed. The picker needs Unicode text input, so it is unavailable with the uinput backend and external JIS. General → **Close emoji picker after choosing** returns to the keys after each emoji.
- Resize the keyboard from its corners, like a Steam window, instead of with the zoom buttons. Aim either controller's laser just outside a bottom corner: a resizer that follows the keyboard's rounded corner fades in there, and fades out as the laser moves away. Hold the trigger on it and drag; the keyboard grows or shrinks around its center. Pinning the keyboard hides the resizers. With the numpad hidden, only Stay open over apps moves to the toolbar's second row.
- Copy and Paste are now equal-height keys below the emoji key, leaving room for a dictation key later.
- In the dashboard, the thumbstick ticks the controller only where it scrolls something: the emoji grid, or a Settings card or list long enough to scroll. It used to tick wherever the laser was on the keyboard, and when pushed toward an end of the emoji grid it had already reached. Outside the dashboard, the stick scrolls the emoji grid as fast as in the dashboard.

## 0.9.2: Swiped words type correctly in X11 apps

- Fixed swiped words and swipe alternatives sometimes arriving scrambled or partly as digits in Brave and other X11 apps, such as `word235` or `htree` for *three*, especially while the Frame was busy. Words of plain ASCII characters are now typed one acknowledged character at a time, the way single key presses already were, with a short gap between characters. This avoids Gamescope's temporary keymap, and keeps the input method from reordering keys. Other text, such as accented words, Japanese or Chinese, is typed as before.

## 0.9.1: Full Keyboard returns after in-app updates

- After an in-app update, Full Keyboard comes back by itself. The old version hands the open keyboard request back to Steam, and the new one takes over Steam's keyboard about a second later, instead of leaving it open until you close it. Updating into this version already takes over a Steam keyboard that opened during the handoff; the automatic return on every update starts with updates from this version onward.

## 0.9.0: Swipe typing, flick input and typing over pinned windows

- With the numpad hidden or on the flick layout, the toolbar uses two rows: Settings, Recenter, Pin and the language on top, the zoom buttons and Stay open over apps below. Swipe suggestions no longer cover the language button.
- Auto leveling now lines the keyboard's long edge up properly. It removes both one end being higher than the other and a slight turn away, and keeps the desk tilt. Near the dashboard the keyboard lines up with the dashboard, which Steam often leaves slightly rolled. Elsewhere it ends level and facing you. Once it has been shown over apps with the dashboard closed, it stays level with the room, even when the dashboard opens again, until you move or recenter it, so opening the dashboard no longer turns it. When the dashboard carries the keyboard to a new heading, the room leveling turns along. Before, it only turned the keyboard around its own face, which levelled the edge but left a hand tilt as a visible twist, and it ignored the dashboard's own roll.
- Added an uninstaller, `uninstall.sh`, in the package and in the installed release. It stops the keyboard and its helpers, then removes the app, the Start with SteamOS service, the launcher and the menu entry. Settings stay unless `--purge` is given.
- Games streamed from the PC keep using Steam's own keyboard. A separate keyboard cannot deliver their text without leaking it into the focused Frame window or losing the request to a laser click; [streamed games](docs/streamed-games.md) records the findings.
- Fixed the keyboard button of games running locally on the Frame, such as a lightweight game, opening Steam's keyboard instead of Full Keyboard with Replace native keyboard on, whether the game is docked, pinned or in theater mode. Their window is named after the game's app ID, which the replacement did not recognize as a Frame app.
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
