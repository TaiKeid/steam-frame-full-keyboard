# Configuration and VR switching

File-based layouts, language support and editable themes are core requirements. Users must be able to add profiles, then select them inside VR without recompiling or restarting the keyboard.

Version 0.1.0 implements runtime profiles, VR selectors, reload and persistent selection. The bundled resources are also embedded for fallback. English and German XKB legends are tested. Native output requires a matching target session keymap, explicitly declared with `--target-language`; automatic synchronization and application delivery remain unverified. IME composition is not implemented.

## Independent profiles

| Profile | Owns | Example |
| --- | --- | --- |
| Layout | Key positions, sizes, physical key IDs and shortcut actions | `layouts/en-us.json`, `layouts/international.json` |
| Language | Input keymap, key legends, locale and font preferences | `languages/en-us.json`, `languages/de-de.json` |
| Theme | Colors, gradients, rounding, sides, press movement and animation timing | `themes/graphite.json` |
| Settings | Active profile IDs and named favorite combinations | `config/default.json` |

Keep these separate. The same geometry can display English or German legends, and a theme can apply to either language. A favorite saves a layout, language and theme together for one-step switching. Full-size is the default, but the runtime must not assume 106 keys or hardcode their arrangement.

The current `en-us-full` layout ID identifies the approved ANSI geometry and its fallback legends. Its name does not restrict it to English. Languages declare `required_keys` as physical action names. The German profile requires `IntlBackslash`, provided by `international-full`. Activation rejects a layout missing that key. This international variant retains the rectangular Enter from the approved design.

## File locations and discovery

Packaged defaults live in the versioned installation's `layouts/`, `languages/`, and `themes/` directories. User files live under `$XDG_CONFIG_HOME/framekeyboard`, defaulting to `~/.config/framekeyboard`:

```text
config.json
layouts/*.json
languages/*.json
themes/*.json
```

The installer must preserve user files during updates and must not rewrite them with bundled defaults. The OS image is not a configuration store. Each profile has a stable `id`, a display `name`, and a `schema_version`. Settings reference IDs, not absolute paths. A user profile with the same ID can override a bundled profile; the VR picker shows its source. Reject duplicate IDs within a source directory instead of depending on filesystem order.

Keep a compiled safe default available. If a user override is invalid, retain the last validated profile and show its filename and error in settings. On a cold start, fall back to the bundled profile or compiled default. Unknown schema versions must not be guessed or silently rewritten.

## Layouts and styles

Layout JSON defines key rectangles and actions. The existing validator checks bounds and overlap. Extend the runtime validator to limit profile size, key count, label length and action types. Profiles are data and cannot execute commands or scripts.

Theme JSON defines renderer properties directly. Support surface and legend colors, key-face gradients, side colors/depth, corner radii, spacing/padding and font sizing, plus hover, pressed, latched and disabled state styles. The current renderer consumes the graphite fields plus `font_size`, `small_font_size`, `padding`, `hover` and `latched`. It uses immediate hover/latched colors and animated press travel. The reserved `disabled` color is parsed but has no disabled-key state in this release. Per-key fonts, arbitrary spacing rules and additional state styles remain future work. Native rendering does not require CSS or JavaScript theme execution.

Validate dimensions and animation limits. In particular, press travel cannot exceed side depth, and painting must stay aligned with hit regions. A theme may change appearance but cannot change the meaning of a key or the input keymap. Geometry changes belong to layout profiles.

## Languages and actual input

A language profile selects a keymap and derives character legends from it, including Shift and AltGr levels. Optional legend overrides are keyed by physical key ID and never change output by themselves. Keep physical positions such as `KeyY` separate from the symbol that a German keymap produces there. Special actions such as Enter and Copy remain explicit actions.

Language profiles use XKB identifiers. `input_method` may be omitted or set to `xkb`; other input methods and nonempty XKB options are rejected in this version. `legends.overrides` maps a key ID to a display string. The uinput backend requires the selected language ID to equal the command-line `--target-language` declaration before input can be armed. This is a user-provided declaration, not automatic detection. Locally changing key labels or compiling a different XKB map does not change the target application's keymap. Desktop and floating-window sessions may need different routing or synchronization strategies. Do not silently change the user's global keyboard settings as a side effect of opening our panel.

Prove US English and German as the first two profiles. German exercises swapped Y/Z, umlauts and AltGr. Additional languages must be addable through files where the backend already supports their input method. Dead keys, Compose, Unicode text delivery, right-to-left scripts and IME candidate/composition handling need explicit capabilities and tests. They are required extension paths; do not claim arbitrary language support from a list of locale names. If a profile needs an unavailable capability, show the reason and leave the working profile active.

Use font fallback and text shaping for non-Latin legends. Treat interface translation separately from typing language; changing the typing language must not unexpectedly change the settings UI language. Do not use the clipboard as an implicit text-insertion fallback.

## Switching inside VR

The top row provides Settings, Release all, Input on/off, Recenter and Close. Settings provides Layout, Language and Theme selectors, favorite combinations, Apply and save, and Reload profiles. Applying returns to the keyboard, which shows the selected design. A separate temporary preview/apply workflow remains future work. Newly added files appear after reload; an automatic file watcher is optional.

Validate and prepare a candidate configuration off to the side. On Apply, cancel active pointer presses, stop repeat and release held keys through the old backend before changing mappings. Clear latched modifiers, replace geometry and hit regions together, and redraw. Preserve the target application's focus. A failed apply keeps the previous working configuration and reports the error in VR.

Persist successful selection to `config.json` using an atomic write. If saving fails, keep the usable runtime configuration and report that it will not survive restart. Reloading files must not overwrite unrelated favorites or silently activate an edited profile while a key is held. The active selection survives app restart; the installer preserves user files across application updates. Reload turns input off and reloads favorites. It keeps the active selection rather than activating a config.json edit behind the user's back.

## Implementation order

Profile parsing, catalog discovery, native rendering and VR selector code are implemented. Next verify controller interaction and real language output in both target sessions. See the plan and acceptance checks for the remaining integration work.
