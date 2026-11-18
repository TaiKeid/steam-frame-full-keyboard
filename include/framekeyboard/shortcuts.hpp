#pragma once
#include "app_identity.hpp"
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The shortcut bar's model and its file (docs/shortcut-bar-design.md,
// Storage), and which profile shows. Plain data and pure logic: nothing here
// draws, sends keys or starts programs. Labels, names, recorded keys,
// commands and script names are the user's own and never appear in an
// error or a log.
namespace framekeyboard {
// The design's limits. Characters are Unicode code points.
namespace shortcut_limits {
inline constexpr std::size_t profiles = 32;
inline constexpr std::size_t buttons = 64;          // in a profile
inline constexpr std::size_t label_characters = 24; // both lines together
inline constexpr std::size_t label_lines = 2;
inline constexpr std::size_t profile_name_characters = 32;
inline constexpr std::size_t chords = 4;            // in a shortcut
inline constexpr std::size_t steps = 32;            // in a macro
inline constexpr std::size_t step_chords = 32;      // in a macro's Keys step
inline constexpr int wait_min_ms = 100, wait_max_ms = 10000;
inline constexpr int repeat_min = 2, repeat_max = 99;
inline constexpr std::size_t command_bytes = 512;
inline constexpr std::size_t script_bytes = 64;
inline constexpr std::size_t apps = 128; // app ties
inline constexpr std::size_t file_bytes = 512 * 1024;
// Not in the design's table: the file's own identifiers and the names it
// keeps for assigned apps, bounded so a hand-edited file stays small.
inline constexpr std::size_t profile_id_bytes = 32;
inline constexpr std::size_t app_id_bytes = 255;
inline constexpr std::size_t app_name_characters = 128;
} // namespace shortcut_limits

// One key combination: modifiers pressed in this order, then `key`. Names
// are the logical key IDs of key_code(), never evdev or Steam codes. An
// empty key is a tap of the modifiers alone, such as Super.
struct ShortcutChord {
    std::vector<std::string> modifiers;
    std::string key;
    bool operator==(const ShortcutChord&) const = default;
};
// A macro presses keys; it never types text (design, Macro). A Keys step
// is combinations recorded in a row, each sent as a shortcut's is.
struct MacroStep {
    enum class Kind { Keys, Wait };
    Kind kind{Kind::Keys};
    std::vector<ShortcutChord> keys; // Keys: 1 to 32 combinations
    int wait_ms{};                   // Wait
    bool operator==(const MacroStep&) const = default;
};
struct MacroRepeat {
    enum class Mode { Once, Times, WhileHeld };
    Mode mode{Mode::Once};
    int count{}; // Times only
    bool operator==(const MacroRepeat&) const = default;
};
struct ShortcutAction {
    enum class Kind { Keys, Macro, Run };
    Kind kind{Kind::Keys};
    std::vector<ShortcutChord> chords; // Keys
    std::vector<MacroStep> steps;      // Macro
    MacroRepeat repeat;                // Macro
    // Run: at most one is set. A script is a file name in the config
    // folder's scripts/; a command is one line for /bin/sh -c.
    std::string script, command;
    // Run: Command is chosen in the editor's Script | Command switch, which
    // keeps both while the editor is open. Not in the file, set from it as
    // whether a command is given, and no part of the action's content: two
    // actions that differ only in it are equal.
    bool command_chosen{};
    bool operator==(const ShortcutAction& other) const {
        return kind == other.kind && chords == other.chords && steps == other.steps &&
               repeat == other.repeat && script == other.script && command == other.command;
    }
};
struct ShortcutButton {
    std::string label; // Up to two lines, split by one '\n'.
    ShortcutAction action;
    bool operator==(const ShortcutButton&) const = default;
};
struct ShortcutProfile {
    std::string id, name;
    std::vector<ShortcutButton> buttons;
    bool operator==(const ShortcutProfile&) const = default;
};
// An assigned app (AppIdentity, app_identity.hpp): the name is looked up
// once when it is assigned.
struct AppTie {
    AppIdentity app;
    std::string name, profile;
    bool operator==(const AppTie&) const = default;
};
struct ShortcutModel {
    std::vector<ShortcutProfile> profiles;
    std::vector<AppTie> apps;
    // The free profile rule 2 returns to; empty for none.
    std::string free_profile;
    bool operator==(const ShortcutModel&) const = default;

