#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
    // Requests for the text answered so far, so Send can tell that an
    // application took it after Ctrl+V.
    virtual std::uint64_t served() const = 0;
};

// Owns CLIPBOARD on every X display Gamescope runs, as Steam uses :0 and
// the applications it starts use :1, and Gamescope does not copy one to the
// other. XCB on a thread of its own: unlike Xlib it never ends the process
// when a display goes away. The text stays until another application copies
// or the keyboard exits.
std::unique_ptr<Clipboard> make_x_clipboard();
} // namespace framekeyboard
