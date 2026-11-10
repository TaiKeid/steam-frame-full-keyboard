#!/usr/bin/env bash
# Fetch the pinned sources of the Chinese conversion engines into SOURCE_DIR.
# Run on the development host. The sources stay outside Git.
#
#   tools/fetch-chinese.sh SOURCE_DIR
#
# Existing checkouts are checked, not updated: a checkout at another commit is
# an error. Needs git, curl, tar with xz and sha256sum.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/chinese-pins.sh
source "$project_dir/tools/chinese-pins.sh"
mkdir -p -- "${1:?source}"
source_dir=$(cd -- "$1" && pwd)

check_commit() { # DIR COMMIT
    if [[ $(git -C "$1" rev-parse HEAD) != "$2" ]]; then
        echo "$1 is not at pinned commit $2." >&2
        exit 1
    fi
}

fetch_git() { # NAME
    local name=$1 url tag commit dir
    url=${name}_URL tag=${name}_TAG commit=${name}_COMMIT
    dir="$source_dir/$name"
    if [[ ! -d $dir ]]; then
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "${!tag}" "${!url}" "$dir"
    fi
    check_commit "$dir" "${!commit}"
    local submodules=${name}_SUBMODULES entry path pinned
    for entry in ${!submodules-}; do
        path=${entry%%=*} pinned=${entry#*=}
        if [[ ! -e $dir/$path/.git ]]; then
            git -C "$dir" submodule update --quiet --init --depth 1 -- "$path" ||
                git -C "$dir" submodule update --quiet --init -- "$path"
        fi
        check_commit "$dir/$path" "$pinned"
    done
    echo "$name ${!tag} at ${!commit}"
}

for name in "${chinese_git_sources[@]}"; do
    fetch_git "$name"
done

# Boost's source release is a tarball; check it before unpacking.
archive="$source_dir/${boost_URL##*/}"
if [[ ! -d $source_dir/$boost_DIR ]]; then
    [[ -f $archive ]] || curl -fL --proto '=https' -o "$archive.part" "$boost_URL"
    [[ -f $archive ]] || mv -- "$archive.part" "$archive"
    echo "$boost_SHA256  $archive" | sha256sum --check --quiet
    tar -xJf "$archive" -C "$source_dir"
    rm -- "$archive"
fi
echo "Boost $boost_DIR"
