#include "framekeyboard/dictation.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <glib.h>
#include <json-c/json.h>
#include <memory>
#include <set>
#include <spawn.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t max_packet = 16384;
constexpr std::size_t max_text = 8192;
constexpr std::size_t max_prompt = 200; // code points
constexpr std::size_t max_events = 256;
constexpr int packets_per_tick = 64;
constexpr double alive_interval = .25;

// Reasons the keyboard may show. Anything else from the helper is replaced, so
// no unexpected text can reach the screen or a log.
const std::set<std::string>& known_reasons() {
    static const std::set<std::string> reasons{"no-microphone", "capture-stopped",  "not-configured",
                                               "unreachable",   "timeout",          "server-error",
                                               "bad-reply",     "key-rejected",     "rate-limited",
                                               "backlog-full",  "keyboard-stalled", "invalid-command",
                                               "helper-exited", "silence",          "busy",
                                               "empty",         "invalid",          "too-long",
                                               "model-missing", "device-failed",    "checksum",
                                               "disk",          "quantize",         "cancelled"};
    return reasons;
}
std::string reason_of(json_object* packet) {
    json_object* value{};
    const char* text = json_object_object_get_ex(packet, "reason", &value) &&
                               json_object_is_type(value, json_type_string)
                           ? json_object_get_string(value)
                           : nullptr;
    return text && known_reasons().contains(text) ? text : "bad-reply";
}
std::optional<std::uint64_t> id_of(json_object* packet) {
    json_object* value{};
    if (!json_object_object_get_ex(packet, "id", &value) || !json_object_is_type(value, json_type_int) ||
        json_object_get_int64(value) <= 0) {
        return {};
    }
    return static_cast<std::uint64_t>(json_object_get_int64(value));
}
std::string packet(const char* op, void (*fill)(json_object*, const void*) = nullptr,
                   const void* data = nullptr) {
    Json object(json_object_new_object(), json_object_put);
    json_object_object_add(object.get(), "op", json_object_new_string(op));
    if (fill) {
        fill(object.get(), data);
    }
    return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}
// The last `limit` code points of valid UTF-8 text.
std::string tail(const std::string& text, std::size_t limit) {
    const auto length =
        static_cast<std::size_t>(g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size())));
    if (length <= limit) {
        return text;
    }
    const char* start = g_utf8_offset_to_pointer(text.c_str(), static_cast<glong>(length - limit));
    return std::string(start, text.c_str() + text.size());
}
} // namespace

Dictation::Dictation(std::filesystem::path key_file, std::filesystem::path helper,
                     std::vector<std::string> arguments)
    : key_file_(std::move(key_file)), helper_(std::move(helper)), arguments_(std::move(arguments)) {
}

Dictation::~Dictation() {
    stop();
    wait_stopped(1);
}

bool Dictation::start() {
    if (fd_ >= 0) {
        return true;
    }
    reap();
    if (child_ > 0) {
        return false; // the previous helper has not exited yet
    }
    auto helper = helper_;
    if (helper.empty()) {
        std::error_code error;
        const auto executable = std::filesystem::canonical("/proc/self/exe", error);
        helper =
            executable.parent_path().parent_path() / "share/framekeyboard/dictation/dictation_helper.py";
        if (!std::filesystem::is_regular_file(helper, error)) {
            helper = executable.parent_path() / "dictation/dictation_helper.py";
        }
    }
    // Only the keyboard's end is nonblocking; the helper waits on its own end.
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        return false;
    }
    if (fcntl(sockets[0], F_SETFL, fcntl(sockets[0], F_GETFL) | O_NONBLOCK) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return false;
    }
    std::vector<std::string> args{"python3", "-I",         helper.string(),   "--fd",
                                  "3",       "--key-file", key_file_.string()};
    args.insert(args.end(), arguments_.begin(), arguments_.end());
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    // Reserve a descriptor above 3 so dup2 never aliases another action.
    const int source = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (source >= 0) {
        posix_spawn_file_actions_adddup2(&actions, source, 3);
        posix_spawn_file_actions_addclose(&actions, source);
    }
    posix_spawn_file_actions_addclosefrom_np(&actions, 4);
    // Unlike the updater, the helper gets no log destination: it handles audio
    // and text, and reports only fixed codes over the socket.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child{};
    const int result =
        source < 0 ? errno : posix_spawnp(&child, "python3", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (source >= 0) {
        close(source);
    }
    close(sockets[1]);
    if (result != 0) {
        close(sockets[0]);
        return false;
    }
    fd_ = sockets[0];
    child_ = child;
    alive_at_ = 0;
    return true;
}

