// Cut, Copy and Paste in terminals (docs/terminal-edit-keys-design.md): which
// programs are terminals, the chord each kind presses, and that KeyboardState
// holds exactly that chord and releases it in reverse.
#include "framekeyboard/input.hpp"
#include "framekeyboard/terminal_keys.hpp"

#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fk = framekeyboard;
using fk::EditAction;
using fk::EditKeys;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void classification_tests() {
    for (const auto* name :
         {"konsole", "gnome-terminal-server", "gnome-terminal", "ptyxis", "kgx", "kitty", "foot",
          "footclient", "alacritty", "wezterm-gui", "wezterm", "lxterminal", "ghostty", "st"}) {
        require(fk::terminal_edit_keys(name) == EditKeys::CtrlShift, "known terminals use Ctrl+Shift");
    }
    for (const auto* name : {"urxvt", "urxvtd", "rxvt-unicode"}) {
        require(fk::terminal_edit_keys(name) == EditKeys::CtrlAlt, "rxvt-unicode uses Ctrl+Meta");
    }
    for (const auto* name : {"xterm", "uxterm", "koi8rxterm"}) {
        require(fk::terminal_edit_keys(name) == EditKeys::SelectionOnly, "xterm copies by selecting");
    }
    // Ordinary apps, the Desktop's compositor, and near misses keep Ctrl+X/C/V.
    for (const auto* name :
         {"", "kwin_wayland", "steamwebhelper", "firefox", "kate", "dolphin", "code", "python3",
          "Konsole", "/usr/bin/konsole", "konsole ", "xterm-256", "sterm", "kitty.bin"}) {
        require(fk::terminal_edit_keys(name) == EditKeys::Standard,
                "anything else, paths and other spellings included, is no terminal");
    }
    require(fk::stream_client("moonlight") && fk::stream_client("moonlight-qt") &&
                fk::terminal_edit_keys("moonlight") == EditKeys::Standard,
            "Moonlight is a streaming client with ordinary edit keys");
    for (const auto* name : {"", "Moonlight", "/app/bin/moonlight", "steamwebhelper", "kwin_wayland"}) {
        require(!fk::stream_client(name), "anything else is not");
    }
    // App IDs as KWin reports them inside SteamOS's Desktop.
    for (const auto* id :
         {"org.kde.konsole", "lxterminal", "kitty", "foot", "Alacritty", "org.wezfurlong.wezterm",
          "org.gnome.Ptyxis", "org.gnome.Console", "org.gnome.Terminal", "com.mitchellh.ghostty"}) {
        require(fk::app_edit_keys(id) == EditKeys::CtrlShift,
                "Desktop terminals by app ID use Ctrl+Shift");
    }
    require(fk::app_edit_keys("XTerm") == EditKeys::SelectionOnly &&
                fk::app_edit_keys("URxvt") == EditKeys::CtrlAlt,
            "X11 window classes match whatever their case");
    for (const auto* id :
         {"", "org.kde.dolphin", "org.kde.kate", "firefox", "org.kde.plasmashell", "org.gnome.Nautilus",
          "org.kde.konsole.desktop.extra", "terminal", "org.example.st", "org.example.foot",
          "com.example.kitty", "net.example.xterm"}) {
        require(fk::app_edit_keys(id) == EditKeys::Standard, "other apps keep Ctrl+X/C/V");
    }
}
void combine_tests() {
    require(fk::combine_edit_keys(EditKeys::Standard, false) == EditKeys::Standard,
            "nothing known: Ctrl+X/C/V");
    require(fk::combine_edit_keys(EditKeys::Standard, true) == EditKeys::CtrlShift,
            "a shell's terminal hint uses the common terminal keys");
    for (const auto keys : {EditKeys::CtrlShift, EditKeys::CtrlAlt, EditKeys::SelectionOnly}) {
        require(fk::combine_edit_keys(keys, true) == keys && fk::combine_edit_keys(keys, false) == keys,
                "a detected terminal's own keys win over the hint");
    }
}
void chord_tests() {
    const auto check = [](EditAction action, EditKeys keys, std::vector<int> modifiers,
                          xkb_keysym_t symbol, const char* key, const char* message) {
        const auto chord = fk::edit_chord(action, keys);
        require(chord.sends() && chord.modifiers == modifiers && chord.symbol == symbol &&
                    chord.key == key,
                message);
    };
    check(EditAction::Cut, EditKeys::Standard, {KEY_LEFTCTRL}, XKB_KEY_x, "KeyX", "Cut is Ctrl+X");
    check(EditAction::Copy, EditKeys::Standard, {KEY_LEFTCTRL}, XKB_KEY_c, "KeyC", "Copy is Ctrl+C");
    check(EditAction::Paste, EditKeys::Standard, {KEY_LEFTCTRL}, XKB_KEY_v, "KeyV", "Paste is Ctrl+V");
    check(EditAction::Copy, EditKeys::CtrlShift, {KEY_LEFTCTRL, KEY_LEFTSHIFT}, XKB_KEY_c, "KeyC",
          "terminal Copy is Ctrl+Shift+C");
    check(EditAction::Paste, EditKeys::CtrlShift, {KEY_LEFTCTRL, KEY_LEFTSHIFT}, XKB_KEY_v, "KeyV",
          "terminal Paste is Ctrl+Shift+V");
    check(EditAction::Copy, EditKeys::CtrlAlt, {KEY_LEFTCTRL, KEY_LEFTALT}, XKB_KEY_c, "KeyC",
          "rxvt-unicode Copy is Ctrl+Alt+C");
    check(EditAction::Paste, EditKeys::CtrlAlt, {KEY_LEFTCTRL, KEY_LEFTALT}, XKB_KEY_v, "KeyV",
          "rxvt-unicode Paste is Ctrl+Alt+V");
    for (const auto keys : {EditKeys::CtrlShift, EditKeys::CtrlAlt}) {
        const auto chord = fk::edit_chord(EditAction::Cut, keys);
        require(!chord.sends() && chord.modifiers.empty(),
                "a terminal cannot cut: Cut sends nothing, never ^X");
    }
    for (const auto action : {EditAction::Cut, EditAction::Copy, EditAction::Paste}) {
        const auto chord = fk::edit_chord(action, EditKeys::SelectionOnly);
        require(!chord.sends() && chord.modifiers.empty(), "xterm gets no key at all");
    }
    require(fk::pastes_clipboard(EditKeys::Standard) && fk::pastes_clipboard(EditKeys::CtrlShift) &&
                fk::pastes_clipboard(EditKeys::CtrlAlt) &&
                !fk::pastes_clipboard(EditKeys::SelectionOnly),
            "only xterm-like terminals have no paste key");
    // Never Insert or Delete: Shift+Delete deletes files in file managers,
    // Shift+Insert pastes the primary selection in terminals.
    for (const auto keys :
         {EditKeys::Standard, EditKeys::CtrlShift, EditKeys::CtrlAlt, EditKeys::SelectionOnly}) {
        for (const auto action : {EditAction::Cut, EditAction::Copy, EditAction::Paste}) {
            const auto chord = fk::edit_chord(action, keys);
            require(chord.key != "Insert" && chord.key != "Delete", "never Insert or Delete");
        }
    }
    require(fk::edit_action("cut") == EditAction::Cut && fk::edit_action("copy") == EditAction::Copy &&
                fk::edit_action("paste") == EditAction::Paste,
            "layout action names");
}
// A private receiver: the evdev events KeyboardState sends, nothing more.
struct Capture : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
void state_tests() {
    fk::Key copy;
    copy.id = "Copy";
    copy.action_kind = fk::ActionKind::Shortcut;
    copy.action = "copy";
    {
        Capture sink;
        fk::KeyboardState state(sink);
        state.down(0, copy, 0);
        state.up(0);
        require(sink.events ==
                    std::vector<std::pair<int, int>>{
                        {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
                "without a chord, Copy is still Ctrl+C");
    }
    {
        Capture sink;
        fk::KeyboardState state(sink);
        const auto chord = fk::edit_chord(EditAction::Copy, EditKeys::CtrlShift);
        state.down(0, copy, 0, false, KEY_C, true, chord.modifiers);
        require(sink.events ==
                    std::vector<std::pair<int, int>>{{KEY_LEFTCTRL, 1}, {KEY_LEFTSHIFT, 1}, {KEY_C, 1}},
                "a terminal's Copy holds Ctrl, then Shift, then C");
        state.up(0);
        require(sink.events == std::vector<std::pair<int, int>>{{KEY_LEFTCTRL, 1},
                                                                {KEY_LEFTSHIFT, 1},
                                                                {KEY_C, 1},
                                                                {KEY_C, 0},
                                                                {KEY_LEFTSHIFT, 0},
                                                                {KEY_LEFTCTRL, 0}},
                "and releases them in reverse");
        require(state.modifiers().empty(), "no modifier stays latched or held");
    }
    {
        // Cancelling a held terminal Copy, as hiding does, releases every key.
        Capture sink;
        fk::KeyboardState state(sink);
        state.down(0, copy, 0, false, KEY_C, true,
                   fk::edit_chord(EditAction::Copy, EditKeys::CtrlAlt).modifiers);
        state.cancel_all();
        int held = 0;
        for (const auto& [code, value] : sink.events) {
            held += value ? 1 : -1;
        }
        require(held == 0 && sink.events.size() == 6, "cancel releases Ctrl, Alt and C");
    }
    {
        // A latched Shift is not added to a shortcut, as before.
        Capture sink;
        fk::KeyboardState state(sink);
        fk::Key shift;
        shift.id = "ShiftLeft";
        shift.action_kind = fk::ActionKind::Key;
        shift.action = "ShiftLeft";
        state.down(1, shift, 0);
        state.up(1);
        state.down(0, copy, .1, false, KEY_C, true,
                   fk::edit_chord(EditAction::Copy, EditKeys::Standard).modifiers);
        state.up(0);
        require(sink.events ==
                    std::vector<std::pair<int, int>>{
                        {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
                "a latched Shift does not turn Copy into Ctrl+Shift+C");
    }
}
} // namespace

int main() {
    try {
        classification_tests();
        combine_tests();
        chord_tests();
        state_tests();
    } catch (const std::exception& error) {
        std::cerr << "terminal keys test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "terminal keys tests passed\n";
    return 0;
}
