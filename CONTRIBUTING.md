# Contributing

Full Keyboard for Steam Frame welcomes bug reports, review, and focused fixes. The app serves the Frame dashboard and local applications. Replacing Steam's keyboard or typing into streamed VR apps is outside the current scope.

## Report a bug

Include the app version, SteamOS/runtime version, controller model, affected app, desktop versus floating-window mode, selected language/layout, reproduction steps, and expected/actual result. Include a screenshot or a short log only if it helps. Remove passwords, private text, account details, and clipboard contents.

For Japanese input, include a harmless example reading, the expected conversion, and the selected Romaji/Kana/JIS mode. Feedback from fluent Japanese users is especially useful.

## Change the code

Read [building](docs/building.md), [architecture](docs/architecture.md), and [configuration](docs/configuration.md). Keep renderer, key state, backend, and profile data separate. Use `.clang-format` for project C++; preserve vendored code's style and attribution.

Run the core test suites for behavior changes. Test input with fake sinks or a dedicated receiver, never an arbitrary focused app. Cover lost releases, tracking loss, disconnects, hiding, and shutdown when changing interaction ownership. Add comments where state lifetime, coordinate transforms, or event ordering are not obvious.

Keep changes focused and describe the behavior, validation, and remaining limits. Do not include sysroots, builds, device logs, personal paths, credentials, or user settings. Third-party additions need an exact revision and license notice.

## Review priorities

Input cancellation, focus routing, two-controller ownership, placement persistence, resource cleanup, parsing of custom profiles, and runtime compatibility deserve close review. Tests and successful builds do not replace in-headset validation; say which one was performed.
