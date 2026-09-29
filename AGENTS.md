# Full Keyboard for Steam Frame development

Follow any enclosing workspace instructions. This file covers project engineering; keep machine-specific notes, deployment records, and credentials outside this repository. The public project name is Full Keyboard for Steam Frame, the app label is Full Keyboard, and internal identifiers remain framekeyboard.

- Read `README.md`, `docs/plan.md`, and the relevant architecture section before implementation. Mark milestone checkboxes only after their acceptance conditions pass.
- The approved appearance is `design/index.html`. Native key geometry is `layouts/en-us.json`; visual constants are `themes/graphite.json`. Keep fixed-size press movement, the flat case, shallow key sides, and left Copy/Paste keys.
- Cross-build deployment artifacts on the development host. Keep host and ARM64 output separate. Do not maintain a source checkout on Frame or run an ARM64 artifact on the host.
- Treat file-based layouts, languages and themes with in-VR selection as core requirements. Read `docs/configuration.md`; keep key geometry, input mapping and styles independent. Do not equate relabeled keys with working language support.
- Keep rendering, key state, input backend, and VR lifecycle separate. No actual input delivery from UI rendering code.
- Default development behavior must not send keys to the user's focused application. Test input with a dedicated receiver before application validation.
- Do not log typed text, credentials, clipboard contents, or browser field values. Do not add clipboard reads for debugging.
- Distinguish logical key IDs, Linux evdev codes, and Steam key codes. Never reuse raw numeric codes across backends.
- Every held key needs a release path on cancellation, target change, disconnect, hide, and shutdown. The stock keyboard must remain usable if the native process dies.
- Keep this a manually launched alternative keyboard. Do not suppress the stock UI or intercept its summon button.
- Feature-check private runtime interfaces and fail without changing Steam files or global input settings.
- Vendor external code only with a pinned revision and its license/attribution. The sibling overlay is a reference, not an implicit build dependency.
- Git stays local until publication is requested. Do not copy build sysroots, credentials, machine logs, or recovery backups into Git.

- Use `.clang-format` for our C++ files. Keep functions focused and comment non-obvious state ownership, coordinate/code conversions and cleanup ordering. Do not reformat vendored code without reason.
- Run `ctest --test-dir build/host --output-on-failure` for core changes. Run the separate uinput smoke test only against its exclusively grabbed device; never turn it into a test of the user's focused app.
