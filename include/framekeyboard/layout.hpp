#pragma once

#include <string>
#include <vector>

namespace framekeyboard {

// Design coordinates are shared by painting and hit testing, before panel scaling.
struct Rect {
    bool operator==(const Rect&) const = default;
    double x{}, y{}, width{}, height{};
    bool contains(double px, double py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

enum class Icon {
    None,
    ScaleDown,
    ScaleUp,
    Settings,
    Back,
    Reload,
    Apply,
    PinOn,
    PinOff,
    Recenter,
    Close,
    Copy,
    Paste,
    SteamFrame,
    SteamOS,
    Backspace,
    PassthroughOn,
    PassthroughOff
};

// Flick keys name a slot of the language's flick map; they never send physical keys.
enum class ActionKind { Key, Shortcut, Flick };
struct Key {
    bool operator==(const Key&) const = default;
    std::string id, label, secondary_label;
    Rect bounds;
    ActionKind action_kind{};
    // Physical key name or shortcut name; never a Steam numeric key code.
    std::string action;
    Icon icon{Icon::None};
    // Modifiers normally latch on a tap. False sends a physical hold instead.
    bool sticky{true};
};
struct Layout {
    bool operator==(const Layout&) const = default;
    std::string id, name;
    // Language to switch to when the current one cannot use this geometry.
    std::string language;
    double width{}, height{};
    std::vector<Key> keys;
};

} // namespace framekeyboard
