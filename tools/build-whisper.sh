#!/usr/bin/env bash
# Build the pinned whisper.cpp used by the on-device dictation engine. Run on
# the development host. The source checkout and output stay outside Git.
#
#   tools/build-whisper.sh host|frame-arm64 SOURCE_DIR PREFIX
#
# SOURCE_DIR must be a whisper.cpp checkout at the pinned commit. frame-arm64
# also needs FRAMEKEYBOARD_SYSROOT, as for the main build. The result is a
# static libwhisper/ggml for the CPU only, without network fetches or OpenMP,
# and whisper.cpp's own quantize tool, which development builds use to make
# the 4-bit model on the Frame.
set -euo pipefail
pinned=d1be6fde11ac6e0407606b4e42fe72d34add8037 # v1.9.5
target=${1:?target}
source_dir=$(cd -- "${2:?source}" && pwd)
prefix=${3:?prefix}
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(git -C "$source_dir" rev-parse HEAD) != "$pinned" ]]; then
    echo "whisper.cpp checkout is not at pinned commit $pinned." >&2
    exit 1
fi
build_dir="$prefix.build"
target_args=()
case "$target" in
host) ;;
frame-arm64)
    # Every Frame has the same SoC. Without an explicit target ggml uses
    # baseline ARMv8 and skips the dot-product, FP16 and int8 matrix
    # instructions that make 4- and 8-bit models fast; SVE is left out
    # because NEON was faster on the Frame (docs/dictation.md, Why these
    # model choices).
    target_args+=(-DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/steam-frame-aarch64.cmake"
        -DGGML_CPU_ARM_ARCH=armv8.6-a+dotprod+fp16+i8mm)
    ;;
*)
    echo "Unknown target $target" >&2
    exit 1
    ;;
esac
# Keep builder paths out of the engine.
maps="-ffile-prefix-map=$source_dir=whisper.cpp -ffile-prefix-map=$build_dir=whisper.cpp-build"
common=("${target_args[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps"
    -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_BLAS=OFF
    -DGGML_VULKAN=OFF -DGGML_CPU_KLEIDIAI=OFF -DWHISPER_CURL=OFF -DWHISPER_SDL2=OFF
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_SERVER=OFF)
cmake -G Ninja -S "$source_dir" -B "$build_dir" "${common[@]}" -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DWHISPER_BUILD_EXAMPLES=OFF
cmake --build "$build_dir"
cmake --install "$build_dir"
# The quantize tool is an example; its own build directory keeps the other
# examples out of the install.
cmake -G Ninja -S "$source_dir" -B "$build_dir-tools" "${common[@]}" -DWHISPER_BUILD_EXAMPLES=ON
cmake --build "$build_dir-tools" --target whisper-quantize
install -D -m 755 "$build_dir-tools/bin/whisper-quantize" "$prefix/bin/whisper-quantize"
echo "whisper.cpp $pinned installed in $prefix"
