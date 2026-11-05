#!/usr/bin/env bash
# Compare unpacked inputs with the pinned packages' payloads before bundling.
set -euo pipefail
kind=${1:?pass the package group}
input_root=${2:?pass its unpacked root}
case $kind in
anthy | ibus | ibus-table-chinese) ;;
*) echo "Unknown AppImage package group: $kind" >&2; exit 1 ;;
esac
manifest=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$kind.sha256
(cd -- "$input_root" && sha256sum --check --strict "$manifest")
