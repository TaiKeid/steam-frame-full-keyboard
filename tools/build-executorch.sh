#!/usr/bin/env bash
# Build the pinned ExecuTorch runtime used by the optional swipe engine module.
# Run on the development host. The source checkout and output stay outside Git.
#
#   tools/build-executorch.sh host|frame-arm64 SOURCE_DIR PREFIX
#
# SOURCE_DIR must be an ExecuTorch checkout named "executorch" at the pinned
# commit with submodules. Kernel registration code generation needs a Python
# with PyTorch; set FRAMEKEYBOARD_EXECUTORCH_PYTHON to it. frame-arm64 also
# needs FRAMEKEYBOARD_SYSROOT, as for the main build.
set -euo pipefail
pinned=3b60683923245cf472b7323426920e15623ba361 # v1.5.1
target=${1:?target}
source_dir=$(cd -- "${2:?source}" && pwd)
prefix=${3:?prefix}
python=${FRAMEKEYBOARD_EXECUTORCH_PYTHON:?set FRAMEKEYBOARD_EXECUTORCH_PYTHON}
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(basename -- "$source_dir") != executorch ]]; then
    echo "ExecuTorch requires its checkout directory to be named executorch." >&2
    exit 1
fi
if [[ $(git -C "$source_dir" rev-parse HEAD) != "$pinned" ]]; then
    echo "ExecuTorch checkout is not at pinned commit $pinned." >&2
    exit 1
fi
build_dir="$prefix.build"
toolchain=()
case "$target" in
host) ;;
frame-arm64)
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/steam-frame-aarch64.cmake")
    ;;
*)
    echo "Unknown target $target" >&2
    exit 1
    ;;
esac
# Keep builder paths out of the release module.
maps="-ffile-prefix-map=$source_dir=executorch -ffile-prefix-map=$build_dir=executorch-build"
cmake -G Ninja -S "$source_dir" -B "$build_dir" "${toolchain[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" -DCMAKE_ASM_FLAGS="$maps" \
    -DPYTHON_EXECUTABLE="$python" \
    -DEXECUTORCH_ENABLE_LOGGING=OFF \
    -DEXECUTORCH_BUILD_XNNPACK=ON \
    -DEXECUTORCH_BUILD_EXTENSION_MODULE=ON -DEXECUTORCH_BUILD_EXTENSION_DATA_LOADER=ON \
    -DEXECUTORCH_BUILD_EXTENSION_FLAT_TENSOR=ON -DEXECUTORCH_BUILD_EXTENSION_NAMED_DATA_MAP=ON \
    -DEXECUTORCH_BUILD_EXTENSION_TENSOR=ON -DEXECUTORCH_BUILD_EXECUTOR_RUNNER=OFF
cmake --build "$build_dir"
cmake --install "$build_dir"
echo "ExecuTorch $pinned installed in $prefix"
