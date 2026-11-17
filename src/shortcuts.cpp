#include "framekeyboard/shortcuts.hpp"
#include "framekeyboard/input.hpp"
#include "framekeyboard/private_file.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <glib.h>
#include <json-c/json.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>

namespace framekeyboard {
namespace fs = std::filesystem;
namespace limits = shortcut_limits;
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

// Valid UTF-8 without control characters (C0, DEL, C1). Up to `breaks` line
// breaks are allowed, which split a label. `characters` counts code points
// other than those breaks.
bool clean_text(const std::string& text, std::size_t breaks, std::size_t& characters) {
    characters = 0;
    // A length makes an embedded NUL invalid too.
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return false;
    }
    std::size_t seen_breaks = 0;
    for (const char* at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
        const gunichar c = g_utf8_get_char(at);
        if (c == '\n') {
            if (++seen_breaks > breaks) {
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
bool clean_text(const std::string& text) {
    std::size_t characters = 0;
    return clean_text(text, 0, characters);
}
// Where in the file a problem is, by position only.
std::string bar_place(std::size_t bar) {
    return "bar " + std::to_string(bar + 1);
}
std::string button_place(std::size_t bar, std::size_t button) {
    return bar_place(bar) + ", button " + std::to_string(button + 1);
}
std::optional<std::string> check_chord(const ShortcutChord& chord) {
    if (chord.key.empty() && chord.modifiers.empty()) {
        return "a key combination has neither a key nor a modifier";
    }
    std::set<std::string> seen;
    for (const auto& modifier : chord.modifiers) {
        if (!shortcut_modifier(modifier)) {
            return "a key combination names an unknown modifier";
        }
        if (!seen.insert(modifier).second) {
            return "a key combination repeats a modifier";
        }
    }
    if (!chord.key.empty()) {
        if (key_code(chord.key) == 0) {
            return "a key combination names an unknown key";
        }
        if (shortcut_modifier(chord.key)) {
            return "a key combination has a modifier as its key";
        }
    }
    return {};
}
bool bar_id(const std::string& id) {
    return !id.empty() && id.size() <= limits::bar_id_bytes &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
           });
}
// A Steam app ID as Gamescope reports it: a nonzero 32-bit number.
bool steam_id(const std::string& id) {
    if (id.empty() || id.size() > 10 || id.front() == '0' ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    return std::stoull(id) <= UINT32_MAX;
}
std::optional<std::string> check_action(const ShortcutAction& action) {
    switch (action.kind) {
    // An action may be empty: the editor saves a button the user has not
    // finished, and a press on it does nothing (shortcut_action_empty).
    case ShortcutAction::Kind::Keys:
        if (action.chords.size() > limits::chords) {
            return "a shortcut has more than 4 key combinations";
        }
        for (const auto& chord : action.chords) {
            if (auto problem = check_chord(chord)) {
                return problem;
            }
        }
        return {};
    case ShortcutAction::Kind::Macro:
        if (action.steps.size() > limits::steps) {
            return "a macro has more than 32 steps";
        }
        for (std::size_t i = 0; i < action.steps.size(); ++i) {
            const auto& step = action.steps[i];
            const auto where = "step " + std::to_string(i + 1) + ": ";
            std::size_t characters = 0;
            switch (step.kind) {
            case MacroStep::Kind::Keys:
                if (auto problem = check_chord(step.keys)) {
                    return where + *problem;
                }
                break;
            case MacroStep::Kind::Text:
                if (!clean_text(step.text, 0, characters)) {
                    return where + "text is not valid UTF-8 or has control characters";
                }
                if (characters == 0) {
                    return where + "text is empty";
                }
                if (characters > limits::text_characters) {
                    return where + "text is longer than 256 characters";
                }
                break;
            case MacroStep::Kind::Wait:
                if (step.wait_ms < limits::wait_min_ms || step.wait_ms > limits::wait_max_ms) {
                    return where + "a wait is not between 100 and 10000 ms";
                }
                break;
            }
        }
        if (action.repeat.mode == MacroRepeat::Mode::Times &&
            (action.repeat.count < limits::repeat_min || action.repeat.count > limits::repeat_max)) {
            return "a repeat count is not between 2 and 99";
        }
        return {};
    case ShortcutAction::Kind::Run:
        if (!action.script.empty() && !action.command.empty()) {
            return "a run action has both a script and a command";
        }
        if (!action.script.empty()) {
            if (action.script.size() > limits::script_bytes) {
                return "a script name is longer than 64 bytes";
            }
            if (action.script == "." || action.script == ".." ||
                action.script.find('/') != std::string::npos || !clean_text(action.script)) {
                return "a script name is not a plain file name";
            }
        } else if (!action.command.empty()) {
            if (action.command.size() > limits::command_bytes) {
                return "a command is longer than 512 bytes";
            }
            if (!clean_text(action.command)) {
                return "a command is not one line of valid UTF-8";
            }
        }
        return {};
    }
    return "an action has an unknown kind";
}
std::optional<std::string> check_tie(const ShortcutModel& model, const AppTie& tie) {
    if (tie.app.id.empty() || tie.app.id.size() > limits::app_id_bytes || !clean_text(tie.app.id)) {
        return "an app ID is empty, too long or not valid text";
    }
    if (tie.app.kind == AppIdentity::Kind::Steam && !steam_id(tie.app.id)) {
        return "a Steam app ID is not a number";
    }
    if (tie.app.kind == AppIdentity::Kind::Program && tie.app.id.find('/') != std::string::npos) {
        return "a program name has a folder part";
    }
    std::size_t characters = 0;
    if (!clean_text(tie.name, 0, characters) || characters > limits::app_name_characters) {
        return "an app name is too long or not valid text";
    }
    if (!model.bar(tie.bar)) {
        return "an app is assigned to a bar that does not exist";
    }
    return {};
}

// Parsing: the JSON's shape and kinds. Limits are validate_shortcuts' job.
[[noreturn]] void invalid(const std::string& where, const std::string& what) {
    throw std::runtime_error(where.empty() ? what : where + ": " + what);
}
json_object* member(json_object* object, const char* name, json_type type, const std::string& where,
                    bool optional = false) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value)) {
        if (optional) {
            return nullptr;
        }
        invalid(where, std::string("missing ") + name);
    }
    if (!json_object_is_type(value, type)) {
        invalid(where, std::string("wrong type of ") + name);
    }
    return value;
}
std::string string_of(json_object* value) {
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::string string_member(json_object* object, const char* name, const std::string& where,
                          bool optional = false) {
    auto* value = member(object, name, json_type_string, where, optional);
    return value ? string_of(value) : std::string();
}
int int_member(json_object* object, const char* name, const std::string& where) {
    auto* value = member(object, name, json_type_int, where);
    const auto number = json_object_get_int64(value);
    if (number < INT32_MIN || number > INT32_MAX) {
        invalid(where, std::string("out of range: ") + name);
    }
    return static_cast<int>(number);
}
json_object* item(json_object* array, std::size_t index, json_type type, const std::string& where) {
    auto* value = json_object_array_get_idx(array, index);
    if (!json_object_is_type(value, type)) {
        invalid(where, "wrong type");
    }
    return value;
}
ShortcutChord parse_chord(json_object* object, const std::string& where) {
    ShortcutChord chord;
    if (auto* modifiers = member(object, "modifiers", json_type_array, where, true)) {
        // Bounded before the copy; the eight modifiers are all a chord can hold.
        if (json_object_array_length(modifiers) > 8) {
            invalid(where, "too many modifiers");
        }
        for (std::size_t i = 0; i < json_object_array_length(modifiers); ++i) {
            chord.modifiers.push_back(string_of(item(modifiers, i, json_type_string, where)));
        }
    }
    if (auto* key = member(object, "key", json_type_string, where, true)) {
        chord.key = string_of(key);
        if (chord.key.empty()) {
            invalid(where, "an empty key");
        }
    }
    return chord;
}
MacroStep parse_step(json_object* object, const std::string& where) {
    MacroStep step;
    auto* keys = member(object, "keys", json_type_object, where, true);
    auto* text = member(object, "text", json_type_string, where, true);
    json_object* wait = nullptr;
    const bool waits = json_object_object_get_ex(object, "wait_ms", &wait);
    if ((keys != nullptr) + (text != nullptr) + waits != 1) {
        invalid(where, "a step needs exactly one of keys, text and wait_ms");
    }
    if (keys) {
        step.kind = MacroStep::Kind::Keys;
        step.keys = parse_chord(keys, where);
    } else if (text) {
        step.kind = MacroStep::Kind::Text;
        step.text = string_of(text);
    } else {
        step.kind = MacroStep::Kind::Wait;
        step.wait_ms = int_member(object, "wait_ms", where);
    }
    return step;
}
ShortcutAction parse_action(json_object* object, const std::string& where) {
    ShortcutAction action;
    const auto kind = string_member(object, "kind", where);
    if (kind == "keys") {
        action.kind = ShortcutAction::Kind::Keys;
        auto* chords = member(object, "chords", json_type_array, where);
        if (json_object_array_length(chords) > limits::chords) {
            invalid(where, "a shortcut has more than 4 key combinations");
        }
        for (std::size_t i = 0; i < json_object_array_length(chords); ++i) {
            action.chords.push_back(parse_chord(item(chords, i, json_type_object, where), where));
        }
    } else if (kind == "macro") {
        action.kind = ShortcutAction::Kind::Macro;
        auto* steps = member(object, "steps", json_type_array, where);
        if (json_object_array_length(steps) > limits::steps) {
            invalid(where, "a macro has more than 32 steps");
        }
        for (std::size_t i = 0; i < json_object_array_length(steps); ++i) {
            const auto place = where + ", step " + std::to_string(i + 1);
            action.steps.push_back(parse_step(item(steps, i, json_type_object, place), place));
        }
        if (auto* repeat = member(object, "repeat", json_type_object, where, true)) {
            const auto mode = string_member(repeat, "mode", where);
            if (mode == "once") {
                action.repeat.mode = MacroRepeat::Mode::Once;
            } else if (mode == "times") {
                action.repeat.mode = MacroRepeat::Mode::Times;
                action.repeat.count = int_member(repeat, "count", where);
            } else if (mode == "while_held") {
                action.repeat.mode = MacroRepeat::Mode::WhileHeld;
            } else {
                invalid(where, "an unknown repeat mode");
            }
        }
    } else if (kind == "run") {
        action.kind = ShortcutAction::Kind::Run;
        auto* script = member(object, "script", json_type_string, where, true);
        auto* command = member(object, "command", json_type_string, where, true);
        // Neither is a Run button not finished yet; an empty name is no name.
        if (script && command) {
            invalid(where, "a run action has both a script and a command");
        }
        if (script || command) {
            (script ? action.script : action.command) = string_of(script ? script : command);
            if ((script ? action.script : action.command).empty()) {
                invalid(where, "a run action names an empty script or command");
            }
        }
    } else {
        invalid(where, "an unknown action kind");
    }
    return action;
}
AppTie parse_tie(json_object* object, const std::string& where) {
    AppTie tie;
    const auto kind = string_member(object, "kind", where);
    if (kind == "steam") {
        tie.app.kind = AppIdentity::Kind::Steam;
    } else if (kind == "program") {
        tie.app.kind = AppIdentity::Kind::Program;
    } else if (kind == "desktop") {
        tie.app.kind = AppIdentity::Kind::Desktop;
    } else {
        invalid(where, "an unknown app kind");
    }
    tie.app.id = string_member(object, "id", where);
    tie.name = string_member(object, "name", where, true);
    tie.bar = string_member(object, "bar", where);
    return tie;
}
const char* kind_name(AppIdentity::Kind kind) {
    switch (kind) {
    case AppIdentity::Kind::Steam:
        return "steam";
    case AppIdentity::Kind::Desktop:
        return "desktop";
    case AppIdentity::Kind::Program:
        break;
    }
    return "program";
}
json_object* new_string(const std::string& text) {
    return json_object_new_string_len(text.data(), static_cast<int>(text.size()));
}
json_object* chord_json(const ShortcutChord& chord) {
    auto* object = json_object_new_object();
    if (!chord.modifiers.empty()) {
        auto* modifiers = json_object_new_array();
        for (const auto& modifier : chord.modifiers) {
            json_object_array_add(modifiers, new_string(modifier));
        }
        json_object_object_add(object, "modifiers", modifiers);
    }
    if (!chord.key.empty()) {
        json_object_object_add(object, "key", new_string(chord.key));
    }
    return object;
}
json_object* action_json(const ShortcutAction& action) {
    auto* object = json_object_new_object();
    switch (action.kind) {
    case ShortcutAction::Kind::Keys: {
        json_object_object_add(object, "kind", json_object_new_string("keys"));
        auto* chords = json_object_new_array();
        for (const auto& chord : action.chords) {
            json_object_array_add(chords, chord_json(chord));
        }
        json_object_object_add(object, "chords", chords);
        break;
    }
    case ShortcutAction::Kind::Macro: {
        json_object_object_add(object, "kind", json_object_new_string("macro"));
        auto* steps = json_object_new_array();
        for (const auto& step : action.steps) {
            auto* entry = json_object_new_object();
            switch (step.kind) {
            case MacroStep::Kind::Keys:
                json_object_object_add(entry, "keys", chord_json(step.keys));
                break;
            case MacroStep::Kind::Text:
                json_object_object_add(entry, "text", new_string(step.text));
                break;
            case MacroStep::Kind::Wait:
                json_object_object_add(entry, "wait_ms", json_object_new_int(step.wait_ms));
                break;
            }
            json_object_array_add(steps, entry);
        }
        json_object_object_add(object, "steps", steps);
        auto* repeat = json_object_new_object();
        const char* mode = action.repeat.mode == MacroRepeat::Mode::Times       ? "times"
                           : action.repeat.mode == MacroRepeat::Mode::WhileHeld ? "while_held"
                                                                                : "once";
        json_object_object_add(repeat, "mode", json_object_new_string(mode));
        if (action.repeat.mode == MacroRepeat::Mode::Times) {
            json_object_object_add(repeat, "count", json_object_new_int(action.repeat.count));
        }
        json_object_object_add(object, "repeat", repeat);
        break;
    }
    case ShortcutAction::Kind::Run:
        json_object_object_add(object, "kind", json_object_new_string("run"));
        if (!action.script.empty()) {
            json_object_object_add(object, "script", new_string(action.script));
        } else if (!action.command.empty()) {
            json_object_object_add(object, "command", new_string(action.command));
        }
        break;
    }
    return object;
}
} // namespace

