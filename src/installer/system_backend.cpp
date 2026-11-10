// The installer's real work, on a thread of its own. Network, checksums and
// unpacking go through update_helper.py; installing and removing run the
// release's own install.sh and uninstall.sh; the first-install choices are
// saved with the keyboard's own settings and startup code, so they land
// exactly as Settings would save them.
#include "backend.hpp"
#include "framekeyboard/config.hpp"
#include "framekeyboard/startup.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <glib.h>
#include <json-c/json.h>
#include <mutex>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace framekeyboard::installer {
namespace {

constexpr const char* unit = "framekeyboard-autostart.service";

fs::path home() {
    const char* value = std::getenv("HOME");
    if (!value || !*value) {
        throw std::runtime_error("HOME is unset");
    }
    return value;
}
fs::path install_root() {
    return home() / ".local/share/framekeyboard";
}
fs::path runtime_lock() {
    return fs::path("/run/user") / std::to_string(getuid()) / "framekeyboard/vr.lock";
}

// Runs a program with stdin and stderr on /dev/null, passing each stdout
// line to `line`. Returns its exit status, or -1 when it didn't exit.
int run(const std::vector<std::string>& arguments,
        const std::function<void(const std::string&)>& line = {}) {
    // Built before fork: the child of a threaded process may not allocate.
    std::vector<char*> argv;
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    int output[2];
    if (pipe2(output, O_CLOEXEC) != 0) {
        return -1;
    }
    const pid_t child = fork();
    if (child == 0) {
        const int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(output[1], 1);
        dup2(null, 2);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(output[1]);
    if (child < 0) {
        close(output[0]);
        return -1;
    }
    std::string pending;
    char buffer[4096];
    for (ssize_t got; (got = read(output[0], buffer, sizeof buffer)) != 0;) {
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        pending.append(buffer, static_cast<std::size_t>(got));
        for (std::size_t end; (end = pending.find('\n')) != std::string::npos;) {
            if (line) {
                line(pending.substr(0, end));
            }
            pending.erase(0, end + 1);
        }
    }
    close(output[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// systemctl --user, and again through the main session bus for a nested
// desktop's private runtime directory (docs/runtime.md, as uninstall.sh does).
int manager(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"systemctl", "--user"});
    const int direct = run(arguments);
    if (direct == 0 || direct == 3) { // 3: is-active's "inactive"
        return direct;
    }
    arguments.insert(arguments.begin(), {"env", "-u", "XDG_RUNTIME_DIR",
                                         "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" +
                                             std::to_string(getuid()) + "/bus"});
    return run(arguments);
}

// True while a keyboard holds its instance lock.
bool keyboard_running() {
    const int fd = open(runtime_lock().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return held;
}
bool wait_until(const std::function<bool()>& done, double seconds) {
    for (int tries = static_cast<int>(seconds * 5); tries > 0; --tries) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return done();
}

// Stops the startup service and every installed keyboard, as uninstall.sh
// does: a normal stop gives requests back to Steam's keyboard. Returns
// whether the service was running, to start it again after installing.
bool stop_keyboard() {
    const bool service = manager({"is-active", "--quiet", unit}) == 0;
    if (service) {
        manager({"stop", unit});
    }
    std::error_code error;
    const auto releases = fs::canonical(install_root(), error) / "releases";
    if (!error) {
        for (const auto& entry : fs::directory_iterator("/proc", error)) {
            const auto name = entry.path().filename().string();
            if (name.find_first_not_of("0123456789") != std::string::npos) {
                continue;
            }
            auto exe = fs::read_symlink(entry.path() / "exe", error).string();
            if (error) {
                continue;
            }
            if (exe.ends_with(" (deleted)")) {
                exe.resize(exe.size() - 10);
            }
            const fs::path path(exe);
            if (path.filename() == "framekeyboard" && path.parent_path().filename() == "bin" &&
                path.parent_path().parent_path().parent_path() == releases) {
                kill(std::stoi(name), SIGTERM);
            }
        }
    }
    if (!wait_until([] { return !keyboard_running(); }, 10)) {
        throw UiError(ui("installer.error.running"));
    }
    return service;
}

// The helper's failure reasons (update_helper.failure_reason) by message.
UiText helper_error(const std::string& reason) {
    if (reason == "network-unavailable" || reason == "server-unavailable" || reason == "rate-limit" ||
        reason == "download-timeout" || reason == "download-failed") {
        return ui("installer.error.network");
    }
    if (reason == "disk-full") {
        return ui("installer.error.disk");
    }
    if (reason == "wrong-platform") {
        return ui("installer.error.platform");
    }
    return ui("installer.error.package");
}

std::string string_field(json_object* object, const char* name) {
    json_object* value = nullptr;
    return json_object_object_get_ex(object, name, &value) &&
                   json_object_is_type(value, json_type_string)
               ? json_object_get_string(value)
               : std::string();
}

class SystemBackend : public Backend {
  public:
    explicit SystemBackend(fs::path helper) : helper_(std::move(helper)) {}
    ~SystemBackend() override {
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    void releases() override {
        start(Task::Releases, [this] {
            update([&](Status& s) { s.installed = installed_version(); });
            std::vector<Release> found;
            std::string reason = "invalid-release";
            run({"python3", "-I", helper_.string(), "--installer-releases", oldest_release},
                [&](const std::string& text) {
                    auto* event = json_tokener_parse(text.c_str());
                    if (!event) {
                        return;
                    }
                    json_object* list = nullptr;
                    if (string_field(event, "event") == "releases" &&
                        json_object_object_get_ex(event, "releases", &list) &&
                        json_object_is_type(list, json_type_array)) {
                        for (std::size_t i = 0; i < json_object_array_length(list); ++i) {
                            auto* item = json_object_array_get_idx(list, i);
                            found.push_back({string_field(item, "tag"), string_field(item, "version")});
                        }
                        reason.clear();
                    } else if (string_field(event, "event") == "error") {
                        reason = string_field(event, "reason");
                    }
                    json_object_put(event);
                });
            if (!reason.empty() || found.empty()) {
                throw UiError(reason.empty() ? ui("installer.error.no-releases") : helper_error(reason));
            }
            update([&](Status& s) { s.releases = std::move(found); });
        });
    }

    void install(const Release& release) override {
        start(Task::Install, [this, release] {
            auto base = fs::path(g_get_user_cache_dir());
            fs::create_directories(base);
            auto pattern = (base / "framekeyboard-installer.XXXXXX").string();
            if (!mkdtemp(pattern.data())) {
                throw UiError(ui("installer.error.disk"));
            }
            const fs::path work(pattern);
            // The download and the unpacked package go with the work folder.
            struct Cleanup {
                fs::path path;
                ~Cleanup() {
                    std::error_code ignored;
                    fs::remove_all(path, ignored);
                }
            } cleanup{work};
            std::string package, reason = "invalid-release";
            run({"python3", "-I", helper_.string(), "--installer-fetch", release.tag, work.string()},
                [&](const std::string& text) {
                    auto* event = json_tokener_parse(text.c_str());
                    if (!event) {
                        return;
                    }
                    const auto kind = string_field(event, "event");
                    if (kind == "download") {
                        json_object *done = nullptr, *total = nullptr;
                        json_object_object_get_ex(event, "done", &done);
                        json_object_object_get_ex(event, "total", &total);
                        const double whole = total ? json_object_get_double(total) : 0;
                        update([&](Status& s) {
                            s.download = whole > 0 ? json_object_get_double(done) / whole : 0;
                        });
                    } else if (kind == "verify") {
                        update([](Status& s) { s.stage = Stage::Verify; });
                    } else if (kind == "ready") {
                        package = string_field(event, "package");
                        reason.clear();
                    } else if (kind == "error") {
                        reason = string_field(event, "reason");
                    }
                    json_object_put(event);
                });
            if (!reason.empty() || package.empty()) {
                throw UiError(helper_error(reason));
            }
            update([](Status& s) { s.stage = Stage::Stopping; });
            const bool service = stop_keyboard();
            update([](Status& s) { s.stage = Stage::Install; });
            const int installed = run({"bash", (fs::path(package) / "install.sh").string()});
            if (service) {
                manager({"start", unit}); // Back as it was, also after a failed install.
            }
            if (installed != 0) {
                throw UiError(ui("installer.error.install"));
            }
            update([&](Status& s) { s.installed = installed_version(); });
        });
    }

    void uninstall(bool keep_settings) override {
        start(Task::Uninstall, [this, keep_settings] {
            const auto script = install_root() / "current/uninstall.sh";
            if (!fs::is_regular_file(script)) {
                throw UiError(ui("installer.error.uninstall"));
            }
            std::vector<std::string> arguments{"bash", script.string(), "--yes"};
            if (!keep_settings) {
                arguments.push_back("--purge");
            }
            if (run(arguments) != 0) {
                throw UiError(ui("installer.error.uninstall"));
            }
            update([](Status& s) { s.installed.clear(); });
        });
    }

    void finish(const Choices& choices) override {
        start(Task::Finish, [choices] {
            const auto config = default_config_dir();
            std::vector<std::string> errors;
            auto settings = load_settings(config, errors);
            settings.replace_native_keyboard = settings.start_with_steamos = choices.replace_native;
            settings.swipe_typing = choices.swipe;
            settings.typing_suggestions = choices.suggestions;
            settings.key_flicks = choices.pull_for_shift;
            settings.hide_numpad = choices.hide_numpad.value_or(false);
            try {
                save_preferences_with_startup(settings, install_root() / "current/bin/framekeyboard",
                                              config);
            } catch (const std::exception&) {
                throw UiError(ui("installer.error.settings"));
            }
            if (!choices.replace_native) {
                return;
            }
            // The service starts hidden; a second launch then shows it, as a
            // launch of a running keyboard recenters and summons it.
            manager({"start", unit});
            if (!wait_until(keyboard_running, 20)) {
                throw UiError(ui("installer.error.start"));
            }
            run({(home() / ".local/bin/framekeyboard").string()});
        });
    }

    Status status() override {
        std::lock_guard lock(mutex_);
        return status_;
    }

  private:
    static std::string installed_version() {
        const auto binary = install_root() / "current/bin/framekeyboard";
        if (!fs::is_symlink(install_root() / "current") || !fs::is_regular_file(binary)) {
            return {};
        }
        std::string version;
        run({binary.string(), "--version"}, [&](const std::string& text) {
            if (text.starts_with("framekeyboard ")) {
                version = text.substr(14);
            }
        });
        return version;
    }
    void update(const std::function<void(Status&)>& change) {
        std::lock_guard lock(mutex_);
        change(status_);
    }
    void start(Task task, std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            if (status_.state == State::Running) {
                return;
            }
            status_.task = task;
            status_.state = State::Running;
            status_.stage = Stage::Download;
            status_.download = 0;
            status_.error = {};
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        worker_ = std::thread([this, work = std::move(work)] {
            UiText error;
            try {
                work();
            } catch (const UiError& failure) {
                error = failure.message();
            } catch (const std::exception&) {
                error = ui("installer.error.files");
            }
            update([&](Status& s) {
                s.state = error.key.empty() ? State::Done : State::Failed;
                s.error = std::move(error);
            });
        });
    }

    fs::path helper_;
    std::mutex mutex_;
    Status status_;
    std::thread worker_;
};

} // namespace

std::unique_ptr<Backend> system_backend(fs::path helper) {
    return std::make_unique<SystemBackend>(std::move(helper));
}

} // namespace framekeyboard::installer
