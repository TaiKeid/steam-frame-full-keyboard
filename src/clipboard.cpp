// This is used only to write into the clipboard for instant dictation
// submission (Send set to Paste). It never reads the clipboard: it takes
// ownership and answers other applications' requests for its own text.
#include "framekeyboard/clipboard.hpp"

#include "ext-data-control-client.h"
#include "wlr-data-control-client.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <xcb/xcb.h>

namespace framekeyboard {
namespace {
// One X display: its connection, a window to own the selection with, and
// the atoms the answers need.
struct Display {
    std::string name;
    xcb_connection_t* connection{};
    xcb_window_t window{};
    xcb_atom_t clipboard{}, targets{}, utf8{}, text{}, atom{};
};

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

// One nested Wayland desktop: its connection and the selection set there.
// Its compositor keeps a clipboard of its own, which data-control sets
// without keyboard focus. Offers it sends back, our own included, are
// destroyed unread. Data-control comes in two protocols with the same
// requests: the standard ext one, which KWin 6.6 offers alone, and wlroots'
// older one; ext_* is used when the compositor offers it.
struct Nested {
    std::string path;
    wl_display* display{};
    wl_registry* registry{};
    wl_seat* seat{};
    ext_data_control_manager_v1* ext_manager{};
    ext_data_control_device_v1* ext_device{};
    ext_data_control_source_v1* ext_source{}; // the current selection, ours
    zwlr_data_control_manager_v1* manager{};
    zwlr_data_control_device_v1* device{};
    zwlr_data_control_source_v1* source{}; // the current selection, ours
    std::string text;                      // what source offers; never logged
    std::atomic<std::uint64_t>* served{};
    bool finished{};
    Nested() = default;
    Nested(const Nested&) = delete;
    Nested& operator=(const Nested&) = delete;
    ~Nested() {
        if (ext_source) {
            ext_data_control_source_v1_destroy(ext_source);
        }
        if (ext_device) {
            ext_data_control_device_v1_destroy(ext_device);
        }
        if (ext_manager) {
            ext_data_control_manager_v1_destroy(ext_manager);
        }
        if (source) {
            zwlr_data_control_source_v1_destroy(source);
        }
        if (device) {
            zwlr_data_control_device_v1_destroy(device);
        }
        if (manager) {
            zwlr_data_control_manager_v1_destroy(manager);
        }
        if (seat) {
            wl_seat_destroy(seat);
        }
        if (registry) {
            wl_registry_destroy(registry);
        }
        if (display) {
            wl_display_disconnect(display); // gives up the clipboard
        }
    }
    bool broken() const { return finished || wl_display_get_error(display) != 0; }
};

void bind_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                 std::uint32_t) {
    auto& nested = *static_cast<Nested*>(data);
    if (!nested.seat && std::strcmp(interface, wl_seat_interface.name) == 0) {
        nested.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (!nested.ext_manager &&
               std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        nested.ext_manager = static_cast<ext_data_control_manager_v1*>(
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
    } else if (!nested.manager &&
               std::strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        // Version 1: no primary selection events.
        nested.manager = static_cast<zwlr_data_control_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 1));
    }
}
void remove_global(void*, wl_registry*, std::uint32_t) {
}
const wl_registry_listener registry_listener{bind_global, remove_global};

// Every offer is followed by a selection naming it; it is destroyed there.
void data_offer(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {
}
void selection(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* offer) {
    if (offer) {
        zwlr_data_control_offer_v1_destroy(offer);
    }
}
void device_finished(void* data, zwlr_data_control_device_v1*) {
    static_cast<Nested*>(data)->finished = true;
}
const zwlr_data_control_device_v1_listener device_listener{data_offer, selection, device_finished,
                                                           selection};
void ext_data_offer(void*, ext_data_control_device_v1*, ext_data_control_offer_v1*) {
}
void ext_selection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
    if (offer) {
        ext_data_control_offer_v1_destroy(offer);
    }
}
void ext_device_finished(void* data, ext_data_control_device_v1*) {
    static_cast<Nested*>(data)->finished = true;
}
const ext_data_control_device_v1_listener ext_device_listener{ext_data_offer, ext_selection,
                                                              ext_device_finished, ext_selection};

// Writes to an application's pipe. SIGPIPE is blocked on the clipboard
// thread; one raised by a reader that went away is taken here.
void write_all(int fd, const std::string& text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const auto written = ::write(fd, text.data() + done, text.size() - done);
        if (written > 0) {
            done += static_cast<std::size_t>(written);
            continue;
        }
        pollfd wait{fd, POLLOUT, 0};
        if (written < 0 && (errno == EINTR || (errno == EAGAIN && poll(&wait, 1, 1000) > 0))) {
            continue;
        }
        if (written < 0 && errno == EPIPE) {
            sigset_t pipe;
            sigemptyset(&pipe);
            sigaddset(&pipe, SIGPIPE);
            const timespec now{};
            sigtimedwait(&pipe, nullptr, &now);
        }
        return;
    }
}
void send_text(void* data, zwlr_data_control_source_v1*, const char*, std::int32_t fd) {
    auto& nested = *static_cast<Nested*>(data);
    write_all(fd, nested.text);
    close(fd);
    ++*nested.served;
}
void cancelled(void* data, zwlr_data_control_source_v1* source) {
    auto& nested = *static_cast<Nested*>(data);
    if (nested.source == source) {
        nested.source = nullptr; // another copy won
    }
    zwlr_data_control_source_v1_destroy(source);
}
const zwlr_data_control_source_v1_listener source_listener{send_text, cancelled};
void ext_send_text(void* data, ext_data_control_source_v1*, const char*, std::int32_t fd) {
    auto& nested = *static_cast<Nested*>(data);
    write_all(fd, nested.text);
    close(fd);
    ++*nested.served;
}
void ext_cancelled(void* data, ext_data_control_source_v1* source) {
    auto& nested = *static_cast<Nested*>(data);
    if (nested.ext_source == source) {
        nested.ext_source = nullptr; // another copy won
    }
    ext_data_control_source_v1_destroy(source);
}
const ext_data_control_source_v1_listener ext_source_listener{ext_send_text, ext_cancelled};

