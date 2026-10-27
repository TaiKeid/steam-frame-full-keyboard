// Desktop focus must never block the VR loop: an X server that accepts the
// connection and stops answering, or none at all, leaves focused() instant.
// A focus change XCB reads while waiting for a reply is seen without more traffic.
#include "framekeyboard/desktop_focus.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace framekeyboard;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
// Listens where libxcb looks first for display `number` on Linux, an abstract
// socket, so nothing is created in the shared /tmp/.X11-unix. It reads the
// connection setup and never answers.
struct SilentServer {
    int listener{-1};
    std::atomic<bool> connected{};
    bool listen_on(int number) {
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = "/tmp/.X11-unix/X" + std::to_string(number);
        name.copy(address.sun_path + 1, name.size());
        const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), size) != 0 || listen(listener, 4) != 0) {
            close(listener);
            listener = -1;
            return false;
        }
        // Detached: it holds the connection open until the test exits.
        std::thread([this] {
            const int client = accept(listener, nullptr, nullptr);
            char setup[64];
            if (client >= 0 && read(client, setup, sizeof setup) > 0) {
                connected = true;
            }
            for (;;) {
                pause();
            }
        }).detach();
        return true;
    }
};
// Binds the abstract socket libxcb tries first for display `number`.
int abstract_listener(int number) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = "/tmp/.X11-unix/X" + std::to_string(number);
    name.copy(address.sun_path + 1, name.size());
    const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), size) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
// Just enough of an X server for the watcher: the setup, InternAtom,
// ChangeWindowAttributes and GetProperty on the root window. GAMESCOPE's
// focus moves from window 10 (a nested compositor) to 20 (this process) as
// the focusable list is read, with the PropertyNotify sent just before that
// reply: XCB then reads the event while it waits for the reply.
struct AnsweringServer {
    static constexpr std::uint32_t root = 0x100, focused_atom = 300, list_atom = 301;
    int listener{-1};
    pid_t compositor{};
    std::atomic<int> property_queries{};
    std::uint16_t sequence{};
    int client{-1};
    bool start(int number, pid_t nested) {
        compositor = nested;
        listener = abstract_listener(number);
        if (listener < 0) {
            return false;
        }
        std::thread([this] { serve(); }).detach();
        return true;
    }
    static void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }
    static void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    void send_all(const std::vector<std::uint8_t>& data) {
        for (std::size_t done = 0; done < data.size();) {
            const auto written = write(client, data.data() + done, data.size() - done);
            if (written <= 0) {
                return;
            }
            done += static_cast<std::size_t>(written);
        }
    }
    bool read_exact(std::uint8_t* data, std::size_t size) {
        for (std::size_t done = 0; done < size;) {
            const auto got = read(client, data + done, size - done);
            if (got <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(got);
        }
        return true;
    }
    void property_notify(std::uint32_t atom) {
        std::vector<std::uint8_t> event{28, 0};
        put16(event, sequence);
        put32(event, root);
        put32(event, atom);
        put32(event, 0); // time
        event.resize(32);
        send_all(event);
    }
    void serve() {
        client = accept(listener, nullptr, nullptr);
        std::uint8_t setup[12];
        if (client < 0 || !read_exact(setup, sizeof setup) || setup[0] != 'l') {
            return;
        }
        const auto padded = [](std::size_t n) { return (n + 3) / 4 * 4; };
        std::vector<std::uint8_t> auth(padded(setup[6] | setup[7] << 8) +
                                       padded(setup[8] | setup[9] << 8));
        if (!read_exact(auth.data(), auth.size())) {
            return;
        }
        // Success, protocol 11.0, then the fixed part, no vendor or formats, one screen.
        std::vector<std::uint8_t> reply{1, 0};
        put16(reply, 11);
        put16(reply, 0);
        put16(reply, (32 + 40) / 4);
        put32(reply, 0);        // release
        put32(reply, 0x200000); // resource id base
        put32(reply, 0x1fffff); // resource id mask
        put32(reply, 0);        // motion buffer
        put16(reply, 0);        // vendor length
        put16(reply, 0xffff);   // maximum request length
        reply.insert(reply.end(), {1, 0, 0, 0, 32, 32, 8, 255, 0, 0, 0, 0});
        put32(reply, root);
        put32(reply, 0x20);     // colormap
        put32(reply, 0xffffff); // white
        put32(reply, 0);        // black
        put32(reply, 0);        // input masks
        put16(reply, 1920);
        put16(reply, 1080);
        put16(reply, 500);
        put16(reply, 300);
        put16(reply, 1);
        put16(reply, 1);
        put32(reply, 0x21); // root visual
        reply.insert(reply.end(), {0, 0, 24, 0});
        send_all(reply);
        std::uint32_t focus = 10;
        for (;;) {
            std::uint8_t head[4];
            if (!read_exact(head, sizeof head)) {
                return;
            }
            ++sequence;
            std::vector<std::uint8_t> body(static_cast<std::size_t>((head[2] | head[3] << 8) * 4 - 4));
            if (!read_exact(body.data(), body.size())) {
                return;
            }
            std::vector<std::uint8_t> out{1, 0};
            if (head[0] == 16) { // InternAtom
                const std::string name(body.begin() + 4, body.begin() + 4 + (body[0] | body[1] << 8));
                put16(out, sequence);
                put32(out, 0);
                put32(out, name == "GAMESCOPE_FOCUSED_WINDOW" ? focused_atom : list_atom);
                out.resize(32);
                send_all(out);
            } else if (head[0] == 20) { // GetProperty
                const std::uint32_t property = body[4] | body[5] << 8 | body[6] << 16 | body[7] << 24;
                std::vector<std::uint32_t> values;
                if (property == focused_atom) {
                    values = {focus};
                } else {
                    if (focus == 10) {
                        // Focus moves on, once: its notification arrives before this reply.
                        focus = 20;
                        property_notify(focused_atom);
                    }
                    values = {10, 0, static_cast<std::uint32_t>(compositor),
                              20, 0, static_cast<std::uint32_t>(getpid())};
                }
                ++property_queries;
                out[1] = 32;
                put16(out, sequence);
                put32(out, static_cast<std::uint32_t>(values.size()));
                put32(out, 6); // CARDINAL
                put32(out, 0); // bytes after
                put32(out, static_cast<std::uint32_t>(values.size()));
                out.resize(32);
                for (const auto value : values) {
                    put32(out, value);
                }
                send_all(out);
            } // ChangeWindowAttributes has no reply
        }
    }
};
template <class Call> std::chrono::steady_clock::duration timed(Call call) {
    const auto start = std::chrono::steady_clock::now();
    call();
    return std::chrono::steady_clock::now() - start;
}
} // namespace

