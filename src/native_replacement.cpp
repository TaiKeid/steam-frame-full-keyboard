#include "framekeyboard/native_replacement.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;
namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::string encoded(const std::string& text) {
    Json value(json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
std::string named_key(int code) {
    switch (code) {
    case KEY_BACKSPACE:
        return "Backspace";
    case KEY_ENTER:
    case KEY_KPENTER:
        return "Enter";
    case KEY_TAB:
        return "Tab";
    case KEY_LEFT:
        return "ArrowLeft";
    case KEY_RIGHT:
        return "ArrowRight";
    case KEY_UP:
        return "ArrowUp";
    case KEY_DOWN:
        return "ArrowDown";
    case KEY_ESC:
        return "VKClose";
    default:
        return {};
    }
}
} // namespace
NativeReplacement::~NativeReplacement() {
    stop();
    // Waits are permitted during shutdown, never during a VR frame.
    for (int i = 0; child_ > 0 && i < 100; ++i) {
        if (waitpid(child_, nullptr, WNOHANG) != 0) {
            child_ = -1;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (child_ > 0) {
        kill(child_, SIGKILL);
        waitpid(child_, nullptr, 0);
    }
    if (watchdog_read_ >= 0) {
        close(watchdog_read_);
    }
    if (watchdog_write_ >= 0) {
        close(watchdog_write_);
    }
}
bool NativeReplacement::prepare_watchdog() {
    if (watchdog_read_ >= 0) {
        return true;
    }
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
        return false;
    }
    watchdog_read_ = descriptors[0];
    watchdog_write_ = descriptors[1];
    return true;
}
void NativeReplacement::watchdog_tick(double now, double slow_work_deadline) const {
    if (watchdog_write_ >= 0) {
        // One fixed-size write is atomic and never waits for the helper. This
        // proves watchdog progress, not render/input readiness for takeover.
        const double health[] = {now, slow_work_deadline, runtime_until_.load()};
        const auto ignored = write(watchdog_write_, health, sizeof(health));
        (void)ignored;
    }
}
void NativeReplacement::begin_runtime_wait(double now) {
    if (runtime_depth_++ == 0) {
        if (runtime_frame_deadline_ == 0) {
            runtime_frame_deadline_ = now + 30;
        }
        runtime_until_.store(runtime_frame_deadline_);
    }
}
void NativeReplacement::end_runtime_wait(double now) {
    if (--runtime_depth_ == 0) {
        runtime_until_.store(std::min(runtime_frame_deadline_, now + 3));
    }
}
void NativeReplacement::frame_completed() {
    runtime_frame_deadline_ = 0;
    runtime_until_.store(0);
}
bool NativeReplacement::packet(const std::string& data) {
    if (fd_ < 0 || data.size() > 32768) {
        return false;
    }
    return ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT) ==
           static_cast<ssize_t>(data.size());
}
const char* NativeReplacement::unavailable_reason() const {
    switch (failure_) {
    case Failure::DebuggerUnavailable:
        return "Steam debugger is unavailable.";
    case Failure::DebuggerBlocked:
        return "Local Steam debugger access is blocked.";
    case Failure::SteamIncompatible:
        return "Steam keyboard interfaces are incompatible.";
    case Failure::ReplacementBusy:
        return "Another keyboard replacement is active.";
    case Failure::HelperUnavailable:
        return "Replacement helper is unavailable.";
    case Failure::ConnectionLost:
        return "Replacement connection was lost.";
    case Failure::None:
        return "";
    }
    return "";
}
void NativeReplacement::retry_now(double now) {
    retry_delay_ = 5;
    next_start_ = now;
}
void NativeReplacement::schedule_retry(double now) {
    const bool persistent =
        !state_.available &&
        (failure_ == Failure::DebuggerUnavailable || failure_ == Failure::DebuggerBlocked ||
         failure_ == Failure::SteamIncompatible || failure_ == Failure::HelperUnavailable);
    next_start_ = now + (persistent ? retry_delay_ : 5);
    retry_delay_ = persistent ? std::min(60.0, retry_delay_ * 2) : 5;
}
bool NativeReplacement::start(double now) {
    next_start_ = now + 5;
    const auto previous_failure = failure_;
    failure_ = Failure::HelperUnavailable;
    if (connect_) {
        fd_ = connect_();
        if (fd_ < 0) {
            return false;
        }
        failure_ = previous_failure;
        last_reply_ = now;
        return true;
    }
    if (watchdog_read_ < 0) {
        return false;
    }
    auto directory = fs::canonical("/proc/self/exe").parent_path();
    auto helper = directory.parent_path() / "share/framekeyboard/bridge/native_bridge.py";
    if (!fs::is_regular_file(helper)) {
        helper = directory / "bridge/native_bridge.py";
    }
    if (!fs::is_regular_file(helper)) {
        return false;
    }
    int pair[2]{};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) < 0) {
        return false;
    }
    // Preserve the health descriptor before spawn actions can overwrite fd 3/4.
    const int health = fcntl(watchdog_read_, F_DUPFD_CLOEXEC, 5);
    if (health < 0) {
        close(pair[0]);
        close(pair[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pair[0]);
    posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
    if (pair[1] != 3) {
        posix_spawn_file_actions_addclose(&actions, pair[1]);
    }
    posix_spawn_file_actions_adddup2(&actions, health, 4);
    posix_spawn_file_actions_addclosefrom_np(&actions, 5);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::string path = helper.string();
    char python[] = "python3", isolated[] = "-I", fd_option[] = "--fd", fd_number[] = "3";
    char health_option[] = "--health-fd", health_number[] = "4";
    char* arguments[] = {python,    isolated,      path.data(),   fd_option,
                         fd_number, health_option, health_number, nullptr};
    const int error = posix_spawnp(&child_, python, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pair[1]);
    close(health);
    if (error) {
        close(pair[0]);
        child_ = -1;
        return false;
    }
    fd_ = pair[0];
    failure_ = previous_failure;
    last_reply_ = now;
    return true;
}
void NativeReplacement::stop() {
    packet("{\"op\":\"stop\"}");
    if (fd_ >= 0) {
        close(std::exchange(fd_, -1));
    }
    state_ = {};
    if (remote_) {
        reset_ = true;
    }
    // Do not change the route until App has released through its old sink.
}
void NativeReplacement::poll(double now) {
    now_ = now;
    const bool child_exited = child_ > 0 && waitpid(child_, nullptr, WNOHANG) != 0;
    if (child_exited) {
        child_ = -1;
    }
    if (fd_ < 0) {
        return;
    }
    for (int count = 0; count < 64; ++count) {
        char buffer[32769];
        const auto size = recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (size <= 0 || size > 32768) {
            failure_ = Failure::ConnectionLost;
            schedule_retry(now);
            stop();
            break;
        }
        Json value(json_tokener_parse(std::string(buffer, static_cast<std::size_t>(size)).c_str()),
                   json_object_put);
        if (!value || json_object_get_type(value.get()) != json_type_object) {
            failure_ = Failure::HelperUnavailable;
            schedule_retry(now);
            stop();
            break;
        }
        auto field = [&](const char* name) { return json_object_object_get(value.get(), name); };
        const auto* operation = json_object_get_string(field("op"));
        if (!operation) {
            failure_ = Failure::HelperUnavailable;
            schedule_retry(now);
            stop();
            break;
        }
        const std::string op(operation);
        if (op == "state" && json_object_get_int(field("version")) == 1) {
            state_ = {json_object_get_uint64(field("request")),
                      json_object_get_boolean(field("open")) != 0,
                      json_object_get_boolean(field("eligible")) != 0,
                      json_object_get_boolean(field("active")) != 0,
                      true,
                      json_object_get_boolean(field("local")) != 0};
            last_reply_ = now;
            failure_ = Failure::None;
            retry_delay_ = 5;
            next_start_ = now + 5;
        } else if (op == "unavailable") {
            // Map only known codes. Never display arbitrary helper/exception text.
            const char* code = json_object_get_string(field("reason"));
            const std::string reason = code ? code : "";
            if (reason == "debugger-unavailable") {
                failure_ = Failure::DebuggerUnavailable;
            } else if (reason == "debugger-blocked") {
                failure_ = Failure::DebuggerBlocked;
            } else if (reason == "steam-incompatible") {
                failure_ = Failure::SteamIncompatible;
            } else if (reason == "replacement-busy") {
                failure_ = Failure::ReplacementBusy;
            } else if (reason == "connection-lost") {
                failure_ = Failure::ConnectionLost;
            } else {
                failure_ = Failure::HelperUnavailable;
            }
            schedule_retry(now);
            stop();
            break;
        } else if (op == "text-result") {
            if (pending_id_ && json_object_get_uint64(field("id")) == pending_id_) {
                pending_result_ = json_object_get_boolean(field("ok")) != 0;
            }
        } else if (op == "key-result") {
            const auto id = json_object_get_uint64(field("id"));
            const auto key = std::find_if(remote_keys_.begin(), remote_keys_.end(),
                                          [id](const auto& held) { return held.second.id == id; });
            if (key != remote_keys_.end()) {
                key->second.pending = false;
                if (!json_object_get_boolean(field("ok"))) {
                    force_fallback_ = reset_ = true;
                }
            }
        } else {
            failure_ = Failure::HelperUnavailable;
            schedule_retry(now);
            stop();
            break;
        }
    }
    if (fd_ >= 0 && (child_exited || now - last_reply_ > .8)) {
        failure_ = Failure::ConnectionLost;
        schedule_retry(now);
        stop();
    }
    if (pending_id_ && !pending_result_ && now - pending_since_ > .6) {
        pending_result_ = false;
        force_fallback_ = true;
    }
}
void NativeReplacement::update(bool enabled, bool ready, std::uint64_t acknowledgement, bool fallback,
                               double now, bool watch_main_loop) {
    if (enabled != enabled_) {
        enabled_ = enabled;
        retry_now(now);
    }
    if (!enabled) {
        stop();
        failure_ = Failure::None;
        return;
    }
    if (fd_ < 0 && child_ < 0 && now >= next_start_) {
        if (!start(now)) {
            schedule_retry(now);
        }
    }
    const bool use_fallback = fallback || std::exchange(force_fallback_, false);
    const auto frame = "{\"op\":\"frame\",\"sent\":" + std::to_string(now) +
                       ",\"progress\":" + std::to_string(++progress_) +
                       ",\"ready\":" + (ready ? "true" : "false") +
                       ",\"ack\":" + std::to_string(acknowledgement) +
                       ",\"watch\":" + (watch_main_loop ? "true" : "false") +
                       ",\"fallbackRequest\":" + std::to_string(state_.request) +
                       ",\"fallback\":" + (use_fallback ? "true}" : "false}");
    // Dropped heartbeats never extend a lease. A dropped explicit fallback closes IPC.
    if (!packet(frame) && use_fallback) {
        stop();
    }
}
bool NativeReplacement::take_input_reset() {
    const bool local_reset = local_.take_input_reset();
    const bool reset = std::exchange(reset_, false);
    return local_reset || reset;
}
void NativeReplacement::dismiss_request() {
    if (state_.open &&
        !packet("{\"op\":\"dismiss\",\"request\":" + std::to_string(state_.request) + "}")) {
        force_fallback_ = true;
    }
}
bool NativeReplacement::take_unsupported_control() {
    return std::exchange(unsupported_control_, false);
}
void NativeReplacement::send(int code, int value) {
    const auto held = remote_keys_.find(code);
    if (value == 0 && held != remote_keys_.end()) {
        const auto release = "{\"op\":\"key-up\",\"request\":" + std::to_string(held->second.request) +
                             ",\"id\":" + std::to_string(held->second.id) + "}";
        remote_keys_.erase(held);
        if (!packet(release)) {
            // A lost release must not leave queued repeats in a live helper.
            force_fallback_ = reset_ = true;
            stop();
        }
        return;
    }
    if (value == 2 && held != remote_keys_.end()) {
        auto& key = held->second;
        if (remote_ && state_.active && state_.request == key.request && modifiers_.empty() &&
            !key.pending) {
            if (packet("{\"op\":\"key\",\"request\":" + std::to_string(key.request) +
                       ",\"id\":" + std::to_string(key.id) +
                       ",\"value\":2,\"text\":" + encoded(named_key(code)) + "}")) {
                key.pending = true;
            } else {
                force_fallback_ = reset_ = true;
            }
        }
        return;
    }
    if (value == 2 && local_keys_.contains(code)) {
        if (!remote_) {
            local_.send(code, value);
        }
        return;
    }
    if (value == 0 && local_keys_.erase(code)) {
        modifiers_.erase(code);
        local_.send(code, value);
        return;
    }
    if (is_modifier(code)) {
        if (value == 0) {
            modifiers_.erase(code);
        } else if (value == 1) {
            modifiers_.insert(code);
        }
    }
    const auto key = named_key(code);
    if (remote_ && !key.empty() && modifiers_.empty()) {
        if (value != 1 || held != remote_keys_.end()) {
            return;
        }
        const auto id = ++sequence_;
        if (!state_.active ||
            !packet("{\"op\":\"key\",\"request\":" + std::to_string(state_.request) +
                    ",\"id\":" + std::to_string(id) + ",\"value\":1,\"text\":" + encoded(key) + "}")) {
            force_fallback_ = reset_ = true;
            return;
        }
        remote_keys_.emplace(code, RemoteKey{id, state_.request, true});
        return;
    }
    if (remote_) {
        // A retained Steam field is not the compositor's focused application.
        // Unsupported physical controls must never escape through local input.
        if (value == 1) {
            unsupported_control_ = true;
        }
        return;
    }
    if (value == 1) {
        local_keys_.insert(code);
    }
    local_.send(code, value);
}
bool NativeReplacement::commit_text(const std::string& text) {
    if (pending_id_) {
        if (text != pending_text_ || !pending_result_.has_value()) {
            return false;
        }
        const bool ok = *pending_result_;
        cancel_text();
        return ok;
    }
    if (!remote_) {
        return local_.commit_text(text);
    }
    if (!state_.active || text.size() > 8192) {
        return false;
    }
    const auto id = ++sequence_;
    if (!packet("{\"op\":\"text\",\"request\":" + std::to_string(state_.request) +
                ",\"id\":" + std::to_string(id) + ",\"text\":" + encoded(text) + "}")) {
        return false;
    }
    pending_id_ = id;
    pending_since_ = now_;
    pending_text_ = text;
    return false;
}
void NativeReplacement::cancel_text() {
    local_.cancel_text();
    pending_id_ = 0;
    pending_result_.reset();
    pending_text_.clear();
}
} // namespace framekeyboard
