#!/usr/bin/env bash
# Compare each pinned third-party source with its upstream's latest release
# (docs/upkeep.md). Read-only: it lists remote tags and fetches public
# metadata over HTTPS; it downloads no code and changes nothing here.
#
#   tools/check-upstream.sh
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# The pins, read from the files that own them.
pin() { grep -oE "$2" "$root/$1" | head -n 1; }
whisper=$(pin tools/build-whisper.sh 'pinned=[0-9a-f]{40}' | cut -d= -f2)
executorch=$(pin tools/build-executorch.sh 'pinned=[0-9a-f]{40}' | cut -d= -f2)
libei=$(pin third_party/libei/provenance.json '"version": "[^"]+"' | cut -d'"' -f4)
overlay=$(grep -A1 'frame-perf-overlay' "$root/third_party/README.md" | grep -oE '[0-9a-f]{40}' | head -n 1)
openvr=$(pin third_party/README.md 'OpenVR [0-9.]+ header' | cut -d' ' -f2)
gamescope=$(grep -A1 'gamescope-input-method.xml' "$root/third_party/README.md" | grep -oE '[0-9a-f]{40}' | head -n 1)
models=$(pin dictation/dictation_helper.py 'MODEL_REVISION = "[0-9a-f]{40}"' | cut -d'"' -f2)
mozc=$(pin tools/fetch-mozc.sh 'version=[^ ]+' | cut -d= -f2)
mozc_dictionary=$(pin tools/build_japanese_words.py 'MOZC_COMMIT = "[0-9a-f]{40}"' | cut -d'"' -f2)
mozc_kaomoji=$(pin tools/build_japanese_kaomoji.py 'MOZC_COMMIT = "[0-9a-f]{40}"' | cut -d'"' -f2)
futo=$(pin third_party/futo-swipe/README.md '`[0-9a-f]{40}`' | tr -d '`')
heliboard=$(pin swipe/lexicons/README.md '`[0-9a-f]{40}`' | tr -d '`')
emoji=$(grep -A1 'emoji-test.txt' "$root/third_party/README.md" | grep -oE 'version [0-9.]+' | head -n 1 | cut -d' ' -f2)
futo_file=$(pin third_party/futo-swipe/README.md '`[a-z_]+/model_fp32.pte`' | tr -d '`')
futo_sha=$(grep -A1 'SHA-256' "$root/third_party/futo-swipe/README.md" | grep -oE '[0-9a-f]{64}' | head -n 1)

latest_tag() { # The newest release tag of a Git remote matching a pattern.
    git ls-remote --tags --refs "$1" 2>/dev/null | awk -F/ '{print $NF}' | grep -E "$2" | sort -V | tail -n 1
}
head_commit() { git ls-remote "$1" HEAD 2>/dev/null | cut -c1-40; }
tag_commit() { # The commit a tag points to, through an annotated tag object.
    local refs
    refs=$(git ls-remote "$1" "refs/tags/$2" "refs/tags/$2^{}" 2>/dev/null)
    { grep '\^{}$' <<<"$refs" || cat <<<"$refs"; } | head -n 1 | cut -c1-40
}
hf_revision() { curl -fsS --max-time 20 "https://huggingface.co/api/models/$1" 2>/dev/null |
    grep -oE '"sha":"[0-9a-f]{40}"' | head -n 1 | cut -d'"' -f4; }
hf_file_sha() { # The SHA-256 of one large file at a model repository's main revision.
    curl -fsS --max-time 20 "https://huggingface.co/api/models/$1/tree/main/${2%/*}" 2>/dev/null |
        python3 -I -c 'import json, sys
for entry in json.load(sys.stdin):
    if entry.get("path") == sys.argv[1]:
        print((entry.get("lfs") or {}).get("oid", ""))' "$2"
}
same() { [[ -n "$2" && "$1" == "$2" ]] && echo "current" || echo "CHECK"; }
row() { printf '%-26s %-14s %-14s %s\n' "$1" "$2" "$3" "$4"; }

