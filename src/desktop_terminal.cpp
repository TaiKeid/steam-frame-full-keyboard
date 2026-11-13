#include "framekeyboard/desktop_terminal.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <type_traits>
#include <unistd.h>

namespace framekeyboard {
namespace {
// The few sd-bus calls used, taken from libsystemd.so.0 if it loads; the
// keyboard never links it, so a system without it only loses this.
struct SdBus {
    decltype(&sd_bus_new) bus_new{};
    decltype(&sd_bus_set_address) set_address{};
    decltype(&sd_bus_set_bus_client) set_bus_client{};
    decltype(&sd_bus_set_method_call_timeout) set_timeout{};
    decltype(&sd_bus_start) start{};
    decltype(&sd_bus_close_unref) close{};
    decltype(&sd_bus_request_name) request_name{};
    decltype(&sd_bus_add_filter) add_filter{};
    decltype(&sd_bus_process) process{};
    decltype(&sd_bus_get_fd) get_fd{};
    decltype(&sd_bus_get_events) get_events{};
    decltype(&sd_bus_get_timeout) get_timeout{};
    decltype(&sd_bus_call_method) call{};
    decltype(&sd_bus_message_is_method_call) is_method_call{};
    decltype(&sd_bus_message_get_sender) get_sender{};
    decltype(&sd_bus_message_get_path) get_path{};
    decltype(&sd_bus_message_has_signature) has_signature{};
    decltype(&sd_bus_add_match) add_match{};
    decltype(&sd_bus_message_read) read{};
    decltype(&sd_bus_message_unref) unref{};
    decltype(&sd_bus_reply_method_return) reply{};
    decltype(&sd_bus_error_free) error_free{};
    bool loaded{};
};
const SdBus& sd() {
    static const SdBus api = [] {
        SdBus a;
        void* library = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return a;
        }
        bool found = true;
        const auto get = [&](auto& function, const char* name) {
            function =
                reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            found = found && function;
        };
        get(a.bus_new, "sd_bus_new");
        get(a.set_address, "sd_bus_set_address");
        get(a.set_bus_client, "sd_bus_set_bus_client");
        get(a.set_timeout, "sd_bus_set_method_call_timeout");
        get(a.start, "sd_bus_start");
        get(a.close, "sd_bus_close_unref");
        get(a.request_name, "sd_bus_request_name");
        get(a.add_filter, "sd_bus_add_filter");
        get(a.process, "sd_bus_process");
        get(a.get_fd, "sd_bus_get_fd");
        get(a.get_events, "sd_bus_get_events");
        get(a.get_timeout, "sd_bus_get_timeout");
        get(a.call, "sd_bus_call_method");
        get(a.is_method_call, "sd_bus_message_is_method_call");
        get(a.get_sender, "sd_bus_message_get_sender");
        get(a.get_path, "sd_bus_message_get_path");
        get(a.has_signature, "sd_bus_message_has_signature");
        get(a.add_match, "sd_bus_add_match");
        get(a.read, "sd_bus_message_read");
        get(a.unref, "sd_bus_message_unref");
        get(a.reply, "sd_bus_reply_method_return");
        get(a.error_free, "sd_bus_error_free");
        a.loaded = found;
        return a;
    }();
    return api;
}
// KWin answers within milliseconds; a call bounded to this keeps the watcher
// thread, which also follows Gamescope's focus, responsive. Attaching makes
// at most six calls, once per Desktop session or retry.
constexpr std::uint64_t call_timeout_us = 100000;
constexpr const char* service = "org.framekeyboard.DesktopFocus";
constexpr const char* plugin = "framekeyboard-desktop-focus";
constexpr std::string_view object = "/org/framekeyboard/DesktopFocus";
// KWin leaving the bus, or a new KWin taking its name.
constexpr const char* kwin_owner_rule =
    "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
    "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.kde.KWin'";
// KWin 6: the app ID, never the caption, of each window that becomes active,
// and of the one active when the script starts.
constexpr std::string_view script =
    R"(// Full Keyboard: tells it which app is active in this Desktop, by app ID only,
// so its Cut, Copy and Paste keys match a terminal's. Loaded for this session.
function report(window) {
    callDBus("org.framekeyboard.DesktopFocus", "/org/framekeyboard/DesktopFocus",
             "org.framekeyboard.DesktopFocus", "Active",
             window && window.resourceClass ? String(window.resourceClass) : "");
}
// KWin 6 names, else KWin 5's; anything else fails to load and leaves the
// keyboard's standard keys.
(workspace.windowActivated || workspace.clientActivated).connect(report);
report(workspace.activeWindow !== undefined ? workspace.activeWindow : workspace.activeClient);
)";
// The script goes beside the keyboard's instance lock, in the login runtime
// directory, private to the user; empty when it could not be written.
std::string write_script() {
    const auto directory = "/run/user/" + std::to_string(getuid()) + "/framekeyboard";
    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
        return {};
    }
    const auto path = directory + "/kwin-desktop-focus.js";
    std::ofstream file(path, std::ios::trunc);
    file << script;
    file.close();
    return file ? path : std::string();
}
// A method call on the Desktop's KWin scripting object; false when it failed.
// With `id`, its integer answer, as loadScript's script ID (negative when
// KWin refused the script).
bool scripting(sd_bus* bus, const char* method, const char* types, const char* argument = nullptr,
               const char* second = nullptr, int* id = nullptr) {
    const auto& api = sd();
    sd_bus_error error{}; // SD_BUS_ERROR_NULL, without its C compound literal
    sd_bus_message* reply = nullptr;
    constexpr const char *destination = "org.kde.KWin", *path = "/Scripting",
                         *interface = "org.kde.kwin.Scripting";
    const int result =
        second     ? api.call(bus, destination, path, interface, method, &error, &reply, types, argument,
                              second)
        : argument ? api.call(bus, destination, path, interface, method, &error, &reply, types, argument)
                   : api.call(bus, destination, path, interface, method, &error, &reply, types);
    api.error_free(&error);
    bool answered = result >= 0;
    if (reply) {
        if (id) {
            answered = answered && api.read(reply, "i", id) > 0;
        }
        api.unref(reply);
    }
    return answered;
}
// Who owns a name on the bus, as its unique name; empty if nobody.
std::string owner(sd_bus* bus, const char* name) {
    const auto& api = sd();
    sd_bus_error error{}; // SD_BUS_ERROR_NULL, without its C compound literal
    sd_bus_message* reply = nullptr;
    std::string result;
    if (api.call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                 "GetNameOwner", &error, &reply, "s", name) >= 0) {
        const char* unique = nullptr;
        if (api.read(reply, "s", &unique) > 0 && unique) {
            result = unique;
        }
    }
    api.error_free(&error);
    if (reply) {
        api.unref(reply);
    }
    return result;
}
} // namespace
struct DesktopTerminal::Bus {
    std::string address;
    sd_bus* bus{};
    // Closed without flushing: a stalled bus must not hold the watcher.
    ~Bus() {
        if (bus) {
            sd().close(bus);
        }
    }
};
DesktopTerminal::DesktopTerminal() = default;
DesktopTerminal::~DesktopTerminal() {
    detach();
}
std::string DesktopTerminal::bus_address(std::uint32_t compositor) {
    if (compositor == 0) {
        return {};
    }
    std::ifstream stat("/proc/" + std::to_string(compositor) + "/stat");
    std::string line;
    std::getline(stat, line);
    const auto name_end = line.rfind(')'); // The name may hold spaces and parentheses.
    if (name_end == std::string::npos || name_end + 2 > line.size()) {
        return {};
    }
    std::istringstream fields(line.substr(name_end + 2));
    char state{};
    std::uint32_t parent = 0;
    fields >> state >> parent;
    if (parent <= 1) {
        return {};
    }
    std::ifstream environment("/proc/" + std::to_string(parent) + "/environ", std::ios::binary);
    constexpr std::string_view key = "DBUS_SESSION_BUS_ADDRESS=";
    for (std::string entry; std::getline(environment, entry, '\0');) {
        if (entry.starts_with(key)) {
            return entry.substr(key.size());
        }
    }
    return {};
}
bool DesktopTerminal::attach(const std::string& address) {
    if (bus_ && bus_->address == address) {
        return true;
    }
    detach();
    const auto& api = sd();
    if (!api.loaded || address.empty()) {
        return false;
    }
    auto fresh = std::make_unique<Bus>();
    fresh->address = address;
    if (api.bus_new(&fresh->bus) < 0 || api.set_address(fresh->bus, address.c_str()) < 0 ||
        api.set_bus_client(fresh->bus, 1) < 0 || api.set_timeout(fresh->bus, call_timeout_us) < 0 ||
        api.start(fresh->bus) < 0) {
        return false;
    }
    // Only KWin's calls count: anyone else on the Desktop's bus could claim
    // another app. Its unique name is taken before the script can report.
    kwin_ = owner(fresh->bus, "org.kde.KWin");
    if (kwin_.empty()) {
        return false;
    }
    // A KWin that leaves or restarts takes the script with it: its last
    // report no longer holds (process() detaches).
    const auto kwin_changed = [](sd_bus_message*, void* data, sd_bus_error*) -> int {
        auto* self = static_cast<DesktopTerminal*>(data);
        self->kwin_lost_ = true;
        self->keys_ = EditKeys::Standard;
        self->reported_ = false;
        return 0;
    };
    // The script's calls arrive here: each says which app became active.
    const auto active = [](sd_bus_message* message, void* data, sd_bus_error*) -> int {
        const auto& api = sd();
        if (api.is_method_call(message, service, "Active") <= 0) {
            return 0;
        }
        auto* self = static_cast<DesktopTerminal*>(data);
        const char* sender = api.get_sender(message);
        const char* path = api.get_path(message);
        const char* app = nullptr;
        if (sender && self->kwin_ == sender && path && object == path &&
            api.has_signature(message, "s") > 0 && api.read(message, "s", &app) > 0) {
            self->keys_ = app_edit_keys(app ? app : "");
            self->reported_ = true;
        }
        api.reply(message, "");
        return 1;
    };
    if (api.add_match(fresh->bus, nullptr, kwin_owner_rule, kwin_changed, this) < 0 ||
        api.request_name(fresh->bus, service, 0) < 0 ||
        api.add_filter(fresh->bus, nullptr, active, this) < 0) {
        kwin_.clear();
        return false;
    }
    const auto path = write_script();
    if (path.empty()) {
        return false;
    }
    script_ = path;
    bus_ = std::move(fresh);
    // A copy left by a keyboard that stopped without unloading it goes first.
    scripting(bus_->bus, "unloadScript", "s", plugin);
    int id = -1;
    if (!scripting(bus_->bus, "loadScript", "ss", path.c_str(), plugin, &id) || id < 0 ||
        !scripting(bus_->bus, "start", "")) {
        detach();
        return false;
    }
    return true;
}
bool DesktopTerminal::attached(const std::string& address) const {
    return bus_ && bus_->address == address;
}
void DesktopTerminal::forget_script() {
    if (!script_.empty()) {
        unlink(script_.c_str());
        script_.clear();
    }
}
void DesktopTerminal::detach() {
    if (bus_) {
        scripting(bus_->bus, "unloadScript", "s", plugin);
        bus_.reset();
    }
    forget_script();
    kwin_.clear();
    kwin_lost_ = false;
    keys_ = EditKeys::Standard;
    reported_ = false;
}
int DesktopTerminal::fd() const {
    return bus_ ? sd().get_fd(bus_->bus) : -1;
}
short DesktopTerminal::events() const {
    if (!bus_) {
        return 0;
    }
    // sd-bus's own mask, zero included, as sd_bus_get_fd(3) asks.
    const int events = sd().get_events(bus_->bus);
    return static_cast<short>(events < 0 ? POLLIN : events);
}
int DesktopTerminal::timeout() const {
    if (!bus_) {
        return -1;
    }
    if (more_) {
        return 0; // Messages still wait in sd-bus's own queue, not the socket.
    }
    std::uint64_t until = 0; // CLOCK_MONOTONIC, in microseconds
    if (sd().get_timeout(bus_->bus, &until) < 0 || until == UINT64_MAX) {
        return -1;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const auto current = static_cast<std::uint64_t>(now.tv_sec) * 1000000 +
                         static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    return until <= current
               ? 0
               : static_cast<int>(std::min<std::uint64_t>((until - current + 999) / 1000, INT_MAX));
}
bool DesktopTerminal::process() {
    if (!bus_) {
        return false;
    }
    const auto before = keys_;
    // A burst larger than one round leaves the rest queued in sd-bus, where
    // poll() can't see it: more_ makes the next wait return at once.
    constexpr int round = 256;
    more_ = false;
    for (int i = 0;; ++i) {
        if (i == round) {
            more_ = true;
            break;
        }
        const int result = sd().process(bus_->bus, nullptr);
        if (result < 0) {
            lose_kwin(); // The Desktop closed: its KWin and the script are gone with it.
            more_ = false;
            break;
        }
        if (result == 0 || kwin_lost_) {
            break;
        }
    }
    if (kwin_lost_) {
        lose_kwin(); // The script went with that KWin: attach() loads it into the next.
    }
    return keys_ != before;
}
void DesktopTerminal::lose_kwin() {
    bus_.reset();
    forget_script();
    kwin_.clear();
    kwin_lost_ = false;
    keys_ = EditKeys::Standard;
    reported_ = false;
}
} // namespace framekeyboard
