# Terminal edit keys: design

Status: implemented. Host-tested; the headset checks at the end are open.

Cut, Copy and Paste send Ctrl+X, Ctrl+C and Ctrl+V. In a terminal, Ctrl+C interrupts the running program, Ctrl+X reaches it too (nano exits on it), and Ctrl+V pastes nothing. The keyboard's own paste paths, dictation's paste Send and Type as I speak, and KDE Desktop Unicode set to Paste, press Ctrl+V and have the same problem. Terminals differ, so the keyboard detects the focused terminal and uses its own keys. Everywhere else nothing changes.

## Signals

| Signal | Used | Why |
| --- | --- | --- |
| Gamescope focus: the focused window's process | Yes | `DesktopFocus` already reads `GAMESCOPE_FOCUSED_WINDOW` and `GAMESCOPE_FOCUSABLE_WINDOWS` on its own thread, and reads the focused process's command line once per newly focused process to find the Desktop's `kwin_wayland`. Gamescope takes that process ID from the X server (XRes client IDs), so it is the host's ID of the process that owns the window, also for Flatpak apps and on any of Gamescope's X displays. The same read now also yields the program's file name. No new I/O. |
| Window title | No | It can hold typed text, and the program name is enough. |
| `WM_CLASS` | No | Window IDs belong to one X display. `DesktopFocus` talks to Steam's display, while a desktop app can be on another of Gamescope's displays, so the same ID could name another app's window there. The process ID has no such ambiguity. |
| Accessibility (AT-SPI) | No | See below. |
| Shell `terminal` purpose hint | Yes, as a fallback | A hosted shell that declares its active field a terminal gets terminal keys when the focused program is not recognized. |

### Why not AT-SPI

Probed read-only on a current SteamOS build (Gamescope session, Desktop not running):

