# Shortcut bar: design

Status: design, not implemented. The owner settled the behavior on 2026-11-17; open facts are listed under [Frame probes](#frame-probes). Milestones are in the [roadmap](plan.md#shortcut-bar).

With **Shortcut bar** on in Settings, a bar of the user's own buttons sits above the keyboard, like a dock. A button sends a shortcut, runs a macro, or starts a script or command. Buttons are grouped into bars; a bar can be chosen by hand or tied to an app, and then shows whenever that app has the keys.

## Terms

| Term | Meaning |
| --- | --- |
| Bar | A named, ordered set of buttons. One bar shows at a time. |
| Button | A label and one action. |
| Action | Shortcut (key combinations in order), Macro (steps), or Run (a script or a command). |
| Assigned app | An app tied to a bar. An app has at most one bar; a bar can serve several apps. |
| Free bar | A bar no app is tied to. |

Code and interface say "assign" and "use for", never "pin": Pin already locks the keyboard in place (`pinned`).

## The bar on the keyboard

The bar is drawn in the texture's room above the case (`popup_margin`, 200 px), so the overlay's size, center and placement math stay as they are. Sizes below are case pixels on the full US layout (scale 1.054).

| Piece | Geometry |
| --- | --- |
| Button | The size of left Ctrl: 69.5 × 62 layout units at the layout's scale (73.3 × 65.4), drawn as a key in Ctrl's tone |
| Gap between buttons | 6 layout units (6.3) |
| Plate | The case's color and opacity, 12 px around the buttons, 90 tall, corners 18; its foot 10 px above the case |
| Picker | A plate of its own, 84 × 90, 8 px right of the bar's plate, holding one flat 60 × 66 button |
| Position | Plate, gap and picker together are centered on the case |
| Width | Grows with its buttons from two slots up to the case's width with the numpad hidden (1326), picker included: 15 buttons. A narrower case, such as the flick layout's, limits it to its own width |

- A bar with no buttons shows two empty slots. One button sits centered in two slots.
- A bar with more buttons than fit scrolls sideways: with the stick while a laser is on it, a button at a time and eased, or by pressing on it and dragging past 12 px. A clipped side fades out under a chevron. Each bar keeps its scroll position while the keyboard runs.
- Buttons look and feel like keys: hover, press travel, vibration and haptic sounds. They act on release over the same button, as controls do, so a drag can scroll. A macro that repeats while held starts on press instead, and a press on it never starts a drag.
- A button whose action can't be sent to the current target is drawn disabled, as Cut is in a terminal (see [where actions can go](#where-actions-can-go)). So is a button that has no action yet.
- The bar hides in Settings and in dictation mode, and stays up with the emoji picker. Under a shell the feature is off.
- While the bar shows, tooltips and the status card sit above it instead of above the case. When both can't sit side by side there, the status card shows and the tooltip waits.

### Drawing and laser areas

- App builds a `ShortcutBarView` in `PanelView`: plate bounds, picker bounds, the visible buttons with labels and states, the scroll offset, and the open menu. The renderer only draws it; no input leaves drawing code.
- `shortcut_bar_bounds(view)` in `panel.hpp` gives the rectangles the laser may hit: both plates and the open menu. `PanelOverlay::mask()` and the `pointer_areas` lambda in `vr.cpp` both add them, so the dashboard laser, the passthrough pointer and a shell's forwarded pointer agree.
- Bar buttons are kept out of the "any control changed" and "any hover changed" full repaints. `shortcut_bar_damage(before, now)` returns the changed rectangles, as `theme_hover_damage` does. The bar overlaps no key, so a hover repaints one button.
- Image export includes the top margin whenever the bar shows.

## Toolbar button

A new icon button, shown while **Shortcut bar** is on, after Pull for Shift and before the language. On a narrow case it is the fourth button of the second row; it hides in dictation mode, so the editor keeps its width. On the full keyboard with all nine buttons, five suggestions still fit at 143 px each (157 px before).

The button is the user's switch for the bar, saved as `shortcuts_switch`. Its icon is crossed while nothing shows.

| Press while | Result |
| --- | --- |
| A bar shows | The bar hides and stays hidden everywhere until the button is pressed again |
| Nothing shows, and a bar is due for the focused app | That bar shows |
| Nothing shows, and no bar is due | An empty bar with two slots shows, so the picker can be reached |

## Which bar shows

Each time another app takes the keys:

1. The app has a bar: that bar.
2. Otherwise the free bar chosen last, if it still exists and is still free.
3. Otherwise nothing. The toolbar button shows off, without changing the user's switch, so a later app with a bar brings the bar back.

Steam's dashboard and an app that can't be identified count as apps without a bar. An empty bar opened with the toolbar button stays until the focus moves on.

Choosing a bar in the picker shows it until the focus changes. If it is a free bar, it also becomes the one rule 2 returns to. The switch turned off by hand overrides all of this.

## Picker menu

The picker's button opens a menu that drops over the case from the picker, right-aligned with it. It closes on a choice, on a click outside (which is consumed), and when the bar hides.

| Row | Content |
| --- | --- |
| Header | "Bars" at the left; the focused app's name and a window icon at the right |
| One per bar, in the bars' order | The name, filled while it is the bar showing, and an assign button at its right |
| Last | **Edit shortcuts**, which opens the editor on the bar showing |

- A press on a name switches to that bar and does nothing else.
- A press on an assign button ties the focused app to that bar, lights the button, and clears any other bar's tie to that app. A press on a lit button removes the tie. Both are saved at once.
- Assign buttons are disabled while the focused app can't be assigned: Steam's dashboard, an unknown focus, SteamOS's Desktop before its window manager has reported.
- Six bars show; more scroll. A tooltip on an assign button says what it will do, naming the bar and the app.

## Apps

| Where the app runs | Identity kept | Name shown |
| --- | --- | --- |
| A Steam game or non-Steam shortcut in Gamescope | `steam`: the Steam app ID Gamescope reports for the focused window | From Steam's app manifest; else the program name; else "App" and the ID |
| Another window in Gamescope | `program`: the file name of the focused process's program | That file name |
| An app in SteamOS's Desktop | `desktop`: the app ID its window manager reports | `Name` from the app's desktop file; else the ID's last part |
| Moonlight | `program`: Moonlight itself; the app on the PC can't be seen | Moonlight |
| A game or app streamed from a PC with Streaming support | To be decided by a [probe](#frame-probes): the stream's own identity if Gamescope reports one, else all such streams as one app | — |
| Steam's dashboard | None; it can't be assigned | — |

`DesktopFocus` already reads all three values; today it keeps only a kind and drops the names. It gains one snapshot, the focused app's kind and ID, read without blocking. The name shown is looked up once when an app is assigned, off the VR thread, and stored with the tie.

## Actions

### Shortcut

One to four key combinations sent in order, once per press: Ctrl+K, then S. A combination is any modifiers plus one key, or modifiers alone (a tap of Super). Every key of the keyboard can be recorded, including F1–F12, Tab, Delete, Home, Page Up and the modifiers, plus F13–F24, which the keyboard has no keys for.

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

`ShortcutRunner` feeds the output queue one combination at a time, each after the one before it was delivered, and is the only code that sends a button's keys. A running button stays lit. A run stops, sending nothing more and retrying nothing, when:

- its button is pressed again, or any key or other button is pressed;
- another window takes the keys, or the target goes away;
- the keyboard hides, Settings or dictation opens, or the headset sleeps;
- a step fails to deliver.

Keys are what streams take best: with Streaming support every key goes to the PC as a press and release with its modifiers, and through Moonlight as a key. The PC reads positions with its own layout.

### Run

Starts a program on the Frame. Nothing is typed, so it works without a typing target.

| Kind | What runs |
| --- | --- |
| Script | A file in `scripts/` in the config folder (`~/.config/framekeyboard/scripts/`), chosen from a list. Run directly when executable, else with `/bin/sh`. The name is checked again at each press: still a plain file name, still a regular file in that folder |
| Command | One line the user typed, run with `/bin/sh -c` |

- The child gets its own session, `/dev/null` for input and output, no other open files, the home folder as its working directory and the session environment the keyboard runs in.
- It outlives the keyboard: nothing waits for it, and the session unit's `KillMode=process` leaves it running across a restart or an update.
- It is started off the VR thread. A program that can't be started shows a status naming the button.

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

- Turning it on while there are no bars shows the empty bar once, so its picker, and through it the editor, can be reached.
- When `shortcuts.json` can't be read, a note under the switch says so and where the fault is, by position (bar 2, button 5), never quoting the user's text.

The editor is a screen of its own, opened from the picker's **Edit shortcuts** (prefix `shortcuts-`), with ← and ✓ at its top left. It fills the case as it is, full width or with the numpad hidden, as the emoji picker does, so the keyboard never changes width around it; on a narrower case, such as the flick layout's, it takes the width of the keyboard without its numpad.

Its layout fits both widths. With the numpad hidden the bars column is 236 wide instead of 286, the label field 250 instead of 300, the strip shows 12 buttons instead of 15, the steps list is narrower, and the strip's hint is left out.

| Area | Content |
| --- | --- |
| Left column | The bars: press to select, drag to reorder. The selected row has rename and delete. **New bar** under the list. Below it, **Used for**: the selected bar's apps as chips, each with ✕ to remove the tie |
| Strip | The selected bar's buttons as they look on the bar, then **+**. Press selects; a sideways drag lifts a button to reorder it; dropped outside the strip it is deleted. It scrolls as the bar does |
| Label and Action | The selected button's label field, and Shortcut, Macro or Run |
| Shortcut | The recorded combinations as chips in a strip of fixed width, then **Record** and clear, which stay in place whatever is recorded. A strip too short for its chips scrolls sideways, as the bar does, and fades at a clipped side |
| Macro | The steps as a list that scrolls: drag to reorder, ✕ to delete. A Keys step shows its combinations as chips and a press on it opens the recorder; a Wait step has − and + beside its time, in steps of 0.1 s. **+ Keys** opens the recorder for a new step and **+ Wait** adds half a second. Repeat: Once, Times with a count, While held |
| Run | Script or Command; a list of the script files, or the command field |

Shortcut and Macro each keep what the user gave them while the editor is open, so switching between them loses nothing; only the chosen one is saved. A shortcut turned into a macro for the first time starts as one Keys step holding its combinations.

Everything in the editor is pending. ✓ saves it and returns to the keyboard. ← returns without saving, and asks first, Discard or Save, when something changed. Deleting a bar that has buttons asks too. Settings' own ✓ and ← do not touch the editor's content.

### Typing and recording

Labels are typed and keys are recorded without leaving the editor, on a mini keyboard in a popup. An earlier build switched to the real keyboard for this, and going back and forth lost the editor's context.

**The popup** sits under the strip, centered, 1051 × 396, the same at the full and the no-numpad width, so the strip stays in view and the selected button shows a label as it is typed. What lies under it is dimmed and takes no press.

| Row | Content |
| --- | --- |
| 1 | What is edited (Label, Bar name, Recording), then **F13–F24** and **Remove last** while recording, **Cancel** and **Done** |
| 2 | A recessed strip across the popup: the text being typed with its caret, or the combinations recorded so far as chips. When they don't fit it scrolls sideways, with the stick or a drag, and follows the newest chip while recording |
| Keys | The mini keyboard |

**The mini keyboard** is a fixed US keyboard, whatever layout and typing language are active, flick layouts included, and with the numpad whether or not Hide numpad is on.

- Its geometry is the full US layout's without the side column (emoji, Cut, Copy, Paste and the mic, which can't be recorded), with the gaps between its blocks closed to 12 layout units, 4 under the function row and none between the other rows. At a scale of 0.771 a letter key is 42 × 48.
- Its keys are the editor's own controls, drawn as keys, not the keyboard's keys: nothing goes through the key state, the output queue or a typing language, so nothing can reach the app, and there is no composition, no candidates, no swipe and no suggestions.
- Modifiers latch on a tap and show lit; a second tap lets one go.
- Num Lock and Caps Lock can't be recorded and are drawn disabled while recording. On the real keyboard they are the keyboard's own switches and are never sent to an app, so a recorded one would flip the app's lock behind the keyboard's back. While text is typed, Caps Lock works as a switch for capitals and Num Lock stays disabled.
- Numpad keys always record as numpad keys (Num 7), never as the navigation keys printed under their digits; those have keys of their own. Whether an app then sees a digit depends on its own Num Lock, which a [probe](#frame-probes) has to measure.

| Editing | A key press | Ends with |
| --- | --- | --- |
| Label, bar name, later a command | Types the key's US character, with a latched Shift or Caps Lock for capitals and symbols. Backspace deletes, the arrows move the caret | Enter or Done saves; Esc or Cancel cancels. In a label Shift+Enter starts a second line, once |
| Recording | Records the key with the modifiers latched then, as one combination. A press on the pending modifiers' chip, or Done, records them alone | Done; Cancel keeps what was there |

- A chip is as wide as its text plus 12 px on each side. The line that says what a button sends ends in an ellipsis when it is too long.
- Recording starts from nothing: Done replaces the button's combinations. A shortcut stops at four combinations, a macro's Keys step at 32.
- Recorded keys are key strokes, stored by the key's name. Nothing is recorded as a character.
- Labels in other languages are typed by renaming a button in place on the bar, which uses the real keyboard.
- Hiding the keyboard or sleep ends the popup as Cancel.

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
  "bars": [
    {
      "id": "b1",
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
    {"kind": "desktop", "id": "org.kde.kate", "name": "Kate", "bar": "b1"}
  ],
  "free_bar": "b1"
}
```

| Limit | Value |
| --- | --- |
| Bars | 32 |
| Buttons in a bar | 64 |
| Label | Two lines, 24 characters in all |
| Bar name | 32 characters |
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
- An action may be empty: a shortcut without combinations, a macro without steps, or Run with neither a script nor a command. The editor saves a button the user has not finished, and a press on it does nothing.
- Labels, names and commands are valid UTF-8 without control characters; a label's one line break is the exception.
- An invalid file or an unknown `schema_version` loads nothing, says so in Settings, and is never overwritten: the next save moves it to `shortcuts.json.bad`.
- What saves when: the editor's ✓ writes its pending copy; a tie made in the picker, a rename in place and the free bar are written at once.
- Every save looks at the file first: one that no longer parses is moved to `shortcuts.json.bad`, and a valid one that differs from what the keyboard last read or wrote, a hand edit made while it ran, is kept as `shortcuts.json.old`. The file is synced to disk before it replaces the old one.
- Reload config rereads and rechecks the file, so a hand edit shows without a restart; a reload that finds it invalid keeps the bars already loaded and reports. `framekeyboard --check` reports the file's state beside the profiles.
- The installer and updates leave the file alone, as they do other user files.

`key_codes` gains `F13` to `F24`, and the stream key table their scancodes.

## Security and privacy

- A button can run a command and press any keys, so `shortcuts.json` is the only place such actions come from. Layout, language and theme profiles stay data and cannot run commands, and nothing in a shell's protocol or the instance socket can add, change or press a button.
- There is no import or sharing of bars in this version.
- Labels, names, recorded keys, commands and script names are never logged.
- App identities: the program name and the Desktop's app ID were read, matched and dropped until now. They are now also kept in memory while that app has the keys and, for an app the user assigned, written to `shortcuts.json` with its name. They are still never logged, and window titles are still not used. [Terminal edit keys](terminal-edit-keys-design.md#privacy) is updated to say so when this lands.

## Code map

| Area | Files |
| --- | --- |
| Model, validation and the file | `include/framekeyboard/shortcuts.hpp`, `src/shortcuts.cpp` |
| Sending actions over time, stop rules | `src/shortcut_runner.cpp` |
| Starting programs | `src/shortcut_launch.cpp` |
| Bar view, picker, which bar shows, fields and recording | `src/app_shortcuts.cpp` |
| Editor popup | `src/app_shortcut_editor.cpp` |
| Drawing, `shortcut_bar_bounds`, `shortcut_bar_damage` | `src/panel.cpp`, `include/framekeyboard/panel.hpp` |
| Focused app's identity | `src/desktop_focus.cpp`, `src/desktop_terminal.cpp`; names in `src/app_names.cpp` |
| Laser areas, stick press, scrolling | `src/vr.cpp` |

The model knows nothing of drawing or output. The runner and the launcher are the only code that acts on a button, and drawing code calls neither.

## Tests

- `shortcuts`: the file's parsing, limits, invalid files and the `.bad` rule; which bar shows for each focus change; the toolbar button's three cases.
- `shortcut-runner`: order and spacing against a recording sink, whole-or-refused sequences, every stop rule, a focus change mid-run.
- `settings`: the General row, the editor's flows (bars, strip drags, steps), ✓ and ← with the question, the popup's typing and recording at both widths.
- `rendering`: bar hover, press and scroll repaints compared with a full repaint; the bar with the tooltip and the status card.
- `locales` and the interface tour: new strings in every language, labels fit, pictures of the bar, the menu and the editor.
- The uinput smoke test's receiver takes a recorded shortcut and a macro, never the focused app.

## Frame probes

Facts to measure on the Frame before the parts that depend on them:

| Probe | Decides |
| --- | --- |
| Stick press: how far the thumbstick component moves, in the dashboard and over apps, and whether SteamVR or the app also acts on it | Renaming in place |
| What Gamescope reports for a game streamed from a PC, a streamed desktop and Moonlight | The stream row under [Apps](#apps) |
| Combinations sent back to back into a private receiver, with gaps from 0 to 50 ms | `shortcut_key_gap` |
| A program started by the keyboard: its environment, and that it survives a keyboard restart | Run |
| A numpad digit key sent into a private receiver under Gamescope, in SteamOS's Desktop and into a stream from a PC: whether it arrives as the digit or as the navigation key | Whether numpad digits are offered for local apps |
| F13–F24 sent into a private receiver under Gamescope and in SteamOS's Desktop: the key seen and any system action. The default keymap turns F13 into Tools, F14–F18 into launcher keys, F20 into mic mute and F21–F23 into touchpad keys | Whether the recorder offers F13–F24 for local apps, or only for streams from a PC |

## Not in this version

- Icons and images on buttons.
- Holding a shortcut down, or keeping it down until pressed again.
- Macro steps that hold and release a key, macro steps that type text, and endless repeats.
- Starting installed apps or Steam games from a list.
- Volume and media keys.
- Sharing or importing bars; the feature under a shell.
