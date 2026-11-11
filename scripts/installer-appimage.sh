#!/usr/bin/env bash
# Builds the installer as an ARM64 AppImage on the development host
# (docs/installer.md#appimage). Nothing is installed or published. The image
# holds only the installer and update_helper.py: it downloads the release it
# installs, and links the Frame's own SDL, Cairo and Pango.
#
#   FRAMEKEYBOARD_SYSROOT           the stable Frame sysroot, as for packages
#   FRAMEKEYBOARD_APPIMAGETOOL      appimagetool to pack with
#   FRAMEKEYBOARD_APPIMAGE_RUNTIME  the aarch64 type2 runtime it puts in front
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
sysroot=${FRAMEKEYBOARD_SYSROOT:?set FRAMEKEYBOARD_SYSROOT (docs/building.md)}
tool=${FRAMEKEYBOARD_APPIMAGETOOL:?set FRAMEKEYBOARD_APPIMAGETOOL (docs/installer.md#appimage)}
runtime=${FRAMEKEYBOARD_APPIMAGE_RUNTIME:?set FRAMEKEYBOARD_APPIMAGE_RUNTIME (docs/installer.md#appimage)}
preset=frame-arm64-release
cmake --preset "$preset" > /dev/null
cmake --build "build/$preset" --target framekeyboard-installer -j "${FRAMEKEYBOARD_JOBS:-4}"
binary="build/$preset/framekeyboard-installer"
readelf -h "$binary" | grep -q 'Machine:.*AArch64' || { echo 'Expected an AArch64 installer.' >&2; exit 1; }
# Every library it needs must be the Frame's own: none is bundled. The sysroot
# keeps some only under their linker names, so match SONAMEs, not file names.
declare -A sonames=()
for file in "$sysroot"/usr/lib/lib*.so* "$sysroot"/usr/lib/ld-*.so*; do
    # Linker scripts such as libc.so aren't ELF files and have none.
    soname=$(readelf -d "$file" 2> /dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p' || true)
    if [[ -n $soname ]]; then
        sonames[$soname]=1
    fi
done
missing=0
while read -r library; do
    if [[ -z ${sonames[$library]:-} ]]; then
        echo "Not in the Frame sysroot: $library" >&2
        missing=1
    fi
done < <(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
(( missing == 0 )) || exit 1

out=out/installer
appdir="$out/AppDir"
# No version in the name: README buttons link to it through GitHub's
# releases/latest/download redirect, which needs the same name every release.
image="$out/Full_Keyboard_installer-aarch64.AppImage"
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/framekeyboard/update" "$appdir/usr/share/doc/framekeyboard"
llvm-strip -o "$appdir/usr/bin/framekeyboard-installer" "$binary"
install -m 644 update/update_helper.py "$appdir/usr/share/framekeyboard/update/"
install -m 644 LICENSE "$appdir/usr/share/doc/framekeyboard/"
# The runtime in front of the image is the only code in it that isn't ours.
install -D -m 644 third_party/appimage-runtime/LICENSE \
    "$appdir/usr/share/doc/framekeyboard/appimage-runtime/LICENSE"
ln -s usr/bin/framekeyboard-installer "$appdir/AppRun"
cp scripts/installer-appimage/framekeyboard-installer.desktop \
    scripts/installer-appimage/framekeyboard-installer.svg "$appdir/"
ln -s framekeyboard-installer.svg "$appdir/.DirIcon"
rm -f -- "$image"
ARCH=aarch64 "$tool" --no-appstream --runtime-file "$runtime" "$appdir" "$image"
(cd "$out" && sha256sum "${image##*/}" > SHA256SUMS)
cat "$out/SHA256SUMS"
