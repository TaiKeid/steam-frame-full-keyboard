#include "framekeyboard/stream_support.hpp"
#include "framekeyboard/localization.hpp"
#include "framekeyboard/startup.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace framekeyboard {
namespace {
constexpr auto drop_in_directory = "steam.service.d";
constexpr auto drop_in_name = "50-framekeyboard-stream.conf";
constexpr auto marker = "# Managed by Full Keyboard\n";
constexpr auto variable = "SDL_OPENVR_LIBRARY";
constexpr auto link_name = "openvr.so";

// The drop-in's unquoted lines and the environment file take only such a path.
bool plain(const fs::path& path) {
    const auto text = path.string();
    return !text.empty() && text.front() == '/' &&
           text.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-") ==
               std::string::npos;
}
// A systemd command-line argument: quoted, with `%` literal.
std::string quoted(const fs::path& path) {
    std::string result = "\"";
    for (const auto c : path.string()) {
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
            throw UiError(ui("startup.control-characters"));
        }
        if (c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
        if (c == '%') {
            result += '%';
        }
    }
    return result + '"';
}
std::string drop_in(const StreamSupportPaths& paths) {
    const auto answer = (paths.runtime / "steam.env").string();
    // Forget the last answer with a system tool first: were the gate gone, an
    // old answer would otherwise still be read.
    return std::string(marker) +
           "# Streaming support. Before Steam starts the last answer is removed, then the\n"
           "# gate writes it again only if the library loads. Every failure is ignored, so\n"
           "# Steam starts as it comes.\n"
           "[Service]\nExecStartPre=-/usr/bin/rm -f " +
           answer + "\nExecStartPre=-/usr/bin/timeout 5 " + quoted(paths.gate) +
           " start\nEnvironmentFile=-" + answer + "\n";
}
void write_atomic(const fs::path& path, const std::string& text) {
    auto temporary = (path.parent_path() / ".framekeyboard-stream-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw UiError(ui("streaming.write-failed"));
    }
    std::size_t offset = 0;
    bool ok = true;
    while (ok && offset < text.size()) {
        const auto count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        ok = count > 0;
        offset += ok ? static_cast<std::size_t>(count) : 0;
    }
    ok = ok && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    std::error_code error;
    if (ok) {
        fs::rename(temporary, path, error);
    }
    if (!ok || error) {
        fs::remove(temporary, error);
        throw UiError(ui("streaming.write-failed"));
    }
}
// Whether a running Steam was started with the variable naming our link.
bool steam_carries(const StreamSupportPaths& paths) {
    const auto wanted = std::string(variable) + "=" + (paths.runtime / link_name).string();
    std::error_code error;
    for (fs::directory_iterator entry(paths.proc, error), end; !error && entry != end;
         entry.increment(error)) {
        const auto name = entry->path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::ifstream comm(entry->path() / "comm");
        std::string program;
        if (!std::getline(comm, program) || program != "steam") {
            continue;
        }
        // Readable only for this user's own programs.
        std::ifstream stream(entry->path() / "environ", std::ios::binary);
        const std::string all((std::istreambuf_iterator<char>(stream)), {});
        for (std::size_t at = 0; at < all.size();) {
            const auto next = all.find('\0', at);
            const auto length = (next == std::string::npos ? all.size() : next) - at;
            if (all.compare(at, length, wanted) == 0) {
                return true;
            }
            at += length + 1;
        }
    }
    return false;
}
std::optional<std::string> read_drop_in(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(fs::symlink_status(file, error))) {
        return {};
    }
    if (fs::is_symlink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 65536) {
        throw UiError(ui("streaming.custom-file"));
    }
    std::ifstream stream(file);
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (!stream || !text.starts_with(marker)) {
        throw UiError(ui("streaming.custom-file"));
    }
    return text;
}
// One bounded run of our own gate, never of Steam or a shared service.
bool run_command(const std::vector<std::string>& command) {
    std::vector<char*> arguments;
    for (const auto& argument : command) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    pid_t pid{};
    // The gate and its check get nothing of the keyboard's but the standard streams.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    const int spawned = posix_spawn(&pid, arguments[0], &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}
// What the gate's `stock` does, done here so turning the feature off never
// depends on the gate being there: a loaded library goes idle, and the next
// stream finds Steam's own library behind the link.
void point_at_stock(const StreamSupportPaths& paths) {
    std::error_code error;
    if (paths.steam_library.empty()) {
        throw UiError(ui("streaming.off-failed"));
    }
    fs::create_directories(paths.runtime, error);
    fs::remove(paths.runtime / "steam.env", error);
    const bool marked = static_cast<bool>(std::ofstream(paths.runtime / "off"));
    const auto temporary = paths.runtime / ".openvr.so.new";
    fs::remove(temporary, error);
    fs::create_symlink(paths.steam_library, temporary, error);
    if (!error) {
        fs::rename(temporary, paths.runtime / link_name, error);
    }
    if (error || !marked) {
        fs::remove(temporary, error);
        throw UiError(ui("streaming.off-failed"));
    }
}
std::optional<std::string> contents_of(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(stream)), {});
}
// Whether the gate's check failed when the running Steam started, with the
// library this release has. The gate copies the library before checking it,
// so a copy that differs from the installed one means a release installed
// since then, which the next start tries anew.
bool failed_check(const StreamSupportPaths& paths) {
    std::error_code error;
    if (!fs::exists(paths.runtime / "failed", error)) {
        return false;
    }
    const auto copy = paths.runtime / "framekeyboard-stream.so";
    if (!fs::exists(copy, error)) {
        return true; // It failed before the library was copied.
    }
    const auto tried = contents_of(copy);
    const auto installed = contents_of(paths.library);
    return !tried || !installed || *tried == *installed;
}
bool supported(const StreamSupportPaths& paths) {
    std::error_code error;
    return !paths.gate.empty() && fs::is_regular_file(paths.gate, error) &&
           access(paths.gate.c_str(), X_OK) == 0 && fs::is_regular_file(paths.library, error) &&
           fs::is_regular_file(paths.steam_library, error) && plain(paths.runtime);
}
} // namespace

