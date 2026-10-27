// The nested desktop clipboard against a minimal data-control compositor:
// a write becomes its selection, a paste request gets the text, and the
// offers it sends back are destroyed without being received.
#include "framekeyboard/clipboard.hpp"

#include "wlr-data-control-server.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <wayland-server.h>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Compositor {
    wl_display* display = wl_display_create();
    wl_resource* device{};
    wl_resource* selection{}; // the client's current source
    std::vector<std::string> types;
    int offers_destroyed{}, received{};
    ~Compositor() { wl_display_destroy(display); }
    void pump() {
        wl_event_loop_dispatch(wl_display_get_event_loop(display), 5);
        wl_display_flush_clients(display);
    }
    template <typename Done> bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            pump();
        }
        return done();
    }
};
Compositor* compositor{};

void destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void unused_seat_object(wl_client*, wl_resource*, std::uint32_t) {}
const struct wl_seat_interface seat_implementation{unused_seat_object, unused_seat_object,
                                                   unused_seat_object, destroy};

void offer_type(wl_client*, wl_resource*, const char* type) {
    compositor->types.push_back(type);
}
const struct zwlr_data_control_source_v1_interface source_implementation{offer_type, destroy};

void receive(wl_client*, wl_resource*, const char*, std::int32_t fd) {
    ++compositor->received;
    close(fd);
}
const struct zwlr_data_control_offer_v1_interface offer_implementation{receive, destroy};

void set_selection(wl_client* client, wl_resource* device, wl_resource* source) {
    compositor->selection = source;
    // Like a compositor, announce the new selection to every data device,
    // the one that set it included.
    auto* offer = wl_resource_create(client, &zwlr_data_control_offer_v1_interface, 1, 0);
    wl_resource_set_implementation(offer, &offer_implementation, nullptr, [](wl_resource*) {
        ++compositor->offers_destroyed;
    });
    zwlr_data_control_device_v1_send_data_offer(device, offer);
    zwlr_data_control_offer_v1_send_offer(offer, "text/plain;charset=utf-8");
    zwlr_data_control_device_v1_send_selection(device, offer);
}
void unused_primary(wl_client*, wl_resource*, wl_resource*) {}
const struct zwlr_data_control_device_v1_interface device_implementation{set_selection, destroy,
                                                                         unused_primary};

void create_source(wl_client* client, wl_resource*, std::uint32_t id) {
    auto* source = wl_resource_create(client, &zwlr_data_control_source_v1_interface, 1, id);
    wl_resource_set_implementation(source, &source_implementation, nullptr, [](wl_resource* gone) {
        if (compositor->selection == gone) {
            compositor->selection = nullptr;
        }
    });
}
void get_device(wl_client* client, wl_resource*, std::uint32_t id, wl_resource*) {
    compositor->device = wl_resource_create(client, &zwlr_data_control_device_v1_interface, 1, id);
    wl_resource_set_implementation(compositor->device, &device_implementation, nullptr, nullptr);
}
const struct zwlr_data_control_manager_v1_interface manager_implementation{create_source,
                                                                           get_device, destroy};

int listen_at(const std::filesystem::path& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof address.sun_path, "%s", path.c_str());
    require(fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                listen(fd, 4) == 0,
            "listen");
    return fd;
}

// The text a paste request gets, read as an application would.
std::string paste(Compositor& server) {
    int pipe_fds[2];
    require(pipe(pipe_fds) == 0, "pipe");
    zwlr_data_control_source_v1_send_send(server.selection, "text/plain;charset=utf-8", pipe_fds[1]);
    close(pipe_fds[1]);
    server.pump();
    std::string text;
    char buffer[256];
    for (;;) {
        server.pump();
        const auto count = read(pipe_fds[0], buffer, sizeof buffer);
        if (count <= 0) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);
    return text;
}

void nested_desktop() {
    const auto runtime = std::filesystem::temp_directory_path() /
                         ("framekeyboard-clipboard-" + std::to_string(getpid()));
    std::filesystem::create_directories(runtime / "nested");
    Compositor server;
    compositor = &server;
    wl_global_create(server.display, &wl_seat_interface, 1, nullptr,
                     [](wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
                         auto* seat = wl_resource_create(client, &wl_seat_interface,
                                                         static_cast<int>(version), id);
                         wl_resource_set_implementation(seat, &seat_implementation, nullptr, nullptr);
                     });
    wl_global_create(server.display, &zwlr_data_control_manager_v1_interface, 1, nullptr,
                     [](wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
                         auto* manager = wl_resource_create(
                             client, &zwlr_data_control_manager_v1_interface, static_cast<int>(version), id);
                         wl_resource_set_implementation(manager, &manager_implementation, nullptr, nullptr);
                     });
    // The compositor's socket sits one directory below the runtime directory
    // the keyboard sees, as SteamOS's Desktop does. No X sockets: the test
    // must not take a real session's clipboard.
    require(wl_display_add_socket_fd(server.display, listen_at(runtime / "nested" / "wayland-0")) == 0,
            "socket");
    std::filesystem::create_directories(runtime / "x11");
    auto clipboard = make_clipboard(runtime / "x11", runtime);
    const auto first = clipboard->write("あいう");
    require(server.pump_until([&] { return clipboard->result(first).has_value(); }), "write finishes");
    require(*clipboard->result(first), "the nested desktop takes the clipboard");
    require(server.selection, "the selection is set");
    require(server.types.size() == 4 && server.types[0] == "text/plain;charset=utf-8", "text types offered");
    require(paste(server) == "あいう" && clipboard->served() == 1, "a paste gets the text");

    const auto second = clipboard->write("かきく");
    require(server.pump_until([&] { return clipboard->result(second).has_value(); }), "second write");
    require(server.pump_until([&] { return server.offers_destroyed == 2; }),
            "offers are destroyed, our own included");
    require(paste(server) == "かきく" && clipboard->served() == 2, "the new text replaces the old");
    require(server.received == 0, "the clipboard is never read");

    clipboard.reset();
    compositor = nullptr;
    std::filesystem::remove_all(runtime);
}
} // namespace

int main() {
    try {
        nested_desktop();
    } catch (const std::exception& error) {
        std::cerr << "clipboard test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "clipboard tests passed\n";
}
