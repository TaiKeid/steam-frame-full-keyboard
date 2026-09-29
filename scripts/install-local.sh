#!/usr/bin/env bash
# Run this from an extracted ARM64 package on the Frame. User files are preserved.
set -euo pipefail
package_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -m) != aarch64 ]]; then
    echo 'This package targets the ARM64 Steam Frame.' >&2
    exit 1
fi
version=$("$package_dir/bin/framekeyboard" --version | awk '{print $2}')
case "$version" in
    ''|*[!0-9.]* ) echo 'Invalid package version.' >&2; exit 1 ;;
esac
install_root="$HOME/.local/share/framekeyboard"
release="$install_root/releases/$version"
if [[ -e "$release" ]]; then
    echo "Release already exists: $release. Keep it for rollback or remove it before reinstalling." >&2
    exit 1
fi
mkdir -p "$install_root/releases" "$HOME/.local/bin" "$HOME/.local/share/applications"
cp -a "$package_dir" "$release"
# Verify the installed resources before making this release the current one.
"$release/bin/framekeyboard" --check --config-dir "$release/.validation"
if [[ -L "$install_root/current" ]]; then
    ln -sfn -- "$(readlink "$install_root/current")" "$install_root/previous"
fi
ln -s -- "releases/$version" "$install_root/current.new"
mv -Tf -- "$install_root/current.new" "$install_root/current"
cat > "$HOME/.local/bin/framekeyboard" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
install_root="$HOME/.local/share/framekeyboard/current"
if [[ $# == 0 || ( $# == 1 && $1 == --vr ) ]]; then
    set -- --vr --input ei --target-language "${FRAMEKEYBOARD_TARGET_LANGUAGE:-en-us}" --start-enabled
fi
exec "$install_root/bin/framekeyboard" --data-dir "$install_root/share/framekeyboard" "$@"
LAUNCHER
chmod +x "$HOME/.local/bin/framekeyboard"
cat > "$HOME/.local/share/applications/framekeyboard.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=FrameKeyboard
Comment=Native VR keyboard with laser dragging
Exec=$HOME/.local/bin/framekeyboard
Icon=input-keyboard
Terminal=false
Categories=Utility;Accessibility;
DESKTOP
printf 'Installed FrameKeyboard %s. No autostart or Steam changes.\n' "$version"
