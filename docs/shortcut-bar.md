# Shortcut bar

The shortcut bar is a row of your own buttons above the keyboard, like a dock. A button sends a shortcut, runs a macro of keys and waits, or starts a script or command on the headset. Buttons are grouped into profiles; the bar shows one profile at a time, chosen by hand or because the app it is assigned to is in front.

## Turning it on

Turn on **Shortcut bar** in Settings → **General**, under Hide numpad, and press ✓. While you have no profiles, the bar first shows two empty slots, so its menu, and through it the editor, can be reached.

A shortcut bar button joins the toolbar, after Pull for Shift. It hides the bar, and the bar stays hidden everywhere until you press the button again; its icon is crossed out while nothing shows. Pressed while nothing shows, it brings back the profile for the app in front, or an empty bar if there is none, so the menu can be reached.

The bar hides in Settings and in dictation mode, and stays up with the emoji picker. A shell that hosts the keyboard has no shortcut bar.

## Profiles and the menu

The tab at the bar's right end opens the **Profiles** menu:

- Press a profile's name to show it on the bar. It shows until another app comes to the front.
- The button at the right of each row assigns that profile to the app in front, and lights up: from then on the bar shows that profile whenever this app is in front. Press a lit button to remove the assignment. An app has at most one profile; a profile can be assigned to several apps, and up to 128 apps can have one.
- **Manage** opens the editor on the profile showing.

The tooltip of an assign button names the profile and the app. When the button is dimmed, its tooltip says why: Steam's dashboard or one of Steam's own windows or text fields is in front, or the keyboard can't tell which app is.

A bar with more buttons than fit scrolls sideways, with the stick while the laser is on it, or by pressing it and dragging. Each profile keeps its scroll position while the keyboard runs.

### Which profile shows

Each time another app comes to the front:

1. The app has a profile: that profile shows.
2. Otherwise, the profile you picked last in the menu for an app without one, as long as no app is assigned to it. If every profile you picked is assigned to an app, the one picked last shows all the same.
3. Otherwise, if you never picked one, no bar shows. The toolbar button then shows the bar as crossed out, but a later app with a profile brings the bar back by itself.

Choosing a profile while an app with its own profile is in front shows it only until the app changes. Saving in the editor shows the profile you had selected there, as if you had picked it.

### What counts as one app

| In front | One app is |
| --- | --- |
| A Steam game, or a non-Steam shortcut, on the headset | The game, by its Steam ID. The same game streamed from your PC with Streaming support is the same app, so one assignment covers both |
| Any other program on the headset | The program's file |
| An app in SteamOS's Desktop | The app, as its window manager names it |
| A game streamed from your PC with Streaming support | The game, by its Steam ID, also when it isn't installed on the headset |
| Your PC's desktop, or anything streamed from it that isn't a Steam game | **PC desktop stream**: all of these are one app, since which program the PC has in front can't be seen |
| Moonlight | Moonlight itself: the app on the PC can't be seen |

Steam's dashboard and Steam's own windows and text fields are never apps that can have a profile. With the uinput backend the keyboard can't tell apps apart: only profiles you pick by hand show.

## Buttons

Buttons look and act like keys: they light up under the laser and press with the trigger, and act when you let go over the same button.

| Kind | What a press does |
| --- | --- |
| **Shortcut** | Sends one to four key combinations in order, such as Ctrl+K, then S |
| **Macro** | Runs up to 32 steps in order: **Keys**, one or more key combinations recorded in a row, and **Wait**, 0.1 to 10 seconds. It presses keys; it never types text |
| **Run** | Starts a script from your scripts folder, or a command line, on the headset |

A Run button has a small `>_` mark in its top right corner, so it is told apart from buttons that press keys.

Only the keys you recorded are sent: a Shift you tapped on the keyboard first is not added to them, and a modifier you hold or locked is let up for the shortcut. A reading or word still being composed goes to the app first.

A button whose keys can't be sent where the keyboard types is dimmed, as Cut is in a terminal: in Steam's own text fields, which take only a few keys, and while Steam's dashboard has the keys. Run buttons are never dimmed for that. A button with nothing to send or start yet is dimmed too.

### Repeat

A macro's **Repeat** is **Once**, a number of **Times** (2 to 99), or **While held**. A While held macro starts as you press it and runs pass after pass until you let go; the pass under way finishes. A quick press on it while that last pass finishes takes the hold again.

A running button stays lit. A run planned to take longer than half a second also shows its progress: a stroke that runs clockwise around the button's border from the top, once over the whole run, or once per pass for While held.

### What stops a run

A run stops, sending nothing more, when:

- you press its button again, or anything else on the keyboard or the bar: a key, another button, the menu's tab, a row or an assign button;
- another window comes to the front, a dialog of the same app included, or where the keyboard types goes away; in a stream from your PC, when that stream ends or another begins;
- the keyboard hides, Settings or dictation mode opens, or the headset sleeps;
- a step can't be delivered.

