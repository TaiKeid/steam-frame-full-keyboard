#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace framekeyboard {
// Which keys the focused application uses for Cut, Copy and Paste
// (docs/terminal-edit-keys-design.md). Standard means Ctrl+X/C/V, also when
// the application is unknown.
enum class EditKeys : std::uint8_t {
    Standard,
    CtrlShift,     // most terminals: Ctrl+Shift+C and Ctrl+Shift+V
    CtrlAlt,       // rxvt-unicode: Ctrl+Meta+C and Ctrl+Meta+V
    SelectionOnly, // xterm: copies by selecting; no key reaches the clipboard
};
enum class EditAction { Cut, Copy, Paste };
// The kind for a program's file name (argv[0]), from a fixed list of
// terminals; Standard for anything else. Exact, case-sensitive names: the
// caller passes only the file name, never a path, title or arguments.
EditKeys terminal_edit_keys(std::string_view program);
// Whether a program (argv[0]'s file name) streams another computer and sends
// it the keys it gets as physical keys, as Moonlight does: text beyond its
// keys must reach that computer another way (docs/streamed-games.md).
bool stream_client(std::string_view program);
// The kind for an app ID as KWin reports it inside SteamOS's Desktop: a
// Wayland app's ID (org.kde.konsole, lxterminal) or an X11 window's class
// (xterm), matched case-insensitively against the same list.
EditKeys app_edit_keys(std::string_view app_id);
// The kind the target takes: what was detected, else a shell's `terminal`
// purpose hint for a field it says is a terminal, which most terminals
// serve with Ctrl+Shift.
EditKeys combine_edit_keys(EditKeys detected, bool terminal_hint);
// The layout action names "cut", "copy" and "paste"; anything else is Paste,
// as shortcut_key() treats it.
EditAction edit_action(std::string_view name);
// What one action presses: `modifiers` in order, then the letter whose
// symbol is `symbol` (resolved through the target keymap, with `key`, a
// logical key ID, as the fallback). An empty `key` sends nothing.
struct EditChord {
    std::vector<int> modifiers;
    xkb_keysym_t symbol{XKB_KEY_NoSymbol};
    std::string key;
    bool sends() const { return !key.empty(); }
};
EditChord edit_chord(EditAction action, EditKeys keys);
// Whether any key pastes the clipboard there (edit_chord(Paste).sends()),
// without building the chord: the paste paths ask every frame.
inline bool pastes_clipboard(EditKeys keys) {
    return keys != EditKeys::SelectionOnly;
}
} // namespace framekeyboard
