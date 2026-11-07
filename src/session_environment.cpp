#include "framekeyboard/session_environment.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <json-c/json.h>
#include <map>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t max_output = 256 * 1024;
std::string login_runtime(uid_t uid) {
    return "/run/user/" + std::to_string(uid);
}
// The manager's Environment property, read from the login session's bus by our
// own busctl client within two seconds; nothing if it fails or takes longer.
std::optional<std::string> read_manager_environment(uid_t uid) {
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    auto address = "unix:path=" + login_runtime(uid) + "/bus";
    char busctl[] = "busctl", flag[] = "--address", json[] = "--json=short", get[] = "get-property",
         service[] = "org.freedesktop.systemd1", path[] = "/org/freedesktop/systemd1",
         interface[] = "org.freedesktop.systemd1.Manager", property[] = "Environment";
    char* arguments[]{busctl,  flag, address.data(), json,     get,
                      service, path, interface,      property, nullptr};
    pid_t child{};
    const int error = posix_spawnp(&child, busctl, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (error != 0) {
        close(pipe_fds[0]);
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string output;
    bool complete = false;
    while (output.size() <= max_output) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }
        pollfd descriptor{pipe_fds[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }
        char buffer[4096];
        const auto count = read(pipe_fds[0], buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            complete = count == 0;
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);
    int status{};
    while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            break;
        }
        if (result < 0 && errno != EINTR) {
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // This is only our own bounded busctl client.
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!complete || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}
} // namespace
bool nested_runtime_directory(const std::filesystem::path& runtime, uid_t uid) {
    const auto normal = runtime.lexically_normal();
    if (!normal.is_absolute()) {
        return false;
    }
    const auto relative = normal.lexically_relative(login_runtime(uid));
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}
std::optional<std::vector<std::string>> parse_manager_environment(std::string_view json) {
    if (json.size() > max_output) {
        return std::nullopt;
    }
    auto* parser = json_tokener_new_ex(8);
    if (!parser) {
        return std::nullopt;
    }
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    // busctl ends its line with a newline, which strict parsing rejects.
    while (!json.empty() && (json.back() == '\n' || json.back() == ' ')) {
        json.remove_suffix(1);
    }
    const std::string text(json);
    Json value(json_tokener_parse_ex(parser, text.c_str(), static_cast<int>(text.size() + 1)),
               json_object_put);
    const auto error = json_tokener_get_error(parser);
    json_tokener_free(parser);
    json_object* type{};
    json_object* data{};
    if (error != json_tokener_success || !value || !json_object_is_type(value.get(), json_type_object) ||
        !json_object_object_get_ex(value.get(), "type", &type) ||
        !json_object_is_type(type, json_type_string) ||
        std::string_view(json_object_get_string(type)) != "as" ||
        !json_object_object_get_ex(value.get(), "data", &data) ||
        !json_object_is_type(data, json_type_array)) {
        return std::nullopt;
    }
    std::vector<std::string> entries;
    for (std::size_t i = 0; i < json_object_array_length(data); ++i) {
        auto* entry = json_object_array_get_idx(data, i);
        if (!json_object_is_type(entry, json_type_string)) {
            return std::nullopt;
        }
        entries.emplace_back(json_object_get_string(entry),
                             static_cast<std::size_t>(json_object_get_string_len(entry)));
    }
    return entries;
}
SessionChanges session_changes(const std::vector<std::string>& manager, uid_t uid) {
    std::map<std::string, std::string, std::less<>> values;
    for (const auto& entry : manager) {
        if (const auto equals = entry.find('='); equals != std::string::npos && equals > 0) {
            values.emplace(entry.substr(0, equals), entry.substr(equals + 1));
        }
    }
    // Only Steam's Gamescope session is taken. A manager that names another
    // runtime directory, or no Gamescope display, describes some other
    // session, such as an ordinary desktop's; the inherited variables stay.
    const auto runtime = values.find("XDG_RUNTIME_DIR");
    const auto gamescope = values.find("GAMESCOPE_WAYLAND_DISPLAY");
    if (runtime == values.end() ||
        std::filesystem::path(runtime->second).lexically_normal() != login_runtime(uid) ||
        gamescope == values.end() || gamescope->second.empty()) {
        return {};
    }
    SessionChanges changes;
    for (const char* name : session_variables) {
        const auto value = values.find(name);
        changes.emplace_back(name, value == values.end() ? std::nullopt : std::optional(value->second));
    }
    return changes;
}
bool adopt_session_environment() {
    const auto uid = getuid();
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !nested_runtime_directory(runtime, uid)) {
        return false;
    }
    const auto output = read_manager_environment(uid);
    const auto manager = output ? parse_manager_environment(*output) : std::nullopt;
    const auto changes = manager ? session_changes(*manager, uid) : SessionChanges{};
    if (changes.empty()) {
        std::cerr << "Full Keyboard: started in a nested desktop session, but the service manager "
                     "did not give Steam's session values; keeping the desktop's.\n";
        return false;
    }
    for (const auto& [name, value] : changes) {
        if (value) {
            setenv(name.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
    std::cout << "Full Keyboard: started in a nested desktop session, so it uses Steam's session "
                 "display, runtime directory and bus.\n";
    return true;
}
} // namespace framekeyboard
