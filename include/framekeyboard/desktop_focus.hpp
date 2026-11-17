#pragma once

#include "app_identity.hpp"
#include "terminal_keys.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// The Steam app ID Gamescope gives Steam's own windows, which are no app the
// user can assign. Believed to be 769; a Frame probe has to confirm it
// (docs/shortcut-bar-design.md, Frame probes). Use this name, not the number.
inline constexpr std::uint32_t steam_client_app_id = 769;
// What the focus watcher knows of the focused window, for focus_identity().
struct FocusFacts {
    std::uint32_t window{};    // Gamescope's focused window; 0 when nothing is focused
    std::uint32_t steam_app{}; // its Steam app ID; 0 for none
    std::string program;       // the focused process's program file name; empty if unreadable
    bool desktop{};            // a nested desktop compositor: SteamOS's Desktop
    // The app ID the Desktop's KWin reported last, once it has reported
    // since attaching (DesktopTerminal::app); empty while no window is
    // active there. None before the first report or when not following.
    std::optional<std::string> desktop_app;
};
// The focused app's identity, by the first row of the design's table that
// fits (docs/shortcut-bar-design.md, Apps: Which identity the focus has):
// none for nothing focused or Steam's own window; the Desktop's app ID
// inside the Desktop, none before its first report; a game-streaming
// client's program name, even when Steam started it; a Steam app ID; else
// the program name. An ID shortcuts.json could not hold is none
// (app_identity_valid). Pure, for tests. Steam's dashboard and Steam's own
// fields are told by App and NativeReplacement, which don't ask this.
std::optional<AppIdentity> focus_identity(const FocusFacts& facts);

// Whether Gamescope's focused window belongs to a nested desktop compositor,
// such as SteamOS's Desktop (kwin_wayland on Gamescope's second X display).
// That compositor reads Gamescope's keys with its own keymap, so text sent
// through Gamescope's temporary keymap arrives as number-row keys there.
// Reads only Gamescope's focus properties on the root window of its X
// display, the focused window's title and the focused process's program name;
// never window contents.
//
// The focused program's name, and inside the Desktop the app ID its KWin
// reported, are kept in memory while that app has the keys, as the focused
// app's identity for the shortcut bar (focused_app()). They are never
// logged, and window titles never go into an identity.
//
// X calls wait for the server, so they run on a thread of its own: a server
// that accepts the connection and stops answering stalls only that thread.
// The thread is detached and owns its state, so destroying this object never
// waits for it either.
class DesktopFocus {
  public:
    // `desktop_apps`: also follow the active app inside SteamOS's Desktop
    // (DesktopTerminal), for Cut, Copy and Paste. Only one watcher may: the
    // Desktop's bus has one name for it.
    explicit DesktopFocus(std::string display, bool desktop_apps = false);
    ~DesktopFocus();
    DesktopFocus(const DesktopFocus&) = delete;
    DesktopFocus& operator=(const DesktopFocus&) = delete;
    // Never blocks: the focus Gamescope last reported, false until known.
    bool focused() const;
    // Never blocks: whether the focused program is a game-streaming client,
    // such as Moonlight (stream_client); false until known.
    bool streaming() const;
    // Never blocks: Gamescope's focused X window and its Steam app ID (0 for
    // other apps), both 0 until known. Used for Keep open's hidden-window cue.
    struct Window {
        std::uint32_t id{}, app{};
    };
    Window window() const;
    // Never blocks: that window's title, kept current as it changes; empty
    // until known. Gamescope names the window's SteamVR overlay after it.
    std::string title() const;
    // Never blocks: Gamescope's focusable X windows, the app windows that
    // still exist, as last read, and how often that list has changed: 0
    // until it is known. Kept as it was across a lost connection.
    std::vector<std::uint32_t> windows() const;
    std::uint64_t windows_changed() const;
    // Never blocks: the focused program's Cut/Copy/Paste keys, a known
    // terminal's own (terminal_edit_keys); inside SteamOS's Desktop, its
    // active app's, when following them (DesktopTerminal). Standard until
    // known and for other programs.
    EditKeys edit_keys() const;
    // Never blocks: false while the Desktop has the focus and its KWin hasn't
    // reported the active app yet, for at most desktop_report_wait.
    bool edit_keys_known() const;
    static constexpr std::chrono::seconds desktop_report_wait{3};
    // Never blocks: how many activations SteamOS's Desktop reported inside
    // its one Gamescope window, when following its apps; 0 otherwise. Each
    // changes it, a dialog of the same app included, which window() can't
    // show (R320).
    std::uint64_t desktop_activations() const;
    // Never blocks: the focused window and its app's identity, published
    // together (focus_identity); empty until known and while the connection
    // is lost.
    FocusedApp focused_app() const;
    // A process's program file name, from argv[0]; empty if it is gone.
    static std::string program(std::uint32_t pid);
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
    struct Sample {
        Window window, app;
        // CLOCK_MONOTONIC seconds before the focus query, not when its reply
        // was published. A delayed pre-click query is still pre-click evidence.
        double started{};
    };
    // Paused, it makes no X calls and reports nothing. One probe lives as
    // long as the VR loop and is paused while not needed, never recreated:
    // a worker stuck in an X call that never returns stays one thread.
    void set_active(bool active);
    // Coherent keyboard/app focus and freshness; started 0 means unknown.
    Sample sample() const;
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
