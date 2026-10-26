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
h_head=$(head_commit https://codeberg.org/Helium314/aosp-dictionaries.git)
row "Swipe word lists" "${heliboard:0:12}" "${h_head:0:12}" "$(same "$heliboard" "$h_head")"
u_ver=$(curl -fsS --max-time 20 https://unicode.org/Public/emoji/latest/emoji-test.txt 2>/dev/null |
    grep -m1 -oE '^# Version: [0-9.]+' | cut -d' ' -f3)
row "Unicode emoji" "$emoji" "$u_ver" "$(same "$emoji" "$u_ver")"
echo
echo "CHECK means upstream moved or could not be reached; docs/upkeep.md says what to rerun."
