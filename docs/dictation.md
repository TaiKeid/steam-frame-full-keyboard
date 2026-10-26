# Dictation plan

Status: M0 done; M1 and M2 done except checks that need the real microphone or App integration; M3a built, host-tested and tried in the headset; M3b Send built and host-tested, and its delivery A/B test run (2026-10-25). Not part of the 0.9.0 release. The main path sends speech to a speech server the user hosts; a limited on-device mode covers English and Spanish. The measurements behind this are in [measurements](#measurements-2026-10-24). Items marked *proposal* are Claude's suggestions that the owner has not confirmed yet. Mark milestone checkboxes only after their acceptance conditions pass.

Dictation turns speech into text. The user speaks into a local editor on the keyboard, corrects the text there, and sends it to the focused application in one go. The Frame records audio, splits it into phrases, edits text and types it. Each phrase is turned into text either by a speech server elsewhere on the network, or by a small model on the Frame.

## Decisions

- **Two recognition modes, chosen in Settings:**
  - **Server:** a speech server the user hosts on a machine with a capable GPU. It gives the best quality and speed and covers all dictation languages. A hosted OpenAI-compatible provider can take its place through `config.json` (see [hosted providers](#hosted-providers)).
  - **On device:** Whisper small on the Frame's CPU, for English and Spanish only. It is a fallback: in the measurements it took about 5 s per phrase and loaded the CPU heavily, and larger models were unusable on the Frame alongside VR.
- **The keyboard asks only for the server's address,** as `host:port` in Settings. Model choice and every other recognition setting live in the server's own configuration. An optional model name exists only in the configuration file, for servers without a default model.
- **We don't build a server.** Users run an existing OpenAI-compatible server, `whisper-server` as the reference; the documentation gives setup instructions only.
- **No server in this repository.** This repository does not ship or vendor a server. The documentation describes a reference setup with an existing open-source server.
- **Reference engine:** whisper.cpp's `whisper-server` with the large-v3-turbo model on a GPU, with Whisper's normal 30 s window. On the measured servers it transcribed a 3 s phrase in 0.09–0.24 s, with Japanese at about 3.4% and English at 3.4% errors.
- **Languages:**
  - English, Spanish, Japanese and Chinese first. One multilingual Whisper model covers all of them on a server.
  - On device: English and Spanish only.
  - A language profile without dictation support hides the mic key.
- **Off by default.** Audio leaves the Frame only in Server mode, only to the configured host, and only for a dictation session's phrases or a Test. That includes phrases still queued when capture stops.
- **Settings:** a Dictation card of its own, third, before Experimental.
- **Privacy:** on the Frame, audio and editor text stay in memory only. Nothing is logged or written to disk, and the system clipboard is never read or written.
- **Release:** not in 0.9.0.
- **Credit:** the helper process, `pw-record` capture, transcript cleanup and repetition guard follow the dictation pull request by amirhh00 and adepssimius (`TaiKeid/steam-frame-full-keyboard#2`). That pull request vendored whisper.cpp and ggml into the repository and ran them on the Frame, and is not merged; its ideas are reimplemented on the current code.

## Keyboard design

### Mic key

![Keyboard with the mic key in the side column (owner mockup)](images/dictation-mic-key.png)

The mic key takes the third of the three equal side-column keys, below Copy and Paste and the small emoji key above them (see [emoji picker](emoji.md#side-column)); the mockup predates that column. It is shown only when dictation is usable in the chosen mode for the active language (see [Settings](#settings)); otherwise its slot stays empty.

- **Normal mode:** pressing the mic key enters dictation mode and starts capturing at once.
- **Type as I speak:** pressing it starts capture without leaving the normal keyboard.

### Dictation mode layout

![Dictation mode with the editor above the keys (owner mockup, incomplete)](images/dictation-mode.png)

The mockup is incomplete; where it differs, the rules below apply.

- An editor replaces the function-key row. Esc stays at its left.
- **Hidden:** F1–F12, Print Screen, Scroll Lock, Pause, Tab, Insert, both Ctrl keys, Super, both Alt keys, Steam and Menu, and the Copy and Paste side keys.
- **Send** takes the place of the right-hand Alt to Ctrl keys.
- **Undo last phrase** takes the first freed side slot, directly above the mic key.
- Letters, digits, punctuation, Shift, Caps Lock, Backspace, Delete, Enter, the arrows, Home/End and the number pad stay and edit the editor. PgUp/PgDn scroll it by a page, locally.
- The toolbar stays. Settings and the language button work only while not capturing; X, Pin, Recenter and Esc keep working.
- Swipe typing and the emoji key are unavailable in dictation mode until they can type into the editor.

### Capturing

- The mic key starts and stops capture: start, pause, resume, pause. Leaving the mode is done with Send, which commits, or Esc, which discards.
- **Pause:** the phrase being spoken ends at the pause, and its audio is recognized and inserted. Resuming starts a new phrase.
- **Mic key while capturing:** five vertical bars replace the mic icon and move with the speech level. They are flat during silence.
- **Editor while capturing:** a waveform overlay covers the editor and the existing text is greyed out. The editor is read-only. The insertion point stays visible.
- **Phrases:**
  - The Frame splits speech at pauses of about 0.6 s, and at 25 s at the latest.
  - Each phrase is sent to the recognizer while capture continues, and is inserted at the cursor as soon as its text returns.
  - Silence is never sent, which prevents most of Whisper's invented phrases.
- **Context:** each phrase includes up to 200 characters of the text before the cursor as Whisper's prompt. This improves casing, punctuation and names, and makes inserting in the middle of the text read naturally.
- **Script prompts:** for Chinese the prompt also fixes the script, because Whisper otherwise mixes simplified and traditional characters. For Japanese it uses 、 and 。, which Whisper often omits.
- **Joining:**
  - In space-separated languages (English, Spanish), a space is added at either edge where needed; in Japanese and Chinese, none.
  - Whisper's casing is kept. The context prompt already tells it when a phrase continues a sentence, and lowercasing would break "I" and names.
  - When text follows the cursor, the phrase's final period is dropped.
- **Order:** resuming while a phrase is still pending queues the new audio, and phrases are inserted in order.
- **Late text:** a phrase is inserted at the cursor as it is when its text arrives, even if the cursor moved or the text changed meanwhile.
- **Cleanup of returned text:**
  - Must be valid UTF-8, or the phrase is dropped.
  - Control characters are removed.
  - Non-speech annotations such as `[BLANK_AUDIO]` and `(music)` are removed.
  - Runs of four or more repeated words are collapsed.
  - Text far longer than the phrase could hold is treated as a failed phrase (*proposal*: more than 25 characters per second of audio).
- **Automatic stop:**
  - after 60 s of silence;
  - when the editor reaches 900 characters;
  - when capture or recognition fails (see [errors](#errors-and-overlays)).
- **Waiting:** if a phrase takes longer than about 1 s, an overlay over the editor says it is waiting for recognition.

### Editor

- The editor holds plain text with line breaks. Tab characters are not supported.
- **Cursor:** a laser click places it, using Pango's position lookup. The arrows, Home and End move it. There is no text selection.
- **Scrolling:** the thumbstick scrolls the editor. With the dashboard open, the overlay requests SteamVR's scroll events; in passthrough, a new stick action is read directly. The view follows the cursor as text is inserted.
- **Counter:** a character counter shows, for example, `412/900`. Characters are counted as Unicode code points, not bytes.
  - Dictation cannot start or continue at 900 characters or more.
  - A phrase still pending when 900 is reached is inserted, up to the hard cap.
  - Manual typing is allowed up to 1000 characters; the counter may then show `1000/900`.
- **Undo last phrase:** removes the most recently inserted phrase. Repeated presses remove earlier phrases. A phrase whose text was edited since is no longer undoable.
- **Composers:** Japanese and Chinese composers commit into the editor instead of the application. Preedit and candidates keep their usual place.

### Esc

Esc in dictation mode is local and never reaches the application.

| State | Esc does |
| --- | --- |
| Capturing | Stops capture and discards phrases not yet inserted. Stays in dictation mode, so the text can be edited. |
| Not capturing, editor empty | Returns to the normal keyboard. |
| Not capturing, editor has text | Asks "Discard text?" in an overlay. Discard leaves the mode; Keep returns to the editor. Esc again means Discard. |

### Send

- **Send** types the editor text into the focused application, presses Enter, clears the editor and returns to the normal keyboard. It is disabled while the editor is empty.
- **Enter** in the editor only adds a line break.
- **Line breaks:**
  - Line breaks and spaces at the start and end are dropped.
  - Those between paragraphs are kept and sent as Shift+Enter, so chat applications insert a line break instead of sending early.
  - A Steam field that holds only one line, such as search, cannot take Shift+Enter. There each run of line breaks, with the spaces around it, becomes a single space, so `a\n\n\nb` is sent as `a b`. Multi-line Steam fields, such as the friends chat message box, keep the line breaks.
  - Only the final Enter is a plain Enter.
- **Delivery:**
  - Text is cut into commits of at most 32 code points through the existing output queue. Each line break is a native key entry. Each piece goes out once the previous one is delivered, so a failure knows exactly what arrived.
  - Plain text to local apps goes one acknowledged character at a time; whole commits scrambled it in the [M3b test](#m3b-results-2026-10-25). Send waits 40 ms between characters by default, about 37 s for 900 characters. The **Send pace** setting offers 30 and 20 ms, faster but more likely to swap neighbouring characters when the Frame is busy. Other typing keeps 20 ms.
  - Sending waits until no key is held, so a held modifier cannot turn letters into shortcuts.
  - The editor is locked and shows progress while sending: delivered text leaves the editor as it goes, the rest is dimmed without a caret, and the counter shows `Sending 120/412`. Esc stops sending.
  - If delivery fails or is interrupted (Esc, hiding the panel, losing the target, or no answer for 5 s), the unsent remainder stays in the editor and the mode stays open, as failed Japanese commits stay in the preedit today. Send continues from there.
    - Text the target acknowledged leaves the editor.
    - A commit that timed out may have arrived anyway, so the editor underlines the text that may have arrived in amber: the one character that timed out when text went one character at a time, or the whole commit otherwise. Editing the marked text removes the mark.
    - The final Enter is tracked on its own, so a retry never sends the message twice. If only the Enter fails, the editor is empty and Send presses just Enter.
- **Send while capturing:** capture stops, Send waits for pending phrases, then sends. The wait can be cancelled with Esc and ends after the request timeout.
- **Target:** Send types into whatever has keyboard focus at that moment. Switching windows mid-dictation is the user's choice.

### Dashboard, passthrough and focus

- **Dashboard closed while capturing:** passthrough is turned on automatically, so the keyboard stays open and capture continues, even if Steam asks to close the keyboard. That passthrough stays on until dictation mode ends.
- **Dashboard closed while not capturing:** the keyboard hides as usual. The dictation state (mode, editor text, cursor, scroll) is kept in memory and restored when the keyboard opens again. A paused editor whose capture already turned passthrough on stays open, because that passthrough lasts until the mode ends.
- **X** discards everything, without asking: capture stops, pending phrases and the editor text are dropped, and dictation mode ends.
- **Typing target lost**, for example when Steam's field closes:
  - **Editor mode:** dictation continues into the editor and the panel stays open. The user can select the right field and Send there. In replacement mode this needs its own presentation rule, because today the panel hides when Steam's request ends, whatever passthrough says.
  - **Type as I speak:** capture stops at once, and phrases not yet typed are discarded.
- **Switching windows** is handled the same way as closing and reopening the dashboard.
- **Unchanged:** held keys are still released on hide and target change, and the stock keyboard fallback is unchanged.
- **App exit or restart:** the editor text is lost.

### Type as I speak

A second checkbox, **Type as I speak**, skips the editor. The mic key starts and stops capture on the normal keyboard, and each phrase is typed into the focused application as soon as its text returns.

- No character limit and no counter; the output queue paces delivery.
- Undo last phrase is not offered, because the cursor in the target may have moved.
- Esc while capturing stops capture and is not sent. Otherwise Esc is the normal key.
- **Mic key while capturing** stops capture. The phrase being spoken is still recognized and typed.
- In space-separated languages, every phrase after the first in a capture session starts with a space. The prompt uses the phrases typed in that session.
- Other keys keep typing while capturing.
- Available only in Server mode. On-device recognition takes about 5 s per phrase, too slow to type as you speak.

## Errors and overlays

Waiting for the recognizer, errors and the discard confirmation appear as overlays over the editor, styled distinctly from entered text. The errors to report are:

- no microphone;
- capture stopped by the system;
- on-device model missing or failed to load;
- speech server not configured, unreachable or timing out;
- API key rejected, or rate limited by a provider;
- server error, or an unusable reply;
- the helper exited.

On a recognizer failure, capture stops, and the failed phrase and every phrase queued after it are dropped, with the reason shown. Text already in the editor stays. In Type as I speak mode errors use the status line.

## Settings

A new **Dictation** card is placed third: Languages and Layouts, General, Dictation, Experimental. It contains:

- **Dictation** dropdown, saved as `dictation`, with three choices:
  - **Off**, the default;
  - **On device**: English and Spanish only;
  - **Server**: the user's own speech server.
- **On device:**
  - The card shows the model's state: not downloaded, downloading, ready or failed.
  - **Delete model** removes the downloaded model.
- **Server:**
  - A **Server** field, saved as `dictation_server`. It holds `host:port`, for example `speech.local:8031` or `192.168.1.20`, and port 8031 is assumed when it is left out. The default avoids 8080, which is common and which Steam on the Frame already uses for its own debugging port. This is all the keyboard asks for; everything else is configured on the server.
  - A state line: not configured, reachable or unreachable.
  - A **Test** button that checks the server.
- **Type as I speak** checkbox, saved as `dictation_direct`, default false.
- **API key** state, shown only when a key file exists or `dictation_url` uses HTTPS: set or missing, with **Delete key** to remove the file. The key itself is never shown.

### Entering the server address

Settings has no text fields, and its cards cover the keys. Selecting the Server field opens an **address popup** over Settings, with its own one-line field and a small hardcoded keyboard. The address is set rarely, so this stays deliberately simple.

- **Keys:** lowercase letters, digits, and `.` `-` `_` `:` `/` `[` `]` `?` (the exact set is a *proposal*). They type only characters that can appear in an address. There is no Space, Shift, Ctrl, Alt, Super, numpad, swipe or Paste; host names ignore case.
- **Editing keys:** Backspace and the arrows. Enter acts on press: it accepts the value as the pending setting, which Apply saves like any other. Esc or the popup's close button cancels.
- **No Paste:** Full Keyboard never reads the clipboard, here or anywhere else.
- **Local only:** the popup types only into its field, never into Steam or the focused application, even with Replace native keyboard on.
- **Size:** smaller than the main keyboard, so it fits inside the panel. The layout is hardcoded and does not follow the active layout or language.
- **Validation:** an invalid value keeps the popup open with a short reason.
- **Recent servers:** up to five recent servers that passed Test are listed under the field (*proposal*).

Fallback: `dictation_server` can also be edited in `config.json`, over SSH or in Desktop Mode, like every other setting.

Full Keyboard never scans the network or discovers servers. It contacts only the address the user typed: Test checks that address, and phrases go to it while capturing.

Mic key visibility: the mic key appears only when dictation is usable and the active language profile has a `dictation` object, holding the Whisper language code and, for Chinese, the script.
- **On device:** the model is ready and the language is English or Spanish.
- **Server:** a server address is set.

The modes never fall back to each other automatically. Audio must not go to a server while the user chose on-device recognition, and on-device recognition cannot cover the server's languages.

Four cards no longer fit the 1564 px settings viewport. Today three 509 px cards with 18 px gaps fill it almost exactly (1563 px), which hides that the row scrolls. Cards become slightly narrower, so three show in full and the start of the fourth is cut off at the right edge, hinting that the row scrolls horizontally. About 470 px gives three full cards and a visible 100 px of Experimental. Labels must still fit beside the controls: at 470 px the label space beside a 318 px dropdown shrinks from about 123 to 84 px, so the dropdown narrows too. The exact widths are settled with rendered settings images. The change ships with the Dictation card; with only three narrower cards the row would end in empty space instead of a cut-off card.

## On-device recognition

A fallback for English and Spanish when no server is available.

- **Model:** Whisper small, about 190 MB at 5-bit quantization. It is downloaded on Apply, like the earlier model-download design: pinned revision, SHA-256 built into the release, verified HTTPS, a temporary file renamed into place, and stored under `~/.local/share/framekeyboard/models/`. `uninstall.sh` removes it.
- **Engine:** whisper.cpp at a pinned commit, built for the Frame's CPU outside Git (`armv8.6-a+dotprod+fp16+i8mm`, no GPU backend), with only its license in `third_party/`.
- **Cost:** in the measurements, small took about 5 s per phrase with 4 threads and kept the CPU about 80% busy while transcribing. The setting therefore describes on-device recognition as slower and heavier than a server.
- **Not measured:**
  - 2 threads;
  - the English-only `small.en` model;
  - pull request #2's window settings with an output cap on the Frame.
- **Kept from the measurements:** the Frame's GPU is never used. It made the dashboard unresponsive.
- **Lifetime:** the model is loaded on the first phrase and unloaded about a minute after dictation mode ends.

## Speech server

### Protocol

The keyboard knows only the server address. Each phrase is one plain HTTP request in the widely implemented OpenAI-style transcription format, so existing self-hosted servers can answer it:

- `POST http://<host>:<port>/v1/audio/transcriptions` as `multipart/form-data`, with these fields:
  - `file`: the phrase as a 16 kHz mono 16-bit WAV;
  - `language`: the Whisper code from the language profile;
  - `prompt`: the context text;
  - `response_format=json` and `temperature=0`.
- The reply is JSON with a `text` field.
- **Path fallback:** if the server answers 404 on `/v1/audio/transcriptions`, the keyboard tries whisper.cpp's default `/inference` once, with the same fields, and remembers which path worked. A `whisper-server` started without `--inference-path` therefore works too.
- **Model name:** sent as `model` only when `dictation_model` is set in `config.json`. Some servers need it, for example Speaches unless a default model is configured; `whisper-server` ignores it.
- **Server-side configuration:** the model, GPU, threads and decoding options belong to the server's own configuration.
- **Timeout:** 10 s per phrase. A connection failure is retried once; that is safe because nothing is typed before the text returns.
- **One request at a time:** a phrase is sent only after the previous one returned and was inserted. App, not the helper, supplies each phrase's prompt from the editor as it is then. Its context then includes the phrase before it, as cleaned, joined or edited. Servers answer in well under half a second, so this costs little.
- **Reply limits:** a reply larger than 64 KB is rejected before it is parsed, and only the `text` field is read.
- **API key:** when a key file exists, it is sent as `Authorization: Bearer <key>`, only over HTTPS, and only to the host of the configured URL. Redirects are never followed. A rejected key (HTTP 401 or 403) and a rate limit (HTTP 429) get their own error messages.

### Hosted providers

Decided 2026-10-25: there are no provider presets for now. The Dictation card offers only the user's own server, and a hosted provider is set up in `config.json`:

- **`dictation_url`:** a full base URL such as `https://api.groq.com/openai/v1`. It takes the place of `dictation_server`, and the keyboard appends `/audio/transcriptions`. Only `http` and `https` are accepted.
- **`dictation_model`:** the provider's model name, for example `whisper-large-v3-turbo` on Groq.
- **Key file:** `~/.config/framekeyboard/dictation-key`, readable only by the user, holding the API key on one line.
  - The user fills it in Desktop Mode, by pasting into a text editor, or over SSH. Full Keyboard never reads the clipboard.
  - The key is never written to `config.json`, which users share in bug reports, and never appears in logs, arguments or the environment. The helper reads the file itself.
  - `uninstall.sh --purge` removes it with the other settings.
- **Providers known to speak the protocol:** Groq, OpenAI and Mistral, each at `/v1/audio/transcriptions` (Groq under `/openai/v1`). Their prices and models change; the setup guide points to them without promising either.
- **Not supported:** providers with their own protocols, such as Deepgram and ElevenLabs.
- **Test:** sends half a second of silence and expects a valid reply. A reply with empty text passes: any valid reply with a `text` field shows the server is reachable and speaks the protocol.

**Why one request per phrase:** Whisper transcribes whole clips anyway, phrase detection is cheap on the Frame, and stateless requests keep any compatible server usable. Audio is about 32 KB per second of speech.

### Reference server

The reference is whisper.cpp's `whisper-server`, at the version used in the measurements (v1.9.5), on the user's machine:

- **Model:** large-v3-turbo, quantized q5_0.
- **Backend:** the machine's GPU.
- **Window:** Whisper's normal 30 s window, never a window sized to the clip.
- **Path:** `--inference-path /v1/audio/transcriptions`.
- **Decoding:** non-speech tokens suppressed (`--suppress-nst`), no timestamps (`--no-timestamps`).
- **No `--convert`:** the keyboard already sends 16 kHz mono WAV. With conversion on, the server writes each upload to a temporary file.
- **Network:** listening on the LAN address on port 8031 (`--host <LAN address> --port 8031`).

Its request fields match the protocol above. The exact command, running it as a background service, and its logging behavior are verified and documented in M1.

### Compatible servers

Any server that implements the OpenAI-style transcription endpoint should work; only `whisper-server` has been measured. Users can choose:

- **whisper.cpp `whisper-server`:** the reference. Small, and runs on Metal, CUDA or Vulkan.
- **Speaches** (formerly faster-whisper-server): Docker, mainly NVIDIA GPUs. It needs a model name unless a default is configured.
- **LocalAI:** a general self-hosted OpenAI-compatible server that can use whisper.cpp.

Servers with their own protocols, such as Home Assistant's Wyoming Faster Whisper, are not supported; a Wyoming client is further work.

### Setup instructions

The repository ships instructions only, no server code; they are in [speech server setup](speech-server.md). They cover:

- installing or building `whisper-server` on Windows, macOS and Linux:
  - **Windows:** NVIDIA (CUDA) and other GPUs (Vulkan).
  - **macOS:** Metal.
  - **Linux:** NVIDIA (CUDA) and other GPUs (Vulkan).
- downloading large-v3-turbo;
- the start command, and running it at login or as a service:
  - **Windows:** a scheduled task or service.
  - **macOS:** a LaunchAgent.
  - **Linux:** a systemd user unit.
- binding it to the LAN address and firewalling it;
- checking it from another machine with `curl` before using it from the keyboard.

### Security and privacy

- **When audio leaves the Frame:** only in Server mode, only to the configured host, and only for a dictation session's phrases or a Test.
- **Plain HTTP on the local network.** Host and port only means no encryption or authentication. Anyone on the local network can reach the server, and audio and text cross the network unencrypted. Therefore:
  - hosts are not restricted, so hosted providers can be used. When the resolved address is not on the local network (RFC 1918, link-local, loopback, IPv6 unique-local or link-local) or Tailscale (100.64.0.0/10, `*.ts.net`, encrypted by WireGuard), the Dictation card shows a red warning naming the host. The warning says the host is not local, and for plain HTTP that the traffic is unencrypted;
  - the setup guide recommends binding the server to the LAN and firewalling it;
  - plain HTTP to a local server is accepted without any notice.
- **HTTPS:** an `https` address is verified against the system's certificates, which covers hosted providers. Self-signed certificates get no special handling; users who want encryption on their own network can put a reverse proxy with a real certificate in front of the server.
- **Hosted providers:** audio goes to that company under its terms. API keys go only over HTTPS to the configured host (see [hosted providers](#hosted-providers)).
- **Other voices:** the microphone picks up nearby speech, such as a video playing on a phone, and dictation transcribes it like the user's own.
- **Logging on the Frame:** no audio and no transcripts. Whether the server logs is up to its configuration; the setup guide points this out.
- **Replies are untrusted input:** they are validated and cleaned as described under [capturing](#capturing), and in dictation mode they only reach the editor until Send.

## Architecture

| Piece | Responsibility | Notes |
| --- | --- | --- |
| `framekeyboard-dictate` helper | Microphone capture through a `pw-record` child (16 kHz mono), speech level, phrase detection, WAV encoding, server requests, reply cleanup | Separate process, so a stuck device or network call cannot block the VR loop. Python standard library only, like the update helper. It runs only during dictation: started on the first mic press, it exits about a minute after dictation mode ends (*proposal*). |
| On-device engine | whisper.cpp with the small model, run by the helper as a child process with a line protocol | Only in On device mode. Low priority; unloads when idle. |
| Phrase detection | Energy-based, with an adaptive noise floor, 0.3 s pre-roll, end after 0.6 s of silence, at most 25 s per phrase | The cheapest detection, in line with keeping Full Keyboard light on CPU, GPU and memory. |
| Helper protocol | Private `SOCK_SEQPACKET` socket pair with bounded packets: level, phrase start and end, text, errors. App answers each finished phrase with its prompt | Transcripts are never logged. A dead helper cannot raise SIGPIPE in the keyboard. |
| Helper lifetime | Exits on socket EOF and kills `pw-record` with it. Captured output only, and not dumpable | The service keeps helpers alive across a handoff (`KillMode=process`), so the helper must notice the keyboard is gone on its own. |
| `DictationEditor` | Text, cursor, line layout, limits, phrase insertion and joining, phrase undo | Pure and host-testable. |
| App integration | Dictation mode layout, key routing to the editor, Esc rules, Send through the output queue, composer commits into the editor | Typed text reaches the editor through `TextOutput`. Rendering stays free of input delivery. |
| Panel | Editor drawing, greyed text, overlays, counter, mic bars, Settings text fields | The bars redraw only the mic key's damaged region. If the full texture upload per frame is too costly with a game running, the bars move to a separate small overlay like the passthrough cursors. |
| VR | Passthrough while capturing, stick scrolling in both modes | |

### Helper protocol

The keyboard starts `dictation/dictation_helper.py` with `python3 -I` and a private `SOCK_SEQPACKET` socket on descriptor 3. Standard input, output and error go to `/dev/null`. Each packet is one JSON object of at most 16 KB. The helper never prints and has no other channel.

Keyboard to helper:

| Packet | Meaning |
| --- | --- |
| `{"op":"configure","server":"host:port"}` or `{"op":"configure","url":"https://…/v1"}`, with optional `"model"` and `"language"` | Recognition target for the following session. `server` defaults to port 8031 and path `/v1`, and may fall back to `/inference`; `url` is used as given. |
| `{"op":"capture","on":true}` / `{"op":"capture","on":false}` | Start or resume capture, or pause it. Pausing ends the phrase being spoken, which is still recognized. |
| `{"op":"prompt","id":N,"text":"…"}` | The context for phrase N, at most 200 code points, sent in answer to `ready`. |
| `{"op":"cancel"}` | Stop capture and drop every pending phrase. The helper answers `cancelled`. |
| `{"op":"test"}` | Send half a second of silence to the configured target. |
| `{"op":"alive"}` | Sent at least every 0.5 s while the keyboard runs. Without one for 2 s, the helper stops capture and drops pending phrases. |

Helper to keyboard:

| Packet | Meaning |
| --- | --- |
| `{"op":"level","value":0…255}` | Speech level for the mic bars, at most 15 times a second while capturing. |
| `{"op":"phrase","id":N}` | A phrase started. |
| `{"op":"ready","id":N}` | Phrase N ended and is next to be recognized; the keyboard answers with `prompt`. Sent only when no request is in flight and the previous text was delivered. |
| `{"op":"text","id":N,"text":"…"}` | Cleaned text for phrase N. |
| `{"op":"dropped","id":N,"reason":R}` | Phrase N produced nothing usable: `empty`, `invalid` or `too-long`. |
| `{"op":"capture","on":false,"reason":R}` | Capture stopped by itself: `silence` after 60 s without speech, or an error code. |
| `{"op":"error","reason":R}` | A fixed error code. Capture stops and pending phrases are dropped. |
| `{"op":"cancelled"}` | The answer to `cancel`. The keyboard ignores every capture packet sent before it, such as a phrase that started just before the cancel arrived. |
| `{"op":"test","ok":true}` / `{"op":"test","ok":false,"reason":R}` | The Test result. |

Error codes: `no-microphone`, `capture-stopped`, `not-configured`, `unreachable`, `timeout`, `server-error`, `bad-reply`, `key-rejected`, `rate-limited`, `backlog-full`, `keyboard-stalled`, `invalid-command`.

- **Timing:** phrase detection counts samples, not wall-clock time, so a test recorder can stream a recording at full speed. Only `alive` and the request timeout use the clock.
- **Backlog:** at most 8 phrases and 120 s of audio wait for recognition. Beyond that, the helper reports `backlog-full`.
- **Shutdown:** the helper exits on socket end-of-file, `SIGTERM` or a stall, and stops `pw-record` first. If the helper is killed, `pw-record` dies on its next write to the closed pipe.

## Milestones

Revised on 2026-10-25 after a second review of this plan. Every milestone keeps its acceptance conditions; a checkbox is marked only once they pass.

- [x] **M0 Feasibility and engine decision:**
  - Whisper measured on the Frame and on two self-hosted machines.
  - Server recognition chosen as the main path.
  - On-device recognition limited to small, English and Spanish.
- [x] **Groundwork (2026-10-25):** fixes outside dictation that it depends on.
  - All of App's typed text goes through one `TextOutput`, so the editor can take the target's place.
  - A retry after a per-character timeout types only the unacknowledged rest, instead of retyping delivered characters.
  - Shift+Enter starts a new line in multi-line Steam fields, such as the friends chat message box, and Enter still sends.
- [ ] **M1 Reference server and microphone:** server part done 2026-10-25; microphone checks partly done. Results are in [M1 results](#m1-results-2026-10-25).
  - Done: `whisper-server` on a Linux desktop with a GPU. The fields, the `/inference` fallback and the ignored `model` field are verified.
  - Done: what the server logs (no transcripts) and that uploads stay in memory without `--convert`.
  - Done: the [setup guide](speech-server.md) for Windows, macOS and Linux. Only Linux is verified; the macOS and Windows steps are untested.
  - Done: end-to-end time from the Frame for 3 s and 10 s phrases over Wi-Fi.
  - **Microphone on the Frame:**
    - Done: `pw-record` at 16 kHz mono 16-bit records the headset microphone in raw samples. It links to the default source by name.
    - Done in part: taking the headset off silences the microphone. Recording while a game plays sound is not tested.
    - Done in part: level samples with steady background noise, which found the noise gate. Not tested: whether the gate clips the start of a word, breathing, quiet speech, short answers and speech running past 25 s. These need a quiet room with no other speakers, and fit the live tests in M3c.
- [ ] **M2 Helper, server path:** built and tested 2026-10-25 (see [M2 results](#m2-results-2026-10-25)). Still open: the update handoff waiting for the helper, which comes with App integration in M3c, and the cost of `pw-record` itself, which needs the real microphone. Capture, level, phrase detection, requests and the [protocol](#helper-protocol), plus:
  - **Lifetime:**
    - The helper exits when the keyboard's socket closes or the keyboard stops making progress, and takes `pw-record` with it.
    - A blocked request can be cancelled.
    - An update handoff starts only after the helper and recorder have gone.
  - **Privacy:**
    - The helper inherits no output, and errors are fixed codes.
    - No text or audio goes into arguments, the environment or files.
    - The helper process cannot dump core.
    - Tests check that marker phrases never show up in output.
  - **Backlog:** queued audio is bounded by bytes, duration and age. Overflow stops capture visibly.
  - **Cost:** measure the helper's CPU and memory on the Frame while capturing and while idle in dictation mode. Full Keyboard should stay as light as possible.
  - **HTTPS and keys:** `dictation_url`, HTTPS verified against the system certificates, and the key file sent only over HTTPS to that host, with no redirects.
  - Host tests with WAV fixtures, a fake `pw-record` and a local fake server, including an HTTPS server with a test certificate authority; no microphone needed.
- [ ] **M3a Editor and key routing, English:** built and host-tested 2026-10-25 (see [M3a results](#m3a-results-2026-10-25)); not yet tried in the headset.
  - `DictationEditor` and its tests.
  - The dictation mode layout, with editor keys handled in App before `KeyboardState`. Typed text goes through `TextOutput`.
  - Minimal config parsing and the English profile's `dictation` entry, so dictation can be configured in `config.json` before M4.
  - Profiles with a composer hide the mic key until M5a.
  - Swipe and the emoji key are unavailable in dictation mode.
  - A `--render-dictation` image export for reviewing the look.
- [ ] **M3b Send:** built and host-tested 2026-10-25, and the A/B test run and the pace made a setting (see [M3b results](#m3b-results-2026-10-25)); the application checks are open.
  - **A/B test of plain-text delivery:** whole commits of up to 32 characters against one acknowledged character at a time, 20 ms apart. The swipe fix that introduced the second way was measured only on replacements.
    - Test the dedicated receiver first, then an X11 browser under load.
    - Count scrambled and reordered text, and time a 900-character text.
    - The default is whichever is reliable.
  - Progress while sending. On failure, only the unsent rest stays in the editor, and the final Enter is tracked separately, so a retry cannot send twice.
  - Line breaks: Shift+Enter in multi-line Steam fields, and single spaces in single-line Steam fields.
  - Delivery verified with the dedicated receiver before any application.
- [ ] **M3c Live dictation:** first part built and host-tested 2026-10-25 (see [M3c results](#m3c-results-2026-10-25)); the dashboard and target rules, stick scrolling and the live checks are open.
  - The helper connected to the editor: phrases, overlays, Esc rules, phrase undo and the mic bars, whose cost is measured with a game running.
  - **Presentation and lifecycle:**
    - A table of what capture, pending phrases and the editor do on dashboard close, X, target loss, native fallback, tracking loss, suspend, Quit and update.
    - Automatic passthrough is tracked apart from the user's own toggle.
- [ ] **M4 Settings:**
  - The Dictation card with the mode dropdown.
  - Narrower cards with the cut-off fourth card.
  - The address popup with its mini keyboard, recent servers, Test, the non-local host warning, the API key state with Delete key, and the error overlays.
- [ ] **M5a Languages:** Spanish, Japanese and Chinese acceptance, script prompts, and composer commits into the editor.
- [ ] **M5b Type as I speak:** its own acceptance for a target change mid-session, a full output queue and uncertain delivery.
- [ ] **M6 On-device mode:**
  - whisper.cpp build and small model download and delete.
  - English and Spanish only.
  - Measure threads, the English-only model and the effect on a running game; acceptance limits for latency and CPU load agreed with the owner first.

## Measurements (2026-10-24)

The same benchmark, built from whisper.cpp v1.9.5, ran on three machines:

- **Clips:** 80 fixed FLEURS test clips (CC-BY-4.0), 20 each in English, Spanish, Japanese and Chinese, 5–15 s long.
- **Decoding:** greedy with temperature fallback, without timestamps, non-speech tokens suppressed, the language set, and a simplified-Chinese prompt for Chinese.
- **Scoring:**
  - English and Spanish by word error rate, Japanese and Chinese by character error rate.
  - Punctuation removed first, and Chinese converted to simplified script.

The benchmark tools and raw results are kept outside the repository.

The machines:
- **Steam Frame:** CPU builds targeted `armv8.6-a+dotprod+fp16+i8mm` and ran 4 threads at `nice 10` with SteamVR running. The GPU build used Vulkan.
- **Server A:** a high-end desktop computer, measured on its GPU and on its CPU with 8 threads.
- **Server B:** a desktop computer with a high-end discrete GPU, measured on the GPU and on its CPU with 8 threads. Normal desktop use continued during the run.

### Steam Frame

| Model (quantized) | Setup | EN | ES | JA | ZH | 10 s clip | Slowest clip | RAM |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| tiny q5_1 | CPU, 30 s window | 10.4% | 21.0% | 45.7% | 34.3% | 0.7 s | 1.2 s | 129 MiB |
| base q5_1 | CPU, 30 s window | 8.2% | 11.9% | 26.7% | 22.0% | 1.5 s | 1.6 s | 162 MiB |
| small q5_1 | CPU, 30 s window | 5.4% | 5.4% | 12.4% | 14.7% | 5.4 s | 5.7 s | 357 MiB |
| small q5_1 | CPU, window sized to the clip | 6.3% | 6.9% | 18.7% | 16.8% | 2.0 s | 18.0 s | 440 MiB |
| medium q5_0 | CPU, 30 s window | 3.4% | 3.5% | 6.2% | 10.3% | 15.4 s | 16.5 s | 831 MiB |
| medium q5_0 | CPU, window sized to the clip | 22.4% | 21.0% | 9.2% | 26.3% | 5.5 s | 47.0 s | 1086 MiB |
| medium q5_0 | GPU, 30 s window, English only | 3.2% | – | – | – | 6.9 s | 7.5 s | 84 MiB + GPU |
| large-v3-turbo q5_0 | CPU, window sized to the clip | 46.7% | 20.4% | 25.5% | 90.8% | 9.0 s | 29.4 s | 753 MiB |

Findings:

- **CPU:** the normal 30 s window makes latency nearly fixed, at over 5 s for small and over 15 s for medium. Four threads kept the CPU about 80% busy.
- **GPU:** the dashboard became barely responsive, the SoC got hot and the device drew about 17 W. ggml's Vulkan scheduler also aborted on a short clip.
- **Sized windows break decoding.** A window sized to the clip (pull request #2's speed trick) made every model repeat sentences on some clips and turned turbo into mixed-language gibberish.
- **Bad output.** Phrases cut mid-word looped too. Whisper also emitted invalid UTF-8 (tiny, Japanese), which is why replies are validated.

### Servers

| Model (quantized) | Machine and setup | EN | ES | JA | ZH | 3 s phrase | 10 s clip | Slowest clip |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| medium q5_0 | A, GPU, 30 s window | 3.4% | 3.5% | 6.2% | 10.3% | 0.19 s | 0.29 s | 0.5 s |
| medium q5_0 | B, GPU, 30 s window | 3.4% | 3.5% | 6.2% | 10.3% | 0.12 s | 0.23 s | 0.5 s |
| medium q5_0 | A, CPU, 8 threads, 30 s window | 3.4% | 3.5% | 6.2% | 10.3% | 0.84 s | 0.95 s | 1.1 s |
| medium q5_0 | B, CPU, 8 threads, 30 s window | 3.4% | 3.5% | 6.2% | 10.3% | 3.88 s | 4.78 s | 6.1 s |
| large-v3-turbo q5_0 | A, GPU, 30 s window | 3.4% | 2.6% | 3.5% | 9.9% | 0.24 s | 0.28 s | 0.3 s |
| large-v3-turbo q5_0 | B, GPU, 30 s window | 3.4% | 2.6% | 3.3% | 9.8% | 0.09 s | 0.12 s | 0.2 s |
| large-v3 q5_0 | A, GPU, 30 s window | 2.9% | 2.6% | 2.8% | 9.6% | 0.32 s | 0.47 s | 0.7 s |
| large-v3 q5_0 | B, GPU, 30 s window | 3.4% | 2.6% | 2.8% | 9.5% | 0.15 s | 0.30 s | 0.5 s |
| medium q5_0 | A, GPU, PR 2 window + output cap | 3.9% | 9.3% | 7.3% | 11.4% | 0.09 s | 0.20 s | 0.4 s |
| large-v3-turbo q5_0 | A, GPU, PR 2 window + output cap | 54.9% | 14.8% | 41.7% | 98.6% | 0.07 s | 0.15 s | 1.2 s |
| large-v3-turbo q5_0 | B, GPU, PR 2 window + output cap | 42.4% | 12.6% | 40.8% | 111.4% | 0.06 s | 0.10 s | 2.5 s |

### Comparison

| Machine | Medium, 10 s clip | Medium, 3 s phrase | Turbo, 10 s clip | Turbo JA / ZH | Large-v3, 10 s clip |
| --- | --- | --- | --- | --- | --- |
| Steam Frame, CPU | 15.4 s | – | not run | – | not run |
| Steam Frame, GPU | 4.8–7.5 s, EN only | – | not run | – | not run |
| Server A, GPU | 0.29 s | 0.19 s | 0.28 s | 3.5% / 9.9% | 0.47 s |
| Server A, CPU | 0.95 s | 0.84 s | – | – | – |
| Server B, GPU | 0.23 s | 0.12 s | 0.12 s | 3.3% / 9.8% | 0.30 s |
| Server B, CPU | 4.78 s | 3.88 s | – | – | – |

Findings:

- **Accuracy is the same everywhere; only speed differs.** Medium uses the same model and settings on every machine. Its text was identical on the Frame and Server A, and Server B differed on one Japanese clip with the same error rate.
- **Both servers' GPUs are fast enough.** With turbo, either answers a phrase in well under half a second. With medium they are 50–70 times faster than the Frame's CPU.
- **Turbo is the right default.** It is close to large-v3's accuracy at medium's speed or better.
- **Use a GPU.** CPU-only speed varies widely between machines (about 1 s and 4.8 s for medium).
- **First request is slower.** On Server B, the GPU backend compiled its shaders on the first request of a new process (7.7 s). Later processes took about 0.4 s from the driver's cache, so a long-running server pays this once.
- **Keep the full window.** Pull request #2's window settings with an output cap still degraded every model.
- **Chinese stays around 10%** for every model. This may partly be how the reference text writes numbers and wording; it was not investigated.
- **Network time from the Frame:** measured in [M1](#m1-results-2026-10-25).

## M1 results (2026-10-25)

### Server

`whisper-server` v1.9.5 was built with Vulkan on a Linux desktop with a high-end discrete GPU. It ran large-v3-turbo `q5_0` with the [reference options](speech-server.md#server-options) on port 8031.

- **Fields:** `language`, `prompt`, `temperature` and `response_format` are honored, and `model` is ignored.
  - The reply is `{"text":" …\n"}`, with a leading space and a trailing newline for cleanup to trim.
  - A request without a file gets HTTP 400 with the plain text `Invalid request`, not JSON.
- **Path fallback:** a server started with the default path answers 404 on `/v1/audio/transcriptions` and transcribes on `/inference`.
- **Logging and storage:**
  - Without `--print-realtime`, the server prints no transcripts; it logs each upload's file name, length and language. The keyboard therefore always sends the fixed name `phrase.wav`.
  - Without `--convert`, uploads are decoded in memory.
- **Silence:** half a second of silence came back as " you", Whisper's usual invention on empty audio. This confirms that silence is never sent; the Test button still passes, because any valid reply does.
- **First request:** 5 s, while the GPU backend prepared itself. Later requests are fast, so a long-running server pays this once.
- **systemd:** the same command ran as a transient systemd user unit, and the Frame reached it.

Round trips from the Frame over Wi-Fi, 10 requests each, the server warm:

| Phrase | Total time | Median |
| --- | --- | --- |
| 3 s (96 KB) | 0.13–0.16 s | 0.14 s |
| 10 s (320 KB) | 0.16–0.21 s | 0.19 s |

Ping was about 8 ms. The network adds 30–50 ms to recognition, mostly for the upload.

### Microphone

- **Source:**
  - The default source is a loopback, `alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source`, in front of the ALSA microphone.
  - `pw-record --raw --rate 16000 --channels 1 --format s16` links to its two channels and delivers raw samples, about 32 KB a second.
  - The ALSA source itself and Steam's echo-cancel and noise-suppression sources gave no data to a second client, so the helper records the default source.
- **Only while worn:** the Frame runs its microphone only while the headset is worn. Off the head, the microphone node stays suspended, and the loopback delivers exact zeros at the normal rate, not an error.
- **Noise gate:** the default source has a noise gate.
  - About 1 s after the input drops below its threshold, it outputs exact zeros.
  - Steady background noise (a dishwasher) was below the threshold. It came through only while speech held the gate open, at a median of about −33 dBFS.
  - The gate stayed open through pauses of about 1.5 s between words.
  - Speech peaked at about −20 dBFS and averaged −37 to −47 dBFS over half a second.
- **Other speakers:** the microphone picks up nearby speech. A phone playing a video across the room held the gate open and came back word for word, so dictation transcribes any voice it hears.

What this means for the design:

- **Phrase detection:** exact zeros mean certain silence, but the gate's hold is longer than the 0.6 s end-of-phrase pause. Detecting the end of a phrase therefore still needs the adaptive noise floor, measured while the gate is open and ignoring zero frames.
- **Word starts:** the 0.3 s pre-roll cannot restore audio the gate muted. Whether the gate clips the start of a word is the main open microphone question; it is tested by transcribing known sentences in M3c.
- **Headset off:** a microphone stopped by taking the headset off looks like silence, so the 60 s silence stop ends such a session. Reading the microphone node's state to report it sooner is a *proposal* for M2.

## M2 results (2026-10-25)

- **Helper:** `dictation/dictation_helper.py` follows the [protocol](#helper-protocol). It uses only the standard library and runs on Python 3.12, the Frame's version, and later.
  - **Phrase detection:** the floor is the quietest open-gate frame of the last 2 s. Until it has 0.5 s of open-gate audio, the default floor is used, and each phrase is re-checked against the floor when it ends, so steady noise at the start of a session is dropped rather than sent.
  - **Requests:** one worker thread sends them, so capture never waits for the network, and the main loop can abort a request.
- **Keyboard client:** `Dictation` (`src/dictation.cpp`) spawns the helper with output to `/dev/null` and keeps only its own end of the socket nonblocking. It sends `alive` every 0.25 s. Text must be valid UTF-8 of at most 8 KB; a reason outside the fixed list becomes `bad-reply`; and a helper that exits or sends an oversized packet becomes `helper-exited`. App does not use it yet; that is M3c.
- **Host tests:**
  - **Helper (34 tests):** phrase detection on synthetic audio (gate zeros, background noise, clicks, long speech, 60 s of silence, levels), text cleanup and targets.
  - **Requests:** fields, the fallback, status codes, oversized and invalid replies, redirects, timeouts, and HTTPS with a throwaway certificate authority, including the key going only to HTTPS.
  - **The helper process with a fake recorder:** one request at a time with the keyboard's prompt, pause, cancel, recorder loss, the backlog, a stalled keyboard, end of file, and a killed helper leaving no recorder. Nothing is printed, and core dumps are off.
  - **Client (C++):** these tests also run against the real helper.
- **On the Frame:** both suites pass, under Python 3.12 with SteamVR running.
- **Cost on the Frame,** with synthetic audio from a fake recorder at real-time pace and SteamVR running:

| Case | Helper CPU | Memory |
| --- | --- | --- |
| Running, not capturing | 0.00% of one core | 21.0 MiB |
| Capturing gate zeros | 0.60% | 21.1 MiB |
| Capturing continuous speech over noise | 0.79% | 21.9 MiB |

  Phrase detection worked in real time: the phrase started at 0.1 s and split at exactly 25.0 s. `pw-record`'s own cost is not measured yet, because measuring it needs the real microphone; it belongs with the M3c live tests.

## M3a results (2026-10-25)

- **Editor:** `DictationEditor` (`src/dictation_editor.cpp`) holds UTF-8 text with a byte cursor on character boundaries; limits count code points.
  - **Typing:** stops at 1000 code points; dictation stops at 900. Tabs and control characters are dropped.
  - **Phrases:** inserted with the [joining](#capturing) rules, and undone in reverse order. An edit inside a phrase makes it no longer undoable; edits before it only move it.
  - **Context:** the last 200 code points before the cursor become the prompt.
- **Layout:** `dictation_layout()` (`src/dictation_mode.cpp`) derives the mode's keys from the active layout, as the emoji picker does.
  - The editor fills the function-key row from Esc to the visible right edge, rising 60 units into the gap under the toolbar and dropping 12 toward the number row, so four lines of 19 px text fit.
  - The mic key is a new `dictation` action at the free side slot in every bundled layout, hidden until dictation is usable.
  - Send spans the right-hand Alt to Ctrl keys, and Undo takes Paste's slot.
- **Look:** the editor is a well recessed into the case, the opposite of a keycap. It is darker than the case, shaded under its top edge, with a rim taken from the keys' colour and a faint light catch on its lower lip; every colour comes from the theme. The text is slightly off-white, the scroll indicator is a quiet pill, and the counter sits grey in the toolbar row, turning amber at 900.
- **Routing:** in dictation mode, every key goes to the editor before swipe, the composers or the output queue.
  - Keys still show as pressed and latch through `KeyboardState` as local keys.
  - Text reaches the editor through `TextOutput`.
  - Backspace, Delete, Enter, the arrows, Home, End and the numpad's navigation keys edit or move. Up and Down keep their column; Home and End follow the wrapped line.
  - PgUp and PgDn scroll by a page, and a laser click places the cursor.
  - Held keys repeat after 0.5 s, every 40 ms, without catching up.
  - Esc leaves the mode and keeps the text; the discard confirmation comes with capture in M3c.
  - Send stays dimmed until M3b.
- **Config:** `config.json` fields as in [configuration](configuration.md#dictation). An invalid dictation field turns only dictation off. The English profiles carry `"dictation": {"language": "en"}`. The mode closes if the profile or settings stop allowing it.
- **Tests:**
  - `dictation-editor`: editing, limits, joining, undo and context.
  - `dictation-mode`: settings, layout, the mic key's visibility, typing, Shift, Backspace, Enter, arrows, Home, Delete, Down and End, repeat, a click placing the cursor, Esc, reopening with the text kept, and a profile change closing the mode. Nothing reaches the sink.
  - Host CTest passes with 30 suites and 3 optional CJK skips, and the ARM64 build is clean.
- **Not yet:** stick scrolling of the editor and the discard confirmation (M3c), Send (M3b), and a check in the headset.

## M3b results (2026-10-25)

- **Pieces:** `send_pieces()` (`src/dictation_send.cpp`) cuts the editor text into commits of at most 32 code points, Shift+Enter breaks or single-line spaces, and the final Enter. Each piece records the editor bytes it covers.
- **Sending:** App sends one piece at a time through `OutputController`, starting once no key is held and earlier output has drained. The queue's new `completed()` counter, with `failures()`, tells a delivered piece from a failed or cancelled one. Breaks and Enter go through the queue's new `tap()`, which presses a chord and releases it in reverse.
- **Target:** `KeySink::single_line_target()` is true for a Steam field without `multiline`; every other target takes Shift+Enter.
- **Failure:** the remainder stays, as described in [Send](#send). A text that went one character at a time keeps only the unacknowledged rest; the queue's own retry is dropped, so the main keyboard's Enter cannot retype it.
- **Tests:** `dictation-send` covers pieces, chunking, trimming and both line-break rules. `dictation-mode` covers sending with Shift+Enter, a single-line field, waiting for a held key, the lock, Esc, a per-character failure keeping only the rest, and an Enter failure retried alone. Host CTest passes with 34 suites and 3 optional CJK skips, and the ARM64 build is clean.
- **Delivery A/B (Frame, headset worn):** a disposable Brave receiver, driven by `text-replace-probe` with its new `whole` and `paced` commands, received the same 900-character English text. The load was 12 busy loops on the 8 cores, plus a page that stays busy for 10 ms on every key.

  | Setup | Exact runs | Wrong characters | Time for 900 |
  | --- | --- | --- | --- |
  | Whole 32-character commits, idle | 0 of 2 | 667 and 887 | 1.3 s |
  | Whole commits, load | 0 of 2 | 568 and 1111 | 1.2 s |
  | One character at a time, 20 ms, idle | 2 of 2 | 0 | 18 s |
  | One character at a time, 20 ms, load | 4 of 7 | 4 swapped neighbours in 6300 | 19 s |
  | One character at a time, 40 ms, load | 5 of 5 | 0 | 37.5 s |

  - Whole commits are unusable for plain text: Gamescope's temporary keymap leaked into the text (`zxcvbnm,./1234567890-=qwerty`), and words were lost or reordered even with the Frame idle. Send keeps one character at a time.
  - The gap trades speed for order under heavy load. A 30 ms run was cut short twice when the receiver lost focus, and nothing was sent after that.
  - **Decision (owner):** a **Send pace** setting with 40 ms (the default), 30 ms and 20 ms, so real use can decide. It is saved as `dictation_send_gap_ms` and sits in the Experimental card until M4's Dictation card. Send sets the gap while it types (`KeySink::set_character_gap`, down to `GamescopeText`) and restores 20 ms afterwards, so swiped words keep their pace.
- **Not yet:** delivery into applications and a check in the headset, including which pace holds up in real use.

## M3c results (2026-10-25)

First part: the helper connected to the editor.

- **Mic key:** from the normal keyboard it opens dictation mode and starts listening; in the mode it pauses and resumes. While listening the key takes the theme's latched colour and shows five level bars, flat in silence. Its level is cut to nine steps, and only the key repaints when it changes.
- **Listening:** the editor's text dims, the editor is read-only, and Settings and the language button wait. Undo, Send, Esc and X keep working.
- **Phrases:** App answers each `ready` with the 200 code points before the cursor and inserts the text at the cursor with the joining rules. A phrase waiting over a second for its text shows "Waiting for recognition…" in a band over the editor's foot. At 900 characters listening pauses.
- **Esc:** listening, it stops and drops the phrases not yet inserted; with an empty editor it leaves; with text it asks "Discard text?" with Discard and Keep, and Esc again discards. Other keys wait while it asks.
- **X:** discards everything without asking.
- **Send while listening:** listening pauses, Send waits for the pending phrases, then sends; Esc ends the wait, and after 20 s it gives up with a notice.
- **Errors:** a helper error stops listening and shows its reason in amber in the band; the text already inserted stays.
- **Cancel:** the helper now answers `cancel` with `cancelled`, and the client discards everything sent before it, so a phrase that started just before a cancel cannot linger as pending.
- **Hiding:** until the dashboard rules exist, hiding the keyboard pauses listening; phrases already spoken are still recognized.
- **Tests:** `dictation-mode` drives the keyboard against a scripted helper: opening and listening, the read-only editor, pausing with a phrase inserted, resuming, Send while listening, Esc in each state with Keep and Discard, X, and a server error. Every App in the suite uses a stand-in helper; one early run started the real helper from the build directory against 127.0.0.1 with nothing listening, which is why. Host CTest passes with 34 suites and 3 optional CJK skips; ARM64 builds clean.
- **Not yet:** automatic passthrough and the dashboard rules, the target-lost rule in replacement mode, stick scrolling, the waveform over the editor, the update handoff waiting for the helper, measuring the cost of the bars and of `pw-record`, and the live checks.

## Open questions

The owner settled the behavior questions and the proposals on 2026-10-25; the rules are in the sections above. Still open:

- **Reply length cap (M2):** measure false rejections per language before fixing 25 characters a second.
- **Mic bars:** the cost of redrawing them with a game running (measured in M3c).
- **Proposals:** the remaining items marked *proposal* above.

## Further work

- Swipe typing into the editor. It needs a suggestion row, and dictation mode has no room for one yet.
- Text selection in the editor.
- Live partial text while speaking, which needs a streaming server protocol.
- A Wyoming protocol client, for Home Assistant's speech servers.
