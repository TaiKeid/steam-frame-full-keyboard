#pragma once
#include "config.hpp"
#include <functional>
#include <string>
#include <vector>

namespace framekeyboard {
// Streaming support (docs/streamed-games.md): Steam's streaming client loads
// Full Keyboard's library only when Steam itself was started with one
// variable set. A drop-in for Steam's user service runs the gate
// (stream/stream_gate.c) before each Steam start; the gate sets the variable
// when the library passes its check. Nothing here touches Steam's files, and
// a missing or failing piece leaves Steam's start as it comes.
struct StreamSupportPaths {
    fs::path gate;           // the installed gate, through the release's `current` link
    fs::path library;        // the library beside it
    fs::path unit_directory; // the user's systemd units, where Steam's service takes drop-ins
    fs::path runtime;        // the gate's folder in the user's runtime directory
    fs::path steam_library;  // Steam's own OpenVR library: where the link leads while this is off
    fs::path proc{"/proc"};  // running programs; tests supply their own
};
// For an installation under `root` (the folder holding `current` and
// `releases`), and for the release `executable` runs from. Outside an
// installed release `gate` and `library` stay empty: unsupported.
StreamSupportPaths stream_support_paths_in(const fs::path& root);
StreamSupportPaths stream_support_paths(const fs::path& executable);

struct StreamSupportStatus {
    bool operator==(const StreamSupportStatus&) const = default;
    bool supported{}; // this release has the library and Steam's own is where it is expected
    bool enabled{};   // Steam's next start is set up for it
    bool active{};    // the running Steam already carries the variable
    // The gate's check failed when the running Steam started, with the
    // library this release has: a restart alone won't start it.
    bool failed{};
};
StreamSupportStatus stream_support_status(const StreamSupportPaths& paths);

// Runs the gate; true when it succeeded. Tests supply their own.
using StreamSupportRun = std::function<bool(const std::vector<std::string>& command)>;
// Makes Steam's next start match `enabled`, and a Steam that already carries
// the variable follow at once: on, it gets a fresh copy of the library; off,
// its link leads to Steam's own library and a loaded library goes idle. A
// failed change leaves what was there. Throws UiError.
void configure_stream_support(bool enabled, const StreamSupportPaths& paths,
                              std::function<void()> reload = {}, StreamSupportRun run = {});

// The preference writer's save and its reconciliation at startup and Reload:
// the boot unit (startup.hpp), then streaming support, which needs
// replacement. When streaming support cannot follow, its saved flag goes back
// and the caller gets a StartupPreferencesWarning, as for the boot unit.
// Both units live in `paths.unit_directory`; tests supply `reload` and `run`.
void save_preferences_with_integration(const Settings& settings, const fs::path& executable,
                                       const fs::path& config_dir, const StreamSupportPaths& paths,
                                       std::function<void()> reload = {}, StreamSupportRun run = {});
void reconcile_integration(const Settings& settings, const fs::path& executable,
                           const fs::path& config_dir, const StreamSupportPaths& paths,
                           std::function<void()> reload = {}, StreamSupportRun run = {});
} // namespace framekeyboard
