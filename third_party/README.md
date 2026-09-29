# Third-party sources

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

Japanese conversion dynamically uses the system Anthy public C API (`anthy.h`, 9100h). Anthy and its dictionary are not vendored. No learning/commit API is called and composition is not written to a user dictionary.
