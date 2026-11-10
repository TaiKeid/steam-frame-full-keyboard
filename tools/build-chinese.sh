#!/usr/bin/env bash
# Build the pinned Chinese conversion engines into one prefix per target: libime
# for Pinyin, McBopomofo's engine for Zhuyin, OpenCC for Traditional output,
# with the libraries they need. Run on the development
# host. The sources and outputs stay outside Git.
#
#   tools/build-chinese.sh host|frame-arm64 SOURCE_DIR PREFIX
#
# SOURCE_DIR comes from tools/fetch-chinese.sh. frame-arm64 also needs
# FRAMEKEYBOARD_SYSROOT, as for the main build, and FRAMEKEYBOARD_CHINESE_HOST,
# a host prefix built from the same pins: dictionaries and language models are
# built by the engines' own tools, which run only on the host, and their files
# are the same on both 64-bit little-endian targets.
#
# LGPL engines (libime, fcitx5's utility library) are shared libraries, so
# they can be replaced; permissive code (zstd, Boost, McBopomofo,
# OpenCC) is static. Jobs default to 4 (CMAKE_BUILD_PARALLEL_LEVEL).
set -euo pipefail
target=${1:?target}
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/chinese-pins.sh
source "$project_dir/tools/chinese-pins.sh"
source_dir=$(cd -- "${2:?source}" && pwd)
mkdir -p -- "${3:?prefix}"
prefix=$(cd -- "$3" && pwd)
build_root="$prefix.build"
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-4}

for name in "${chinese_git_sources[@]}"; do
    commit=${name}_COMMIT
    if [[ $(git -C "$source_dir/$name" rev-parse HEAD) != "${!commit}" ]]; then
        echo "$source_dir/$name is not at pinned commit ${!commit}; run tools/fetch-chinese.sh." >&2
        exit 1
    fi
done
[[ -f $source_dir/$boost_DIR/CMakeLists.txt ]] || {
    echo "$source_dir/$boost_DIR is missing; run tools/fetch-chinese.sh." >&2
    exit 1
}

mkdir -p -- "$build_root"
toolchain=()
host_prefix=
case "$target" in
host)
    # Only our own prefix answers pkg-config, so libime takes the static zstd
    # built here rather than the host's.
    export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH=
    ;;
frame-arm64)
    host_prefix=$(cd -- "${FRAMEKEYBOARD_CHINESE_HOST:?set FRAMEKEYBOARD_CHINESE_HOST}" && pwd)
    if ! cmp -s "$project_dir/tools/chinese-pins.sh" "$host_prefix/share/framekeyboard/chinese-pins.sh"; then
        echo "$host_prefix was built from other pins; rebuild it first." >&2
        exit 1
    fi
    # The Frame toolchain searches only the sysroot. This prefix is searched as
    # a second root, and pkg-config sees only it, without the sysroot prefix.
    cat >"$build_root/toolchain.cmake" <<EOF
include("$project_dir/cmake/steam-frame-aarch64.cmake")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
list(APPEND CMAKE_FIND_ROOT_PATH "$prefix")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "$prefix/lib/pkgconfig")
EOF
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$build_root/toolchain.cmake")
    ;;
*)
    echo "Unknown target $target" >&2
    exit 1
    ;;
esac

