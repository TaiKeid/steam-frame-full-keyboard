// What a shell asks of App (docs/integration.md). The VR loop parses the
// protocol and owns presentation, placement and pointers; App keeps typing
// state, settings, themes and dictation, as it does for its own panel.
#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"

#include <algorithm>
#include <glib.h>

namespace framekeyboard {
namespace {
const std::set<std::string> purposes{"text",  "password", "pin",    "number",
                                     "email", "url",      "search", "terminal"};
const std::set<std::string> enter_actions{"enter", "go", "search", "send", "next", "done"};
const std::set<std::string> capitalizations{"none", "sentences", "words"};

json_object* string_array(const std::vector<std::string>& values) {
    auto* array = json_object_new_array();
    for (const auto& value : values) {
        json_object_array_add(array, json_object_new_string(value.c_str()));
    }
    return array;
}
// The Dictation card's Test labels as fixed codes.
std::string test_code(const std::string& label) {
    static const std::map<std::string, std::string> codes{
        {"Reachable", "reachable"},    {"Unreachable", "unreachable"},
        {"No answer", "no_answer"},    {"Server error", "server_error"},
        {"Bad reply", "bad_reply"},    {"Helper stopped", "helper_stopped"},
        {"No server set", "no_server"}};
    const auto found = codes.find(label);
    return found == codes.end() ? "" : found->second;
}
} // namespace

void App::host_event(const std::string& name, JsonPtr fields, bool telemetry) {
    if (!hosted()) {
        return;
    }
    host_.events.push_back({name, std::move(fields), telemetry});
}

std::pair<std::string, std::string> App::check_hints(const HostHints& hints) const {
    if (!purposes.contains(hints.purpose)) {
        return {"bad_request", "hints.purpose"};
    }
    if (!enter_actions.contains(hints.enter)) {
        return {"bad_request", "hints.enter"};
    }
    if (!capitalizations.contains(hints.autocapitalize)) {
        return {"bad_request", "hints.autocapitalize"};
    }
    if (!hints.language.empty() && !profiles_.languages.contains(hints.language)) {
        return {"bad_request", "hints.language"};
    }
    return {};
}

void App::set_context(std::optional<std::string> context, const HostHints& hints) {
    if (context != host_.context) {
        // Nothing addressed to the old field may reach the new one: Type as
        // I speak phrases, swipe results and corrections, held keys and
        // modifiers, open composition (discarded, not committed), and queued
        // or retried output.
        if (direct_active()) {
            stop_direct({});
        }
        // A Send authorized for the old field never continues into the new
        // one: what was not delivered stays in the editor for a fresh Send.
        if (sending_) {
            stop_send(sending_->in_flight);
        }
        send_wait_since_.reset();
        swipe_track_.reset();
        discard_swipe();
        swipe_space_ = false;
        cancel(true);
        output_.cancel();
        // The old context's hints end with it.
        end_language_hint();
        if (host_.num_before) {
            keyboard_.set_num(*host_.num_before);
            host_.num_before.reset();
        }
        host_.numpad_shown = false;
    }
    host_.context = std::move(context);
    host_.hints = host_.context ? hints : HostHints{};
    if (host_.context) {
        status_.clear(); // a new field starts without the last one's notices
    }
    if (options_.shell_router) {
        options_.shell_router->set_context(host_.context.value_or(""));
    }
    if (host_.context) {
        if (host_.hints.purpose == "number" || host_.hints.purpose == "pin") {
            host_.numpad_shown = true;
            if (!host_.num_before) {
                host_.num_before = keyboard_.num();
            }
            keyboard_.set_num(true);
        }
        if (!host_.hints.language.empty() && host_.hints.language != settings_.active.language) {
            Selection selection = settings_.active;
            selection.language = host_.hints.language;
            selection.layout = compatible_layout(profiles_, selection);
            try {
                const auto before = settings_.active;
                activate(selection); // temporary: never saved, restored when the context ends
                // A later hint for the same context keeps the selection
                // from before the first one.
                if (!host_.before_language) {
                    host_.before_language = before;
                }
                host_.hinted_language = host_.hints.language;
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        } else if (host_.hints.language.empty() && host_.before_language) {
            end_language_hint(); // the same context, now without a language
        }
    }
    if (dictation_open_) {
        refresh_dictation_layout();
    }
    refresh_typing();
    auto fields = json_ptr(json_object_new_object());
    json_object_object_add(fields.get(), "owner", json_object_new_string(sink_on() ? "sink" : "normal"));
    json_object_object_add(fields.get(), "context",
                           host_.context ? json_object_new_string(host_.context->c_str()) : nullptr);
    host_event("target", std::move(fields));
    dirty = true;
}

void App::end_language_hint() {
    if (host_.before_language && settings_.active.language == host_.hinted_language) {
        // The user did not pick another since: back to the layout and
        // language before the hint, keeping any theme chosen meanwhile.
        Selection selection = settings_.active;
        selection.layout = host_.before_language->layout;
        selection.language = host_.before_language->language;
        try {
            activate(selection);
        } catch (const std::exception& error) {
            status_ = error.what();
        }
    }
    host_.before_language.reset();
    host_.hinted_language.clear();
}

void App::set_sink(bool on, const std::string& client) {
    if (!options_.shell_router || on == sink_on()) {
        return;
    }
    // Dictation's output was authorized for the old destination: Type as I
    // speak ends with its late phrases, and a Send stops with its draft kept.
    if (direct_active()) {
        stop_direct({});
    }
    if (sending_) {
        stop_send(sending_->in_flight);
    }
    send_wait_since_.reset();
    // Held keys are released and queued output dropped through the old
    // destination before the new one takes over.
    cancel(false);
    output_.cancel();
    options_.shell_router->set_on(on);
    host_.sink_client = on ? client : std::string{};
    refresh_typing();
    auto fields = json_ptr(json_object_new_object());
    json_object_object_add(fields.get(), "owner", json_object_new_string(on ? "sink" : "normal"));
    json_object_object_add(fields.get(), "context",
                           host_.context ? json_object_new_string(host_.context->c_str()) : nullptr);
    host_event("target", std::move(fields));
    dirty = true;
}

std::string App::view_name() const {
    return settings_open_    ? "settings"
           : dictation_open_ ? "dictation"
           : emoji_open_     ? "emoji"
                             : "keyboard";
}

std::string App::show_view(const std::string& view) {
    if (view != "keyboard" && view != "emoji" && view != "dictation" && view != "settings") {
        return "bad_request";
    }
    if (view == view_name()) {
        return {};
    }
    // A protocol view change never commits unfinished input.
    if (composing_text()) {
        return "composing";
    }
    if (view == "emoji" && (!emoji_available() || hint_private())) {
        return "unavailable";
    }
    if (view == "dictation" && (!dictation_available() || hint_private() || direct_dictation())) {
        return "unavailable";
    }
    if (settings_open_) {
        back(); // unapplied Settings edits are dropped, as Back does
    }
    if (view == "keyboard") {
        close_emoji();
        close_dictation();
    } else if (view == "emoji") {
        close_dictation();
        open_emoji();
    } else if (view == "dictation") {
        close_emoji();
        open_dictation();
    } else {
        show_settings();
    }
    dirty = true;
    return view == view_name() ? "" : "unavailable";
}

void App::set_chrome(const HostChrome& chrome) {
    host_.chrome = chrome;
    dirty = true;
}

void App::set_locks(bool move, bool resize) {
    host_.lock_move = move;
    host_.lock_resize = resize;
    dirty = true;
}

void App::set_forwarded(bool on) {
    if (on != host_.forwarded) {
        cancel(false); // every pointer switch releases keys
    }
    host_.forwarded = on;
    dirty = true;
}

std::string App::hint_case(std::string text) const {
    if (!host_.context || text.empty() || !g_utf8_validate(text.c_str(), -1, nullptr)) {
        return text;
    }
    std::string result;
    bool start = true;
    for (const char* at = text.c_str(); *at; at = g_utf8_next_char(at)) {
        gunichar c = g_utf8_get_char(at);
        if (host_.hints.autocapitalize == "none" && at == text.c_str()) {
            c = g_unichar_tolower(c);
        } else if (host_.hints.autocapitalize == "words" && start) {
            c = g_unichar_toupper(c);
        }
        start = g_unichar_isspace(c);
        char buffer[8]{};
        result.append(buffer, static_cast<std::size_t>(g_unichar_to_utf8(c, buffer)));
    }
    return result;
}

JsonPtr App::host_settings() const {
    auto object = json_ptr(json_object_new_object());
    auto flag = [&](const char* key, bool value) {
        json_object_object_add(object.get(), key, json_object_new_boolean(value));
    };
    flag("swipe_typing", settings_.swipe_typing);
    flag("num_lock", keyboard_.num());
    flag("hide_numpad", settings_.hide_numpad);
    flag("use_drag_to_move", settings_.use_drag_to_move);
    flag("auto_leveling", settings_.auto_leveling);
    flag("pinned", settings_.pinned);
    flag("emoji_close_on_select", settings_.emoji_close_on_select);
    json_object_object_add(object.get(), "dictation",
                           json_object_new_string(settings_.dictation.c_str()));
    json_object_object_add(object.get(), "dictation_server",
                           json_object_new_string(settings_.dictation_server.c_str()));
    json_object_object_add(object.get(), "dictation_send_gap",
                           json_object_new_int(settings_.dictation_send_gap));
    flag("dictation_send_paste", settings_.dictation_send_paste);
    flag("dictation_direct", settings_.dictation_direct);
    flag("dictation_fallback", settings_.dictation_fallback);
    return object;
}

std::pair<std::string, std::string> App::host_settings_set(json_object* values, std::int64_t request) {
    if (!values || !json_object_is_type(values, json_type_object)) {
        return {"bad_request", "values"};
    }
    static const std::set<std::string> booleans{"swipe_typing",          "num_lock",
                                                "hide_numpad",           "use_drag_to_move",
                                                "auto_leveling",         "pinned",
                                                "emoji_close_on_select", "dictation_send_paste",
                                                "dictation_direct",      "dictation_fallback"};
    // Every value is checked before any is applied: one invalid value
    // rejects the whole request.
    json_object_object_foreach(values, check_key, check_value) {
        const std::string key = check_key;
        if (booleans.contains(key)) {
            if (!json_object_is_type(check_value, json_type_boolean)) {
                return {"invalid_setting", key};
            }
        } else if (key == "dictation") {
            const std::string mode = json_object_is_type(check_value, json_type_string)
                                         ? json_object_get_string(check_value)
                                         : "";
            if (mode != "off" && mode != "server" && mode != "device") {
                return {"invalid_setting", key};
            }
        } else if (key == "dictation_server") {
            if (!json_object_is_type(check_value, json_type_string) ||
                (json_object_get_string_len(check_value) > 0 &&
                 !valid_dictation_server(json_object_get_string(check_value)))) {
                return {"invalid_setting", key};
            }
        } else if (key == "dictation_send_gap") {
            const int gap =
                json_object_is_type(check_value, json_type_int) ? json_object_get_int(check_value) : 0;
            if (gap != 20 && gap != 30 && gap != 40) {
                return {"invalid_setting", key};
            }
        } else {
            // Standalone-only, development and read-only keys included.
            return {"invalid_setting", key};
        }
    }
    const auto before = settings_;
    json_object_object_foreach(values, key_name, value) {
        const std::string key = key_name;
        const bool on = json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
        // A protocol write updates the pending Settings value for its key too,
        // so unapplied edits to other keys stay as they are.
        if (key == "swipe_typing") {
            settings_.swipe_typing = pending_swipe_typing_ = on;
        } else if (key == "num_lock") {
            keyboard_.set_num(on);
            settings_.num_lock = on;
        } else if (key == "hide_numpad") {
            settings_.hide_numpad = pending_hide_numpad_ = on;
        } else if (key == "use_drag_to_move") {
            settings_.use_drag_to_move = pending_use_drag_to_move_ = on;
        } else if (key == "auto_leveling") {
            settings_.auto_leveling = pending_auto_leveling_ = on;
        } else if (key == "pinned") {
            settings_.pinned = on;
        } else if (key == "emoji_close_on_select") {
            settings_.emoji_close_on_select = pending_emoji_close_on_select_ = on;
        } else if (key == "dictation_send_paste") {
            settings_.dictation_send_paste = pending_dictation_send_paste_ = on;
        } else if (key == "dictation_direct") {
            settings_.dictation_direct = pending_dictation_direct_ = on;
        } else if (key == "dictation_fallback") {
            settings_.dictation_fallback = pending_dictation_fallback_ = on;
        } else if (key == "dictation") {
            settings_.dictation = pending_dictation_ = json_object_get_string(value);
        } else if (key == "dictation_server") {
            settings_.dictation_server = pending_dictation_server_ = json_object_get_string(value);
            // A server the shell chose gets no editor text as context.
            settings_.dictation_server_by_shell = !settings_.dictation_server.empty();
        } else if (key == "dictation_send_gap") {
            settings_.dictation_send_gap = pending_dictation_send_gap_ = json_object_get_int(value);
        }
    }
    if (before.swipe_typing != settings_.swipe_typing) {
        prepare_swipe();
    }
    if (dictation_open_ && !dictation_available()) {
        close_dictation();
    }
    update_settings_ui();
    host_.unsaved_requests.push_back(request);
    save();
    host_settings_changed(before, "shell");
    dirty = true;
    return {};
}

std::string App::host_select(const std::string& layout, const std::string& language,
                             const std::string& favorite, std::int64_t request) {
    Selection selection = settings_.active;
    if (selection.theme == shell_theme_id) {
        selection.theme = host_.theme_before;
    }
    if (!favorite.empty()) {
        const auto found = std::find_if(settings_.favorites.begin(), settings_.favorites.end(),
                                        [&](const Favorite& f) { return f.id == favorite; });
        if (found == settings_.favorites.end()) {
            return "bad_request";
        }
        selection.layout = found->selection.layout;
        selection.language = found->selection.language;
    }
    if (!layout.empty()) {
        selection.layout = layout;
    }
    if (!language.empty()) {
        selection.language = language;
    }
    if (!profiles_.layouts.contains(selection.layout) ||
        !profiles_.languages.contains(selection.language) || !fits(profiles_, selection)) {
        return "bad_request";
    }
    const auto before = settings_;
    const bool custom = settings_.active.theme == shell_theme_id;
    try {
        apply(selection); // persists, as Settings → Apply does
    } catch (const std::exception& error) {
        status_ = error.what();
        return "failed";
    }
    if (custom) {
        settings_.active.theme = shell_theme_id; // the shell's theme stays on screen
        pending_.theme = shell_theme_id;
    }
    // A hinted language gives way to an explicit choice.
    host_.before_language.reset();
    host_.hinted_language.clear();
    host_.unsaved_requests.push_back(request);
    host_settings_changed(before, "shell");
    return {};
}

JsonPtr App::host_profiles() const {
    auto object = json_ptr(json_object_new_object());
    auto list = [&](const char* key, const auto& profiles) {
        auto* array = json_object_new_array();
        for (const auto& [id, profile] : profiles) {
            if (id == shell_theme_id) {
                continue;
            }
            auto* entry = json_object_new_object();
            json_object_object_add(entry, "id", json_object_new_string(id.c_str()));
            json_object_object_add(entry, "name", json_object_new_string(profile.name.c_str()));
            json_object_array_add(array, entry);
        }
        json_object_object_add(object.get(), key, array);
    };
    list("layouts", profiles_.layouts);
    list("languages", profiles_.languages);
    list("themes", profiles_.themes);
    auto* favorites = json_object_new_array();
    for (const auto& favorite : settings_.favorites) {
        auto* entry = json_object_new_object();
        json_object_object_add(entry, "id", json_object_new_string(favorite.id.c_str()));
        json_object_object_add(entry, "name", json_object_new_string(favorite.name.c_str()));
        json_object_object_add(entry, "layout",
                               json_object_new_string(favorite.selection.layout.c_str()));
        json_object_object_add(entry, "language",
                               json_object_new_string(favorite.selection.language.c_str()));
        json_object_array_add(favorites, entry);
    }
    json_object_object_add(object.get(), "favorites", favorites);
    return object;
}

std::string App::host_theme_select(const std::string& id, std::int64_t request) {
    if (id == shell_theme_id || !profiles_.themes.contains(id)) {
        return "bad_request";
    }
    const auto before = settings_;
    Selection selection = settings_.active;
    selection.theme = id;
    try {
        apply(selection);
    } catch (const std::exception& error) {
        status_ = error.what();
        return "failed";
    }
    host_.theme.reset();
    host_.theme_json.clear();
    profiles_.themes.erase(shell_theme_id);
    update_settings_ui();
    host_.unsaved_requests.push_back(request);
    host_settings_changed(before, "shell");
    return {};
}

std::pair<std::string, std::string> App::host_theme_set(const std::string& json) {
    Theme theme;
    try {
        // The same validator as theme files; its message names the field.
        theme = parse_theme(json);
    } catch (const std::exception& error) {
        return {"invalid_theme", error.what()};
    }
    theme.id = shell_theme_id;
    theme.name = "Shell theme";
    const auto before = settings_;
    if (settings_.active.theme != shell_theme_id) {
        host_.theme_before = settings_.active.theme;
    }
    host_.theme = theme;
    host_.theme_json = json;
    profiles_.themes[shell_theme_id] = std::move(theme);
    // Shown at once and for this process only; save() writes the theme it replaced.
    settings_.active.theme = shell_theme_id;
    if (!settings_open_) {
        pending_.theme = shell_theme_id;
    }
    update_settings_ui();
    host_settings_changed(before, "shell");
    dirty = true;
    return {};
}

void App::host_theme_reset() {
    if (!host_.theme) {
        return;
    }
    const auto before = settings_;
    host_.theme.reset();
    host_.theme_json.clear();
    if (settings_.active.theme == shell_theme_id) {
        settings_.active.theme =
            profiles_.themes.contains(host_.theme_before) ? host_.theme_before : "graphite";
    }
    if (pending_.theme == shell_theme_id) {
        pending_.theme = settings_.active.theme;
    }
    profiles_.themes.erase(shell_theme_id);
    update_settings_ui();
    host_settings_changed(before, "shell");
    dirty = true;
}

JsonPtr App::host_theme() const {
    auto object = json_ptr(json_object_new_object());
    const bool custom = settings_.active.theme == shell_theme_id;
    json_object_object_add(
        object.get(), "id",
        json_object_new_string(custom ? shell_theme_id : settings_.active.theme.c_str()));
    json_object_object_add(object.get(), "custom", json_object_new_boolean(custom));
    return object;
}

JsonPtr App::host_dictation() const {
    auto object = json_ptr(json_object_new_object());
    const std::string state = settings_.dictation == "off" || !dictation_available() ? "off"
                              : notice_error_                                        ? "error"
                              : capturing_                                           ? "listening"
                              : recognizing() || phrases_pending()                   ? "recognizing"
                              : dictation_open_                                      ? "paused"
                                                                                     : "idle";
    json_object_object_add(object.get(), "state", json_object_new_string(state.c_str()));
    json_object_object_add(object.get(), "editor", json_object_new_boolean(dictation_open_));
    if (download_) {
        auto* download = json_object_new_object();
        json_object_object_add(download, "bytes",
                               json_object_new_int64(static_cast<int64_t>(download_->done)));
        json_object_object_add(download, "total",
                               json_object_new_int64(static_cast<int64_t>(download_->total)));
        json_object_object_add(download, "preparing", json_object_new_boolean(download_->quantizing));
        json_object_object_add(object.get(), "download", download);
    }
    return object;
}

std::pair<std::string, JsonPtr> App::host_dictation_request(const std::string& op, json_object* body) {
    auto none = json_ptr(nullptr);
    if (op == "dictation.start") {
        if (hint_private() || !dictation_available()) {
            return {"unavailable", std::move(none)};
        }
        if (composing_text()) {
            return {"composing", std::move(none)};
        }
        // The same way the mic key does: Type as I speak listens on the
        // keyboard; otherwise the editor opens and listens.
        if (!direct_dictation() && !dictation_open_) {
            if (settings_open_) {
                back();
            }
            close_emoji();
            open_dictation();
        }
        if (!capturing_ && !start_capture()) {
            return {"failed", std::move(none)};
        }
        dirty = true;
        return {std::string{}, std::move(none)};
    }
    if (op == "dictation.stop") {
        if (capturing_) {
            pause_capture(); // recognized text stays in the editor
        }
        return {std::string{}, std::move(none)};
    }
    if (op == "dictation.test_server") {
        if (settings_.dictation_server.empty()) {
            return {"unavailable", std::move(none)};
        }
        if (testing_server_ ||
            (settings_open_ && pending_dictation_server_ != settings_.dictation_server)) {
            return {"busy", std::move(none)};
        }
        pending_dictation_server_ = settings_.dictation_server;
        host_.test_by_shell = true;
        test_dictation_server();
        return {std::string{}, std::move(none)};
    }
    if (op == "dictation.model") {
        json_object* action = nullptr;
        const std::string name = json_object_object_get_ex(body, "action", &action) &&
                                         json_object_is_type(action, json_type_string)
                                     ? json_object_get_string(action)
                                     : "";
        const auto& model = settings_.dictation_device_model;
        if (name == "status") {
            auto fields = json_ptr(json_object_new_object());
            json_object_object_add(fields.get(), "model", json_object_new_string(model.c_str()));
            json_object_object_add(fields.get(), "ready", json_object_new_boolean(model_ready(model)));
            json_object_object_add(fields.get(), "downloading",
                                   json_object_new_boolean(download_.has_value()));
            json_object_object_add(fields.get(), "megabytes",
                                   json_object_new_int(device_model_megabytes(model)));
            return {std::string{}, std::move(fields)};
        }
        if (name == "download") {
            if (model_ready(model)) {
                return {std::string{}, std::move(none)};
            }
            if (download_) {
                return {"busy", std::move(none)};
            }
            // Several hundred megabytes stay the user's decision: the panel
            // asks, and the user confirms there.
            if (!settings_open_) {
                close_emoji();
                show_settings();
            }
            pending_dictation_device_model_ = model;
            confirm_ = Confirm::Download;
            update_settings_ui();
            dirty = true;
            return {std::string{}, std::move(none)};
        }
        return {"bad_request", std::move(none)};
    }
    return {"unknown_op", std::move(none)};
}

JsonPtr App::host_state() const {
    auto object = json_ptr(json_object_new_object());
    json_object_object_add(object.get(), "view", json_object_new_string(view_name().c_str()));
    json_object_object_add(object.get(), "context",
                           host_.context ? json_object_new_string(host_.context->c_str()) : nullptr);
    auto* selection = json_object_new_object();
    json_object_object_add(selection, "layout", json_object_new_string(settings_.active.layout.c_str()));
    json_object_object_add(selection, "language",
                           json_object_new_string(settings_.active.language.c_str()));
    json_object_object_add(object.get(), "selection", selection);
    json_object_object_add(object.get(), "theme", json_object_get(host_theme().get()));
    json_object_object_add(object.get(), "settings", json_object_get(host_settings().get()));
    json_object_object_add(object.get(), "swipe",
                           json_object_new_boolean(swipe_ && swipe_keyboard_ && !hint_private()));
    json_object_object_add(object.get(), "dictation", json_object_get(host_dictation().get()));
    json_object_object_add(object.get(), "sink", json_object_new_boolean(sink_on()));
    if (!options_.last_update.empty()) {
        json_object_object_add(object.get(), "last_update",
                               json_object_new_string(options_.last_update.c_str()));
    }
    return object;
}

bool App::idle_for_update() const {
    return !keyboard_.active() && !composing_text() && editor_.empty() && !capturing_ &&
           !phrases_pending() && !recognizing() && !swipe_waiting_ && !swipe_track_ && !sending_ &&
           !output_.text_pending() && !output_.retry_available() && direct_waiting_.empty() &&
           !direct_paste_ && (!options_.shell_router || options_.shell_router->settled());
}

void App::activate_update() {
    // Unsent dictation is dropped, never committed; composition goes with cancel().
    end_dictation_for_update();
    cancel(true);
    set_interaction_active(false);
    quit_ = true;
}

void App::host_settings_changed(const Settings& before, const char* source) {
    if (!hosted()) {
        return;
    }
    std::vector<std::string> changed;
    auto compare = [&](const char* key, bool differs) {
        if (differs) {
            changed.emplace_back(key);
        }
    };
    compare("swipe_typing", before.swipe_typing != settings_.swipe_typing);
    compare("num_lock", before.num_lock != settings_.num_lock);
    compare("hide_numpad", before.hide_numpad != settings_.hide_numpad);
    compare("use_drag_to_move", before.use_drag_to_move != settings_.use_drag_to_move);
    compare("auto_leveling", before.auto_leveling != settings_.auto_leveling);
    compare("pinned", before.pinned != settings_.pinned);
    compare("emoji_close_on_select", before.emoji_close_on_select != settings_.emoji_close_on_select);
    compare("dictation", before.dictation != settings_.dictation);
    compare("dictation_server", before.dictation_server != settings_.dictation_server);
    compare("dictation_send_gap", before.dictation_send_gap != settings_.dictation_send_gap);
    compare("dictation_send_paste", before.dictation_send_paste != settings_.dictation_send_paste);
    compare("dictation_direct", before.dictation_direct != settings_.dictation_direct);
    compare("dictation_fallback", before.dictation_fallback != settings_.dictation_fallback);
    if (!changed.empty()) {
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "changed", string_array(changed));
        json_object_object_add(fields.get(), "source", json_object_new_string(source));
        host_event("settings", std::move(fields));
    }
    if (before.active.layout != settings_.active.layout ||
        before.active.language != settings_.active.language) {
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "layout",
                               json_object_new_string(settings_.active.layout.c_str()));
        json_object_object_add(fields.get(), "language",
                               json_object_new_string(settings_.active.language.c_str()));
        json_object_object_add(fields.get(), "source", json_object_new_string(source));
        host_event("selection", std::move(fields));
    }
    if (before.active.theme != settings_.active.theme) {
        auto fields = host_theme();
        json_object_object_add(fields.get(), "source", json_object_new_string(source));
        host_event("theme", std::move(fields));
    }
}

