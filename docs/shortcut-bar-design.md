# Shortcut bar: design

Status: implemented on the way to 0.20, except [renaming in place](#renaming-in-place), which is not built; acceptance in the headset is still to run. The owner settled the behavior on 2026-11-17; open facts are listed under [Frame probes](#frame-probes). Milestones are in the [roadmap](plan.md#shortcut-bar), and the page for users is [the shortcut bar](shortcut-bar.md).

With **Shortcut bar** on in Settings, a bar of the user's own buttons sits above the keyboard, like a dock. A button sends a shortcut, runs a macro, or starts a script or command. Buttons are grouped into profiles; a profile is chosen by hand or tied to an app, and then shows in the bar whenever that app has the keys.

## Terms

| Term | Meaning |
| --- | --- |
| Bar | The strip above the keyboard that holds the buttons. |
| Profile | A named, ordered set of buttons. The bar shows one profile at a time. |
| Button | A label and one action. |
| Action | Shortcut (key combinations in order), Macro (steps), or Run (a script or a command). |
| Assigned app | An app tied to a profile. An app has at most one profile; a profile can serve several apps. |
| Free profile | A profile no app is tied to. |

Code and interface say "assign" and "use for", never "pin": Pin already locks the keyboard in place (`pinned`). Layouts, languages and themes are called profiles in the [configuration reference](configuration.md) too; a shortcut profile is none of those and lives in a file of its own.

## The bar on the keyboard

The bar is drawn in the texture's room above the case (`popup_margin`, 200 px), so the overlay's size, center and placement math stay as they are. Sizes below are case pixels on the full US layout (scale 1.054).

| Piece | Geometry |
| --- | --- |
| Button | The size of left Ctrl on the full layouts: 69.5 × 62 layout units at their scale (73.3 × 65.4), the same on every layout, the flick layout's included (R379); drawn as a key in Ctrl's tone |
| Gap between buttons | 6 layout units (6.3) |
| Plate | The case's color and opacity, 12 px around the buttons, 90 tall, corners 18; its foot 10 px above the case |
| Picker | A tab on the plate's right side, part of the plate: 44 × 56, centered on the plate's height, the plate's color and opacity, its two outer corners rounded. The whole tab is the button, with a three-line menu icon |
| Position | Plate and tab together are centered on the case |
| Width | Grows with its buttons from two slots up to the case's width with the numpad hidden (1326), the tab included: 15 buttons. A narrower case, such as the flick layout's, limits it to its own width |

- A profile with no buttons shows as two empty slots. One button sits centered in two slots.
- A profile with more buttons than fit scrolls sideways: with the stick while a laser is on it, a button at a time and eased, or by pressing on it and dragging past 12 px. A clipped side fades out under a chevron. Each profile keeps its scroll position while the keyboard runs.
- Buttons look and feel like keys: hover, press travel, vibration and haptic sounds. They act on release over the same button, as controls do, so a drag can scroll. A macro that repeats while held starts on press instead, and a press on it never starts a drag.
- A Run button carries a small prompt mark, `>_`, in its top right corner, so a button that starts a script or a command is told apart from one that presses keys. The mark is a drawn icon in the theme's dimmed label color, 16 × 12 with its corner 7 px in from the button's top and right edges. It moves with the button's press and fades with a disabled button. It shows wherever the button is drawn: on the bar, in the editor's strip, and while the button is carried. Shortcut and Macro buttons have no mark.
- A button whose action can't be sent to the current target is drawn disabled, as Cut is in a terminal (see [where actions can go](#where-actions-can-go)). So is a button that has no action yet.
- The bar hides in Settings and in dictation mode, and stays up with the emoji picker. Under a shell the feature is off.
- A tooltip keeps its place right above the case while the bar shows, and is drawn over the bar where the two overlap. It takes no pointer: a button under it still hovers and presses. The status card sits above the bar. The bar's own tooltips, for the picker, **Manage** and the assign buttons, point at their parts of the bar; one that would share the status card's row waits while the card shows.

### Drawing and laser areas

- App builds a `ShortcutBarView` in `PanelView`: plate bounds, the picker tab's bounds, the visible buttons with labels and states, the scroll offset, and the open menu. The renderer only draws it; no input leaves drawing code.
- `shortcut_bar_bounds(view)` in `panel.hpp` gives the rectangles the laser may hit: the plate, its tab and the open menu. `PanelOverlay::mask()` and the `pointer_areas` lambda in `vr.cpp` both add them, so the dashboard laser, the passthrough pointer and a shell's forwarded pointer agree.
- Bar buttons are kept out of the "any control changed" and "any hover changed" full repaints. `shortcut_bar_damage(before, now)` returns the changed rectangles, as `theme_hover_damage` does. The bar overlaps no key, so a hover repaints one button.
- Image export includes the top margin whenever the bar shows.

## Toolbar button

A new icon button, shown while **Shortcut bar** is on, after Pull for Shift and before the language. On a narrow case it is the fourth button of the second row; it hides in dictation mode, so the editor keeps its width. On the full keyboard with all nine buttons, five suggestions still fit at 143 px each (157 px before).

The button is the user's switch for the bar, saved as `shortcuts_switch`. Its icon is crossed while nothing shows.

| Press while | Result |
| --- | --- |
| A profile shows | The bar hides and stays hidden everywhere until the button is pressed again |
| Nothing shows, and a profile is due for the focused app | That profile shows |
| Nothing shows, and no profile is due | An empty bar with two slots shows, so the picker can be reached |

## Which profile shows

Each time another app takes the keys:

1. The app has a profile: that profile.
2. Otherwise a profile picked before for apps without one: the one picked last if no app is assigned to it; else the most recently picked one that no app is assigned to; else, when every profile picked is assigned to some app, the one picked last all the same. A profile never picked is not chosen for the user.
3. Otherwise, nothing having been picked, nothing. The toolbar button shows off, without changing the user's switch, so a later app with a profile brings the bar back.

Steam's dashboard and an app that can't be identified count as apps without a profile. An empty bar opened with the toolbar button stays until the focus moves on.

Inside SteamOS's Desktop another app taking the keys is a change of focus, as another window is elsewhere. Another window of the same app, such as a dialog, is not: the bar and a profile chosen in the picker stay, and only a run under way stops.

Choosing a profile in the picker shows it until the focus changes. Chosen while an app without a profile of its own has the keys, it also goes first in the picks rule 2 returns to; chosen while an app with its own profile has them, it stays temporary. A deleted profile leaves the picks. The switch turned off by hand overrides all of this.

## Picker menu

The picker's tab opens a menu that drops over the case from under the bar, its right edge on the tab's. The tab stays lit while the menu is open. It closes on a choice, on a click outside (which is consumed), and when the bar hides.

| Row | Content |
| --- | --- |
| Header | "Profiles" at the left; **Manage** at the right, a button that opens the editor on the profile showing |
| One per profile, in the profiles' order | The name, filled while it is the profile showing, and an assign button at its right |

- A press on a name switches to that profile and does nothing else.
- A press on an assign button ties the focused app to that profile, lights the button, and clears any other profile's tie to that app. A press on a lit button removes the tie. Both are saved at once, and the menu stays open.
- A profile just tied to the focused app shows at once. Removing a tie leaves the bar as it is until the focus changes.
- Assign buttons are disabled while the focused app can't be assigned: Steam's dashboard, an unknown focus, SteamOS's Desktop before its window manager has reported, a stream from a PC until its app is known. The tooltip of a disabled one says that it can't assign here, and why. They are also disabled, for an app without a tie, once 128 apps have one.
- Six profiles show; more scroll. A tooltip on an assign button says what it will do, naming the profile and the app; on a disabled one it says why.
- The menu itself does not name the focused app: the lit assign button shows which profile it has, and the tooltips name it.

## Apps

| Where the app runs | Identity kept | Name shown |
| --- | --- | --- |
| A Steam game or non-Steam shortcut in Gamescope | `steam`: the Steam app ID Gamescope reports for the focused window | From Steam's app manifest; else the program name; else "App" and the ID |
| Another window in Gamescope | `program`: the file name of the focused process's program | That file name |
| An app in SteamOS's Desktop | `desktop`: the app ID its window manager reports | `Name` from the app's desktop file; else the ID's last part |
| Moonlight | `program`: Moonlight itself; the app on the PC can't be seen | Moonlight |
| A game streamed from a PC with Streaming support | `steam`: the game's Steam app ID, from the arguments Steam's streaming client was started with. The same identity the game has when it runs on the headset, so one assignment covers both | As for a Steam game. A game not installed on the headset has no manifest there: its name comes from Steam's own app cache, and only if that has none it shows as "App" and the ID |
| A stream from a PC without a game ID, such as its desktop or a shortcut that isn't a Steam game | `stream`: all of them are one app, with the one ID `pc`. Which app the PC has in front can't be seen. Steam starts a desktop stream with the ID of its own Remote Play Client app, 202355 (measured with a Windows PC; the app cache names it so): that ID is this identity too, never a `steam` one, and a tie saved under it as `steam` moves over unless a `stream` tie exists | "PC desktop stream"; "your PC's desktop stream" in a sentence |
| Steam's dashboard | None; it can't be assigned | — |

Which identity the focus has, by the first row that fits:

| Focus | Identity |
| --- | --- |
| Steam's dashboard has the keys, the keys go to one of Steam's own text fields, the focused window is Steam's own, or nothing is focused | None |
| A stream from a PC holds the typing request (Streaming support) | `steam` with the streamed game's ID, or `stream`; none until this session's answer is in |
| SteamOS's Desktop | `desktop`, with the app ID its window manager reported last; none before its first report and while no window is active there |
| A game-streaming client, such as Moonlight | `program`, even when Steam started it as a non-Steam shortcut: its file name outlasts a shortcut's ID |
| A window with a Steam app ID | `steam` |
| Any other window | `program`; none when the program's name can't be read |

- `DesktopFocus` already reads all three values; today it keeps only a kind and drops the names. It gains one snapshot, read without blocking: the focused window with its app's kind and ID, published together, so the bar never sees a new window with the last one's app.
- Gamescope reports nothing for a stream from a PC: no focused app and no window (measured for a streamed desktop and a streamed game). Its app is read when a stream session begins, off the VR thread, from the process `streaming_client`: the one number after `--appid`, taken strictly (digits only, no leading zero, nonzero, within 32 bits, exactly one client running). That line also holds the account and the PC's address: nothing else of it is kept and none of it is logged. Any surprise means the `stream` identity, never a guess. An answer belongs to its session: a new session never shows the last one's game, and until its answer is in the bar keeps what it shows and nothing can be assigned.
- An ID that `shortcuts.json` could not hold is no identity: empty, longer than 255 bytes, not UTF-8, or with a control character. A program's name comes from its command line, which the program itself can set. Some programs, Chromium-based browsers among them, rewrite their arguments into one string with spaces: when the first argument, cut at one of its spaces, names the file `/proc/<pid>/exe` points to, the name is that file's, without the flags. Otherwise it is what follows the last folder separator, so a Windows game run by Wine keeps a file name with spaces.
- The uinput backend can't tell which app has the keys: there is no identity there, only free profiles.
- A name is looked up when the picker's menu opens, when an app is assigned, and when a stream session's app becomes known, off the VR thread: one small file read, cut off at a fixed size, of which only the name is kept. Until it arrives a tooltip uses what the ID itself gives: the program's file name, or the last part of a Desktop app ID. The name is stored with the tie.
- Names found for Steam games and Desktop apps are also kept in `app-names.json` in the keyboard's cache folder: at most 256, the oldest found dropped, the user's alone, written off the VR thread; a bad file reads as none. A lookup that finds nothing uses the name kept, and a tie saved under its ID's own name, such as "App" and a Steam ID, takes the real name whenever one is found or kept.
- A game streamed from a PC need not have a manifest on the headset, even while it streams (measured: none for a game streamed from a Windows PC). When no manifest names a Steam ID, the name is read from Steam's own app cache, `appcache/appinfo.vdf` under Steam's folder: the entry for the ID, and in it `appinfo` / `common` / `name`. The format is Steam's private one, so it is read only when its first four bytes are the version known (`0x07564429`: entries of a stated size, keys as numbers into a table of strings at the file's end); any other version, a file over the size limit, an offset or size that points outside the file, an unknown value type or a name that is not UTF-8 gives no name and no error. Read off the VR thread, never written, nothing of it logged or kept but the one name.
- Desktop files are looked for by the app ID as a file name, in the user's and the system's application folders, Flatpak's included.

