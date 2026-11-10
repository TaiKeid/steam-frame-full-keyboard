# Third-party sources

## libei client

`libei/` contains the client and shared utility sources from libei 1.6.0,
commit `8a46bf3d4b6af7f25a61be53386cf618218114ef`, under the adjacent MIT
`COPYING`. See `libei/README.md` and `provenance.json` for the official archive,
checksum, selected files and protocol regeneration commands. The fixed client
is compiled into Full Keyboard; SteamOS libraries are unchanged. This avoids
libei 1.4.1's null connection cleanup when a peer aborts negotiation. No libeis
server or portal helper is bundled.

## Overlay texture and OpenVR

`overlay_texture/vk_texture.{h,cpp}` comes from
https://github.com/sasaken1102r/frame-perf-overlay at commit
`61c597c4f57624339c222094582f15da32016716`, under its MIT license.
It handles OpenVR-compatible persistent Vulkan textures. Local changes are noted
in comments and Git history. The transport also accepts an explicit Vulkan pixel format, defaulting to its original RGBA format. Native-BGRA selection and fallback are owned by `include/framekeyboard/vr_texture.hpp`.

`openvr/openvr.h` is the Valve OpenVR 2.15.6 header from that same pinned tree.
The adjacent BSD license was retrieved from Valve's OpenVR repository.
The SDK library is supplied by the installed SteamVR runtime, not redistributed.

## Gamescope input-method protocol

`gamescope/gamescope-input-method.xml` comes from ValveSoftware/gamescope commit `6867f509874f9bc52e12d6f4c4596cdf0d5be6b4`, under the MIT license included in the XML. Wayland client stubs are generated at build time. Only version 1 text submission is used, feature-checked at runtime.

## wlroots data-control protocol

`wlr-protocols/wlr-data-control-unstable-v1.xml` comes from https://gitlab.freedesktop.org/wlroots/wlr-protocols commit `151664b67ab1eecf6f2eca055dccea66516cab54`, under the MIT-style license included in the XML (SHA-256 `5ac39552975b28d0ea42f636a79b7fc8e2d1fcfa31cbe173f549bd4b044ae2de`). Wayland client stubs are generated at build time. Version 1 is used only to set the clipboard of a nested desktop session; offers are never received.

## Standard data-control protocol

`wayland-protocols/ext-data-control-v1.xml` comes from https://gitlab.freedesktop.org/wayland/wayland-protocols tag `1.49` (commit `ee78491a237eaff9389a0ccf8680521d074407d3`), `staging/ext-data-control/`, under the MIT-style license included in the XML (SHA-256 `293deba5dfdb974fbcf8dcf891a30a51a36a8bba0047dc6bdf6a03194089b7fb`). Wayland client stubs are generated at build time. Version 1 is used, as the wlroots protocol above, only to set the clipboard of a nested desktop session whose compositor offers this standard protocol instead, such as KWin 6.6; offers are never received.

## Swipe typing

`futo-swipe/` holds the unmodified FUTO Swipe encoder weights and their FUTO
Model Weights License 1.0; see `futo-swipe/README.md` for the pinned revision
and hash. The license requires a visible end-user notice, which Settings shows
beside the swipe option.

The weights run through ExecuTorch v1.5.1 (commit
`3b60683923245cf472b7323426920e15623ba361`, BSD-3-Clause) with its XNNPACK
backend, statically linked into the optional `framekeyboard-swipe` module.
`tools/build-executorch.sh` builds it from that pinned checkout; the source is
not vendored. `executorch/` holds the licenses of ExecuTorch, XNNPACK, cpuinfo,
pthreadpool, FP16, FXdiv (BSD/MIT), flatbuffers and KleidiAI (Apache-2.0).

The pinned backend initializes process-global CPU metadata when the module
loads. Its cpuinfo and XNNPACK deinitializers do not release that storage, so
the adapter is mapped for the process lifetime with `RTLD_NODELETE`. Repeated
encoder creation reuses those globals; each encoder and model instance is still
destroyed normally. This lifetime policy avoids orphaning backend allocations
when the module would otherwise unload, without a leak-report suppression.

Word lists in `../swipe/lexicons/` are derived from CC BY lists (Leipzig Corpora
Collection, partly via HeliBoard) blended with CC BY-SA 4.0 subtitle frequencies
(FrequencyWords, from OpenSubtitles 2018), so those lists are CC BY-SA 4.0. The
Filipino list has no subtitles blended in and stays CC BY 4.0; the Azerbaijani
list comes from a CC BY-SA 4.0 list; the Chinese Pinyin list comes from jieba
and pypinyin under MIT. See their README
and licence files for sources, hashes and changes.