void App::host_tick(double now) {
    if (!hosted()) {
        return;
    }
    auto dictation = host_dictation();
    json_object* state = nullptr;
    json_object_object_get_ex(dictation.get(), "state", &state);
    const std::string current = json_object_get_string(state);
    const std::string error = notice_error_ ? "error" : "";
    if (current != host_.dictation_state || error != host_.dictation_error) {
        host_.dictation_state = current;
        host_.dictation_error = error;
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "state", json_object_new_string(current.c_str()));
        if (!error.empty()) {
            json_object_object_add(fields.get(), "error", json_object_new_string(error.c_str()));
        }
        host_event("dictation", std::move(fields));
    }
    // The microphone level, 0–100, at most ten times a second; never words.
    if (capturing_ && !mic_levels_.empty() && now - host_.level_at >= .1) {
        const int level = std::clamp(mic_levels_.back() * 100 / 8, 0, 100);
        if (level != host_.dictation_level) {
            host_.dictation_level = level;
            host_.level_at = now;
            auto fields = json_ptr(json_object_new_object());
            json_object_object_add(fields.get(), "state", json_object_new_string(current.c_str()));
            json_object_object_add(fields.get(), "level", json_object_new_int(level));
            host_event("dictation", std::move(fields), true);
        }
    }
    if (download_ && download_->done != host_.download_bytes) {
        host_.download_bytes = download_->done;
        host_.download_reported = true;
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "state", json_object_new_string(current.c_str()));
        json_object_object_add(fields.get(), "download",
                               json_object_get(json_object_object_get(dictation.get(), "download")));
        host_event("dictation", std::move(fields), true);
    } else if (!download_ && host_.download_reported) {
        host_.download_reported = false;
        host_.download_bytes = 0;
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "state", json_object_new_string(current.c_str()));
        json_object_object_add(fields.get(), "model_ready",
                               json_object_new_boolean(model_ready(settings_.dictation_device_model)));
        host_event("dictation", std::move(fields));
    }
    // A Test's answer, as a fixed code.
    if (!testing_server_ && host_.test_reported != dictation_test_) {
        host_.test_reported = dictation_test_;
        if (const auto code = test_code(dictation_test_);
            !code.empty() && dictation_test_ != "Testing…") {
            auto fields = json_ptr(json_object_new_object());
            json_object_object_add(fields.get(), "state", json_object_new_string(current.c_str()));
            json_object_object_add(fields.get(), "test", json_object_new_string(code.c_str()));
            host_event("dictation", std::move(fields));
        }
        host_.test_by_shell = false;
    }
}
} // namespace framekeyboard
