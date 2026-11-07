#pragma once
#include "terminal_keys.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace framekeyboard {
// SteamOS's Desktop runs its apps out of Gamescope's sight, so the focused
// program there is only ever kwin_wayland. While attached, the Desktop's own
// KWin runs a few lines of script for this session that send the app ID of
// each newly active window (never its title) to this object over the
// Desktop's session bus, which picks the edit keys. Nothing is saved in the
// Desktop's settings; the script is unloaded on detaching. systemd's sd-bus
// is loaded at runtime: without it nothing is known.
class DesktopTerminal {
  public:
    DesktopTerminal();
    ~DesktopTerminal();
    DesktopTerminal(const DesktopTerminal&) = delete;
    DesktopTerminal& operator=(const DesktopTerminal&) = delete;
    // The Desktop's bus address, read from the environment of the process
    // that started its compositor (`compositor` is kwin_wayland's PID, whose
    // own environment is unreadable); empty when unknown. Only that one
    // variable is kept.
    static std::string bus_address(std::uint32_t compositor);
    // Connects to the Desktop's bus at `address`, if not already, and has its
    // KWin report the active window; false when that failed.
    bool attach(const std::string& address);
    bool attached() const { return bus_ != nullptr; }
    bool attached(const std::string& address) const;
    // For poll(): the connection's descriptor (or -1) and events, and how long
    // poll() may sleep in milliseconds: -1 forever, 0 while messages wait.
    int fd() const;
    short events() const;
    int timeout() const;
    // Takes what arrived; true when the active window's kind changed. A
    // Desktop that closed, or whose KWin left or restarted, detaches, its
    // keys Standard, until attach() is called again.
    bool process();
    // The keys the Desktop's active window takes; Standard when unknown.
    EditKeys edit_keys() const { return keys_; }

  private:
    struct Bus;
    void detach();
    std::unique_ptr<Bus> bus_;
    EditKeys keys_{EditKeys::Standard};
    // KWin's unique bus name: only its calls count. kwin_lost_ when it left.
    std::string kwin_;
    bool kwin_lost_{}, more_{};
};
} // namespace framekeyboard
