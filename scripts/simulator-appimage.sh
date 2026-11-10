#!/usr/bin/env bash
# Builds the desktop simulator as an x86-64 AppImage on the development host
# (docs/simulator.md#appimage). Nothing is installed or published. The image
# carries this host's libraries, so it runs on systems at least as new.
#
#   FRAMEKEYBOARD_APPIMAGETOOL      appimagetool to pack with
#   FRAMEKEYBOARD_APPIMAGE_RUNTIME  the type2 runtime it puts in front
#   FRAMEKEYBOARD_ANTHY_ROOT        optional: an unpacked Anthy package
#                                   (usr/lib, usr/share/anthy) to bundle
#   FRAMEKEYBOARD_IBUS_ROOT         optional: unpacked libibus and ibus packages
#                                   (usr/lib, usr/share/ibus/dicts), for emoji
#   FRAMEKEYBOARD_IBUS_TABLE_ROOT   optional: an unpacked ibus-table-chinese
#                                   package (usr/share/ibus-table/tables), for
#                                   Cangjie and Quick
#   FRAMEKEYBOARD_EXECUTORCH_HOST   optional: includes swipe typing, as for
#                                   the host build (docs/building.md)
#   FRAMEKEYBOARD_MOZC_HOST         optional: Mozc's server, so Japanese
#                                   converts with Mozc (docs/building.md)
#   FRAMEKEYBOARD_CHINESE_HOST      optional: the Chinese engines' host
#                                   prefix, so Pinyin and Zhuyin convert with
#                                   libime and McBopomofo
#                                   (docs/building.md)
set -euo pipefail
shopt -s nullglob
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
tool=${FRAMEKEYBOARD_APPIMAGETOOL:?set FRAMEKEYBOARD_APPIMAGETOOL (docs/simulator.md#appimage)}
runtime=${FRAMEKEYBOARD_APPIMAGE_RUNTIME:?set FRAMEKEYBOARD_APPIMAGE_RUNTIME (docs/simulator.md#appimage)}
# Package versions, archive hashes and exact bundled payload hashes live in
# scripts/simulator-appimage/{anthy,ibus,ibus-table-chinese}.sha256.
anthy=${FRAMEKEYBOARD_ANTHY_ROOT:-}
ibus=${FRAMEKEYBOARD_IBUS_ROOT:-}
tables=${FRAMEKEYBOARD_IBUS_TABLE_ROOT:-}
if [[ -n $anthy ]]; then
    bash scripts/simulator-appimage/verify-inputs.sh anthy "$anthy"
fi
if [[ -n $ibus ]]; then
    bash scripts/simulator-appimage/verify-inputs.sh ibus "$ibus"
fi
if [[ -n $tables ]]; then
    bash scripts/simulator-appimage/verify-inputs.sh ibus-table-chinese "$tables"
fi

cmake --preset simulator
cmake --build --preset simulator
build=build/simulator
source "$build/simulator-engines.conf"
version=$(sed -n 's/^project(framekeyboard VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)
out=out/simulator
appdir=$out/FullKeyboardSimulator.AppDir
image=$out/Full_Keyboard_simulator-$version-x86_64.AppImage
# A rebuild must not keep anything an earlier one bundled.
rm -rf -- "$appdir" "$image"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib/framekeyboard" "$appdir/usr/share/licenses"

install -m 755 "$build/framekeyboard-simulator" "$appdir/usr/bin/"
strip "$appdir/usr/bin/framekeyboard-simulator"
# Composition engines and swipe typing load from lib/framekeyboard beside bin.
for engine in "$build"/engines/*.so; do
    if [[ ${engine##*/} == framekeyboard-swipe.so && $pack_swipe == 0 ]]; then
        continue
    fi
    case ${engine##*/} in
    framekeyboard-libime.so | framekeyboard-mcbopomofo.so)
        [[ $pack_chinese == 1 ]] || continue
        ;;
    esac
    install -m 644 "$engine" "$appdir/usr/lib/framekeyboard/"
    strip --strip-unneeded "$appdir/usr/lib/framekeyboard/${engine##*/}"
done
if [[ $pack_swipe == 1 ]]; then
    mkdir -p "$appdir/usr/share/framekeyboard/swipe/futo-swipe"
    cp "$build/swipe/futo-swipe/model_fp32.pte" "$appdir/usr/share/framekeyboard/swipe/futo-swipe/"
    cp -r swipe/lexicons "$appdir/usr/share/framekeyboard/swipe/" # with their sources and licenses
else
    echo 'Packing without the swipe engine: swipe typing will stay off.' >&2
fi
# The Chinese engines' libraries and data sit in chinese/ beside their modules;
# the build tree links them to the prefix, so they are copied as files.
if [[ $pack_chinese == 1 ]]; then
    cp -rL "$build/engines/chinese" "$appdir/usr/lib/framekeyboard/"
    for library in "$appdir"/usr/lib/framekeyboard/chinese/*.so.*; do
        strip --strip-unneeded "$library"
    done
else
    echo 'Packing without the new Chinese engines: Pinyin and Zhuyin convert with PyZy where it is installed.' >&2
fi
# Japanese converts with Mozc's server where the build has it, Anthy otherwise.
if [[ $pack_mozc == 1 ]]; then
    install -m 755 "$build/engines/framekeyboard-mozc" "$appdir/usr/lib/framekeyboard/"
else
    echo 'Packing without Mozc: Japanese converts with Anthy alone.' >&2
fi
# Cangjie's and Quick's character order, built in like the tables' engine.
mkdir -p "$appdir/usr/share/framekeyboard/chinese"
cp chinese/char-ranks.txt chinese/README.md "$appdir/usr/share/framekeyboard/chinese/"
# Japanese completion's common words, with their sources.
mkdir -p "$appdir/usr/share/framekeyboard/japanese"
cp japanese/words.tsv japanese/kaomoji.tsv japanese/preferred.tsv japanese/mozc-words.tsv \
    japanese/README.md \
    "$appdir/usr/share/framekeyboard/japanese/"
install -m 755 scripts/simulator-appimage/AppRun "$appdir/AppRun"
cp scripts/simulator-appimage/framekeyboard-simulator.desktop scripts/simulator-appimage/framekeyboard-simulator.svg \
    "$appdir/"
ln -s framekeyboard-simulator.svg "$appdir/.DirIcon"

# The system keeps its C/C++ runtimes, graphics stack and display connections.
# Child programs and graphics plugins inherit LD_LIBRARY_PATH: bundling older
# libstdc++ or libgcc_s would mask the host runtimes they were built against.
system_library() {
    case $1 in
    ld-linux* | libc.so* | libm.so* | libmvec.so* | libdl.so* | librt.so* | libpthread.so* | \
        libresolv.so* | libutil.so* | libstdc++.so* | libgcc_s.so*) return 0 ;;
    libGL* | libEGL* | libOpenGL* | libgbm* | libdrm* | libvulkan* | libwayland-* | libX* | libxcb* | \
        libdecor*) return 0 ;;
    libasound* | libpulse* | libpipewire* | libdbus-* | libsystemd* | libudev*) return 0 ;;
    esac
    return 1
}
bundled=()
bundle_libraries() {
    local file name arrow path
    for file in "$@"; do
        while read -r name arrow path _; do
            [[ $arrow == '=>' && $path == /* ]] || continue
            name=${name##*/} # the loader shows as a path
            # Libraries the image already carries, such as the Chinese engines' own.
            if system_library "$name" || [[ -e $appdir/usr/lib/$name || $path == "$project_dir/$appdir"/* ]]; then
                continue
            fi
            cp -L "$path" "$appdir/usr/lib/$name"
            bundled+=("$path")
        done < <(ldd "$file")
    done
}
roots=("$appdir/usr/bin/framekeyboard-simulator" "$appdir"/usr/lib/framekeyboard/*.so)
bundle_libraries "${roots[@]}"
# sdl2-compat opens SDL 3 by name at run time, which ldd cannot see.
if grep -aq 'libSDL3\.so\.0' "$appdir/usr/lib/libSDL2-2.0.so.0" 2>/dev/null; then
    # awk reads to the end: leaving early would stop ldconfig with SIGPIPE,
    # which pipefail turns into a failed build.
    sdl3=$(ldconfig -p | awk '$1 == "libSDL3.so.0" && !found { print $NF; found = 1 }')
    cp -L "$sdl3" "$appdir/usr/lib/libSDL3.so.0"
    bundled+=("$sdl3")
    bundle_libraries "$appdir/usr/lib/libSDL3.so.0"
fi

# Our own license and those of the code compiled into the simulator.
mkdir -p "$appdir/usr/share/licenses/framekeyboard/third_party"
cp LICENSE third_party/README.md "$appdir/usr/share/licenses/framekeyboard/"
cp -r third_party/libei third_party/gamescope third_party/wlr-protocols third_party/wayland-protocols \
    third_party/unicode third_party/mozc third_party/chinese-ranks \
    "$appdir/usr/share/licenses/framekeyboard/third_party/"
if [[ $pack_chinese == 1 ]]; then
    cp -r third_party/chinese-engines "$appdir/usr/share/licenses/framekeyboard/third_party/"
fi
if [[ -f $appdir/usr/lib/framekeyboard/framekeyboard-swipe.so ]]; then
    cp -r third_party/futo-swipe third_party/executorch "$appdir/usr/share/licenses/framekeyboard/third_party/"
    rm -f "$appdir/usr/share/licenses/framekeyboard/third_party/futo-swipe/model_fp32.pte"
fi
if [[ -n $anthy ]]; then
    # Unchanged from the package, with its licenses (third_party/anthy).
    cp -L "$anthy/usr/lib/libanthy.so.0" "$anthy/usr/lib/libanthydic.so.0" "$appdir/usr/lib/"
    mkdir -p "$appdir/usr/share/anthy"
    cp "$anthy/usr/share/anthy/anthy.dic" "$appdir/usr/share/anthy/"
    cp -r third_party/anthy "$appdir/usr/share/licenses/"
fi
if [[ -n $ibus ]]; then
    # The Frame has both; the simulator brings them for emoji candidates.
    cp -L "$ibus/usr/lib/libibus-1.0.so.5" "$appdir/usr/lib/"
    mkdir -p "$appdir/usr/share/ibus/dicts"
    cp "$ibus/usr/share/ibus/dicts/emoji-ja.dict" "$appdir/usr/share/ibus/dicts/"
    cp -r third_party/ibus "$appdir/usr/share/licenses/"
fi
if [[ -n $tables ]]; then
    # The Frame has them; the simulator brings the two it reads.
    mkdir -p "$appdir/usr/share/ibus-table/tables"
    cp "$tables/usr/share/ibus-table/tables/cangjie5.db" "$tables/usr/share/ibus-table/tables/quick5.db" \
        "$appdir/usr/share/ibus-table/tables/"
    cp -r third_party/ibus-table-chinese "$appdir/usr/share/licenses/"
fi
# Each bundled library's package, version and license, with the package's
# own license files and the SPDX texts it names.
{
    echo "Libraries bundled from the build host, with their packages and licenses."
    echo "Their sources are those of the named package versions."
    echo
    for path in "${bundled[@]}"; do
        package=$(pacman -Qqo "$path" 2>/dev/null || echo unknown)
        if [[ $package == unknown ]]; then
            echo "${path##*/}: package unknown"
            continue
        fi
        info=$(LC_ALL=C pacman -Qi "$package")
        echo "${path##*/}: $package $(awk -F' : ' '/^Version/ { print $2 }' <<<"$info"), \
$(awk -F' : ' '/^Licenses/ { print $2 }' <<<"$info")"
        if [[ -d /usr/share/licenses/$package && ! -e $appdir/usr/share/licenses/$package ]]; then
            cp -rL "/usr/share/licenses/$package" "$appdir/usr/share/licenses/"
        fi
        for id in $(awk -F' : ' '/^Licenses/ { print $2 }' <<<"$info"); do
            if [[ -f /usr/share/licenses/spdx/$id.txt ]]; then
                mkdir -p "$appdir/usr/share/licenses/spdx"
                cp "/usr/share/licenses/spdx/$id.txt" "$appdir/usr/share/licenses/spdx/"
            fi
        done
    done
} >"$appdir/usr/share/licenses/BUNDLED.txt"

# Our binaries must not carry this machine's paths.
for binary in "$appdir/usr/bin/framekeyboard-simulator" "$appdir"/usr/lib/framekeyboard/*.so; do
    for path in "$project_dir" "$HOME"; do
        if grep -aqF "$path" "$binary"; then
            echo "${binary#"$appdir"/} contains a build-machine path: $path" >&2
            exit 1
        fi
    done
done

ARCH=x86_64 "$tool" --no-appstream --runtime-file "$runtime" "$appdir" "$image"
(cd "$out" && sha256sum "${image##*/}" >SHA256SUMS)
cat "$out/SHA256SUMS"
# The newest C library version anything in it asks for.
glibc=$(for binary in "$appdir"/usr/bin/* "$appdir"/usr/lib/*.so* "$appdir"/usr/lib/framekeyboard/*; do
    objdump -T "$binary"
done 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)
echo "It needs glibc ${glibc#GLIBC_} or newer, and a CPU this machine's libraries were built for."
