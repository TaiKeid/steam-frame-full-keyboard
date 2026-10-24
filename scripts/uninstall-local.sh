#!/usr/bin/env bash
# Remove Full Keyboard from this user account on the Frame: the app, its
# helpers, its launcher and menu entry, and the Start with SteamOS service.
# Settings and custom profiles stay unless --purge is given. Steam files are
# never changed by Full Keyboard, so there is nothing to restore there.
set -euo pipefail
usage() {
    echo 'Usage: bash uninstall.sh [--purge] [--yes]' >&2
    echo '  --purge  also delete settings, keyboard placement and custom profiles' >&2
    echo '  --yes    do not ask for confirmation' >&2
}
purge=false
confirmed=false
for argument in "$@"; do
    case $argument in
    --purge) purge=true ;;
    --yes | -y) confirmed=true ;;
    --help | -h) usage; exit 0 ;;
    *) usage; exit 1 ;;
    esac
done
# The same overrides as the installer let integration tests use an isolated home.
install_home=${FRAMEKEYBOARD_INSTALL_HOME:-$HOME}
if [[ -n ${FRAMEKEYBOARD_INSTALL_HOME:-} ]]; then
    config_home="$install_home/.config"
elif [[ ${XDG_CONFIG_HOME:-} == /* ]]; then
    config_home=$XDG_CONFIG_HOME # The app follows XDG_CONFIG_HOME for settings and its unit.
else
    config_home="$HOME/.config"
fi
install_root="$install_home/.local/share/framekeyboard"
launcher="$install_home/.local/bin/framekeyboard"
desktop="$install_home/.local/share/applications/framekeyboard.desktop"
settings="$config_home/framekeyboard"
unit_dir="$config_home/systemd/user"
unit_name=framekeyboard-autostart.service
unit="$unit_dir/$unit_name"
unit_links=("$unit_dir/steamvr.service.wants/$unit_name" "$unit_dir/gamescope-session.target.wants/$unit_name")
runtime_base="/run/user/$(id -u)"
[[ -d $runtime_base ]] || runtime_base=${XDG_RUNTIME_DIR:-}
runtime_dir=${FRAMEKEYBOARD_INSTALL_RUNTIME_DIR:-${runtime_base:+$runtime_base/framekeyboard}}
# Tests run without the user's service manager.
use_systemctl=true
if [[ ${FRAMEKEYBOARD_UNINSTALL_SYSTEMCTL:-1} == 0 ]] || ! command -v systemctl > /dev/null; then
    use_systemctl=false
fi

for path in "$install_root" "$settings"; do
    if [[ -L $path ]]; then
        echo "$path is a symbolic link; remove this installation manually." >&2
        exit 1
    fi
done
# Only files Full Keyboard created are removed; a same-named file of another
# program is left alone.
ours_unit=false
if [[ -f $unit ]] && [[ $(head -n 1 -- "$unit") == '# Managed by Full Keyboard' ]]; then
    ours_unit=true
fi
ours_launcher=false
if [[ -f $launcher ]] && grep -q '/.local/share/framekeyboard/current' -- "$launcher"; then
    ours_launcher=true
fi
ours_desktop=false
if [[ -f $desktop ]] && grep -qx 'Name=Full Keyboard' -- "$desktop"; then
    ours_desktop=true
fi

echo 'This removes Full Keyboard from this user account:'
[[ -e $install_root ]] && echo "  $install_root (the app, its helpers and every installed release)"
$ours_launcher && echo "  $launcher"
$ours_desktop && echo "  $desktop"
$ours_unit && echo "  $unit (Start with SteamOS) and its links"
if $purge; then
    [[ -e $settings ]] && echo "  $settings (settings, placement and custom profiles)"
else
    [[ -e $settings ]] && echo "Settings stay in $settings; add --purge to delete them too."
fi
if [[ ! -e $install_root ]] && ! $ours_launcher && ! $ours_desktop && ! $ours_unit &&
    ! ($purge && [[ -e $settings ]]); then
    echo 'Nothing to remove: Full Keyboard is not installed for this user.'
    exit 0
fi
if ! $confirmed; then
    if [[ ! -t 0 ]]; then
        echo 'Run with --yes to uninstall without a terminal prompt.' >&2
        exit 1
    fi
    read -r -p 'Uninstall Full Keyboard? [y/N] ' answer
    [[ $answer == [yY] || $answer == [yY][eE][sS] ]] || { echo 'Nothing was removed.'; exit 1; }
fi

# Stop the startup service before anything else can restart the app.
if $ours_unit && $use_systemctl; then
    systemctl --user stop "$unit_name" 2> /dev/null || true
fi
# Stop any other running copy, such as one started from the launcher. Its
# normal shutdown gives requests back to Steam's keyboard and removes the
# bridge's hooks from Steam's UI. Only executables inside this installation
# are signalled.
real_root=''
[[ -d $install_root ]] && real_root=$(realpath -e -- "$install_root")
running() {
    [[ -n $real_root ]] || return 0
    local exe pid
    for exe in /proc/[0-9]*/exe; do
        pid=${exe#/proc/}
        pid=${pid%/exe}
        exe=$(readlink -- "$exe" 2> /dev/null) || continue
        exe=${exe% (deleted)}
        [[ $exe == "$real_root"/releases/*/bin/framekeyboard ]] && echo "$pid"
    done
    return 0
}
pids=$(running)
if [[ -n $pids ]]; then
    # shellcheck disable=SC2086 # One PID per word.
    kill -TERM $pids 2> /dev/null || true
fi
# The instance lock is held for the app's whole lifetime: once it can be taken,
# nothing is running. Keep holding it so nothing starts during removal.
if [[ -n $runtime_dir && -d $runtime_dir && ! -L $runtime_dir && -f $runtime_dir/vr.lock &&
      ! -L $runtime_dir/vr.lock ]]; then
    exec 8>> "$runtime_dir/vr.lock"
    stopped=false
    for _ in $(seq 1 50); do
        if flock -n 8; then
            stopped=true
            break
        fi
        sleep 0.2
    done
    if ! $stopped; then
        echo 'Full Keyboard did not stop. Quit it in Settings, then run this again.' >&2
        exit 1
    fi
fi
if [[ -n $real_root ]]; then
    # Wait for an in-app update or another install to finish first.
    exec 9>> "$real_root/install.lock"
    if ! flock -w 10 9; then
        echo 'An installation or update is running. Try again when it has finished.' >&2
        exit 1
    fi
fi

if $ours_unit; then
    for link in "${unit_links[@]}"; do
        if [[ -L $link ]]; then
            rm -f -- "$link"
        fi
    done
    rm -f -- "$unit"
    if $use_systemctl; then
        systemctl --user daemon-reload 2> /dev/null || true
        systemctl --user reset-failed "$unit_name" 2> /dev/null || true
    fi
fi
$ours_launcher && rm -f -- "$launcher"
$ours_desktop && rm -f -- "$desktop"
if [[ -n $real_root ]]; then
    rm -rf -- "$real_root"
fi
if [[ -n $runtime_dir && -d $runtime_dir && ! -L $runtime_dir ]]; then
    rm -f -- "$runtime_dir/vr.lock"
    rmdir -- "$runtime_dir" 2> /dev/null || true
fi
if $purge && [[ -d $settings ]]; then
    rm -rf -- "$settings"
fi
# A recovery helper that reopened Steam's keyboard for an app stays until that
# keyboard closes, then exits by itself. Steam's own keyboard keeps working.
echo 'Full Keyboard is uninstalled. Steam'"'"'s keyboard is used again.'
$purge || [[ ! -e $settings ]] || echo "Your settings remain in $settings."