# Keep builder paths out of the engines.
maps="-ffile-prefix-map=$source_dir=chinese -ffile-prefix-map=$build_root=chinese-build"
# The engines compile their install prefix in as default data and plugin paths.
# They are configured for a prefix that exists nowhere, so those paths name no
# build machine and find nothing by accident (the app passes its own), then
# installed into PREFIX. They get no RUNPATH; the app's modules carry their own.
neutral=/framekeyboard-chinese
common=("${toolchain[@]}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$neutral"
    -DCMAKE_SKIP_INSTALL_RPATH=ON -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" -DBUILD_TESTING=OFF)

configure() { # NAME SOURCE ARGS...
    local name=$1 src=$2
    shift 2
    cmake -S "$src" -B "$build_root/$name" "${common[@]}" "$@"
}
build() { # NAME [TARGETS...]
    local name=$1
    shift
    cmake --build "$build_root/$name" --parallel "$jobs" ${1:+--target "$@"}
}
install_to_prefix() { # BUILD_DIR
    # Staged, because some install rules use absolute paths under the prefix.
    local stage="$build_root/stage"
    rm -rf -- "$stage"
    DESTDIR="$stage" cmake --install "$1"
    cp -a -- "$stage$neutral/." "$prefix/"
    rm -rf -- "$stage"
    # Exported targets and pkg-config files name the neutral prefix; make them
    # find the installed copy.
    find "$prefix/lib/cmake" -name '*Targets*.cmake' -exec sed -i "s|$neutral|\${_IMPORT_PREFIX}|g" {} +
    find "$prefix/lib/pkgconfig" -name '*.pc' -exec sed -i "s|$neutral|$prefix|g" {} +
}
# zstd and Boost are only linked, never loaded from their prefix, so they are
# configured for PREFIX itself: zstd's pkg-config file names it.
real_prefix=(-DCMAKE_INSTALL_PREFIX="$prefix")

# zstd, static: libime reads its compressed dictionaries with it.
configure zstd "$source_dir/zstd/build/cmake" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
    -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_LEGACY_SUPPORT=OFF \
    -DZSTD_MULTITHREAD_SUPPORT=OFF "${real_prefix[@]}"
build zstd
cmake --install "$build_root/zstd"

# Boost, static: the header libraries libime includes, and Iostreams without
# compression filters. Program Options is for libime's host data tools.
boost_libraries="iostreams;program_options;bimap;container;container_hash;functional;interprocess"
boost_libraries+=";iterator;lexical_cast;multi_index;optional;ptr_container;random;range;smart_ptr"
boost_libraries+=";thread;throw_exception;tuple;type_traits;unordered;utility"
configure boost "$source_dir/$boost_DIR" -DBUILD_SHARED_LIBS=OFF \
    -DBOOST_INCLUDE_LIBRARIES="$boost_libraries" -DBOOST_IOSTREAMS_ENABLE_ZLIB=OFF \
    -DBOOST_IOSTREAMS_ENABLE_BZIP2=OFF -DBOOST_IOSTREAMS_ENABLE_LZMA=OFF \
    -DBOOST_IOSTREAMS_ENABLE_ZSTD=OFF "${real_prefix[@]}"
build boost
cmake --install "$build_root/boost"
boost_dir=$(dirname -- "$(find "$prefix/lib/cmake" -path '*/Boost-*/BoostConfig.cmake' -print -quit)")

# ECM is only CMake modules; configure it for the host either way.
cmake -S "$source_dir/ecm" -B "$build_root/ecm" -G Ninja -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF
cmake --install "$build_root/ecm"
ecm=(-DECM_DIR="$prefix/share/ECM/cmake")

# fcitx5: only its utility library, without event loop, D-Bus or front ends.
configure fcitx5 "$source_dir/fcitx5" "${ecm[@]}" -DBUILD_SHARED_LIBS=ON \
    -DENABLE_TEST=OFF -DENABLE_TESTING_ADDONS=OFF -DENABLE_ENCHANT=OFF -DENABLE_X11=OFF \
    -DENABLE_WAYLAND=OFF -DENABLE_DBUS=OFF -DENABLE_SERVER=OFF -DENABLE_KEYBOARD=OFF \
    -DENABLE_XDGAUTOSTART=OFF -DENABLE_EMOJI=OFF -DENABLE_LIBUUID=OFF -DBUILD_SPELL_DICT=OFF \
    -DENABLE_DOC=OFF -DEVENT_LOOP_BACKEND=none
build fcitx5 Fcitx5Utils
install_to_prefix "$build_root/fcitx5/src/lib/fcitx-utils"
# Its package file names the neutral prefix's CMake directory, where libime
# looks for fcitx5's compiler settings; point it at the installed copy.
sed -i 's|^set(FCITX_INSTALL_CMAKECONFIG_DIR .*|get_filename_component(FCITX_INSTALL_CMAKECONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)|' \
    "$prefix/lib/cmake/Fcitx5Utils/Fcitx5UtilsConfig.cmake"

# libime. The host build also makes the dictionary and language model.
data=ON
[[ $target == host ]] || data=OFF
configure libime "$source_dir/libime" "${ecm[@]}" -DBUILD_SHARED_LIBS=ON \
    -DFcitx5Utils_DIR="$prefix/lib/cmake/Fcitx5Utils" -DBoost_DIR="$boost_dir" \
    -DENABLE_TEST=OFF -DENABLE_DOC=OFF -DENABLE_DATA=$data -DENABLE_TOOLS=$data
build libime
install_to_prefix "$build_root/libime"
if [[ $data == ON ]]; then
    # The language model again, pruned (libime_LM_PRUNE), as libime builds its
    # own. Its prediction model goes: the keyboard doesn't predict.
    rm -f -- "$prefix/lib/libime/zh_CN.lm.predict"
    if [[ $libime_LM_PRUNE != 0 ]]; then
        pruned="$build_root/lm_sc.pruned.arpa"
        python3 "$project_dir/tools/prune_chinese_lm.py" "$build_root/libime/data/lm_sc.arpa" \
            "$pruned" "$libime_LM_PRUNE"
        LD_LIBRARY_PATH="$prefix/lib" "$prefix/bin/libime_slm_build_binary" -s -a 22 -q 4 trie \
            "$pruned" "$prefix/lib/libime/zh_CN.lm"
        rm -f -- "$pruned"
    fi
fi

# McBopomofo: its engine has no install rules; keep its static libraries,
# headers and plain-text data.
configure mcbopomofo "$source_dir/mcbopomofo/src/Engine" -DENABLE_TEST=OFF
build mcbopomofo
for library in libMcBopomofoLMLib gramambular2/libgramambular2_lib Mandarin/libMandarinLib; do
    install -D -m 644 "$build_root/mcbopomofo/$library.a" -t "$prefix/lib/mcbopomofo"
done
(cd "$source_dir/mcbopomofo/src" && find Engine -name '*.h' -exec install -D -m 644 {} "$prefix/include/mcbopomofo/{}" \;)
for file in data.txt data-plain-bpmf.txt associated-phrases-v2.txt bpmfvs-variants.txt; do
    install -D -m 644 "$source_dir/mcbopomofo/data/$file" -t "$prefix/share/mcbopomofo"
done

# OpenCC, static with its bundled marisa-trie. On the host it also builds its
# dictionaries; on the Frame only the library and its headers.
configure opencc "$source_dir/opencc" -DBUILD_SHARED_LIBS=OFF -DOPENCC_ENABLE_INSTALL=ON \
    -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF -DBUILD_PYTHON=OFF -DBUILD_DOCUMENTATION=OFF
if [[ $target == host ]]; then
    build opencc
    install_to_prefix "$build_root/opencc"
else
    build opencc src/all
    install_to_prefix "$build_root/opencc/src"
fi

if [[ -n $host_prefix ]]; then
    # Data built on the host, from the same pins.
    rm -rf -- "$prefix/share/libime" "$prefix/lib/libime" "$prefix/share/opencc"
    cp -a -- "$host_prefix/share/libime" "$host_prefix/share/opencc" "$prefix/share/"
    cp -a -- "$host_prefix/lib/libime" "$prefix/lib/"
fi
install -D -m 644 "$project_dir/tools/chinese-pins.sh" -t "$prefix/share/framekeyboard"
echo "Chinese engines installed in $prefix"
