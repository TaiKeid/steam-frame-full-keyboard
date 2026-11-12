#!/usr/bin/env bash
# Run on the development host. Nothing is installed on the Frame by this script.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
# The installer is built last; check its tools before the long build.
: "${FRAMEKEYBOARD_APPIMAGETOOL:?set FRAMEKEYBOARD_APPIMAGETOOL (docs/installer.md#appimage)}"
: "${FRAMEKEYBOARD_APPIMAGE_RUNTIME:?set FRAMEKEYBOARD_APPIMAGE_RUNTIME (docs/installer.md#appimage)}"
# Its own build tree, with debug mode off: a release has no Debug card and
# never reads --debug. Development builds of the frame-arm64 preset keep it.
./scripts/build.sh frame-arm64-release
version=$(sed -n 's/^project(framekeyboard VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)
package_dir="$project_dir/out/framekeyboard-$version-aarch64"
# A rebuild must not retain resources removed from the install rules.
rm -rf -- "$package_dir"
mkdir -p "$package_dir"
cmake --install build/frame-arm64-release --prefix "$package_dir" --strip
if grep -q -a -e 'Display key collision' -e 'Mark Anthy suggestions' "$package_dir/bin/framekeyboard"; then
    echo 'The release contains debug mode: build it with FRAMEKEYBOARD_DEBUG_MODE=OFF.' >&2
    exit 1
fi
python3 tools/check_release_binary.py "$package_dir/bin/framekeyboard" \
    "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
# The streaming client loads this library in place of its OpenVR library, so
# a wrong dependency or a stray symbol would reach Steam's own process.
stream_library="$package_dir/lib/framekeyboard/framekeyboard-stream.so"
python3 tools/check_release_binary.py "$stream_library" "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
python3 tools/check_stream_library.py "$stream_library"
# The retained adapters are release assets even though their system dependencies
# are optional at runtime. Check stripping and path hygiene for them too.
for engine in libime mcbopomofo hangul; do
    python3 tools/check_release_binary.py "$package_dir/lib/framekeyboard/framekeyboard-$engine.so" \
        "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
done
# The swipe engine is optional: builds without FRAMEKEYBOARD_EXECUTORCH_FRAME omit it.
swipe_engine="$package_dir/lib/framekeyboard/framekeyboard-swipe.so"
if [[ -f $swipe_engine ]]; then
    python3 tools/check_release_binary.py "$swipe_engine" "$project_dir" "$FRAMEKEYBOARD_SYSROOT" \
        "$FRAMEKEYBOARD_EXECUTORCH_FRAME"
    test -f "$package_dir/share/framekeyboard/swipe/futo-swipe/model_fp32.pte"
else
    echo 'Packaging without the swipe engine: swipe typing will stay disabled.' >&2
fi
# On-device dictation is the default, so a release must carry its engine.
for engine in framekeyboard-whisper whisper-quantize; do
    if [[ ! -f $package_dir/lib/framekeyboard/$engine ]]; then
        echo "Missing $engine: set FRAMEKEYBOARD_WHISPER_FRAME (docs/building.md)." >&2
        exit 1
    fi
    python3 tools/check_release_binary.py "$package_dir/lib/framekeyboard/$engine" \
        "$project_dir" "$FRAMEKEYBOARD_SYSROOT" "$FRAMEKEYBOARD_WHISPER_FRAME"
done
# Japanese converts with Mozc's server; a release without it falls back to
# Anthy for good (docs/japanese.md, Mozc).
if [[ ! -f $package_dir/lib/framekeyboard/framekeyboard-mozc ]]; then
    echo "Missing framekeyboard-mozc: set FRAMEKEYBOARD_MOZC_FRAME (docs/building.md)." >&2
    exit 1
fi
python3 tools/check_release_binary.py "$package_dir/lib/framekeyboard/framekeyboard-mozc" \
    "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
# It is prebuilt, not linked against the sysroot, so check its library
# versions against the Frame's.
python3 tools/check_frame_libraries.py "$package_dir/lib/framekeyboard/framekeyboard-mozc" \
    "$FRAMEKEYBOARD_SYSROOT"
if readelf -d "$package_dir/bin/framekeyboard" | grep -E 'NEEDED.*(libIME|libFcitx5Utils|libhangul)'; then
    echo 'Optional language libraries must not be startup dependencies.' >&2
    exit 1
fi
if readelf -d "$package_dir/bin/framekeyboard" | grep -E 'NEEDED.*libei\.so'; then
    echo 'The release must use its pinned libei client, not the system library.' >&2
    exit 1
fi
cp scripts/install-local.sh "$package_dir/install.sh"
cp scripts/uninstall-local.sh "$package_dir/uninstall.sh"
tar -C out -czf "out/framekeyboard-$version-aarch64.tar.gz" "framekeyboard-$version-aarch64"
(cd out && sha256sum "framekeyboard-$version-aarch64.tar.gz" > SHA256SUMS)
cat out/SHA256SUMS
# The installer ships with every release as an asset of its own
# (docs/installer.md). SHA256SUMS keeps only the archive: the README's
# `sha256sum -c SHA256SUMS` must pass without the installer downloaded.
bash scripts/installer-appimage.sh
installer=Full_Keyboard_installer-aarch64.AppImage
cp "out/installer/$installer" "out/$installer"
(cd out && sha256sum "$installer")
