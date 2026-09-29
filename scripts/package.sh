#!/usr/bin/env bash
# Run on the development host. Nothing is installed on the Frame by this script.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
./scripts/build.sh frame-arm64
version=$(sed -n 's/^project(framekeyboard VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)
package_dir="$project_dir/out/framekeyboard-$version-aarch64"
mkdir -p "$package_dir"
cmake --install build/frame-arm64 --prefix "$package_dir"
cp scripts/install-local.sh "$package_dir/install.sh"
cp README.md "$package_dir/README.md"
tar -C out -czf "out/framekeyboard-$version-aarch64.tar.gz" "framekeyboard-$version-aarch64"
sha256sum "out/framekeyboard-$version-aarch64.tar.gz"
