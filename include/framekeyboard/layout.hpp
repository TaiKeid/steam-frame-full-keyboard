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
    Recenter,
    Close,
    Copy,
    Paste,
    SteamFrame,
    SteamOS,
    Dictate
};

// App actions run inside the keyboard (e.g. dictation) and never send input.
enum class ActionKind { Key, Shortcut, App };
struct Key {
    bool operator==(const Key&) const = default;
    std::string id, label, secondary_label;
    Rect bounds;
    ActionKind action_kind{};
    // Physical key, shortcut or app action name; never a Steam numeric key code.
    std::string action;
    Icon icon{Icon::None};
    // Modifiers normally latch on a tap. False sends a physical hold instead.
    bool sticky{true};
};
struct Layout {
    bool operator==(const Layout&) const = default;
    std::string id, name;
    double width{}, height{};
    std::vector<Key> keys;
};

} // namespace framekeyboard