int main() {
    try {
        static SilentServer server;
        int number = 40000 + getpid() % 10000;
        while (!server.listen_on(number)) {
            ++number;
        }
        auto silent = std::make_unique<DesktopFocus>(":" + std::to_string(number));
        for (int i = 0; i < 300 && !server.connected; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        require(server.connected, "the watcher reaches the silent server");
        std::this_thread::sleep_for(100ms); // it now waits for a setup reply that never comes
        for (int i = 0; i < 20; ++i) {
            bool focused = true;
            require(timed([&] { focused = silent->focused(); }) < 5ms && !focused,
                    "a silent server never blocks focused(), which stays false");
        }
        require(timed([&] { silent.reset(); }) < 5ms, "destroying it does not wait for the server");
        DesktopFocus absent(":" + std::to_string(number + 1));
        std::this_thread::sleep_for(50ms);
        require(timed([&] { (void)absent.focused(); }) < 5ms && !absent.focused(),
                "no server: false, at once");
        require(!DesktopFocus::nested_compositor(static_cast<std::uint32_t>(getpid())),
                "this test is not a nested compositor");

        // A focus change whose notification XCB read while waiting for a
        // reply is seen without any further traffic from the server.
        const pid_t nested = fork();
        if (nested == 0) {
            char program[] = "kwin_wayland", seconds[] = "30";
            char* arguments[] = {program, seconds, nullptr};
            execv("/bin/sleep", arguments);
            _exit(1);
        }
        std::this_thread::sleep_for(50ms); // until it runs as kwin_wayland
        require(DesktopFocus::nested_compositor(static_cast<std::uint32_t>(nested)),
                "the stand-in runs as kwin_wayland");
        static AnsweringServer answering;
        int answering_number = number + 2;
        while (!answering.start(answering_number, nested)) {
            ++answering_number;
        }
        DesktopFocus moving(":" + std::to_string(answering_number));
        for (int i = 0; i < 200 && (answering.property_queries < 4 || moving.focused()); ++i) {
            std::this_thread::sleep_for(10ms);
        }
        kill(nested, SIGKILL);
        waitpid(nested, nullptr, 0);
        require(answering.property_queries == 4 && !moving.focused(),
                "the queued focus change is read at once: no longer the Desktop");
    } catch (const std::exception& error) {
        std::cerr << "desktop focus test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "desktop focus tests passed\n";
    return 0;
}
