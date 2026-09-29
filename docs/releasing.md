# Release checklist

The public name is **Full Keyboard for Steam Frame**; the launcher label is **Full Keyboard**. Keep executable names, config paths, overlay identifiers, and package names as `framekeyboard` for compatibility.

## Prepare

- Resolve independent-review findings and update the changelog.
- Set the version in `CMakeLists.txt`; update README archive examples.
- Run host tests and build the ARM64 package from a matching Frame sysroot.
- Run the safe ARM64 core tests on Frame and record the manual checks in [acceptance](acceptance.md). Do not turn untested items into claims.
- Regenerate the [README image](building.md#refresh-the-readme-image) if the default appearance changed.
- Validate a clean install, update, rollback, and uninstall; preserve user settings.
- Check that LICENSE, third-party notices, README, and docs are included in the archive.
- Inspect tracked files and Git history for private data before making the repository public. Do not publish local sysroots, user profiles, logs, backups, or credentials.

## Package and publish

```sh
export FRAMEKEYBOARD_SYSROOT=/absolute/path/to/frame-sysroot
./scripts/package.sh
```

Upload the generated `out/framekeyboard-VERSION-aarch64.tar.gz` and `out/SHA256SUMS` as assets for the matching tag. Use the archive basename from `SHA256SUMS`; do not rename it after hashing. The checksum file covers that release's install archive, not GitHub's generated source archives.

GitHub publication, repository naming, tags, and release visibility are separate deliberate steps. Preparing a local package does not publish it. No automatic upload workflow is configured.

Describe supported on-device use and known limitations in release notes. Link the README for installation and recovery. If the project has not yet completed a check, state that directly.