- The accessibility bus runs (`at-spi-bus-launcher`, `at-spi2-registryd`), but `org.a11y.Status` reports `IsEnabled` and `ScreenReaderEnabled` both false. Qt applications, Konsole included, connect to that bus only when one of them is true (Qt's `DBusConnection::serviceRegistered`, or `QT_LINUX_ACCESSIBILITY_ALWAYS_ON`). Only Steam's web helper and one GTK 3 app were registered. Konsole, the terminal SteamOS ships, would never be seen.
- Turning `IsEnabled` on is a session-wide accessibility switch, not the keyboard's to flip. It would make every Qt app, and Chromium-based Steam UI, build and update accessibility trees. That costs CPU and memory on the headset.
- AT-SPI has no "focused object" query. Following focus means registering for focus events, which makes every registered application send them over the bus, or walking each application's tree.
- kitty, foot, Alacritty, WezTerm, xterm, urxvt and st expose no accessibility tree at all.
- SteamOS's Desktop is a separate `dbus-run-session` with its own runtime directory (`nested_plasma`), so its accessibility bus would be another socket (`nested_plasma/at-spi/bus_0`) with the same switch. With no Desktop running, it could not be probed.

AT-SPI would add cost and a global setting for no terminal that SteamOS actually has. It can be revisited if a terminal appears that only AT-SPI can identify.

### The shell hint

`purpose: "terminal"` on the current context, with normal output (sink off), means the shell says the field it activated is a terminal. If the focused program is recognized, its own keys win. Otherwise the keyboard uses Ctrl+Shift, the most common terminal convention. A shell whose terminal needs other keys can keep the text sink on, where nothing changes: Cut, Copy and Paste still arrive as `KeyX`, `KeyC` and `KeyV` with `Ctrl`, logical key IDs the shell maps itself.

## Mapping

Matching is on the file name of the focused process's program (`argv[0]`), exactly, from a fixed list. Unknown programs keep the standard keys.

| Terminal | Programs | Copy | Paste | Source |
| --- | --- | --- | --- | --- |
| Konsole | `konsole` | Ctrl+Shift+C | Ctrl+Shift+V | `src/session/SessionController.cpp`: `Konsole::ACCEL` (Ctrl+Shift) + C and + V |
| GNOME Terminal | `gnome-terminal-server`, `gnome-terminal` | Ctrl+Shift+C | Ctrl+Shift+V | `org.gnome.Terminal.gschema.xml`: `copy` `<Control><Shift>c`, `paste` `<Control><Shift>v` |
| Ptyxis | `ptyxis` | Ctrl+Shift+C | Ctrl+Shift+V | `src/org.gnome.Ptyxis.gschema.xml.in`: `copy-clipboard` `<ctrl><shift>c`, `paste-clipboard` `<ctrl><shift>v` |
| GNOME Console | `kgx` | Ctrl+Shift+C | Ctrl+Shift+V | `src/kgx-application.c`: `term.copy` `<shift><primary>c`, `term.paste` `<shift><primary>v` |
| kitty | `kitty` | Ctrl+Shift+C | Ctrl+Shift+V | kitty.conf docs: `kitty_mod` defaults to `ctrl+shift`; `copy_to_clipboard` `kitty_mod+c`, `paste_from_clipboard` `kitty_mod+v` |
| foot | `foot`, `footclient` | Ctrl+Shift+C | Ctrl+Shift+V | foot.ini(5): `clipboard-copy` `Control+Shift+c`, `clipboard-paste` `Control+Shift+v` |
| Alacritty | `alacritty` | Ctrl+Shift+C | Ctrl+Shift+V | alacritty-bindings(5), Linux/BSD: Copy `Control+Shift+C`, Paste `Control+Shift+V` |
| WezTerm | `wezterm-gui`, `wezterm` | Ctrl+Shift+C | Ctrl+Shift+V | WezTerm default keys: `CTRL+SHIFT c` `CopyTo Clipboard`, `CTRL+SHIFT v` `PasteFrom Clipboard` |
| LXTerminal | `lxterminal` | Ctrl+Shift+C | Ctrl+Shift+V | `src/setting.h`: `COPY_ACCEL_DEF` `<Primary><Shift>c`, `PASTE_ACCEL_DEF` `<Primary><Shift>v` |
| Ghostty | `ghostty` | Ctrl+Shift+C | Ctrl+Shift+V | `src/config/Config.zig`: on Linux `.{ .ctrl = true, .shift = true }` with `c` `copy_to_clipboard` and `v` `paste_from_clipboard`, performable, so Copy with nothing selected reaches the program, as in Konsole |
| st | `st` | Ctrl+Shift+C | Ctrl+Shift+V | `config.def.h`: `TERMMOD` (Control+Shift) + C `clipcopy`, + V `clippaste` |
| rxvt-unicode | `urxvt`, `urxvtd`, `rxvt-unicode` | Ctrl+Alt+C | Ctrl+Alt+V | rxvt(1): "Ctrl-Meta-c and Ctrl-Meta-v to interact with the CLIPBOARD selection"; Meta is Alt by default |
| xterm | `xterm`, `uxterm`, `koi8rxterm` | nothing | nothing | xterm(1): selecting copies to PRIMARY (`selectToClipboard` defaults to false); no key pastes the clipboard; `modifyOtherKeys` defaults to 0, so Ctrl+Shift+C sends the same ^C as Ctrl+C |

Never used: Ctrl+Insert, Shift+Insert, Shift+Delete. Shift+Delete deletes files permanently in Dolphin, and Shift+Insert pastes the primary selection in foot, Alacritty, WezTerm, Konsole and xterm, which is not what the user copied.

xterm-like terminals copy by selecting and paste with the middle button. Any key the keyboard could send there reaches the program as a control character, so Cut, Copy and Paste send nothing there and show disabled, as a key that can't be used right now does. Dictation's paste Send and Type as I speak type the text instead.

The letter goes through the existing `shortcut_code` lookup, so a non-Latin target keymap still gets the physical C or V.

Terminals not on the list keep today's keys. Python-based ones such as Terminator and Guake run as `python3` and are not detected. Terminals embedded in other apps, such as Dolphin's or Kate's terminal panel or an editor's integrated terminal, belong to that app and keep the standard keys.

## Cut

Cut is disabled in terminals: it sends nothing and shows disabled while a terminal has focus (owner's decision, 2026-11-07). A terminal cannot remove text from its screen or scrollback, and its input line belongs to the shell, so it has no cut. Ctrl+Shift+X is no terminal's cut and would reach the program as ^X. Copy is the key to use there.

Known limit, the same as with a physical keyboard: Konsole disables its Copy action while nothing is selected, and the key then reaches the program. Copy pressed in Konsole with nothing selected can therefore interrupt, as Ctrl+Shift+C does there from a real keyboard. The keyboard cannot tell whether text is selected without reading the selection, which it never does.

## Fallback

When detection fails, the keys are today's: Ctrl+X, Ctrl+C, Ctrl+V. That covers no focus known yet, a lost X connection, an unknown program, a process that has already exited, and every non-Gamescope backend. A wrong terminal guess in an ordinary app would be worse: Ctrl+Shift+C opens the element inspector in Chromium and Firefox.

The keys are chosen when the key goes down and released exactly as pressed, so a focus change while Copy is held cannot leave Shift or Alt down.

## SteamOS's Desktop

Inside the Desktop, Gamescope's focus only shows `kwin_wayland`, and the Desktop runs its own session bus under `dbus-run-session`. AT-SPI is off there as well (its registry does not start), and KWin's D-Bus interface has no call for the active window. KWin does load scripts over that bus, so the Desktop's own KWin reports the active app (`DesktopTerminal`, owner's decision, 2026-11-07):

- **Finding the bus:** while Gamescope's focus is the Desktop, its address is read from the environment of the process that started `kwin_wayland` (whose own is unreadable, as it has capabilities); only `DBUS_SESSION_BUS_ADDRESS` is kept.
- **The script:** the keyboard writes a few lines of KWin script to `/run/user/<uid>/framekeyboard/kwin-desktop-focus.js` and loads it with `org.kde.kwin.Scripting.loadScript` as `framekeyboard-desktop-focus`, after unloading any copy a crashed keyboard left. On each `workspace.windowActivated`, and once as it starts, it calls `Active(resourceClass)` on `org.framekeyboard.DesktopFocus`, a name the keyboard owns on the Desktop's bus. It sends the app ID only (`org.kde.konsole`, `lxterminal`, an X11 window's class such as `XTerm`), never the caption. It takes KWin 6's names, else KWin 5's. The keyboard deletes the file once the script's first report arrives, as KWin has read it by then, or when it unloads the script; `uninstall.sh` removes one left by a keyboard that stopped first.
- **Matching:** `app_edit_keys` matches the app ID, case-insensitively and whole, against the same terminal list, and against the reverse-DNS IDs that differ from their programs' names: `org.kde.konsole`, `org.gnome.Terminal`, `org.gnome.Ptyxis`, `org.gnome.Console`, `org.wezfurlong.wezterm` and `com.mitchellh.ghostty`. An ID is never matched by its last part alone, which would take any `org.example.st` for a terminal.
- **Cost:** no polling. The watcher thread waits on the bus's descriptor with Gamescope's and wakes when KWin reports a change. Attaching makes a few calls, each bounded to 100 ms, once per Desktop session or 5-second retry.
- **Session only:** nothing is written to KWin's settings. Detaching, or the keyboard quitting, unloads the script; a Desktop that closes takes it along.
- **Failure:** without `libsystemd.so.0` (loaded at runtime, never linked), when the script fails to load or never reports, or when KWin changes its scripting API, the Desktop's keys stay Ctrl+X, Ctrl+C and Ctrl+V, as before.
- KDE Desktop Unicode set to Paste and dictation's paste take the detected keys too. A hosted shell's `terminal` hint still applies where nothing is detected.

