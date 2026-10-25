# Dictation plan

Status: designed, not started, and not part of the 0.9.0 release. The main path sends speech to a speech server the user hosts; a limited on-device mode covers English and Spanish. The measurements behind this are in [measurements](#measurements-2026-10-24). Items marked *proposal* are Claude's suggestions that the owner has not confirmed yet. Mark milestone checkboxes only after their acceptance conditions pass.

Dictation turns speech into text. The user speaks into a local editor on the keyboard, corrects the text there, and sends it to the focused application in one go. The Frame records audio, splits it into phrases, edits text and types it. Each phrase is turned into text either by a speech server elsewhere on the network, or by a small model on the Frame.

## Decisions

- **Two recognition modes, chosen in Settings:**
  - **Server:** a speech server the user hosts on a machine with a capable GPU. It gives the best quality and speed and covers all dictation languages.
  - **On device:** Whisper small on the Frame's CPU, for English and Spanish only. It is a fallback: in the measurements it took about 5 s per phrase and loaded the CPU heavily, and larger models were unusable on the Frame alongside VR.
- **The keyboard asks only for the server's host and port.** Model choice and every other recognition setting live in the server's own configuration.
- **No server in this repository.** This repository does not ship or vendor a server. The documentation describes a reference setup with an existing open-source server.
- **Reference engine:** whisper.cpp's `whisper-server` with the large-v3-turbo model on a GPU, with Whisper's normal 30 s window. On the measured servers it transcribed a 3 s phrase in 0.09–0.24 s, with Japanese at about 3.4% and English at 3.4% errors.
- **Languages:**
  - English, Spanish, Japanese and Chinese first. One multilingual Whisper model covers all of them on a server.
  - On device: English and Spanish only.
  - A language profile without dictation support hides the mic key.
- **Off by default.** Audio leaves the Frame only in Server mode, only for the configured host and only while capturing.
- **Settings:** a Dictation card of its own, third, before Experimental.
- **Privacy:** on the Frame, audio and editor text stay in memory only. Nothing is logged or written to disk, and the system clipboard is never read or written.
- **Release:** not in 0.9.0.
- **Credit:** the helper process, `pw-record` capture, transcript cleanup and repetition guard follow the dictation pull request by amirhh00 and adepssimius (`TaiKeid/steam-frame-full-keyboard#2`). That pull request vendored whisper.cpp and ggml into the repository and ran them on the Frame, and is not merged; its ideas are reimplemented on the current code.

## Keyboard design

### Mic key

![Keyboard with the mic key in the side column (owner mockup)](images/dictation-mic-key.png)

The mic key sits in the side column below Copy and Paste, which become shorter to make room. It is shown only when dictation is usable in the chosen mode for the active language (see [Settings](#settings)); otherwise the side column is unchanged.

- **Normal mode:** pressing the mic key enters dictation mode and starts capturing at once.
- **Type as I speak:** pressing it starts capture without leaving the normal keyboard.

### Dictation mode layout

![Dictation mode with the editor above the keys (owner mockup, incomplete)](images/dictation-mode.png)

The mockup is incomplete; where it differs, the rules below apply.

- An editor replaces the function-key row. Esc stays at its left.
- **Hidden:** F1–F12, Print Screen, Scroll Lock, Pause, Tab, Insert (*proposal*), both Ctrl keys, Super, both Alt keys, Steam and Menu, and the Copy and Paste side keys.
- **Send** takes the place of the right-hand Alt to Ctrl keys.
- **Undo last phrase** takes the first freed side slot, directly above the mic key.
- Letters, digits, punctuation, Shift, Caps Lock, Backspace, Delete, Enter, the arrows, Home/End and the number pad stay and edit the editor. PgUp/PgDn scroll it (*proposal*).
- The toolbar stays. Settings and the language button work only while not capturing (*proposal*).

### Capturing

- The mic key starts and stops capture: start, pause, resume, pause. Leaving the mode is done with Send or Esc.
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
  - When the inserted phrase continues a sentence, its first letter is lowercased.
  - When text follows the cursor, the phrase's final period is dropped.
- **Order:** resuming while a phrase is still pending queues the new audio, and phrases are inserted in order.
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
  - Those at the start and end are dropped.
  - Those between paragraphs are kept and sent as Shift+Enter, so chat applications insert a line break instead of sending early.
  - Only the final Enter is a plain Enter.
- **Delivery:**
  - Text is cut into commits of at most 32 code points through the existing output queue. Each line break is a native key entry, so text with many lines goes out in several batches as the queue drains.
  - Sending waits until no key is held, so a held modifier cannot turn letters into shortcuts.
  - The editor is locked and shows progress while sending.
  - If delivery fails or is interrupted, the unsent remainder stays in the editor and the mode stays open, as failed Japanese commits stay in the preedit today.
- **Send while capturing:** capture stops, Send waits for pending phrases, then sends (*proposal*).
- **Target:** Send types into whatever has keyboard focus at that moment. Switching windows mid-dictation is the user's choice.

### Dashboard, passthrough and focus

- **Dashboard closed while capturing:** passthrough is turned on automatically, so the keyboard stays open and capture continues, even if Steam asks to close the keyboard. That passthrough stays on until dictation mode ends.
- **Dashboard closed while not capturing:** the keyboard hides as usual. The dictation state (mode, editor text, cursor, scroll) is kept in memory and restored when the keyboard opens again.
- **Switching windows** is handled the same way as closing and reopening the dashboard.
- **Unchanged:** held keys are still released on hide and target change, and the stock keyboard fallback is unchanged.
- **App exit or restart:** the editor text is lost.

### Type as I speak

A second checkbox, **Type as I speak**, skips the editor. The mic key starts and stops capture on the normal keyboard, and each phrase is typed into the focused application as soon as its text returns.

- No character limit and no counter; the output queue paces delivery.
- Undo last phrase is not offered, because the cursor in the target may have moved.
- Esc while capturing stops capture and is not sent. Otherwise Esc is the normal key.
- In space-separated languages, every phrase after the first in a capture session starts with a space. The prompt uses the phrases typed in that session.
- Other keys keep typing while capturing (*proposal*).

## Errors and overlays

Waiting for the recognizer, errors and the discard confirmation appear as overlays over the editor, styled distinctly from entered text. The errors to report are:

- no microphone;
- capture stopped by the system;
- on-device model missing or failed to load;
- speech server not configured, unreachable or timing out;
- server error, or an unusable reply;
- the helper exited.

On a recognizer failure, capture stops and the failed phrase is discarded; text already in the editor stays (*proposal*). In Type as I speak mode errors use the status line.

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
  - **Host** and **Port** fields, saved as `dictation_host` and `dictation_port`. These are all the keyboard asks for; everything else is configured on the server.
  - A state line: not configured, reachable or unreachable.
  - A **Test** button that checks the server.
- **Type as I speak** checkbox, saved as `dictation_direct`, default false.

Field entry (*proposal*): Settings has no text fields yet, and Settings covers the keys. Selecting Host or Port opens a one-line editor above the keys, as in dictation mode. The keys type into it, Enter confirms and Esc cancels. Port accepts digits only.

Mic key visibility: the mic key appears only when dictation is usable and the active language profile has a `dictation` object, holding the Whisper language code and, for Chinese, the script.
- **On device:** the model is ready and the language is English or Spanish.
- **Server:** a host and port are set.

The modes never fall back to each other automatically (*proposal*). Audio must not go to a server while the user chose on-device recognition, and on-device recognition cannot cover the server's languages.

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

### Protocol (*proposal*)

The keyboard knows only the host and port. Each phrase is one plain HTTP request in the widely implemented OpenAI-style transcription format, so existing self-hosted servers can answer it:

- `POST http://<host>:<port>/v1/audio/transcriptions` as `multipart/form-data`, with these fields:
  - `file`: the phrase as a 16 kHz mono 16-bit WAV;
  - `language`: the Whisper code from the language profile;
  - `prompt`: the context text;
  - `response_format=json` and `temperature=0`.
- The reply is JSON with a `text` field.
- **Server-side configuration:** the model, GPU, threads and decoding options belong to the server's own configuration.
- **Timeout:** 10 s per phrase. A connection failure is retried once; that is safe because nothing is typed before the text returns.
- **Test:** sends half a second of silence and expects a valid reply.

**Why one request per phrase:** Whisper transcribes whole clips anyway, phrase detection is cheap on the Frame, and stateless requests keep any compatible server usable. Audio is about 32 KB per second of speech.

### Reference server

The reference is whisper.cpp's `whisper-server`, at the version used in the measurements (v1.9.5), on the user's machine:

- **Model:** large-v3-turbo, quantized q5_0.
- **Backend:** the machine's GPU.
- **Window:** Whisper's normal 30 s window, never a window sized to the clip.
- **Path:** `--inference-path /v1/audio/transcriptions`.
- **Decoding:** non-speech tokens suppressed (`--suppress-nst`), no timestamps (`--no-timestamps`).
- **Network:** listening on the LAN address (`--host`, `--port`).

Its request fields match the protocol above. The exact command, running it as a background service, and its logging behavior are verified and documented in M1.

### Security and privacy

- **When audio leaves the Frame:** only in Server mode, only while capturing, and only to the configured host.
- **No encryption or authentication.** Host and port only means plain HTTP. Anyone on the local network can reach the server, and audio and text cross the network unencrypted. To limit this:
  - the keyboard accepts only private-network hosts: RFC 1918 and link-local addresses, `.local` names, and loopback (*proposal*);
  - the setup guide recommends binding the server to the LAN and firewalling it;
  - choosing Server mode shows a one-line notice about this (*proposal*).
- **Logging on the Frame:** no audio and no transcripts. Whether the server logs is up to its configuration; the setup guide points this out.
- **Replies are untrusted input:** they are validated and cleaned as described under [capturing](#capturing), and in dictation mode they only reach the editor until Send.

## Architecture

| Piece | Responsibility | Notes |
| --- | --- | --- |
| `framekeyboard-dictate` helper | Microphone capture through a `pw-record` child (16 kHz mono), speech level, phrase detection, WAV encoding, server requests, reply cleanup | Separate process, so a stuck device or network call cannot block the VR loop. Python standard library only, like the update helper (*proposal*). |
| On-device engine | whisper.cpp with the small model, run by the helper as a child process with a line protocol | Only in On device mode. Low priority; unloads when idle (*proposal*). |
| Phrase detection | Energy-based, with an adaptive noise floor, 0.3 s pre-roll, end after 0.6 s of silence, at most 25 s per phrase | Cheap enough for the Frame (*proposal*). |
| Helper protocol | Socket pair with bounded line messages: level, phrase start and end, text, errors | Transcripts are never logged. A dead helper cannot raise SIGPIPE in the keyboard. |
| `DictationEditor` | Text, cursor, line layout, limits, phrase insertion and joining, phrase undo | Pure and host-testable. |
| App integration | Dictation mode layout, key routing to the editor, Esc rules, Send through the output queue, composer commits into the editor | Rendering stays free of input delivery. |
| Panel | Editor drawing, greyed text, overlays, counter, mic bars, Settings text fields | The bars redraw only the mic key's damaged region. If the full texture upload per frame is too costly with a game running, the bars move to a separate small overlay like the passthrough cursors. |
| VR | Passthrough while capturing, stick scrolling in both modes | |

## Milestones

- [x] **M0 Feasibility and engine decision:**
  - Whisper measured on the Frame and on two self-hosted machines.
  - Server recognition chosen as the main path.
  - On-device recognition limited to small, English and Spanish.
- [ ] **M1 Reference server:**
  - Verify `whisper-server` with the protocol above.
  - Document the setup, running it as a service, and its logging.
  - Measure end-to-end time from the Frame for 3 s and 10 s phrases over Wi-Fi.
- [ ] **M2 Helper, server path:** capture, level, phrase detection, requests and the protocol. Host tests with WAV fixtures and a local fake server; no microphone needed.
- [ ] **M3 Dictation mode, English:** editor model and tests, layout, Esc rules, undo, Send through the queue. Delivery verified with the dedicated receiver before any application.
- [ ] **M4 Settings:** Dictation card with the mode dropdown, narrower cards with the cut-off fourth card, Host and Port entry, Test, error overlays.
- [ ] **M5 Languages and direct mode:** Spanish, Japanese and Chinese acceptance, script prompts, composer commits into the editor, Type as I speak.
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
- **Not measured:** network time from the Frame to a server.

## Open questions

- **Which server:** whisper.cpp's `whisper-server` as is, or a small dedicated server of our own. Our own server could offer a health check, discovery, a fixed path and its own configuration file.
- **Discovery:** finding the server on the local network, for example with mDNS, so typing a host is optional.
- **Mic bars:** the cost of redrawing them with a game running.
- **Proposals:** the items marked *proposal* above.

## Further work

- Swipe typing into the editor. It needs a suggestion row, and dictation mode has no room for one yet.
- Text selection in the editor.
- Live partial text while speaking, which needs a streaming server protocol.