// A roundtrip that gives up: a stopped compositor must not stall the X
// displays this thread also serves.
bool roundtrip(wl_display* display, int milliseconds) {
    bool done = false;
    auto* callback = wl_display_sync(display);
    static const wl_callback_listener listener{
        [](void* data, wl_callback*, std::uint32_t) { *static_cast<bool*>(data) = true; }};
    wl_callback_add_listener(callback, &listener, &done);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done && wl_display_dispatch_pending(display) >= 0 && !done) {
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            break;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }
        if (wl_display_prepare_read(display) != 0) {
            continue; // events already queued: dispatched next round
        }
        pollfd readable{wl_display_get_fd(display), POLLIN, 0};
        if (poll(&readable, 1, static_cast<int>(left)) <= 0) {
            wl_display_cancel_read(display);
            break;
        }
        if (wl_display_read_events(display) < 0) {
            break;
        }
    }
    wl_callback_destroy(callback);
    return done;
}

class XClipboard final : public Clipboard {
  public:
    XClipboard(std::filesystem::path x11_sockets, std::filesystem::path runtime)
        : x11_sockets_(std::move(x11_sockets)), runtime_(std::move(runtime)),
          wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
    ~XClipboard() override {
        running_ = false;
        signal();
        if (thread_.joinable()) {
            thread_.join();
        }
        nested_.clear();
        for (auto& display : displays_) {
            xcb_disconnect(display.connection); // gives up the clipboard
        }
        if (wake_ >= 0) {
            close(wake_);
        }
    }
    std::uint64_t write(const std::string& text) override { return write(text, false); }
    std::uint64_t write_gamescope(const std::string& text) override { return write(text, true); }
    std::uint64_t write(const std::string& text, bool gamescope_only) {
        std::uint64_t number;
        {
            std::lock_guard lock(mutex_);
            text_ = text;
            gamescope_only_ = gamescope_only;
            number = ++requested_;
        }
        if (!thread_.joinable() && wake_ >= 0) {
            thread_ = std::thread([this] { run(); });
        }
        if (wake_ < 0) {
            done_ = number; // owned_ stays behind: reported as failed
        }
        signal();
        return number;
    }
    std::optional<bool> result(std::uint64_t number) const override {
        if (done_.load() < number) {
            return {};
        }
        return owned_.load() >= number;
    }
    std::optional<bool> desktop_result(std::uint64_t number) const override {
        if (done_.load() < number) {
            return {};
        }
        return desktop_owned_.load() >= number;
    }
    std::uint64_t served() const override { return served_.load(); }
    std::optional<std::vector<std::uint64_t>> readers_since(std::uint64_t since) const override {
        std::vector<std::uint64_t> found;
        std::lock_guard lock(readers_mutex_);
        for (const auto& [serial, reader] : readers_) {
            if (serial > since) {
                found.push_back(reader);
            }
        }
        return found;
    }