## Steam fields and the shell sink

- A Steam field reached through native replacement, or a game or app streamed from a PC through Streaming support, is never a terminal, whatever Gamescope's focus is: standard keys. The keyboard cannot see which program has the PC's focus.
- With the shell's text sink on, nothing changes (see the shell hint above).

## Privacy

- Only the program's file name is read, from `/proc/<pid>/cmdline`, as `DesktopFocus` already does. Nothing else of the command line is kept.
- The name is compared against the fixed list on the watcher thread and dropped. Only the resulting kind (standard, Ctrl+Shift, Ctrl+Alt, selection-only) is kept, as one atomic value.
- Inside SteamOS's Desktop, only `DBUS_SESSION_BUS_ADDRESS` is taken from the environment of `kwin_wayland`'s parent, and the script sends only the active window's app ID, never its caption. Only reports from KWin's own unique name count. The app ID is matched and dropped like a program name.
- No window titles, no `WM_CLASS` from Gamescope's displays, no accessibility data, no clipboard or primary selection reads. Program names, app IDs and kinds are never logged.

## Cost

- No new thread or polling. The watcher thread already wakes on Gamescope's focus property changes and reads the focused process's command line once per newly focused process. Inside SteamOS's Desktop it also holds one connection to the Desktop's bus, which wakes it only when KWin reports another active window, or every 5 s while attaching keeps failing. The classification is one string comparison against about 20 names at that moment.
- The VR thread reads one atomic value per Cut, Copy or Paste press and per paste. Nothing blocks it.

## Code

- `terminal_keys`: the program list, the kind, and the chord for each action and kind. Pure and unit-tested.
- `DesktopFocus::edit_keys()`: the focused program's kind, never blocking.
- `KeySink::edit_keys()`: Gamescope's sink reports it; the native replacement sink reports standard for a Steam field or a stream from a PC; the shell sink reports standard while it is on.
- `App`: combines the sink's kind with the shell hint, picks the chord at key down, and uses the Paste chord for dictation's paste paths. `EiSink` uses the Paste chord, with the hint, for the Desktop's Unicode paste.
- `KeyboardState::down` takes the chord's modifiers instead of always Ctrl.

## Headset checks

On the Frame, with a dedicated scratch file or shell and nothing running that an interrupt would hurt: Konsole from the dashboard (Copy copies a selection, Paste pastes, Cut shows disabled, Copy with nothing selected), xterm (all three show disabled and send nothing), a browser and Kate (unchanged), Konsole inside the Desktop (unchanged), dictation paste Send into Konsole and into xterm.
