#include "framekeyboard/desktop_focus.hpp"
#include "framekeyboard/desktop_terminal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>

namespace framekeyboard {
struct DesktopFocus::State {
    std::string display;
    bool desktop_apps{}; // Follows the Desktop's active app (DesktopTerminal).
    std::atomic<bool> focused{}, stop{};
    // The focused program's Cut/Copy/Paste keys; only the kind is kept.
    std::atomic<EditKeys> edit_keys{EditKeys::Standard};
    std::atomic<std::uint64_t> window{}; // X window << 32 | Steam app ID
    mutable std::mutex mutex;
    std::string title; // the focused window's, for Keep open's cue and tooltip
    // The focusable list's windows, under mutex, and how often it changed.
    std::vector<std::uint32_t> windows;
    std::atomic<std::uint64_t> windows_changed{};
    int wake{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    ~State() {
        if (wake >= 0) {
            close(wake);
        }
    }
};
namespace {
constexpr int retry_milliseconds = 5000;
// KeyboardFocusProbe: a click on the dashboard should stop typing before the next key.
constexpr int poll_milliseconds = 100;

xcb_atom_t intern(xcb_connection_t* connection, const char* name) {
    auto cookie = xcb_intern_atom(connection, 0, static_cast<std::uint16_t>(std::strlen(name)), name);
    auto* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    xcb_atom_t atom = XCB_ATOM_NONE;
    if (reply) {
        atom = reply->atom;
    }
    std::free(reply);
    return atom;
}
// A root window property of 32-bit values, empty if unset.
std::vector<std::uint32_t> cardinals(xcb_connection_t* connection, xcb_window_t root,
                                     xcb_atom_t property) {
    // (window, app, pid) triplets for the focusable list; Gamescope lists few windows.
    auto* reply = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, root, property, XCB_ATOM_CARDINAL, 0, 3 * 256),
        nullptr);
    std::vector<std::uint32_t> values;
    if (reply && reply->format == 32) {
        const auto* data = static_cast<const std::uint32_t*>(xcb_get_property_value(reply));
        values.assign(data, data + xcb_get_property_value_length(reply) / 4);
    }
    std::free(reply);
    return values;
}
// A window's text property, empty if unset.
std::string text(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property) {
    auto* reply = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0, 64),
        nullptr);
    std::string value;
    if (reply && reply->format == 8) {
        value.assign(static_cast<const char*>(xcb_get_property_value(reply)),
                     static_cast<std::size_t>(xcb_get_property_value_length(reply)));
    }
    std::free(reply);
    return value;
}
// Waits for the server or a stop request; false once the object is gone.
bool wait(DesktopFocus::State& state, int fd, int milliseconds, int other = -1, short other_events = 0) {
    pollfd fds[3]{{state.wake, POLLIN, 0}, {fd, POLLIN, 0}, {other, other_events, 0}};
    // A negative descriptor is skipped by poll().
    poll(fds, other >= 0 ? 3 : fd >= 0 ? 2 : 1, milliseconds);
    return !state.stop;
}
// One connection's life: reads the focus, then again on every change, until
// the connection fails or the object is destroyed.
void watch_connection(DesktopFocus::State& state, xcb_connection_t* connection, int screen_number) {
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < screen_number && screens.rem; ++i) {
        xcb_screen_next(&screens);
    }
    const auto root = screens.data->root;
    const auto focused_window = intern(connection, "GAMESCOPE_FOCUSED_WINDOW");
    const auto focusable_windows = intern(connection, "GAMESCOPE_FOCUSABLE_WINDOWS");
    const auto name = intern(connection, "_NET_WM_NAME");
    // The focused window's title changes with its content, such as a browser
    // tab; its property changes are watched, not polled.
    std::uint32_t titled = 0;
    bool retitle = false;
    if (focused_window == XCB_ATOM_NONE || focusable_windows == XCB_ATOM_NONE) {
        return;
    }
    const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection, root, XCB_CW_EVENT_MASK, &mask);
    std::uint32_t pid = 0;
    bool nested = false, stale = true;
    auto edit_keys = EditKeys::Standard;
    // The Desktop's own bus, found once per compositor; its KWin reports the
    // app active there (DesktopTerminal).
    DesktopTerminal terminal;
    std::string desktop_bus;
    bool desktop = false;
    while (!state.stop && !xcb_connection_has_error(connection)) {
        // XCB reads events that arrive while it waits for a reply into its own
        // queue, not the socket: take them before sleeping on the socket.
        while (auto* event = xcb_poll_for_event(connection)) {
            if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
                const auto* change = reinterpret_cast<xcb_property_notify_event_t*>(event);
                if (change->window == root) {
                    stale |= change->atom == focused_window || change->atom == focusable_windows;
                } else if (change->window == titled) {
                    retitle |= change->atom == name || change->atom == XCB_ATOM_WM_NAME;
                }
            }
            std::free(event);
        }
        if (stale) {
            // Read again at once; the reads may queue further events, taken above.
            stale = false;
            const auto window = cardinals(connection, root, focused_window);
            const auto list = cardinals(connection, root, focusable_windows);
            bool focused = false;
            auto keys = EditKeys::Standard;
            std::uint64_t packed = 0;
            for (std::size_t i = 0; !window.empty() && i + 2 < list.size(); i += 3) {
                if (list[i] == window[0]) {
                    packed = std::uint64_t{list[i]} << 32 | list[i + 1];
                    // The command line is read once per newly focused process;
                    // only what its program name says is kept.
                    if (list[i + 2] != pid) {
                        pid = list[i + 2];
                        const auto name = DesktopFocus::program(pid);
                        nested = name == "kwin_wayland";
                        edit_keys = terminal_edit_keys(name);
                        desktop_bus = nested && state.desktop_apps ? DesktopTerminal::bus_address(pid)
                                                                   : std::string();
                    }
                    focused = nested;
                    keys = edit_keys;
                    break;
                }
            }
            state.focused = focused;
            state.window = packed;
            desktop = focused && !desktop_bus.empty();
            if (desktop && terminal.attach(desktop_bus)) {
                terminal.process(); // The script reports the active window as it starts.
                keys = terminal.edit_keys();
            }
            state.edit_keys = keys;
            // Keep open's cue forgets the titles of windows no longer listed.
            std::vector<std::uint32_t> windows;
            for (std::size_t i = 0; i + 2 < list.size(); i += 3) {
                windows.push_back(list[i]);
            }
            {
                std::lock_guard lock(state.mutex);
                if (windows != state.windows) {
                    state.windows = std::move(windows);
                    ++state.windows_changed;
                }
            }
            const auto now_focused = static_cast<std::uint32_t>(packed >> 32);
            if (now_focused != titled) {
                // Only the focused window is watched: a background window's
                // property changes would otherwise keep waking this thread.
                // A window already gone just answers with an error, ignored.
                if (titled) {
                    const std::uint32_t none = XCB_EVENT_MASK_NO_EVENT;
                    xcb_change_window_attributes(connection, titled, XCB_CW_EVENT_MASK, &none);
                }
                titled = now_focused;
                retitle = true;
                if (titled) {
                    xcb_change_window_attributes(connection, titled, XCB_CW_EVENT_MASK, &mask);
                }
            }
            continue;
        }
        if (retitle) {
            retitle = false;
            auto title = titled ? text(connection, titled, name) : std::string();
            if (titled && title.empty()) {
                title = text(connection, titled, XCB_ATOM_WM_NAME);
            }
            {
                std::lock_guard lock(state.mutex);
                state.title = std::move(title);
            }
            continue; // The reads may have queued events, taken above.
        }
        // The Desktop's KWin says when another app became active there; the
        // keys change only while the Desktop itself has the focus.
        if (terminal.process() && desktop) {
            state.edit_keys = terminal.edit_keys();
        }
        if (!wait(state, xcb_get_file_descriptor(connection), -1, terminal.fd(), terminal.events())) {
            return;
        }
    }
}
void watch(const std::shared_ptr<DesktopFocus::State>& state) {
    while (!state->stop) {
        int screen_number = 0;
        auto* connection = xcb_connect(state->display.c_str(), &screen_number);
        if (!xcb_connection_has_error(connection)) {
            watch_connection(*state, connection, screen_number);
        }
        xcb_disconnect(connection);
        state->focused = false;
        state->edit_keys = EditKeys::Standard;
        state->window = 0;
        {
            std::lock_guard lock(state->mutex);
            state->title.clear();
        }
        wait(*state, -1, retry_milliseconds);
    }
}
} // namespace

