# Shell integration protocol

**Status: protocol 1 implemented, not released.** Everything below is implemented except `theme.derive` and the Wayland input method bridge, which come later. Host tests cover the connection, the sink, running instances, the update handover and App's side. On the Frame, a scripted shell passed launch, presentation, placement, settings, themes, a forwarded press into the sink, previews, the update check, a launcher tap, the lease and a clean exit (2026-10-27). Still open: a visual check, anchors to another overlay, two-hand forwarding and swipe through it, and the update handover end to end, which needs a newer release. Operation names, fields, limits and exit codes may change until protocol 1 ships. An independent design review on 2026-10-26 found the architecture sound; its findings are folded in.

Alternative Steam Frame shells can use Full Keyboard as their keyboard. A shell launches the keyboard, controls its visibility and placement, matches it to its own look, sets typing settings, controls dictation, and keeps the keyboard updated together with itself. The keyboard keeps its own rendering, key state, composition and input delivery. The shell only sees what the user has committed.

## Goals and limits

- One local control protocol for every shell, whether the shell is a SteamVR overlay, a scene app or a Wayland compositor.
- Default behavior does not change. A keyboard the user starts behaves exactly as it does today and has no shell-control protocol.
- Only the process that launched the keyboard can control it. Nothing can attach to a running keyboard.
- No uncommitted text leaves the keyboard. This covers composition (preedit), swipe candidates, suggestions, dictation partials and the dictation editor's buffer. Only committed text reaches the shell, and only through the text sink.
- A shell that crashes or hangs cannot leave held keys, a stuck panel or a running keyboard behind.
- No clipboard reads, no typed text in logs, no changes to Steam files or global input settings, and no new listening socket.
- The protocol does not render the keyboard inside the shell's scene. The keyboard keeps its own overlay, and the shell places it.

## Standalone and hosted modes

The keyboard runs in one of two modes, chosen at launch and fixed for the process lifetime.

| | Standalone | Hosted |
| --- | --- | --- |
| Started by | The user, or Start with SteamOS | A shell, with `--hosted FD` |
| Shell-control protocol | None | Over the connection the shell passed at launch |
| Instance socket (`vr.sock`) | `recenter-v1`, `quit-v1` | `quit-v1` only |
| Presentation | SteamOS dashboard and passthrough, as today | The shell decides |
| Integration card (Replace native keyboard, Start with SteamOS) | Available | Hidden; replacement and boot start inactive |
| X | Exits, or dismisses in replacement mode | Hides the panel |
| Ends when | Quit, or the user's session ends | Quit, the shell closes its connection, or the shell stays hung (see [Lease](#lease)) |

