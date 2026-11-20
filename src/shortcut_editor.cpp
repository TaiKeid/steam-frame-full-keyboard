#include "framekeyboard/shortcut_editor.hpp"
#include "framekeyboard/shortcut_launch.hpp"

#include <algorithm>
#include <glib.h>
#include <map>

namespace framekeyboard {
namespace limits = shortcut_limits;
namespace {
// Code points, the line breaks apart; false for control characters, more
// line breaks than `breaks`, or invalid UTF-8.
bool count_text(const std::string& text, std::size_t breaks, std::size_t& characters) {
    characters = 0;
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return false;
    }
    std::size_t seen = 0;
    for (const char* at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
        const gunichar c = g_utf8_get_char(at);
        if (c == '\n') {
            if (++seen > breaks) {
                return false;
            }
            continue;
        }
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) {
            return false;
        }
        ++characters;
    }
    return true;
}
// The first `count` code points of valid text.
std::string first_characters(const std::string& text, std::size_t count) {
    const char* end = text.data();
    for (std::size_t i = 0; i < count && end < text.data() + text.size(); ++i) {
        end = g_utf8_next_char(end);
    }
    return {text.data(), end};
}
} // namespace

bool fits_label(const std::string& text) {
    std::size_t characters = 0;
    return count_text(text, limits::label_lines - 1, characters) &&
           characters <= limits::label_characters;
}
bool fits_profile_name(const std::string& text) {
    std::size_t characters = 0;
    return count_text(text, 0, characters) && characters <= limits::profile_name_characters;
}
bool fits_command(const std::string& text) {
    std::size_t characters = 0;
    return text.size() <= limits::command_bytes && count_text(text, 0, characters);
}
std::string chord_text(const ShortcutChord& chord) {
    static const std::map<std::string, std::string> modifiers{
        {"ControlLeft", "Ctrl"}, {"ControlRight", "Ctrl"}, {"ShiftLeft", "Shift"},
        {"ShiftRight", "Shift"}, {"AltLeft", "Alt"},       {"AltRight", "AltGr"},
        {"MetaLeft", "Super"},   {"MetaRight", "Super"}};
    static const std::map<std::string, std::string> keys{{"Escape", "Esc"},
                                                         {"Backspace", "Backspace"},
                                                         {"ArrowLeft", "←"},
                                                         {"ArrowRight", "→"},
                                                         {"ArrowUp", "↑"},
                                                         {"ArrowDown", "↓"},
                                                         {"PageUp", "Page Up"},
                                                         {"PageDown", "Page Down"},
                                                         {"PrintScreen", "Print"},
                                                         {"ScrollLock", "Scroll"},
                                                         {"ContextMenu", "Menu"},
                                                         {"CapsLock", "Caps Lock"},
                                                         {"NumLock", "Num Lock"},
                                                         {"Backquote", "`"},
                                                         {"Minus", "-"},
                                                         {"Equal", "="},
                                                         {"BracketLeft", "["},
                                                         {"BracketRight", "]"},
                                                         {"Backslash", "\\"},
                                                         {"Semicolon", ";"},
                                                         {"Quote", "'"},
                                                         {"Comma", ","},
                                                         {"Period", "."},
                                                         {"Slash", "/"},
                                                         {"NumpadEnter", "Num Enter"}};
    std::string text;
    for (const auto& name : chord.modifiers) {
        const auto found = modifiers.find(name);
        text += (found == modifiers.end() ? name : found->second) + " + ";
    }
    if (chord.key.empty()) {
        return text.empty() ? text : text.substr(0, text.size() - 3);
    }
    std::string key = chord.key;
    if (const auto found = keys.find(key); found != keys.end()) {
        key = found->second;
    } else if (key.size() == 4 && key.starts_with("Key")) {
        key = key.substr(3);
    } else if (key.starts_with("Digit")) {
        key = key.substr(5);
    } else if (key.starts_with("Numpad") && key.size() == 7) {
        key = "Num " + key.substr(6);
    }
    return text + key;
}

