# Dictation

Dictation turns speech into text. The user speaks into a local editor on the keyboard, corrects the text there, and sends it to the focused application in one go; or, with **Type as I speak**, each phrase is typed into the application as soon as it is recognized. The Frame records audio, splits it into phrases, edits text and types it. Each phrase is turned into text either by a speech server the user hosts elsewhere on the network, or by a small model on the Frame. Dictation is not part of the 0.9.0 release.

## Recognition modes

The **Dictation** setting chooses one of three modes:

- **Off.**
- **On device**, the default: multilingual Whisper small on the Frame's CPU. It is best in English and Spanish; other dictation languages work with a low-accuracy warning. A phrase takes about 2 s. The model is downloaded once, after the user confirms it.
- **Self-hosted** (`server`): a speech server the user runs on a computer with a capable GPU. It gives the best quality and speed, covers every dictation language, and answers a phrase in well under half a second. A hosted OpenAI-compatible provider can take its place through `config.json` (see [hosted providers](#hosted-providers)).

The modes never switch on their own. Audio never goes to a server while On device is chosen. In Self-hosted mode, the opt-in **Fall back to on device** switch lets the Frame recognize a phrase the server could not, because it was unreachable or timed out, once the model is downloaded.

### Why on device uses the CPU

- **The GPU belongs to VR.** The headset renders the dashboard, the keyboard and any game on it. Whisper on the Frame's GPU made the dashboard barely responsive, heated the SoC, drew about 17 W, and ggml's Vulkan scheduler aborted on a short clip. A recognizer that can stall or crash rendering is not acceptable in a headset, so the engine is built without a GPU backend.
- **The CPU is fast enough for small.** With the Frame's dot-product and int8 matrix instructions, 8-bit small recognizes a phrase in about 2 s with 5.2% word errors in English and 5.4% in Spanish, at `nice 10`, so VR and games keep priority. Larger models were too slow on the Frame alongside VR.
- **Better quality comes from a self-hosted server,** not a larger model on the headset: a computer with a GPU runs large-v3-turbo in a fraction of a second, with fewer errors in every language.

### What the keyboard configures

- The keyboard asks only for the server's address, as `host:port`. Model choice and every other recognition setting live in the server's own configuration. An optional model name exists only in `config.json`, for servers without a default model.
- Full Keyboard does not ship, build or vendor a speech server. Users run an existing OpenAI-compatible server; the [setup guide](speech-server.md) describes a reference setup with whisper.cpp's `whisper-server`.

## Languages

- Every bundled language profile offers dictation except Japanese JIS, which types through the system's own input method and so cannot receive dictated text. A profile without dictation support hides the mic key.
- One multilingual Whisper model covers every language, on a server or on device.
- On device, English and Spanish are accurate. For other languages, Settings warns that accuracy on the device is low and that a self-hosted server gives better results.
- A profile's `dictation` object holds its Whisper language code and, where needed, a [script prompt](#script-prompts) (see [configuration](configuration.md#dictation)).

## Mic key

The mic key is the third of the three equal side-column keys, below Copy and Paste and the small emoji key above them (see [emoji picker](emoji.md#side-column)). It is shown only when dictation is usable in the chosen mode for the active language (see [mic key visibility](#mic-key-visibility)); otherwise its slot stays empty.

- **Normal mode:** pressing the mic key enters dictation mode and starts capturing at once.
- **Type as I speak:** pressing it starts capture without leaving the normal keyboard.
- **While capturing:** five vertical bars replace the mic icon. They stay in place while the speech level passes through them from right to left, each bar easing to its new height; the left ones are fainter. They are flat during silence, and silence repaints nothing.
- **Start and stop felt in the hand:** when listening starts, the controller that last pressed gives two light rising ticks; when it stops, for any reason, one soft low pulse. Silence or a full editor can end listening while the eyes are on the text.
- **While recognizing:** while a spoken phrase waits for its text, a short bright segment circles just inside the key's border. The phrase still being spoken does not count. On device, recognition takes seconds, and this shows it is under way.
- **Where speech is recognized:** a small icon in the key's top-right corner says where phrases go: a headset for On device, a computer for Self-hosted, a cloud for a hosted provider. It tells before anyone speaks whether audio leaves the headset. In the editor it sits left of the counter. With the fallback, it turns to the headset while phrases come from the device and back on the next server phrase.
- **While the model downloads:** the key stays shown but disabled, with a slim progress track along its foot.

## Dictation mode

### Dictation mode layout

- An editor replaces the function-key row. Esc stays at its left.
- **Hidden:** F1–F12, Print Screen, Scroll Lock, Pause, Tab, Insert, both Ctrl keys, Super, both Alt keys, Steam and Menu, and the Copy and Paste side keys.
- **Send** takes the place of the right-hand Alt to Ctrl keys. Layouts without them, such as flick, put it in Tab's place.
- **Undo last phrase** takes the first freed side slot, directly above the mic key.
- Letters, digits, punctuation, Shift, Caps Lock, Backspace, Delete, Enter, the arrows, Home/End and the number pad stay and edit the editor. PgUp/PgDn scroll it by a page, locally.
- The status line, which the editor covers, moves into the toolbar row, wrapping onto a second line when long.
- The toolbar stays. Opening Settings pauses capture; Settings waits only while Send runs. The language button waits until no phrase is being captured, recognized or typed. X, Pin, Recenter and Esc keep working.
- Swipe typing and the emoji key are unavailable in dictation mode.
- The editor is a well recessed into the case, darker than it on dark themes and lighter on light ones, with every colour taken from the theme.

### Capturing

- The mic key starts and stops capture: start, pause, resume, pause. The mode is left with Send, which commits, or Esc, which discards.
- **Pause:** the phrase being spoken ends at the pause, and its audio is still recognized and inserted. Resuming starts a new phrase.
- **Editor while capturing:** the existing text is greyed out and read-only; the insertion point stays visible.
- **Phrases:**
  - The Frame splits speech at pauses of about 0.6 s, and at 25 s at the latest.
  - Each phrase is sent to the recognizer while capture continues, and is inserted at the cursor as soon as its text returns.
  - It appears word by word, one word every 20 ms, as if typed very fast; Japanese and Chinese, without spaces, appear a character every 10 ms. Typing into the editor waits until it has appeared, and Esc or the mode's keys show the rest at once.
  - Silence is never sent. Whisper invents text for empty audio, such as " you" for half a second of silence.
- **Context:** each phrase includes up to 200 characters of the text before the cursor as Whisper's prompt. This improves casing, punctuation and names, and makes inserting in the middle of the text read naturally.
- **Order:** resuming while a phrase is still pending queues the new audio, and phrases are inserted in order.
- **Late text:** a phrase is inserted at the cursor as it is when its text arrives, even if the cursor moved or the text changed meanwhile.
- **Waiting:** if a phrase takes longer than about 1 s, a band over the editor's foot says it is waiting for recognition.
- **Automatic stop:** after 60 s without speech, when the editor reaches 900 characters, or when capture or recognition fails (see [errors](#errors)).

### Script prompts

A language profile can put a short sentence of up to 60 characters before the context:

- **Chinese:** 好的，没问题。 for simplified and 好的，沒問題。 for traditional. They fix the script, which Whisper otherwise chooses by itself, and keep Chinese punctuation.
- **Japanese:** はい、そうですね。. Without it Whisper often leaves out 、 and 。, and a context without them makes every following phrase leave them out too.
- **English, Spanish and the other languages** have none.

Short conversational sentences are used rather than descriptions such as 以下是普通话的句子。: Whisper repeats descriptive prompts as text on short unclear phrases, while these give fewer invented phrases than no prompt at all. A conversational English prompt made Whisper invent "Thank you." more often, and Spanish prompts did not help, so neither language has one.

### Joining and cleanup

- **Spaces:** in space-separated languages a space is added at either edge where needed; in Japanese and Chinese, none.
- **Casing:** Whisper's casing is kept. The context prompt already tells it when a phrase continues a sentence, and lowercasing would break "I" and names.
- **Final period:** when text follows the cursor, the phrase's final period is dropped.
- **Cleanup of returned text,** which is untrusted input:
  - It must be valid UTF-8, or the phrase is dropped. Whisper can emit invalid UTF-8.
  - Control characters are removed.
  - Non-speech annotations such as `[BLANK_AUDIO]` and `(music)` are removed.
  - Runs of four or more repeated words are collapsed.
  - Text longer than 25 characters per second of audio plus 20 is treated as a failed phrase.

### Editor

- The editor holds plain text with line breaks. Tab characters are not supported. There is no text selection.
- **Cursor:** a laser click places it, using Pango's position lookup. The arrows, Home and End move it; Up and Down keep their column, and Home and End follow the wrapped line. Held keys repeat after 0.5 s, every 40 ms.
- **Scrolling:** the thumbstick scrolls the editor a line a step while the laser points at it. With the dashboard open, the overlay requests SteamVR's scroll events; in passthrough, the stick is read directly, as for the emoji grid. Pressing on the text and dragging scrolls it like a touch screen; a press released where it began places the cursor. Scrolling works while listening too. The view follows the cursor as text is inserted.
- **Counter:** a character counter in the toolbar row shows, for example, `412/900`, turning amber at 900. Characters are counted as Unicode code points, not bytes.
  - Dictation cannot start or continue at 900 characters or more.
  - A phrase still pending when 900 is reached is inserted, up to the hard cap.
  - Manual typing is allowed up to 1000 characters; the counter may then show `1000/900`.
- **Undo last phrase:** removes the most recently inserted phrase. Repeated presses remove earlier phrases. A phrase whose text was edited since is no longer undoable; edits before it only move it.
- **Composers:** Japanese and Chinese composers commit into the editor instead of the application, with the same keys as on the normal keyboard; the toolbar's composer buttons stay.
  - The reading being composed shows in the editor at the cursor, underlined as in a text field, with the segment being converted underlined more strongly.
  - **Convert** moves beside the editor, into the side column's free slot next to Esc, as large as Undo, its label on two lines. It is shown while a reading is composed.
  - Candidates fill the band over the editor's foot while there are any to choose; the text above it stays in view.
  - Any key that edits the editor, the mic key, Send, Undo and a tap in the text commit the reading first. Esc cancels the reading before it acts on the editor. Closing the mode drops a reading not yet committed.
  - Before the mode opens, a reading in progress goes to the application, as before any other key.

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
  - Text is cut into commits of at most 32 code points through the output queue. Each line break is a native key entry. Each piece goes out once the previous one is delivered, so a failure knows exactly what arrived.
  - Plain text to local applications goes one acknowledged character at a time. Whole commits are unusable there: Gamescope's temporary keymap leaks into the text, and words are lost or reordered even with the Frame idle.
  - The **Send** setting sets the pace: 20 ms between characters (the default, about 19 s for 900 characters), or 30 or 40 ms, slower but steadier when the Frame is busy. Under heavy load, 20 ms can occasionally swap neighbouring characters. Other typing keeps 20 ms.
  - **Paste,** the fourth choice of the **Send** setting: Send writes the text to the clipboard, line breaks included, presses Ctrl+V, and waits until the application takes the text before the final Enter.
    - The keyboard owns the clipboard on every X display, since Steam uses `:0` and the applications it starts use `:1`, and Gamescope does not copy one to the other. It answers requests for its own text and never asks for anyone else's. The text stays there until something else is copied or the keyboard exits.
    - Gamescope fetches a new clipboard at once, so Send waits 150 ms before Ctrl+V; Enter follows 100 ms after the application's request.
    - If no application asks for the text within 2 s, the text stays in the editor and Enter is not pressed. An interruption after Ctrl+V marks the whole text as possibly arrived.
    - Steam's own fields reached through native keyboard replacement keep the direct path, which already inserts text at once; the clipboard cannot reach them.
    - Applications that paste with another shortcut, such as terminals, do not take it.
  - Sending waits until no key is held, so a held modifier cannot turn letters into shortcuts.
  - The editor is locked and shows progress while sending: delivered text leaves the editor as it goes, the rest is dimmed without a caret, and the counter shows `Sending 120/412`. Esc stops sending.
- **Failure:** if delivery fails or is interrupted (Esc, hiding the panel, losing the target, or no answer for 5 s), the unsent remainder stays in the editor and the mode stays open. Send continues from there.
  - Text the target acknowledged leaves the editor.
  - A commit that timed out may have arrived anyway, so the editor underlines the text that may have arrived in amber: the one character that timed out when text went one character at a time, or the whole commit otherwise. Editing the marked text removes the mark.
  - The final Enter is tracked on its own, so a retry never sends the message twice. If only the Enter fails, the editor is empty and Send presses just Enter.
- **Send while capturing:** capture stops, Send waits for pending phrases, then sends. The wait can be cancelled with Esc, and ends with a notice when one phrase takes longer than recognition may (35 s on the device, 15 s with a server, 45 s with the fallback); each phrase recognized restarts it.
- **Target:** Send types into whatever has keyboard focus at that moment. Switching windows mid-dictation is the user's choice.

## Type as I speak

The **Type as I speak** switch on the Dictation card skips the editor. The mic key starts and stops capture on the normal keyboard, and each phrase is typed into the focused application as soon as its text returns. Turning it on from dictation mode returns to the normal keyboard.

- **Live words,** with a server: while a phrase is spoken, the helper recognizes its audio so far once 1.5 s are in and then every second, whenever nothing else waits. A word is typed once three guesses in a row agree on it exactly, never the newest guess's last word, which may be cut off mid-word. When the phrase ends, its final text types the rest, continuing where the typed words end in it by the cheapest alignment, in which one word may also stand for two, so a word the final text added, dropped or rewrote (such as "twenty five" becoming "25") is neither repeated nor lost. Typed words are never revised.
  - Three exact agreements stay close to the error rate of recognizing each phrase whole while typing most words seconds before the phrase ends; fewer guesses or a looser match lock in early words that Whisper later changes.
  - The prompt is asked for once, before the first partial, and the final request reuses it, so the phrase's own early words never become its context.
  - Each partial uploads the phrase so far: several times the audio of the final request alone, still only to the configured server.
  - Live words stay server-only: on device, recognizing every second would keep four cores busy. The editor does not use live words.
- On device, Type as I speak types each phrase when it is recognized, about 2 s after it ends.
- No character limit and no counter; the output queue paces delivery.
- **Order:** each phrase goes into the output queue after keys already queued, cut into commits of at most 32 characters, the most the local text path takes at once. While the queue is full, a composer reading is in progress, or a failure waits for its retry, recognized phrases wait in order. More than about 1000 characters waiting pause listening, with a status; what was said still types.
- **Delivery:** phrases follow the **Send** setting. Typed, they go out a character at a time at the chosen pace. With **Paste**, each phrase replaces the clipboard and is pasted with Ctrl+V like Send's text, without Enter; the next phrase waits until the application has taken it. Steam's own fields keep their direct path. If the clipboard is unavailable or Ctrl+V cannot be pressed, the session types instead; if the application does not take a pasted phrase within 2 s, listening pauses, since it may still arrive.
- **Uncertain delivery:** if a phrase fails partway, listening pauses and the status line asks for Enter. Enter retries only the characters that had not arrived, then the waiting phrases follow. A phrase the target refuses outright waits at the front, and Enter, which is then not sent, tries it again.
- Undo last phrase is not offered, because the cursor in the target may have moved.
- Esc while capturing, or while phrases are still to be typed, stops capture, drops what is not yet typed, and is not sent. Otherwise Esc is the normal key.
- **Mic key while capturing** stops capture. The phrase being spoken is still recognized and typed.
- In space-separated languages, every phrase after the first in a capture session starts with a space. The prompt's context is the phrases typed in that session, at most 200 code points, kept in memory only.
- Other keys keep typing while capturing.

## Dashboard, passthrough and focus

- **Dashboard closed while capturing:** passthrough is turned on automatically, so the keyboard stays open and capture continues, even if Steam asks to close the keyboard. That passthrough is tracked apart from the user's own toggle; the toolbar button shows either, and turning it off ends both. It stays on until dictation mode ends; with Type as I speak, until the last phrase is typed.
- **Dashboard closed while not capturing:** the keyboard hides as usual. The dictation state (mode, editor text, cursor, scroll) is kept in memory and restored when the keyboard opens again. A paused editor whose capture already turned passthrough on stays open, because that passthrough lasts until the mode ends.
- **X** discards everything, without asking: capture stops, pending phrases and the editor text are dropped, and dictation mode ends.
- **Typing target lost,** for example when Steam's field closes:
  - **Editor mode:** dictation continues into the editor and the panel stays open, so the user can select the right field and Send there. In replacement mode, dictation holds the panel while it listens, has phrases on the way, sends, or has text. Without a Steam field the editor keeps working but nothing is sent: Send is disabled and the editor says "The text field closed. Select a text field to send." The focused application never receives text it did not ask for. A new field takes over through the usual handoff, and the stock keyboard's fallback always wins over the hold.
  - **Type as I speak:** capture stops at once, and phrases not yet typed are discarded, including text already queued for the old field. The status line says "Dictation stopped: the text field changed." The keyboard sees a field change only when it replaces Steam's keyboard, where each field is its own request; otherwise it cannot see which window has focus.
- **Switching windows** is handled the same way as closing and reopening the dashboard.
- **Panel hidden by the keyboard itself:** tracking lost (including taking the headset off), **Use Steam keyboard**, or the watchdog handing a request back to Steam all hide the panel, even while dictation holds it:
  - **Editor mode:** listening pauses. Phrases already spoken are still recognized and inserted, and the text stays for the next time the keyboard opens. After **Use Steam keyboard**, Steam's keyboard always takes that request; once it closes, an editor that still has text holds the panel again, as after a field closes.
  - **Type as I speak:** capture stops and phrases not yet typed are dropped, as when the field changes. Hiding the panel stops it the same way.
- **Headset off:** the headset enters standby a few seconds after it is taken off, and listening stops at once, checked four times a second while listening. The editor keeps its text and says "Listening stopped: the headset was taken off."; Type as I speak stops as when the field changes, with the same words in the status line. Phrases already spoken in the editor are still recognized.
- **Sleep:** listening stops in the first frame after wake, with "Listening stopped: the headset was asleep." The keyboard sees a sleep as boot time, which counts suspend, running ahead of the monotonic clock, which does not. The clocks both processes use stop during sleep, so neither treats the other as stalled on wake. A recognition request cut off by sleep fails with its usual error.
- **Update:** when an update is ready to install, dictation is discarded: capture stops, pending phrases and the editor text are dropped, and the helper is stopped. The keyboard restarts anyway, which would lose the text. The handoff starts only once the helper and its recorder have gone.
- **App exit or restart:** the editor text is lost.
- **Unchanged:** held keys are still released on hide and target change, and the stock keyboard fallback is unchanged.

## Errors

Waiting for the recognizer, errors and the discard confirmation appear in a band over the editor, styled distinctly from entered text; errors are amber. In Type as I speak mode they use the status line. The errors reported are:

- no microphone;
- capture stopped by the system;
- on-device model missing or failed to load, or the device taking too long;
- speech server not configured, unreachable or timing out;
- API key rejected, or rate limited by a provider;
- server error, or an unusable reply;
- the helper exited.

On a recognizer failure, capture stops, and the failed phrase and every phrase queued after it are dropped, with the reason shown. Text already in the editor stays.

## Settings

The **Dictation** card is third: Languages and Layouts, General, Dictation, Integration. Cards are 470 px wide, so three show in full and the start of the fourth is cut off at the right edge, hinting that the row scrolls sideways with the stick or the horizontal scrollbar.

- **Dictation** dropdown, saved as `dictation`:
  - **Off**: the card shows nothing else;
  - **On device**, the default. With a language other than English or Spanish, an amber line under the selector says that this language will have low accuracy on the device and that a self-hosted speech server gives better results;
  - **Self-hosted** (`server`): the user's own speech server.
- **On device:**
  - The **Model** row shows the model's size and state: not downloaded, downloading with its progress, ready or failed, with **Download**, **Cancel** or **Delete**.
  - Applying a change to On device, the fallback or the format without the model asks first ("Download the speech model?"), naming the download size; Cancel applies nothing. Any other Apply does not ask, so the default needs the Model row's **Download**. Progress also shows on the main keyboard without blocking it ("Downloading the speech model: 45%"), and the mic key stays disabled until the model is ready.
  - **Delete** removes the downloaded model.
  - **Format** row, only with `developer_options` in `config.json`, to compare formats: 8-bit (default), 5-bit and 4-bit. 4-bit downloads the 488 MB f16 file and makes a 145 MB model from it, shown as "488 → 145 MB" so it does not look larger than 5-bit.
- **Self-hosted:**
  - A **Server** field, saved as `dictation_server`, drawn as a small recessed text field. It holds `host:port`, for example `speech.local:8031` or `192.168.1.20`; port 8031 is assumed when it is left out. The default avoids 8080, which is common and which Steam on the Frame already uses for its own debugging port.
  - **Connection** with a **Test** button, which sends half a second of silence to the pending address before Apply. The result is a word or two: Reachable, Unreachable, No answer, Server error, Bad reply, Helper stopped.
  - **Fall back to on device** switch, saved as `dictation_fallback`, default off.
- **Send:** the pace (20, 30 or 40 ms) or Paste; see [Send](#send).
- **Type as I speak** switch, saved as `dictation_direct`, default off.

Hosted providers and their API keys are configured only in `config.json` and the key file; the card has no provider presets. Apply keeps `dictation_url` and `dictation_model` as they are in the file. All fields are listed in [configuration](configuration.md#dictation).

### Entering the server address

Settings has no text fields of its own, and its cards cover the keys. Selecting the Server field opens an **address popup** over a dimmed Settings, with its own one-line field and a small hardcoded keyboard. The address is set rarely, so this stays deliberately simple.

- **Keys:** lowercase letters, digits, and `.` `-` `:` `[` `]`, the characters a `host:port` address can hold, in a 12-column grid: `Esc 1–0 ⌫`, `q–p - .`, `a–l : [ ]`, `z–m ← → Enter`. There is no Space, Shift, Ctrl, Alt, Super, numpad, swipe or Paste; host names ignore case. Every key acts on press.
- **Editing keys:** Backspace and the arrows. Enter accepts the value as the pending setting, which Apply saves like any other. Esc or the popup's close button cancels. While the popup is open, nothing under it responds; it closes with Settings.
- **No Paste:** Full Keyboard never reads the clipboard, here or anywhere else.
- **Local only:** the popup types only into its field, never into Steam or the focused application, even with Replace native keyboard on.
- **Size:** smaller than the main keyboard, so it fits inside the panel. The layout is hardcoded and does not follow the active layout or language.
- **Validation:** an invalid value keeps the popup open with a short reason in red. A valid address outside the local network shows an amber warning that speech would cross the internet unencrypted. Private and link-local IPv4 and IPv6, loopback, Tailscale (`100.64.0.0/10`, `*.ts.net`), single-label names and the `.local`, `.lan`, `.home`, `.home.arpa` and `.internal` suffixes count as local.
- **Recent servers:** up to five servers that passed Test, newest first, are offered as chips under the field; one press fills the field. They are saved as `dictation_recent_servers`.

`dictation_server` can also be edited in `config.json`, over SSH or in Desktop Mode, like every other setting.

Full Keyboard never scans the network or discovers servers. It contacts only the address the user typed: Test checks that address, and phrases go to it while capturing.

### Mic key visibility

The mic key appears only when the active language profile has a `dictation` object and dictation is usable:

- **On device:** the model is ready and not being downloaded (while it downloads, the key is shown disabled).
- **Self-hosted:** a server address is set.

## On-device recognition

Recognition on the Frame's CPU when no server is used, or as the opt-in fallback when the server fails.

- **Model:** multilingual Whisper small, one model for every language. The default is 8-bit (`ggml-small-q8_0.bin`, 264 MB, about 2 s per phrase, about 426 MiB loaded). The development Format row also offers 5-bit (hosted, 190 MB) and 4-bit (made on the Frame from the 488 MB f16 file with whisper.cpp's `whisper-quantize`).
- **Download:** only after the user confirms it in Settings, which names the size. From Hugging Face `ggerganov/whisper.cpp` at a pinned revision over verified HTTPS, following redirects only to Hugging Face hosts, with each file's SHA-256 built into the release. A `0600` temporary file in a `0700` directory is renamed into place under `~/.local/share/framekeyboard/models/` once size and SHA-256 match. **Delete** removes it, and `uninstall.sh` too.
- **Engine:** a small program of the keyboard's own, `framekeyboard-whisper`, built against whisper.cpp at a pinned commit for the Frame's CPU (`armv8.6-a+dotprod+fp16+i8mm`, no GPU backend). The build fetches that commit; the repository keeps only its license in `third_party/`. The engine loads the model once and answers requests on standard input and output: a JSON header line, then 16-bit samples; one JSON line back. It prints nothing else and cannot dump core. The dictation helper starts it and talks to it through pipes, so nothing listens on the network.
- **Threads and priority:** 4 threads at `nice 10`.
- **Lifetime:** the engine starts on the first on-device phrase, loading the model in about 0.3 s, and stops after 60 s unused. A cancel kills it.
- **Time limit:** 30 s per phrase on the device; with the fallback, the server's 10 s and then the device's 30 s. A timeout says the device took too long rather than blaming the server.
- **Fallback:** with **Fall back to on device**, a phrase the server cannot recognize because it is unreachable or timed out goes to the engine, and its text is marked as recognized on the device, which turns the [place icon](#mic-key) to the headset. Live-word partials never fall back. Connecting to a server gives up after 3 s, so the fallback starts quickly even when a firewall silently drops the connection.
- **Limits:** a phrase recognized within about 3 s of its end; at most about 450 MiB while loaded and nothing once unloaded; no noticeable effect on a running game.
- **Features:** editor dictation and Type as I speak, without live words.

### Why these model choices

- **8-bit:** 4- and 8-bit models use ggml's ARM int8 matrix paths (`i8mm`, `dotprod`); 5-bit has none and takes about 2.5 times as long. 8-bit has the best accuracy, is only 0.1–0.2 s slower than 4-bit, and is hosted ready-made at the pinned revision, so the Frame needs no conversion step. 4-bit loses about a point in Spanish.
- **Multilingual, not `small.en`:** the English-only model gains nothing in English, so one model serves every language.
- **4 threads:** fewer are clearly slower (about 3.3 s with 2 and 2.4 s with 3 for a 10 s clip); 6 saves only 0.1 s.
- **30 s window:** Whisper's normal window makes latency nearly fixed, so a 3 s phrase takes almost as long as a 10 s one. A window sized to the clip is faster, but made every model repeat sentences on some clips and turned large-v3-turbo into mixed-language gibberish, so the full window is always used.

| Model on the Frame (4 threads, `nice 10`) | Word errors | 3 s phrase | Peak memory |
| --- | --- | --- | --- |
| small 4-bit (`q4_0`) | EN 5.4%, ES 6.5% | 1.8 s | 312 MiB |
| small 5-bit (`q5_1`) | EN 5.4%, ES 5.4% | 5.1 s | 352 MiB |
| small 8-bit (`q8_0`, default) | EN 5.2%, ES 5.4% | 1.9 s | 426 MiB |

## Microphone

- **Capture:** the helper records 16 kHz mono 16-bit audio with a `pw-record` child, about 32 KB a second. `pw-record` itself uses about 0.4% of a core on average and about 8 MiB.
- **Voice filters:** while a game runs, SteamOS turns on WirePlumber "smart" filters in front of the default source: a mic EQ, WebRTC echo cancellation with automatic gain, and noise suppression. Their noise suppression mutes stretches of speech to exact zeros and mangles sentences, while the raw microphone transcribes nearly word for word. Without a game they are off.
- **Raw microphone:** WirePlumber redirects any stream aimed at the microphone or a filter stage to the filter chain, and PipeWire's default socket may not create links. So at capture start the helper looks for the one microphone that smart source filters target (`pw-dump`), starts `pw-record` without automatic linking, and links the microphone's channels to it through the session manager's socket, `pipewire-0-manager`. Without that socket, a single clear target or a working link within 1 s, it records the default source instead. A linked recorder that delivers nothing for 1 s, such as after a lost link, is replaced by the default source for the rest of that capture. Without a game this records the same audio as the default source.
- **Game sound:** the raw microphone hears the game too, quietly. While the filters are linked, found at capture start or at a phrase start at least 20 s after the last look, the status line says "Game sounds may be picked up during a play session." until capture stops. With game sound under the speech, pauses are found less often, so phrases can run longer.
- **Only while worn:** the Frame runs its microphone only while the headset is worn. Off the head, the source delivers exact zeros at the normal rate, not an error, so a session ends through the 60 s silence stop.
- **Noise gate:** the default source has a noise gate that outputs exact zeros about 1 s after the input drops below its threshold, and holds open through pauses of about 1.5 s between words.
- **Phrase detection:** energy-based, with an adaptive noise floor, 0.3 s of pre-roll, an end after 0.6 s of silence and at most 25 s per phrase; the cheapest detection, in line with keeping Full Keyboard light. Exact zeros mean certain silence, but the gate's hold is longer than the end-of-phrase pause, so the floor is the quietest open-gate frame of the last 2 s, ignoring zero frames. Until it has 0.5 s of open-gate audio a default floor is used, and each phrase is re-checked against the floor when it ends, so steady noise at the start of a session is dropped rather than sent. Detection counts samples, not wall-clock time.
- **Other voices:** the microphone picks up nearby speech, such as a video playing on a phone across the room, and dictation transcribes it like the user's own.

## Speech server

### Protocol

The keyboard knows only the server address. Each phrase is one plain HTTP request in the widely implemented OpenAI-style transcription format, so existing self-hosted servers can answer it:

- `POST http://<host>:<port>/v1/audio/transcriptions` as `multipart/form-data`, with these fields:
  - `file`: the phrase as a 16 kHz mono 16-bit WAV, always named `phrase.wav`;
  - `language`: the Whisper code from the language profile;
  - `prompt`: the profile's script prompt followed by the context text;
  - `response_format=json` and `temperature=0`.
- The reply is JSON with a `text` field.
- **Path fallback:** if the server answers 404 on `/v1/audio/transcriptions`, the keyboard tries whisper.cpp's default `/inference` once, with the same fields, and remembers which path worked. A `whisper-server` started without `--inference-path` therefore works too.
- **Model name:** sent as `model` only when `dictation_model` is set in `config.json`. Some servers need it, for example Speaches unless a default model is configured; `whisper-server` ignores it.
- **Server-side configuration:** the model, GPU, threads and decoding options belong to the server's own configuration.
- **Timeout:** 3 s to connect and 10 s per phrase. A connection failure is retried once; that is safe because nothing is typed before the text returns.
- **One request at a time:** a phrase is sent only after the previous one returned and was inserted. The keyboard, not the helper, supplies each phrase's prompt from the editor as it is then, so its context includes the phrase before it, as cleaned, joined or edited. Servers answer in well under half a second, so this costs little.
- **Reply limits:** a reply larger than 64 KB is rejected before it is parsed, and only the `text` field is read.
- **API key:** when a key file exists, it is sent as `Authorization: Bearer <key>`, only over HTTPS, and only to the host of the configured URL. Redirects are never followed. A rejected key (HTTP 401 or 403) and a rate limit (HTTP 429) get their own error messages.
- **Test:** sends half a second of silence and expects a valid reply. A reply with empty or invented text passes: any valid reply with a `text` field shows the server is reachable and speaks the protocol.

**Why one request per phrase:** Whisper transcribes whole clips anyway, phrase detection is cheap on the Frame, and stateless requests keep any compatible server usable. Audio is about 32 KB per second of speech, and the network adds only 30–50 ms per phrase over Wi-Fi.

### Hosted providers

There are no provider presets in Settings. A hosted provider is set up in `config.json`:

- **`dictation_url`:** a full base URL such as `https://api.groq.com/openai/v1`. It takes the place of `dictation_server`, and the keyboard appends `/audio/transcriptions`. Only `http` and `https` are accepted.
- **`dictation_model`:** the provider's model name, for example `whisper-large-v3-turbo` on Groq.
- **Key file:** `~/.config/framekeyboard/dictation-key`, readable only by the user, holding the API key on one line.
  - The user fills it in Desktop Mode, by pasting into a text editor, or over SSH. Full Keyboard never reads the clipboard.
  - The key is never written to `config.json`, which users share in bug reports, and never appears in logs, arguments or the environment. The helper reads the file itself.
  - `uninstall.sh --purge` removes it with the other settings.
- **Providers known to speak the protocol:** Groq, OpenAI and Mistral, each at `/v1/audio/transcriptions` (Groq under `/openai/v1`). Their prices and models change; the setup guide points to them without promising either.
- **Not supported:** providers with their own protocols, such as Deepgram and ElevenLabs.

### Reference server

The reference is whisper.cpp's `whisper-server` (v1.9.5) on the user's computer:

- **Model:** large-v3-turbo, quantized q5_0.
- **Backend:** the computer's GPU.
- **Window:** Whisper's normal 30 s window, never a window sized to the clip.
- **Path:** `--inference-path /v1/audio/transcriptions`.
- **Decoding:** non-speech tokens suppressed (`--suppress-nst`), no timestamps (`--no-timestamps`).
- **No `--convert`:** the keyboard already sends 16 kHz mono WAV, and uploads are then decoded in memory. With conversion on, the server writes each upload to a temporary file.
- **Logging:** without `--print-realtime`, the server prints no transcripts; it logs each upload's file name, length and language.
- **Network:** listening on the LAN address on port 8031 (`--host <LAN address> --port 8031`).

The exact commands, running it as a service, and checking it are in [speech server setup](speech-server.md#server-options).

### Why large-v3-turbo on a GPU

Measured with 80 FLEURS test clips (CC-BY-4.0), 20 each in English, Spanish, Japanese and Chinese, 5–15 s long; English and Spanish scored by word error rate, Japanese and Chinese by character error rate, punctuation removed:

| Model and setup | EN | ES | JA | ZH | 3 s phrase |
| --- | --- | --- | --- | --- | --- |
| large-v3-turbo q5_0, desktop GPU | 3.4% | 2.6% | 3.3–3.5% | 9.8–9.9% | 0.09–0.24 s |
| large-v3 q5_0, desktop GPU | 2.9–3.4% | 2.6% | 2.8% | 9.5–9.6% | 0.15–0.32 s |
| medium q5_0, desktop GPU | 3.4% | 3.5% | 6.2% | 10.3% | 0.12–0.19 s |
| medium q5_0, desktop CPU, 8 threads | 3.4% | 3.5% | 6.2% | 10.3% | 0.84–3.88 s |

- **Turbo is the right default:** close to large-v3's accuracy at medium's speed or better.
- **Use a GPU:** a desktop GPU answers a phrase in well under half a second; CPU-only speed varies widely between computers. Accuracy is the same on every machine for a given model; only speed differs.
- **First request:** a GPU backend can take several seconds on the first request of a new process while it prepares itself, so a long-running server pays this once.
- **Chinese** stays around 10% for every model, partly from how the reference text writes numbers and wording.

### Compatible servers

Any server that implements the OpenAI-style transcription endpoint should work; `whisper-server` is the measured reference. Users can choose:

- **whisper.cpp `whisper-server`:** the reference. Small, and runs on Metal, CUDA or Vulkan.
- **Speaches** (formerly faster-whisper-server): Docker, mainly NVIDIA GPUs. It needs a model name unless a default is configured.
- **LocalAI:** a general self-hosted OpenAI-compatible server that can use whisper.cpp.

Servers with their own protocols, such as Home Assistant's Wyoming Faster Whisper, are not supported.

### Setup

The repository ships instructions only, no server code; they are in [speech server setup](speech-server.md). They cover installing or building `whisper-server` on Linux, macOS and Windows with a GPU backend, downloading large-v3-turbo, the start command, running it at login or as a service, binding it to the LAN address and firewalling it, and checking it from another machine with `curl` before using it from the keyboard.

## Privacy and security

- **When audio leaves the Frame:** only in Self-hosted mode, only to the configured host, and only for a dictation session's phrases or a Test, including phrases still queued when capture stops.
- **On the Frame:** audio and editor text stay in memory only. No audio, transcript or editor text is logged or written to disk; the log records only lengths, codes and why a session stopped. The helper inherits no output, sends fixed error codes, and puts no text or audio into arguments, the environment or files. Neither the helper nor the engine can dump core.
- **Clipboard:** the keyboard never reads the clipboard. It writes it only when Send is set to Paste, and then holds just the sent text.
- **Plain HTTP on the local network:** host and port only means no encryption or authentication. Anyone on the local network can reach the server, and audio and text cross the network unencrypted. Therefore:
  - hosts are not restricted, so hosted providers can be used. When an address is not on the local network (RFC 1918, link-local, loopback, IPv6 unique-local or link-local) or Tailscale (`100.64.0.0/10`, `*.ts.net`, encrypted by WireGuard), Settings warns that it is not local, and for plain HTTP that the traffic is unencrypted;
  - the setup guide recommends binding the server to the LAN and firewalling it;
  - plain HTTP to a local server is accepted without any notice.
- **HTTPS:** an `https` address is verified against the system's certificates, which covers hosted providers. Self-signed certificates get no special handling; users who want encryption on their own network can put a reverse proxy with a real certificate in front of the server.
- **Hosted providers:** audio goes to that company under its terms. API keys go only over HTTPS to the configured host (see [hosted providers](#hosted-providers)).
- **Other voices:** the microphone picks up nearby speech, and dictation transcribes it like the user's own.
- **Server logs:** whether the server logs is up to its configuration; the setup guide points this out.
- **Replies are untrusted input:** they are validated and cleaned as described under [joining and cleanup](#joining-and-cleanup), and in dictation mode they only reach the editor until Send.

## Architecture

| Piece | Responsibility | Notes |
| --- | --- | --- |
| `framekeyboard-dictate` helper (`dictation/dictation_helper.py`) | Microphone capture through a `pw-record` child, speech level, phrase detection, WAV encoding, server requests, reply cleanup, the on-device engine, model downloads | Separate process, so a stuck device or network call cannot block the VR loop. Python standard library only, like the update helper. One worker thread sends requests, so capture never waits for the network. Started on the first use of dictation; idle it uses no CPU and about 21 MiB, and under 1% of a core while capturing. |
| On-device engine (`framekeyboard-whisper`, `src/whisper_engine.cpp`) | whisper.cpp with the small model, run by the helper as a child process with a line protocol | Only for On device and the fallback. Low priority; unloads when idle. |
| Helper client (`Dictation`, `src/dictation.cpp`) | Spawns the helper, sends `alive`, validates packets | Text must be valid UTF-8 of at most 8 KB; a reason outside the fixed list becomes `bad-reply`; a helper that exits or sends an oversized packet becomes `helper-exited`. |
| Helper lifetime | Exits on socket EOF, `SIGTERM` or a stall, and stops `pw-record` first | The service keeps helpers alive across a handoff (`KillMode=process`), so the helper must notice the keyboard is gone on its own. `~Dictation` waits up to 1 s for it, then kills it; a killed helper's recorder dies on its next write to the closed pipe. |
| `DictationEditor` (`src/dictation_editor.cpp`) | UTF-8 text with a byte cursor on character boundaries, line layout, limits in code points, phrase insertion and joining, phrase undo, context | Pure and host-testable. |
| Dictation layout (`src/dictation_mode.cpp`) | The mode's keys, derived from the active layout as the emoji picker does | The mic key is a `dictation` action at the free side slot in every bundled layout. |
| Send (`src/dictation_send.cpp`) | Cuts the editor text into pieces: commits, Shift+Enter breaks or single-line spaces, and the final Enter, each with the editor bytes it covers | App sends one piece at a time through the output queue. |
| App integration | Dictation mode, key routing to the editor, Esc rules, Send, Type as I speak, composer commits into the editor | In dictation mode every key goes to the editor before swipe, the composers or the output queue; keys still show as pressed and latch through `KeyboardState` as local keys. Typed text reaches the editor through `TextOutput`. Rendering stays free of input delivery. |
| Panel | Editor drawing, greyed text, overlays, counter, mic bars, ring, Settings text fields, address popup | The bars and ring redraw at 30 fps and only the mic key's region, which a partial texture upload copies to the GPU. |
| VR | Passthrough while capturing, stick scrolling in both modes, the dashboard and Steam keyboard request reported to App each frame | |

`--render-dictation` exports an image of dictation mode for reviewing its look, with a sample reading for composer profiles.

### Helper protocol

The keyboard starts `dictation/dictation_helper.py` with `python3 -I` and a private `SOCK_SEQPACKET` socket on descriptor 3. Standard input, output and error go to `/dev/null`. Each packet is one JSON object of at most 16 KB. The helper never prints and has no other channel. A dead helper cannot raise `SIGPIPE` in the keyboard, and transcripts are never logged.

Keyboard to helper:

| Packet | Meaning |
| --- | --- |
| `{"op":"configure","server":"host:port"}` or `{"op":"configure","url":"https://…/v1"}`, or `{"op":"configure","device":"<model file>"}`, with optional `"model"`, `"language"` and, with a server, `"device"` and `"fallback"` | Recognition target for the following session. `server` defaults to port 8031 and path `/v1`, and may fall back to `/inference`; `url` is used as given. |
| `{"op":"capture","on":true}` / `{"op":"capture","on":false}` | Start or resume capture, or pause it. Pausing ends the phrase being spoken, which is still recognized. |
| `{"op":"prompt","id":N,"text":"…"}` | The prompt for phrase N, sent in answer to `ready`: the profile's script prompt, then the context, at most 200 code points in all. |
| `{"op":"cancel"}` | Stop capture and drop every pending phrase. The helper answers `cancelled`. |
| `{"op":"test"}`, optionally with `"server"` or `"url"`, `"model"` and `"language"` | Send half a second of silence to the configured target, or to the one given, such as an address not yet applied; phrases still waiting keep the configured target. |
| `{"op":"download","model":M,"directory":"…"}` / `{"op":"download-cancel"}` | Download model M from the pinned revision into that absolute directory, or stop the download. |
| `{"op":"alive"}` | Sent every 0.25 s while capturing, while phrases or a Test are pending, and for 5 s after. Without one for 2 s while it captures or has phrases, the helper stops capture and drops pending phrases. Idle, neither process wakes for it. |

Helper to keyboard:

| Packet | Meaning |
| --- | --- |
| `{"op":"level","value":0…255}` | Speech level for the mic bars, at most 15 times a second while capturing. |
| `{"op":"phrase","id":N}` | A phrase started. |
| `{"op":"ready","id":N}` | Phrase N ended and is next to be recognized; the keyboard answers with `prompt`. Sent only when no request is in flight and the previous text was delivered. |
| `{"op":"text","id":N,"text":"…"}` | Cleaned text for phrase N, with `"device":true` when the fallback recognized it on the Frame. |
| `{"op":"dropped","id":N,"reason":R}` | Phrase N produced nothing usable: `empty`, `invalid` or `too-long`. |
| `{"op":"capture","on":false,"reason":R}` | Capture stopped by itself: `silence` after 60 s without speech, or an error code. |
| `{"op":"error","reason":R}` | A fixed error code. Capture stops and pending phrases are dropped. |
| `{"op":"paused"}` | The answer to `capture` off. Every phrase spoken before the pause has been announced, so Send can wait for exactly those. |
| `{"op":"microphone","unfiltered":true}` | Capture records the microphone in front of SteamOS's voice filters while they are on, so game sound may be picked up. Sent at most once per capture; the keyboard says so in the status line while capturing. |
| `{"op":"cancelled"}` | The answer to `cancel`. The keyboard ignores every capture packet sent before it, such as a phrase that started just before the cancel arrived. |
| `{"op":"test","ok":true}` / `{"op":"test","ok":false,"reason":R}` | The Test result. |
| `{"op":"download","state":S,…}` | Download state: `progress` with `done` and `total` bytes, at most once per percent; `quantizing`; `ready`; or `failed` with a reason. |

Error codes: `no-microphone`, `capture-stopped`, `not-configured`, `unreachable`, `timeout`, `server-error`, `bad-reply`, `key-rejected`, `rate-limited`, `backlog-full`, `keyboard-stalled`, `invalid-command`.

- **Timing:** phrase detection counts samples, not wall-clock time, so a test recorder can stream a recording at full speed. Only `alive` and the request timeouts use the clock.
- **Backlog:** at most 8 phrases and 120 s of audio wait for recognition. Beyond that, the helper reports `backlog-full`.
- **Shutdown:** the helper exits on socket end-of-file, `SIGTERM` or a stall, and stops `pw-record` first. If the helper is killed, `pw-record` dies on its next write to the closed pipe.

## Credit

The helper process, `pw-record` capture, transcript cleanup and repetition guard follow the dictation pull request by amirhh00 and adepssimius (`TaiKeid/steam-frame-full-keyboard#2`). That pull request vendored whisper.cpp and ggml into the repository and ran them on the Frame, and is not merged; its ideas are reimplemented on the current code.
