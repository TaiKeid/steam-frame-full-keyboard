#pragma once

#include "layout.hpp"

namespace framekeyboard {
// Dictation mode's keys, generated from the main layout in its own coordinates
// so both share one scale (docs/dictation.md, Dictation mode layout). Function
// keys, Tab, Insert, Ctrl, Super, Alt, Steam, Menu, Copy, Paste and Emoji
// go. Send spans the right-hand Alt to Ctrl keys, or takes Tab's slot in
// layouts without them, and Undo takes Paste's slot above the mic key. The
// editor replaces the function-key row: from right of Esc to the visible
// right edge, reaching up into the gap under the toolbar. Hidden numpad space
// stays empty.
struct DictationLayout {
    Layout layout;
    Rect editor; // Layout coordinates; its top is above the layout's own.
};
DictationLayout dictation_layout(const Layout& main, double visible_width);
// How far the editor reaches above the layout's top, and below the
// function-key row toward the number row, in layout units. Four lines fit.
constexpr double dictation_editor_rise = 60;
constexpr double dictation_editor_drop = 12;
} // namespace framekeyboard
