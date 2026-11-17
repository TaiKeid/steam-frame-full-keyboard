#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The shortcut bar's model and its file (docs/shortcut-bar-design.md,
// Storage), and which bar shows. Plain data and pure logic: nothing here
// draws, sends keys or starts programs. Labels, macro text, commands and
// script names are the user's own and never appear in an error or a log.
namespace framekeyboard {
// The design's limits. Characters are Unicode code points.
namespace shortcut_limits {
inline constexpr std::size_t bars = 32;
inline constexpr std::size_t buttons = 64;          // in a bar
inline constexpr std::size_t label_characters = 24; // both lines together
inline constexpr std::size_t label_lines = 2;
inline constexpr std::size_t bar_name_characters = 32;
inline constexpr std::size_t chords = 4;            // in a shortcut
inline constexpr std::size_t steps = 32;            // in a macro
inline constexpr std::size_t text_characters = 256; // in a text step
inline constexpr int wait_min_ms = 100, wait_max_ms = 10000;
inline constexpr int repeat_min = 2, repeat_max = 99;
inline constexpr std::size_t command_bytes = 512;
inline constexpr std::size_t script_bytes = 64;
inline constexpr std::size_t apps = 128; // app ties
inline constexpr std::size_t file_bytes = 512 * 1024;
// Not in the design's table: the file's own identifiers and the names it
// keeps for assigned apps, bounded so a hand-edited file stays small.
inline constexpr std::size_t bar_id_bytes = 32;
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
struct MacroStep {
    enum class Kind { Keys, Text, Wait };
    Kind kind{Kind::Keys};
    ShortcutChord keys; // Keys
    std::string text;   // Text: typed as the keyboard types text
    int wait_ms{};      // Wait
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
    bool operator==(const ShortcutAction&) const = default;
};
struct ShortcutButton {
    std::string label; // Up to two lines, split by one '\n'.
    ShortcutAction action;
    bool operator==(const ShortcutButton&) const = default;
};
struct ShortcutBar {
    std::string id, name;
    std::vector<ShortcutButton> buttons;
    bool operator==(const ShortcutBar&) const = default;
};
// The app that has the keys, as far as it can be told (design, Apps).
// Steam's dashboard and an unknown focus have none.
struct AppIdentity {
    enum class Kind { Steam, Program, Desktop };
    Kind kind{Kind::Program};
    std::string id;
    bool operator==(const AppIdentity&) const = default;
};
// An assigned app: the name is looked up once when it is assigned.
struct AppTie {
    AppIdentity app;
    std::string name, bar;
    bool operator==(const AppTie&) const = default;
};
struct ShortcutModel {
    std::vector<ShortcutBar> bars;
    std::vector<AppTie> apps;
    // The free bar rule 2 returns to; empty for none.
    std::string free_bar;
    bool operator==(const ShortcutModel&) const = default;

    const ShortcutBar* bar(const std::string& id) const;
    // The bar tied to this app, or null.
    const ShortcutBar* bar_for(const AppIdentity& app) const;
    // Whether no app is tied to this bar.
    bool free(const std::string& id) const;
};

// Every rule of the file's content: limits, key and modifier names, unique
// bar IDs, ties and free_bar naming existing bars, text that is valid UTF-8
// without control characters. Loading and the editor both use it. The
// reason, when invalid, names the place by position (bar 2, button 5) and
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
// Reads and saves one shortcuts.json. It remembers whether the file was
// invalid when loaded: such a file is never overwritten, and the first save
// moves it to shortcuts.json.bad before writing. Synchronous; callers keep
// it off the VR thread.
class ShortcutStore {
  public:
    explicit ShortcutStore(std::filesystem::path path) : path_(std::move(path)) {}
    ShortcutsLoad load();
    // Validates, then writes atomically, readable by the user alone. The
    // reason when nothing was written: an invalid model, an invalid file that
    // could not be moved aside, or a failed write.
    std::optional<std::string> save(const ShortcutModel& model);
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
    // The file on disk was invalid when last loaded and is still there.
    bool keep_aside_{};
};

// Which bar shows (design, Toolbar button and Which bar shows). Pure state:
// App feeds it focus changes and presses and draws what it says. It owns
// the user's toolbar switch (shortcuts_switch) while the keyboard runs; the
// model it reads and updates (free_bar) belongs to the caller.
class ShortcutBarChoice {
  public:
    // What the caller should save after a call.
    struct Changes {
        bool free_bar{}, shortcuts_switch{};
    };
    // `enabled`: Shortcuts and macros is on and no shell runs the keyboard.
    // `on`: the saved toolbar switch.
    void configure(const ShortcutModel& model, bool enabled, bool on);
    // Another app has the keys: none for Steam's dashboard or an app that
    // can't be identified. Every call is a change of focus.
    void focus(const ShortcutModel& model, std::optional<AppIdentity> app);
    // The toolbar button's three cases.
    Changes toolbar(const ShortcutModel& model);
    // A bar chosen in the picker: it shows until the focus changes, and a
    // free one becomes the free bar rule 2 returns to.
    Changes pick(ShortcutModel& model, const std::string& bar);
    // The model changed (an editor save, a tie): a bar that no longer exists
    // stops showing, and a free_bar naming none is cleared.
    Changes model_changed(ShortcutModel& model);

    // The bar showing, or null. With the empty bar or nothing showing, null.
    const std::string* shown() const { return shown_ ? &*shown_ : nullptr; }
    // The two-slot bar the toolbar button opens when no bar is due.
    bool empty_bar() const { return empty_; }
    // Something shows: the button reads on; crossed otherwise.
    bool button_on() const { return shown_.has_value() || empty_; }
    bool switch_on() const { return on_; }
    bool enabled() const { return enabled_; }
    // The bar due for the focused app: the picker's choice since the focus
    // came, else rule 1, else rule 2; none for rule 3.
    std::optional<std::string> due(const ShortcutModel& model) const;

  private:
    void resolve(const ShortcutModel& model);
    bool enabled_{}, on_{true};
    std::optional<AppIdentity> app_;
    // A bar chosen in the picker for this focus; cleared when it moves on.
    std::optional<std::string> chosen_;
    std::optional<std::string> shown_;
    bool empty_{};
};
} // namespace framekeyboard
