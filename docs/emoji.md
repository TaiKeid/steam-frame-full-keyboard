# Emoji picker plan

Status: designed on `feature/emojis` (2026-10-25), not started. Mark milestone checkboxes only after their acceptance conditions pass. Items marked *proposal* are not yet confirmed by the owner.

The emoji picker is a second view of the keyboard. An emoji key in the side column switches to it. The view shows a scrollable grid of emoji with category tabs, a space bar, and Backspace, Enter and arrow keys, so a user can type a short message without switching back.

## Side column

![Side column with the emoji key (owner mockup; the red frame marks the three equal keys)](images/emoji-side-column.png)

The left side column changes in every layout, whether or not the picker is available:

1. A small **emoji key** at the top, where the mockup's `:)` placeholder is. It shows a smiley line icon in the toolbar's icon style.
2. Three tall keys of equal size below it: **Copy**, **Paste**, and **Dictation**.

Dictation is not implemented, so its slot is left empty: no key, no placeholder drawing. [Dictation](dictation.md) fills the slot later.

The emoji key is disabled (drawn with the theme's disabled color, not clickable) when the active input mode can't type Unicode text. This covers the uinput backend, a Unicode transport that isn't ready, and no input backend at all. If that happens while the picker is open, the keyboard returns to the main layout.

## Picker view

![Picker layout (owner mockup)](images/emoji-picker.png)

- **Side column:** the same, except that the emoji key becomes a Back key that returns to the main layout.
- **Category row:** across the top of the main area. Recent comes first, with a clock icon, followed by the Unicode groups: Smileys & Emotion, People & Body, Animals & Nature, Food & Drink, Travel & Places, Activities, Objects, Symbols, Flags. Each tab has its own line icon. If the tabs don't fit, they scroll horizontally, and Recent stays fixed at the left like a frozen table column.
- **Grid:** one horizontal strip covering every category in tab order, filled column by column. Tapping a tab scrolls the grid to that category. The highlighted tab follows the scroll position.
- **Bottom:** a space bar under the grid.
- **Right side:** Backspace and Enter, with the arrow keys below them.
- **Compact (no numpad) mode:** the keyboard keeps its compact width, unlike Settings, which widens it. The grid shrinks to fit: fewer columns are visible at once.
- **Toolbar:** unchanged, except that the language key is disabled while the picker is open.

The view's geometry is generated from the active layout's case size, key unit and side column, rather than from a separate layout file (*proposal*).

## Interaction

- **Typing:** an emoji is typed when the trigger is released over the same cell. If the laser moves more than a few millimeters during the press, it becomes a scroll and nothing is typed. The picker stays open, unless the close-on-select setting is on.
- **No repeat:** holding an emoji never repeats it. A hold opens the skin-tone popup instead (below).
- **Scrolling:** two ways to scroll, both moving the grid horizontally:
  - Dragging with the trigger held.
  - The thumbstick of the controller whose laser is on the grid. In the dashboard this arrives as scroll events. In passthrough, the keyboard reads the stick itself.
  - Scrolling stops at the ends with no momentum (*proposal*).
- **Swipe typing:** belongs to the main layout and is inactive in the picker. No trail is drawn.
- **Haptics:** a hover tick and a press buzz, like the keys.

### Skin tones

Holding an emoji that supports skin tones for 500 ms (*proposal*) opens a popup above it with six variants:

- the default yellow first;
- then the five tones from light to dark.

Releasing over a variant types it and makes that tone the keyboard-wide choice. From then on, every toned emoji in the grid and in Recent is drawn and typed with that tone. Emoji with more than one person use the same tone for each of them. The choice is saved with the recent emoji.

### Recent

- **Contents:** up to six columns of emoji, less one cell for Clear (29 with five rows; *proposal*), stored without a tone so the global tone applies.
- **Layout:** most recent first, filled down each column and then across, like the rest of the grid. With five rows and 17 emoji:

  ```txt
  1 6  11 16
  2 7  12 17
  3 8  13
  4 9  14
  5 10 15  ×
  ```

- **Clear:** a clear icon button takes the bottom cell of Recent's last column, inside the scrolling grid. When that column is full, Clear starts a new column of its own. It is not shown at all while Recent is empty. Pressing it empties Recent at once, with no confirmation (*proposal*); the empty Recent section then shows only a short hint (*proposal*).
- **Order:** a typed emoji moves to the front only the next time the picker opens, so cells never shift under the laser.
- **Storage:** a small file in the app's state directory, not the config. It is a record of typed content, so it is never logged.

### Entering and leaving

- **Entering the picker:** releases every held key and latched modifier, cancels pending dead-key accents, and finishes or cancels any Japanese/CJK composition. This uses the existing release path, so nothing leaks into the target.
- **Hide or reopen:** the keyboard always reopens on the main layout. The picker's category and scroll position are not kept.
- **Settings:** opening Settings from the picker returns to the main layout when Settings closes (*proposal*).

## Setting

**Close emoji picker after choosing**: a checkbox in the General card, saved as the optional global boolean `emoji_close_on_select`. It defaults to false.

## Delivery

Each emoji is sent as one text commit, through the same path as accented and IME text:

- **Steam typing targets** (native replacement): the whole string goes to Steam's text field in one `insertText`.
- **Gamescope** (libei): the whole string goes in one input-method commit. Emoji never qualify for the per-character mode, which is only used when every character is on the ordinary keymap. Gamescope then types the code points through a temporary keymap, one key event each, in order. Steam's own keyboard uses the same Gamescope input-method protocol.
- **Length:** the longest sequences (ZWJ families, subdivision flags) are about 10 code points, within Gamescope's 32-character limit.

Whether an X11 or Wayland application joins a ZWJ sequence or keeps a variation selector depends on that application's text widget. Acceptance tests this in a few common targets.

## Emoji data and coverage

- **Source:** a table generated from Unicode's `emoji-test.txt`, keeping the fully-qualified entries. It records each emoji's group and order and whether it takes skin tones. The Unicode version is pinned, and the Unicode license and attribution go in `third_party/`.
- **Coverage:** only emoji the system color emoji font can draw are shown. Each sequence is shaped once with Pango, and kept only if it comes out as a single cluster with no missing-glyph boxes. A category with nothing left is hidden.
- **No font:** if no color emoji font is available, the emoji key is disabled.

## Rendering and performance

- **Glyph cache:** color emoji fonts store glyphs as large bitmaps, which are slow to scale on every draw. Each emoji is rasterized once at cell size into a cache, and the grid copies from that cache. The cache depends on the theme and the panel's pixel scale, not on the keyboard's size in the world, so resizing keeps it.
- **Async:** a worker thread shapes, checks and rasterizes, starting when the picker first opens:
  - visible cells first, then the rest of the current category, then the others;
  - with its own Pango font map and Cairo surfaces, since neither is shared across threads;
  - cells not yet ready draw empty, and the grid redraws as batches finish.
- **Memory:** at about 64 px per cell, every emoji uses about 30 MB. Keep the categories near the scroll position and drop the rest under a cap (*proposal*: about 8 MB).
- **Redraw:** only while scrolling, hovering, pressing or filling the cache. Nothing redraws when the view is idle.

## Milestones

- [ ] Side column: emoji key, three equal keys, reserved dictation slot, in every layout file.
- [ ] Emoji data table, font coverage check, and Unicode attribution.
- [ ] Picker view with tabs, grid, frozen Recent tab and the compact width.
- [ ] Release-to-type, drag and thumbstick scrolling, no repeat.
- [ ] Async glyph cache with memory cap.
- [ ] Skin-tone popup and global tone.
- [ ] Recent emoji storage and clearing.
- [ ] Close-on-select setting.
- [ ] Disabled states: Unicode transport, language key, missing font.
- [ ] Docs: configuration, acceptance, architecture, changelog.

## Acceptance

- In uinput mode, the emoji key is disabled. If the Unicode transport is lost while the picker is open, the keyboard returns to the main layout.
- Typing works in a Steam text field, a Wayland text field and an X11 text field: a single emoji, a toned emoji, a ZWJ family and a flag, each arriving whole.
- A drag over the grid types nothing, and holding an emoji never repeats it.
- No held key or modifier reaches the target after entering the picker.
- The first open shows the grid without a stall in the VR loop; frame time stays within budget while scrolling.
- Idle CPU with the picker open matches the main layout.
