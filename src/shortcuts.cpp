#include "framekeyboard/shortcuts.hpp"
#include "framekeyboard/input.hpp"
#include "framekeyboard/private_file.hpp"
#include "framekeyboard/shortcut_launch.hpp"

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
std::string bar_place(std::size_t profile) {
    return "profile " + std::to_string(profile + 1);
}
std::string button_place(std::size_t profile, std::size_t button) {
    return bar_place(profile) + ", button " + std::to_string(button + 1);
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
        // Num Lock and Caps Lock are the keyboard's own switches, never sent
        // to an app: one recorded would flip the app's lock (R330).
        if (chord.key == "NumLock" || chord.key == "CapsLock") {
            return "a key combination has a lock as its key";
        }
    }
    return {};
}
bool profile_id(const std::string& id) {
    return !id.empty() && id.size() <= limits::profile_id_bytes &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
           });
}
// A Steam app ID as Gamescope reports it: a nonzero 32-bit number.
// The one ID of a stream without a Steam app ID (stream_app.hpp's pc_stream_id).
constexpr const char* stream_id = "pc";
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
            switch (step.kind) {
            case MacroStep::Kind::Keys:
                if (step.keys.empty()) {
                    return where + "a keys step has no key combinations";
                }
                if (step.keys.size() > limits::step_chords) {
                    return where + "a keys step has more than 32 key combinations";
                }
                for (const auto& chord : step.keys) {
                    if (auto problem = check_chord(chord)) {
                        return where + *problem;
                    }
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
            if (!script_name_valid(action.script)) {
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
    // The same rule the focus applies before it reports an identity
    // (app_identity_valid), with the reason for a hand-edited file.
    if (tie.app.id.empty() || tie.app.id.size() > limits::app_id_bytes || !clean_text(tie.app.id)) {
        return "an app ID is empty, too long or not valid text";
    }
    if (tie.app.kind == AppIdentity::Kind::Steam && !steam_id(tie.app.id)) {
        return "a Steam app ID is not a number";
    }
    if (tie.app.kind == AppIdentity::Kind::Program && tie.app.id.find('/') != std::string::npos) {
        return "a program name has a folder part";
    }
    if (tie.app.kind == AppIdentity::Kind::Stream && tie.app.id != stream_id) {
        return "a stream's ID is not \"pc\"";
    }
    std::size_t characters = 0;
    if (!clean_text(tie.name, 0, characters) || characters > limits::app_name_characters) {
        return "an app name is too long or not valid text";
    }
    if (!model.profile(tie.profile)) {
        return "an app is assigned to a profile that does not exist";
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
    // A macro presses keys only: a text step, from an earlier draft of the
    // format, makes the file invalid rather than being skipped.
    if (json_object* text = nullptr; json_object_object_get_ex(object, "text", &text)) {
        invalid(where, "a text step; macros press keys only");
    }
    auto* keys = member(object, "keys", json_type_array, where, true);
    json_object* wait = nullptr;
    const bool waits = json_object_object_get_ex(object, "wait_ms", &wait);
    if ((keys != nullptr) + waits != 1) {
        invalid(where, "a step needs exactly one of keys and wait_ms");
    }
    if (keys) {
        step.kind = MacroStep::Kind::Keys;
        // Bounded before the copy; validation reports the limit itself.
        if (json_object_array_length(keys) > limits::step_chords) {
            invalid(where, "a keys step has more than 32 key combinations");
        }
        for (std::size_t i = 0; i < json_object_array_length(keys); ++i) {
            step.keys.push_back(parse_chord(item(keys, i, json_type_object, where), where));
        }
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
        action.command_chosen = !action.command.empty();
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
    } else if (kind == "stream") {
        tie.app.kind = AppIdentity::Kind::Stream;
    } else {
        invalid(where, "an unknown app kind");
    }
    tie.app.id = string_member(object, "id", where);
    tie.name = string_member(object, "name", where, true);
    tie.profile = string_member(object, "profile", where);
    return tie;
}
const char* kind_name(AppIdentity::Kind kind) {
    switch (kind) {
    case AppIdentity::Kind::Steam:
        return "steam";
    case AppIdentity::Kind::Desktop:
        return "desktop";
    case AppIdentity::Kind::Stream:
        return "stream";
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
            case MacroStep::Kind::Keys: {
                auto* keys = json_object_new_array();
                for (const auto& chord : step.keys) {
                    json_object_array_add(keys, chord_json(chord));
                }
                json_object_object_add(entry, "keys", keys);
                break;
            }
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

bool script_name_valid(std::string_view name) {
    return !name.empty() && name.size() <= limits::script_bytes && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && clean_text(std::string(name));
}
bool app_identity_valid(const AppIdentity& app) {
    return !app.id.empty() && app.id.size() <= limits::app_id_bytes && clean_text(app.id) &&
           (app.kind != AppIdentity::Kind::Steam || steam_id(app.id)) &&
           (app.kind != AppIdentity::Kind::Program || app.id.find('/') == std::string::npos) &&
           (app.kind != AppIdentity::Kind::Stream || app.id == stream_id);
}
bool ShortcutModel::remember_pick(const std::string& id) {
    if (free_profile == id || !profile(id)) {
        return false;
    }
    std::erase(earlier_picks, id);
    if (!free_profile.empty()) {
        earlier_picks.insert(earlier_picks.begin(), free_profile);
    }
    free_profile = id;
    if (earlier_picks.size() >= limits::profiles) {
        earlier_picks.resize(limits::profiles - 1);
    }
    return true;
}
bool ShortcutModel::forget_missing_picks() {
    const auto before = earlier_picks.size();
    std::erase_if(earlier_picks, [&](const std::string& id) { return !profile(id); });
    bool changed = earlier_picks.size() != before;
    if (!free_profile.empty() && !profile(free_profile)) {
        // The next pick takes the last one's place.
        if (earlier_picks.empty()) {
            free_profile.clear();
        } else {
            free_profile = earlier_picks.front();
            earlier_picks.erase(earlier_picks.begin());
        }
        changed = true;
    }
    return changed;
}
const ShortcutProfile* ShortcutModel::profile(const std::string& id) const {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [&](const ShortcutProfile& profile) { return profile.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}
const ShortcutProfile* ShortcutModel::profile_for(const AppIdentity& app) const {
    const auto found =
        std::find_if(apps.begin(), apps.end(), [&](const AppTie& tie) { return tie.app == app; });
    return found == apps.end() ? nullptr : profile(found->profile);
}
bool ShortcutModel::free(const std::string& id) const {
    return std::none_of(apps.begin(), apps.end(), [&](const AppTie& tie) { return tie.profile == id; });
}
const AppTie* ShortcutModel::tie(const AppIdentity& app) const {
    const auto found =
        std::find_if(apps.begin(), apps.end(), [&](const AppTie& tie) { return tie.app == app; });
    return found == apps.end() ? nullptr : &*found;
}
bool ShortcutModel::can_assign(const AppIdentity& app) const {
    return tie(app) || apps.size() < limits::apps;
}
bool ShortcutModel::assign(const AppIdentity& app, const std::string& name, const std::string& id) {
    if (!profile(id) || !can_assign(app)) {
        return false;
    }
    // An app has at most one profile: its tie moves, keeping its place.
    for (auto& tie : apps) {
        if (tie.app == app) {
            tie.profile = id;
            tie.name = name;
            return true;
        }
    }
    apps.push_back({app, name, id});
    return true;
}
bool ShortcutModel::unassign(const AppIdentity& app) {
    return std::erase_if(apps, [&](const AppTie& tie) { return tie.app == app; }) > 0;
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
    if (model.profiles.size() > limits::profiles) {
        return "more than 32 profiles";
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < model.profiles.size(); ++i) {
        const auto& profile = model.profiles[i];
        if (!profile_id(profile.id)) {
            return bar_place(i) + ": the ID is not 1 to 32 letters, digits, - or _";
        }
        if (!ids.insert(profile.id).second) {
            return bar_place(i) + ": the ID is used by another profile";
        }
        std::size_t characters = 0;
        if (!clean_text(profile.name, 0, characters)) {
            return bar_place(i) + ": the name is not valid UTF-8 or has control characters";
        }
        if (characters > limits::profile_name_characters) {
            return bar_place(i) + ": the name is longer than 32 characters";
        }
        if (profile.buttons.size() > limits::buttons) {
            return bar_place(i) + ": more than 64 buttons";
        }
        for (std::size_t j = 0; j < profile.buttons.size(); ++j) {
            const auto& button = profile.buttons[j];
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
    if (!model.free_profile.empty() && !model.profile(model.free_profile)) {
        return "free_profile names a profile that does not exist";
    }
    if (!model.earlier_picks.empty() && model.free_profile.empty()) {
        return "earlier_picks without free_profile";
    }
    if (model.earlier_picks.size() >= limits::profiles) {
        return "more than 31 earlier picks";
    }
    for (std::size_t i = 0; i < model.earlier_picks.size(); ++i) {
        const auto& id = model.earlier_picks[i];
        if (!model.profile(id)) {
            return "earlier_picks names a profile that does not exist";
        }
        if (id == model.free_profile ||
            std::find(model.earlier_picks.begin(),
                      model.earlier_picks.begin() + static_cast<std::ptrdiff_t>(i),
                      id) != model.earlier_picks.begin() + static_cast<std::ptrdiff_t>(i)) {
            return "a profile is in the picks twice";
        }
    }
    return {};
}

std::string serialize_shortcuts(const ShortcutModel& model) {
    Json document(json_object_new_object(), json_object_put);
    json_object_object_add(document.get(), "schema_version", json_object_new_int(1));
    auto* profiles = json_object_new_array();
    for (const auto& profile : model.profiles) {
        auto* object = json_object_new_object();
        json_object_object_add(object, "id", new_string(profile.id));
        json_object_object_add(object, "name", new_string(profile.name));
        auto* buttons = json_object_new_array();
        for (const auto& button : profile.buttons) {
            auto* entry = json_object_new_object();
            json_object_object_add(entry, "label", new_string(button.label));
            json_object_object_add(entry, "action", action_json(button.action));
            json_object_array_add(buttons, entry);
        }
        json_object_object_add(object, "buttons", buttons);
        json_object_array_add(profiles, object);
    }
    json_object_object_add(document.get(), "profiles", profiles);
    auto* apps = json_object_new_array();
    for (const auto& tie : model.apps) {
        auto* object = json_object_new_object();
        json_object_object_add(object, "kind", json_object_new_string(kind_name(tie.app.kind)));
        json_object_object_add(object, "id", new_string(tie.app.id));
        json_object_object_add(object, "name", new_string(tie.name));
        json_object_object_add(object, "profile", new_string(tie.profile));
        json_object_array_add(apps, object);
    }
    json_object_object_add(document.get(), "apps", apps);
    if (!model.free_profile.empty()) {
        json_object_object_add(document.get(), "free_profile", new_string(model.free_profile));
    }
    if (!model.earlier_picks.empty()) {
        auto* earlier = json_object_new_array();
        for (const auto& id : model.earlier_picks) {
            json_object_array_add(earlier, new_string(id));
        }
        json_object_object_add(document.get(), "earlier_picks", earlier);
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
    auto* profiles = member(document.get(), "profiles", json_type_array, "");
    if (json_object_array_length(profiles) > limits::profiles) {
        invalid("", "more than 32 profiles");
    }
    for (std::size_t i = 0; i < json_object_array_length(profiles); ++i) {
        const auto where = bar_place(i);
        auto* object = item(profiles, i, json_type_object, where);
        ShortcutProfile profile;
        profile.id = string_member(object, "id", where);
        profile.name = string_member(object, "name", where);
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
            profile.buttons.push_back(std::move(button));
        }
        model.profiles.push_back(std::move(profile));
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
    if (auto* free = member(document.get(), "free_profile", json_type_string, "", true)) {
        model.free_profile = string_of(free);
        if (model.free_profile.empty()) {
            invalid("", "free_profile is empty");
        }
    }
    // A file from before R350 has none: its free_profile is a history of one.
    if (auto* earlier = member(document.get(), "earlier_picks", json_type_array, "", true)) {
        if (json_object_array_length(earlier) >= limits::profiles) {
            invalid("", "more than 31 earlier picks");
        }
        for (std::size_t i = 0; i < json_object_array_length(earlier); ++i) {
            const auto where = "earlier pick " + std::to_string(i + 1);
            model.earlier_picks.push_back(string_of(item(earlier, i, json_type_string, where)));
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
    bool renamed = false;
    if (!write_private_file(path_, text, true, true, &renamed)) {
        if (!renamed) {
            return "the file could not be written";
        }
        // Only the folder's sync failed: the new file is in place, so it is
        // what the next save compares, not a hand edit to keep (R326).
        known_ = text;
        return std::string(unsynced);
    }
    known_ = text;
    return {};
}

void ShortcutProfileChoice::configure(const ShortcutModel& model, bool enabled, bool on) {
    enabled_ = enabled;
    on_ = on;
    empty_ = false;
    resolve(model);
}
void ShortcutProfileChoice::focus(const ShortcutModel& model, std::optional<AppIdentity> app) {
    app_ = std::move(app);
    // A picked profile and the empty bar last until the focus moves on.
    chosen_.reset();
    empty_ = false;
    resolve(model);
}
std::optional<std::string> ShortcutProfileChoice::due(const ShortcutModel& model) const {
    if (chosen_ && model.profile(*chosen_)) {
        return chosen_;
    }
    // Rule 1: the app has a profile. The dashboard and unknown apps have none.
    if (app_) {
        if (const auto* profile = model.profile_for(*app_)) {
            return profile->id;
        }
    }
    // Rule 2 (R350): the profile picked last for apps without one, while no
    // app is assigned to it; else the newest earlier pick no app is assigned
    // to; else, every pick being assigned, the last pick all the same.
    if (!model.free_profile.empty() && model.profile(model.free_profile)) {
        if (model.free(model.free_profile)) {
            return model.free_profile;
        }
        for (const auto& id : model.earlier_picks) {
            if (model.profile(id) && model.free(id)) {
                return id;
            }
        }
        return model.free_profile;
    }
    return {}; // Rule 3: nothing was ever picked.
}
void ShortcutProfileChoice::resolve(const ShortcutModel& model) {
    // Turned off by hand, the switch overrides every rule. Rule 3 leaves it
    // on, so a later app with a profile brings the bar back.
    shown_ = enabled_ && on_ ? due(model) : std::nullopt;
    if (!enabled_ || !on_) {
        empty_ = false;
    }
}
ShortcutProfileChoice::Changes ShortcutProfileChoice::toolbar(const ShortcutModel& model) {
    Changes changes;
    if (!enabled_) {
        return changes;
    }
    if (button_on()) {
        // A profile, or the empty bar, shows: it hides everywhere until pressed again.
        shown_.reset();
        empty_ = false;
        changes.shortcuts_switch = on_;
        on_ = false;
        return changes;
    }
    changes.shortcuts_switch = !on_;
    on_ = true;
    shown_ = due(model);
    // No profile is due: the empty bar, so the picker can be reached.
    empty_ = !shown_;
    return changes;
}
ShortcutProfileChoice::Changes ShortcutProfileChoice::pick(ShortcutModel& model,
                                                           const std::string& profile) {
    Changes changes;
    // The picker hangs off a profile that shows, so the switch is on.
    if (!enabled_ || !on_ || !model.profile(profile)) {
        return changes;
    }
    chosen_ = profile;
    shown_ = profile;
    empty_ = false;
    // Only a pick for an app without a profile of its own is remembered.
    if (!(app_ && model.profile_for(*app_))) {
        changes.free_profile = model.remember_pick(profile);
    }
    return changes;
}
ShortcutProfileChoice::Changes ShortcutProfileChoice::model_changed(ShortcutModel& model) {
    Changes changes;
    changes.free_profile = model.forget_missing_picks();
    if (chosen_ && !model.profile(*chosen_)) {
        chosen_.reset();
    }
    // A profile that shows stays, even when ties changed (assigning doesn't switch
    // profiles). A deleted one goes, and nothing showing may now have a profile due.
    if ((shown_ && !model.profile(*shown_)) || (!shown_ && !empty_)) {
        resolve(model);
    }
    return changes;
}
} // namespace framekeyboard
