#pragma once
#include "shortcuts.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// The shortcut editor's pending copy (docs/shortcut-bar-design.md, Settings
// and the editor): the bars as edited, the bar and button selected, and
// every change the editor makes. It enforces the limits itself, so the
// model it holds always validates; the editor's controls ask can_*() and
// show disabled at a limit. Pure data: no drawing, no input, no saving.
class ShortcutEditor {
  public:
    // Opens on `showing` when it names a bar, else on the first bar.
    ShortcutEditor(ShortcutModel model, const std::string* showing);
    const ShortcutModel& model() const { return model_; }
    // Something differs from the copy the editor opened with.
    bool changed() const { return model_ != original_; }

    std::optional<std::size_t> bar() const { return bar_; }
    std::optional<std::size_t> button() const { return button_; }
    const ShortcutBar* selected_bar() const;
    const ShortcutButton* selected_button() const;
    void select_bar(std::size_t index);
    void select_button(std::optional<std::size_t> index);

    bool can_add_bar() const { return model_.bars.size() < shortcut_limits::bars; }
    // A new, empty bar named `name`, selected. Its ID is new in the model.
    void add_bar(std::string name);
    // Names are cut to the limit, as a field typed in already keeps them.
    void rename_bar(std::string name);
    // The selected bar goes, with the ties to it and the free bar naming it.
    void delete_bar();
    void move_bar(std::size_t from, std::size_t to);

    bool can_add_button() const;
    // A button with an empty label and an empty shortcut, selected.
    void add_button();
    void delete_button(std::size_t index);
    void move_button(std::size_t from, std::size_t to);
    void set_label(std::string label);
    // The selected button's shortcut, at most four combinations; a button
    // whose action is a macro or Run is left as it is.
    void set_chords(std::vector<ShortcutChord> chords);

    // The model's ties to the selected bar, as indices into model().apps.
    std::vector<std::size_t> ties() const;
    void remove_tie(std::size_t app_index);

  private:
    ShortcutModel model_, original_;
    std::optional<std::size_t> bar_, button_;
};
// Text as a label or a bar name may hold it: without control characters,
// one line break at most in a label, and no more than the limit's
// characters. A field's typing checks each insertion with this.
bool fits_label(const std::string& text);
bool fits_bar_name(const std::string& text);
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
