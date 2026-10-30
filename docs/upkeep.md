# Upkeep checklist

Full Keyboard builds on pinned third-party code and data, on libraries SteamOS provides, and on private Steam and SteamOS behavior that no version number pins. This checklist keeps all three current. Licenses and attribution for the pinned code are in [third_party/README.md](../third_party/README.md); release steps are in [releasing](releasing.md).

## When

- Monthly, and before every release.
- After a SteamOS, Steam client or SteamVR update on the Frame: see [after a SteamOS or Steam update](#after-a-steamos-or-steam-update).
- When an upstream security fix is announced for anything below.

## Check for updates

```sh
tools/check-upstream.sh
```

It reads each pin from the file that owns it and compares it with the upstream's latest release, using only `git ls-remote` and public metadata over HTTPS. **CHECK** means upstream moved, or could not be reached. Then, for each CHECK:

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
| Swipe word lists | `swipe/lexicons/README.md` (commit, source hashes), `tools/build_swipe_lexicon.py` | Swipe vocabulary | `swipe`, `swipe-chinese` |
| libei client | `third_party/libei/provenance.json`, `third_party/libei/` | Typing into local apps | `ei-handshake`, `ei-recovery`; `ei-target-probe` in its own receiver; the [input matrix](acceptance.md#input-matrix). If SteamOS's own libei gains the negotiation fix, consider dropping the copy |
| OpenVR header | `third_party/openvr/openvr.h` | Overlay, input and render-model calls | Full build; every VR check in [acceptance](acceptance.md) |
| Overlay texture (frame-perf-overlay) | `third_party/overlay_texture/`, commit in `third_party/README.md` | Persistent Vulkan textures | `rendering`; the panel in the headset at every size |
| Gamescope input-method protocol | `third_party/gamescope/gamescope-input-method.xml`, commit in `third_party/README.md` | Unicode text into local apps | `unicode-target-probe`, `japanese-target-probe`; Unicode, Japanese and dictation Send in local apps |
| Unicode emoji list | `tools/emoji_table.py`, version and SHA-256 in `third_party/README.md` | Emoji picker | `emoji`. Only emoji the Frame's color emoji font draws are listed, so check that font's version before taking a newer Unicode version |

## SteamOS libraries

These are linked or used as the Frame provides them, never bundled:
- Anthy;
- PyZy and libhangul;
- Cairo, Pango and HarfBuzz;
- json-c, xkbcommon and the Wayland client;
- Python 3, 3.12 on the Frame (the dictation and update helpers);
- PipeWire and WirePlumber (`pw-record`, `pw-dump`, `pw-link`);
- the system fonts.

After a SteamOS update:

- Refresh the [ARM64 sysroot](building.md#arm64-sysroot) from the Frame, rebuild, and run the ARM64 core tests on the Frame ([tests](building.md#tests)), with `FRAMEKEYBOARD_TEST_ANTHY_REQUIRED=1`.
- Check that the release binary links only the expected system libraries (`tools/check_release_binary.py`, run by `scripts/package.sh`).
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

2026-10-26:
- **Current:** whisper.cpp v1.9.5, ExecuTorch v1.5.1, libei 1.6.0, OpenVR 2.15.6, the Gamescope protocol file, the Whisper models, the FUTO Swipe encoder file and the word lists.
- **frame-perf-overlay:** moved 25 commits. `vk_texture.cpp` and `vk_texture.h` were moved to `src/`, with 23 and 4 changed lines; still to be reviewed against our local changes.
- **Unicode emoji:** 18.0 is out; we use 15.1. The Frame has Noto Color Emoji 2.042, which covers Unicode 15.0 emoji, so the list stays at 15.1 until the font is newer.