const ShortcutBar* ShortcutModel::bar(const std::string& id) const {
    const auto found =
        std::find_if(bars.begin(), bars.end(), [&](const ShortcutBar& bar) { return bar.id == id; });
    return found == bars.end() ? nullptr : &*found;
}
const ShortcutBar* ShortcutModel::bar_for(const AppIdentity& app) const {
    const auto found =
        std::find_if(apps.begin(), apps.end(), [&](const AppTie& tie) { return tie.app == app; });
    return found == apps.end() ? nullptr : bar(found->bar);
}
bool ShortcutModel::free(const std::string& id) const {
    return std::none_of(apps.begin(), apps.end(), [&](const AppTie& tie) { return tie.bar == id; });
}

bool shortcut_action_empty(const ShortcutAction& action) {
    switch (action.kind) {
    case ShortcutAction::Kind::Keys:
        return action.chords.empty();
    case ShortcutAction::Kind::Macro:
        return action.steps.empty();
    case ShortcutAction::Kind::Run:
        return action.script.empty() && action.command.empty();
    }
    return true;
}
bool shortcut_modifier(const std::string& name) {
    const int code = key_code(name);
    return code != 0 && is_modifier(code);
}

std::optional<std::string> validate_shortcuts(const ShortcutModel& model) {
    if (model.bars.size() > limits::bars) {
        return "more than 32 bars";
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < model.bars.size(); ++i) {
        const auto& bar = model.bars[i];
        if (!bar_id(bar.id)) {
            return bar_place(i) + ": the ID is not 1 to 32 letters, digits, - or _";
        }
        if (!ids.insert(bar.id).second) {
            return bar_place(i) + ": the ID is used by another bar";
        }
        std::size_t characters = 0;
        if (!clean_text(bar.name, 0, characters)) {
            return bar_place(i) + ": the name is not valid UTF-8 or has control characters";
        }
        if (characters > limits::bar_name_characters) {
            return bar_place(i) + ": the name is longer than 32 characters";
        }
        if (bar.buttons.size() > limits::buttons) {
            return bar_place(i) + ": more than 64 buttons";
        }
        for (std::size_t j = 0; j < bar.buttons.size(); ++j) {
            const auto& button = bar.buttons[j];
            if (!clean_text(button.label, limits::label_lines - 1, characters)) {
                return button_place(i, j) +
                       ": the label is not valid UTF-8, has control characters or more than two lines";
            }
            if (characters > limits::label_characters) {
                return button_place(i, j) + ": the label is longer than 24 characters";
            }
            if (auto problem = check_action(button.action)) {
                return button_place(i, j) + ": " + *problem;
            }
        }
    }
    if (model.apps.size() > limits::apps) {
        return "more than 128 assigned apps";
    }
    for (std::size_t i = 0; i < model.apps.size(); ++i) {
        const auto where = "app " + std::to_string(i + 1);
        if (auto problem = check_tie(model, model.apps[i])) {
            return where + ": " + *problem;
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (model.apps[k].app == model.apps[i].app) {
                return where + ": the app is assigned twice";
            }
        }
    }
    if (!model.free_bar.empty() && !model.bar(model.free_bar)) {
        return "free_bar names a bar that does not exist";
    }
    return {};
}

