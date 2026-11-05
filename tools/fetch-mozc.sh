#!/usr/bin/env bash
# Fetch the pinned Mozc server used for Japanese conversion. Run on the
# development host. The package and the server stay outside Git.
#
#   tools/fetch-mozc.sh host|frame-arm64 PREFIX
#
# The server is mozc_server from the fcitx5-mozc package: Arch Linux's for
# host and simulator builds, Arch Linux ARM's build of it for the Frame. It
# links only glibc, libstdc++ and libgcc_s. Both package signatures were
# checked when they were pinned, Arch Linux's against its packager's key and
# Arch Linux ARM's against its build system key, which its master keys
# certify; the SHA-256 below pins those files. frame-arm64 with
# FRAMEKEYBOARD_SYSROOT also checks the server against the Frame's libraries.
#
# Arch Linux ARM keeps no archive of old packages. Once it updates
# fcitx5-mozc, this version is left only in PREFIX.download, so keep it.
set -euo pipefail
version=3.35.6354.2-1
target=${1:?target}
prefix=${2:?prefix}
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
case "$target" in
host)
    package=fcitx5-mozc-$version-x86_64.pkg.tar.zst
    url=https://archive.archlinux.org/packages/f/fcitx5-mozc/$package
    sha256=e9751e1a1e7113d4ab8099f03bc4fcf21af134b7c05fc050f312337c15458931
    ;;
frame-arm64)
    # Arch Linux ARM's mirrors serve plain HTTP; the checksum pins the file.
    package=fcitx5-mozc-$version-aarch64.pkg.tar.xz
    url=http://mirror.archlinuxarm.org/aarch64/extra/$package
    sha256=334ecc42bd4de5d4ee341fbc4db8efbc7de645d91aeadc7a37d25ae7ae38be1a
    ;;
*)
    echo "Unknown target $target" >&2
    exit 1
    ;;
esac
download="$prefix.download/$package"
if [[ ! -f $download ]]; then
    mkdir -p "$prefix.download"
    curl -fL --proto '=http,https' -o "$download.part" "$url"
    mv "$download.part" "$download"
fi
if ! sha256sum --check --status <<<"$sha256  $download"; then
    echo "$download does not match the pinned SHA-256 $sha256." >&2
    exit 1
fi
# Only the server and its notices: the package's fcitx5 module and its
# settings tool are not used.
mkdir -p "$prefix"
tar -xf "$download" -C "$prefix" --no-same-owner --strip-components=3 \
    usr/lib/mozc/mozc_server
tar -xf "$download" -C "$prefix" --no-same-owner --strip-components=4 \
    usr/share/doc/mozc/credits_en.html
if ! cmp -s "$prefix/credits_en.html" "$project_dir/third_party/mozc/credits_en.html"; then
    echo "The package's credits_en.html differs from third_party/mozc/credits_en.html;" \
        "update the notices with the pin." >&2
    exit 1
fi
if [[ $target == frame-arm64 && -n ${FRAMEKEYBOARD_SYSROOT:-} ]]; then
    python3 "$project_dir/tools/check_frame_libraries.py" "$prefix/mozc_server" \
        "$FRAMEKEYBOARD_SYSROOT"
fi
echo "Mozc $version server installed as $prefix/mozc_server"
