// This is used only to write into the clipboard for instant dictation
// submission (Send set to Paste). It never reads the clipboard: it takes
// ownership and answers other applications' requests for its own text.
#include "framekeyboard/clipboard.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
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

class XClipboard final : public Clipboard {
  public:
    XClipboard() : wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
    ~XClipboard() override {
        running_ = false;
        signal();
        if (thread_.joinable()) {
            thread_.join();
        }
        for (auto& display : displays_) {
            xcb_disconnect(display.connection); // gives up the clipboard
        }
        if (wake_ >= 0) {
            close(wake_);
        }
    }
    std::uint64_t write(const std::string& text) override {
        std::uint64_t number;
        {
            std::lock_guard lock(mutex_);
            text_ = text;
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
    std::uint64_t served() const override { return served_.load(); }

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
        for (const auto& entry : std::filesystem::directory_iterator("/tmp/.X11-unix", error)) {
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
            int screen_number = 0;
            auto* connection = xcb_connect(name.c_str(), &screen_number);
            if (xcb_connection_has_error(connection)) {
                xcb_disconnect(connection);
                continue;
            }
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
    // Takes the clipboard on every display; true when at least one holds it.
    bool own() {
        bool any = false;
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
            ++served_;
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
        std::uint64_t handled = 0;
        while (running_) {
            std::vector<pollfd> fds{{wake_, POLLIN, 0}};
            for (const auto& display : displays_) {
                fds.push_back({xcb_get_file_descriptor(display.connection), POLLIN, 0});
            }
            poll(fds.data(), fds.size(), -1);
            std::uint64_t count;
            [[maybe_unused]] const auto read_bytes = ::read(wake_, &count, sizeof count);
            std::string text;
            std::uint64_t requested;
            {
                std::lock_guard lock(mutex_);
                text = text_;
                requested = requested_;
            }
            if (requested != handled && running_) {
                connect();
                if (own()) {
                    owned_ = requested;
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
            std::erase_if(displays_, [](const Display& display) {
                if (!xcb_connection_has_error(display.connection)) {
                    return false;
                }
                xcb_disconnect(display.connection);
                return true;
            });
        }
    }
    int wake_;
    std::thread thread_;
    std::mutex mutex_;
    std::string text_; // never logged
    std::uint64_t requested_{};
    std::atomic<std::uint64_t> done_{}, owned_{}, served_{};
    std::atomic<bool> running_{true};
    std::vector<Display> displays_; // the thread's own
};
} // namespace

std::unique_ptr<Clipboard> make_x_clipboard() {
    return std::make_unique<XClipboard>();
}
} // namespace framekeyboard