  private:
    void signal() {
        if (wake_ >= 0) {
            const std::uint64_t one = 1;
            [[maybe_unused]] const auto written = ::write(wake_, &one, sizeof one);
        }
    }
    void connect() {
        // Every X display Gamescope runs has a socket here.
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(x11_sockets_, error)) {
            const auto file = entry.path().filename().string();
            if (file.size() < 2 || file[0] != 'X') {
                continue;
            }
            const auto name = ":" + file.substr(1);
            bool known = false;
            for (const auto& display : displays_) {
                known |= display.name == name;
            }
            if (known) {
                continue;
            }
            // A display that refused us, such as the Desktop's own Xwayland,
            // which wants its auth file, is tried again only once a new
            // server makes its socket anew: libxcb prints every refusal.
            struct stat status{};
            const auto socket = ::stat(entry.path().c_str(), &status) == 0
                                    ? std::pair{status.st_dev, status.st_ino}
                                    : std::pair<dev_t, ino_t>{};
            if (const auto refused = refused_.find(name);
                refused != refused_.end() && refused->second == socket) {
                continue;
            }
            int screen_number = 0;
            auto* connection = xcb_connect(name.c_str(), &screen_number);
            if (xcb_connection_has_error(connection)) {
                xcb_disconnect(connection);
                refused_[name] = socket;
                continue;
            }
            refused_.erase(name);
            auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
            for (int i = 0; i < screen_number && iterator.rem; ++i) {
                xcb_screen_next(&iterator);
            }
            Display display{name, connection};
            display.window = xcb_generate_id(connection);
            xcb_create_window(connection, XCB_COPY_FROM_PARENT, display.window, iterator.data->root, 0,
                              0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
            display.clipboard = intern(connection, "CLIPBOARD");
            display.targets = intern(connection, "TARGETS");
            display.utf8 = intern(connection, "UTF8_STRING");
            display.text = intern(connection, "TEXT");
            display.atom = XCB_ATOM_ATOM;
            displays_.push_back(display);
        }
    }
    // Nested desktops put their Wayland socket in a subdirectory of the
    // runtime directory (SteamOS: nested_plasma/wayland-0). Only compositors
    // offering data-control are kept; others are tried again on a later write.
    void connect_nested() {
        auto runtime = runtime_;
        if (runtime.empty()) {
            const char* variable = std::getenv("XDG_RUNTIME_DIR");
            if (!variable) {
                return;
            }
            runtime = variable;
        }
        std::error_code error;
        for (const auto& directory : std::filesystem::directory_iterator(runtime, error)) {
            if (!directory.is_directory(error) || directory.is_symlink(error)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(directory.path(), error)) {
                const auto file = entry.path().filename().string();
                const auto path = entry.path().string();
                if (!file.starts_with("wayland-") || !entry.is_socket(error) ||
                    std::any_of(nested_.begin(), nested_.end(),
                                [&](const auto& nested) { return nested->path == path; })) {
                    continue;
                }
                auto nested = std::make_unique<Nested>();
                nested->path = path;
                nested->served = &served_;
                nested->display = wl_display_connect(path.c_str()); // absolute: used as is
                if (!nested->display) {
                    continue;
                }
                nested->registry = wl_display_get_registry(nested->display);
                wl_registry_add_listener(nested->registry, &registry_listener, nested.get());
                if (!roundtrip(nested->display, 500) || !nested->seat ||
                    (!nested->ext_manager && !nested->manager)) {
                    continue;
                }
                if (nested->ext_manager) {
                    if (nested->manager) {
                        zwlr_data_control_manager_v1_destroy(nested->manager);
                        nested->manager = nullptr;
                    }
                    nested->ext_device =
                        ext_data_control_manager_v1_get_data_device(nested->ext_manager, nested->seat);
                    ext_data_control_device_v1_add_listener(nested->ext_device, &ext_device_listener,
                                                            nested.get());
                } else {
                    nested->device =
                        zwlr_data_control_manager_v1_get_data_device(nested->manager, nested->seat);
                    zwlr_data_control_device_v1_add_listener(nested->device, &device_listener,
                                                             nested.get());
                }
                nested_.push_back(std::move(nested));
            }
        }
    }
    // Replaces our selection in a nested desktop; true unless it was refused
    // or the compositor did not answer.
    static bool own(Nested& nested, const std::string& text) {
        if (nested.ext_manager) {
            auto* source = ext_data_control_manager_v1_create_data_source(nested.ext_manager);
            for (const char* type : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT"}) {
                ext_data_control_source_v1_offer(source, type);
            }
            ext_data_control_source_v1_add_listener(source, &ext_source_listener, &nested);
            ext_data_control_device_v1_set_selection(nested.ext_device, source);
            if (nested.ext_source) {
                ext_data_control_source_v1_destroy(nested.ext_source);
            }
            nested.ext_source = source;
            nested.text = text;
            return roundtrip(nested.display, 500) && nested.ext_source == source;
        }
        auto* source = zwlr_data_control_manager_v1_create_data_source(nested.manager);
        for (const char* type : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT"}) {
            zwlr_data_control_source_v1_offer(source, type);
        }
        zwlr_data_control_source_v1_add_listener(source, &source_listener, &nested);
        zwlr_data_control_device_v1_set_selection(nested.device, source);
        // The old source goes now, so a late request cannot get the new text from it.
        if (nested.source) {
            zwlr_data_control_source_v1_destroy(nested.source);
        }
        nested.source = source;
        nested.text = text;
        return roundtrip(nested.display, 500) && nested.source == source;
    }
    // Takes the clipboard on every display; true when at least one holds it.
    // `desktop` says whether a nested desktop does, the one its own
    // applications paste from: Gamescope's X displays are not theirs.
    // `gamescope_only` leaves the nested desktops' clipboards as they are.
    bool own(const std::string& text, bool& desktop, bool gamescope_only) {
        bool any = false;
        desktop = false;
        for (auto& nested : nested_) {
            desktop |= !gamescope_only && own(*nested, text);
        }
        any = desktop;
        for (auto& display : displays_) {
            xcb_set_selection_owner(display.connection, display.window, display.clipboard,
                                    XCB_CURRENT_TIME);
            auto cookie = xcb_get_selection_owner(display.connection, display.clipboard);
            auto* reply = xcb_get_selection_owner_reply(display.connection, cookie, nullptr);
            any |= reply && reply->owner == display.window;
            std::free(reply);
        }
        return any;
    }
    void answer(Display& display, const xcb_selection_request_event_t& request,
                const std::string& text) {
        xcb_atom_t property = request.property == XCB_ATOM_NONE ? request.target : request.property;
        if (request.target == display.targets) {
            const xcb_atom_t offered[]{display.targets, display.utf8, display.text};
            xcb_change_property(display.connection, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                display.atom, 32, 3, offered);
        } else if (request.target == display.utf8 || request.target == display.text) {
            xcb_change_property(display.connection, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                display.utf8, 8, static_cast<std::uint32_t>(text.size()), text.data());
            // Who asked: a display and its window, never what was asked for.
            const auto reader = std::hash<std::string>{}(display.name) * 31 + request.requestor;
            std::lock_guard lock(readers_mutex_);
            readers_.emplace_back(++served_, reader);
            if (readers_.size() > 64) {
                readers_.pop_front();
            }
        } else {
            property = XCB_ATOM_NONE; // a format we do not offer
        }
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = request.time;
        notify.requestor = request.requestor;
        notify.selection = request.selection;
        notify.target = request.target;
        notify.property = property;
        xcb_send_event(display.connection, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char*>(&notify));
        xcb_flush(display.connection);
    }
    void run() {
        sigset_t pipe;
        sigemptyset(&pipe);
        sigaddset(&pipe, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe, nullptr); // write_all() takes it instead
        std::uint64_t handled = 0;
        while (running_) {
            std::vector<pollfd> fds{{wake_, POLLIN, 0}};
            for (const auto& display : displays_) {
                fds.push_back({xcb_get_file_descriptor(display.connection), POLLIN, 0});
            }
            for (const auto& nested : nested_) {
                wl_display_dispatch_pending(nested->display);
                wl_display_flush(nested->display);
                fds.push_back({wl_display_get_fd(nested->display), POLLIN, 0});
            }
            poll(fds.data(), fds.size(), -1);
            for (std::size_t i = 0; i < nested_.size(); ++i) {
                if (fds[1 + displays_.size() + i].revents) {
                    wl_display_dispatch(nested_[i]->display); // readable: does not block
                }
            }
            std::uint64_t count;
            [[maybe_unused]] const auto read_bytes = ::read(wake_, &count, sizeof count);
            std::string text;
            std::uint64_t requested;
            bool gamescope_only = false;
            {
                std::lock_guard lock(mutex_);
                text = text_;
                requested = requested_;
                gamescope_only = gamescope_only_;
            }
            if (requested != handled && running_) {
                connect();
                connect_nested();
                bool desktop = false;
                if (own(text, desktop, gamescope_only)) {
                    owned_ = requested;
                }
                if (desktop) {
                    desktop_owned_ = requested;
                }
                done_ = requested;
                handled = requested;
            }
            for (auto& display : displays_) {
                while (auto* event = xcb_poll_for_event(display.connection)) {
                    if ((event->response_type & 0x7f) == XCB_SELECTION_REQUEST) {
                        answer(display, *reinterpret_cast<xcb_selection_request_event_t*>(event), text);
                    }
                    std::free(event); // a selection clear needs nothing: another copy won
                }
            }
            // A display that went away is dropped and found again on the next write.
            std::erase_if(nested_, [](const auto& nested) { return nested->broken(); });
            std::erase_if(displays_, [](const Display& display) {
                if (!xcb_connection_has_error(display.connection)) {
                    return false;
                }
                xcb_disconnect(display.connection);
                return true;
            });
        }
    }
    std::filesystem::path x11_sockets_, runtime_;
    int wake_;
    std::thread thread_;
    std::mutex mutex_;
    std::string text_; // never logged
    bool gamescope_only_{};
    std::uint64_t requested_{};
    // The latest X reads: served()'s count after each, and its reader.
    mutable std::mutex readers_mutex_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> readers_;
    std::atomic<std::uint64_t> done_{}, owned_{}, desktop_owned_{}, served_{};
    std::atomic<bool> running_{true};
    std::vector<Display> displays_;               // the thread's own
    std::vector<std::unique_ptr<Nested>> nested_; // the thread's own
    // The thread's own: each display that refused us, and its socket then.
    std::map<std::string, std::pair<dev_t, ino_t>> refused_;
};
} // namespace

std::unique_ptr<Clipboard> make_clipboard(std::filesystem::path x11_sockets,
                                          std::filesystem::path runtime) {
    return std::make_unique<XClipboard>(std::move(x11_sockets), std::move(runtime));
}
} // namespace framekeyboard