bool Dictation::send(const std::string& data) {
    if (fd_ < 0 || data.size() > max_packet) {
        return false;
    }
    // A full socket means the helper is not reading; dropping one packet is
    // safer than blocking the VR thread, and the helper's own checks recover.
    return ::send(fd_, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL) ==
           static_cast<ssize_t>(data.size());
}

bool Dictation::configure(const Target& target) {
    // A server or a URL, not both; or on device only, the model alone.
    const bool remote = !target.server.empty() || !target.url.empty();
    if ((!target.server.empty() && !target.url.empty()) || (!remote && target.device.empty()) ||
        !start()) {
        return false;
    }
    return send(packet(
        "configure",
        [](json_object* object, const void* data) {
            const auto& t = *static_cast<const Target*>(data);
            const auto add = [&](const char* key, const std::string& value) {
                if (!value.empty()) {
                    json_object_object_add(object, key, json_object_new_string(value.c_str()));
                }
            };
            add("server", t.server);
            add("url", t.url);
            add("model", t.model);
            add("language", t.language);
            if (t.live) {
                json_object_object_add(object, "live", json_object_new_boolean(true));
            }
            add("device", t.device);
            if (t.fallback) {
                json_object_object_add(object, "fallback", json_object_new_boolean(true));
            }
        },
        &target));
}

bool Dictation::capture(bool on) {
    return send(packet(
        "capture",
        [](json_object* object, const void* data) {
            json_object_object_add(object, "on",
                                   json_object_new_boolean(*static_cast<const bool*>(data)));
        },
        &on));
}

bool Dictation::prompt(std::uint64_t id, const std::string& text) {
    if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr)) {
        return false;
    }
    const std::pair<std::uint64_t, std::string> content{id, tail(text, max_prompt)};
    return send(packet(
        "prompt",
        [](json_object* object, const void* data) {
            const auto& [ident, context] =
                *static_cast<const std::pair<std::uint64_t, std::string>*>(data);
            json_object_object_add(object, "id",
                                   json_object_new_int64(static_cast<std::int64_t>(ident)));
            json_object_object_add(
                object, "text",
                json_object_new_string_len(context.c_str(), static_cast<int>(context.size())));
        },
        &content));
}

bool Dictation::cancel() {
    if (!send(packet("cancel"))) {
        return false;
    }
    ++cancels_;
    // Events already read but not yet taken are just as stale.
    std::erase_if(events_, [](const Event& event) {
        return event.kind != Event::Kind::Test && event.reason != "helper-exited";
    });
    return true;
}

bool Dictation::test() {
    return send(packet("test"));
}

bool Dictation::download(const std::string& model, const std::filesystem::path& directory) {
    if (!start()) {
        return false;
    }
    const std::pair<std::string, std::string> values{model, directory.string()};
    return send(packet(
        "download",
        [](json_object* object, const void* data) {
            const auto& v = *static_cast<const std::pair<std::string, std::string>*>(data);
            json_object_object_add(object, "model", json_object_new_string(v.first.c_str()));
            json_object_object_add(object, "directory", json_object_new_string(v.second.c_str()));
        },
        &values));
}

bool Dictation::cancel_download() {
    return fd_ >= 0 && send(packet("download-cancel"));
}

