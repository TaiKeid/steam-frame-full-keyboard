// Desktop focus must never block the VR loop: an X server that accepts the
// connection and stops answering, or none at all, leaves focused() instant.
#include "framekeyboard/desktop_focus.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

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
    } catch (const std::exception& error) {
        std::cerr << "desktop focus test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "desktop focus tests passed\n";
    return 0;
}