ShortcutEditor::ShortcutEditor(ShortcutModel model, const std::string* showing)
    : model_(std::move(model)), original_(model_) {
    // A Run button shows Command when it has one, as one read from the file does.
    for (auto& profile : model_.profiles) {
        for (auto& button : profile.buttons) {
            button.action.command_chosen = !button.action.command.empty();
        }
    }
    original_ = saved(); // as a save would write it, so an untouched copy is unchanged
    for (std::size_t i = 0; showing && i < model_.profiles.size(); ++i) {
        if (model_.profiles[i].id == *showing) {
            profile_ = i;
        }
    }
    if (!profile_ && !model_.profiles.empty()) {
        profile_ = 0;
    }
    select_button(selected_profile() && !selected_profile()->buttons.empty()
                      ? std::optional<std::size_t>(0)
                      : std::nullopt);
}
ShortcutModel ShortcutEditor::saved() const {
    auto model = model_;
    for (auto& profile : model.profiles) {
        for (auto& button : profile.buttons) {
            auto& action = button.action;
            if (action.kind != ShortcutAction::Kind::Keys) {
                action.chords.clear();
            }
            if (action.kind != ShortcutAction::Kind::Macro) {
                action.steps.clear();
                action.repeat = {};
            } else if (action.repeat.mode != MacroRepeat::Mode::Times) {
                action.repeat.count = 0; // remembered for Times while editing (set_repeat), not saved
            }
            // Run keeps the one chosen of Script and Command.
            if (action.kind != ShortcutAction::Kind::Run || action.command_chosen) {
                action.script.clear();
            }
            if (action.kind != ShortcutAction::Kind::Run || !action.command_chosen) {
                action.command.clear();
            }
            action.command_chosen = !action.command.empty();
        }
    }
    return model;
}
ShortcutAction* ShortcutEditor::macro() {
    const auto* button = selected_button();
    if (!button || button->action.kind != ShortcutAction::Kind::Macro) {
        return nullptr;
    }
    return &model_.profiles[*profile_].buttons[*button_].action;
}
void ShortcutEditor::set_kind(ShortcutAction::Kind kind) {
    if (!selected_button()) {
        return;
    }
    auto& action = model_.profiles[*profile_].buttons[*button_].action;
    if (kind == ShortcutAction::Kind::Macro && action.kind == ShortcutAction::Kind::Keys &&
        action.steps.empty() && !action.chords.empty()) {
        MacroStep step;
        step.kind = MacroStep::Kind::Keys;
        step.keys = action.chords;
        action.steps.push_back(std::move(step));
    }
    action.kind = kind;
}
bool ShortcutEditor::can_add_step() const {
    const auto* button = selected_button();
    return button && button->action.kind == ShortcutAction::Kind::Macro &&
           button->action.steps.size() < limits::steps;
}
void ShortcutEditor::add_step(MacroStep step) {
    if (!can_add_step()) {
        return;
    }
    if (step.kind == MacroStep::Kind::Keys &&
        (step.keys.empty() || step.keys.size() > limits::step_chords)) {
        return;
    }
    if (step.kind == MacroStep::Kind::Wait) {
        step.wait_ms = std::clamp(step.wait_ms, limits::wait_min_ms, limits::wait_max_ms);
    }
    macro()->steps.push_back(std::move(step));
}
void ShortcutEditor::set_step_keys(std::size_t index, std::vector<ShortcutChord> keys) {
    auto* action = macro();
    if (!action || index >= action->steps.size() || action->steps[index].kind != MacroStep::Kind::Keys ||
        keys.empty() || keys.size() > limits::step_chords) {
        return;
    }
    action->steps[index].keys = std::move(keys);
}
void ShortcutEditor::set_wait(std::size_t index, int ms) {
    auto* action = macro();
    if (action && index < action->steps.size() && action->steps[index].kind == MacroStep::Kind::Wait) {
        action->steps[index].wait_ms = std::clamp(ms, limits::wait_min_ms, limits::wait_max_ms);
    }
}
void ShortcutEditor::delete_step(std::size_t index) {
    auto* action = macro();
    if (action && index < action->steps.size()) {
        action->steps.erase(action->steps.begin() + static_cast<std::ptrdiff_t>(index));
    }
}
void ShortcutEditor::move_step(std::size_t from, std::size_t to) {
    auto* action = macro();
    if (!action || from >= action->steps.size() || to >= action->steps.size() || from == to) {
        return;
    }
    auto moved = std::move(action->steps[from]);
    action->steps.erase(action->steps.begin() + static_cast<std::ptrdiff_t>(from));
    action->steps.insert(action->steps.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
}
void ShortcutEditor::set_repeat(MacroRepeat repeat) {
    if (auto* action = macro()) {
        // Once and While held keep the count for while the editor is open, so Times shows it again
        // (R371); saved() leaves it out, as the file has a count only for Times.
        if (repeat.mode == MacroRepeat::Mode::Times) {
            repeat.count = std::clamp(repeat.count, limits::repeat_min, limits::repeat_max);
        }
        action->repeat = repeat;
    }
}
const ShortcutProfile* ShortcutEditor::selected_profile() const {
    return profile_ && *profile_ < model_.profiles.size() ? &model_.profiles[*profile_] : nullptr;
}
const ShortcutButton* ShortcutEditor::selected_button() const {
    const auto* profile = selected_profile();
    return profile && button_ && *button_ < profile->buttons.size() ? &profile->buttons[*button_]
                                                                    : nullptr;
}
void ShortcutEditor::select_profile(std::size_t index) {
    if (index >= model_.profiles.size()) {
        return;
    }
    profile_ = index;
    button_ = model_.profiles[index].buttons.empty() ? std::nullopt : std::optional<std::size_t>(0);
}
void ShortcutEditor::select_button(std::optional<std::size_t> index) {
    const auto* profile = selected_profile();
    button_ = profile && index && *index < profile->buttons.size() ? index : std::nullopt;
}
void ShortcutEditor::add_profile(std::string name) {
    if (!can_add_profile()) {
        return;
    }
    // p1, p2, …: the first such ID no profile has.
    std::string id;
    for (std::size_t n = 1; id.empty() || model_.profile(id); ++n) {
        id = "p" + std::to_string(n);
    }
    model_.profiles.push_back({std::move(id), fits_profile_name(name) ? name : std::string(), {}});
    profile_ = model_.profiles.size() - 1;
    button_.reset();
}
void ShortcutEditor::rename_profile(std::string name) {
    if (!profile_) {
        return;
    }
    std::size_t characters = 0;
    if (!count_text(name, 0, characters)) {
        return;
    }
    model_.profiles[*profile_].name = first_characters(name, limits::profile_name_characters);
}
void ShortcutEditor::delete_profile() {
    if (!profile_) {
        return;
    }
    const auto id = model_.profiles[*profile_].id;
    model_.profiles.erase(model_.profiles.begin() + static_cast<std::ptrdiff_t>(*profile_));
    std::erase_if(model_.apps, [&](const AppTie& tie) { return tie.profile == id; });
    model_.forget_missing_picks(); // the next pick takes its place (R350)
    if (model_.profiles.empty()) {
        profile_.reset();
        button_.reset();
    } else {
        select_profile(std::min(*profile_, model_.profiles.size() - 1));
    }
}
void ShortcutEditor::move_profile(std::size_t from, std::size_t to) {
    auto& profiles = model_.profiles;
    if (from >= profiles.size() || to >= profiles.size() || from == to) {
        return;
    }
    const bool selected = profile_ == from;
    auto moved = std::move(profiles[from]);
    profiles.erase(profiles.begin() + static_cast<std::ptrdiff_t>(from));
    profiles.insert(profiles.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    // The selection follows its profile.
    if (selected) {
        profile_ = to;
    } else if (profile_ && from < *profile_ && to >= *profile_) {
        --*profile_;
    } else if (profile_ && from > *profile_ && to <= *profile_) {
        ++*profile_;
    }
}
bool ShortcutEditor::can_add_button() const {
    const auto* profile = selected_profile();
    return profile && profile->buttons.size() < limits::buttons;
}
void ShortcutEditor::add_button() {
    if (!can_add_button()) {
        return;
    }
    auto& buttons = model_.profiles[*profile_].buttons;
    buttons.push_back({}); // An empty label and an empty shortcut.
    button_ = buttons.size() - 1;
}
void ShortcutEditor::delete_button(std::size_t index) {
    const auto* profile = selected_profile();
    if (!profile || index >= profile->buttons.size()) {
        return;
    }
    auto& buttons = model_.profiles[*profile_].buttons;
    buttons.erase(buttons.begin() + static_cast<std::ptrdiff_t>(index));
    if (buttons.empty()) {
        button_.reset();
    } else if (button_ && *button_ >= index) {
        // The one after it, or the last; the selection never jumps ahead.
        button_ = *button_ == index ? std::min(index, buttons.size() - 1) : *button_ - 1;
    }
}
void ShortcutEditor::move_button(std::size_t from, std::size_t to) {
    const auto* profile = selected_profile();
    if (!profile || from >= profile->buttons.size() || to >= profile->buttons.size() || from == to) {
        return;
    }
    auto& buttons = model_.profiles[*profile_].buttons;
    auto moved = std::move(buttons[from]);
    buttons.erase(buttons.begin() + static_cast<std::ptrdiff_t>(from));
    buttons.insert(buttons.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    if (button_ == from) {
        button_ = to;
    } else if (button_ && from < *button_ && to >= *button_) {
        --*button_;
    } else if (button_ && from > *button_ && to <= *button_) {
        ++*button_;
    }
}
bool ShortcutEditor::can_take(std::size_t profile) const {
    return profile < model_.profiles.size() && model_.profiles[profile].buttons.size() < limits::buttons;
}
void ShortcutEditor::carry_button(std::size_t index, std::size_t profile, bool copy) {
    const auto* selected = selected_profile();
    if (!selected || index >= selected->buttons.size() || profile == *profile_ || !can_take(profile)) {
        return;
    }
    // Copied first: the target's vector may grow over the source's storage.
    auto button = selected->buttons[index];
    model_.profiles[profile].buttons.push_back(std::move(button));
    if (!copy) {
        delete_button(index);
    }
}
void ShortcutEditor::set_run_command(bool command) {
    const auto* button = selected_button();
    if (button && button->action.kind == ShortcutAction::Kind::Run) {
        model_.profiles[*profile_].buttons[*button_].action.command_chosen = command;
    }
}
void ShortcutEditor::set_script(std::string name) {
    const auto* button = selected_button();
    if (button && button->action.kind == ShortcutAction::Kind::Run &&
        (name.empty() || script_name_valid(name))) {
        model_.profiles[*profile_].buttons[*button_].action.script = std::move(name);
    }
}
void ShortcutEditor::set_command(std::string command) {
    const auto* button = selected_button();
    if (button && button->action.kind == ShortcutAction::Kind::Run && fits_command(command)) {
        model_.profiles[*profile_].buttons[*button_].action.command = std::move(command);
    }
}
void ShortcutEditor::set_label(std::string label) {
    if (selected_button() && fits_label(label)) {
        model_.profiles[*profile_].buttons[*button_].label = std::move(label);
    }
}
void ShortcutEditor::set_chords(std::vector<ShortcutChord> chords) {
    const auto* button = selected_button();
    if (!button || button->action.kind != ShortcutAction::Kind::Keys) {
        return;
    }
    if (chords.size() > limits::chords) {
        chords.resize(limits::chords);
    }
    model_.profiles[*profile_].buttons[*button_].action.chords = std::move(chords);
}
std::vector<std::size_t> ShortcutEditor::ties() const {
    std::vector<std::size_t> result;
    if (const auto* profile = selected_profile()) {
        for (std::size_t i = 0; i < model_.apps.size(); ++i) {
            if (model_.apps[i].profile == profile->id) {
                result.push_back(i);
            }
        }
    }
    return result;
}
void ShortcutEditor::remove_tie(std::size_t app_index) {
    if (app_index < model_.apps.size()) {
        model_.apps.erase(model_.apps.begin() + static_cast<std::ptrdiff_t>(app_index));
    }
}

ShortcutsCheck check_shortcuts(const std::filesystem::path& config_dir) {
    ShortcutStore store(shortcuts_path(config_dir));
    const auto loaded = store.load();
    switch (loaded.state) {
    case ShortcutsLoad::State::Missing:
        return {"shortcuts: none", true};
    case ShortcutsLoad::State::Invalid:
        return {"shortcuts: invalid: " + loaded.reason, false};
    case ShortcutsLoad::State::Valid:
        break;
    }
    std::size_t buttons = 0;
    for (const auto& profile : loaded.model.profiles) {
        buttons += profile.buttons.size();
    }
    return {"shortcuts: " + std::to_string(loaded.model.profiles.size()) + " profiles, " +
                std::to_string(buttons) + " buttons, " + std::to_string(loaded.model.apps.size()) +
                " assigned apps",
            true};
}
} // namespace framekeyboard
