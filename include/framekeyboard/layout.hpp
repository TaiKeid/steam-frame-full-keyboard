#pragma once

#include <string_view>

namespace framekeyboard {

// Coordinates are unscaled design pixels. Rendering and hit testing must share them.
struct Rect {
    double x;
    double y;
    double width;
    double height;
};

enum class ActionKind { Key, Shortcut };

struct Key {
    std::string_view id;
    std::string_view label;
    std::string_view secondary_label;
    Rect bounds;
    ActionKind action_kind;
    // Logical key name or shortcut name, never an implicit Steam/evdev numeric code.
    std::string_view action;
};

} // namespace framekeyboard