namespace {
// The user's home, from HOME or else the password database. Never a relative
// path: Steam's library there is where the link leads while this is off.
fs::path home_directory() {
    if (const char* home = std::getenv("HOME"); home && home[0] == '/') {
        return home;
    }
    if (const auto* entry = getpwuid(getuid()); entry && entry->pw_dir && entry->pw_dir[0] == '/') {
        return entry->pw_dir;
    }
    return {};
}
} // namespace
StreamSupportPaths stream_support_paths_in(const fs::path& root) {
    StreamSupportPaths paths;
    paths.unit_directory = default_config_dir().parent_path() / "systemd/user";
    paths.runtime = fs::path("/run/user") / std::to_string(getuid()) / "framekeyboard-stream";
    // Without a home there is no Steam library to name: unsupported, and
    // turning it off fails rather than leave a relative link.
    if (const auto home = home_directory(); !home.empty()) {
        paths.steam_library = home / ".local/share/Steam/steamrtarm64/libopenvr_api.so";
    }
    if (!root.empty()) {
        // Through `current`, so the drop-in outlives updates of the keyboard.
        paths.gate = root / "current/lib/framekeyboard/framekeyboard-stream-gate";
        paths.library = root / "current/lib/framekeyboard/framekeyboard-stream.so";
    }
    return paths;
}
StreamSupportPaths stream_support_paths(const fs::path& executable) {
    std::error_code error;
    const auto binary = fs::canonical(executable, error);
    const auto release = binary.parent_path().parent_path();
    const auto root = release.parent_path().parent_path();
    const bool installed = !error && binary.parent_path().filename() == "bin" &&
                           release.parent_path().filename() == "releases" &&
                           fs::is_symlink(root / "current", error) &&
                           fs::canonical(root / "current", error) == release;
    return stream_support_paths_in(installed ? root : fs::path{});
}
StreamSupportStatus stream_support_status(const StreamSupportPaths& paths) {
    StreamSupportStatus status;
    status.supported = supported(paths);
    try {
        status.enabled =
            read_drop_in(paths.unit_directory / drop_in_directory / drop_in_name).has_value();
    } catch (const std::exception&) {
        status.enabled = false; // Someone else's file: not ours to count.
    }
    status.active = steam_carries(paths);
    status.failed = !status.active && failed_check(paths);
    return status;
}
void configure_stream_support(bool enabled, const StreamSupportPaths& paths,
                              std::function<void()> reload, StreamSupportRun run, bool refresh) {
    if (!reload) {
        reload = reload_session_manager;
    }
    if (!run) {
        run = run_command;
    }
    // A keyboard that is not an installed release, such as a development
    // build, has no library to offer and leaves the installed one's setting
    // and runtime folder as they are, whatever its own config says.
    if (paths.gate.empty()) {
        if (enabled) {
            throw UiError(ui("streaming.unavailable"));
        }
        return;
    }
    const auto directory = paths.unit_directory / drop_in_directory;
    const auto file = directory / drop_in_name;
    // Someone else's file under our name blocks turning this on; off, it is
    // simply not ours to remove.
    std::optional<std::string> previous;
    try {
        previous = read_drop_in(file);
    } catch (const UiError&) {
        if (enabled) {
            throw;
        }
    }
    std::error_code ignored;
    // Puts the drop-in back as it was after a failed change.
    const auto restore = [&] {
        try {
            if (previous) {
                fs::create_directories(directory, ignored); // Removed with the drop-in when empty.
                write_atomic(file, *previous);
            } else {
                fs::remove(file, ignored);
                fs::remove(directory, ignored); // Only when it holds nothing else.
            }
            reload();
        } catch (const std::exception&) {
        }
    };
    if (enabled) {
        if (!supported(paths)) {
            throw UiError(ui("streaming.unavailable"));
        }
        const auto contents = drop_in(paths);
        const bool changed = previous != contents;
        if (changed) {
            fs::create_directories(directory, ignored);
            write_atomic(file, contents);
            try {
                reload();
            } catch (...) {
                restore();
                throw;
            }
        }
        // A Steam that already has the variable can use the library with its
        // next stream: give it this release's copy, checked, when it was just
        // turned on, and when the keyboard starts or reloads, perhaps as a new
        // release. Not on every save: a check that failed mid-stream would
        // turn the running stream back to Steam's keyboard.
        if ((changed || refresh) && steam_carries(paths) && !run({paths.gate.string(), "prepare"})) {
            if (changed) {
                restore();
            }
            throw UiError(ui("streaming.library-failed"));
        }
        return;
    }
    // Turning off fails as a whole or not at all: first Steam's next start,
    // then a Steam that already carries the variable. A failure puts the
    // drop-in back and leaves a loaded library typing, so the setting, the
    // drop-in and the library still agree when the save puts the flag back.
    if (previous) {
        fs::remove(file, ignored);
        fs::remove(directory, ignored); // Only when it holds nothing else.
        try {
            reload();
        } catch (...) {
            restore();
            throw;
        }
    }
    // Only when it is being turned off, or when the keyboard starts or
    // reloads: a save that leaves it off has nothing to change.
    if ((previous || refresh) && (fs::exists(paths.runtime, ignored) || steam_carries(paths))) {
        // A mark that was there before, as a failed check leaves it, is not this call's to take back.
        const bool marked = fs::exists(paths.runtime / "off", ignored);
        try {
            point_at_stock(paths);
        } catch (...) {
            if (previous) {
                if (!marked) {
                    fs::remove(paths.runtime / "off", ignored); // The link is unchanged or Steam's.
                }
                restore();
            }
            throw;
        }
    }
}
void save_preferences_with_integration(const Settings& settings, const fs::path& executable,
                                       const fs::path& config_dir, const StreamSupportPaths& paths,
                                       std::function<void()> reload, StreamSupportRun run) {
    std::vector<std::string> unreadable, ignored;
    const auto previous = load_settings(config_dir, unreadable);
    std::optional<StartupPreferencesWarning> warning;
    try {
        save_preferences_with_startup(settings, executable, config_dir, paths.unit_directory, reload);
    } catch (const StartupPreferencesWarning& startup) {
        warning = startup;
    }
    // What was saved decides: a refused replacement change keeps streaming
    // support where it was as well.
    auto saved = load_settings(config_dir, ignored);
    try {
        configure_stream_support(saved.replace_native_keyboard && saved.streaming_support, paths, reload,
                                 std::move(run), false);
    } catch (const std::exception& error) {
        if (!warning) {
            const bool changed = saved.streaming_support != previous.streaming_support;
            saved.streaming_support = previous.streaming_support;
            save_settings(config_dir, saved);
            throw StreamingSupportWarning(changed ? "streaming.saved-without-change"
                                                  : "streaming.saved-not-configured",
                                          error_text(error), saved.streaming_support);
        }
    }
    if (warning) {
        throw *warning;
    }
}
void reconcile_integration(const Settings& settings, const fs::path& executable,
                           const fs::path& config_dir, const StreamSupportPaths& paths,
                           std::function<void()> reload, StreamSupportRun run) {
    std::exception_ptr first;
    try {
        configure_startup(settings, executable, config_dir, paths.unit_directory, reload);
    } catch (...) {
        first = std::current_exception();
    }
    configure_stream_support(settings.replace_native_keyboard && settings.streaming_support, paths,
                             reload, std::move(run));
    if (first) {
        std::rethrow_exception(first);
    }
}
} // namespace framekeyboard