std::string serialize_shortcuts(const ShortcutModel& model) {
    Json document(json_object_new_object(), json_object_put);
    json_object_object_add(document.get(), "schema_version", json_object_new_int(1));
    auto* bars = json_object_new_array();
    for (const auto& bar : model.bars) {
        auto* object = json_object_new_object();
        json_object_object_add(object, "id", new_string(bar.id));
        json_object_object_add(object, "name", new_string(bar.name));
        auto* buttons = json_object_new_array();
        for (const auto& button : bar.buttons) {
            auto* entry = json_object_new_object();
            json_object_object_add(entry, "label", new_string(button.label));
            json_object_object_add(entry, "action", action_json(button.action));
            json_object_array_add(buttons, entry);
        }
        json_object_object_add(object, "buttons", buttons);
        json_object_array_add(bars, object);
    }
    json_object_object_add(document.get(), "bars", bars);
    auto* apps = json_object_new_array();
    for (const auto& tie : model.apps) {
        auto* object = json_object_new_object();
        json_object_object_add(object, "kind", json_object_new_string(kind_name(tie.app.kind)));
        json_object_object_add(object, "id", new_string(tie.app.id));
        json_object_object_add(object, "name", new_string(tie.name));
        json_object_object_add(object, "bar", new_string(tie.bar));
        json_object_array_add(apps, object);
    }
    json_object_object_add(document.get(), "apps", apps);
    if (!model.free_bar.empty()) {
        json_object_object_add(document.get(), "free_bar", new_string(model.free_bar));
    }
    return std::string(json_object_to_json_string_ext(
               document.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE)) +
           '\n';
}