## Actions

### Shortcut

One to four key combinations sent in order, once per press: Ctrl+K, then S. A combination is any modifiers plus one key, or modifiers alone (a tap of Super). Every key of the keyboard can be recorded, including F1–F12, Tab, Delete, Home, Page Up and the modifiers. F13–F24 are not offered: on the headset's own routes the keymap turns most of them into other keys (in SteamOS's Desktop F13 opens System Settings and F20 mutes the microphone).

- Each combination is one `OutputController::tap()`: modifiers down, key down, then up in reverse. `can_queue()` is checked for the whole sequence first, so it is sent whole or refused.
- The runner ends a shortcut before its next combination when another window has taken the keys since it started: between combinations the output queue is empty and can't notice.
- A press is admitted as a key press is, so a refusal shows the same status and an earlier failure with nothing to retry is cleared.
- Combinations are spaced by `shortcut_key_gap` (30 ms until the [probe](#frame-probes) says otherwise), and by the stream gap where that is longer.
- Only the button's own modifiers are sent. Latched modifiers are cleared, not added; held or locked ones are let up first, as Cut, Copy and Paste do, and a lock stays locked.
- An unfinished reading or a pending swiped word goes out first, as before any raw shortcut. The word being typed is forgotten, as after any key that may move the cursor.
- Letters are stored by the letter the key showed when it was recorded and resolved through the target's keymap when sent (`shortcut_code`), so Ctrl+A recorded on AZERTY is Ctrl+A on any target. Other keys go by position.

### Macro

Up to 32 steps, run in order. A macro presses keys; it does not type text.

| Step | Sends |
| --- | --- |
| Keys | One to 32 key combinations recorded in a row, each as in a shortcut. Typing a word in a macro is recording its keys |
| Wait | 0.1 to 10 s |

| Repeat | Behavior |
| --- | --- |
| Once | One pass per press |
| Times | 2 to 99 passes |
| While held | Starts on press and runs passes until the trigger lets go, as a held key repeats, also when the laser drifts off the button; the pass under way finishes |

`ShortcutRunner` feeds the output queue one combination at a time, each after the one before it was delivered, and is the only code that sends a button's keys. A running button stays lit. A run planned to take longer than 0.5 s also shows its progress: a stroke that runs clockwise around the button's border from the top center, once over the whole run for Once and for a count, once per pass for While held. It is drawn as a locked modifier's border is, in the theme's colors, and goes with the run however it ends. A run stops, sending nothing more and retrying nothing, when:

- its button is pressed again, or anything else on the keyboard or the bar is pressed: a key, another button, the picker, a row or an assign button of its menu;
- another window takes the keys, or the target goes away; on Steam's route, where there is no window, when the typing request changes or ends: another stream, a Steam field after a stream; over a stream, when the dashboard opens and SteamVR's laser leaves the stream for it, though the session lasts a moment longer;
- the keyboard hides, Settings or dictation opens, or the headset sleeps;
- a step fails to deliver.

A While held button is the exception to the first rule. Once its press lets go, its run is only finishing a pass, and a new press on it takes the hold again, so quick taps each act.

A stream session takes a combination at once but passes it on to the PC only a while later (R369), so there a combination counts as delivered once it is estimated through to the PC (`KeySink::keys_through`), and the next combination, or a wait, follows from then. A stop then leaves at most one combination still on its way. The journal says when a run starts, as what kind of run into what kind of target, and why it ends, in fixed words: never a label, a key, an app or a profile.

Keys are what streams take best: with Streaming support every key goes to the PC as a press and release with its modifiers, and through Moonlight as a key. The PC reads positions with its own layout.

### Run

Starts a program on the Frame. Nothing is typed, so it works without a typing target.

| Kind | What runs |
| --- | --- |
| Script | A file in `scripts/` in the config folder (`~/.config/framekeyboard/scripts/`), chosen from a list. Run directly when executable, else with `/bin/sh`. The name is checked again at each press: still a plain file name, still a regular file in that folder |
| Command | One line the user typed, run with `/bin/sh -c` |

- The script list shows the folder's regular files by name, up to 64: no folders, no links, no hidden files, and no name the file could not hold. The keyboard makes the folder when the list first opens, so the path the empty list names exists. The folder is read off the VR thread, when the editor shows the list and on **Refresh**.
- An executable file that the system can't run by itself, one without an interpreter line, is run with `/bin/sh` too, as a shell would.
- A press starts one program. More presses on that button are ignored until its start has been reported. A press on a Run button stops a run under way, as any press on the bar does. A Run button is never disabled for lack of a typing target.
- The child gets its own session, `/dev/null` for input and output, no other open files, the home folder as its working directory, and default signal handling and scheduling.
- Its environment is the keyboard's without what starting the keyboard added: the keyboard's own library path and preload, its own variables, and those of an AppImage it runs from. One function holds that list.
- It outlives the keyboard: nothing waits for it, no finished child stays behind as a zombie while the keyboard runs, and the session unit's `KillMode=process` leaves it running across a restart or an update.
- It is started off the VR thread. A script that is missing, is not a regular file or can't be executed shows a status naming the button. A command always starts its shell; what the command then does is not reported.
- Only the kind of failure is logged, as a number: never the label, the script's name or the command.

## Where actions can go

| Target | Shortcut and macro keys | Run |
| --- | --- | --- |
| A local app through Gamescope | Yes | Yes |
| An app in SteamOS's Desktop | Yes | Yes |
| Moonlight | Yes; Moonlight keeps its own Ctrl+Alt+Shift shortcuts | Yes |
| A game or app streamed from a PC with Streaming support | Yes, read by the PC with its own layout | Yes, on the Frame |
| One of Steam's own text fields | No: Steam takes only Backspace, Enter, Tab, the arrows and Esc there | Yes |
| Steam's dashboard has the keys, or there is no target | No, as for typing | Yes |
| The uinput backend and external JIS | Yes | Yes |

Buttons that can't be sent are drawn disabled; a press on one shakes or reports as a key press does there.

## Settings and the editor

Settings has one row for the feature, in the General card after Hide numpad. Under a shell it is left out.

| Row | Control |
| --- | --- |
| Shortcut bar, with the second line "Your own buttons above the keyboard: shortcuts, macros and scripts" | Switch, saved as `shortcuts`, off by default; pending until ✓ like the rows around it |

- Turning it on while there are no profiles shows the empty bar once, so its picker, and through it the editor, can be reached.
- When `shortcuts.json` can't be read, a note under the switch says so and where the fault is, by position (profile 2, button 5), never quoting the user's text.

The editor is a screen of its own, opened from the picker's **Manage** (prefix `shortcuts-`), with ← and ✓ at its top left. It fills the case as it is, full width or with the numpad hidden, as the emoji picker does, so the keyboard never changes width around it; on a narrower case, such as the flick layout's, it takes the width of the keyboard without its numpad. While it is open a composing language's reading and candidates are out of sight and take no press, as in Settings, and come back when it closes.

Its layout fits both widths. With the numpad hidden the profiles column is 236 wide instead of 286, the label field 250 instead of 300, the strip shows 12 buttons instead of 15, the steps list is narrower, and the strip's hint is left out.

| Area | Content |
| --- | --- |
| Left column | The profiles: press to select, drag to reorder. The selected row has rename and delete. **New profile** under the list. Below it, **Used for**: the selected profile's apps as chips, each with ✕ to remove the tie |
| Strip | The selected profile's buttons as they look on the bar, then **+**. Press selects; a sideways drag lifts a button to reorder it; dropped on another profile's row it is moved or copied there; dropped anywhere else outside the strip it is deleted. It scrolls as the bar does |
| Label and Action | The selected button's label field, and Shortcut, Macro or Run |
| Shortcut | The recorded combinations as chips in a strip of fixed width, then **Record** and clear, which stay in place whatever is recorded. A strip too short for its chips scrolls sideways, as the bar does, and fades at a clipped side |
| Macro | The steps as a list that scrolls: drag to reorder, ✕ to delete. A Keys step shows its combinations as chips and a press on it opens the recorder; a Wait step has − and + beside its time, in steps of 0.1 s. **+ Keys** opens the recorder for a new step and **+ Wait** adds half a second. Repeat: Once, Times with a count, While held |
| Run | **Script** or **Command**, a switch as Repeat's is. Script: the folder's files as a list that scrolls as the steps do; a press chooses a file and a press on the chosen one clears it; a chosen file that is gone stays in the list, marked missing; an empty list names the folder; **Refresh** beside the list reads the folder again. Command: a field that opens the popup to type one line. A line under it says what the button starts |

A button lifted from the strip can be carried to another profile. The row under it lights up, and letting go there opens a small popover beside that row with **Move** and **Copy**. Either puts the button at the end of that profile; a press anywhere else closes the popover and the button goes back to its place. The selected profile stays selected, so more buttons can follow. A profile that already holds 64 buttons takes no drop.

Shortcut, Macro and Run each keep what the user gave them while the editor is open, so switching between them loses nothing; only the chosen one is saved. Inside Run, Script and Command do the same. A shortcut turned into a macro for the first time starts as one Keys step holding its combinations.

Everything in the editor is pending. ✓ saves it and returns to the keyboard, showing the profile that was selected in the editor, as if it had been chosen in the picker: it goes first in the picks for apps without a profile. So a first profile shows as soon as it is saved. ← returns without saving, and asks first, Discard or Save, when something changed. Deleting a profile that has buttons asks too. Settings' own ✓ and ← do not touch the editor's content.

### Typing and recording

Labels are typed and keys are recorded without leaving the editor, on a mini keyboard in a popup. An earlier build switched to the real keyboard for this, and going back and forth lost the editor's context.

**The popup** sits under the strip, centered, 1051 × 380, the same at the full and the no-numpad width, so the strip stays in view and the selected button shows a label as it is typed. What lies under it is dimmed and takes no press.

| Row | Content |
| --- | --- |
| 1 | What is edited (Label, Profile name, Recording), then **Cancel** and **Done** |
| 2 | A recessed strip across the popup: the text being typed with its caret, or the combinations recorded so far as chips. When they don't fit it scrolls sideways, with the stick or a drag, and follows the newest chip while recording. While recording, a delete-left icon at the strip's right end takes back the last combination (Remove last); the chips stop short of it |
| Keys | The mini keyboard |

**The mini keyboard** is a fixed US keyboard, whatever layout and typing language are active, flick layouts included, and with the numpad whether or not Hide numpad is on.

- Its geometry is the full US layout's without the side column (emoji, Cut, Copy, Paste and the mic, which can't be recorded), with the gaps between its blocks closed to 12 layout units, 4 under the function row and none between the other rows. At a scale of 0.771 a letter key is 42 × 48.
- Its keys are the editor's own controls, drawn as keys, not the keyboard's keys: nothing goes through the key state, the output queue or a typing language, so nothing can reach the app, and there is no composition, no candidates, no swipe and no suggestions.
- Modifiers latch on a tap and show lit; a second tap lets one go.
- Num Lock and Caps Lock can't be recorded and are drawn disabled while recording. On the real keyboard they are the keyboard's own switches and are never sent to an app, so a recorded one would flip the app's lock behind the keyboard's back. While text is typed, Caps Lock works as a switch for capitals and Num Lock stays disabled.
- Numpad keys always record as numpad keys (Num 7), never as the navigation keys printed under their digits; those have keys of their own. Whether an app then sees a digit depends on its own Num Lock, which a [probe](#frame-probes) has to measure.

| Editing | A key press | Ends with |
| --- | --- | --- |
| Label, profile name, command | Types the key's US character, with a latched Shift or Caps Lock for capitals and symbols. Backspace deletes, the arrows move the caret. A command stops taking characters at 512 bytes | Enter or Done saves; Esc or Cancel cancels. In a label Shift+Enter starts a second line, once |
| Recording | Records the key with the modifiers latched then, as one combination. A press on the pending modifiers' chip, or Done, records them alone | Done; Cancel keeps what was there |

- A chip is as wide as its text plus 12 px on each side. The line that says what a button sends ends in an ellipsis when it is too long.
- Recording starts from nothing: Done replaces the button's combinations. A shortcut stops at four combinations, a macro's Keys step at 32.
- Recorded keys are key strokes, stored by the key's name. Nothing is recorded as a character.
- Labels in other languages are typed by renaming a button in place on the bar, which uses the real keyboard.
- Hiding the keyboard or sleep keeps the popup open as it was, with what was typed or recorded in it (R409). A mini key held down is let go and stops repeating, and a button waiting for Move or Copy goes back to its place.

## Renaming in place

Outside the editor, pointing at a bar button and pressing the stick of the pointing hand turns its label into a field, typed on the real keyboard in the current typing language. This is how a label gets text the mini keyboard has no keys for. Enter saves at once, Esc cancels, Shift+Enter starts a second line once. It is not offered while a macro runs.

- The toolbar row shows a hint with **Cancel** and **Done** meanwhile, and nothing reaches the app. Typing goes to the field through the `TextOutput` substitution dictation's editor uses, so composition, swipe typing and Pull for Shift work in it; typing suggestions and the mic are off.
- A composing language's candidates take the hint's place, with Cancel and Done staying at the right.

Nothing reads a stick press today. A new reader measures the thumbstick's render component as the grip and bumper readers do, for the dashboard pointer and the passthrough pointer alike.

## Storage

Two switches in `config.json`: `shortcuts` (the Settings switch, false by default) and `shortcuts_switch` (the toolbar button, true by default).

Everything else is in `shortcuts.json` beside it, readable by the user alone, written atomically by the preference worker, never on the VR thread.

```json
{
  "schema_version": 1,
  "profiles": [
    {
      "id": "p1",
      "name": "Editor",
      "buttons": [
        {
          "label": "Save\nall",
          "action": {
            "kind": "keys",
            "chords": [
              {"modifiers": ["ControlLeft"], "key": "KeyK"},
              {"key": "KeyS"}
            ]
          }
        },
        {
          "label": "Build",
          "action": {
            "kind": "macro",
            "steps": [
              {"keys": [{"modifiers": ["ControlLeft", "ShiftLeft"], "key": "KeyB"}]},
              {"wait_ms": 500},
              {"keys": [{"key": "KeyL"}, {"key": "KeyS"}, {"key": "Enter"}]}
            ],
            "repeat": {"mode": "times", "count": 3}
          }
        },
        {"label": "Term", "action": {"kind": "run", "script": "open-terminal.sh"}}
      ]
    }
  ],
  "apps": [
    {"kind": "desktop", "id": "org.kde.kate", "name": "Kate", "profile": "p1"}
  ],
  "free_profile": "p1",
  "earlier_picks": ["p2"]
}
```

| Limit | Value |
| --- | --- |
| Profiles | 32 |
| Buttons in a profile | 64 |
| Label | Two lines, 24 characters in all |
| Profile name | 32 characters |
| Combinations in a shortcut | 4 |
| Steps in a macro | 32 |
| Combinations in a macro's Keys step | 32 |
| Wait | 100 to 10,000 ms |
| Repeat count | 2 to 99 |
| Command | 512 bytes, one line |
| Script name | 64 bytes, no folder part |
| App ties | 128 |
| File | 512 KB |

- Key names are the logical names of `key_codes`; a chord without `key` is modifiers alone. An unknown name, kind or field value makes the file invalid as a whole.
- An app tie's `kind` is `steam`, `program`, `desktop` or `stream` ([Apps](#apps)). A `steam` ID is a nonzero 32-bit number, a `program` name has no folder part, and a `stream` tie's ID is `pc`.
- An action may be empty: a shortcut without combinations, a macro without steps, or Run with neither a script nor a command. The editor saves a button the user has not finished, and a press on it does nothing.
- Labels, names and commands are valid UTF-8 without control characters; a label's one line break is the exception.
- An invalid file or an unknown `schema_version` loads nothing, says so in Settings, and is never overwritten: the next save moves it to `shortcuts.json.bad`.
- `free_profile` is the profile picked last for apps without one, and `earlier_picks` those picked before it, newest first; a file without `earlier_picks` has a history of one. A pick that names no profile, or one that is there twice, makes the file invalid.
- What saves when: the editor's ✓ writes its pending copy; a tie made in the picker, a rename in place and a pick for apps without a profile are written at once.
- Every save looks at the file first: one that no longer parses is moved to `shortcuts.json.bad`, and a valid one that differs from what the keyboard last read or wrote, a hand edit made while it ran, is kept as `shortcuts.json.old`. The file is synced to disk before it replaces the old one.
- Reload config rereads and rechecks the file, so a hand edit shows without a restart; a reload that finds it invalid keeps the profiles already loaded and reports. `framekeyboard --check` reports the file's state beside the layouts, languages and themes.
- The installer and updates leave the file alone, as they do other user files.

`key_codes` gains `F13` to `F24`, and the stream key table their scancodes. The recorder does not offer them; a file or a button that holds them still loads, shows its chips and sends.

## Security and privacy

- A button can run a command and press any keys, so `shortcuts.json` is the only place such actions come from. Layout, language and theme profiles stay data and cannot run commands, and nothing in a shell's protocol or the instance socket can add, change or press a button.
- There is no import or sharing of profiles in this version.
- Labels, names, recorded keys, commands and script names are never logged.
- App identities: the program name and the Desktop's app ID were read, matched and dropped until now. They are now also kept in memory while that app has the keys and, for an app the user assigned, written to `shortcuts.json` with its name. The names found for Steam games and Desktop apps, with their IDs, are kept in `app-names.json` in the cache folder, also for apps the user did not assign. They are still never logged, and window titles are still not used. [Terminal edit keys](terminal-edit-keys-design.md#privacy) says the same.

## Code map

| Area | Files |
| --- | --- |
| Model, validation and the file | `include/framekeyboard/shortcuts.hpp`, `src/shortcuts.cpp` |
| Sending actions over time, stop rules | `src/shortcut_runner.cpp` |
| Starting programs | `src/shortcut_launch.cpp` |
| Bar view, picker, which profile shows, fields and recording | `src/app_shortcuts.cpp` |
| Editor popup | `src/app_shortcut_editor.cpp` |
| Drawing, `shortcut_bar_bounds`, `shortcut_bar_damage` | `src/panel.cpp`, `include/framekeyboard/panel.hpp` |
| Focused app's identity | `src/desktop_focus.cpp`, `src/desktop_terminal.cpp`; names in `src/app_names.cpp` |
| Laser areas, stick press, scrolling | `src/vr.cpp` |

The model knows nothing of drawing or output. The runner and the launcher are the only code that acts on a button, and drawing code calls neither.

## Tests

- `shortcuts`: the file's parsing, limits, invalid files and the `.bad` rule; which profile shows for each focus change; the toolbar button's three cases.
- `shortcut-runner`: order and spacing against a recording sink, whole-or-refused sequences, every stop rule, a focus change mid-run.
- `shortcut-launch`: in a temporary config folder, a script that writes a marker file, run directly and through `/bin/sh`; a command; the name rules and a missing script; the child's environment, open files, session and working directory; no child left to wait for.
- `settings`: the General row, the editor's flows (profiles, strip drags between profiles, steps), ✓ and ← with the question, the popup's typing and recording at both widths.
- `rendering`: bar hover, press and scroll repaints compared with a full repaint; the bar with the tooltip and the status card.
- `locales` and the interface tour: new strings in every language, labels fit, pictures of the bar, the menu and the editor.
- The uinput smoke test's receiver takes a recorded shortcut and a macro, never the focused app.

## Frame probes

Facts to measure on the Frame before the parts that depend on them:

| Probe | Decides |
| --- | --- |
| Stick press: how far the thumbstick component moves, in the dashboard and over apps, and whether SteamVR or the app also acts on it | Renaming in place |
| What Gamescope reports for a game streamed from a PC, a streamed desktop and Moonlight | The stream rows under [Apps](#apps). Answered for Steam's streams: nothing, so the identity comes from the streaming client. Moonlight is still to read |
| Combinations sent back to back into a private receiver, with gaps from 0 to 50 ms | `shortcut_key_gap` |
| A program started by the keyboard: its environment, and that it survives a keyboard restart. When the keyboard was started from Steam's launcher: whether Steam still shows the keyboard as running after it quits while that program lives | Run |
| A numpad digit key sent into a private receiver under Gamescope, in SteamOS's Desktop and into a stream from a PC: whether it arrives as the digit or as the navigation key | Whether numpad digits are offered for local apps |
| F13–F24 sent into a private receiver under Gamescope and in SteamOS's Desktop: the key seen and any system action. The default keymap turns F13 into Tools, F14–F18 into launcher keys, F20 into mic mute and F21–F23 into touchpad keys | Whether the recorder offers F13–F24 for local apps, or only for streams from a PC. Answered by hand in SteamOS's Desktop: F13 opens System Settings and F20 mutes the microphone, so the recorder offers none of them |

## Not in this version

- Icons and images of the user's choice on buttons; the Run mark is the only one.
- Holding a shortcut down, or keeping it down until pressed again.
- Macro steps that hold and release a key, macro steps that type text, and endless repeats.
- Starting installed apps or Steam games from a list.
- Volume and media keys.
- Sharing or importing profiles; the feature under a shell.
