# Roadmap

Full Keyboard for Steam Frame is a manually launched alternative keyboard for local dashboard apps. Default behavior leaves the system keyboard untouched. An experimental, default-off replacement bridge handles supported native requests while the app runs. Streamed VR-app input is outside the project scope.

## Implemented

- Native ARM64 overlay, libei key delivery, desktop preview, and image export.
- Full-size US, international, and Japanese layouts with editable JSON profiles.
- European/Cyrillic XKB profiles, Brazilian ABNT2 geometry, integrated Japanese, Simplified/Traditional Pinyin and Korean two-set composition, plus external JIS mode.
- In-VR settings, theme selection, favorites, reload, and persistent configuration.
- Manual stable-release checks and verified, staged update/restart with rollback. See [updates](updates.md); future-release headset acceptance remains pending.
- Laser-trigger movement from a line below the keyboard, optional grip movement, same-hand stick depth in either controller mode, laser corner resizers, and optional release-only horizon alignment, enabled by default.
- Dashboard position/rotation following and dashboard-only visibility/input.
- Saved placement, single-instance launches, and relaunch-to-recenter.
- Per-controller haptics, cancellation paths, host tests, and ARM64 release packaging.
- Experimental swipe typing for nine languages, powered by FUTO Swipe (0.9.0). See [swipe typing](swipe-typing.md).
- Japanese 12-key flick input and typing over pinned windows in passthrough (0.9.0).
- Emoji picker with recent emoji, categories and skin tones, and resizing from the corners (0.10.0). See [emoji picker](emoji.md).

## In progress, not yet released

- **Dictation:** on device or with a self-hosted Whisper server, in an editor or typed straight into the focused app. Implemented and tested in the headset; see [dictation](dictation.md). Left before release: an update and sleep while dictating, tested in the headset; the on-device thread count, measured with a game running; independent review and a release build.
- 25 more national layouts, each with dictation, for 37 languages in all.
- Ten more themes, a raised Settings design, and Settings cards that glide and drag.
- The keyboard fades in and out as it opens and closes; the hand feels dictation start and stop.

## Before each release

- Independent code review and resolution of actionable findings.
- The [upkeep checklist](upkeep.md) for third-party code and SteamOS dependencies.
- Review the [release checklist](releasing.md) and [manual acceptance matrix](acceptance.md).
- Confirm install/update/uninstall instructions from a fresh release archive.
- Review the public repository and package for private files and complete third-party notices.

## Experimental native replacement

- [x] Default-off setting, temporary bridge, request readiness and independent native fallback.
- [x] Resident panel dismissal with × and a separate Settings Quit shown only in replacement mode.
- [x] Optional Start with SteamOS, dependent on replacement, with hidden background startup.
- [x] Isolated request, timeout, target-switch, Unicode and cleanup regressions.
- [ ] In-headset automatic Steam-field, dashboard summon and zero-flash acceptance.
- [ ] Crash, hang, backend loss and sleep/wake acceptance with usable native recovery.
- [ ] Buffered requests and broader native target coverage.

## Further work

- Broader local-app, nested-desktop, and language testing.
- Fluent Japanese-user feedback on composition and candidate controls.
- Broader application testing for Unicode delivery and native shortcuts across system keymaps.
- Sleep/wake, runtime-update, and longer-session testing.
- Measured battery, memory, and latency results.
- Shell integration, scheduled after the first public release: alternative shells launch the keyboard in hosted mode and control it over an inherited connection. A standalone keyboard has no shell-control protocol. Presentation, placement, themes, typing contexts, settings, dictation, silent updates, pointer forwarding and a commits-only text sink are specified in [shell integration](integration.md). Protocol 1 is implemented and host-tested; headset validation with the test shell is next, then `theme.derive` and the Wayland bridge.

Swipe typing for Japanese and Korean is planned in [swipe typing](swipe-typing.md). No universal native keyboard replacement, streamed PC input, or cross-session clipboard synchronization is promised. See [changelog](../CHANGELOG.md) for released behavior changes.