ShortcutModel parse_shortcuts(const std::string& json) {
    if (json.size() > limits::file_bytes) {
        invalid("", "the file is larger than 512 KB");
    }
    auto* parser = json_tokener_new_ex(16);
    if (!parser) {
        throw std::bad_alloc();
    }
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json document(json_tokener_parse_ex(parser, json.c_str(), static_cast<int>(json.size() + 1)),
                  json_object_put);
    const auto error = json_tokener_get_error(parser);
    json_tokener_free(parser);
    if (error != json_tokener_success || !document) {
        invalid("", "not valid JSON");
    }
    if (!json_object_is_type(document.get(), json_type_object)) {
        invalid("", "not a JSON object");
    }
    if (int_member(document.get(), "schema_version", "") != 1) {
        invalid("", "unsupported schema_version");
    }
    ShortcutModel model;
    auto* bars = member(document.get(), "bars", json_type_array, "");
    if (json_object_array_length(bars) > limits::bars) {
        invalid("", "more than 32 bars");
    }
    for (std::size_t i = 0; i < json_object_array_length(bars); ++i) {
        const auto where = bar_place(i);
        auto* object = item(bars, i, json_type_object, where);
        ShortcutBar bar;
        bar.id = string_member(object, "id", where);
        bar.name = string_member(object, "name", where);
        auto* buttons = member(object, "buttons", json_type_array, where);
        if (json_object_array_length(buttons) > limits::buttons) {
            invalid(where, "more than 64 buttons");
        }
        for (std::size_t j = 0; j < json_object_array_length(buttons); ++j) {
            const auto place = button_place(i, j);
            auto* entry = item(buttons, j, json_type_object, place);
            ShortcutButton button;
            button.label = string_member(entry, "label", place);
            button.action = parse_action(member(entry, "action", json_type_object, place), place);
            bar.buttons.push_back(std::move(button));
        }
        model.bars.push_back(std::move(bar));
    }
    if (auto* apps = member(document.get(), "apps", json_type_array, "", true)) {
        if (json_object_array_length(apps) > limits::apps) {
            invalid("", "more than 128 assigned apps");
        }
        for (std::size_t i = 0; i < json_object_array_length(apps); ++i) {
            const auto where = "app " + std::to_string(i + 1);
            model.apps.push_back(parse_tie(item(apps, i, json_type_object, where), where));
        }
    }
    if (auto* free = member(document.get(), "free_bar", json_type_string, "", true)) {
        model.free_bar = string_of(free);
        if (model.free_bar.empty()) {
            invalid("", "free_bar is empty");
        }
    }
    if (auto problem = validate_shortcuts(model)) {
        invalid("", *problem);
    }
    return model;
}