row "Component" "Pinned" "Upstream" "State"
w_tag=$(latest_tag https://github.com/ggml-org/whisper.cpp.git '^v[0-9]+\.[0-9]+\.[0-9]+$')
row "whisper.cpp" "${whisper:0:12}" "$w_tag" "$(same "$whisper" "$(tag_commit https://github.com/ggml-org/whisper.cpp.git "$w_tag")")"
e_tag=$(latest_tag https://github.com/pytorch/executorch.git '^v[0-9]+\.[0-9]+\.[0-9]+$')
row "ExecuTorch" "${executorch:0:12}" "$e_tag" "$(same "$executorch" "$(tag_commit https://github.com/pytorch/executorch.git "$e_tag")")"
l_tag=$(latest_tag https://gitlab.freedesktop.org/libinput/libei.git '^[0-9]+\.[0-9]+\.[0-9]+$')
row "libei client" "$libei" "$l_tag" "$(same "$libei" "$l_tag")"
o_vr=$(latest_tag https://github.com/ValveSoftware/openvr.git '^v[0-9]+\.[0-9]+\.[0-9]+$')
row "OpenVR header" "$openvr" "$o_vr" "$(same "v$openvr" "$o_vr")"
o_head=$(head_commit https://github.com/sasaken1102r/frame-perf-overlay.git)
row "frame-perf-overlay" "${overlay:0:12}" "${o_head:0:12}" "$(same "$overlay" "$o_head")"
# Only the protocol file matters: compare its upstream copy with ours.
g_file=$(curl -fsS --max-time 20 \
    https://raw.githubusercontent.com/ValveSoftware/gamescope/master/protocol/gamescope-input-method.xml 2>/dev/null |
    sha256sum | cut -c1-12)
g_ours=$(sha256sum "$root/third_party/gamescope/gamescope-input-method.xml" | cut -c1-12)
row "gamescope IME protocol" "${gamescope:0:12}" "file $g_file" "$(same "$g_ours" "$g_file")"
m_rev=$(hf_revision ggerganov/whisper.cpp)
row "Whisper models (HF)" "${models:0:12}" "${m_rev:0:12}" "$(same "$models" "$m_rev")"
# The repository moves with other files; what matters is our encoder file.
f_sha=$(hf_file_sha futo-org/futo-swipe "$futo_file")
row "FUTO Swipe encoder (HF)" "${futo:0:12}" "file ${f_sha:0:7}" "$(same "$futo_sha" "$f_sha")"
# The Frame's server is Arch Linux ARM's build, which can lag Arch Linux's.
z_ver=$(curl -fsS --max-time 20 https://archlinuxarm.org/packages/aarch64/fcitx5-mozc 2>/dev/null |
    grep -m1 -oE '[0-9]+(\.[0-9]+)+-[0-9]+')
row "Mozc server (fcitx5-mozc)" "$mozc" "$z_ver" "$(same "$mozc" "$z_ver")"
z_dictionary_head=$(head_commit https://github.com/google/mozc.git)
z_dictionary_state=$(same "$mozc_dictionary" "$z_dictionary_head")
[[ "$mozc_dictionary" == "$mozc_kaomoji" ]] || z_dictionary_state=CHECK
row "Mozc dictionary and kaomoji" "${mozc_dictionary:0:12}" "${z_dictionary_head:0:12}" "$z_dictionary_state"
h_head=$(head_commit https://codeberg.org/Helium314/aosp-dictionaries.git)
row "Swipe word lists" "${heliboard:0:12}" "${h_head:0:12}" "$(same "$heliboard" "$h_head")"
u_ver=$(curl -fsS --max-time 20 https://unicode.org/Public/emoji/latest/emoji-test.txt 2>/dev/null |
    grep -m1 -oE '^# Version: [0-9.]+' | cut -d' ' -f3)
row "Unicode emoji" "$emoji" "$u_ver" "$(same "$emoji" "$u_ver")"
# Chinese engines: tools/chinese-pins.sh holds a URL, tag and commit for each.
# shellcheck source=tools/chinese-pins.sh
source "$root/tools/chinese-pins.sh"
declare -A chinese_tags=([zstd]='^v[0-9]+\.[0-9]+\.[0-9]+$' [ecm]='^v[0-9]+\.[0-9]+\.[0-9]+$'
    [fcitx5]='^[0-9]+\.[0-9]+\.[0-9]+$' [libime]='^[0-9]+\.[0-9]+\.[0-9]+$'
    [mcbopomofo]='^[0-9]+(\.[0-9]+)+$' [opencc]='^ver\.[0-9]+\.[0-9]+\.[0-9]+$')
for name in "${chinese_git_sources[@]}"; do
    url=${name}_URL tag=${name}_TAG commit=${name}_COMMIT
    c_tag=$(latest_tag "${!url}" "${chinese_tags[$name]}")
    row "$name (Chinese)" "${!tag}" "$c_tag" "$(same "${!commit}" "$(tag_commit "${!url}" "$c_tag")")"
done
ranks_cantonese=$(pin tools/build_chinese_ranks.py 'RIME_CANTONESE_COMMIT = "[0-9a-f]{40}"' | cut -d'"' -f2)
r_head=$(head_commit https://github.com/rime/rime-cantonese.git)
row "rime-cantonese (ranks)" "${ranks_cantonese:0:12}" "${r_head:0:12}" "$(same "$ranks_cantonese" "$r_head")"
b_tag=$(latest_tag https://github.com/boostorg/boost.git '^boost-[0-9]+\.[0-9]+\.[0-9]+$')
row "Boost (Chinese)" "$boost_DIR" "$b_tag" "$(same "$boost_DIR" "$b_tag")"
echo
echo "CHECK means upstream moved or could not be reached; docs/upkeep.md says what to rerun."
