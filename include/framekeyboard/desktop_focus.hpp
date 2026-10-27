#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <xcb/xcb.h>

namespace framekeyboard {
// Whether Gamescope's focused window belongs to a nested desktop compositor,
// such as SteamOS's Desktop (kwin_wayland on Gamescope's second X display).
// That compositor reads Gamescope's keys with its own keymap, so text sent
// through Gamescope's temporary keymap arrives as number-row keys there.
// Reads only Gamescope's focus properties on the root window of its X
// display and the focused process's command line; never window contents.
class DesktopFocus {
  public:
    explicit DesktopFocus(std::string display);
    ~DesktopFocus();
    DesktopFocus(const DesktopFocus&) = delete;
    DesktopFocus& operator=(const DesktopFocus&) = delete;
    // Never blocks on the X server: answers from the focus Gamescope last
    // reported and asks again when its properties change.
    bool focused();
    // Whether this process is a nested desktop compositor (exposed for tests).
    static bool nested_compositor(std::uint32_t pid);

  private:
    bool connect();
    void disconnect();
    void request();
    void collect();
    std::string display_;
    xcb_connection_t* connection_{};
    xcb_window_t root_{};
    xcb_atom_t focused_window_{}, focusable_windows_{};
    // Outstanding property requests, answered in a later call.
    std::optional<xcb_get_property_cookie_t> window_request_, list_request_;
    xcb_get_property_reply_t *window_reply_{}, *list_reply_{};
    bool focused_{}, nested_{}, stale_{true};
    std::uint32_t pid_{};
    std::chrono::steady_clock::time_point retry_at_{};
};
} // namespace framekeyboard
