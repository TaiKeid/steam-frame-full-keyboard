#include "framekeyboard/text_input.hpp"
#include "gamescope-input-method-client.h"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <glib.h>
#include <poll.h>
#include <stdexcept>
#include <wayland-client.h>

namespace framekeyboard {
namespace {
bool synchronize(wl_display* display) {
    bool completed = false;
    auto* callback = wl_display_sync(display);
    static const wl_callback_listener listener{
        [](void* data, wl_callback*, std::uint32_t) { *static_cast<bool*>(data) = true; }};
    wl_callback_add_listener(callback, &listener, &completed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        if (wl_display_dispatch_pending(display) < 0) {
            break;
        }
        if (completed) {
            break;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            break;
        }
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        if (poll(&fd, 1, 50) > 0 && wl_display_dispatch(display) < 0) {
            break;
        }
    }
    wl_callback_destroy(callback);
    return completed;
}
} // namespace

struct GamescopeText::State {
    wl_display* display{};
    wl_registry* registry{};
    wl_seat* seat{};
    gamescope_input_method_manager* manager{};
    gamescope_input_method* input{};
    std::uint32_t serial{};
    bool available{};
    ~State() {
        if (input) {
            gamescope_input_method_destroy(input);
        }
        if (manager) {
            gamescope_input_method_manager_destroy(manager);
        }
        if (seat) {
            wl_seat_destroy(seat);
        }
        if (registry) {
            wl_registry_destroy(registry);
        }
        if (display) {
            wl_display_flush(display);
            wl_display_disconnect(display);
        }
    }
    static void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                       std::uint32_t) {
        auto& s = *static_cast<State*>(data);
        if (std::strcmp(interface, "wl_seat") == 0 && !s.seat) {
            s.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        }
        if (std::strcmp(interface, "gamescope_input_method_manager") == 0 && !s.manager) {
            s.manager = static_cast<gamescope_input_method_manager*>(
                wl_registry_bind(registry, name, &gamescope_input_method_manager_interface, 1));
        }
    }
    static void removed(void*, wl_registry*, std::uint32_t) {}
    static void unavailable(void* data, gamescope_input_method*) {
        static_cast<State*>(data)->available = false;
    }
    static void done(void* data, gamescope_input_method*, std::uint32_t serial) {
        auto& s = *static_cast<State*>(data);
        s.serial = serial;
        s.available = true;
    }
};
GamescopeText::GamescopeText(const std::string& socket) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.display = wl_display_connect(socket.c_str());
    if (!s.display) {
        throw std::runtime_error("Japanese text connection unavailable");
    }
    s.registry = wl_display_get_registry(s.display);
    static const wl_registry_listener registry_listener{State::global, State::removed};
    wl_registry_add_listener(s.registry, &registry_listener, &s);
    if (!synchronize(s.display) || !s.manager || !s.seat) {
        throw std::runtime_error("Gamescope does not expose Japanese text input");
    }
    s.input = gamescope_input_method_manager_create_input_method(s.manager, s.seat);
    static const gamescope_input_method_listener input_listener{State::unavailable, State::done};
    gamescope_input_method_add_listener(s.input, &input_listener, &s);
    if (!synchronize(s.display) || !s.available) {
        throw std::runtime_error("Gamescope rejected the Japanese text connection");
    }
}
GamescopeText::~GamescopeText() = default;
bool GamescopeText::ready() {
    auto& s = *state_;
    if (!s.available || wl_display_get_error(s.display)) {
        return false;
    }
    // Single-threaded connection: dispatch only already readable events. No idle
    // roundtrips and no blocking wait are added to the VR loop.
    if (wl_display_flush(s.display) < 0 && errno != EAGAIN) {
        s.available = false;
        return false;
    }
    if (wl_display_dispatch_pending(s.display) < 0) {
        return false;
    }
    pollfd fd{wl_display_get_fd(s.display), POLLIN, 0};
    if (poll(&fd, 1, 0) > 0 && wl_display_dispatch(s.display) < 0) {
        s.available = false;
    }
    return s.available;
}
bool GamescopeText::commit(const std::string& text) {
    if (text.empty() || text.find('\0') != std::string::npos ||
        !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) ||
        g_utf8_strlen(text.c_str(), -1) > 32 || !ready()) {
        return false;
    }
    // Bound commits below Gamescope's temporary Unicode-keymap capacity. The
    // compositor injects committed text; ordinary Enter/Ctrl stay on libei.
    auto& s = *state_;
    gamescope_input_method_set_string(s.input, text.c_str());
    gamescope_input_method_commit(s.input, s.serial);
    // Key events use a different socket. Wait until Gamescope processes this
    // commit before the caller forwards Tab, Delete or a Ctrl shortcut over libei.
    // This bounded roundtrip occurs only on commit, never during idle polling.
    if (!synchronize(s.display)) {
        s.available = false;
        return false;
    }
    return s.available;
}
} // namespace framekeyboard
