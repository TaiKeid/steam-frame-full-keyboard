#include "framekeyboard/shortcut_editor.hpp"

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
bool fits_bar_name(const std::string& text) {
    std::size_t characters = 0;
    return count_text(text, 0, characters) && characters <= limits::bar_name_characters;
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
    for (std::size_t i = 0; showing && i < model_.bars.size(); ++i) {
        if (model_.bars[i].id == *showing) {
            bar_ = i;
        }
    }
    if (!bar_ && !model_.bars.empty()) {
        bar_ = 0;
    }
    select_button(selected_bar() && !selected_bar()->buttons.empty() ? std::optional<std::size_t>(0)
                                                                     : std::nullopt);
}
const ShortcutBar* ShortcutEditor::selected_bar() const {
    return bar_ && *bar_ < model_.bars.size() ? &model_.bars[*bar_] : nullptr;
}
const ShortcutButton* ShortcutEditor::selected_button() const {
    const auto* bar = selected_bar();
    return bar && button_ && *button_ < bar->buttons.size() ? &bar->buttons[*button_] : nullptr;
}
void ShortcutEditor::select_bar(std::size_t index) {
    if (index >= model_.bars.size()) {
        return;
    }
    bar_ = index;
    button_ = model_.bars[index].buttons.empty() ? std::nullopt : std::optional<std::size_t>(0);
}
void ShortcutEditor::select_button(std::optional<std::size_t> index) {
    const auto* bar = selected_bar();
    button_ = bar && index && *index < bar->buttons.size() ? index : std::nullopt;
}
void ShortcutEditor::add_bar(std::string name) {
    if (!can_add_bar()) {
        return;
    }
    // b1, b2, …: the first such ID no bar has.
    std::string id;
    for (std::size_t n = 1; id.empty() || model_.bar(id); ++n) {
        id = "b" + std::to_string(n);
    }
    model_.bars.push_back({std::move(id), fits_bar_name(name) ? name : std::string(), {}});
    bar_ = model_.bars.size() - 1;
    button_.reset();
}
void ShortcutEditor::rename_bar(std::string name) {
    if (!bar_) {
        return;
    }
    std::size_t characters = 0;
    if (!count_text(name, 0, characters)) {
        return;
    }
    model_.bars[*bar_].name = first_characters(name, limits::bar_name_characters);
}
void ShortcutEditor::delete_bar() {
    if (!bar_) {
        return;
    }
    const auto id = model_.bars[*bar_].id;
    model_.bars.erase(model_.bars.begin() + static_cast<std::ptrdiff_t>(*bar_));
    std::erase_if(model_.apps, [&](const AppTie& tie) { return tie.bar == id; });
    if (model_.free_bar == id) {
        model_.free_bar.clear();
    }
    if (model_.bars.empty()) {
        bar_.reset();
        button_.reset();
    } else {
        select_bar(std::min(*bar_, model_.bars.size() - 1));
    }
}
void ShortcutEditor::move_bar(std::size_t from, std::size_t to) {
    auto& bars = model_.bars;
    if (from >= bars.size() || to >= bars.size() || from == to) {
        return;
    }
    const bool selected = bar_ == from;
    auto moved = std::move(bars[from]);
    bars.erase(bars.begin() + static_cast<std::ptrdiff_t>(from));
    bars.insert(bars.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    // The selection follows its bar.
    if (selected) {
        bar_ = to;
    } else if (bar_ && from < *bar_ && to >= *bar_) {
        --*bar_;
    } else if (bar_ && from > *bar_ && to <= *bar_) {
        ++*bar_;
    }
}
bool ShortcutEditor::can_add_button() const {
    const auto* bar = selected_bar();
    return bar && bar->buttons.size() < limits::buttons;
}
void ShortcutEditor::add_button() {
    if (!can_add_button()) {
        return;
    }
    auto& buttons = model_.bars[*bar_].buttons;
    buttons.push_back({}); // An empty label and an empty shortcut.
    button_ = buttons.size() - 1;
}
void ShortcutEditor::delete_button(std::size_t index) {
    const auto* bar = selected_bar();
    if (!bar || index >= bar->buttons.size()) {
        return;
    }
    auto& buttons = model_.bars[*bar_].buttons;
    buttons.erase(buttons.begin() + static_cast<std::ptrdiff_t>(index));
    if (buttons.empty()) {
        button_.reset();
    } else if (button_ && *button_ >= index) {
        // The one after it, or the last; the selection never jumps ahead.
        button_ = *button_ == index ? std::min(index, buttons.size() - 1) : *button_ - 1;
    }
}
void ShortcutEditor::move_button(std::size_t from, std::size_t to) {
    const auto* bar = selected_bar();
    if (!bar || from >= bar->buttons.size() || to >= bar->buttons.size() || from == to) {
        return;
    }
    auto& buttons = model_.bars[*bar_].buttons;
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
void ShortcutEditor::set_label(std::string label) {
    if (selected_button() && fits_label(label)) {
        model_.bars[*bar_].buttons[*button_].label = std::move(label);
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
    model_.bars[*bar_].buttons[*button_].action.chords = std::move(chords);
}
std::vector<std::size_t> ShortcutEditor::ties() const {
    std::vector<std::size_t> result;
    if (const auto* bar = selected_bar()) {
        for (std::size_t i = 0; i < model_.apps.size(); ++i) {
            if (model_.apps[i].bar == bar->id) {
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
    for (const auto& bar : loaded.model.bars) {
        buttons += bar.buttons.size();
    }
    return {"shortcuts: " + std::to_string(loaded.model.bars.size()) + " bars, " +
                std::to_string(buttons) + " buttons, " + std::to_string(loaded.model.apps.size()) +
                " assigned apps",
            true};
}
} // namespace framekeyboard