    const ShortcutProfile* profile(const std::string& id) const;
    // The profile tied to this app, or null.
    const ShortcutProfile* profile_for(const AppIdentity& app) const;
    // Whether no app is tied to this profile.
    bool free(const std::string& id) const;
    // This app's tie, or null.
    const AppTie* tie(const AppIdentity& app) const;
    // Whether this app can be tied: it has a tie to move, or fewer than
    // shortcut_limits::apps apps have one.
    bool can_assign(const AppIdentity& app) const;
    // Ties the app to an existing profile, in place of its tie to any
    // other, with the name to show for it; false, changing nothing, past
    // the limit or for a profile that doesn't exist.
    bool assign(const AppIdentity& app, const std::string& name, const std::string& profile);
    // Removes the app's tie; false when it had none.
    bool unassign(const AppIdentity& app);
};

// Every rule of the file's content: limits, key and modifier names, unique
// profile IDs, ties and free_profile naming existing profiles, text that is valid UTF-8
// without control characters. Loading and the editor both use it. The
// reason, when invalid, names the place by position (profile 2, button 5) and
// never quotes the user's labels, text, commands or names.
std::optional<std::string> validate_shortcuts(const ShortcutModel& model);
// An action with nothing to do yet: a shortcut without combinations, a
// macro without steps, Run with neither a script nor a command. Valid, as
// the editor saves unfinished buttons; drawn disabled, and a press does nothing.
bool shortcut_action_empty(const ShortcutAction& action);
// Whether a key name is one of the eight modifiers.
bool shortcut_modifier(const std::string& name);

// The model as the file's JSON, and back. parse_shortcuts throws
// std::runtime_error with a reason as above; it validates too.
std::string serialize_shortcuts(const ShortcutModel& model);
ShortcutModel parse_shortcuts(const std::string& json);

// shortcuts.json beside config.json, in the config folder (--config-dir).
std::filesystem::path shortcuts_path(const std::filesystem::path& config_dir);

struct ShortcutsLoad {
    // No file is an empty model and fine; an invalid file loads nothing.
    enum class State { Missing, Valid, Invalid };
    State state{State::Missing};
    ShortcutModel model;
    std::string reason; // Invalid only; never quotes the file's content.
};
// Reads and saves one shortcuts.json. A save looks at the file first, as the
// user may have edited it by hand while the keyboard ran: one that doesn't
// parse is moved to shortcuts.json.bad, and a valid one that differs from
// what this store last read or wrote is kept as shortcuts.json.old; then the
// save goes ahead. Synchronous; callers keep it off the VR thread.
class ShortcutStore {
  public:
    explicit ShortcutStore(std::filesystem::path path) : path_(std::move(path)) {}
    ShortcutsLoad load();
    // Validates, then writes atomically, readable by the user alone. The
    // reason when nothing was written: an invalid model, a file that could
    // not be moved aside, or a failed write; or `unsynced` when the file
    // was written in place but its folder's sync failed (R336).
    std::optional<std::string> save(const ShortcutModel& model);
    static constexpr std::string_view unsynced = "the file could not be synced to disk";
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
    // The file's bytes as this store last read (when valid) or wrote them;
    // none before.
    std::optional<std::string> known_;
};

// Which profile shows (design, Toolbar button and Which profile shows). Pure state:
// App feeds it focus changes and presses and draws what it says. It owns
// the user's toolbar switch (shortcuts_switch) while the keyboard runs; the
// model it reads and updates (free_profile) belongs to the caller.
class ShortcutProfileChoice {
  public:
    // What the caller should save after a call.
    struct Changes {
        bool free_profile{}, shortcuts_switch{};
    };
    // `enabled`: Shortcuts and macros is on and no shell runs the keyboard.
    // `on`: the saved toolbar switch.
    void configure(const ShortcutModel& model, bool enabled, bool on);
    // Another app has the keys: none for Steam's dashboard or an app that
    // can't be identified. Every call is a change of focus.
    void focus(const ShortcutModel& model, std::optional<AppIdentity> app);
    // The toolbar button's three cases.
    Changes toolbar(const ShortcutModel& model);
    // A profile chosen in the picker: it shows until the focus changes, and a
    // free one becomes the free profile rule 2 returns to.
    Changes pick(ShortcutModel& model, const std::string& profile);
    // The model changed (an editor save, a tie): a profile that no longer exists
    // stops showing, and a free_profile naming none is cleared.
    Changes model_changed(ShortcutModel& model);

    // The profile showing, or null. With the empty bar or nothing showing, null.
    const std::string* shown() const { return shown_ ? &*shown_ : nullptr; }
    // The two-slot bar the toolbar button opens when no profile is due.
    bool empty_bar() const { return empty_; }
    // Something shows: the button reads on; crossed otherwise.
    bool button_on() const { return shown_.has_value() || empty_; }
    bool switch_on() const { return on_; }
    bool enabled() const { return enabled_; }
    // The profile due for the focused app: the picker's choice since the focus
    // came, else rule 1, else rule 2; none for rule 3.
    std::optional<std::string> due(const ShortcutModel& model) const;

  private:
    void resolve(const ShortcutModel& model);
    bool enabled_{}, on_{true};
    std::optional<AppIdentity> app_;
    // A profile chosen in the picker for this focus; cleared when it moves on.
    std::optional<std::string> chosen_;
    std::optional<std::string> shown_;
    bool empty_{};
};
} // namespace framekeyboard