A While held button you are still holding is the exception to the first rule: its own press takes the hold again rather than stopping it.

## The editor

**Manage** in the menu opens the editor, a screen of its own with ← and ✓ at its top left. It fills the keyboard's case, with the numpad or without it.

- **Profiles**, at the left: press one to select it, drag to reorder. The selected profile's row has rename and delete; deleting a profile that has buttons asks first. **New profile** adds one. **Used for** lists the apps the profile is assigned to, each with ✕ to remove it.
- The strip at the top shows the selected profile's buttons as they look on the bar, then **+**. Press a button to edit it; drag it sideways to reorder, drop it outside the strip to delete it, or drop it on another profile's row and choose **Move** or **Copy**.
- **Label** is the button's text, two lines and 24 characters at most. **Action** is Shortcut, Macro or Run.
- Shortcut: **Record** records the keys; the line under it says what a press sends.
- Macro: the steps, which you drag to reorder and remove with ✕. A press on a Keys step records it again; a Wait step has − and + for its time. **+ Keys** records a new step, **+ Wait** adds half a second.
- Run: **Script** lists the files in your scripts folder; **Refresh** reads the folder again. A chosen script that is no longer there stays in the list, marked missing. **Command** opens a field for one line.

Shortcut, Macro and Run each keep what you gave them while the editor is open, so switching between them loses nothing; only the chosen kind is saved. ✓ saves everything and returns to the keyboard; ← leaves without saving, and asks first when something changed.

### Typing and recording

Labels, profile names and commands are typed, and keys recorded, on a small US keyboard in a popup inside the editor, so you never leave it. Nothing pressed there reaches an app: it has no composition, candidates, swipe typing or suggestions, and its keys are the US layout's whatever language you type in.

- Typing: Shift and Caps Lock give capitals and symbols. Enter or **Done** saves, Esc or **Cancel** cancels. In a label, Shift+Enter starts its second line.
- Recording: tap modifiers, which stay lit, then a key: that is one combination, and the next can follow. A press on the chip of the lit modifiers, or **Done**, records them by themselves, as a tap of Super. The icon at the strip's right end takes back the last combination. **Done** replaces the button's keys with what you recorded; **Cancel** keeps what was there. A shortcut takes four combinations, a macro's Keys step 32.
- Hiding the keyboard, or the headset sleeping, keeps the editor and an open popup as they are, with what you typed or recorded, until the keyboard shows again.
- Num Lock and Caps Lock can't be recorded: on the keyboard they are its own switches and never reach an app. Numpad keys record as numpad keys.

## Scripts and commands

Run starts a program on the headset, never on a PC you stream from.

- **Script**: a file in `~/.config/framekeyboard/scripts/`. The keyboard makes the folder when its list first opens. The list shows the folder's regular files by name, up to 64: no folders, links or hidden files. An executable file is run directly; one the system can't run by itself, such as a script without an interpreter line, and a file that isn't executable run with `/bin/sh`. The name is checked again at each press.
- **Command**: one line, at most 512 bytes, run with `/bin/sh -c`.

The program starts in your home folder, with nothing for input or output, in a session of its own, and keeps running after the keyboard quits, restarts or updates. Its environment is the keyboard's, without what starting the keyboard added. A press starts one program; more presses on the button are ignored until that start is reported. If a script is missing or can't be run, a message names the button. A command always starts its shell; what the command then does is not reported.

## Streams from your PC

With [Streaming support](streamed-games.md#through-steams-own-streaming-streaming-support), shortcut and macro keys go to the PC as key presses, with their modifiers, and through Moonlight as keys; the PC reads them with its own keyboard layout. A game streamed from your PC can have its own profile, as above. Run buttons start their programs on the headset. See [streamed games](streamed-games.md#shortcuts-and-macros-in-a-stream).

## Privacy

- Labels, profile names, recorded keys, commands and script names are never logged.
- For an app you assign, its ID (a Steam ID, a program's file name or a Desktop app's ID) and its name are saved in `shortcuts.json`.
- Names found for Steam games and Desktop apps are kept, with their IDs, in `~/.cache/framekeyboard/app-names.json`, at most 256, also for apps you did not assign. They are never logged.
- Window titles are not read. Nothing in a shell's protocol or the instance socket can add, change or press a button.

## Limits

- Labels and names are typed with the editor's US letters. A label in another script can only be written into `shortcuts.json` by hand.
- The recorder offers no F13 to F24: on the headset most of them are system keys, such as System Settings and microphone mute. A hand-written file can still use them.
- There is no list of installed apps or Steam games to start; Run starts a script or a command.
- No icons on buttons, no keys held down by a button, no macro steps that type text, and no endless repeat.
- Profiles can't be imported or shared, other than by copying `shortcuts.json`.

The file's format, limits and backups are in [configuration](configuration.md#shortcut-bar).

The Debug card of development builds has a button that adds sample profiles, and probe programs for the headset are listed in [building](building.md); neither is part of a release.
