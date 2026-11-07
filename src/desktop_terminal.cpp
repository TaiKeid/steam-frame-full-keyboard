#include "framekeyboard/desktop_terminal.hpp"

#include <cerrno>
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
    decltype(&sd_bus_flush_close_unref) close{};
    decltype(&sd_bus_request_name) request_name{};
    decltype(&sd_bus_add_filter) add_filter{};
    decltype(&sd_bus_process) process{};
    decltype(&sd_bus_get_fd) get_fd{};
    decltype(&sd_bus_get_events) get_events{};
    decltype(&sd_bus_call_method) call{};
    decltype(&sd_bus_message_is_method_call) is_method_call{};
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
        get(a.close, "sd_bus_flush_close_unref");
        get(a.request_name, "sd_bus_request_name");
        get(a.add_filter, "sd_bus_add_filter");
        get(a.process, "sd_bus_process");
        get(a.get_fd, "sd_bus_get_fd");
        get(a.get_events, "sd_bus_get_events");
        get(a.call, "sd_bus_call_method");
        get(a.is_method_call, "sd_bus_message_is_method_call");
        get(a.read, "sd_bus_message_read");
        get(a.unref, "sd_bus_message_unref");
        get(a.reply, "sd_bus_reply_method_return");
        get(a.error_free, "sd_bus_error_free");
        a.loaded = found;
        return a;
    }();
    return api;
}
// A call that answers within this keeps the watcher thread responsive.
constexpr std::uint64_t call_timeout_us = 250000;
constexpr const char* service = "org.framekeyboard.DesktopFocus";
constexpr const char* plugin = "framekeyboard-desktop-focus";
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
bool scripting(sd_bus* bus, const char* method, const char* types, const char* argument = nullptr,
               const char* second = nullptr) {
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
    if (reply) {
        api.unref(reply);
    }
    return result >= 0;
}
} // namespace
struct DesktopTerminal::Bus {
    std::string address;
    sd_bus* bus{};
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
    // The script's calls arrive here: each says which app became active.
    const auto active = [](sd_bus_message* message, void* data, sd_bus_error*) -> int {
        const auto& api = sd();
        if (api.is_method_call(message, service, "Active") <= 0) {
            return 0;
        }
        const char* app = nullptr;
        if (api.read(message, "s", &app) >= 0) {
            static_cast<DesktopTerminal*>(data)->keys_ = app_edit_keys(app ? app : "");
        }
        api.reply(message, "");
        return 1;
    };
    if (api.request_name(fresh->bus, service, 0) < 0 ||
        api.add_filter(fresh->bus, nullptr, active, this) < 0) {
        return false;
    }
    const auto path = write_script();
    if (path.empty()) {
        return false;
    }
    bus_ = std::move(fresh);
    // A copy left by a keyboard that stopped without unloading it goes first.
    scripting(bus_->bus, "unloadScript", "s", plugin);
    if (!scripting(bus_->bus, "loadScript", "ss", path.c_str(), plugin) ||
        !scripting(bus_->bus, "start", "")) {
        detach();
        return false;
    }
    return true;
}
void DesktopTerminal::detach() {
    if (bus_) {
        scripting(bus_->bus, "unloadScript", "s", plugin);
        bus_.reset();
    }
    keys_ = EditKeys::Standard;
}
int DesktopTerminal::fd() const {
    return bus_ ? sd().get_fd(bus_->bus) : -1;
}
short DesktopTerminal::events() const {
    const int events = bus_ ? sd().get_events(bus_->bus) : 0;
    return static_cast<short>(events > 0 ? events : POLLIN);
}
bool DesktopTerminal::process() {
    if (!bus_) {
        return false;
    }
    const auto before = keys_;
    for (int i = 0; i < 64; ++i) {
        const int result = sd().process(bus_->bus, nullptr);
        if (result < 0) {
            bus_.reset(); // The Desktop closed: its KWin and the script are gone with it.
            keys_ = EditKeys::Standard;
            break;
        }
        if (result == 0) {
            break;
        }
    }
    return keys_ != before;
}
} // namespace framekeyboard
