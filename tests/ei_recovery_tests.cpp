#include "framekeyboard/ei_input.hpp"
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <libeis.h>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
// This compositor exists only inside the test. Its keys never reach the desktop.
struct Server {
    eis* context{eis_new(nullptr)};
    eis_client* client{};
    eis_seat* seat{};
    eis_device* device{};
    std::vector<std::pair<unsigned, bool>> keys;
    fs::path directory;
    Server() {
        std::string temporary = "/tmp/full-keyboard-eis-XXXXXX";
        require(mkdtemp(temporary.data()), "temporary directory");
        directory = temporary;
        require(context && eis_set_flag(context, EIS_FLAG_DEVICE_READY) == 0, "device-ready protocol");
        require(eis_setup_backend_socket(context, (directory / "input").c_str()) == 0,
                "isolated EIS socket");
    }
    ~Server() {
        if (device) {
            eis_device_remove(device);
            eis_device_unref(device);
        }
        if (seat) {
            eis_seat_remove(seat);
            eis_seat_unref(seat);
        }
        if (client) {
            eis_client_unref(client);
        }
        eis_unref(context);
        fs::remove_all(directory);
    }
    void pump() {
        eis_dispatch(context);
        while (auto* event = eis_get_event(context)) {
            switch (eis_event_get_type(event)) {
            case EIS_EVENT_CLIENT_CONNECT:
                client = eis_client_ref(eis_event_get_client(event));
                eis_client_connect(client);
                seat = eis_client_new_seat(client, "test");
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_add(seat);
                break;
            case EIS_EVENT_SEAT_BIND: {
                if (device || !eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
                    break;
                }
                device = eis_seat_new_device(seat);
                eis_device_configure_name(device, "isolated test keyboard");
                eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
                auto* xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                auto* map = xkb_keymap_new_from_names(xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
                char* text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
                const auto size = std::strlen(text) + 1;
                const int fd = memfd_create("test-keymap", MFD_CLOEXEC);
                require(fd >= 0 && write(fd, text, size) == static_cast<ssize_t>(size), "keymap file");
                auto* keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd, size);
                require(keymap, "EIS keymap");
                eis_keymap_add(keymap);
                eis_keymap_unref(keymap);
                close(fd);
                free(text);
                xkb_keymap_unref(map);
                xkb_context_unref(xkb);
                eis_device_add(device);
                break;
            }
            case EIS_EVENT_DEVICE_READY:
                eis_device_resume(eis_event_get_device(event));
                break;
            case EIS_EVENT_KEYBOARD_KEY:
                keys.emplace_back(eis_event_keyboard_get_key(event),
                                  eis_event_keyboard_get_key_is_press(event));
                break;
            default:
                break;
            }
            eis_event_unref(event);
        }
    }
    template <class Condition> void until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (condition()) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("EIS test timed out");
    }
};
} // namespace
int main() {
    try {
        Server server;
        auto connecting = std::async(
            std::launch::async, [&] { return std::make_unique<EiSink>(server.directory / "input"); });
        server.until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
        auto sink = connecting.get();
        sink->take_input_reset();
        sink->send(30, 1);
        server.until([&] { return server.keys.size() == 1; });
        eis_device_pause(server.device);
        server.until([&] { return !sink->pump(); });
        require(sink->can_resume() && sink->take_input_reset(), "pause is recoverable and clears holds");
        sink->send(31, 1);
        eis_device_resume(server.device);
        server.until([&] { return sink->pump(); });
        sink->send(30, 0); // Old key was released by EIS at pause, not replayed.
        sink->send(32, 1);
        sink->send(32, 0);
        server.until([&] { return server.keys.size() >= 3; });
        require(server.keys ==
                    std::vector<std::pair<unsigned, bool>>{{30, true}, {32, true}, {32, false}},
                "resume permits fresh keys but never paused or stale keys");
        eis_device_pause(server.device);
        eis_device_resume(server.device);
        server.pump();
        sink->send(33, 1); // Must not pass the reset barrier before the app clears its UI holds.
        require(sink->take_input_reset(), "same-batch pause/resume retains reset notification");
        require(sink->pump(), "same-batch pause/resume stays usable after reset acknowledgement");
        sink->send(34, 1);
        sink->send(34, 0);
        server.until([&] { return server.keys.size() >= 5; });
        require(server.keys[3] == std::pair<unsigned, bool>{34, true} &&
                    server.keys[4] == std::pair<unsigned, bool>{34, false},
                "output before interruption acknowledgement is suppressed");
        eis_client_disconnect(server.client);
        server.until([&] { return !sink->pump(); });
        require(!sink->can_resume() && sink->take_input_reset(), "disconnect requires a new connection");
        std::cout << "Real libei pause/resume, stale-key suppression and disconnect passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
