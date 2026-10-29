# Roadmap

Full Keyboard for Steam Frame is a manually launched alternative keyboard for local dashboard apps. Default behavior leaves the system keyboard untouched. A default-off replacement bridge handles supported native requests while the app runs. Streamed VR-app input is outside the project scope.

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
- Experimental swipe typing, powered by FUTO Swipe (0.9.0), with Polish, Dutch and Swedish added in 0.11.0. See [swipe typing](swipe-typing.md).
- Japanese 12-key flick input and typing over pinned windows in passthrough (0.9.0).
- Emoji picker with recent emoji, categories and skin tones, and resizing from the corners (0.10.0). See [emoji picker](emoji.md).
- Dictation on device or with a self-hosted Whisper server, in an editor or typed straight into the focused app (0.11.0). See [dictation](dictation.md).
- 25 more national layouts, each with dictation, for 37 languages in all; Korean 12-key input; emoji favorites (0.11.0).
- Ten more themes, a raised Settings design, and Settings cards that glide and drag (0.11.0).
- Shell integration protocol 1: alternative shells launch the keyboard in hosted mode and control it over an inherited connection (0.11.0). See [shell integration](integration.md).
- Focus mode, which keeps the keyboard up between text fields and flags typing into a window out of sight; nothing is typed while Steam's dashboard has focus; a see-through Background; and clicking floating windows from Stay open over apps (0.12.0). See [configuration](configuration.md).

## Before each release

- Independent code review and resolution of actionable findings.
- The [upkeep checklist](upkeep.md) for third-party code and SteamOS dependencies.
- Review the [release checklist](releasing.md) and [manual acceptance matrix](acceptance.md).
- Confirm install/update/uninstall instructions from a fresh release archive.
- Review the public repository and package for private files and complete third-party notices.

## Native replacement

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
- Shell integration: `theme.derive`, the Wayland input method bridge, and headset checks of anchors, two-hand forwarding and the update handover.
- An in-app update while dictating, tested in the headset.
- PC apps through Moonlight and Sunshine: Moonlight runs as a local window on the Frame, so typing, shortcuts and dictation should reach the PC that Sunshine streams from. Test it, including non-ASCII text, and document it as the way to type into PC apps, since Steam Link streams cannot receive our keys (see [streamed games](streamed-games.md)).

Swipe typing for Japanese and Korean is planned in [swipe typing](swipe-typing.md). No universal native keyboard replacement, streamed PC input, or cross-session clipboard synchronization is promised. See [changelog](../CHANGELOG.md) for released behavior changes.
