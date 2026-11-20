#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Starting a Run button's program (docs/shortcut-bar-design.md, Run): a
// script from the config folder's scripts/, or one command line for
// /bin/sh -c. The child gets its own session, /dev/null for its standard
// streams, no other open file, the home folder as its working directory,
// default signal handling and scheduling, and the keyboard's environment
// without what starting the keyboard added. Nothing waits for it and no
// finished child stays behind: it is started through a short-lived
// intermediate process, the only one waited for. Script names and commands
// are the user's own: never logged, never in an error.
namespace framekeyboard {
// Why a start failed. Logged as its number only.
enum class LaunchFailure : int {
    None = 0,
    NoAction = 1,   // neither a script nor a command
    BadName = 2,    // not a plain file name a profile could hold
    Missing = 3,    // no such file in scripts/
    NotRegular = 4, // a folder, a link or a device
    NotStarted = 5, // the system refused to run it, through /bin/sh too
    System = 6,     // no process could be made
};

// Where Run looks for scripts: scripts/ in the config folder.
std::filesystem::path scripts_folder(const std::filesystem::path& config_dir);
// Whether `name` is a script name a profile can hold (validate_shortcuts'
// rule): a plain file name, no folder part, at most 64 bytes, valid UTF-8
// without control characters. Defined in shortcuts.cpp.
bool script_name_valid(std::string_view name);
// The folder's scripts as the editor lists them: regular files only (no
// folders, links or hidden files), names a profile can hold, sorted, at
// most 64. Empty when the folder can't be read.
std::vector<std::string> list_scripts(const std::filesystem::path& folder);
inline constexpr std::size_t listed_scripts = 64;
// Makes the folder, private to the user, if it isn't there; false if it
// can't be made.
bool make_scripts_folder(const std::filesystem::path& folder);

// The child's environment: the keyboard's, given as NAME=value entries,
// without what starting the keyboard added. Pure, for tests.
std::vector<std::string> child_environment(const std::vector<std::string>& keyboard);
// The keyboard's own environment, as entries.
std::vector<std::string> current_environment();

struct LaunchRequest {
    // One of the two: a script's file name in `scripts`, or a command.
    std::string script, command;
    std::filesystem::path scripts, home;
    std::vector<std::string> environment;
};
// Starts it now and reports whether it started: blocks until the child has
// executed its program or failed to, so keep it off the VR thread.
LaunchFailure launch(const LaunchRequest& request);

// Runs starts on a detached thread of its own, one at a time, so neither a
// slow start nor destroying this ever holds up the caller. Results are
// taken on a tick, by the ID given with the request.
class ShortcutLauncher {
  public:
    struct Result {
        std::uint64_t id{};
        LaunchFailure failure{};
    };
    ShortcutLauncher();
    ~ShortcutLauncher();
    ShortcutLauncher(const ShortcutLauncher&) = delete;
    ShortcutLauncher& operator=(const ShortcutLauncher&) = delete;
    void start(std::uint64_t id, LaunchRequest request);
    std::optional<Result> take();
    // Shared with the thread; defined in shortcut_launch.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};

// Reads the scripts folder for the editor's list on a detached thread of its
// own, never the VR thread. The source gives the names: tests and the tour
// give their own; without one the folder is made if missing, then listed
// (list_scripts). The newest read wins; results are taken on a tick.
using ScriptListSource = std::function<std::vector<std::string>(const std::filesystem::path&)>;
// Whether `name` is a script a press would start: a valid name for a regular
// file in `folder`, not a link. Reads the folder: off the VR thread.
bool script_present(const std::filesystem::path& folder, const std::string& name);
// A read: the names listed, at most listed_scripts, and which of the names
// asked about are scripts in the folder though not listed (R354): a button's
// chosen script past the first 64 is there, not missing.
struct ScriptList {
    std::vector<std::string> names;
    std::vector<std::string> also_present;
};
class ScriptLister {
  public:
    explicit ScriptLister(ScriptListSource source);
    ~ScriptLister();
    ScriptLister(const ScriptLister&) = delete;
    ScriptLister& operator=(const ScriptLister&) = delete;
    // Lists `folder`, and checks each of `chosen` the list leaves out against
    // the folder itself (with a source, the source's names stand in for it).
    void read(const std::filesystem::path& folder, std::vector<std::string> chosen = {});
    std::optional<ScriptList> take();
    // Ends its thread, if one runs, dropping a read under way and its
    // answer; the next read starts another (R407).
    void stop();
    // Shared with the thread; defined in shortcut_launch.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
} // namespace framekeyboard