void Dictation::tick(double now) {
    if (fd_ >= 0 && now >= alive_at_) {
        send(packet("alive"));
        alive_at_ = now + alive_interval;
    }
    for (int i = 0; fd_ >= 0 && i < packets_per_tick; ++i) {
        std::array<char, max_packet + 1> data{};
        const auto count = recv(fd_, data.data(), data.size() - 1, MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }
        if (count <= 0 || static_cast<std::size_t>(count) > max_packet) {
            lost(); // end of file, an error, or a packet over the protocol's bound
            break;
        }
        Json message(json_tokener_parse(data.data()), json_object_put);
        json_object* op_value{};
        if (!message || !json_object_object_get_ex(message.get(), "op", &op_value) ||
            !json_object_is_type(op_value, json_type_string)) {
            continue;
        }
        const std::string op = json_object_get_string(op_value);
        if (op == "cancelled") {
            if (cancels_) {
                --cancels_;
            }
            continue;
        }
        Event event;
        if (op == "level") {
            json_object* value{};
            if (!json_object_object_get_ex(message.get(), "value", &value) ||
                !json_object_is_type(value, json_type_int) || events_.size() > max_events / 4) {
                continue; // levels are only for display and never crowd out text
            }
            event.kind = Event::Kind::Level;
            event.level = std::clamp(json_object_get_int(value), 0, 255);
        } else if (op == "phrase" || op == "ready" || op == "partial" || op == "text" ||
                   op == "dropped") {
            const auto id = id_of(message.get());
            if (!id) {
                continue;
            }
            event.id = *id;
            if (op == "phrase") {
                event.kind = Event::Kind::Phrase;
            } else if (op == "ready") {
                event.kind = Event::Kind::Ready;
            } else if (op == "dropped") {
                event.kind = Event::Kind::Dropped;
                event.reason = reason_of(message.get());
            } else {
                json_object* text{};
                if (!json_object_object_get_ex(message.get(), "text", &text) ||
                    !json_object_is_type(text, json_type_string)) {
                    continue;
                }
                const auto length = static_cast<std::size_t>(json_object_get_string_len(text));
                const char* value = json_object_get_string(text);
                if (length > max_text || !g_utf8_validate(value, static_cast<gssize>(length), nullptr) ||
                    std::string_view(value, length).find('\0') != std::string_view::npos) {
                    if (op == "partial") {
                        continue; // only an interim guess; the phrase's own text still follows
                    }
                    event.kind = Event::Kind::Dropped;
                    event.reason = "invalid";
                } else {
                    event.kind = op == "partial" ? Event::Kind::Partial : Event::Kind::Text;
                    event.text.assign(value, length);
                    json_object* device{};
                    event.device = json_object_object_get_ex(message.get(), "device", &device) &&
                                   json_object_get_boolean(device);
                }
            }
        } else if (op == "paused") {
            event.kind = Event::Kind::Paused;
        } else if (op == "capture") {
            event.kind = Event::Kind::Stopped;
            event.reason = reason_of(message.get());
        } else if (op == "error") {
            event.kind = Event::Kind::Error;
            event.reason = reason_of(message.get());
        } else if (op == "download") {
            json_object* value{};
            const auto field = [&](const char* name) {
                return json_object_object_get_ex(message.get(), name, &value) &&
                               json_object_is_type(value, json_type_int)
                           ? std::max<std::int64_t>(0, json_object_get_int64(value))
                           : std::int64_t{};
            };
            event.kind = Event::Kind::Download;
            event.text = json_object_object_get_ex(message.get(), "state", &value) &&
                                 json_object_is_type(value, json_type_string)
                             ? json_object_get_string(value)
                             : "";
            event.done = static_cast<std::uint64_t>(field("done"));
            event.total = static_cast<std::uint64_t>(field("total"));
            if (event.text == "failed") {
                event.reason = reason_of(message.get());
            }
        } else if (op == "test") {
            json_object* ok{};
            event.kind = Event::Kind::Test;
            event.ok = json_object_object_get_ex(message.get(), "ok", &ok) &&
                       json_object_is_type(ok, json_type_boolean) && json_object_get_boolean(ok);
            if (!event.ok) {
                event.reason = reason_of(message.get());
            }
        } else {
            continue;
        }
        // Packets sent before the helper saw a cancel belong to the cancelled
        // phrases; the socket keeps order, so `cancelled` ends them. A Test
        // result is not part of capture.
        if (cancels_ && event.kind != Event::Kind::Test && event.kind != Event::Kind::Download) {
            continue;
        }
        if (events_.size() < max_events) {
            events_.push_back(std::move(event));
        }
    }
    reap();
    if (fd_ >= 0 && child_ < 0) {
        lost(); // the helper exited while its socket still looked open
    }
}

std::optional<Dictation::Event> Dictation::next() {
    if (events_.empty()) {
        return {};
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void Dictation::lost() {
    stop();
    cancels_ = 0;
    Event event;
    event.kind = Event::Kind::Error;
    event.reason = "helper-exited";
    events_.push_back(std::move(event));
}

void Dictation::stop() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (child_ > 0) {
        kill(child_, SIGTERM);
    }
}

void Dictation::reap() {
    if (child_ > 0 && waitpid(child_, nullptr, WNOHANG) == child_) {
        child_ = -1;
    }
}

bool Dictation::wait_stopped(double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (child_ > 0 && std::chrono::steady_clock::now() < deadline) {
        reap();
        if (child_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (child_ > 0) {
        // The recorder dies on its next write once the helper's pipe closes.
        kill(child_, SIGKILL);
        waitpid(child_, nullptr, 0);
        child_ = -1;
        return false;
    }
    return true;
}
} // namespace framekeyboard
