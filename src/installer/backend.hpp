#pragma once
// What the installer's screens ask of the system: the release list, install,
// uninstall and applying the first-install choices. The window only starts
// these and polls their status on its own thread; the real backend works on
// a thread of its own, the simulated one only watches the clock.
#include "framekeyboard/localization.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard::installer {

// The oldest release the version list offers: the first that understands
// every choice the wizard sets.
inline constexpr const char* oldest_release = "0.16.0";

struct Release {
    std::string tag, version;
};

// The wizard's answers, all Yes by default: the installer shows off what
// the keyboard can do, while features new in an update stay opt-in.
struct Choices {
    bool replace_native{true}, swipe{true}, suggestions{true}, pull_for_shift{true}, numpad{true};
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
};

class Backend {
  public:
    virtual ~Backend() = default;
    // Each starts a task and returns at once; a task in progress ignores
    // another start. Releases also reads the installed version.
    virtual void releases() = 0;
    virtual void install(const Release& release) = 0;
    virtual void uninstall(bool keep_settings) = 0;
    // Saves the choices and, with replacement on, starts the keyboard.
    virtual void finish(const Choices& choices) = 0;
    virtual Status status() = 0;
};

struct SimulatedOptions {
    std::string installed; // pretend this version is installed
    // The version list, newest first; empty lists this build's version.
    std::vector<std::string> releases;
    // The task that fails, by name: releases, download, install, uninstall or finish.
    std::string fail;
    double speed{1};
};
std::unique_ptr<Backend> simulated_backend(SimulatedOptions options);
// `helper` is update_helper.py.
std::unique_ptr<Backend> system_backend(fs::path helper);

} // namespace framekeyboard::installer