There is no setting that enables shell integration. A standalone keyboard accepts no shell-control messages, so no shell can take over a keyboard the user started. Its instance socket keeps only the existing recenter request and the new quit request (see [Running instances](#running-instances)).

The Integration card belongs to standalone use with the SteamOS dashboard. In hosted mode its settings are unavailable. The saved values in `config.json` are kept unchanged for the next standalone launch, and the protocol cannot change them.

### Launch-mode policy

Hiding the card is not enough. Several code paths assume standalone use today, so the launch mode becomes an explicit policy that each of them reads:

- The boot-unit callbacks that `main.cpp` installs for VR startup and saving, and the startup-unit reconciliation on start and reload, are skipped in hosted mode. Saved standalone flags are preserved.
- X hides instead of exiting; today it exits unless replacement is on.
- Settings → Quit remains and ends the hosted keyboard.
- Dictation-driven visibility (dictation passthrough) and pointer-source selection follow shell presentation instead of the dashboard.
- The VR watchdog that hides the panel when the main loop stalls currently runs only with replacement. Hosted mode enables it too.

## Launch

The shell creates a connected socket pair, keeps one end and passes the other to the keyboard as an inherited file descriptor. The shell chooses the output contract explicitly:

```sh
# Typing goes to the Gamescope keyboard focus (normal output) or to the sink.
~/.local/bin/framekeyboard --vr --hosted 3 --input ei --start-enabled
# Sink only: no system input device is opened.
~/.local/bin/framekeyboard --vr --hosted 3 --input none
```

- The installed launcher adds `--input ei --start-enabled` only for a bare `--vr` launch, and in-app updates do not refresh the launcher. Shells therefore always pass the input arguments themselves.
- Normal output goes to the Gamescope keyboard focus through libei. It cannot address a particular shell surface or a Steam field. A shell that wants text for its own UI uses the [text sink](#text-sink).
- The descriptor must be a connected `AF_UNIX` `SOCK_STREAM` socket whose peer runs as the same user (`SO_PEERCRED`). Otherwise the keyboard exits before initializing VR. It sets close-on-exec on the descriptor at once.
- There is no socket path, so there is nothing for another process to connect to. The shell's end of the pair is the only way to control this keyboard. If the shell passes its end to other processes, that is the shell's choice.
- The keyboard sends `ready` once VR is initialized and the panel is ready but hidden. The shell sends `hello` within 10 seconds of `ready`, or the keyboard exits.
- When the connection closes, the keyboard shuts down as Settings → Quit does: it releases held input, saves hosted placement and settings, and exits. To restart the keyboard, the shell launches it again.

### Exit status

A shell distinguishes launch failures by exit status. Status 3 keeps its existing meaning for the boot unit.

| Status | Meaning |
| --- | --- |
| 0 | Orderly exit (Quit, connection closed) |
| 10 | Invalid control descriptor |
| 11 | A running keyboard did not accept `quit-v1` (older release or not responding) |
| 12 | Another launch acquired the instance lock first |
| 13 | No `hello` before the deadline, or the lease grace ran out |

### Running instances

One keyboard owns VR per login session. A hosted launch that finds another keyboard running, standalone or hosted, sends `quit-v1` over the instance socket once.

- A keyboard that supports it acknowledges, then exits as Settings → Quit does. A standalone keyboard withdraws replacement so the stock keyboard works again. A hosted keyboard's shell sees its connection close.
- A keyboard started by the boot unit exits with status 3, so `Restart=always` does not respawn it against the handover.
- An acknowledgement is not ownership. The hosted launch then waits up to 10 seconds to acquire the instance lock itself. If another launch acquires it first, the hosted launch exits with status 12 and does not send `quit-v1` again. Two shells therefore cannot keep evicting each other through repeated launches.
- Releases before protocol 1 understand only `recenter-v1` and close the connection without answering. The hosted launch then exits with status 11. The shell may stop that keyboard itself: with `systemctl --user stop framekeyboard-autostart.service` when the boot unit owns it, otherwise with `SIGTERM`, which the keyboard handles as an orderly shutdown. It then launches again.

The next standalone launch, or the next login with Start with SteamOS, returns to standalone mode.

A plain launch while a hosted keyboard is running, for example from the dashboard launcher, does not recenter or take over. It exits with a message that a shell is managing the keyboard, and the running keyboard sends its shell a `launch` event (`source:"launcher"`) so the shell can show the panel. Silent takeover would start respawn fights with the shell. An explicit standalone takeover can be added later.

## Transport

- One JSON object per line, UTF-8, terminated by `\n`, over the inherited stream socket.
- Requests carry a shell-chosen integer `id` and an `op`. Each request gets exactly one response with the same `id`: `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"code","message":"..."}`. The message is short, fixed English text and never contains input text.
- Events carry `"event"` and no `id`. `ready`, `update`, `regions` and sink events are always sent. Others are sent only after `subscribe`.
- Limits: 64 KiB per message, 128 KiB of buffered inbound bytes, and a bounded number of messages parsed per frame. A shell that exceeds a limit or sends malformed JSON gets one `bad_request` error and is disconnected, which ends the keyboard.
- Outbound capacity is reserved for responses and committed output. Telemetry (`geometry`, microphone level, download progress) is coalesced to the latest value and may be dropped under pressure. Sink events are never dropped: when the sink queue is full, the keyboard stops accepting key presses until it drains.
- The VR thread reads and writes nonblocking. While the panel is hidden, the loop waits on the control socket as well, instead of only sleeping up to 250 ms, so requests are answered promptly.
- Network, hashing, update preparation and model downloads run off the VR thread, as today. Profile activation still prepares synchronously on the VR thread, so `select`, `reload` and `theme.set` cost the same frame time they cost from Settings.
- Unknown fields are ignored. Unknown operations return `unknown_op`. This keeps newer shells working with older keyboards and the reverse.

## Session

### `ready` and `hello`

```json
← {"event":"ready","protocol":[1],"version":"0.10.0","session":1}
→ {"id":1,"op":"hello","protocol":1,"client":"Example Shell","client_version":"1.4.0","lease_ms":2000}
← {"id":1,"ok":true,"protocol":1,"version":"0.10.0",
   "features":["pointer","sink","dictation.server","dictation.device","swipe","update"]}
```

`ready` lists the protocol versions the keyboard supports, and `hello` picks one. `session` increases with every process on the same connection, including update successors and rollbacks, so a shell can tell a fresh process from the old one. Any other request before `hello` returns `no_hello`. `features` lists what works now. Features that depend on the device or installation are left out when they are unavailable, for example `dictation.device` before its model is downloaded, or `update` outside a versioned installation. `client` is shown to the user while the [text sink](#text-sink) is on.

### `state` and `subscribe`

`state` returns a snapshot of everything that events report: visibility and its reason, view, current context, geometry, selection, theme, settings, swipe availability, dictation state, whether the sink is on, the pointer mode and update state, plus `last_update` after a rollback. It never includes text. Regions come only as `regions` events.

`{"op":"subscribe","events":["visibility","geometry",...]}` replaces the subscription set. See [Events](#events).

### Lease

`lease_ms` is 500–5000, default 2000. Any message from the shell renews it, and `ping` renews it without other effect. The keyboard answers `ping` only from a VR loop iteration that completed, so a stalled keyboard does not appear alive to the shell.

If the lease expires while the connection stays open, the shell is treated as hung:

1. The keyboard hides the panel, releases held keys, ends forwarded pointers and pauses the sink. It invalidates the current [typing context](#typing-context) and every queued output for it.
2. Host-scoped state (custom theme, chrome, locks, anchor) is kept, because the shell may only have stalled.
3. The next message from the shell renews the lease. The shell must set a fresh context and `show` again before typing resumes.
4. If no message arrives within 30 seconds of expiry, the keyboard exits with status 13. A hung shell therefore cannot leave a keyboard running indefinitely.

Closing the connection ends the keyboard, as described in [Launch](#launch).

### Future roles

Protocol 1 has one controller: the launching shell. A later version may accept more inherited descriptors at launch, for example read-only observers. It would still never accept connections after launch.

## Presentation

| Op | Fields |
| --- | --- |
| `show` | `view`: `keyboard` (default), `emoji`, `dictation`, `settings`; optional `context` |
| `hide` | — |

Only the shell shows the panel, and the SteamOS dashboard gate does not apply. Input is gated on shell visibility instead. This uses the same presentable path as passthrough, so hiding still releases held keys, cancels grabs and drains pointer events first.

Protocol view changes never commit unfinished input. Today, opening emoji or dictation from the panel commits an open composition. A `show` that would do so keeps the composition pending where the view can keep it. Otherwise it fails with `composing` and changes nothing. Hiding keeps an open composition pending for the same context; a context change or the lease running out discards it. (A standalone keyboard still discards it on hide.)

X in the panel hides it and sends `visibility` with `reason:"user"`. The shell decides whether to show it again. Settings → Quit exits the keyboard, and the shell sees its connection close. If the stall watchdog hides the panel, `visibility` reports `reason:"stalled"`, and the shell shows it again when it wants.

## Typing context

A shell can have several text fields. Without a way to tell them apart, switching from a search box to a chat field could deliver queued text, a swipe correction, a held modifier or a late dictation phrase to the wrong field. Each field the shell activates is therefore a typing context:

```json
→ {"id":5,"op":"context","context":"c-41","hints":{"purpose":"search","enter":"search"}}
→ {"id":6,"op":"context","context":null}
```

- `context` is an opaque string of up to 64 bytes chosen by the shell. It may also be passed with `show`.
- Setting a different context, or `null`, first cancels everything addressed to the old one: held keys and modifiers, queued and retried output, open composition (discarded, not committed), pending swipe results and corrections, and Type as I speak phrases still being recognized. Then the new context and its hints take effect together, so a password hint never applies late.
- With the sink on, typing requires a context; key presses without one are ignored. With normal output, a context is optional but still cancels queued output when the shell switches fields.
- Every sink event carries its `context`. The shell discards events for a context it no longer has active.
- Lease expiry and hiding with `"end_context":true` end the context too.

### Hints

| Field | Values | Effect |
| --- | --- | --- |
| `purpose` | `text`, `password`, `pin`, `number`, `email`, `url`, `search`, `terminal` | `password`/`pin`: no swipe, suggestions, dictation or emoji. `number`/`pin`: opens with the numpad shown and Num Lock on. `email`/`url`: no swipe autospace. `terminal`: no autospace or suggestions. |
| `enter` | `enter`, `go`, `search`, `send`, `next`, `done` | Legend on Enter; the key still sends Enter |
| `autocapitalize` | `none`, `sentences`, `words` | Swipe and dictation only: `none` lowercases the first letter of each dictated phrase and swiped word, `words` capitalizes every word, `sentences` keeps the usual casing |
| `language` | language ID | Temporary selection for this context |

Hints belong to their context and are never saved. Saving settings for another reason never writes a hinted value. When the context ends, a temporary language is restored only if the user has not picked another language in the meantime. The user can still change language and settings in the panel.

## Placement and geometry

| Op | Fields |
| --- | --- |
| `recenter` | — |
| `set_width` | `meters`: 0.45–2.0 |
| `set_pose` | `matrix`: 3×4 row-major, standing tracking space; optional `level` |
| `anchor` | `overlay`: overlay key, `matrix`: transform relative to that overlay; `null` clears the anchor |
| `lock` | `move`, `resize`: booleans |

Hosted mode never follows the SteamOS dashboard. Without a pose or anchor, `recenter` uses the existing headset fallback: 0.85 m ahead and 0.65 m below the headset. An anchored panel follows its overlay at the same rate standalone mode follows the dashboard, without repainting. If the overlay disappears or becomes invisible, the panel hides and reports `reason:"anchor_lost"`. Horizon alignment applies to shell poses only when `auto_leveling` is on and the request does not set `"level":false`. Width is the case width, which matches today's resize limits.

Movement and resizes by the user are reported, so the shell can follow them. Locks hide the move line and resizers. Hosted placement is saved separately from standalone placement, so neither mode moves the other's keyboard.

`geometry` events report `width`, `height`, the panel `matrix`, and the key-area rectangle in texture coordinates. They are sent after every change, coalesced to at most 10 per second.

## Appearance

| Op | Fields |
| --- | --- |
| `theme.select` | `theme`: the ID of an installed theme; persists like Settings |
| `theme.set` | `theme`: a full theme object with the [theme schema](configuration.md#layouts-and-styles) |
| `theme.reset` | Drops the custom theme |
| `chrome` | `close`, `settings`, `move`, `resize`, `favorites`: booleans (shown) |

`theme.set` is not written to disk and lasts for the process lifetime. It goes through the same validator as theme files and returns `invalid_theme` with the field name. While a custom theme is active, the Settings theme picker shows it as "Shell theme". Picking another theme there sends a `theme` event with `source:"user"`, and the shell can reapply its theme or accept the choice.

Hiding chrome hides only those controls. The Settings view is still reachable through `show {"view":"settings"}`.

**Deferred past protocol 1:** `theme.derive`, which would build a full theme in the house style (gradients, sides, hover, latched, disabled and notice colours) from four seed colours: `surface`, `key`, `legend`, `accent`. It is advertised as feature `theme.derive` once available. Until then shells send a full theme.

## Settings

| Op | Fields |
| --- | --- |
| `settings.get` | — |
| `settings.set` | `values`: object of the keys below |
| `select` | `layout`, `language` and/or `favorite` |
| `reload` | Reload profiles from disk |
| `profiles.list` | Layout, language and theme IDs with display names, plus favorites |

Settings written over the protocol persist, exactly like the same change in Settings, through the same writer and validator.

| Key | Type and range |
| --- | --- |
| `swipe_typing`, `num_lock`, `hide_numpad`, `use_drag_to_move`, `auto_leveling`, `pinned`, `emoji_close_on_select` | boolean |
| `dictation` | `off`, `server`, `device` |
| `dictation_server` | `host[:port]`, validated like the Dictation card; see [Dictation](#dictation) |
| `dictation_send_gap` | 20, 30 or 40 (ms) |
| `dictation_send_paste`, `dictation_direct`, `dictation_fallback` | boolean |

Not writable: `replace_native_keyboard` and `start_with_steamos` (standalone-only, see [modes](#standalone-and-hosted-modes)), `developer_options`, `dictation_device_model`, `dictation_url`, `dictation_model`, and favorites (read-only in protocol 1). `settings.get` leaves out the standalone-only keys.

- `settings.set` is all-or-nothing: one invalid value rejects the request with `invalid_setting` and the key name.
- `ok` means validated and applied. Saving is asynchronous and the writer coalesces snapshots, so the save result comes later as a `saved` event with `through` set to the highest request `id` it covers and `ok` true or false.
- Writes from Settings and from the protocol apply on the VR thread in arrival order; the later one wins. If Settings has unapplied edits open, a protocol write updates only the pending values for the keys it wrote.

`settings` events report the changed keys and `source` (`user`, `shell`, `reload`). A save result covers every write queued before it, since the writer reports only once nothing newer is pending; `saved` therefore names the highest request it covers.

## Dictation

| Op | Fields |
| --- | --- |
| `dictation.start` | Opens dictation the same way the mic key does |
| `dictation.stop` | Stops capture; recognized text stays in the dictation editor |
| `dictation.test_server` | Tests `dictation_server` like the card's Test |
| `dictation.model` | `action`: `status` or `download` |

- The dictation editor stays inside the keyboard. Its text reaches the target only through the user's Send, or per phrase with Type as I speak, and so through the normal output path or the sink.
- Words that Type as I speak has already typed are commits. Its partial hypotheses are never sent.
- `dictation` events report only `state` (`off`, `idle`, `listening`, `recognizing`, `paused`, `error`), the microphone level (0–100, at most 10 per second), error codes, download progress in bytes, `model_ready` when a download ends, and `test` with a Test's result (`reachable`, `unreachable`, `no_answer`, `server_error`, `bad_reply`, `helper_stopped`). They never contain partial, phrase or editor text.
- A model download is several hundred megabytes and must stay a user decision. `download` shows the keyboard's existing download confirmation in the panel, and the user confirms it there. The shell is told through `dictation` events.

**Servers chosen by the shell (owner, 2026-10-26).** A speech server receives the user's speech, and the recognition prompt also carries the editor text before the cursor. That text is unsent and must not leave the keyboard by the shell's choice. The keyboard records who chose the current `dictation_server` (`dictation_server_source` in `config.json`). For a server the shell set, the prompt carries only the language profile's prompt, without editor context. A server the user chose or tested in the Dictation card gets the full context, as today.

## Updates

Shells update the keyboard together with themselves, without showing anything to the user.

| Op | Fields |
| --- | --- |
| `update.check` | Optional `version` |
| `update.install` | Optional `version`; `when`: `idle` (default) or `now` |
| `update.cancel` | Cancels a pending install before activation |
| `quiesced` | The shell's last message before a restart (see below) |

- `update.check` answers once the check is done: `{"latest":"0.10.1","available":true}`, or a failure reason. Up to date, `latest` is the running version. It uses the same fixed-repository check as Settings, and never downloads.
- `update.install` installs the newest stable release, or the exact stable `version` given. Pinning lets a shell ship with a keyboard version it has tested. The helper already fetches exact tags. Drafts and prereleases are still rejected. Before activation, the helper checks that the package supports the protocol version in use, or fails with `incompatible`.
- Verification, staging, startup confirmation and rollback are the same as [updates from Settings](updates.md). The keyboard shows no update UI for shell-requested updates, and the Settings button shows only status.
- `update` events report `checking`, `available`, `downloading` (with `bytes` and `total` of the archive, coalesced, reported every 256 KiB), `prepared`, `quiesce` and `restarting`. Failures report the same short reason codes as Settings (`network`, `rate_limit`, `checksum`, `locked`, `disk`, `invalid_package`), plus `incompatible`, and `expired` when the prepared package was lost; the shell may install again.
- A prepared update for a shell waits for idleness as long as it takes; its staged package is kept until the handover or `update.cancel`.

### Idle

Preparation runs in the background while the user may keep typing. Idleness is checked immediately before activation, not before downloading. Today, reaching Prepared ends dictation and quits unconditionally; hosted updates change that.

With `when:"idle"`, the keyboard activates only when all of these hold: the panel is hidden, no key is held, no composition is open, no dictation draft is unsent, no recognition or swipe decoding is pending, no output is queued, retried or being sent, and every sink event is acknowledged. Hidden does not mean empty: a hidden panel can still finish phrases that were already spoken. If the check fails, the prepared update waits and the check repeats.

With `when:"now"`, activation starts as soon as preparation finishes. It **may discard unfinished work**: an open composition and an unsent dictation draft are discarded, never committed.

### Handover

The shell's connection is the hosted session across an update. The original keyboard process exits during the update, so a shell that supervises the process ID must not relaunch the keyboard while the connection is still open.

1. When idle, the keyboard sends `update {"state":"quiesce"}`.
2. The shell stops sending new requests and replies with `{"op":"quiesced"}` as its last message. The keyboard handles and answers everything received before it, pings included; a render or update check still in progress fails. If `quiesced` does not arrive within 2 seconds, activation is postponed and the update returns to `prepared`.
3. The keyboard checks idleness again, since a `show` sent before `quiesced` may have reopened the panel; if it is no longer idle, the update returns to `prepared` and the keyboard asks again later. Otherwise it writes every reply out, sends `update {"state":"restarting"}`, stops reading, and hands over only once that is written in full. A shell that does not read within a second postpones the update the same way.
4. It passes the control descriptor, and its instance-lock descriptor, to the update helper. The helper starts the successor with both, so no other launch can take the instance lock during the gap. The descriptor number may change; the successor gets its own number in `--hosted`.
5. The successor stays non-interactive during the existing three-second startup probation. It sends `ready` only after the helper has committed the update, with the next `session` number.
6. The shell sends `hello`, then its host-scoped state again: custom theme, chrome, locks, anchor and context. Settings, selection and hosted placement are kept on disk.

If the successor fails probation, the helper rolls back and starts the previous release on the same descriptors. Its `ready` reports the old version, and `state` includes `last_update` with the failure code. If neither can start, the helper closes the descriptor and the shell sees the connection close. The shell may then launch the keyboard again.

Replacement never runs in hosted mode, so there is no Steam request to hand back or adopt, and the boot service is not involved.

Implementation notes for the updater, from the current code:

- `Updates` reserves descriptor 3 for the helper's private socket and closes every higher descriptor (`updates.cpp`). Both hosted descriptors must be passed explicitly.
- `update_helper.py` passes only its acknowledgement socket to the successor. It must pass the hosted descriptors for the successor, the rollback launch, and the old-binary fallback.
- The fallback `execv` in `main.cpp` must keep them too.
- Rollback snapshots cover `config.json` and `placement.json` only. They must include the hosted placement file.

## Pointer forwarding

A shell that owns the controllers, for example a scene app, forwards its own pointer instead of having the keyboard read controller poses. This avoids double handling of the trigger.

```json
→ {"op":"pointer","hand":"left","phase":"move","u":0.412,"v":0.733}
→ {"op":"pointer","hand":"left","phase":"down","u":0.412,"v":0.733}
→ {"op":"pointer","hand":"left","phase":"up","u":0.413,"v":0.731}
→ {"op":"pointer","hand":"left","phase":"scroll","dx":0,"dy":-1}
→ {"op":"pointer","hand":"left","phase":"cancel"}
```

- Enabled with `{"op":"pointer_mode","mode":"forwarded"}`. While it is on, every native pointer source is off: dashboard events, the keyboard's own controller pointer and grip grabs. Every switch releases keys and clears pointers, as switching between dashboard and passthrough pointers does today.
- `u`,`v` are coordinates on the full overlay texture from the top left, in 0–1. Dropdowns and flick previews can extend outside the case, so the keyboard sends `regions` events with the accepted rectangles whenever they change. A position outside every region counts as leaving: the press is cancelled as on focus leave.
- The move line and corner resizers need a world ray and separate overlays, so they are disabled in forwarded mode. The shell moves and resizes the panel with `set_pose` and `set_width`.
- `pointer` messages have no `id` and get no response, to keep latency low. They are applied in order. Moves are never merged across `down`, `up` or `cancel`. Hover moves may be merged to the latest. Moves during a press are kept, up to 8 per hand per frame, so swipe recognition keeps enough samples.
- `cancel`, hiding, a context change, lease expiry and the connection closing cancel each forwarded pointer. A cancel never releases into a key press.
- Haptics stay with the keyboard. It pulses the hand named in `hand`, through the existing per-controller path. A shell that wants to play haptics itself sets `"haptics":"shell"` and receives `haptic` events (`hand`, `kind`: `press`, `release`, `hover`) instead.

## Text sink

By default, the keyboard types into the Gamescope keyboard focus. A shell that wants text for its own UI turns on the sink:

```json
→ {"id":9,"op":"sink","on":true}
→ {"id":10,"op":"context","context":"c-41","hints":{"purpose":"text"}}
← {"event":"commit","seq":1,"context":"c-41","text":"Hello"}
← {"event":"key","seq":2,"context":"c-41","key":"Enter","modifiers":[]}
← {"event":"key","seq":3,"context":"c-41","key":"KeyA","modifiers":["Ctrl"]}
→ {"op":"ack","context":"c-41","seq":3}
```

### Exclusive destination

While the sink is on, it is the only destination for committed output. Typed characters, named controls, Copy and Paste, emoji, dictation Send and Type as I speak all go to the sink. libei, uinput and the clipboard are not used; `dictation_send_paste` is ignored. The sink works with `--input none`.

Turning the sink on or off first cancels through the old destination: held keys are released there and queued output for it is dropped. Typing resumes only once a valid context is set for the new destination.

### Events and acknowledgement

- `commit` carries committed text. `key` is one atomic action, press and release, for a named control. A held key's repeats arrive as further `key` events with `"repeat":true`. Modifier presses alone are not sent. Latched or held modifiers apply to the next `key` and are listed in `modifiers`.
- Key names are the logical key IDs from the layout (`Enter`, `Backspace`, `ArrowLeft`, `KeyA`, ...), never evdev or Steam codes. Copy and Paste arrive as `KeyC` and `KeyV` with `Ctrl`.
- Events are ordered and numbered by `seq`. A swipe correction arrives as `Backspace` keys followed by the new commit, in order, in the same context.
- A socket write does not mean a field accepted the text. The shell acknowledges with `ack` (`context`, highest accepted `seq`), or rejects one event with `{"op":"reject","context":...,"seq":N}`. A rejected commit is kept for explicit retry, as failed commits are in native replacement. Dictation Send clears the editor only after its commit is acknowledged.
- One event is in flight at a time (owner, 2026-10-26): the next is sent only after the shell answers the last, exactly as the output queue waits for an asynchronous transport. Each is one local round trip.

### Privacy

- Only committed text is sent. Composition, preedit, candidates, swipe suggestions, dictation partials and the dictation editor buffer stay inside the keyboard. A Japanese, Chinese or Korean composition reaches the sink only when it is committed. A swipe word reaches it only once accepted.
- While the sink is on, the panel shows a "Typing into <client>" line in the case, using the `client` from `hello`, and a **Stop shell typing** button. The user can turn it off there. The shell is then told with a `target` event, and output returns to the normal destination after the cancellation above.
- Nothing from the sink is logged by the keyboard. The shell must treat it as typed input.

## Wayland input method bridge

Later, and separate from this connection. Compositor shells can then use the standard `zwp_input_method_v2` and `zwp_virtual_keyboard_v1` protocols, with `text-input-v3` content types mapped to [hints](#hints). The bridge follows the same privacy rule: preedit stays in the keyboard and only commits are sent, even though the input method protocol has a preedit channel. The control connection still handles presentation, placement, theme, dictation and updates.

## Preview render

`render` returns a PNG of the keyboard, settings, emoji or dictation view with a given theme, layout and language, for a shell's own pickers.

- It renders isolated sample state, never the live editor, composition, candidates, recent emoji or settings values that identify the user.
- It uses the existing desktop renderer in a worker, one request at a time.
- `{"op":"render","view":"keyboard","layout":...,"language":...,"theme":...,"width":800}`; omitted fields take the current selection. `width` is 64 to 2048 pixels and keeps the texture's proportions; without it the image is the panel's own 1600 pixels. `theme:"shell"` renders the shell's current custom theme.
- It returns `{"path":".../preview-N.png"}` in the keyboard's private runtime directory and does not send image data over the connection. At most 4 previews are kept; older ones are overwritten, and all are removed when the keyboard exits.

## Events

| Event | Fields |
| --- | --- |
| `ready` | `protocol`, `version`, `session` (always sent) |
| `visibility` | `visible`, `view`, `reason`: `shell`, `user`, `anchor_lost`, `tracking`, `lease`, `stalled` |
| `geometry` | `width`, `height`, `matrix`, `keys_uv`, `source`: `user`, `shell`, `recenter` |
| `regions` | `rects`: accepted pointer rectangles, each `u`, `v`, `width`, `height` in texture coordinates (always sent in forwarded mode) |
| `theme` | `id`, `custom`, `source` |
| `selection` | `layout`, `language`, `source` |
| `settings` | `changed`, `source` |
| `saved` | `through`, `ok` |
| `dictation` | `state`, `level`, `error`, `download` |
| `update` | `state`, `latest`, `bytes`, `error` (always sent) |
| `target` | `owner`: `normal`, `sink`; `context` |
| `haptic` | `hand`, `kind` (with shell haptics) |
| `launch` | `source`: `launcher`, when the user launched the keyboard while the shell runs it |
| `commit`, `key` | Sink only; `seq`, `context` |

## Errors

`no_hello`, `unknown_op`, `bad_request`, `unavailable` (a feature missing from `features`), `composing`, `invalid_theme`, `invalid_setting`, `incompatible`, `busy`, `failed`.

## Versioning

`protocol` is an integer that changes only for incompatible changes. New operations, fields and events are added within a version and advertised in `features`. The keyboard supports the previous protocol version for at least one release after a new one ships.

## Implementation order

Steps 1–7 and the preview render are implemented (2026-10-27); `theme.derive` and the Wayland bridge remain.

1. Launch-mode policy: hosted flag, skipped boot-unit callbacks, X and Quit behavior, hosted watchdog.
2. Hosted launch: `--hosted FD`, descriptor checks, exit statuses, `ready`/`hello`/`state`/`subscribe`, lease and grace, exit on close, `quit-v1` with single eviction, a development test shell under `tools/` that launches the keyboard.
3. Presentation without implicit commits, typing contexts and hints, width, pose, anchor, locks, chrome, separate hosted placement.
4. `theme.set`, settings with save results, `select`, profiles.
5. Updates without UI: exact versions, idle check before activation, quiesce handover, descriptors and instance lock through the helper, rollback with hosted placement.
6. Dictation control and the shell-chosen server rule.
7. Pointer forwarding with regions and scroll, then the exclusive text sink with acknowledgements.
8. Preview render, `theme.derive`, then the Wayland bridge.

Each step needs host tests with a fake shell: lease expiry and grace, connection close, context changes with queued output, held-key release on every exit path, a standalone keyboard that has no shell-control protocol, an older keyboard that rejects `quit-v1`, and malformed or oversize input. The update step needs offline tests for quiesce timeout, probation failure and rollback on the same descriptor. The pointer and sink steps then need headset validation against the test shell before validation with a real application.

## Decisions

The draft's open questions were settled by the owner on 2026-10-26: a server the shell chose gets no editor context; `when:"now"` discards an unsent dictation draft; the `hello` deadline is 10 seconds and the lease grace 30 seconds.

## Trying it

`tools/shell-test.py` launches the installed keyboard in hosted mode, greets it, keeps the lease alive, acknowledges sink events and prints events. Type requests as JSON or as shortcuts such as `show`, `hide`, `sink on`, `context c1 text` or `pointer forwarded`. It never prints committed text unless asked to with `--print-text`.