Japanese conversion dynamically uses the system Anthy public C API (`anthy.h`, 9100h). Anthy and its dictionary are not vendored. Its commit and prediction APIs learn under Full Keyboard's own personality (docs/japanese.md, Learning). Only the desktop simulator's AppImage can carry a copy of Anthy; `anthy/` records which and its licenses.

Japanese conversion uses Mozc's server, `mozc_server` from Mozc 3.35.6354.2, shipped unchanged as `lib/framekeyboard/framekeyboard-mozc` (docs/japanese.md, Mozc). It is taken from the fcitx5-mozc 3.35.6354.2-1 package, Arch Linux ARM's aarch64 build for the Frame and Arch Linux's x86_64 one for host builds, each pinned by SHA-256 in `tools/fetch-mozc.sh` after its signature was checked (docs/building.md). `mozc/LICENSE` and `mozc/credits_en.html` hold its notices: Mozc (BSD 3-Clause) and the libraries and dictionaries built into it, among them Abseil (Apache 2.0), Protocol Buffers (BSD 3-Clause), IPAdic and the Okinawa dictionary.

The Japanese common-word list, `japanese/words.tsv`, and kaomoji list, `japanese/kaomoji.tsv`, are derived from Mozc's open-source dictionary and emoticon data (BSD 3-Clause; IPAdic, ICOT Free Software and the public-domain Okinawa dictionary). `mozc/LICENSE` holds those notices; `japanese/README.md` the pinned commit, file hashes and how it is built.

The Chinese engines are libime with OpenCC, and McBopomofo's engine. `tools/build-chinese.sh` builds them, with their dictionaries and the libraries they need, from the revisions pinned in `tools/chinese-pins.sh`. Their sources are not vendored. `chinese-engines/` holds their licences and lists what each one covers: LGPL-2.1-or-later for libime, KenLM and fcitx5's utility library, which stay shared libraries. The rest is MIT, Apache-2.0, BSD and BSL-1.0.

Cangjie and Quick order a code's characters by `chinese/char-ranks.txt`. It is derived from McBopomofo's phrase data (MIT) and rime-cantonese's Cantonese frequencies (CC BY 4.0) by `tools/build_chinese_ranks.py`. `chinese-ranks/` holds both notices and the attribution; they ship with every build, as the list does.

Korean composition links the installed libhangul public C API, under
LGPL-2.1-or-later. Frame builds use its system library. The simulator AppImage
can bundle the build host's copy, with package details and notices recorded in
its `usr/share/licenses/`. The adapter keeps no typing history.
API reference: https://libhangul.github.io/libhangul-doc/git/group__hangulic.html.

## IBus dictionaries and tables

`ibus/` records the notices for libibus and the Japanese emoji dictionary,
read from SteamOS by the keyboard. Only the desktop simulator's AppImage
bundles them, unchanged, from Arch Linux's `libibus` and `ibus` 1.5.32-1
packages. libibus is LGPL-2.1-or-later; the dictionary's CLDR annotations use
the Unicode License. See `ibus/README.md`, its adjacent `COPYING`, and
`unicode/LICENSE` for sources and licenses.

`ibus-table-chinese/` records the notices for the Cangjie and Quick tables,
read from SteamOS by the keyboard. Only the simulator's AppImage bundles
`cangjie5.db` and `quick5.db`, unchanged, from Arch Linux's
`ibus-table-chinese` 1.8.14-1 package under GPL-3.0. Its README names the
source release; the adjacent `COPYING` holds the license.

The AppImage's input package versions, archive SHA-256 values and bundled
file SHA-256 values are pinned in
`scripts/simulator-appimage/{anthy,ibus,ibus-table-chinese}.sha256`.
Packaging checks the unpacked files against those hashes before building.

## Emoji data

`../emoji/emoji.tsv` is generated by `tools/emoji_table.py` from Unicode's
`emoji-test.txt` version 17.0 (https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt,
SHA-256 `1d8a944f88d7952f7ef7c5167fef3c67995bcae24543949710231b03a201acda`). It
keeps the fully-qualified emoji, their CLDR order and groups, and their
single-tone skin variants. The source file is not vendored. `unicode/LICENSE`
is the Unicode License v3 that covers it. Emoji images come from the system's
color emoji font, which is not redistributed.

## On-device dictation

The optional `framekeyboard-whisper` engine statically links whisper.cpp and
its ggml library at v1.9.5 (commit `d1be6fde11ac6e0407606b4e42fe72d34add8037`,
MIT; `whisper.cpp/LICENSE`). `tools/build-whisper.sh` builds it from that pinned
checkout for the CPU only; the source is not vendored. whisper.cpp's own
`whisper-quantize` tool from the same checkout is installed beside it for the
development 4-bit model. The Whisper model is not bundled: the user downloads
it from Settings (docs/dictation.md, On-device recognition).
