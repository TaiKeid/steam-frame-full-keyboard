#include "framekeyboard/desktop_focus.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <xcb/xcbext.h>

namespace framekeyboard {
namespace {
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
std::span<const std::uint32_t> cardinals(const xcb_get_property_reply_t* reply) {
    if (!reply || reply->format != 32) {
        return {};
    }
    return {static_cast<const std::uint32_t*>(xcb_get_property_value(reply)),
            static_cast<std::size_t>(xcb_get_property_value_length(reply)) / 4};
}
} // namespace

DesktopFocus::DesktopFocus(std::string display) : display_(std::move(display)) {}
DesktopFocus::~DesktopFocus() {
    disconnect();
}
bool DesktopFocus::nested_compositor(std::uint32_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string program;
    std::getline(file, program, '\0');
    return std::filesystem::path(program).filename() == "kwin_wayland";
}
bool DesktopFocus::connect() {
    int screen_number = 0;
    connection_ = xcb_connect(display_.c_str(), &screen_number);
    if (xcb_connection_has_error(connection_)) {
        disconnect();
        return false;
    }
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    for (int i = 0; i < screen_number && screens.rem; ++i) {
        xcb_screen_next(&screens);
    }
    root_ = screens.data->root;
    focused_window_ = intern(connection_, "GAMESCOPE_FOCUSED_WINDOW");
    focusable_windows_ = intern(connection_, "GAMESCOPE_FOCUSABLE_WINDOWS");
    const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection_, root_, XCB_CW_EVENT_MASK, &mask);
    stale_ = true;
    return focused_window_ != XCB_ATOM_NONE && focusable_windows_ != XCB_ATOM_NONE;
}
void DesktopFocus::disconnect() {
    // Replies still owed are discarded with the connection.
    std::free(window_reply_);
    std::free(list_reply_);
    window_reply_ = list_reply_ = nullptr;
    window_request_.reset();
    list_request_.reset();
    if (connection_) {
        xcb_disconnect(connection_);
        connection_ = nullptr;
    }
    focused_ = false;
}
void DesktopFocus::request() {
    window_request_ =
        xcb_get_property(connection_, 0, root_, focused_window_, XCB_ATOM_CARDINAL, 0, 1);
    // (window, app, pid) triplets; Gamescope lists few windows.
    list_request_ =
        xcb_get_property(connection_, 0, root_, focusable_windows_, XCB_ATOM_CARDINAL, 0, 3 * 256);
    xcb_flush(connection_);
}
void DesktopFocus::collect() {
    for (auto [request, reply] : {std::pair{&window_request_, &window_reply_},
                                  std::pair{&list_request_, &list_reply_}}) {
        void* answer = nullptr;
        xcb_generic_error_t* error = nullptr;
        if (request->has_value() && xcb_poll_for_reply(connection_, (*request)->sequence, &answer, &error)) {
            *reply = static_cast<xcb_get_property_reply_t*>(answer);
            std::free(error);
            request->reset();
        }
    }
    if (window_request_ || list_request_) {
        return;
    }
    focused_ = false;
    const auto window = cardinals(window_reply_);
    const auto list = cardinals(list_reply_);
    for (std::size_t i = 0; !window.empty() && i + 2 < list.size(); i += 3) {
        if (list[i] == window[0]) {
            // The command line is read once per newly focused process.
            if (list[i + 2] != pid_) {
                pid_ = list[i + 2];
                nested_ = nested_compositor(pid_);
            }
            focused_ = nested_;
            break;
        }
    }
    std::free(window_reply_);
    std::free(list_reply_);
    window_reply_ = list_reply_ = nullptr;
}
bool DesktopFocus::focused() {
    const auto now = std::chrono::steady_clock::now();
    if (!connection_) {
        if (now < retry_at_) {
            return false;
        }
        if (!connect()) {
            disconnect();
            retry_at_ = now + std::chrono::seconds(5);
            return false;
        }
    }
    if (xcb_connection_has_error(connection_)) {
        disconnect();
        retry_at_ = now + std::chrono::seconds(5);
        return false;
    }
    while (auto* event = xcb_poll_for_event(connection_)) {
        if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
            const auto atom = reinterpret_cast<xcb_property_notify_event_t*>(event)->atom;
            stale_ |= atom == focused_window_ || atom == focusable_windows_;
        }
        std::free(event);
    }
    if (stale_ && !window_request_ && !list_request_) {
        stale_ = false;
        request();
    }
    if (window_request_ || list_request_) {
        collect();
    }
    return focused_;
}
} // namespace framekeyboard
