# Speech server setup

Dictation in **Server** mode sends each spoken phrase to a speech server you run on your own computer with a capable GPU. Full Keyboard ships no server; this guide sets up whisper.cpp's `whisper-server`, which answers the OpenAI-style requests described in [dictation](dictation.md#protocol). The keyboard needs only the server's address, for example `192.168.1.20:8031`.

Dictation is designed but not released yet; this guide is part of its first milestone.

## What you need

- A computer on the same network as the Frame, ideally with a GPU. On a GPU, a phrase comes back in well under half a second; CPU-only servers take seconds and vary widely (see [why large-v3-turbo on a GPU](dictation.md#why-large-v3-turbo-on-a-gpu)).
- About 600 MB of disk for the model, and about 1 GB of GPU memory while the server runs.
- whisper.cpp **v1.9.5**, the version these instructions were tested with.
- The **large-v3-turbo** model, quantized `q5_0`:
  - file `ggml-large-v3-turbo-q5_0.bin`, 574,041,195 bytes;
  - SHA-256 `394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2`;
  - from Hugging Face `ggerganov/whisper.cpp`, revision `5359861c739e955e79d9a303bcbc70fb988958b1`:
    `https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo-q5_0.bin`

Check the model's SHA-256 after downloading it: `sha256sum` on Linux, `shasum -a 256` on macOS, `Get-FileHash` in PowerShell on Windows.

## Status of these instructions

| Platform | GPU | Status |
| --- | --- | --- |
| Linux | Vulkan | Verified 2026-10-25: built from source, run under systemd as a user unit, and reached from the Frame over Wi-Fi |
| Linux | NVIDIA CUDA | Not tested; same steps with the CUDA option |
| macOS (Apple silicon) | Metal | The Metal build of v1.9.5 was benchmarked; the server and LaunchAgent steps are not tested |
| Windows | NVIDIA CUDA | Not tested; uses whisper.cpp's prebuilt CUDA package |
| Windows | Other GPUs (Vulkan) | Not tested; needs a source build |

## Server options

Every platform starts the server with the same options:

```
whisper-server -m ggml-large-v3-turbo-q5_0.bin --host 0.0.0.0 --port 8031 --inference-path /v1/audio/transcriptions --suppress-nst --no-timestamps
```

- **`--port 8031`:** the keyboard's default port. Port 8080, whisper-server's own default, is avoided because many programs use it, including Steam on the Frame.
- **`--host 0.0.0.0`:** listens on every network interface, so the server still starts when Wi-Fi comes up after it. Limit who can connect with the firewall (see below). On a computer you only start by hand, you can give its LAN address instead.
- **`--inference-path /v1/audio/transcriptions`:** the OpenAI-style path. Without it the keyboard falls back to whisper.cpp's `/inference` path, so this is optional.
- **`--suppress-nst --no-timestamps`:** suppress non-speech tokens such as `[music]`, and skip timestamps the keyboard does not use.
- **Do not add `--convert`.** The keyboard already sends 16 kHz mono WAV, and with conversion on, the server writes every upload to a temporary file.
- **Do not add `--print-realtime`.** Without it the server's log holds no transcripts; it records only each request's file name (always `phrase.wav`), length and language.

The first request after the server starts can take several seconds while the GPU backend prepares itself; later requests are fast. A server left running pays this once.

## Linux

1. Install the build tools and GPU headers.
   - **Arch-based:** `sudo pacman -S --needed git cmake ninja vulkan-headers vulkan-icd-loader shaderc spirv-headers`. The verified build used these, except that SPIRV-Headers came from a Khronos source install instead of the `spirv-headers` package.
   - **Debian or Ubuntu (not tested):** `sudo apt install git cmake ninja-build libvulkan-dev glslc spirv-headers`
   - **NVIDIA with CUDA:** install the CUDA toolkit from your distribution or NVIDIA instead of the Vulkan packages.
2. Build `whisper-server`:

   ```
   git clone --branch v1.9.5 --depth 1 https://github.com/ggml-org/whisper.cpp.git
   cd whisper.cpp
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON
   cmake --build build --target whisper-server
   ```

   For CUDA, use `-DGGML_CUDA=ON` instead of `-DGGML_VULKAN=ON`. The program is `build/bin/whisper-server`.
3. Download the model next to it and check its SHA-256 (see [what you need](#what-you-need)).
4. Run it as a systemd user service. Create `~/.config/systemd/user/whisper-server.service`, replacing the paths:

   ```
   [Unit]
   Description=whisper.cpp speech server for Full Keyboard dictation

   [Service]
   ExecStart=/path/to/whisper.cpp/build/bin/whisper-server -m /path/to/ggml-large-v3-turbo-q5_0.bin --host 0.0.0.0 --port 8031 --inference-path /v1/audio/transcriptions --suppress-nst --no-timestamps
   Restart=on-failure

   [Install]
   WantedBy=default.target
   ```

   Then `systemctl --user daemon-reload` and `systemctl --user enable --now whisper-server`. The service runs while you are logged in; `loginctl enable-linger` keeps it running without a login. `systemctl --user disable --now whisper-server` stops and removes it from login.
5. Allow only the Frame through the firewall, using the Frame's address on your network:
   - **ufw:** `sudo ufw allow from <frame-address> to any port 8031 proto tcp`
   - **firewalld:** `sudo firewall-cmd --permanent --add-rich-rule='rule family="ipv4" source address="<frame-address>" port port="8031" protocol="tcp" accept'`, then `sudo firewall-cmd --reload`

## macOS

1. Install Apple's command-line tools (`xcode-select --install`) and CMake, either from cmake.org or with `pip install cmake ninja` in a Python virtual environment. Homebrew's `whisper.cpp` package does not include `whisper-server`.
2. Build it; Metal is used by default on Apple silicon:

   ```
   git clone --branch v1.9.5 --depth 1 https://github.com/ggml-org/whisper.cpp.git
   cd whisper.cpp
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --target whisper-server --config Release
   ```

3. Download the model and check its SHA-256.
4. Start it at login with a LaunchAgent. Create `~/Library/LaunchAgents/local.whisper-server.plist`, replacing the paths:

   ```
   <?xml version="1.0" encoding="UTF-8"?>
   <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
   <plist version="1.0">
   <dict>
     <key>Label</key><string>local.whisper-server</string>
     <key>ProgramArguments</key>
     <array>
       <string>/path/to/whisper.cpp/build/bin/whisper-server</string>
       <string>-m</string><string>/path/to/ggml-large-v3-turbo-q5_0.bin</string>
       <string>--host</string><string>0.0.0.0</string>
       <string>--port</string><string>8031</string>
       <string>--inference-path</string><string>/v1/audio/transcriptions</string>
       <string>--suppress-nst</string>
       <string>--no-timestamps</string>
     </array>
     <key>RunAtLoad</key><true/>
     <key>KeepAlive</key><true/>
   </dict>
   </plist>
   ```

   Load it with `launchctl load ~/Library/LaunchAgents/local.whisper-server.plist`; `launchctl unload` with the same path stops it.
5. When macOS asks whether `whisper-server` may accept incoming connections, allow it.

## Windows

**NVIDIA GPUs** can use whisper.cpp's prebuilt package. Its builds are published under build numbers; build `b5454` is v1.9.5.

1. Download `whisper-bin-win-cuda-12.4.0-x64.zip` from `https://github.com/ggml-org/whisper.cpp/releases/tag/b5454`. Its SHA-256 is `afef0b881c500958921c3f5523b50e59ee2ec9b6f5cbd25b324c51ed308a957a`. Use `whisper-bin-win-cuda-11.8.0-x64.zip` for older drivers. A current NVIDIA driver is needed.
2. Unzip it, for example to `C:\whisper`. The `Release` folder contains `whisper-server.exe`.
3. Download the model into the same folder and check it with `Get-FileHash`.
4. Start it at logon with Task Scheduler: create a task that runs at logon, with the program `C:\whisper\Release\whisper-server.exe` and the [server options](#server-options) as its arguments.
5. Allow only the Frame through Windows Firewall, in an administrator PowerShell:

   ```
   New-NetFirewallRule -DisplayName "whisper-server (Full Keyboard)" -Direction Inbound -Protocol TCP -LocalPort 8031 -RemoteAddress <frame-address> -Action Allow
   ```

**Other GPUs (AMD, Intel)** need a source build, because no prebuilt Vulkan package is published: install Visual Studio with C++ tools, CMake and the Vulkan SDK, then build as on Linux with `-DGGML_VULKAN=ON`.

## Check the server

From another computer on the network, or from the Frame in Desktop Mode, send a short recording. Any 16 kHz mono WAV works, for example `samples/jfk.wav` from the whisper.cpp repository:

```
curl http://<server-address>:8031/v1/audio/transcriptions -F file=@jfk.wav -F language=en -F response_format=json
```

A working server answers with JSON such as `{"text":" And so my fellow Americans..."}`. If the request times out, check the firewall rule and the address the server listens on. On Windows, use `curl.exe` in PowerShell.

Then enter `<server-address>:8031` in Full Keyboard's Dictation settings and press Test.

## Hosted providers

Instead of your own server, you can use a hosted service that speaks the same OpenAI-style protocol, such as Groq, OpenAI or Mistral. Your audio then goes to that company under its terms, and it bills per minute of audio. There are no presets in Settings yet; set it up in Desktop Mode or over SSH:

1. In `~/.config/framekeyboard/config.json`, set `dictation_url` to the provider's base URL, for example `https://api.groq.com/openai/v1`, and `dictation_model` to its model name, for example `whisper-large-v3-turbo`. Check the provider's documentation for current URLs and models.
2. Put your API key on one line in `~/.config/framekeyboard/dictation-key`, then make it readable only by you: `chmod 600 ~/.config/framekeyboard/dictation-key`.

The key is sent only over HTTPS and only to that URL's host. Settings shows whether a key is set and can delete it, but never shows the key.

## Privacy

- Audio and text travel unencrypted on your network. Keep the server on your own network and limit the firewall rule to the Frame's address.
- Give the Frame a fixed address in your router (a DHCP reservation), so the firewall rule keeps matching.
- The server keeps nothing: it decodes each upload in memory and does not log transcripts with the options above.
