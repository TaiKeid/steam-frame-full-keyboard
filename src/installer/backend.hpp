#pragma once
// What the installer's screens ask of the system: the release list, install,
// uninstall and applying the first-install choices. The window only starts
// these and polls their status on its own thread; the real backend works on
// a thread of its own, the simulated one only watches the clock.
#include "framekeyboard/localization.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace framekeyboard::installer {

// The oldest release the version list offers: the first that understands
// every choice the wizard sets.
inline constexpr const char* oldest_release = "0.16.0";

// The first release with Streaming support. An older one cannot take that
// answer, so the wizard leaves its page out.
inline constexpr const char* streaming_release = "0.18.0";

// Whether a dotted version is at least another.
inline bool at_least(std::string_view version, std::string_view minimum) {
    const auto next = [](std::string_view& text) {
        long value = 0;
        std::size_t used = 0;
        while (used < text.size() && text[used] >= '0' && text[used] <= '9') {
            value = value * 10 + (text[used++] - '0');
        }
        text.remove_prefix(std::min(text.size(), used + 1)); // the number and its dot
        return value;
    };
    while (!version.empty() || !minimum.empty()) {
        const long have = next(version), want = next(minimum);
        if (have != want) {
            return have > want;
        }
    }
    return true;
}

struct Release {
    std::string tag, version;
};

// The wizard's answers, all Yes by default: the installer shows off what
// the keyboard can do, while features new in an update stay opt-in.
struct Choices {
    bool replace_native{true}, swipe{true}, suggestions{true}, pull_for_shift{true}, numpad{true},
        streaming{true};
};

enum class Task { None, Releases, Install, Uninstall, Finish };
enum class State { Idle, Running, Done, Failed };
// Install's steps, in order. Stopping happens only when a keyboard runs.
enum class Stage { Download, Verify, Stopping, Install };

struct Status {
    Task task{Task::None};
    State state{State::Idle};
    Stage stage{Stage::Download};
    double download{}; // 0..1 of the archive while downloading
    std::vector<Release> releases;
    std::string installed; // the installed version, empty when none
    UiText error;          // why the task failed, in interface text
    // Read with the release list: turning Streaming support on waits for a
    // SteamOS restart, because the running Steam was started without it.
    bool streaming_restart{};
};

class Backend {
  public:
    virtual ~Backend() = default;
    // Each starts a task and returns at once; a task in progress ignores
    // another start. Releases also reads the installed version.
    virtual void releases() = 0;
    virtual void install(const Release& release) = 0;
    virtual void uninstall(bool keep_settings) = 0;
    // Saves the choices and, with replacement on, starts the keyboard. With
    // `restart` it restarts SteamOS instead, which starts the keyboard too.
    virtual void finish(const Choices& choices, bool restart) = 0;
    virtual Status status() = 0;
};

struct SimulatedOptions {
    std::string installed; // pretend this version is installed
    // The version list, newest first; empty lists this build's version.
    std::vector<std::string> releases;
    // The task that fails, by name: releases, download, install, uninstall,
    // finish, or streaming (Finish while Streaming support is chosen).
    std::string fail;
    double speed{1};
    // Pretend the running Steam already has Streaming support's variable, so
    // no restart is offered.
    bool streaming_active{};
};
std::unique_ptr<Backend> simulated_backend(SimulatedOptions options);
// `helper` is update_helper.py. With `local_packages`, release packages in
// that folder are offered too and installed from there (the test installer,
// docs/installer.md#testing-unpublished-releases).
std::unique_ptr<Backend> system_backend(fs::path helper, fs::path local_packages = {});

} // namespace framekeyboard::installer
