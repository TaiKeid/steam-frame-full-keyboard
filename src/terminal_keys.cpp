#include "framekeyboard/terminal_keys.hpp"

#include <array>
#include <linux/input-event-codes.h>
#include <utility>

namespace framekeyboard {
namespace {
// Each terminal's own defaults; sources in docs/terminal-edit-keys-design.md.
constexpr std::array<std::pair<std::string_view, EditKeys>, 19> terminals{{
    {"konsole", EditKeys::CtrlShift},
    {"gnome-terminal-server", EditKeys::CtrlShift},
    {"gnome-terminal", EditKeys::CtrlShift},
    {"ptyxis", EditKeys::CtrlShift},
    {"kgx", EditKeys::CtrlShift},
    {"kitty", EditKeys::CtrlShift},
    {"foot", EditKeys::CtrlShift},
    {"footclient", EditKeys::CtrlShift},
    {"alacritty", EditKeys::CtrlShift},
    {"wezterm-gui", EditKeys::CtrlShift},
    {"wezterm", EditKeys::CtrlShift},
    {"lxterminal", EditKeys::CtrlShift},
    {"st", EditKeys::CtrlShift},
    {"urxvt", EditKeys::CtrlAlt},
    {"urxvtd", EditKeys::CtrlAlt},
    {"rxvt-unicode", EditKeys::CtrlAlt},
    {"xterm", EditKeys::SelectionOnly},
    {"uxterm", EditKeys::SelectionOnly},
    {"koi8rxterm", EditKeys::SelectionOnly},
}};
} // namespace

EditKeys terminal_edit_keys(std::string_view program) {
    for (const auto& [name, keys] : terminals) {
        if (program == name) {
            return keys;
        }
    }
    return EditKeys::Standard;
}
EditKeys combine_edit_keys(EditKeys detected, bool terminal_hint) {
    return detected == EditKeys::Standard && terminal_hint ? EditKeys::CtrlShift : detected;
}
EditAction edit_action(std::string_view name) {
    return name == "cut" ? EditAction::Cut : name == "copy" ? EditAction::Copy : EditAction::Paste;
}
EditChord edit_chord(EditAction action, EditKeys keys) {
    if (keys == EditKeys::SelectionOnly) {
        return {}; // Any key would reach the program as a control character.
    }
    // A terminal cannot remove text, so Cut copies; Ctrl+Shift+X is no
    // terminal's cut and would reach the program as ^X.
    if (keys != EditKeys::Standard && action == EditAction::Cut) {
        action = EditAction::Copy;
    }
    EditChord chord;
    chord.modifiers = {KEY_LEFTCTRL};
    if (keys == EditKeys::CtrlShift) {
        chord.modifiers.push_back(KEY_LEFTSHIFT);
    } else if (keys == EditKeys::CtrlAlt) {
        chord.modifiers.push_back(KEY_LEFTALT);
    }
    switch (action) {
    case EditAction::Cut:
        chord.symbol = XKB_KEY_x;
        chord.key = "KeyX";
        break;
    case EditAction::Copy:
        chord.symbol = XKB_KEY_c;
        chord.key = "KeyC";
        break;
    case EditAction::Paste:
        chord.symbol = XKB_KEY_v;
        chord.key = "KeyV";
        break;
    }
    return chord;
}
} // namespace framekeyboard
