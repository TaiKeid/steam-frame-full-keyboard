// Streaming support's set-up for Steam's start (src/stream_support.cpp), in a private folder: the
// drop-in, what a Steam that already carries the variable gets, and the preference writer's save.
#include "framekeyboard/startup.hpp"
#include "framekeyboard/stream_support.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool throws(const std::function<void()>& work) {
    try {
        work();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
} // namespace
int main() {
    try {
        char pattern[] = "/tmp/full-keyboard-stream-XXXXXX";
        const fs::path base = mkdtemp(pattern);
        const auto root = base / "install";
        const auto binary = root / "releases/test/bin/framekeyboard";
        const auto tools = root / "releases/test/lib/framekeyboard";
        const auto config = base / "config";
        fs::create_directories(binary.parent_path());
        fs::create_directories(tools);
        std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
        fs::permissions(binary, fs::perms::owner_all);
        fs::create_symlink("releases/test", root / "current");

        StreamSupportPaths paths;
        paths.gate = root / "current/lib/framekeyboard/framekeyboard-stream-gate";
        paths.library = root / "current/lib/framekeyboard/framekeyboard-stream.so";
        paths.unit_directory = base / "units";
        paths.runtime = base / "runtime";
        paths.steam_library = base / "steam/libopenvr_api.so";
        paths.proc = base / "proc";
        const auto drop_in = paths.unit_directory / "steam.service.d/50-framekeyboard-stream.conf";
        const auto link = paths.runtime / "openvr.so";
        int reloads = 0;
        bool reload_fails = false;
        auto reload = [&] {
            ++reloads;
            if (reload_fails) {
                throw std::runtime_error("reload failed");
            }
        };
        std::vector<std::vector<std::string>> runs;
        bool run_succeeds = true;
        auto run = [&](const std::vector<std::string>& command) {
            runs.push_back(command);
            return run_succeeds;
        };

        // Nothing installed: not supported, and turning it on is refused without touching anything.
        require(stream_support_status(paths) == StreamSupportStatus{}, "an empty system offers nothing");
        require(throws([&] { configure_stream_support(true, paths, reload, run); }) &&
                    !fs::exists(drop_in) && reloads == 0,
                "turning it on without the library is refused");
        configure_stream_support(false, paths, reload, run);
        require(reloads == 0 && !fs::exists(paths.runtime), "off on a clean system changes nothing");

        // The release's pieces and Steam's own library make it supported.
        std::ofstream(tools / "framekeyboard-stream-gate") << "#!/bin/sh\nexit 0\n";
        fs::permissions(tools / "framekeyboard-stream-gate", fs::perms::owner_all);
        std::ofstream(tools / "framekeyboard-stream.so") << "library";
        require(!stream_support_status(paths).supported, "Steam's own library has to be there too");
        fs::create_directories(paths.steam_library.parent_path());
        std::ofstream(paths.steam_library) << "steam";
        require(stream_support_status(paths) == StreamSupportStatus{.supported = true},
                "supported, not yet on");
        const auto found = stream_support_paths(binary);
        require(found.gate == paths.gate && found.library == paths.library,
                "an installed release finds its pieces through the current link");
        require(stream_support_paths(base / "config").gate.empty(),
                "outside an installed release there is nothing to offer");

        // A keyboard outside an installed release leaves the installed one's setting alone, off or on.
        {
            auto development = stream_support_paths(base / "config");
            development.unit_directory = paths.unit_directory;
            development.runtime = paths.runtime;
            development.steam_library = paths.steam_library;
            development.proc = paths.proc;
            fs::create_directories(drop_in.parent_path());
            std::ofstream(drop_in) << "# Managed by Full Keyboard\n";
            configure_stream_support(false, development, reload, run);
            require(fs::exists(drop_in) && !fs::exists(paths.runtime) && reloads == 0,
                    "turning it off from a development build changes nothing");
            require(throws([&] { configure_stream_support(true, development, reload, run); }) &&
                        read(drop_in) == "# Managed by Full Keyboard\n",
                    "nor can it turn it on");
            fs::remove_all(drop_in.parent_path());
        }

        // On: a drop-in that clears the last answer, runs the gate and reads its answer.
        configure_stream_support(true, paths, reload, run);
        const auto contents = read(drop_in);
        const auto answer = (paths.runtime / "steam.env").string();
        require(contents.starts_with("# Managed by Full Keyboard\n") &&
                    contents.find("[Service]\nExecStartPre=-/usr/bin/rm -f " + answer + "\n") !=
                        std::string::npos &&
                    contents.find("ExecStartPre=-/usr/bin/timeout 5 \"" + paths.gate.string() +
                                  "\" start\n") != std::string::npos &&
                    contents.ends_with("EnvironmentFile=-" + answer + "\n"),
                "the drop-in clears, checks and reads, and none of it can stop Steam");
        require(reloads == 1 && runs.empty() && !fs::exists(paths.runtime),
                "the manager is told once, and a Steam without the variable is left alone");
        require(stream_support_status(paths) == StreamSupportStatus{.supported = true, .enabled = true},
                "on, waiting for Steam's next start");
        // The gate's check failed when Steam started: with this release's library, a restart alone
        // won't help; a release installed since then is tried at the next start.
        fs::create_directories(paths.runtime);
        std::ofstream(paths.runtime / "failed");
        require(stream_support_status(paths).failed, "a check that failed before copying counts");
        std::ofstream(paths.runtime / "framekeyboard-stream.so") << "library";
        require(stream_support_status(paths).failed, "so does one that failed with this library");
        std::ofstream(paths.runtime / "framekeyboard-stream.so") << "older library";
        require(!stream_support_status(paths).failed, "a library installed since is not judged yet");
        // A copy that failed its check keeps a name of its own, beside the one checked before (R246).
        std::ofstream(paths.runtime / "framekeyboard-stream-check.so") << "library";
        require(stream_support_status(paths).failed, "the copy that failed is the one compared");
        std::ofstream(paths.runtime / "framekeyboard-stream-check.so") << "older library";
        require(!stream_support_status(paths).failed, "and a release installed since is tried anew");
        fs::remove_all(paths.runtime);
        configure_stream_support(true, paths, reload, run);
        require(reloads == 1 && runs.empty(), "an unchanged drop-in is not written again");

        // A running Steam: only one started with our link counts as carrying the variable.
        fs::create_directories(paths.proc / "200");
        fs::create_directories(paths.proc / "self");
        std::ofstream(paths.proc / "200/comm") << "steam\n";
        const std::string other =
            std::string("HOME=/home/user") + '\0' + "SDL_OPENVR_LIBRARY=/somewhere/else.so" + '\0';
        std::ofstream(paths.proc / "200/environ", std::ios::binary) << other;
        require(!stream_support_status(paths).active, "another library's path is not ours");
        const std::string ours =
            std::string("HOME=/home/user") + '\0' + "SDL_OPENVR_LIBRARY=" + link.string() + '\0';
        fs::create_directories(paths.proc / "300");
        std::ofstream(paths.proc / "300/comm") << "steamwebhelper\n";
        std::ofstream(paths.proc / "300/environ", std::ios::binary) << ours;
        require(!stream_support_status(paths).active, "only Steam itself is asked");
        std::ofstream(paths.proc / "200/environ", std::ios::binary) << ours;
        require(stream_support_status(paths).active, "Steam carries the variable");

        // On while Steam carries it: the gate gives it this release's library at once.
        configure_stream_support(true, paths, reload, run);
        require(reloads == 1 && runs.size() == 1 &&
                    runs[0] == std::vector<std::string>{paths.gate.string(), "prepare"},
                "a Steam that carries the variable gets a checked copy");
        configure_stream_support(true, paths, reload, run, false);
        require(runs.size() == 1, "a save that leaves it on doesn't check the library again");

        // Off: the link leads to Steam's own library, a loaded library is told to idle, the drop-in
        // goes.
        fs::create_directories(paths.runtime);
        std::ofstream(paths.runtime / "steam.env") << "old";
        configure_stream_support(false, paths, reload, run);
        require(!fs::exists(drop_in) && !fs::exists(drop_in.parent_path()) && reloads == 2,
                "off removes the drop-in and its empty folder");
        require(fs::is_symlink(link) && fs::read_symlink(link) == paths.steam_library &&
                    fs::exists(paths.runtime / "off") && !fs::exists(paths.runtime / "steam.env") &&
                    runs.size() == 1,
                "off needs no gate: link to Steam's library, idle mark, no answer left");
        require(stream_support_status(paths) == StreamSupportStatus{.supported = true, .active = true},
                "off, though Steam still carries the variable");

        // A library that fails its check: the change is taken back.
        run_succeeds = false;
        require(throws([&] { configure_stream_support(true, paths, reload, run); }) &&
                    !fs::exists(drop_in),
                "a failed check leaves it off");
        run_succeeds = true;

        // The manager cannot be told: the drop-in is taken back as well.
        reload_fails = true;
        require(throws([&] { configure_stream_support(true, paths, reload, run); }) &&
                    !fs::exists(drop_in),
                "a failed reload leaves no drop-in behind");
        reload_fails = false;
        configure_stream_support(true, paths, reload, run);
        // A loaded library is typing: the link leads to the copy and nothing tells it to idle.
        fs::remove(paths.runtime / "off");
        fs::remove(link);
        fs::create_symlink("framekeyboard-stream.so", link);
        reload_fails = true;
        require(throws([&] { configure_stream_support(false, paths, reload, run); }) &&
                    read(drop_in) == contents,
                "a failed reload while turning off puts the drop-in back");
        require(fs::read_symlink(link) == "framekeyboard-stream.so" &&
                    !fs::exists(paths.runtime / "off"),
                "and leaves the loaded library typing, as the setting stays on");
        reload_fails = false;
        // The reload works but Steam's library cannot be named (no home): the drop-in comes back,
        // and a loaded library stays typing.
        {
            auto homeless = paths;
            homeless.steam_library.clear();
            const auto reloaded = reloads;
            require(throws([&] { configure_stream_support(false, homeless, reload, run); }) &&
                        read(drop_in) == contents && reloads == reloaded + 2,
                    "a turn-off that cannot point at Steam's library puts the drop-in back");
            require(fs::read_symlink(link) == "framekeyboard-stream.so" &&
                        !fs::exists(paths.runtime / "off"),
                    "and leaves the loaded library typing");
            std::ofstream(paths.runtime / "off");
            require(throws([&] { configure_stream_support(false, homeless, reload, run); }) &&
                        fs::exists(paths.runtime / "off"),
                    "a mark that was there before stays");
            fs::remove(paths.runtime / "off");
        }

        // Someone else's file under our name: never overwritten, never removed.
        std::ofstream(drop_in) << "[Service]\nEnvironment=MINE=1\n";
        require(throws([&] { configure_stream_support(true, paths, reload, run); }) &&
                    read(drop_in) == "[Service]\nEnvironment=MINE=1\n",
                "a foreign file blocks turning it on");
        require(!stream_support_status(paths).enabled, "and does not count as ours");
        configure_stream_support(false, paths, reload, run);
        require(read(drop_in) == "[Service]\nEnvironment=MINE=1\n", "off leaves a foreign file alone");
        fs::remove(drop_in);

        // The preference writer: streaming support follows the saved flags, and needs replacement.
        Settings settings;
        settings.replace_native_keyboard = settings.start_with_steamos = settings.streaming_support =
            true;
        save_preferences_with_integration(settings, binary, config, paths, reload, run);
        std::vector<std::string> errors;
        require(fs::exists(drop_in) &&
                    fs::exists(paths.unit_directory / "framekeyboard-autostart.service") &&
                    load_settings(config, errors).streaming_support,
                "a save sets up the boot unit and streaming support");
        const auto checks = runs.size();
        save_preferences_with_integration(settings, binary, config, paths, reload, run);
        require(runs.size() == checks, "saving other preferences leaves the running Steam alone");
        settings.replace_native_keyboard = false;
        save_preferences_with_integration(settings, binary, config, paths, reload, run);
        require(!fs::exists(drop_in), "without replacement streaming support is off");
        settings.replace_native_keyboard = true;
        reconcile_integration(settings, binary, config, paths, reload, run);
        require(fs::exists(drop_in), "reconciling at startup sets it up again");

        // It cannot follow: the saved flag goes back and the save reports a warning.
        settings.streaming_support = false;
        save_preferences_with_integration(settings, binary, config, paths, reload, run);
        require(!fs::exists(drop_in) && !load_settings(config, errors).streaming_support, "saved off");
        fs::remove(tools / "framekeyboard-stream.so");
        settings.streaming_support = true;
        bool warned = false;
        try {
            save_preferences_with_integration(settings, binary, config, paths, reload, run);
        } catch (const StreamingSupportWarning& warning) {
            warned = warning.message().key == "streaming.saved-without-change" && !warning.saved();
        }
        const auto kept = load_settings(config, errors);
        require(warned && !kept.streaming_support && kept.replace_native_keyboard &&
                    !fs::exists(drop_in),
                "an unavailable library keeps the flag off and says so, saving the rest");

        // The drop-in cannot be removed: turning it off keeps the flag on and says so, rather than
        // leave Steam's start set up behind a setting that reads off (R250).
        std::ofstream(tools / "framekeyboard-stream.so") << "library";
        save_preferences_with_integration(settings, binary, config, paths, reload, run);
        require(fs::exists(drop_in) && load_settings(config, errors).streaming_support, "on again");
        if (geteuid() != 0) { // Permissions don't stop root.
            const auto folder = drop_in.parent_path();
            fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec);
            settings.streaming_support = false;
            bool refused = false;
            try {
                save_preferences_with_integration(settings, binary, config, paths, reload, run);
            } catch (const StreamingSupportWarning& warning) {
                refused = warning.message().key == "streaming.saved-without-change" && warning.saved();
            }
            fs::permissions(folder, fs::perms::owner_all);
            require(refused && fs::exists(drop_in) && load_settings(config, errors).streaming_support,
                    "a drop-in that cannot be removed keeps the flag on and says so");
        }
        fs::remove_all(base);
        std::cout << "Streaming support's drop-in, running-Steam handling and preference save passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