DesktopFocus::DesktopFocus(std::string display, bool desktop_apps) : state_(std::make_shared<State>()) {
    state_->display = std::move(display);
    state_->desktop_apps = desktop_apps;
    // The thread keeps its own share of the state, so it may outlive this object.
    std::thread([state = state_] { watch(state); }).detach();
}
DesktopFocus::~DesktopFocus() {
    state_->stop = true;
    if (state_->wake >= 0) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto written = write(state_->wake, &one, sizeof one);
    }
}
std::string DesktopFocus::title() const {
    std::lock_guard lock(state_->mutex);
    return state_->title;
}
std::vector<std::uint32_t> DesktopFocus::windows() const {
    std::lock_guard lock(state_->mutex);
    return state_->windows;
}
std::uint64_t DesktopFocus::windows_changed() const {
    return state_->windows_changed.load();
}
bool DesktopFocus::focused() const {
    return state_->focused.load();
}
EditKeys DesktopFocus::edit_keys() const {
    return state_->edit_keys.load();
}
DesktopFocus::Window DesktopFocus::window() const {
    const auto packed = state_->window.load();
    return {static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed)};
}
struct KeyboardFocusProbe::State {
    std::string display;
    std::atomic<bool> stop{}, active{true};
    mutable std::mutex mutex;
    // Signalled on resume and shutdown, so a paused worker sleeps without a timer.
    std::condition_variable wake;
    KeyboardFocusProbe::Sample sample;
    std::uint64_t epoch{}; // Under mutex; invalidates queries crossing pause/resume.
};
namespace {
bool has_property(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property) {
    auto* reply = xcb_get_property_reply(
        connection, xcb_get_property(connection, 0, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0, 2),
        nullptr);
    const bool set = reply && reply->type != XCB_ATOM_NONE;
    std::free(reply);
    return set;
}
// X focus often lands on an unnamed child: walk up to the first named window.
KeyboardFocusProbe::Window describe(xcb_connection_t* connection, xcb_window_t focus, xcb_atom_t name,
                                    xcb_atom_t target) {
    KeyboardFocusProbe::Window result{focus, {}, false};
    xcb_window_t window = focus;
    for (int depth = 0; depth < 6 && window > 1 && result.title.empty(); ++depth) {
        result.title = text(connection, window, name);
        if (result.title.empty()) {
            result.title = text(connection, window, XCB_ATOM_WM_NAME);
        }
        result.steam |= has_property(connection, window, target);
        auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr);
        const bool top = !tree || tree->parent == tree->root;
        window = tree ? tree->parent : 0;
        std::free(tree);
        if (top) {
            break;
        }
    }
    return result;
}
// Idle while paused: no X traffic, and nothing to read until active again.
bool paused(KeyboardFocusProbe::State& state) {
    if (state.active) {
        return false;
    }
    std::unique_lock lock(state.mutex);
    state.wake.wait(lock, [&] { return state.active || state.stop; });
    return true;
}
void probe(const std::shared_ptr<KeyboardFocusProbe::State>& state) {
    while (!state->stop) {
        if (paused(*state)) {
            continue; // Connects only once first needed.
        }
        auto* connection = xcb_connect(state->display.c_str(), nullptr);
        const auto name = xcb_connection_has_error(connection) ? xcb_atom_t{XCB_ATOM_NONE}
                                                               : intern(connection, "_NET_WM_NAME");
        const auto target = xcb_connection_has_error(connection)
                                ? xcb_atom_t{XCB_ATOM_NONE}
                                : intern(connection, "STEAM_GAMESCOPE_VROVERLAY_TARGET");
        const auto focused_app = xcb_connection_has_error(connection)
                                     ? xcb_atom_t{XCB_ATOM_NONE}
                                     : intern(connection, "GAMESCOPE_FOCUSED_WINDOW");
        const auto root = xcb_connection_has_error(connection)
                              ? xcb_window_t{}
                              : xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
        // Three round trips a poll; a window is described again only when it
        // changes, but the app's title is read each time: it follows the app's
        // contents, and DashboardFocus matches it to Gamescope's overlay names.
        KeyboardFocusProbe::Window window, app;
        std::optional<std::uint64_t> epoch;
        while (!state->stop && !xcb_connection_has_error(connection)) {
            if (paused(*state)) {
                window = app = {}; // Described again once active.
                continue;
            }
            std::uint64_t query_epoch;
            {
                std::lock_guard lock(state->mutex);
                if (!state->active || state->stop) {
                    continue;
                }
                query_epoch = state->epoch;
            }
            if (epoch != query_epoch) {
                window = app = {};
                epoch = query_epoch;
            }
            const double started =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            auto* focus =
                xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
            const auto app_window = cardinals(connection, root, focused_app);
            const std::uint32_t app_id = app_window.empty() ? 0 : app_window[0];
            if (app_id != app.id) {
                app = app_id ? describe(connection, app_id, name, target) : KeyboardFocusProbe::Window{};
            } else if (app_id) {
                auto title = text(connection, app_id, name);
                if (title.empty()) {
                    title = text(connection, app_id, XCB_ATOM_WM_NAME);
                }
                if (!title.empty()) { // Else it came from a parent window, which keeps it.
                    app.title = std::move(title);
                }
            }
            if (focus && focus->focus != window.id) {
                window = describe(connection, focus->focus, name, target);
            }
            const bool valid = focus && !xcb_connection_has_error(connection);
            std::free(focus);
            {
                std::lock_guard lock(state->mutex);
                if (state->active && state->epoch == query_epoch) {
                    state->sample = valid ? KeyboardFocusProbe::Sample{window, app, started}
                                          : KeyboardFocusProbe::Sample{};
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_milliseconds));
        }
        xcb_disconnect(connection);
        {
            std::lock_guard lock(state->mutex);
            state->sample = {};
        }
        std::unique_lock lock(state->mutex);
        state->wake.wait_for(lock, std::chrono::milliseconds(retry_milliseconds),
                             [&] { return state->stop.load(); });
    }
}
} // namespace
KeyboardFocusProbe::KeyboardFocusProbe(std::string display) : state_(std::make_shared<State>()) {
    state_->display = std::move(display);
    std::thread([state = state_] { probe(state); }).detach();
}
KeyboardFocusProbe::~KeyboardFocusProbe() {
    // Under the lock, so a worker about to wait cannot miss the signal.
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}
void KeyboardFocusProbe::set_active(bool active) {
    if (state_->active == active) {
        return; // The VR loop calls this every frame.
    }
    std::lock_guard lock(state_->mutex);
    state_->active = active;
    ++state_->epoch;
    state_->sample = {};
    state_->wake.notify_all();
}
KeyboardFocusProbe::Sample KeyboardFocusProbe::sample() const {
    std::lock_guard lock(state_->mutex);
    return state_->sample;
}
KeyboardFocusProbe::Window KeyboardFocusProbe::window() const {
    std::lock_guard lock(state_->mutex);
    return state_->sample.window;
}
KeyboardFocusProbe::Window KeyboardFocusProbe::app() const {
    std::lock_guard lock(state_->mutex);
    return state_->sample.app;
}
std::string DesktopFocus::program(std::uint32_t pid) {
    if (pid == 0) {
        return {};
    }
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string program;
    std::getline(file, program, '\0');
    return std::filesystem::path(program).filename().string();
}
bool DesktopFocus::nested_compositor(std::uint32_t pid) {
    return program(pid) == "kwin_wayland";
}
} // namespace framekeyboard
