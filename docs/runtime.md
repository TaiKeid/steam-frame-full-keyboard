# Runtime and troubleshooting

## Launch and dashboard behavior

The installed **Full Keyboard** launcher uses:

```sh
framekeyboard --vr --input ei --target-language en-us --start-enabled
```

It connects to the running Frame Gamescope compositor at `/run/user/<uid>/gamescope-0-ei`. The headset's VR session must already be running. The app does not start SteamVR, replace the stock keyboard, or register autostart. It is visible and accepts input only while the dashboard is open. Hiding cancels keys, unfinished composition, and grabs.

First launch starts at 95 cm wide, tilted back 50° from upright, just beneath the dashboard. If the dashboard anchor is unavailable, it falls back to 65 cm below eye level and 85 cm forward. Placement follows dashboard position and full rotation, including when a local app tab is selected.

Closing saves the pose, size, and dashboard reference. Reopening restores them. Launching an already running instance recenters it and preserves size; it also returns from Settings to the keys. A launch while the dashboard is hidden queues recentering for its next opening. Later launch flags do not change the running instance's backend or language. Close it before changing those options.

A temporary compositor pause releases held keys and resumes typing when its keyboard device returns, without replaying old holds. A disconnected compositor socket still requires closing and reopening the app. Connection status remains visible even if the interruption happened while the dashboard was closed.

Tracking loss hides the keyboard and releases input. When tracking returns, the panel returns to its pose. A tracking-origin reset or a known saved tracking-space mismatch recenters it while retaining size. Corrupt saved placement is ignored. Forced termination cannot save the latest movement.

Keys have no fixed hold deadline: holding Shift with one hand or repeating Backspace can continue beyond ten seconds. A known controller losing tracking or leaving the panel cancels only its pointer; hiding, application-focus loss, and shutdown release all keys.

## Movement and feedback

Point at the keyboard and squeeze grab/grip to move and rotate it. Either hand works. While grabbing, that hand's thumbstick moves it farther with up and closer with down. Grip must be released once after launch or a controller reconnect. Tracking loss, hiding, recentering, or a 30-second grab timeout ends movement and restores the typing state.

The keyboard reads grip travel from the native Frame controller model. Other controller models are unsupported. Steam updates that change this model may require an app update. No global SteamVR input setting is changed.

Horizon assistance runs only after release: a roll within 5° eases level over 500 ms without changing pitch. Toolbar size controls change width by 5 cm within 45 cm to 2 m.

Press feedback uses 25 ms at full strength; release uses 8 ms at 35%, both at 150 Hz. Hover uses 4 ms at 10% and 240 Hz. Feedback goes to the controller responsible for the interaction. Toolbar actions, repeats, and dragging do not pulse.

## Language and input routing

The target-language option declares the receiving session's existing keymap. It does not set that keymap. The normal launcher assumes US English. Integrated Japanese Romaji/Kana also use this physical map for shortcuts, with composed text sent separately.

For a session already configured for German, close the keyboard and run:

```sh
~/.local/bin/framekeyboard --vr --input ei --target-language de-de --start-enabled
```

Choose the German language and international layout in Settings, then Apply and save. For an existing Japanese keymap and system IME, use `--target-language ja-jis` instead and select the JIS preset. A selected profile that does not match the declared target disables typing while keeping Settings accessible. The launcher also accepts `FRAMEKEYBOARD_TARGET_LANGUAGE` for a different default.

`--ei-socket PATH` overrides the compositor key-input socket. Japanese text follows the matching Wayland socket: for example, `/run/user/1000/gamescope-2-ei` pairs with `/run/user/1000/gamescope-2`. Relative names resolve under `$XDG_RUNTIME_DIR` in both backends. For nonstandard names, add `--text-socket PATH` explicitly; otherwise Japanese text is unavailable rather than being sent to the default compositor. `--text-socket` requires `--input ei`. Close the running keyboard before changing launch options, since launching a second instance only recenters the first.

For example, with the chosen compositor already using the US physical keymap:

```sh
~/.local/bin/framekeyboard --vr --input ei --target-language en-us --start-enabled \
  --ei-socket gamescope-2-ei
```

The optional `--input uinput` developer backend requires access to `/dev/uinput`; newly created devices were not discovered by the tested outer VR session. The installed launcher uses libei and does not need this backend.

Copy/Paste send Ctrl+C/Ctrl+V. They do not synchronize clipboards between sessions, implement terminal Ctrl+Shift shortcuts, or read clipboard data. Lock indicators follow presses in this panel and are not synchronized with physical keyboards or an already enabled session lock.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Keyboard is missing | Open the dashboard, wake the headset, then launch Full Keyboard again to recenter |
| Keyboard is offscreen | Relaunch while it is running, or use the recenter icon |
| App exits immediately | Run `~/.local/bin/framekeyboard` in a terminal and read the error; check that the VR session is running |
| Keys do not reach a local app | Focus a harmless text field first; check the language profile and launch declaration |
| Keyboard paused by compositor | Wait for the device to resume; old holds are cleared |
| Input connection was lost | Close and reopen the keyboard to reconnect |
| Wrong characters | Match the session's physical keymap to the selected profile and `--target-language`; legends alone do not change output |
| Copy/Paste fail in a terminal | This version sends Ctrl+C/Ctrl+V, not terminal Ctrl+Shift shortcuts |
| Grip does nothing | Release it fully, point at the keyboard, then squeeze; wake the controller and verify it is a Frame controller |
| Japanese only previews text | Check Anthy, fonts, the native text protocol, and the selected mode; see [Japanese input](japanese.md) |
| Installer says release exists | That version is already installed; launch it or install a newer release |
| New name is absent in Steam Library | A manually added shortcut has its own title; rename it to Full Keyboard in its Properties |

No input is sent outside the dashboard. Streamed VR applications, including VRChat, are outside the supported scope. Focus behavior in nested desktops and floating apps needs testing per application. Do not assume success in a dedicated receiver proves every app works.

## Update, rollback, and removal

Close the keyboard before installing a new version. The installer validates the copied ARM64 binary and profile files in a temporary staging directory before switching `current`, retains the old target in `previous`, and preserves `~/.config/framekeyboard`. A failed copy, validation, or activation removes the incomplete new release so the same version can be retried. Completed releases remain protected from overwrite.

To roll back, close the keyboard and run on Frame:

```sh
cd ~/.local/share/framekeyboard
readlink previous
ln -sfn "$(readlink previous)" current
~/.local/bin/framekeyboard
```

Use this only when `previous` exists and names the release you want. Old releases remain available until you remove them. The installed app label stays Full Keyboard when rolling back to an older binary.

To uninstall, close the app, then run:

```sh
rm ~/.local/bin/framekeyboard
rm ~/.local/share/applications/framekeyboard.desktop
rm -r ~/.local/share/framekeyboard
```

Keep `~/.config/framekeyboard` if you want to preserve themes, layouts, and placement. If you added a Steam Library shortcut manually, remove it through Steam too. No OS files, services, or stock keyboard files need restoring.

## Developer diagnostics

Desktop preview and PNG export never inject input. A direct `--vr` binary launch also starts input-disabled unless a backend, target language, and `--start-enabled` are supplied. For an input-disabled installed VR check, close any running instance first, then use:

```sh
~/.local/bin/framekeyboard --vr --input none --duration 4 --config-dir /tmp/full-keyboard-smoke
```

A second launch only recenters the first instance; it cannot turn an active typing session into a safe test session. Core CTest suites use fake sinks. Input probes are opt-in and require a dedicated receiver or their own exclusively grabbed device. See [building and tests](building.md).
