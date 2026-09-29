# Configuration and VR switching

File-based layouts, language support and editable themes are core requirements. Users must be able to add profiles, then select them inside VR without recompiling or restarting the keyboard.

This document defines the proposed version 1 contract. The JSON examples are design specifications. The current bootstrap embeds only `layouts/en-us.json`; it does not load runtime settings, language profiles or themes. The German example is not a claim of working German input.

## Independent profiles

| Profile | Owns | Example |
| --- | --- | --- |
| Layout | Key positions, sizes, physical key IDs and shortcut actions | `layouts/en-us.json` |
| Language | Input keymap, key legends, locale and font preferences | `languages/en-us.json`, `languages/de-de.json` |
| Theme | Colors, gradients, rounding, sides, press movement and animation timing | `themes/graphite.json` |
| Settings | Active profile IDs and named favorite combinations | `config/default.json` |

Keep these separate. The same geometry can display English or German legends, and a theme can apply to either language. A favorite saves a layout, language and theme together for one-step switching. Full-size is the default, but the runtime must not assume 106 keys or hardcode their arrangement.

The current `en-us-full` layout ID identifies the approved ANSI geometry and its fallback legends. Its name does not restrict it to English. Languages that need extra physical positions can ship another layout, such as ISO. Validate required positions against each chosen keymap and report missing keys before activation.

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

Theme JSON defines renderer properties directly. Support surface and legend colors, key-face gradients, side colors/depth, corner radii, spacing/padding and font sizing, plus hover, pressed, latched and disabled state styles. The current graphite file records the approved base colors and press geometry; add the remaining style fields as the renderer implements them. Native rendering does not require CSS or JavaScript theme execution.

Validate dimensions and animation limits. In particular, press travel cannot exceed side depth, and painting must stay aligned with hit regions. A theme may change appearance but cannot change the meaning of a key or the input keymap. Geometry changes belong to layout profiles.

## Languages and actual input

A language profile selects a keymap and derives character legends from it, including Shift and AltGr levels. Optional legend overrides are keyed by physical key ID and never change output by themselves. Keep physical positions such as `KeyY` separate from the symbol that a German keymap produces there. Special actions such as Enter and Copy remain explicit actions.

The proposed language examples use XKB identifiers. The input backend must establish that its output mapping matches the receiving session before enabling a language. Locally changing key labels or compiling a different XKB map does not change the target application's keymap. Desktop and floating-window sessions may need different routing or synchronization strategies. Do not silently change the user's global keyboard settings as a side effect of opening our panel.

Prove US English and German as the first two profiles. German exercises swapped Y/Z, umlauts and AltGr. Additional languages must be addable through files where the backend already supports their input method. Dead keys, Compose, Unicode text delivery, right-to-left scripts and IME candidate/composition handling need explicit capabilities and tests. They are required extension paths; do not claim arbitrary language support from a list of locale names. If a profile needs an unavailable capability, show the reason and leave the working profile active.

Use font fallback and text shaping for non-Latin legends. Treat interface translation separately from typing language; changing the typing language must not unexpectedly change the settings UI language. Do not use the clipboard as an implicit text-insertion fallback.

## Switching inside VR

Provide a settings control on our panel. Its placement still needs visual design work. The panel must offer Layout, Language and Theme selectors, favorite combinations, a preview, and Reload profiles. Newly added files appear after reload; an automatic file watcher is optional.

Validate and prepare a candidate configuration off to the side. On Apply, cancel active pointer presses, stop repeat and release held keys through the old backend before changing mappings. Clear latched modifiers, replace geometry and hit regions together, and redraw. Preserve the target application's focus. A failed apply keeps the previous working configuration and reports the error in VR.

Persist successful selection to `config.json` using an atomic write. If saving fails, keep the usable runtime configuration and report that it will not survive restart. Reloading files must not overwrite unrelated favorites or silently activate an edited profile while a key is held. The active selection should survive app restart and user-local application updates.

## Implementation order

Implement profile parsing, validation, catalog discovery and selection state before hardcoding native rendering behavior. Build the native preview around that model. Add the VR selectors once the overlay exists, then verify language output through each real input backend. See the plan and acceptance checks for completion criteria.
