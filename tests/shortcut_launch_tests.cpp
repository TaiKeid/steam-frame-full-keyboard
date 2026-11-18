// Run's launcher (docs/shortcut-bar-design.md, Run), in a temporary home and
// scripts folder only. The programs started write marker files there or
// sleep briefly; none touches anything else.
#include "framekeyboard/shortcut_launch.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = (fs::temp_directory_path() / "framekeyboard-launch-tests-XXXXXX").string();
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path& path, const std::string& text,
           fs::perms mode = fs::perms::owner_read | fs::perms::owner_write) {
    std::ofstream(path, std::ios::binary) << text;
    fs::permissions(path, mode);
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
// Waits up to two seconds for a file to hold something.
bool appears(const fs::path& path) {
    for (int i = 0; i < 200; ++i) {
        std::error_code error;
        if (fs::exists(path, error) && fs::file_size(path, error) > 0) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}
bool has(const std::vector<std::string>& entries, const std::string& entry) {
    return std::find(entries.begin(), entries.end(), entry) != entries.end();
}
bool names(const std::vector<std::string>& entries, const std::string& name) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const std::string& entry) { return entry.starts_with(name + "="); });
}
const auto executable = fs::perms::owner_all;

void name_rules() {
    require(script_name_valid("backup.sh") && script_name_valid("été tool") &&
                script_name_valid(std::string(64, 'a')),
            "plain names, UTF-8 and 64 bytes are fine");
    for (const auto& bad :
         std::vector<std::string>{"", ".", "..", "a/b", "../x", std::string(65, 'a'),
                                  std::string("tab\there"), std::string("bad\xff\xfe")}) {
        require(!script_name_valid(bad),
                "a folder part, an empty or long name, control or bad bytes: no");
    }
}
void environment_rules() {
    const std::vector<std::string> keyboard{
        "HOME=/home/someone",
        "PATH=/steam/runtime/bin:/usr/bin",
        "SYSTEM_PATH=/usr/local/bin:/usr/bin",
        "LD_LIBRARY_PATH=/steam/runtime/lib",
        "SYSTEM_LD_LIBRARY_PATH=/usr/lib/extra",
        "LD_PRELOAD=/steam/ubuntu12_64/gameoverlayrenderer.so:/usr/lib/libkeep.so",
        "FRAMEKEYBOARD_SERVICE=framekeyboard-autostart.service",
        "FRAMEKEYBOARD_TARGET_LANGUAGE=de-de",
        "NOTIFY_SOCKET=/run/systemd/notify",
        "INVOCATION_ID=0123",
        "JOURNAL_STREAM=8:1234",
        "SteamAppId=12345",
        "SteamGameId=12345",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH=/steam",
        "STEAM_RUNTIME=1",
        "SDL_GAMECONTROLLERCONFIG=mapping",
        "WAYLAND_DISPLAY=gamescope-0",
        "XDG_RUNTIME_DIR=/run/user/1000",
        "BROKEN",
    };
    const auto child = child_environment(keyboard);
    require(has(child, "HOME=/home/someone") && has(child, "WAYLAND_DISPLAY=gamescope-0") &&
                has(child, "XDG_RUNTIME_DIR=/run/user/1000"),
            "the session's own values stay");
    require(has(child, "PATH=/usr/local/bin:/usr/bin") && has(child, "LD_LIBRARY_PATH=/usr/lib/extra"),
            "Steam's runtime paths go back to the system's copies Steam saved");
    require(has(child, "LD_PRELOAD=/usr/lib/libkeep.so"),
            "Steam's overlay leaves the preload; others stay");
    for (const char* gone :
         {"FRAMEKEYBOARD_SERVICE", "FRAMEKEYBOARD_TARGET_LANGUAGE", "NOTIFY_SOCKET", "INVOCATION_ID",
          "JOURNAL_STREAM", "SteamAppId", "SteamGameId", "STEAM_COMPAT_CLIENT_INSTALL_PATH",
          "STEAM_RUNTIME", "SDL_GAMECONTROLLERCONFIG", "SYSTEM_PATH", "SYSTEM_LD_LIBRARY_PATH",
          "BROKEN"}) {
        require(!names(child, gone), std::string("what starting the keyboard added goes: ") + gone);
    }
    // Steam with no system library path saved, and only its overlay preloaded.
    const auto plain =
        child_environment({"LD_PRELOAD=/x/gameoverlayrenderer.so", "LD_LIBRARY_PATH=/usr/lib"});
    require(!names(plain, "LD_PRELOAD") && has(plain, "LD_LIBRARY_PATH=/usr/lib"),
            "an empty preload is none; a library path Steam didn't replace stays");
    // The simulator's AppImage: its own libraries come first.
    const auto appimage = child_environment(
        {"APPDIR=/tmp/.mount_x", "APPIMAGE=/home/a/Sim.AppImage", "ARGV0=./Sim.AppImage", "OWD=/home/a",
         "LD_LIBRARY_PATH=/tmp/.mount_x/usr/lib:/opt/lib:/tmp/.mount_xy"});
    require(appimage == std::vector<std::string>{"LD_LIBRARY_PATH=/opt/lib:/tmp/.mount_xy"},
            "an AppImage's variables and library path go; a path that merely starts alike stays");
    require(child_environment({"LD_LIBRARY_PATH=/tmp/.mount_x/usr/lib", "APPDIR=/tmp/.mount_x"}).empty(),
            "a library path of the AppImage's alone goes whole");
}
void listing() {
    TemporaryDirectory home;
    const auto folder = scripts_folder(home.path / "config");
    require(list_scripts(folder).empty(), "no folder: an empty list");
    fs::create_directories(home.path / "config");
    require(make_scripts_folder(folder) && fs::is_directory(folder) &&
                (fs::status(folder).permissions() & fs::perms::all) == fs::perms::owner_all,
            "the folder is made, private");
    require(make_scripts_folder(folder), "and is fine when it is there");
    write(folder / "b.sh", "x");
    write(folder / "a.sh", "x");
    write(folder / ".hidden", "x");
    write(folder / std::string("bad\x01name"), "x");
    fs::create_directory(folder / "folder");
    fs::create_symlink(folder / "a.sh", folder / "link");
    require(list_scripts(folder) == std::vector<std::string>{"a.sh", "b.sh"},
            "regular files only, sorted: no folder, link, hidden file or bad name");
    for (int i = 0; i < 70; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "s%02d", i);
        write(folder / name, "x");
    }
    const auto many = list_scripts(folder);
    require(many.size() == listed_scripts && many.front() == "a.sh" && many.back() == "s61",
            "at most 64, the first in order");
    TemporaryDirectory other;
    write(other.path / "file", "x");
    require(!make_scripts_folder(other.path / "file"), "a file in the folder's place is no folder");
}
// A request in this test's temporary home.
LaunchRequest request(const fs::path& home, const fs::path& folder) {
    LaunchRequest r;
    r.scripts = folder;
    r.home = home;
    r.environment = child_environment(current_environment());
    return r;
}
void starts() {
    TemporaryDirectory home;
    setenv("HOME", home.path.c_str(), 1);
    const auto folder = home.path / "scripts";
    fs::create_directories(folder);
    auto r = request(home.path, folder);
    // An executable with an interpreter line runs directly.
    write(folder / "direct", "#!/bin/sh\necho direct > \"$HOME/direct.out\"\n", executable);
    r.script = "direct";
    require(launch(r) == LaunchFailure::None && appears(home.path / "direct.out"),
            "a script runs directly");
    // Without the execute bit, through /bin/sh.
    write(folder / "plain", "echo plain > \"$HOME/plain.out\"\n");
    r.script = "plain";
    require(launch(r) == LaunchFailure::None && appears(home.path / "plain.out"),
            "a script that isn't executable runs with /bin/sh");
    // Executable, with no interpreter line: the system can't run it (ENOEXEC).
    write(folder / "bare", "echo bare > \"$HOME/bare.out\"\n", executable);
    r.script = "bare";
    require(launch(r) == LaunchFailure::None && appears(home.path / "bare.out"),
            "an executable without an interpreter line runs with /bin/sh");
    // A command, in the home folder.
    r.script.clear();
    r.command = "echo command > command.out";
    require(launch(r) == LaunchFailure::None && appears(home.path / "command.out"),
            "a command runs with /bin/sh -c, in the home folder");
    r.command.clear();
    // What can't start.
    require(launch(r) == LaunchFailure::NoAction, "nothing to run");
    r.script = "../direct";
    require(launch(r) == LaunchFailure::BadName, "a name with a folder part");
    r.script = "gone";
    require(launch(r) == LaunchFailure::Missing, "a missing script");
    fs::create_directory(folder / "folder");
    r.script = "folder";
    require(launch(r) == LaunchFailure::NotRegular, "a folder");
    fs::create_symlink(folder / "direct", folder / "link");
    r.script = "link";
    require(launch(r) == LaunchFailure::NotRegular, "a link, wherever it points");
    write(folder / "nowhere", "#!/no/such/interpreter\n", executable);
    r.script = "nowhere";
    require(launch(r) == LaunchFailure::NotStarted, "a script whose interpreter is missing");
    require(fs::directory_iterator(home.path) != fs::directory_iterator(),
            "the home folder is the test's");
}
// The child's session, streams, files, folder, signals and environment, seen
// while it sleeps.
void child_state() {
    TemporaryDirectory home;
    setenv("HOME", home.path.c_str(), 1);
    setenv("FRAMEKEYBOARD_LAUNCH_TEST", "1", 1);
    setenv("SteamAppId", "12345", 1);
    setenv("LAUNCH_TEST_KEEP", "kept", 1);
    const auto folder = home.path / "scripts";
    fs::create_directories(folder);
    // A file of ours without close-on-exec, which must not reach the child.
    const int leak = open(home.path.c_str(), O_RDONLY | O_DIRECTORY);
    require(leak >= 0, "a file of ours");
    // Ignored and blocked signals of ours, which the child must not inherit.
    std::signal(SIGUSR1, SIG_IGN);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    write(folder / "sleeper",
          "#!/bin/sh\necho $$ > \"$HOME/pid.tmp\"\nmv \"$HOME/pid.tmp\" \"$HOME/pid\"\nexec sleep 5\n",
          executable);
    auto r = request(home.path, folder);
    r.script = "sleeper";
    require(launch(r) == LaunchFailure::None && appears(home.path / "pid"), "the sleeper starts");
    const pid_t child = std::stoi(read(home.path / "pid"));
    const auto proc = fs::path("/proc") / std::to_string(child);
    for (int i = 0; i < 200 && read(proc / "comm") != "sleep\n"; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    require(read(proc / "comm") == "sleep\n", "it became sleep");
    std::set<std::string> fds;
    for (const auto& entry : fs::directory_iterator(proc / "fd")) {
        fds.insert(entry.path().filename().string());
        require(fs::read_symlink(entry.path()) == "/dev/null", "its open files are /dev/null");
    }
    require(fds == std::set<std::string>{"0", "1", "2"}, "no other open file");
    require(getsid(child) == child, "a session of its own");
    require(fs::read_symlink(proc / "cwd") == home.path, "the home folder as working directory");
    const auto status = read(proc / "status");
    require(status.find("SigBlk:\t0000000000000000") != std::string::npos &&
                status.find("SigIgn:\t0000000000000000") != std::string::npos,
            "no signal blocked or ignored");
    require(status.find("PPid:\t" + std::to_string(getpid()) + "\n") == std::string::npos,
            "it is no child of ours");
    const auto environ = read(proc / "environ");
    require(environ.find("LAUNCH_TEST_KEEP=kept") != std::string::npos &&
                environ.find("HOME=" + home.path.string()) != std::string::npos &&
                environ.find("FRAMEKEYBOARD_LAUNCH_TEST") == std::string::npos &&
                environ.find("SteamAppId") == std::string::npos,
            "the keyboard's environment without what starting it added");
    kill(child, SIGKILL);
    close(leak);
    pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr);
    std::signal(SIGUSR1, SIG_DFL);
    unsetenv("FRAMEKEYBOARD_LAUNCH_TEST");
    unsetenv("SteamAppId");
    unsetenv("LAUNCH_TEST_KEEP");
}
void nothing_to_wait_for() {
    // Every start above waited for its own intermediate only; the programs
    // were never ours.
    errno = 0;
    require(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD, "no child is left to wait for");
}
void worker() {
    TemporaryDirectory home;
    setenv("HOME", home.path.c_str(), 1);
    const auto folder = home.path / "scripts";
    fs::create_directories(folder);
    ShortcutLauncher launcher;
    auto r = request(home.path, folder);
    r.command = "echo worker > worker.out";
    launcher.start(7, r);
    r.command.clear();
    r.script = "gone";
    launcher.start(8, r);
    std::vector<ShortcutLauncher::Result> results;
    for (int i = 0; i < 300 && results.size() < 2; ++i) {
        if (auto result = launcher.take()) {
            results.push_back(*result);
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    require(results.size() == 2 && results[0].id == 7 && results[0].failure == LaunchFailure::None &&
                results[1].id == 8 && results[1].failure == LaunchFailure::Missing &&
                appears(home.path / "worker.out"),
            "the worker starts in order and reports by ID");
}
} // namespace

int main() {
    try {
        name_rules();
        environment_rules();
        listing();
        starts();
        child_state();
        nothing_to_wait_for();
        worker();
        std::cout << "Run's launcher passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
