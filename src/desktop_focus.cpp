#include "framekeyboard/desktop_focus.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>

namespace framekeyboard {
struct DesktopFocus::State {
    std::string display;
    std::atomic<bool> focused{}, stop{};
    int wake{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    ~State() {
        if (wake >= 0) {
            close(wake);
        }
    }
};
namespace {
constexpr int retry_milliseconds = 5000;

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
// Waits for the server or a stop request; false once the object is gone.
bool wait(DesktopFocus::State& state, int fd, int milliseconds) {
    pollfd fds[2]{{state.wake, POLLIN, 0}, {fd, POLLIN, 0}};
    poll(fds, fd >= 0 ? 2 : 1, milliseconds);
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
    if (focused_window == XCB_ATOM_NONE || focusable_windows == XCB_ATOM_NONE) {
        return;
    }
    const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection, root, XCB_CW_EVENT_MASK, &mask);
    std::uint32_t pid = 0;
    bool nested = false, stale = true;
    while (!state.stop && !xcb_connection_has_error(connection)) {
        if (stale) {
            stale = false;
            const auto window = cardinals(connection, root, focused_window);
            const auto list = cardinals(connection, root, focusable_windows);
            bool focused = false;
            for (std::size_t i = 0; !window.empty() && i + 2 < list.size(); i += 3) {
                if (list[i] == window[0]) {
                    // The command line is read once per newly focused process.
                    if (list[i + 2] != pid) {
                        pid = list[i + 2];
                        nested = DesktopFocus::nested_compositor(pid);
                    }
                    focused = nested;
                    break;
                }
            }
            state.focused = focused;
        }
        if (!wait(state, xcb_get_file_descriptor(connection), -1)) {
            return;
        }
        while (auto* event = xcb_poll_for_event(connection)) {
            if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
                const auto atom = reinterpret_cast<xcb_property_notify_event_t*>(event)->atom;
                stale |= atom == focused_window || atom == focusable_windows;
            }
            std::free(event);
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
        wait(*state, -1, retry_milliseconds);
    }
}
} // namespace

DesktopFocus::DesktopFocus(std::string display) : state_(std::make_shared<State>()) {
    state_->display = std::move(display);
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
bool DesktopFocus::focused() const {
    return state_->focused.load();
}
bool DesktopFocus::nested_compositor(std::uint32_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string program;
    std::getline(file, program, '\0');
    return std::filesystem::path(program).filename() == "kwin_wayland";
}
} // namespace framekeyboard
