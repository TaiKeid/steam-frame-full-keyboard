# Release checklist

The public name is **Full Keyboard for Steam Frame**; the launcher label is **Full Keyboard**. Keep executable names, config paths, overlay identifiers, and package names as `framekeyboard` for compatibility.

## Prepare

- Resolve independent-review findings and update the changelog.
- Run the [upkeep checklist](upkeep.md): `tools/check-upstream.sh`, and the private-behavior checks after any SteamOS or Steam update since the last release.
- The project owner sets the version in `CMakeLists.txt` and updates README archive examples. Contributors must leave release versions unchanged; see [version ownership](../CONTRIBUTING.md#version-ownership).
- Run host tests and build the ARM64 package from a matching Frame sysroot.
- Run the safe ARM64 core tests on Frame and record the manual checks in [acceptance](acceptance.md). Do not turn untested items into claims.
- Regenerate the [README image](building.md#refresh-the-readme-image) if the default appearance changed.
- Validate a clean install, update, rollback, and uninstall; preserve user settings.
- Run the release's installer on the Frame from a download: a first install after uninstalling, through its wizard, and a reinstall over the previous release.
- Verify same-version reinstall and downgrade into fresh release directories, refusal while the instance lock is held, deletion only after activation/startup confirmation, and preservation of development/unmarked releases. Manual rollback reinstalls an older package; no permanent `previous` link is maintained. Keep a newer installer available to install packages whose bundled installer predates reinstall support.
- Check upgrade compatibility with the previous release's launcher arguments and environment. In-app updates preserve `~/.local/bin/framekeyboard` and the desktop entry, including user customizations; they do not run the new installer. If a release needs new launcher flags or environment variables, keep older launchers working or provide a migration before offering that release through the updater. A manual install refreshes the generated launcher and desktop entry.
- Verify the new package with the previous release's updater helper, which performs the installation before the new helper can run. Preserve the `framekeyboard-VERSION-aarch64.tar.gz` and `SHA256SUMS` asset names, the `framekeyboard-VERSION-aarch64` archive directory, executable `bin/framekeyboard` and `bin/framekeyboard-native-guard`, and at least one JSON profile in each of `layouts`, `languages` and `themes` under `share/framekeyboard`. Keep `--version` printing `framekeyboard X.Y.Z`, `--check --data-dir PATH --config-dir PATH`, and startup confirmation through `FRAMEKEYBOARD_UPDATE_READY_FD` with a single byte `1` within 45 seconds. Format or protocol changes require an upgrade path the old helper already understands.
- Preserve the `--autostart` flag and both startup preference booleans for installations using Start with SteamOS. Verify a hidden boot start, native summon, manual summon, opt-out and update handoff without killing independent helpers.
- Check that LICENSE, third-party notices, every README and CHANGELOG are in the archive, and that their links into `docs/`, which the archive leaves out, point to the release's tag on GitHub.
- Inspect tracked files and Git history for private data before making the repository public. Do not publish local sysroots, user profiles, logs, backups, or credentials.

## Package and publish

```sh
export FRAMEKEYBOARD_SYSROOT=/absolute/path/to/frame-sysroot
export FRAMEKEYBOARD_EXECUTORCH_FRAME=/absolute/path/to/executorch-arm64
export FRAMEKEYBOARD_WHISPER_FRAME=/absolute/path/to/whisper-arm64
export FRAMEKEYBOARD_MOZC_FRAME=/absolute/path/to/mozc-arm64/mozc_server
export FRAMEKEYBOARD_APPIMAGETOOL=/absolute/path/to/appimagetool
export FRAMEKEYBOARD_APPIMAGE_RUNTIME=/absolute/path/to/runtime-aarch64
./scripts/package.sh
```

The script configures the ARM64 release preset, `frame-arm64-release`, from these variables, so an unset ExecuTorch path leaves swipe typing out of the package. That preset leaves out [debug mode](building.md#debug-mode), and the script refuses an executable that still has it. It also refuses to package without the dictation engine or Mozc's server, and checks the server's library versions against the sysroot; see [building](building.md#dependencies).

Upload the generated `out/framekeyboard-VERSION-aarch64.tar.gz`, `out/SHA256SUMS` and `out/Full_Keyboard_installer-VERSION-aarch64.AppImage` as assets for the matching tag. Every release carries the [installer](installer.md), built last by the same script from the same tree. Use the archive basename from `SHA256SUMS`; do not rename it after hashing. The checksum file covers only that release's install archive: not the installer, so the README's `sha256sum -c SHA256SUMS` passes without it, and not GitHub's generated source archives. Give the installer's SHA-256, which the script prints, in the release notes; GitHub also shows it beside the asset.

GitHub publication, repository naming, tags, and release visibility are separate deliberate steps. Preparing a local package does not publish it. No automatic upload workflow is configured.

Describe supported on-device use and known limitations in release notes. Link the README for installation and recovery. If the project has not yet completed a check, state that directly.
