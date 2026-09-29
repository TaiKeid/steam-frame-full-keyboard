#!/usr/bin/env bash
# Run this from an extracted ARM64 package on the Frame. User files are preserved.
set -euo pipefail
package_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -m) != aarch64 ]]; then
    echo 'This package targets the ARM64 Steam Frame.' >&2
    exit 1
fi
version=$("$package_dir/bin/framekeyboard" --version | awk '{print $2}')
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo 'Invalid package version.' >&2
    exit 1
fi
# The override lets integration tests install into an isolated directory.
install_home=${FRAMEKEYBOARD_INSTALL_HOME:-$HOME}
install_root="$install_home/.local/share/framekeyboard"
release="$install_root/releases/$version"
mkdir -p "$install_root/releases" "$install_home/.local/bin" "$install_home/.local/share/applications"
exec 9> "$install_root/install.lock"
flock -n 9 || { echo 'Another installation is running.' >&2; exit 1; }
if [[ -e "$release" || -L "$release" ]]; then
    echo "Release already exists: $release. Keep it for rollback or remove it before reinstalling." >&2
    exit 1
fi
stage=''
launcher_tmp=''
desktop_tmp=''
link_stage=''
release_created=false
activated=false
cleanup() {
    [[ -z $stage ]] || rm -rf -- "$stage"
    [[ -z $launcher_tmp ]] || rm -f -- "$launcher_tmp"
    [[ -z $desktop_tmp ]] || rm -f -- "$desktop_tmp"
    [[ -z $link_stage ]] || rm -rf -- "$link_stage"
    if $release_created && ! $activated; then
        rm -rf -- "$release"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
stage=$(mktemp -d "$install_root/releases/.install-$version.XXXXXX")
cp -a "$package_dir/." "$stage/"
for kind in layouts languages themes; do
    if [[ ! -d $stage/share/framekeyboard/$kind ]]; then
        echo "Missing packaged profiles: $kind" >&2
        exit 1
    fi
done
# Validate the copied JSON, not just the fallback profiles embedded in the binary.
"$stage/bin/framekeyboard" --check --data-dir "$stage/share/framekeyboard" --config-dir "$stage/.validation"
launcher_tmp=$(mktemp "$install_home/.local/bin/.framekeyboard.XXXXXX")
desktop_tmp=$(mktemp "$install_home/.local/share/applications/.framekeyboard.XXXXXX")
cat > "$launcher_tmp" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
install_root="${FRAMEKEYBOARD_INSTALL_HOME:-$HOME}/.local/share/framekeyboard/current"
if [[ $# == 0 || ( $# == 1 && $1 == --vr ) ]]; then
    set -- --vr --input ei --target-language "${FRAMEKEYBOARD_TARGET_LANGUAGE:-en-us}" --start-enabled
fi
exec "$install_root/bin/framekeyboard" --data-dir "$install_root/share/framekeyboard" "$@"
LAUNCHER
chmod 755 "$launcher_tmp"
cat > "$desktop_tmp" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Full Keyboard
Comment=Full-size keyboard for Steam Frame dashboard and local apps
Exec="$install_home/.local/bin/framekeyboard"
Icon=input-keyboard
Terminal=false
Categories=Utility;Accessibility;
DESKTOP
chmod 644 "$desktop_tmp"
# Finish every fallible preparation before switching the active release.
link_stage=$(mktemp -d "$install_root/.links.XXXXXX")
ln -s -- "releases/$version" "$link_stage/current"
if [[ -L "$install_root/current" ]]; then
    ln -s -- "$(readlink "$install_root/current")" "$link_stage/previous"
fi
mv -T -- "$stage" "$release"
stage=''
release_created=true
mv -Tf -- "$launcher_tmp" "$install_home/.local/bin/framekeyboard"
launcher_tmp=''
mv -Tf -- "$desktop_tmp" "$install_home/.local/share/applications/framekeyboard.desktop"
desktop_tmp=''
if [[ -L "$link_stage/previous" ]]; then
    mv -Tf -- "$link_stage/previous" "$install_root/previous"
fi
mv -Tf -- "$link_stage/current" "$install_root/current"
activated=true
printf 'Installed Full Keyboard %s. No autostart or Steam changes.\n' "$version"
