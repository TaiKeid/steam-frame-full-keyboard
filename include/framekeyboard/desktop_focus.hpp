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
// display, the focused window's title and the focused process's command line;
// never window contents.
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
    // Never blocks: that window's title, kept current as it changes; empty
    // until known. Gamescope names the window's SteamVR overlay after it.
    std::string title() const;
    // Whether this process is a nested desktop compositor (exposed for tests).
    static bool nested_compositor(std::uint32_t pid);
    // Shared with the watcher thread; defined in desktop_focus.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
// The window Gamescope sends keys to (DashboardFocus). It follows SteamVR's
// overlay input focus, so it can be a Steam dashboard window while
// DesktopFocus still reports the focused app. X reports no event for it, so
// this polls X's input focus ten times a second on a detached thread of its
// own, like DesktopFocus, only while the panel shows in the dashboard. Reads
// window names and Steam's overlay-target property only; names are never logged.
class KeyboardFocusProbe {
  public:
    explicit KeyboardFocusProbe(std::string display);
    ~KeyboardFocusProbe();
    KeyboardFocusProbe(const KeyboardFocusProbe&) = delete;
    KeyboardFocusProbe& operator=(const KeyboardFocusProbe&) = delete;
    struct Window {
        std::uint32_t id{};
        std::string title;
        bool steam{}; // A Steam window drawn into a VR overlay, such as the dashboard.
    };
    // Never blocks: the last window read, id 0 until known.
    Window window() const;
    // Never blocks: Gamescope's focused app window (GAMESCOPE_FOCUSED_WINDOW),
    // where keys fall back once SteamVR gives our keyboard its input focus.
    Window app() const;
    // Shared with the polling thread; defined in desktop_focus.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
} // namespace framekeyboard
