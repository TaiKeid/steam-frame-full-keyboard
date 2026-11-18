#include "framekeyboard/shortcut_launch.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace framekeyboard {
namespace fs = std::filesystem;
namespace {
bool starts_with(std::string_view text, std::string_view start) {
    return text.substr(0, start.size()) == start;
}
// NAME of a NAME=value entry.
std::string_view entry_name(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}
// The entries of a colon-separated list that `keep` accepts, joined again.
template <class Keep> std::string filter_list(std::string_view list, Keep keep) {
    std::string result;
    while (true) {
        const auto colon = list.find(':');
        const auto item = list.substr(0, colon);
        if (!item.empty() && keep(item)) {
            result += (result.empty() ? "" : ":") + std::string(item);
        }
        if (colon == std::string_view::npos) {
            break;
        }
        list.remove_prefix(colon + 1);
    }
    return result;
}
} // namespace

fs::path scripts_folder(const fs::path& config_dir) {
    return config_dir / "scripts";
}
bool script_present(const fs::path& folder, const std::string& name) {
    struct stat info{};
    return script_name_valid(name) && lstat((folder / name).c_str(), &info) == 0 &&
           S_ISREG(info.st_mode);
}
std::vector<std::string> list_scripts(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error)) {
        const auto name = entry->path().filename().string();
        // symlink_status: a link is no script, wherever it points.
        std::error_code status_error;
        const auto status = entry->symlink_status(status_error);
        if (!status_error && fs::is_regular_file(status) && !name.starts_with('.') &&
            script_name_valid(name)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    if (names.size() > listed_scripts) {
        names.resize(listed_scripts);
    }
    return names;
}
bool make_scripts_folder(const fs::path& folder) {
    struct stat info{};
    if (lstat(folder.c_str(), &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    return mkdir(folder.c_str(), 0700) == 0 || errno == EEXIST;
}

// What starting the keyboard adds to the environment it was given, which a
// program it starts should not inherit (design, Run). Names only, never
// values, are in this list, in logs and in reports. A Frame probe should
// confirm Steam's part.
//
// - The keyboard's own: every FRAMEKEYBOARD_* variable (the session unit's
//   FRAMEKEYBOARD_SERVICE, a hosted session's lock and update notes, the
//   installed launcher's target language, the simulator's dictionaries).
// - The session unit's, from systemd: NOTIFY_SOCKET (NotifyAccess=all would
//   let the child speak for the keyboard), INVOCATION_ID, JOURNAL_STREAM
//   (the child's output is /dev/null, not the journal), MANAGERPID,
//   SYSTEMD_EXEC_PID, MEMORY_PRESSURE_WATCH, MEMORY_PRESSURE_WRITE,
//   WATCHDOG_PID, WATCHDOG_USEC, LISTEN_PID, LISTEN_FDS, LISTEN_FDNAMES.
//   The unit's EnvironmentFile (Valve's mesavars.sh for SteamVR) is kept:
//   its names aren't known here, and they describe the system's graphics.
// - An AppImage's, the simulator's: APPIMAGE, APPDIR, ARGV0, OWD, and the
//   library path entries inside APPDIR that its AppRun put first.
// - Steam's, when the keyboard was started from Steam's launcher as a
//   non-Steam shortcut: the overlay in LD_PRELOAD (gameoverlayrenderer),
//   the runtime's LD_LIBRARY_PATH and PATH, which go back to the copies of
//   the system's that Steam saved (SYSTEM_LD_LIBRARY_PATH, SYSTEM_PATH),
//   those copies, STEAM_RUNTIME, STEAM_RUNTIME_LIBRARY_PATH, every
//   STEAM_COMPAT_* variable, and the per-game SteamAppId, SteamGameId,
//   SteamOverlayGameId, SteamClientLaunch, SteamEnv,
//   ENABLE_VK_LAYER_VALVE_steam_overlay_1,
//   ENABLE_VK_LAYER_VALVE_steam_fossilize_1, STEAM_FOSSILIZE_DUMP_PATH,
//   DXVK_STATE_CACHE_PATH, __GL_SHADER_DISK_CACHE_PATH,
//   SDL_GAMECONTROLLERCONFIG, SDL_GAMECONTROLLER_IGNORE_DEVICES and
//   SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD (Steam Input's
//   controller setup for the keyboard, not for what it starts).
//
// The session values the keyboard adopts in a nested desktop
// (adopt_session_environment) stay: the child runs in the session the
// keyboard types into.
std::vector<std::string> child_environment(const std::vector<std::string>& keyboard) {
    static constexpr std::string_view dropped[] = {
        "NOTIFY_SOCKET",
        "INVOCATION_ID",
        "JOURNAL_STREAM",
        "MANAGERPID",
        "SYSTEMD_EXEC_PID",
        "MEMORY_PRESSURE_WATCH",
        "MEMORY_PRESSURE_WRITE",
        "WATCHDOG_PID",
        "WATCHDOG_USEC",
        "LISTEN_PID",
        "LISTEN_FDS",
        "LISTEN_FDNAMES",
        "APPIMAGE",
        "APPDIR",
        "ARGV0",
        "OWD",
        "SYSTEM_LD_LIBRARY_PATH",
        "SYSTEM_PATH",
        "STEAM_RUNTIME",
        "STEAM_RUNTIME_LIBRARY_PATH",
        "SteamAppId",
        "SteamGameId",
        "SteamOverlayGameId",
        "SteamClientLaunch",
        "SteamEnv",
        "ENABLE_VK_LAYER_VALVE_steam_overlay_1",
        "ENABLE_VK_LAYER_VALVE_steam_fossilize_1",
        "STEAM_FOSSILIZE_DUMP_PATH",
        "DXVK_STATE_CACHE_PATH",
        "__GL_SHADER_DISK_CACHE_PATH",
        "SDL_GAMECONTROLLERCONFIG",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES",
        "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD",
    };
    static constexpr std::string_view dropped_prefixes[] = {"FRAMEKEYBOARD_", "STEAM_COMPAT_"};
    const auto value_of = [&](std::string_view name) -> std::optional<std::string_view> {
        for (const auto& entry : keyboard) {
            if (entry_name(entry) == name && entry.size() > name.size()) {
                return std::string_view(entry).substr(name.size() + 1);
            }
        }
        return std::nullopt;
    };
    const auto appdir = value_of("APPDIR");
    const auto system_libraries = value_of("SYSTEM_LD_LIBRARY_PATH");
    const auto system_path = value_of("SYSTEM_PATH");
    std::vector<std::string> result;
    for (const auto& entry : keyboard) {
        const auto name = entry_name(entry);
        if (name.size() == entry.size() || name.empty() ||
            std::find(std::begin(dropped), std::end(dropped), name) != std::end(dropped) ||
            std::any_of(std::begin(dropped_prefixes), std::end(dropped_prefixes),
                        [&](std::string_view prefix) { return starts_with(name, prefix); })) {
            continue;
        }
        std::string value(std::string_view(entry).substr(name.size() + 1));
        if (name == "LD_LIBRARY_PATH") {
            if (system_libraries) {
                value = *system_libraries; // what Steam's runtime replaced
            } else if (appdir && !appdir->empty()) {
                value = filter_list(value, [&](std::string_view item) {
                    return !(starts_with(item, *appdir) &&
                             (item.size() == appdir->size() || item[appdir->size()] == '/'));
                });
            }
        } else if (name == "PATH" && system_path) {
            value = *system_path;
        } else if (name == "LD_PRELOAD") {
            value = filter_list(value, [](std::string_view item) {
                return item.find("gameoverlayrenderer") == std::string_view::npos;
            });
        }
        if (value.empty() && (name == "LD_LIBRARY_PATH" || name == "LD_PRELOAD")) {
            continue; // an empty list is no list
        }
        result.push_back(std::string(name) + "=" + value);
    }
    // Steam saved a system path that the keyboard's environment no longer
    // names: it is the system's.
    if (system_libraries && !system_libraries->empty() &&
        std::none_of(result.begin(), result.end(),
                     [](const std::string& entry) { return entry_name(entry) == "LD_LIBRARY_PATH"; })) {
        result.push_back("LD_LIBRARY_PATH=" + std::string(*system_libraries));
    }
    return result;
}
std::vector<std::string> current_environment() {
    std::vector<std::string> entries;
    for (char** variable = environ; *variable; ++variable) {
        entries.emplace_back(*variable);
    }
    return entries;
}

namespace {
// Everything the two processes below need, built before they start: between
// a process's start and its exec only async-signal-safe calls are made, and
// nothing is allocated or written but `error`, through the pipe.
struct Start {
    char* const* argv;  // the program, or /bin/sh -c for a command
    char* const* shell; // /bin/sh and the script, for one the system can't run
    char* const* envp;
    const char* home;
    int report;             // the pipe's writing end, closed on exec
    char* grandchild_stack; // the top of its stack
};
// The program's side: a session of its own, /dev/null for its streams, no
// other open file, default signals and scheduling, the home folder; then the
// program, or /bin/sh with the script.
int run_program(void* data) {
    const auto& start = *static_cast<const Start*>(data);
    for (int number = 1; number < NSIG; ++number) {
        struct sigaction reset{};
        reset.sa_handler = SIG_DFL;
        sigaction(number, &reset, nullptr); // fails harmlessly for KILL, STOP and unused numbers
    }
    setsid();
    const int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    // Every other file goes, except the report, which goes on exec.
    if (start.report > 3) {
        close_range(3, static_cast<unsigned>(start.report) - 1, 0);
    }
    close_range(static_cast<unsigned>(start.report) + 1, ~0U, 0);
    sched_param normal{};
    sched_setscheduler(0, SCHED_OTHER, &normal);
    setpriority(PRIO_PROCESS, 0, 0);
    if (chdir(start.home) != 0) {
        chdir("/");
    }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execve(start.argv[0], start.argv, start.envp);
    int error = errno;
    if (error == ENOEXEC && start.shell) {
        // An executable the system can't run by itself: as a shell would.
        execve(start.shell[0], start.shell, start.envp);
        error = errno;
    }
    [[maybe_unused]] const auto written = write(start.report, &error, sizeof error);
    _exit(127);
}
// The intermediate: starts the program and leaves at once, so the program is
// no child of the keyboard's and its end leaves nothing for the keyboard to
// wait for; the session's subreaper or init adopts it.
int start_program(void* data) {
    auto& start = *static_cast<Start*>(data);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    const pid_t program = fork();
    if (program == 0) {
        run_program(&start);
    }
#else
    // As the intermediate does with the keyboard: the program shares its
    // memory until it executes, which copies no page tables.
    const pid_t program =
        clone(run_program, start.grandchild_stack, CLONE_VM | CLONE_VFORK | SIGCHLD, &start);
#endif
    if (program < 0) {
        int error = errno;
        [[maybe_unused]] const auto written = write(start.report, &error, sizeof error);
    }
    _exit(0);
}
std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> result;
    for (auto& string : strings) {
        result.push_back(string.data());
    }
    result.push_back(nullptr);
    return result;
}
} // namespace

LaunchFailure launch(const LaunchRequest& request) {
    std::vector<std::string> program, shell;
    if (!request.script.empty()) {
        if (!script_name_valid(request.script)) {
            return LaunchFailure::BadName;
        }
        const auto path = request.scripts / request.script;
        struct stat info{};
        if (lstat(path.c_str(), &info) != 0) {
            return LaunchFailure::Missing;
        }
        if (!S_ISREG(info.st_mode)) {
            return LaunchFailure::NotRegular;
        }
        // Run directly when executable, else with /bin/sh.
        shell = {"/bin/sh", path.string()};
        if (access(path.c_str(), X_OK) == 0) {
            program = {path.string()};
        } else {
            program = shell;
            shell.clear();
        }
    } else if (!request.command.empty()) {
        program = {"/bin/sh", "-c", request.command};
    } else {
        return LaunchFailure::NoAction;
    }
    auto environment = request.environment;
    auto argv = pointers(program);
    auto shell_argv = shell.empty() ? std::vector<char*>{} : pointers(shell);
    auto envp = pointers(environment);
    const auto home = request.home.string();
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) {
        return LaunchFailure::System;
    }
    std::vector<char> stacks(2 * 64 * 1024);
    Start start{argv.data(), shell_argv.empty() ? nullptr : shell_argv.data(),
                envp.data(), home.c_str(),
                report[1],   stacks.data() + stacks.size()};
    // Nothing of ours runs in either process before it resets the handlers.
    sigset_t all, saved;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &saved);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // The sanitizers can't follow a child sharing our memory: fork, as the
    // Mozc server's start does in their builds.
    const pid_t intermediate = fork();
    if (intermediate == 0) {
        start_program(&start);
    }
#else
    const pid_t intermediate = clone(start_program, stacks.data() + stacks.size() / 2,
                                     CLONE_VM | CLONE_VFORK | SIGCHLD, &start);
#endif
    pthread_sigmask(SIG_SETMASK, &saved, nullptr);
    close(report[1]);
    if (intermediate < 0) {
        close(report[0]);
        return LaunchFailure::System;
    }
    // Only our own intermediate, which has left or is leaving.
    while (waitpid(intermediate, nullptr, 0) < 0 && errno == EINTR) {
    }
    // Empty once the program has executed: its end of the pipe closed then.
    int error = 0;
    ssize_t got;
    do {
        got = read(report[0], &error, sizeof error);
    } while (got < 0 && errno == EINTR);
    close(report[0]);
    return got == sizeof error ? LaunchFailure::NotStarted : LaunchFailure::None;
}