fs::path shortcuts_path(const fs::path& config_dir) {
    return config_dir / "shortcuts.json";
}

namespace {
// The file's bytes, one past the cap at most, so a file too large shows;
// none when it isn't there, or isn't a regular file one can read.
enum class FileRead { Missing, Unreadable, Read };
FileRead read_shortcuts_file(const fs::path& path, std::string& data) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return FileRead::Missing;
    }
    if (error || status.type() != fs::file_type::regular) {
        return FileRead::Unreadable;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return FileRead::Unreadable;
    }
    data.assign(limits::file_bytes + 1, '\0');
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(file.gcount()));
    return FileRead::Read;
}
} // namespace
ShortcutsLoad ShortcutStore::load() {
    ShortcutsLoad result;
    std::string data;
    const auto read = read_shortcuts_file(path_, data);
    known_.reset();
    if (read == FileRead::Missing) {
        return result; // Missing: an empty model.
    }
    result.state = ShortcutsLoad::State::Invalid;
    if (read == FileRead::Unreadable) {
        result.reason = "not a readable file";
        return result;
    }
    try {
        result.model = parse_shortcuts(data);
        result.state = ShortcutsLoad::State::Valid;
        known_ = data; // Only a valid file: an invalid one is always kept aside.
    } catch (const std::exception& problem) {
        result.model = {};
        result.reason = problem.what();
    }
    return result;
}
std::optional<std::string> ShortcutStore::save(const ShortcutModel& model) {
    if (auto problem = validate_shortcuts(model)) {
        return problem;
    }
    const auto text = serialize_shortcuts(model);
    // What is there now is the user's work: an invalid file, or a valid one
    // edited by hand since this store read or wrote it, is kept beside.
    std::string data;
    const auto read = read_shortcuts_file(path_, data);
    if (read != FileRead::Missing && (read != FileRead::Read || data != known_)) {
        bool valid = false;
        if (read == FileRead::Read) {
            try {
                parse_shortcuts(data);
                valid = true;
            } catch (const std::exception&) {
            }
        }
        auto aside = path_;
        aside += valid ? ".old" : ".bad";
        std::error_code error;
        fs::rename(path_, aside, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return "the file on disk could not be moved aside";
        }
    }
    // Written on the preference worker, so synced as config.json is.
    if (!write_private_file(path_, text, true, true)) {
        return "the file could not be written";
    }
    known_ = text;
    return {};
}

