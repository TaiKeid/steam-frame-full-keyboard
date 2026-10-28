#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace framekeyboard {
// Whether Gamescope's focused window belongs to a nested desktop compositor,
// such as SteamOS's Desktop (kwin_wayland on Gamescope's second X display).
// That compositor reads Gamescope's keys with its own keymap, so text sent
// through Gamescope's temporary keymap arrives as number-row keys there.
// Reads only Gamescope's focus properties on the root window of its X
// display and the focused process's command line; never window contents.
//
// X calls wait for the server, so they run on a thread of its own: a server
// that accepts the connection and stops answering stalls only that thread.
// The thread is detached and owns its state, so destroying this object never
// waits for it either.
class DesktopFocus {
  public:
    explicit DesktopFocus(std::string display);
    ~DesktopFocus();
    DesktopFocus(const DesktopFocus&) = delete;
    DesktopFocus& operator=(const DesktopFocus&) = delete;
    // Never blocks: the focus Gamescope last reported, false until known.
    bool focused() const;
    // Never blocks: Gamescope's focused X window and its Steam app ID (0 for
    // other apps), both 0 until known. Used for Keep open's hidden-window cue.
    struct Window {
        std::uint32_t id{}, app{};
    };
    Window window() const;
    // Whether this process is a nested desktop compositor (exposed for tests).
    static bool nested_compositor(std::uint32_t pid);
    // Shared with the watcher thread; defined in desktop_focus.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
} // namespace framekeyboard
