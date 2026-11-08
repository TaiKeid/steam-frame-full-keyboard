#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// Writes text to the clipboard for dictation's paste Send (docs/dictation.md,
// Send). The keyboard only ever writes: it takes ownership of the clipboard
// and answers requests for its own text, and never asks for anyone else's.
class Clipboard {
  public:
    virtual ~Clipboard() = default;
    // Starts owning the clipboard with `text`. Returns a number for result().
    virtual std::uint64_t write(const std::string& text) = 0;
    // Whether write `number` took the clipboard on some display; empty while
    // it is still being set.
    virtual std::optional<bool> result(std::uint64_t number) const = 0;
    // The same, but only a nested desktop session counts: its applications
    // paste from its own clipboard, so owning Gamescope's X displays alone
    // would let Ctrl+V paste whatever the desktop held before.
    virtual std::optional<bool> desktop_result(std::uint64_t number) const { return result(number); }
    // Requests for the text answered so far, so Send can tell that an
    // application took it after Ctrl+V.
    virtual std::uint64_t served() const = 0;
    // Like write(), but on Gamescope's own X displays only, where a streaming
    // client such as Moonlight reads it: a nested desktop keeps its own
    // clipboard, and its clipboard history doesn't record each character.
    virtual std::uint64_t write_gamescope(const std::string& text) { return write(text); }
    // Who read the text on Gamescope's X displays since served() was
    // `since`, one number per reader (a display and its requesting window);
    // empty when nobody did, and no value when this clipboard can't tell.
    // A paste can then tell an application's read from Gamescope's own.
    virtual std::optional<std::vector<std::uint64_t>> readers_since(std::uint64_t) const { return {}; }
};

// Owns CLIPBOARD on every X display Gamescope runs, as Steam uses :0 and
// the applications it starts use :1, and Gamescope does not copy one to the
// other. Also sets the clipboard of every nested Wayland desktop session
// (a socket in a subdirectory of XDG_RUNTIME_DIR, such as SteamOS's Desktop),
// whose compositor keeps its own and copies nothing from Gamescope, through
// the standard ext data-control protocol or wlroots' older one. XCB and
// Wayland on a thread of its own: unlike Xlib, XCB never ends the process
// when a display goes away. The text stays until another application copies
// or the keyboard exits. Tests pass their own directories, so they never
// touch a real session's clipboard.
std::unique_ptr<Clipboard> make_clipboard(std::filesystem::path x11_sockets = "/tmp/.X11-unix",
                                          std::filesystem::path runtime = {});
} // namespace framekeyboard
