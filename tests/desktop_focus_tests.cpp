// Desktop focus must never block the VR loop: an X server that accepts the
// connection and stops answering, or none at all, leaves focused() instant.
// A focus change XCB reads while waiting for a reply is seen without more traffic.
#include "framekeyboard/desktop_focus.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
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
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
            listen(listener, 4) != 0) {
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
    // Whether window 10, focused first, is still watched for property changes.
    std::atomic<bool> first_watched{};
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
            } else if (head[0] == 20 &&
                       (body[0] | body[1] << 8 | body[2] << 16 | body[3] << 24) != root) {
                // A window's own property, such as its title: unset here.
                put16(out, sequence);
                out.resize(32);
                send_all(out);
            } else if (head[0] == 20) { // GetProperty on the root window
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
            } else if (head[0] == 2 && body.size() >= 12 && (body[4] | body[5] << 8) & 0x800 &&
                       (body[0] | body[1] << 8 | body[2] << 16 | body[3] << 24) == 10) {
                // ChangeWindowAttributes with an event mask, which has no reply.
                first_watched = (body[8] | body[9] << 8 | body[10] << 16 | body[11] << 24) != 0;
            }
        }
    }
};
// A scoped fake for KeyboardFocusProbe. Each GetInputFocus reply waits for an
// explicit permit, so pause/resume races do not depend on scheduler sleeps.
// The focused app changes with each query, or with `steady_app` stays one
// window whose title still does, as a document's name follows its contents.
class ProbeServer {
  public:
    explicit ProbeServer(int listener, bool steady_app = false)
        : listener_(listener), steady_app_(steady_app), worker_([this] { serve(); }) {}
    ~ProbeServer() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        shutdown(listener_, SHUT_RDWR);
        {
            std::lock_guard lock(client_mutex_);
            if (client_ >= 0) {
                shutdown(client_, SHUT_RDWR);
            }
        }
        worker_.join();
        close(listener_);
    }
    bool wait_for_query(unsigned count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [&] { return queries_ >= count || stopping_; }) &&
               queries_ >= count;
    }
    void permit(unsigned count) {
        std::lock_guard lock(mutex_);
        permitted_ = count;
        changed_.notify_all();
    }

  private:
    static constexpr std::uint32_t root = AnsweringServer::root;
    static constexpr std::uint32_t name_atom = 302, target_atom = 303, focus_atom = 304;
    static std::uint32_t word(const std::vector<std::uint8_t>& data, std::size_t offset) {
        return std::uint32_t{data[offset]} | std::uint32_t{data[offset + 1]} << 8 |
               std::uint32_t{data[offset + 2]} << 16 | std::uint32_t{data[offset + 3]} << 24;
    }
    static bool read_exact(int fd, std::uint8_t* data, std::size_t size) {
        for (std::size_t done = 0; done < size;) {
            const auto count = read(fd, data + done, size - done);
            if (count <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    static bool send_all(int fd, const std::vector<std::uint8_t>& data) {
        for (std::size_t done = 0; done < data.size();) {
            const auto count = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (count <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    static std::vector<std::uint8_t> setup_reply() {
        // xcb_setup_t followed by one xcb_screen_t, no formats or depths.
        std::vector<std::uint8_t> reply(80);
        auto put = [&](std::size_t at, std::uint32_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) {
                reply[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
            }
        };
        reply[0] = 1;
        put(2, 11, 2);
        put(6, 18, 2);
        put(12, 0x200000, 4);
        put(16, 0x1fffff, 4);
        put(26, 0xffff, 2);
        reply[28] = 1;
        reply[32] = reply[33] = 32;
        reply[34] = 8;
        reply[35] = 255;
        put(40, root, 4);
        put(44, 0x20, 4);
        put(48, 0xffffff, 4);
        put(60, 1920, 2);
        put(62, 1080, 2);
        put(64, 500, 2);
        put(66, 300, 2);
        put(68, 1, 2);
        put(70, 1, 2);
        put(72, 0x21, 4);
        reply[78] = 24;
        return reply;
    }
    void answer(int fd) {
        std::uint8_t setup[12];
        if (!read_exact(fd, setup, sizeof setup) || setup[0] != 'l') {
            return;
        }
        const auto padded = [](std::size_t n) { return (n + 3) / 4 * 4; };
        std::vector<std::uint8_t> auth(padded(setup[6] | setup[7] << 8) +
                                       padded(setup[8] | setup[9] << 8));
        if (!read_exact(fd, auth.data(), auth.size()) || !send_all(fd, setup_reply())) {
            return;
        }
        std::uint16_t sequence = 0;
        unsigned query = 0;
        while (!stopping_) {
            std::uint8_t head[4];
            if (!read_exact(fd, head, sizeof head)) {
                return;
            }
            const auto length = static_cast<std::size_t>(head[2] | head[3] << 8) * 4;
            if (length < 4 || length > 1024) {
                return;
            }
            std::vector<std::uint8_t> body(length - 4);
            if (!read_exact(fd, body.data(), body.size())) {
                return;
            }
            std::vector<std::uint8_t> reply{1, 0};
            AnsweringServer::put16(reply, ++sequence);
            AnsweringServer::put32(reply, 0);
            if (head[0] == 16 && body.size() >= 4) { // InternAtom
                const std::string name(body.begin() + 4, body.end());
                const auto atom = name.starts_with("_NET_WM_NAME")               ? name_atom
                                  : name.starts_with("GAMESCOPE_FOCUSED_WINDOW") ? focus_atom
                                                                                 : target_atom;
                AnsweringServer::put32(reply, atom);
                reply.resize(32);
            } else if (head[0] == 43) { // GetInputFocus
                std::unique_lock lock(mutex_);
                query = ++queries_;
                changed_.notify_all();
                changed_.wait(lock, [&] { return stopping_ || permitted_ >= query; });
                if (stopping_) {
                    return;
                }
                AnsweringServer::put32(reply, query * 10);
                reply.resize(32);
            } else if (head[0] == 20 && body.size() >= 8) { // GetProperty
                const auto window = word(body, 0), property = word(body, 4);
                std::vector<std::uint8_t> value;
                std::uint32_t type = 0, items = 0;
                if (window == root && property == focus_atom) {
                    reply[1] = 32;
                    type = 6;
                    items = 1;
                    AnsweringServer::put32(value, steady_app_ ? 110 : 100 + query * 10);
                } else if (property == name_atom) {
                    reply[1] = 8;
                    type = 31;
                    const auto title =
                        std::string(window >= 100 ? "app-" : "window-") + std::to_string(query);
                    value.assign(title.begin(), title.end());
                    items = static_cast<std::uint32_t>(value.size());
                } else if (property == target_atom && window >= 100) {
                    reply[1] = 32;
                    type = 6;
                    items = 1;
                    AnsweringServer::put32(value, 1);
                }
                const auto words = static_cast<std::uint32_t>((value.size() + 3) / 4);
                for (unsigned i = 0; i < 4; ++i) {
                    reply[4 + i] = static_cast<std::uint8_t>(words >> (i * 8));
                }
                AnsweringServer::put32(reply, type);
                AnsweringServer::put32(reply, 0);
                AnsweringServer::put32(reply, items);
                reply.resize(32);
                reply.insert(reply.end(), value.begin(), value.end());
                reply.resize(32 + words * 4);
            } else if (head[0] == 15) { // QueryTree
                AnsweringServer::put32(reply, root);
                AnsweringServer::put32(reply, root);
                reply.resize(32);
            } else {
                continue;
            }
            if (!send_all(fd, reply)) {
                return;
            }
        }
    }
    void serve() {
        const int fd = accept(listener_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        {
            std::lock_guard lock(client_mutex_);
            client_ = fd;
        }
        if (!stopping_) {
            answer(fd);
        }
        std::lock_guard lock(client_mutex_);
        close(fd);
        client_ = -1;
    }
    int listener_, client_{-1};
    bool steady_app_;
    std::atomic<bool> stopping_{};
    std::mutex mutex_, client_mutex_;
    std::condition_variable changed_;
    unsigned queries_{}, permitted_{};
    std::thread worker_;
};
void probe_epoch_tests(int number) {
    int listener;
    while ((listener = abstract_listener(number)) < 0) {
        ++number;
    }
    ProbeServer server(listener);
    KeyboardFocusProbe probe(":" + std::to_string(number));
    require(server.wait_for_query(1), "the probe starts its first focus query");
    server.permit(1);
    auto wait_for_sample = [&](std::uint32_t window) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (probe.sample().window.id != window && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return probe.sample();
    };
    const auto initial = wait_for_sample(10);
    require(initial.started > 0 && initial.window.id == 10 && initial.window.title == "window-1" &&
                initial.app.id == 110 && initial.app.title == "app-1" && initial.app.steam &&
                !initial.window.steam,
            "the first sample coherently describes both windows and its query start");
    require(server.wait_for_query(2), "the next pre-pause query is deliberately in flight");
    probe.set_active(false);
    auto empty = probe.sample();
    require(empty.started == 0 && empty.window.id == 0 && empty.app.id == 0 && probe.window().id == 0 &&
                probe.app().id == 0,
            "pausing immediately invalidates the complete sample and legacy accessors");
    const auto resumed_at =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    probe.set_active(true);
    server.permit(2); // Its reply belongs to the old epoch, despite arriving after resume.
    require(server.wait_for_query(3), "the old reply is consumed before the next fresh query");
    empty = probe.sample();
    require(empty.started == 0 && empty.window.id == 0 && empty.app.id == 0,
            "a pre-pause query cannot republish a sample after pause and resume");
    server.permit(3);
    const auto fresh = wait_for_sample(30);
    require(fresh.started >= resumed_at && fresh.window.id == 30 && fresh.window.title == "window-3" &&
                fresh.app.id == 130 && fresh.app.title == "app-3" && fresh.app.steam &&
                !fresh.window.steam,
            "the next fresh query publishes a new coherent sample with a post-resume start");
    require(probe.window().id == fresh.window.id && probe.app().id == fresh.app.id,
            "legacy accessors continue to expose the published sample");
    probe.set_active(false);
    require(probe.sample().started == 0 && probe.sample().window.id == 0 && probe.sample().app.id == 0,
            "pausing a fresh sample invalidates it again");
}
// DashboardFocus matches the focused app's title to Gamescope's overlay
// names, so the probe reads it again while the same app keeps focus.
void probe_title_tests(int number) {
    int listener;
    while ((listener = abstract_listener(number)) < 0) {
        ++number;
    }
    ProbeServer server(listener, true);
    KeyboardFocusProbe probe(":" + std::to_string(number));
    server.permit(3);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (probe.sample().window.id != 30 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto sample = probe.sample();
    require(sample.window.id == 30 && sample.app.id == 110 && sample.app.title == "app-3" &&
                sample.app.steam,
            "the same app's title is read again at each poll, keeping what describe found");
}
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
        for (int i = 0; i < 100 && answering.first_watched; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        require(!answering.first_watched,
                "the window that lost focus is no longer watched for its title");
        probe_epoch_tests(answering_number + 1);
        probe_title_tests(answering_number + 10);
    } catch (const std::exception& error) {
        std::cerr << "desktop focus test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "desktop focus tests passed\n";
    return 0;
}
