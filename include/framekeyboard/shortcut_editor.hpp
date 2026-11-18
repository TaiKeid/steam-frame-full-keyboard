#pragma once
#include "shortcuts.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// The shortcut editor's pending copy (docs/shortcut-bar-design.md, Settings
// and the editor): the profiles as edited, the profile and button selected, and
// every change the editor makes. It enforces the limits itself, so the
// model it holds always validates; the editor's controls ask can_*() and
// show disabled at a limit. Pure data: no drawing, no input, no saving.
class ShortcutEditor {
  public:
    // Opens on `showing` when it names a profile, else on the first profile.
    ShortcutEditor(ShortcutModel model, const std::string* showing);
    // The pending copy. A button's Shortcut and Macro content both stay in
    // it while the editor is open, whichever is chosen (design, Settings and
    // the editor); saved() keeps only the chosen one.
    const ShortcutModel& model() const { return model_; }
    ShortcutModel saved() const;
    // Something the save would change, from the copy the editor opened with.
    bool changed() const { return saved() != original_; }

    std::optional<std::size_t> profile() const { return profile_; }
    std::optional<std::size_t> button() const { return button_; }
    const ShortcutProfile* selected_profile() const;
    const ShortcutButton* selected_button() const;
    void select_profile(std::size_t index);
    void select_button(std::optional<std::size_t> index);

    bool can_add_profile() const { return model_.profiles.size() < shortcut_limits::profiles; }
    // A new, empty bar named `name`, selected. Its ID is new in the model.
    void add_profile(std::string name);
    // Names are cut to the limit, as a field typed in already keeps them.
    void rename_profile(std::string name);
    // The selected profile goes, with the ties to it and the free profile naming it.
    void delete_profile();
    void move_profile(std::size_t from, std::size_t to);

    bool can_add_button() const;
    // A button with an empty label and an empty shortcut, selected.
    void add_button();
    void delete_button(std::size_t index);
    void move_button(std::size_t from, std::size_t to);
    void set_label(std::string label);
    // Carrying a button to another profile (design, Settings and the
    // editor): whether `profile` can take one more, and the selected
    // profile's button `index` moved or copied to its end. The selection
    // stays on the selected profile.
    bool can_take(std::size_t profile) const;
    void carry_button(std::size_t index, std::size_t profile, bool copy);
    // The selected button's shortcut, at most four combinations; a button
    // whose action is a macro or Run is left as it is.
    void set_chords(std::vector<ShortcutChord> chords);

    // Shortcut, Macro or Run for the selected button, each keeping its
    // content. A shortcut turned into a macro with no steps yet starts as
    // one Keys step holding its combinations.
    void set_kind(ShortcutAction::Kind kind);
    // The selected Run button's Script | Command switch, each keeping its
    // content; its script (a valid name, or empty for none) and its command
    // (fits_command, or empty).
    void set_run_command(bool command);
    void set_script(std::string name);
    void set_command(std::string command);
    // The selected button's macro: steps added, changed, moved, deleted, at
    // most 32, each valid as it is set.
    bool can_add_step() const;
    void add_step(MacroStep step);
    void set_step_keys(std::size_t index, std::vector<ShortcutChord> keys);
    void set_wait(std::size_t index, int ms);
    void delete_step(std::size_t index);
    void move_step(std::size_t from, std::size_t to);
    void set_repeat(MacroRepeat repeat);

    // The model's ties to the selected profile, as indices into model().apps.
    std::vector<std::size_t> ties() const;
    void remove_tie(std::size_t app_index);

  private:
    // The selected button's macro, when it is one.
    ShortcutAction* macro();
    ShortcutModel model_, original_;
    std::optional<std::size_t> profile_, button_;
};
// Text as a label or a profile name may hold it: without control characters,
// one line break at most in a label, and no more than the limit's
// characters. A field's typing checks each insertion with this.
bool fits_label(const std::string& text);
bool fits_profile_name(const std::string& text);
// A command as Run holds it: one line of valid UTF-8 without control
// characters, at most shortcut_limits::command_bytes bytes.
bool fits_command(const std::string& text);
// How a combination reads: Ctrl + Shift + K, F13, Super.
std::string chord_text(const ShortcutChord& chord);

// What `framekeyboard --check` says about shortcuts.json: a line, and
// whether the file is fine (missing counts as fine). Never quotes it.
struct ShortcutsCheck {
    std::string line;
    bool ok{true};
};
ShortcutsCheck check_shortcuts(const std::filesystem::path& config_dir);
} // namespace framekeyboard
