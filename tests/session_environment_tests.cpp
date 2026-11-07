#include "framekeyboard/session_environment.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using namespace framekeyboard;
namespace fs = std::filesystem;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::string variable(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "(unset)";
}
int main() {
    try {
        require(nested_runtime_directory("/run/user/1000/nested_plasma", 1000),
                "SteamOS's Desktop runtime directory is nested in the login one");
        require(!nested_runtime_directory("/run/user/1000", 1000) &&
                    !nested_runtime_directory("/run/user/1000/", 1000) &&
                    !nested_runtime_directory("/run/user/1000/.", 1000),
                "the login runtime directory itself is not nested");
        require(!nested_runtime_directory("/run/user/10000/nested_plasma", 1000) &&
                    !nested_runtime_directory("/run/user/1000/../1001/nested", 1000) &&
                    !nested_runtime_directory("/tmp/nested_plasma", 1000) &&
                    !nested_runtime_directory("run/user/1000/nested_plasma", 1000),
                "another user's, an outside or a relative directory is not nested");

        // busctl --json=short's reply on a Frame, trimmed.
        const std::string reply =
            R"({"type":"as","data":["HOME=/home/steamos","DISPLAY=:0","XDG_RUNTIME_DIR=/run/user/1000",)"
            R"("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus","GAMESCOPE_WAYLAND_DISPLAY=gamescope-0",)"
            R"("LIBEI_SOCKET=gamescope-0-ei","XDG_CURRENT_DESKTOP=gamescope","XDG_SESSION_TYPE=x11",)"
            R"("EXTRA=a=b"]})"
            "\n";
        const auto manager = parse_manager_environment(reply);
        require(manager && manager->size() == 9 && manager->back() == "EXTRA=a=b",
                "the manager's environment is read from busctl's reply");
        require(!parse_manager_environment("") && !parse_manager_environment("not json") &&
                    !parse_manager_environment(R"({"type":"s","data":"DISPLAY=:0"})") &&
                    !parse_manager_environment(R"({"type":"as","data":["DISPLAY=:0",1]})") &&
                    !parse_manager_environment(R"({"type":"as","data":[]} trailing)"),
                "any other reply is refused");

        const auto changes = session_changes(*manager, 1000);
        require(changes.size() == session_variables.size(), "every session variable is decided");
        auto change = [&](std::string_view name) {
            for (const auto& [key, value] : changes) {
                if (key == name) {
                    return value;
                }
            }
            throw std::runtime_error("a session variable is missing");
        };
        require(change("XDG_RUNTIME_DIR") == "/run/user/1000" && change("DISPLAY") == ":0" &&
                    change("DBUS_SESSION_BUS_ADDRESS") == "unix:path=/run/user/1000/bus" &&
                    change("LIBEI_SOCKET") == "gamescope-0-ei",
                "the manager's values replace the Desktop's");
        require(!change("WAYLAND_DISPLAY") && !change("XAUTHORITY"),
                "variables Steam's session lacks are unset, not kept from the Desktop");
        require(session_changes(*manager, 1001).empty() &&
                    session_changes({"DISPLAY=:0", "XDG_RUNTIME_DIR=/run/user/1000/nested_plasma"}, 1000)
                        .empty() &&
                    session_changes({"DISPLAY=:0"}, 1000).empty(),
                "a manager naming another runtime directory, or none, changes nothing");
        require(
            session_changes({"XDG_RUNTIME_DIR=/run/user/1000", "DISPLAY=:0", "WAYLAND_DISPLAY=wayland-0",
                             "XDG_CURRENT_DESKTOP=KDE"},
                            1000)
                    .empty() &&
                session_changes({"XDG_RUNTIME_DIR=/run/user/1000", "GAMESCOPE_WAYLAND_DISPLAY="}, 1000)
                    .empty(),
            "an ordinary desktop's manager, without Gamescope, changes nothing");

        // End to end, with a stand-in busctl alone on PATH so no real bus is
        // asked. It records its arguments and answers as on a Frame.
        char pattern[] = "/tmp/full-keyboard-session-XXXXXX";
        const fs::path base = mkdtemp(pattern);
        const auto calls = base / "calls";
        const auto busctl = base / "busctl";
        const auto uid = std::to_string(getuid());
        auto stand_in = [&](const std::string& body) {
            std::ofstream(busctl) << "#!/bin/sh\necho \"$*\" >> \"" << calls.string() << "\"\n" << body;
            fs::permissions(busctl, fs::perms::owner_all);
        };
        const char* const path = std::getenv("PATH");
        const std::string saved_path = path ? path : "";
        setenv("PATH", base.c_str(), 1);
        auto desktop = [&] {
            setenv("XDG_RUNTIME_DIR", ("/run/user/" + uid + "/nested_plasma").c_str(), 1);
            setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus-desktop", 1);
            setenv("DISPLAY", ":2", 1);
            setenv("WAYLAND_DISPLAY", "wayland-0", 1);
            fs::remove(calls);
        };

        // Only shell builtins: PATH holds nothing else.
        const auto answer = "echo '{\"type\":\"as\",\"data\":[\"XDG_RUNTIME_DIR=/run/user/" + uid +
                            "\",\"DISPLAY=:0\",\"GAMESCOPE_WAYLAND_DISPLAY=gamescope-0\","
                            "\"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" +
                            uid + "/bus\"]}'\n";
        stand_in(answer);
        desktop();
        require(adopt_session_environment(), "a Desktop launch takes Steam's session values");
        require(read(calls) ==
                    "--address unix:path=/run/user/" + uid +
                        "/bus --json=short get-property org.freedesktop.systemd1 "
                        "/org/freedesktop/systemd1 org.freedesktop.systemd1.Manager Environment\n",
                "the manager is asked on the login session's bus");
        require(variable("XDG_RUNTIME_DIR") == "/run/user/" + uid && variable("DISPLAY") == ":0" &&
                    variable("DBUS_SESSION_BUS_ADDRESS") == "unix:path=/run/user/" + uid + "/bus" &&
                    variable("WAYLAND_DISPLAY") == "(unset)",
                "the Desktop's display, runtime directory and bus are replaced");
        fs::remove(calls);
        require(!adopt_session_environment() && !fs::exists(calls),
                "a launch in Steam's session asks nothing and changes nothing");

        stand_in("exit 1\n");
        desktop();
        require(!adopt_session_environment() && variable("DISPLAY") == ":2" &&
                    variable("WAYLAND_DISPLAY") == "wayland-0",
                "an unreachable manager keeps the Desktop's values");

        // A busctl that hangs before answering, or after answering without
        // exiting, is given up on after two seconds and reaped.
        auto given_up = [&](const std::string& body) {
            stand_in(body);
            desktop();
            const auto started = std::chrono::steady_clock::now();
            const bool adopted = adopt_session_environment();
            const auto took = std::chrono::steady_clock::now() - started;
            return !adopted && fs::exists(calls) && variable("DISPLAY") == ":2" &&
                   took > std::chrono::milliseconds(1800) && took < std::chrono::seconds(5) &&
                   waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD;
        };
        require(given_up("exec /bin/sleep 10\n"), "a manager that never answers is given up on");
        require(given_up(answer + "exec >&-\nexec /bin/sleep 10\n"),
                "a client that answers but never exits is given up on");

        setenv("PATH", saved_path.c_str(), 1);
        fs::remove_all(base);
        std::cout << "Session environment: nesting, busctl replies, chosen values and Desktop "
                     "launches passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
