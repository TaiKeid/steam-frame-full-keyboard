# Third-party sources

`overlay_texture/vk_texture.{h,cpp}` comes from
https://github.com/sasaken1102r/frame-perf-overlay at commit
`61c597c4f57624339c222094582f15da32016716`, under its MIT license.
It handles OpenVR-compatible persistent Vulkan textures. Local changes are noted
in comments and Git history. Application behavior lives in our own `src/` files.

`openvr/openvr.h` is the Valve OpenVR 2.15.6 header from that same pinned tree.
The adjacent BSD license was retrieved from Valve's OpenVR repository.
The SDK library is supplied by the installed SteamVR runtime, not redistributed.
