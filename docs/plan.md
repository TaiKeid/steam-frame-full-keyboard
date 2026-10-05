# Roadmap

Full Keyboard for Steam Frame is a manually launched alternative keyboard for local dashboard apps. It does not replace the system keyboard or intercept its summon button. Streamed VR-app input is outside the project scope.

## Implemented

- Native ARM64 overlay, libei key delivery, desktop preview, and image export.
- Full-size US, international, and Japanese layouts with editable JSON profiles.
- European/Cyrillic XKB profiles, Brazilian ABNT2 geometry, integrated Japanese, Simplified/Traditional Pinyin and Korean two-set composition, plus external JIS mode.
- In-VR settings, theme selection, favorites, reload, and persistent configuration.
- Grip movement, same-hand stick depth, resize icons, and release-only horizon alignment.
- Dashboard position/rotation following and dashboard-only visibility/input.
- Saved placement, single-instance launches, and relaunch-to-recenter.
- Per-controller haptics, cancellation paths, host tests, and ARM64 release packaging.
- On-device offline speech-to-text dictation with whisper.cpp and PipeWire audio capture.

## Before the first public release

- Independent code review and resolution of actionable findings.
- Review the [release checklist](releasing.md) and [manual acceptance matrix](acceptance.md).
- Confirm install/update/uninstall instructions from a fresh release archive.
- Review the public repository and package for private files and complete third-party notices.
- Publish a release with the ARM64 archive and checksums once review is complete.

## Further work

- Broader local-app, nested-desktop, and language testing.
- Fluent Japanese-user feedback on composition and candidate controls.
- Broader application testing for Unicode delivery and native shortcuts across system keymaps.
- Sleep/wake, runtime-update, and longer-session testing.
- Measured battery, memory, and latency results.

No stock keyboard takeover, streamed PC input, swipe typing, or cross-session clipboard synchronization is promised. See [changelog](../CHANGELOG.md) for released behavior changes.