struct ShortcutLauncher::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<std::uint64_t, LaunchRequest>> waiting;
    std::deque<Result> done;
    bool started{}, stop{};
};
namespace {
void run_starts(const std::shared_ptr<ShortcutLauncher::State>& state) {
    std::unique_lock lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&] { return state->stop || !state->waiting.empty(); });
        if (state->stop) {
            return;
        }
        auto [id, request] = std::move(state->waiting.front());
        state->waiting.pop_front();
        lock.unlock();
        const auto failure = launch(request);
        lock.lock();
        state->done.push_back({id, failure});
    }
}
} // namespace
ShortcutLauncher::ShortcutLauncher() : state_(std::make_shared<State>()) {
}
ShortcutLauncher::~ShortcutLauncher() {
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}
void ShortcutLauncher::start(std::uint64_t id, LaunchRequest request) {
    std::lock_guard lock(state_->mutex);
    state_->waiting.emplace_back(id, std::move(request));
    if (!state_->started) {
        // Detached with its own share of the state: a start under way never
        // holds up the keyboard's exit.
        state_->started = true;
        std::thread([state = state_] { run_starts(state); }).detach();
    }
    state_->wake.notify_one();
}
std::optional<ShortcutLauncher::Result> ShortcutLauncher::take() {
    std::lock_guard lock(state_->mutex);
    if (state_->done.empty()) {
        return {};
    }
    auto result = state_->done.front();
    state_->done.pop_front();
    return result;
}
struct ScriptLister::State {
    ScriptListSource source;
    std::mutex mutex;
    std::condition_variable wake;
    struct Request {
        fs::path folder;
        std::vector<std::string> chosen;
    };
    std::optional<Request> waiting;
    std::optional<ScriptList> done;
    bool started{}, stop{};
};
namespace {
void run_reads(const std::shared_ptr<ScriptLister::State>& state) {
    std::unique_lock lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&] { return state->stop || state->waiting.has_value(); });
        if (state->stop) {
            return;
        }
        const auto request = *std::exchange(state->waiting, std::nullopt);
        lock.unlock();
        ScriptList list;
        if (state->source) {
            list.names = state->source(request.folder);
        } else {
            make_scripts_folder(request.folder); // so the folder the empty list names exists
            list.names = list_scripts(request.folder);
            for (const auto& name : request.chosen) {
                if (std::find(list.names.begin(), list.names.end(), name) == list.names.end() &&
                    script_present(request.folder, name)) {
                    list.also_present.push_back(name);
                }
            }
        }
        lock.lock();
        state->done = std::move(list);
    }
}
} // namespace
ScriptLister::ScriptLister(ScriptListSource source) : state_(std::make_shared<State>()) {
    state_->source = std::move(source);
}
ScriptLister::~ScriptLister() {
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}
void ScriptLister::read(const fs::path& folder, std::vector<std::string> chosen) {
    std::lock_guard lock(state_->mutex);
    state_->waiting = State::Request{folder, std::move(chosen)};
    if (!state_->started) {
        state_->started = true;
        std::thread([state = state_] { run_reads(state); }).detach();
    }
    state_->wake.notify_one();
}
std::optional<ScriptList> ScriptLister::take() {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->done, std::nullopt);
}
} // namespace framekeyboard
