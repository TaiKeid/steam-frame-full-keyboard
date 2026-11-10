# Upkeep checklist

Full Keyboard builds on pinned third-party code and data, on libraries SteamOS provides, and on private Steam and SteamOS behavior that no version number pins. This checklist keeps all three current. Licenses and attribution for the pinned code are in [third_party/README.md](../third_party/README.md); release steps are in [releasing](releasing.md).

## When

- Monthly, and before every release.
- After a SteamOS, Steam client or SteamVR update on the Frame: see [SteamOS libraries](#steamos-libraries) and [private behavior](#private-behavior).
- When an upstream security fix is announced for anything below.

## Check for updates

```sh
tools/check-upstream.sh
```

It reads each pin from the file that owns it and compares it with the upstream's latest release, using only `git ls-remote` and public metadata over HTTPS. It does not cover the two data-control protocol files or the AppImage's Anthy, IBus and Chinese-table packages; compare those by hand. **CHECK** means upstream moved, or could not be reached. Then, for each CHECK:

1. Read upstream's changelog or the diff of the files we use, and decide: update, or stay and record why.
2. To update, change the pin where the table below says. Then update the license file and [third_party/README.md](../third_party/README.md) in the same commit.
3. Run that row's tests, then the [integration tests](#integration-tests).
4. Record the date, the versions and the decision in the release notes, or in the changelog when users notice it.

## Pinned third-party code and data

| Component | Pin lives in | Used for | After updating, run |
| --- | --- | --- | --- |
| whisper.cpp (and ggml) | `tools/build-whisper.sh` (`pinned=`), `third_party/whisper.cpp/LICENSE` | On-device dictation engine, `whisper-quantize` | Rebuild with `tools/build-whisper.sh` for host and frame-arm64; `dictation-helper`; the on-device benchmark on the Frame against the [limits](dictation.md#on-device-recognition) (time per phrase, memory, word errors); a headset try of each model format |
| Whisper models | `dictation/dictation_helper.py`: `MODEL_REVISION`, `MODELS` (sizes, SHA-256) | Downloads in Settings | `dictation-helper` (Downloads); a real download of each format on the Frame |
| ExecuTorch with XNNPACK | `tools/build-executorch.sh` (`pinned=`), `third_party/executorch/` | Swipe model runtime | Rebuild with `tools/build-executorch.sh`; `swipe-model`; swipe in the headset |
| FUTO Swipe encoder | `third_party/futo-swipe/README.md` (revision, SHA-256), `model_fp32.pte` | Swipe typing | `swipe-model`; the license notice beside the setting is still accurate |
| Mozc server (fcitx5-mozc) | `tools/fetch-mozc.sh` (`version=`, a SHA-256 per architecture), `third_party/mozc/` | Japanese conversion | Check both packages' signatures before pinning ([building](building.md#dependencies)); `tools/fetch-mozc.sh` for host and frame-arm64; `japanese-input`; the simulator's `--japanese-candidates` report over a fixed phrase set, compared with the pinned version's; Japanese conversion and the server's memory in the headset |
| Mozc common-word and kaomoji data | `tools/build_japanese_words.py` and `tools/build_japanese_kaomoji.py` (`MOZC_COMMIT`, currently `74f8ee66c5546d1f3ce4fadbef23a3b402239eed`); `japanese/README.md` records source hashes | Japanese completion and kaomoji | Keep both pins identical; rebuild both lists and update source hashes; `japanese-input`; compare the simulator's `--japanese-candidates` report over a fixed phrase set |
| Chinese engines: libime with its dictionary and model, fcitx5's utility library, McBopomofo, OpenCC, zstd, Boost, extra-cmake-modules | `tools/chinese-pins.sh` (URL, tag and commit each; Boost's archive SHA-256; libime's data SHA-256 values come with its commit), `third_party/chinese-engines/` | Pinyin (libime), Traditional output (OpenCC) and Zhuyin (McBopomofo) | `tools/fetch-chinese.sh`, then `tools/build-chinese.sh` for host and frame-arm64; check the frame-arm64 libraries with `objdump -T` for symbol versions newer than stable SteamOS's; `chinese-input`; the simulator's `--chinese-candidates --chinese-engine all` over the benchmark sets, compared with the pinned version's |
| AppImage Anthy, IBus and Chinese tables | `scripts/simulator-appimage/{anthy,ibus,ibus-table-chinese}.sha256` (package versions, archive hashes and payload hashes) | Desktop simulator conversion, Japanese emoji candidates, Cangjie and Quick | Check the new packages and their licenses; regenerate payload hashes from the packages; update the adjacent third-party notices; build the AppImage and try each input method. Packaging rejects unpacked inputs whose bundled bytes differ from the pins |
| Swipe word lists | `swipe/lexicons/README.md` (commit, source hashes), `tools/build_swipe_lexicon.py` | Swipe vocabulary | `swipe`, `swipe-chinese` |
| Offensive word lists (LDNOOBW) | `swipe/lexicons/README.md` (Offensive words: commit, hashes), `tools/build_offensive_words.py` (`ROOTS`, `KEEP`) | Typing suggestions hold these back until used | `word-suggestions`, `typing-suggestions`; rebuild the lists after any word list changes |
| libei client | `third_party/libei/provenance.json`, `third_party/libei/` | Typing into local apps | `ei-handshake`, `ei-recovery`; `ei-target-probe` in its own receiver; the [input matrix](acceptance.md#input-matrix). If SteamOS's own libei gains the negotiation fix, consider dropping the copy |
| OpenVR header | `third_party/openvr/openvr.h` | Overlay, input and render-model calls | Full build; every VR check in [acceptance](acceptance.md) |
| Overlay texture (frame-perf-overlay) | `third_party/overlay_texture/`, commit in `third_party/README.md` | Persistent Vulkan textures | `rendering`; the panel in the headset at every size |
| Gamescope input-method protocol | `third_party/gamescope/gamescope-input-method.xml`, commit in `third_party/README.md` | Unicode text into local apps | `unicode-target-probe`, `japanese-target-probe`; Unicode, Japanese and dictation Send in local apps |
| Data-control protocols | `third_party/wayland-protocols/ext-data-control-v1.xml` and `third_party/wlr-protocols/wlr-data-control-unstable-v1.xml`, tag, commits and SHA-256 in `third_party/README.md` | Setting the clipboard of a nested desktop such as SteamOS's: the standard protocol, the only one KWin 6.6 offers, else wlroots' older one | `clipboard`; in the Desktop, dictation's paste Send and KDE Desktop Unicode set to Paste |
| Unicode emoji list | `tools/emoji_table.py`, version and SHA-256 in `third_party/README.md` | Emoji picker | `emoji`. Only emoji the Frame's color emoji font draws are listed, so check that font's version before taking a newer Unicode version |

## SteamOS libraries

These are linked or used as the Frame provides them, never bundled:
- Anthy;
- libibus (`libibus-1.0.so.5`) and `/usr/share/ibus/dicts/emoji-ja.dict` (Japanese emoji candidates);
- PyZy and libhangul;
- libsqlite3 and the `cangjie5` and `quick5` tables in `/usr/share/ibus-table/tables` (Cangjie and Quick);
- Cairo, Pango, HarfBuzz and GLib;
- json-c, xkbcommon, XCB and the Wayland client;
- Vulkan, SDL2 and SteamVR's `libopenvr_api.so`;
- bubblewrap (`bwrap`), Mozc's sandbox where it is installed;
- Python 3, 3.12 on the Frame (the dictation and update helpers);
- PipeWire and WirePlumber (`pw-record`, `pw-dump`, `pw-link`, and `libpipewire-0.3.so.0` for Haptic sounds);
- the system fonts.

After a SteamOS update:

- Refresh the [ARM64 sysroot](building.md#arm64-sysroot) from the Frame, rebuild, and run the ARM64 core tests on the Frame ([tests](building.md#tests)), with `FRAMEKEYBOARD_TEST_ANTHY_REQUIRED=1`.
- Build releases against the stable SteamOS libraries. A sysroot copied from a beta Frame can tie binaries to newer symbol versions that stable SteamOS can't load, such as glibc 2.43's `acosf` and `sqrtf` in the swipe and whisper engines. Keep such a copy apart until stable catches up, and check release binaries with `objdump -T` for symbol versions newer than stable's.
- Package again: `scripts/package.sh` checks that the release binaries are AArch64, stripped and free of build paths (`tools/check_release_binary.py`), that the executable needs neither PyZy, libhangul nor the system libei, and that the sysroot has every library version Mozc's prebuilt server needs (`tools/check_frame_libraries.py`).
- Check that every bundled script still renders in the headset, including Arabic, Hebrew, Devanagari, Thai, Greek, CJK and emoji. A missing font shows blank keys.

## Private behavior

Nothing pins these. Each one is feature-checked at runtime and fails safe, but an update can still turn a feature off. After a SteamOS, Steam or SteamVR update, check every row:

| Behavior | Used by | How to check |
| --- | --- | --- |
| Steam's local UI debugger on `127.0.0.1:8080` and its keyboard interfaces | [Native keyboard replacement](native-replacement.md) | The [validation list](native-replacement.md#validation). Settings names the reason when it is unavailable |
| Steam dashboard overlay keys, such as `valve.steam.gamepadui.main` | Placement next to the dashboard | Summon from the dashboard; placement and auto leveling |
| Frame controller render models (`frame_controller_left` and `_right`, the `trigger`, `bumper` and `tip` components) | Passthrough pointer, panel drag and resize, keys held off the panel | Passthrough typing, a laser drag and resize, and a repeating key held past the panel's edge |
| SteamOS microphone filters (WirePlumber smart filters in `/etc/pipewire/pipewire.conf.d/`) and the `pipewire-0-manager` socket | Dictation records the raw microphone | Dictate with and without a game; the log shows "recording the microphone without the game's voice filters" only with a game. See [microphone](dictation.md#microphone) |
| Gamescope's text and keymap behavior | Typing and Send's pace | If text arrives scrambled: send a 900-character text to a disposable receiver with `text-replace-probe`'s `whole` and `paced` commands, idle and under load, and count wrong characters (see [Send](dictation.md#send)) |
| Steam app IDs as window names for local games | Native replacement over local games | A local game's keyboard button with replacement on |
| KWin scripting in SteamOS's Desktop (`org.kde.kwin.Scripting`, `workspace.windowActivated`, `resourceClass`) and `libsystemd.so.0` | Terminal Cut, Copy and Paste inside the Desktop ([design](terminal-edit-keys-design.md#steamoss-desktop)) | Open the Desktop with Konsole and Dolphin: Cut shows disabled in Konsole and not in Dolphin, and Copy and Paste work in both |

## Integration tests

After any update above, run these in order and stop at the first failure:

1. `ctest --test-dir build/host --output-on-failure`, with `FRAMEKEYBOARD_TEST_ANTHY_REQUIRED=1` where Anthy is installed.
2. A cross-build with `FRAMEKEYBOARD_SYSROOT=… cmake --build --preset frame-arm64`, and the ARM64 core tests on the Frame.
3. The optional probes, each in its own disposable receiver ([tests](building.md#tests)): `ei-target-probe`, `unicode-target-probe`, `japanese-target-probe`, and `uinput-smoke-test` against its own device only.
4. In the headset:
   - the [acceptance](acceptance.md) checklist;
   - the native replacement [validation](native-replacement.md#validation);
   - dictation in editor mode and Type as I speak, on device and self-hosted, with and without a game.

## Last check

2026-11-06, for 0.15.0:
- **Current:** whisper.cpp v1.9.5, ExecuTorch v1.5.1, libei 1.6.0, OpenVR 2.15.6, the Gamescope protocol file, the Whisper models, the FUTO Swipe encoder file, the Mozc server 3.35.6354.2-1 and the word lists.
- **frame-perf-overlay:** no new upstream commits since the 0.14.1 comparison; the pinned texture code stays.
- **Mozc dictionary and kaomoji data:** upstream moved past the pinned commit; the pin stays until the next Japanese tuning round.
- **Unicode emoji:** SteamOS beta's Noto Color Emoji 2.051 draws all of Unicode 17.0, so the list moved from 15.1 to 17.0; 18.0 has emoji it doesn't draw. Stable SteamOS's older font hides the newer emoji at runtime.
- **SteamOS libraries:** the beta has glibc 2.43 and GCC 15.2.1; releases still build against stable's glibc 2.39 libraries.

2026-10-26:
- **Current:** whisper.cpp v1.9.5, ExecuTorch v1.5.1, libei 1.6.0, OpenVR 2.15.6, the Gamescope protocol file, the Whisper models, the FUTO Swipe encoder file and the word lists.
- **frame-perf-overlay:** moved 25 commits. `vk_texture.cpp` and `vk_texture.h` were moved to `src/`, with 23 and 4 changed lines; still to be reviewed against our local changes.
- **Unicode emoji:** 18.0 is out; we use 15.1. The Frame has Noto Color Emoji 2.042, which covers Unicode 15.0 emoji, so the list stays at 15.1 until the font is newer.