void ShortcutBarChoice::configure(const ShortcutModel& model, bool enabled, bool on) {
    enabled_ = enabled;
    on_ = on;
    empty_ = false;
    resolve(model);
}
void ShortcutBarChoice::focus(const ShortcutModel& model, std::optional<AppIdentity> app) {
    app_ = std::move(app);
    // A picked bar and the empty bar last until the focus moves on.
    chosen_.reset();
    empty_ = false;
    resolve(model);
}
std::optional<std::string> ShortcutBarChoice::due(const ShortcutModel& model) const {
    if (chosen_ && model.bar(*chosen_)) {
        return chosen_;
    }
    // Rule 1: the app has a bar. The dashboard and unknown apps have none.
    if (app_) {
        if (const auto* bar = model.bar_for(*app_)) {
            return bar->id;
        }
    }
    // Rule 2: the free bar chosen last, while it exists and is still free.
    if (!model.free_bar.empty() && model.bar(model.free_bar) && model.free(model.free_bar)) {
        return model.free_bar;
    }
    return {}; // Rule 3: nothing.
}
void ShortcutBarChoice::resolve(const ShortcutModel& model) {
    // Turned off by hand, the switch overrides every rule. Rule 3 leaves it
    // on, so a later app with a bar brings the bar back.
    shown_ = enabled_ && on_ ? due(model) : std::nullopt;
    if (!enabled_ || !on_) {
        empty_ = false;
    }
}
ShortcutBarChoice::Changes ShortcutBarChoice::toolbar(const ShortcutModel& model) {
    Changes changes;
    if (!enabled_) {
        return changes;
    }
    if (button_on()) {
        // A bar, or the empty one, shows: it hides everywhere until pressed again.
        shown_.reset();
        empty_ = false;
        changes.shortcuts_switch = on_;
        on_ = false;
        return changes;
    }
    changes.shortcuts_switch = !on_;
    on_ = true;
    shown_ = due(model);
    // No bar is due: the empty bar, so the picker can be reached.
    empty_ = !shown_;
    return changes;
}
ShortcutBarChoice::Changes ShortcutBarChoice::pick(ShortcutModel& model, const std::string& bar) {
    Changes changes;
    // The picker hangs off a bar that shows, so the switch is on.
    if (!enabled_ || !on_ || !model.bar(bar)) {
        return changes;
    }
    chosen_ = bar;
    shown_ = bar;
    empty_ = false;
    if (model.free(bar) && model.free_bar != bar) {
        model.free_bar = bar;
        changes.free_bar = true;
    }
    return changes;
}
ShortcutBarChoice::Changes ShortcutBarChoice::model_changed(ShortcutModel& model) {
    Changes changes;
    if (!model.free_bar.empty() && !model.bar(model.free_bar)) {
        model.free_bar.clear();
        changes.free_bar = true;
    }
    if (chosen_ && !model.bar(*chosen_)) {
        chosen_.reset();
    }
    // A bar that shows stays, even when ties changed (assigning doesn't switch
    // bars). A deleted one goes, and nothing showing may now have a bar due.
    if ((shown_ && !model.bar(*shown_)) || (!shown_ && !empty_)) {
        resolve(model);
    }
    return changes;
}
} // namespace framekeyboard
