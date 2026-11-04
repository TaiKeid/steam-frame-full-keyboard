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

// Which ways something under a laser can still scroll: back toward its
// start, forward toward its end.
struct ScrollRoom {
    bool back{}, forward{};
    bool operator==(const ScrollRoom&) const = default;
};

enum class Icon {
    None,
    Volume25,
    Volume50,
    Volume75,
    Volume100,
    HapticSound,
    HapticSoundOff,
    KeyFlick,
    KeyFlickOff,
    PopHigh,
    PopHighClick,
    PopMiddle,
    PopMiddleClick,
    PopLow,
    PopLowClick,
    Settings,
    Back,
    Reload,
    Apply,
    PinOn,
    PinOff,
    Recenter,
    Close,
    Copy,
    Cut,
    Paste,
    SteamFrame,
    SteamOS,
    Backspace,
    PassthroughOn,
    PassthroughOff,
    KeepOpenOn,
    KeepOpenOff,
    Emoji,
    // Emoji picker categories.
    Recent,
    Favorites, // a star
    Smileys,
    People,
    Animals,
    Food,
    Travel,
    Activities,
    Objects,
    Symbols,
    Flags,
    Clear,
    // Empties a text field: a circled cross.
    ClearText,
    // Dictation; where speech is recognized: the headset, the user's own
    // computer, or a hosted provider.
    Mic,
    Undo,
    // The Japanese flick ↶ (Gboard's 逆トグル／戻す), which acts on the reading,
    // apart from the dictation editor's Undo.
    TurnBack,
    // Space and Enter on the flick layouts (␣, ↵), and the Japanese band's ∨
    // and ∧, which open and close the candidate list.
    Space,
    Enter,
    ChevronDown,
    ChevronUp,
    Headset,
    Computer,
    Cloud,
    Eraser,
    Reset,
    Swipe,
    SwipeOff,
};

// Flick keys name a slot of the language's flick map; they never send physical keys.
// The emoji key switches between the keyboard and the emoji picker; it sends nothing.
// Dictation keys ("mic" in layouts; "send" and "undo" in dictation mode) act
// locally and send nothing themselves.
enum class ActionKind { Key, Shortcut, Flick, Emoji, Dictation };
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
    // Japanese flick: the layout shown in English mode. A companion layout is
    // only such a page, never chosen in Settings.
    std::string latin_layout;
    bool companion{};
    double width{}, height{};
    std::vector<Key> keys;
};

} // namespace framekeyboard
