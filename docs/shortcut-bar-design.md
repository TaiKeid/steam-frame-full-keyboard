# Shortcut bar: design

Status: design, not implemented. The owner settled the behavior on 2026-11-17; open facts are listed under [Frame probes](#frame-probes). Milestones are in the [roadmap](plan.md#shortcut-bar).

With **Shortcuts and macros** on, a bar of the user's own buttons sits above the keyboard, like a dock. A button sends a shortcut, runs a macro, or starts a script or command. Buttons are grouped into bars; a bar can be chosen by hand or tied to an app, and then shows whenever that app has the keys.

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
- A button whose action can't be sent to the current target is drawn disabled, as Cut is in a terminal (see [where actions can go](#where-actions-can-go)).
- The bar hides in Settings and in dictation mode, and stays up with the emoji picker. Under a shell the feature is off.
- While the bar shows, tooltips and the status card sit above it instead of above the case. When both can't sit side by side there, the status card shows and the tooltip waits.

### Drawing and laser areas

- App builds a `ShortcutBarView` in `PanelView`: plate bounds, picker bounds, the visible buttons with labels and states, the scroll offset, and the open menu. The renderer only draws it; no input leaves drawing code.
- `shortcut_bar_bounds(view)` in `panel.hpp` gives the rectangles the laser may hit: both plates and the open menu. `PanelOverlay::mask()` and the `pointer_areas` lambda in `vr.cpp` both add them, so the dashboard laser, the passthrough pointer and a shell's forwarded pointer agree.
- Bar buttons are kept out of the "any control changed" and "any hover changed" full repaints. `shortcut_bar_damage(before, now)` returns the changed rectangles, as `theme_hover_damage` does. The bar overlaps no key, so a hover repaints one button.
- Image export includes the top margin whenever the bar shows.

## Toolbar button

A new icon button, shown while **Shortcuts and macros** is on, after Pull for Shift and before the language. On a narrow case it is the fourth button of the second row; it hides in dictation mode, so the editor keeps its width. On the full keyboard with all nine buttons, five suggestions still fit at 143 px each (157 px before).

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
| Last | **Edit shortcuts**, which opens Settings with the editor on the bar showing |

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
- Combinations are spaced by `shortcut_key_gap` (30 ms until the [probe](#frame-probes) says otherwise), and by the stream gap where that is longer.
- Only the button's own modifiers are sent. Latched modifiers are cleared, not added; held or locked ones are let up first, as Cut, Copy and Paste do, and a lock stays locked.
- An unfinished reading or a pending swiped word goes out first, as before any raw shortcut. The word being typed is forgotten, as after any key that may move the cursor.
- Letters are stored by the letter the key showed when it was recorded and resolved through the target's keymap when sent (`shortcut_code`), so Ctrl+A recorded on AZERTY is Ctrl+A on any target. Other keys go by position.

### Macro

Up to 32 steps, run in order.

| Step | Sends |
| --- | --- |
| Keys | One key combination, as in a shortcut |
| Text | Up to 256 characters, typed as the keyboard types text, in any language |
| Wait | 0.1 to 10 s |

| Repeat | Behavior |
| --- | --- |
| Once | One pass per press |
| Times | 2 to 99 passes |
| While held | Starts on press and runs passes until the trigger lets go; the pass under way finishes |

`ShortcutRunner` feeds the output queue one step at a time, each after the one before it was delivered, and is the only code that sends a button's keys or text. A running button stays lit. A run stops, sending nothing more and retrying nothing, when:

- its button is pressed again, or any key or other button is pressed;
- another window takes the keys, or the target goes away;
- the keyboard hides, Settings or dictation opens, or the headset sleeps;
- a step fails to deliver.

### Run

Starts a program on the Frame. Nothing is typed, so it works without a typing target.

| Kind | What runs |
| --- | --- |
| Script | A file in `scripts/` in the config folder (`~/.config/framekeyboard/scripts/`), chosen from a list. Run directly when executable, else with `/bin/sh` |
| Command | One line the user typed, run with `/bin/sh -c` |

- The child gets its own session, `/dev/null` for input and output, no other open files, the home folder as its working directory and the session environment the keyboard runs in.
- It outlives the keyboard: nothing waits for it, and the session unit's `KillMode=process` leaves it running across a restart or an update.
- It is started off the VR thread. A program that can't be started shows a status naming the button.

## Where actions can go

| Target | Shortcut and macro keys | Macro text | Run |
| --- | --- | --- | --- |
| A local app through Gamescope | Yes | Yes | Yes |
| An app in SteamOS's Desktop | Yes | As that route types text | Yes |
| Moonlight | Yes | As that route types text | Yes |
| A game or app streamed from a PC with Streaming support | Yes | Yes | Yes, on the Frame |
| One of Steam's own text fields | No: Steam takes only Backspace, Enter, Tab, the arrows and Esc there | — | Yes |
| Steam's dashboard has the keys, or there is no target | No, as for typing | No | Yes |
| The uinput backend and external JIS | Yes | No | Yes |

Buttons that can't be sent are drawn disabled; a press on one shakes or reports as a key press does there.

## Settings and the editor

A sixth card, **Shortcuts and macros**, follows Dictation. It is inserted by position, since Integration must stay last. Under a shell it is left out.

| Row | Control |
| --- | --- |
| Shortcuts and macros, with a second line saying what it adds | Switch, saved as `shortcuts`, off by default |
| Edit shortcuts | Button across the card; opens the editor; disabled while the switch is off |

The editor covers Settings as Additional languages and the theme picker do (prefix `shortcuts-`).

| Area | Content |
| --- | --- |
| Left column | The bars: press to select, drag to reorder. The selected row has rename and delete. **New bar** under the list. Below it, **Used for**: the selected bar's apps as chips, each with ✕ to remove the tie |
| Strip | The selected bar's buttons as they look on the bar, then **+**. Press selects; a sideways drag lifts a button to reorder it; dropped outside the strip it is deleted. It scrolls as the bar does |
| Label and Action | The selected button's label field, and Shortcut, Macro or Run |
| Shortcut | The recorded combinations as chips, **Record**, and clear |
| Macro | The steps as a list that scrolls: drag to reorder, ✕ to delete, press to edit. **+ Keys**, **+ Text**, **+ Wait**. Repeat: Once, Times with a count, While held |
| Run | Script or Command; a list of the script files, or the command field |

Everything in the editor is pending, like other settings: ✓ saves it, and Settings' ← discards it. Leaving Settings with editor changes asks first, Discard or Save, as it does for language changes. Deleting a bar that has buttons asks too.

### Typing and recording

The editor has no keys of its own. Fields are typed on the real keyboard, in the current typing language: the keyboard comes back with its toolbar row replaced by a strip, and nothing reaches the app.

| Field | On the keyboard | Ends with |
| --- | --- | --- |
| Label | The bar shows above with the button's label as a field | Enter saves; Shift+Enter starts a second line, once; Esc cancels |
| Bar name, macro text, command | A one-line field in the strip | Enter or Done saves; Esc or Cancel cancels |
| Keys | The strip shows the combinations recorded so far, with **F13–F24**, **Remove last**, **Cancel** and **Done** | Done |

- Typing goes to the field through the same `TextOutput` substitution dictation's editor uses, so composition, swipe typing and Pull for Shift work in it. Typing suggestions and the mic are off.
- Recording: a modifier tap latches as usual and shows as a pending chip; the next key completes the combination. A press on the pending chip, or Done, records the modifiers alone. Recording a shortcut stops at four combinations; a macro's Keys step takes one.
- Hiding the keyboard or sleep ends a field as Cancel.

## Renaming in place

Outside Settings, pointing at a bar button and pressing the stick of the pointing hand turns its label into the same Label field. Enter saves at once, Esc cancels, Shift+Enter starts a second line once. It is not offered while a macro runs.

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
              {"keys": {"modifiers": ["ControlLeft", "ShiftLeft"], "key": "KeyB"}},
              {"wait_ms": 500},
              {"text": "npm run dev"},
              {"keys": {"key": "Enter"}}
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
| Text step | 256 characters |
| Wait | 100 to 10,000 ms |
| Repeat count | 2 to 99 |
| Command | 512 bytes, one line |
| Script name | 64 bytes, no folder part |
| App ties | 128 |
| File | 512 KB |

- Key names are the logical names of `key_codes`; a chord without `key` is modifiers alone. An unknown name, kind or field value makes the file invalid as a whole.
- An invalid file or an unknown `schema_version` loads nothing, says so in Settings, and is never overwritten: the first save moves it to `shortcuts.json.bad`.
- What saves when: ✓ writes the editor's pending copy; a tie made in the picker, a rename in place and the free bar are written at once.
- The installer and updates leave the file alone, as they do other user files.

`key_codes` gains `F13` to `F24`, and the stream key table their scancodes.

## Security and privacy

- A button can run a command and type any text, so `shortcuts.json` is the only place such actions come from. Layout, language and theme profiles stay data and cannot run commands, and nothing in a shell's protocol or the instance socket can add, change or press a button.
- There is no import or sharing of bars in this version.
- Labels, macro text, commands and script names are never logged.
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
- `shortcut-runner`: order and spacing against a recording sink, whole-or-refused sequences, every stop rule, a focus change mid-run, text on a route without text.
- `settings`: the card, the editor's flows (bars, strip drags, steps), pending and the save question, fields and recording.
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

## Not in this version

- Icons and images on buttons.
- Holding a shortcut down, or keeping it down until pressed again.
- Macro steps that hold and release a key, and endless repeats.
- Starting installed apps or Steam games from a list.
- Volume and media keys.
- Sharing or importing bars; the feature under a shell.
