# FrameKeyboard

A standalone native virtual keyboard for Steam Frame, with a full-size custom layout, real Enter, Ctrl/Alt, and dedicated Copy/Paste keys on the left. The target is local applications, including Brave in KDE and Brave as a standalone floating window.

## Current state

This repository contains the project foundation, the approved HTML visual reference, a validated 106-key layout, a theme specification, and a buildable C++20 command-line bootstrap. **It is not a working VR keyboard yet.** The executable does not open overlays, create input devices, change Steam, or register autostart.

- [Implementation plan](docs/plan.md)
- [Architecture and unresolved integration questions](docs/architecture.md)
- [Acceptance checks](docs/acceptance.md)
- [Visual reference](design/index.html)

## Build on CachyOS

Requirements for this bootstrap: CMake 3.24+, Ninja, a C++20 compiler, and Python 3. Python validates the JSON layout and generates a C++ header at build time; it is not a runtime dependency of the executable.

```sh
./scripts/build.sh host
./build/host/framekeyboard --describe-layout
```

Build for Frame using the existing sibling toolchain and copied Frame sysroot:

```sh
./scripts/build.sh frame-arm64
```

The ARM64 preset expects `../toolchains/steam-frame/aarch64-clang.cmake`. See the toolchain's README for its library snapshot and refresh procedure. For another host/toolchain, use a local, ignored `CMakeUserPresets.json` or configure CMake directly with `-DCMAKE_TOOLCHAIN_FILE=...`.

Host and target builds stay in separate directories. `build/frame-arm64/framekeyboard` is the deployment candidate; the script checks its ELF machine type. Do not execute it on the x86_64 host. Cross-compilation does not establish on-device behavior. No deployment or install script exists yet.

## Visual reference

```sh
./scripts/preview.sh
```

Open `http://127.0.0.1:8767/`. A different port may be supplied as the first argument. The HTML is also self-contained and can be opened directly. Copy/Paste in this reference uses an internal demonstration clipboard. It does not operate the system keyboard or clipboard.

The approved appearance has a flat charcoal surface, grey gradient keycaps, white legends, rounded corners, and shallow raised sides. Keycaps move down at a fixed size when pressed. Preserve the 3.5px side depth and 3px press travel unless the design changes.

## Layout and theme

`layouts/en-us.json` defines the native layout in unscaled design pixels, excluding outer case padding. It has 104 standard keys plus Copy and Paste. IDs distinguish left/right modifiers and main/numpad Enter. Actions use logical names rather than OS scan codes. Backends must explicitly map those names to their own codes.

```sh
python3 tools/compile_layout.py layouts/en-us.json
```

The validator checks text fields, unique IDs, finite positive dimensions, canvas bounds and non-overlapping hit regions. Building embeds the validated layout. Runtime loading of user layouts is planned later. `themes/graphite.json` records the renderer's visual targets and is not consumed by the bootstrap yet.

`design/index.html` is a visual reference snapshot, not generated from the JSON. During native renderer work, compare geometry and behavior against it and document intentional differences.

## Planned runtime

C++ draws the keyboard using Cairo, uploads its image to a Vulkan texture, and displays it through OpenVR. This follows the existing Frame overlay's approach. Input delivery and stock-keyboard integration are isolated adapters. A small runtime JavaScript bridge may be needed to use the existing keyboard button and suppress the stock popup; it would not render our keys.

Keep source and builds on CachyOS. Eventual installation belongs under the Frame user's home directory. Packaged Steam files and the read-only OS are not patched.
