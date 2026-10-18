#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"
#include <utility>

#include <algorithm>
#include <chrono>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <tuple>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
constexpr auto output_blocked_status =
    "Waiting for compositor text delivery. Try the key again when it completes.";
bool same_keymap(const Language& a, const Language& b) {
    return std::tie(a.rules, a.model, a.keymap, a.variant, a.options) ==
           std::tie(b.rules, b.model, b.keymap, b.variant, b.options);
}
std::unique_ptr<CjkComposer> prepare_cjk(const Language& language) {
    if (language.input_method == "korean-2set" || language.input_method.starts_with("chinese-")) {
        return std::make_unique<CjkComposer>(language.input_method);
    }
    return {};
}
std::string compatible_layout(const Profiles& profiles, const Selection& selection) {
    try {
        validate_selection(profiles, selection);
        return selection.layout;
    } catch (const std::exception&) {
        // Prefer the smallest complete geometry, not alphabetical catalog order.
        const Layout* best = nullptr;
        for (const auto& [id, layout] : profiles.layouts) {
            try {
                validate_selection(profiles, {id, selection.language, selection.theme});
                if (!best || layout.keys.size() < best->keys.size()) {
                    best = &layout;
                }
            } catch (const std::exception&) {
            }
        }
        if (best) {
            return best->id;
        }
    }
    return selection.layout; // Apply reports the validation error if none fit.
}
bool same_pair(const Selection& a, const Selection& b) {
    return a.layout == b.layout && a.language == b.language;
}
std::string language_short_name(const Language& language) {
    // Locale preserves distinctions such as US/UK English and Chinese script.
    auto name = language.locale.empty() ? language.id : language.locale;
    std::replace(name.begin(), name.end(), '_', '-');
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return g_ascii_toupper(c); });
    return name;
}
} // namespace
App::App(const Options& options, KeySink& sink, SettingsWriter::Write write_preferences,
         Updates* updates, SettingsWriter::Write reconcile_preferences)
    : updates_(updates), options_(options),
      profiles_(load_profiles(options.data_dir, options.config_dir)),
      settings_writer_(options.config_dir, std::move(write_preferences),
                       std::move(reconcile_preferences)),
      gate_(sink), keyboard_(gate_) {
    std::vector<std::string> settings_errors;
    settings_ = load_settings(options.config_dir, settings_errors);
    profiles_.errors.insert(profiles_.errors.end(), settings_errors.begin(), settings_errors.end());
    persisted_active_ = settings_.active;
    auto fallback_status = [&](const std::string& message) {
        profiles_.errors.push_back(message);
        if (!startup_status_.empty()) {
            startup_status_ += " ";
        }
        startup_status_ += message;
    };
    if (!profiles_.themes.contains(settings_.active.theme)) {
        fallback_status("Saved theme unavailable. Using Graphite.");
        settings_.active.theme = "graphite";
    }
    if (!profiles_.languages.contains(settings_.active.language)) {
        fallback_status("Saved language unavailable. Using English.");
        settings_.active.language = "en-us";
    }
    try {
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
        cjk_ = prepare_cjk(profiles_.languages.at(settings_.active.language));
    } catch (const std::exception& error) {
        fallback_status("Saved language could not be prepared. Using English. " +
                        std::string(error.what()));
        settings_.active.language = "en-us";
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at("en-us"));
    }
    const auto layout = compatible_layout(profiles_, settings_.active);
    if (layout != settings_.active.layout) {
        fallback_status("Saved layout unavailable or incompatible. Using " + layout + ".");
        settings_.active.layout = layout;
    }
    try {
        validate_selection_profiles(profiles_, settings_.active);
    } catch (const std::exception& error) {
        profiles_.errors.push_back(error.what());
        // Even the default IDs can have user overrides. Restore only their
        // embedded models as a last resort, keeping other profiles and the
        // persisted choice available for repair in Settings.
        settings_.active = {};
        const auto defaults = load_profiles({}, {});
        profiles_.layouts[settings_.active.layout] = defaults.layouts.at(settings_.active.layout);
        profiles_.languages[settings_.active.language] =
            defaults.languages.at(settings_.active.language);
        profiles_.themes[settings_.active.theme] = defaults.themes.at(settings_.active.theme);
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
        cjk_.reset();
        startup_status_ = "Saved selection has no compatible layout. Using English defaults.";
    }
    keyboard_.set_num(settings_.num_lock);
    pending_ = default_ = settings_.active;
    rebuild_cycle_entries();
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    update_settings_ui();
    status_ = !startup_status_.empty()   ? startup_status_
              : profiles_.errors.empty() ? ""
                                         : profiles_.errors.front();
    if (const auto target = profiles_.languages.find(options_.target_language);
        target != profiles_.languages.end()) {
        target_language_ = target->second;
    }
    // A saved language mismatch must leave Settings reachable, not abort launch.
    refresh_typing();
    // Refresh owned units after an update without requiring Apply or Reload.
    // Invalid settings must not remove an existing startup configuration.
    if (options.mode == "vr" && settings_errors.empty()) {
        settings_writer_.reconcile(settings_);
    }
}
std::vector<Control> App::controls() const {
    std::vector<Control> result;
    PanelView geometry;
    geometry.layout = &profiles_.layouts.at(settings_.active.layout);
    geometry.theme = &profiles_.themes.at(settings_.active.theme);
    geometry.language = &profiles_.languages.at(settings_.active.language);
    geometry.hide_numpad = settings_.hide_numpad;
    geometry.settings = settings_open_;
    const auto body = panel_case_bounds(geometry);
    auto append_move_handle = [&] {
        if (!settings_.pinned && !settings_.use_drag_to_move) {
            result.push_back({"move-handle",
                              "Move keyboard",
                              {body.x + body.width / 2 - 220, panel_height + 12, 440, 60},
                              dragging_,
                              Icon::None,
                              ControlStyle::MoveHandle});
        }
    };
    if (settings_open_) {
        result = {{"settings", "Back (Esc)", {18, 12, 60, 42}, false, Icon::Back},
                  {"apply", "Apply and save", {88, 12, 60, 42}, false, Icon::Apply},
                  {"reload", "Reload config", {158, 12, 60, 42}, false, Icon::Reload}};
        result.push_back(
            {"updates", updates_ ? updates_->label() : "Check for updates", {550, 12, 500, 42}});
        if (settings_.replace_native_keyboard && native_session_) {
            result.push_back({"native-fallback", "Use Steam keyboard", {250, 12, 245, 42}});
        }
        if (resident_replacement()) {
            result.push_back({"quit", "Quit Full Keyboard", {body.x + body.width - 268, 12, 250, 42}});
        }
        append_move_handle();
        return result;
    }
    const double x = body.x + 18;
    result.push_back({"settings", "Settings", {x, 12, 60, 42}, false, Icon::Settings});
    result.push_back({"recenter", "Recenter", {x + 70, 12, 60, 42}, false, Icon::Recenter});
    result.push_back(
        {"size-smaller", "Smaller keyboard", {x + 140, 12, 60, 42}, false, Icon::ScaleDown});
    result.push_back({"size-larger", "Larger keyboard", {x + 210, 12, 60, 42}, false, Icon::ScaleUp});
    result.push_back({"pin",
                      settings_.pinned ? "Unpin keyboard" : "Pin keyboard",
                      {x + 280, 12, 60, 42},
                      false,
                      settings_.pinned ? Icon::PinOn : Icon::PinOff});
    if (!settings_.favorites.empty()) {
        result.push_back({"language-cycle",
                          language_short_name(profiles_.languages.at(settings_.active.language)),
                          {x + 350, 12, 100, 42}});
    }
    result.push_back({"close",
                      resident_replacement() ? "Hide keyboard" : "Close",
                      {body.x + body.width - 78, 12, 60, 42},
                      false,
                      Icon::Close});
    const auto ime_start = result.size();
    if (japanese()) {
        result.push_back({"ime-toggle", japanese_latin_ ? "A / あ" : "あ / A", {610, 12, 115, 42}});
        if (!japanese_latin_) {
            result.push_back({"ime-hiragana", "ひらがな", {735, 12, 120, 42}});
            result.push_back({"ime-katakana", "カタカナ", {865, 12, 120, 42}});
            result.push_back({"ime-commit", "確定 / Commit", {995, 12, 180, 42}});
            result.push_back({"ime-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = composition_.candidates();
            if (!candidates.empty()) {
                result.push_back({"ime-prev", "↑", {20, 148, 55, 42}});
                const int page = composition_.selected() / 5 * 5;
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    result.push_back(
                        {"ime-candidate-" + std::to_string(i),
                         std::to_string(i + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == composition_.selected()});
                }
                result.push_back({"ime-next", "↓", {1290, 148, 55, 42}});
                result.push_back({"ime-segment-prev", "←", {1355, 148, 55, 42}});
                result.push_back({"ime-segment-next", "→", {1420, 148, 55, 42}});
                result.push_back({"ime-segment-label",
                                  std::to_string(composition_.active_segment() + 1) + "/" +
                                      std::to_string(composition_.segment_count()),
                                  {1485, 148, 90, 42}});
            } else {
                result.push_back({"ime-convert", "変換 / Convert", {20, 148, 230, 42}});
            }
        }
    }
    if (cjk_) {
        result.push_back({"cjk-toggle",
                          cjk_latin_ ? "A / native" : (cjk_->chinese() ? "中文 / A" : "한 / A"),
                          {610, 12, 150, 42}});
        if (!cjk_latin_) {
            result.push_back({"cjk-commit", "Commit", {995, 12, 180, 42}});
            result.push_back({"cjk-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = cjk_->candidates();
            if (!candidates.empty()) {
                const int page = cjk_->selected() / 5 * 5;
                result.push_back({"cjk-prev", "↑", {20, 148, 55, 42}});
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    result.push_back(
                        {"cjk-candidate-" + std::to_string(i),
                         std::to_string(i - page + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == cjk_->selected()});
                }
                result.push_back({"cjk-next", "↓", {1290, 148, 55, 42}});
            }
        }
    }
    // Composition controls use the available case width; the main toolbar keeps
    // its button sizes and order when the numpad is hidden.
    for (std::size_t i = ime_start; i < result.size(); ++i) {
        result[i].bounds.x = body.x + result[i].bounds.x * body.width / panel_width;
        result[i].bounds.width *= body.width / panel_width;
    }
    append_move_handle();
    return result;
}
void App::update_settings_ui() {
    auto choices = [](const auto& profiles) {
        std::vector<SettingChoice> result;
        for (const auto& [id, profile] : profiles) {
            result.push_back({id, profile.name});
        }
        return result;
    };
    const bool favorite =
        std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                    [&](const Favorite& f) { return same_pair(f.selection, pending_); });
    settings_ui_.set_cards(
        {{"languages",
          "Languages and Layouts",
          {{"layout", "Layout:", pending_.layout, ControlStyle::Dropdown, false,
            choices(profiles_.layouts)},
           {"language", "Language:", pending_.language, ControlStyle::Dropdown, false,
            choices(profiles_.languages)},
           {"favorite", "Favorite:", "", ControlStyle::Checkbox, favorite, {}}}},
         {"general",
          "General",
          {{"theme", "Theme:", pending_.theme, ControlStyle::Dropdown, false, choices(profiles_.themes)},
           {"hide-numpad", "Hide numpad", "", ControlStyle::Checkbox, pending_hide_numpad_, {}, true},
           {"use-drag-to-move",
            "Use Drag to Move",
            "",
            ControlStyle::Checkbox,
            pending_use_drag_to_move_,
            {},
            true},
           {"auto-leveling",
            "Auto leveling",
            "",
            ControlStyle::Checkbox,
            pending_auto_leveling_,
            {},
            true}}},
         {"experimental",
          "Experimental",
          {{"replace-native-keyboard",
            "Replace native keyboard:",
            "",
            ControlStyle::Checkbox,
            pending_replace_native_keyboard_,
            {},
            false,
            300},
           {"start-with-steamos",
            "Start with SteamOS",
            "",
            ControlStyle::Checkbox,
            pending_start_with_steamos_ && pending_replace_native_keyboard_,
            {},
            false,
            300,
            pending_replace_native_keyboard_}}}});
}
PanelView App::view() const {
    PanelView v;
    v.layout = &profiles_.layouts.at(settings_.active.layout);
    v.theme = &profiles_.themes.at(settings_.active.theme);
    v.language = &profiles_.languages.at(settings_.active.language);
    v.keymap = keymap_.get();
    v.keyboard = &keyboard_;
    v.hide_numpad = settings_.hide_numpad;
    v.controls = controls();
    if (settings_open_) {
        settings_ui_.append(v);
    }
    v.status = dragging_ ? (settings_.use_drag_to_move ? "Release grab to place keyboard"
                                                       : "Release trigger to place keyboard")
                         : (status_.empty() ? startup_status_ : status_);
    if (settings_open_ && !dragging_ && updates_ && !updates_->failure_reason().empty()) {
        v.status = updates_->failure_reason();
    }
    // Image exports show the layout without the interactive preview's input notice.
    const bool image_export = options_.mode == "render" || options_.mode == "render-settings";
    if (v.status.empty() && !typing_ready_ && !image_export) {
        v.status = options_.input == "none"
                       ? "Preview only: this launch cannot type."
                       : "Typing disabled: choose a profile matching the target keymap used at launch.";
    }
    v.settings = settings_open_;
    if (settings_open_ && settings_.replace_native_keyboard && !replacement_available_ &&
        v.status.empty()) {
        v.status = replacement_unavailable_reason_.empty() ? "Native replacement unavailable. "
                                                           : replacement_unavailable_reason_ + " ";
        v.status += "Steam keyboard remains available.";
    }
    v.composing = japanese() && !japanese_latin_;
    if (v.composing) {
        v.preedit = composition_.preedit(true);
        if (v.language->input_method == "japanese-kana") {
            const auto mods = keyboard_.modifiers();
            const bool shifted =
                mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
            for (const auto& key : v.layout->keys) {
                const auto& map = shifted && v.language->kana_shift.contains(key.action)
                                      ? v.language->kana_shift
                                      : v.language->kana;
                if (auto it = map.find(key.action); it != map.end()) {
                    v.key_labels[key.id] = it->second;
                }
            }
        }
    }
    if (cjk_ && !cjk_latin_) {
        v.composing = true;
        v.preedit = cjk_->preedit();
        const auto mods = keyboard_.modifiers();
        const bool shifted =
            mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
        for (const auto& key : v.layout->keys) {
            const auto& map = shifted && v.language->composition_shift.contains(key.action)
                                  ? v.language->composition_shift
                                  : v.language->composition_keys;
            if (const auto it = map.find(key.action); it != map.end()) {
                v.key_labels[key.id] = it->second;
            }
        }
    }
    if (!backend_ready_) {
        v.status = backend_can_resume_ ? "Keyboard paused or disconnected. Waiting for the compositor."
                   : resident_replacement()
                       ? "Keyboard connection lost. Use Settings > Quit Full Keyboard, then relaunch."
                       : "Keyboard connection lost. Reopen the app to reconnect.";
    } else if ((unicode_mode() || japanese() || cjk_) && options_.input != "none" && !text_ready_) {
        v.status = "Text delivery unavailable. Waiting for the compositor connection to recover.";
    }
    for (const auto& [pointer, target] : hovered_) {
        (void)pointer;
        if (target.kind == HoverTarget::Kind::Key) {
            v.hovered_keys.insert(target.id);
        } else if (target.kind == HoverTarget::Kind::Control) {
            v.hovered_controls.insert(target.id);
        }
    }
    return v;
}
bool App::scroll(unsigned pointer, double dx, double dy) {
    if (!settings_open_ || !interaction_active_ || dragging_ || !pointer_positions_.contains(pointer)) {
        return false;
    }
    const auto [x, y] = pointer_positions_.at(pointer);
    if (!settings_ui_.scroll(x, y, dx, dy)) {
        return false;
    }
    // A scroll must never put a different action under a held trigger.
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
    return true;
}
bool App::move(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    pointer_positions_[pointer] = {x, y};
    if (settings_open_ && settings_ui_.move(pointer, x, y)) {
        pressed_controls_.clear();
        hovered_.clear();
        dirty = true;
        return false;
    }
    const auto v = view();
    HoverTarget target;
    for (auto it = v.controls.rbegin(); it != v.controls.rend(); ++it) {
        if (v.popup && !it->id.starts_with("choose:") && it->id != "scroll-menu") {
            continue;
        }
        if (it->hit(x, y)) {
            target = {HoverTarget::Kind::Control, it->id};
            break;
        }
    }
    if (target.kind == HoverTarget::Kind::None) {
        if (const auto* key = renderer.hit_key(v, x, y)) {
            target = {HoverTarget::Kind::Key, key->id};
        }
    }
    if (hovered_[pointer] != target) {
        hovered_[pointer] = target;
        dirty = true;
        return target.kind == HoverTarget::Kind::Key && !dragging_ &&
               !keyboard_.pointer_pressed(pointer);
    }
    return false;
}

bool App::down(unsigned pointer, double x, double y, double now) {
    if (dragging_ || !interaction_active_) {
        return false;
    }
    move(pointer, x, y);
    const auto v = view();
    if (settings_open_ && settings_ui_.down(pointer, x, y)) {
        pressed_controls_.clear();
        dirty = true;
        return false;
    }
    if (v.popup && !v.popup->contains(x, y)) {
        settings_ui_.close_popup();
        pressed_controls_.clear();
        hovered_.clear();
        dirty = true;
        return false; // Dismissal consumes the click instead of hitting through.
    }
    if (hovered_[pointer].kind == HoverTarget::Kind::Control) {
        pressed_controls_[pointer] = hovered_[pointer].id;
        return false;
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        const auto modifiers = keyboard_.modifiers();
        const bool ime_local = japanese_key(*key, false, modifiers) || cjk_key(*key, false, modifiers);
        const bool text_local = !ime_local && text_key(*key, modifiers);
        const bool cancel_accent = unicode_mode() && keymap_->composing() &&
                                   (key->action == "Escape" || key->action == "Backspace");
        const bool retry_text = !pending_text_.empty() && key->action == "Enter";
        const bool local = ime_local || text_local || cancel_accent || retry_text;
        if (!backend_ready_ || keyboard_.pointer_pressed(pointer)) {
            return false;
        }
        // Pending text is resolved below. Cancellation can leave a transport
        // barrier even when there is no pending result left for App to consume.
        if (!local && !gate_.text_pending() && gate_.output_blocked()) {
            status_ = output_blocked_status;
            dirty = true;
            return false;
        }
        const bool num_dependent = key->action == "NumpadDecimal" ||
                                   (key->action.starts_with("Numpad") && key->action.size() == 7 &&
                                    g_ascii_isdigit(key->action.back()));
        if (!unicode_mode() && num_dependent && gate_.enabled) {
            // Never guess a native target's lock state or blindly toggle it at
            // launch. Wait for feedback before forwarding a lock-dependent key.
            const auto target_num = gate_.num_lock_state();
            if (target_num && native_num_requested_ == target_num) {
                native_num_requested_.reset();
            }
            if (native_num_requested_ || !target_num || *target_num != keyboard_.num()) {
                if (target_num && !native_num_requested_) {
                    gate_.send(KEY_NUMLOCK, 1);
                    gate_.send(KEY_NUMLOCK, 0);
                    native_num_requested_ = !*target_num;
                }
                status_ = "Waiting for target Num Lock. Press the keypad key again.";
                dirty = true;
                return false;
            }
        }
        const bool sends_action = key->action_kind == ActionKind::Shortcut ||
                                  !is_modifier(key_code(key->action)) || !key->sticky;
        if (!pending_text_.empty() && sends_action) {
            if (!flush_text()) {
                dirty = true;
                return false;
            }
        }
        if (!ime_local && (!composition_.empty() || (cjk_ && !cjk_->empty())) && sends_action) {
            // Commit before either native events or ordinary Unicode text. Spaces,
            // digits and punctuation must not overtake the unfinished IME word.
            try {
                commit_japanese();
                commit_cjk();
            } catch (const std::exception& error) {
                status_ = error.what();
            }
            dirty = true;
            if (!composition_.empty() || (cjk_ && !cjk_->empty())) {
                return false; // Failed commit must not lose text or change the target.
            }
        }
        if (gate_.text_pending()) {
            return false;
        }
        // Capture the symbol before a tapped Shift/AltGr is consumed by down().
        const auto symbol = text_local
                                ? keymap_->symbol(*key, modifiers, keyboard_.caps(), keyboard_.num())
                                : XKB_KEY_NoSymbol;
        const bool old_num = keyboard_.num();
        const bool accepted =
            keyboard_.down(pointer, *key, now, local, local ? 0 : native_code(*key, modifiers));
        if (accepted) {
            try {
                if (ime_local) {
                    japanese_key(*key, true, modifiers);
                    cjk_key(*key, true, modifiers);
                } else if (cancel_accent) {
                    keymap_->cancel_compose();
                    status_.clear();
                } else if (text_local && key->action != "CapsLock" && key->action != "NumLock") {
                    pending_text_ = keymap_->compose(symbol);
                    const auto repeated = pending_text_;
                    if ((flush_text() || gate_.text_pending()) && !repeated.empty()) {
                        text_repeats_[pointer] = {repeated, now + .5};
                    }
                    if (keymap_->composing()) {
                        status_ = "Accent pending";
                    }
                } else if (!local) {
                    keymap_->cancel_compose();
                    status_.clear();
                }
            } catch (const std::exception& error) {
                status_ = error.what();
            }
            if (keyboard_.num() != old_num) {
                settings_.num_lock = keyboard_.num();
                save();
            }
        }
        dirty |= accepted;
        return accepted;
    }
    return false;
}

bool App::up(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    if (settings_open_ && settings_ui_.up(pointer)) {
        pressed_controls_.erase(pointer);
        dirty = true;
        return false;
    }
    text_repeats_.erase(pointer);
    const bool key_released = keyboard_.up(pointer);
    move(pointer, x, y);
    const auto it = pressed_controls_.find(pointer);
    if (it != pressed_controls_.end()) {
        const auto id = it->second;
        pressed_controls_.erase(it);
        if (hovered_[pointer] == HoverTarget{HoverTarget::Kind::Control, id}) {
            try {
                action(id);
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
    }
    dirty = true;
    return key_released;
}

void App::cancel_pointer(unsigned pointer) {
    settings_ui_.cancel_pointer(pointer);
    pointer_positions_.erase(pointer);
    text_repeats_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    settings_ui_.cancel();
    pointer_positions_.clear();
    text_repeats_.clear();
    if (discard_composition) {
        gate_.cancel_text();
        pending_text_.clear();
        if (keymap_) {
            keymap_->cancel_compose();
        }
        composition_.cancel();
        if (cjk_) {
            cjk_->cancel();
        }
    }
    keyboard_.cancel_all();
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
}
bool App::tick(double now) {
    if (updates_) {
        if (updates_->tick(now)) {
            // A new async label must not turn an already-held press into installation.
            std::erase_if(pressed_controls_,
                          [](const auto& entry) { return entry.second == "updates"; });
            dirty = true;
        }
        if (updates_->prepared()) {
            set_interaction_active(false);
            quit_ = true;
        }
    }
    update_save_status();
    refresh_typing();
    if (gate_.enabled && gate_.text_pending()) {
        if (!pending_text_.empty()) {
            flush_text();
        } else if (!composition_.empty()) {
            commit_japanese();
        } else if (cjk_ && !cjk_->empty()) {
            commit_cjk();
        }
        dirty = true;
    }
    dirty |= keyboard_.tick(now);
    if (!pending_text_.empty() && !gate_.text_pending()) {
        text_repeats_.clear();
    }
    for (auto it = text_repeats_.begin(); it != text_repeats_.end();) {
        if (now >= it->second.next) {
            if (gate_.text_pending()) {
                ++it;
                continue;
            }
            pending_text_ = it->second.text;
            if (!flush_text() && !gate_.text_pending()) {
                // Keep the unsent text, stop repeating, and require an explicit retry.
                text_repeats_.clear();
                break;
            }
            it->second.next = now + .04;
        }
        ++it;
    }
    return dirty || renderer.animating();
}
void App::paint(double now) {
    renderer.paint(view(), now);
    dirty = false;
}
void App::set_interaction_active(bool active) {
    if (active == interaction_active_) {
        return;
    }
    // Release through the still-open gate; disabling first would strand held
    // modifiers in the compositor. Also discard preedit and queued UI clicks.
    cancel();
    interaction_active_ = active;
    if (active) {
        refresh_typing();
    } else {
        gate_.enabled = false;
    }
}
void App::set_dragging(bool dragging) {
    if (dragging && settings_.pinned) {
        return;
    }
    if (dragging_ == dragging) {
        return;
    }
    cancel();
    dragging_ = dragging;
    status_.clear();
}
void App::report_status(const std::string& message) {
    status_ = message;
    dirty = true;
}
void App::show_settings() {
    cancel();
    settings_open_ = true;
    pending_ = settings_.active;
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    update_settings_ui();
    settings_ui_.reset();
    dirty = true;
}
void App::back() {
    if (settings_ui_.popup_open()) {
        settings_ui_.close_popup();
        pressed_controls_.clear();
        hovered_.clear();
    } else if (settings_open_) {
        cancel();
        settings_open_ = false;
    }
    dirty = true;
}
void App::summon() {
    dismiss_requested_ = false;
    // Release held keys before moving; keep the input connection unchanged.
    cancel();
    settings_open_ = false;
    placement_actions_.clear();
    recenter_ = true;
    status_.clear();
}
std::vector<PlacementAction> App::take_placement_actions() {
    std::vector<PlacementAction> result;
    result.swap(placement_actions_);
    return result;
}
void App::activate(Selection selection) {
    allow_slow_work();
    validate_selection_profiles(profiles_, selection);
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    auto cjk = prepare_cjk(profiles_.languages.at(selection.language));
    cancel();
    japanese_latin_ = false;
    cjk_latin_ = false;
    cjk_ = std::move(cjk);
    settings_.active = std::move(selection);
    keymap_ = std::move(keymap);
    pending_ = settings_.active;
    refresh_typing();
    status_.clear();
    update_settings_ui();
    dirty = true;
}
void App::save() {
    try {
        auto saved = settings_;
        saved.active = persisted_active_;
        settings_writer_.enqueue(std::move(saved));
    } catch (const std::exception& error) {
        status_ = "Applied, but not saved: " + std::string(error.what());
    }
}
void App::update_save_status() {
    if (const auto result = settings_writer_.take_result()) {
        if (result->error.empty()) {
            if (status_ == save_status_) {
                status_.clear();
            }
            save_status_.clear();
        } else {
            save_status_ =
                (result->reconcile ? "SteamOS startup not configured: " : "Applied, but not saved: ") +
                result->error;
            status_ = save_status_;
        }
        dirty = true;
    }
}
void App::flush_settings() {
    allow_slow_work();
    settings_writer_.flush();
    update_save_status();
}
void App::allow_slow_work() {
    double unset = 0;
    slow_work_deadline_.compare_exchange_strong(unset, monotonic_seconds() + 30);
}
void App::apply(Selection selection) {
    activate(std::move(selection));
    default_ = settings_.active;
    persisted_active_ = settings_.active;
    startup_status_.clear();
    rebuild_cycle_entries();
    save();
}
void App::rebuild_cycle_entries() {
    std::vector<Selection> entries;
    std::map<std::string, bool> languages;
    auto append = [&](Selection candidate) {
        candidate.theme = settings_.active.theme;
        if (std::any_of(entries.begin(), entries.end(),
                        [&](const Selection& entry) { return same_pair(entry, candidate); })) {
            return;
        }
        try {
            validate_selection_profiles(profiles_, candidate);
            if (!languages.contains(candidate.language)) {
                // Compile each language once during cache rebuilding, never
                // compile the entire favorites list in a cycle click handler.
                languages[candidate.language] = false;
                LanguageMap check(profiles_.languages.at(candidate.language));
                languages[candidate.language] = true;
            }
            if (!languages.at(candidate.language)) {
                return;
            }
            entries.push_back(std::move(candidate));
        } catch (const std::exception&) {
            // A removed/incompatible favorite must not prevent cycling the rest.
        }
    };
    append(default_);
    for (const auto& favorite : settings_.favorites) {
        append(favorite.selection);
    }
    cycle_entries_ = std::move(entries);
}
void App::reload() {
    allow_slow_work();
    // A pending preference write must finish before reading an edited config;
    // otherwise an older snapshot could overwrite it after Reload returns.
    flush_settings();
    auto candidate = load_profiles(options_.data_dir, options_.config_dir, &profiles_);
    validate_selection_profiles(candidate, settings_.active);
    // Preparing a keymap can fail. Finish that work before releasing the old model.
    auto keymap = std::make_unique<LanguageMap>(candidate.languages.at(settings_.active.language));
    auto cjk = prepare_cjk(candidate.languages.at(settings_.active.language));
    cancel();
    cjk_ = std::move(cjk);
    profiles_ = std::move(candidate);
    keymap_ = std::move(keymap);
    std::vector<std::string> config_errors;
    const auto updated_settings = load_settings(options_.config_dir, config_errors);
    if (config_errors.empty()) {
        settings_.favorites = updated_settings.favorites;
        settings_.pinned = updated_settings.pinned;
        settings_.num_lock = updated_settings.num_lock;
        settings_.hide_numpad = updated_settings.hide_numpad;
        settings_.use_drag_to_move = updated_settings.use_drag_to_move;
        settings_.auto_leveling = updated_settings.auto_leveling;
        settings_.replace_native_keyboard = updated_settings.replace_native_keyboard;
        settings_.start_with_steamos = updated_settings.start_with_steamos;
        settings_writer_.reconcile(settings_);
        keyboard_.set_num(settings_.num_lock);
    }
    profiles_.errors.insert(profiles_.errors.end(), config_errors.begin(), config_errors.end());
    pending_ = settings_.active;
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    rebuild_cycle_entries();
    update_settings_ui();
    settings_ui_.reset();
    refresh_typing();
    status_ = profiles_.errors.empty() ? "" : profiles_.errors.front();
}
bool App::japanese() const {
    return profiles_.languages.at(settings_.active.language).input_method.starts_with("japanese-");
}
void App::refresh_typing() {
    const bool ready = gate_.pump();
    if (status_ == output_blocked_status && !gate_.output_blocked()) {
        status_.clear();
        dirty = true;
    }
    const bool reset = gate_.take_input_reset();
    const bool can_resume = gate_.can_resume();
    if (reset || (backend_ready_ && !ready)) {
        native_num_requested_.reset();
        // Clear UI holds even if input was already disabled by a hidden dashboard.
        // The backend has released its keys; never replay those holds on resume.
        cancel();
    }
    dirty |= ready != backend_ready_ || can_resume != backend_can_resume_;
    backend_ready_ = ready;
    backend_can_resume_ = can_resume;
    const auto& language = profiles_.languages.at(settings_.active.language);
    text_ready_ = (unicode_mode() || japanese() || cjk_) && gate_.text_available();
    // Unicode mode owns character mapping. Only physical-only backends and
    // external JIS still rely on a declared receiving-session keymap.
    const bool matching = target_language_ && same_keymap(*target_language_, language) &&
                          (japanese() || cjk_ || options_.target_language == settings_.active.language);
    // Hidden panels and native handoffs deliberately pause interaction. That is
    // separate from an unavailable typing backend and must not show a warning.
    const bool typing_ready =
        options_.start_enabled && options_.mode == "vr" &&
        (options_.input == "ei" || options_.input == "uinput") &&
        (unicode_mode() ? text_ready_ : matching && (!(japanese() || cjk_) || text_ready_)) &&
        backend_ready_;
    dirty |= typing_ready_ != typing_ready;
    typing_ready_ = typing_ready;
    if (gate_.enabled && !(interaction_active_ && typing_ready_)) {
        // Transport loss ends key ownership, but unsent composition stays for
        // an explicit retry once text delivery is available again.
        cancel(false);
    }
    gate_.enabled = interaction_active_ && typing_ready_;
}
void App::convert_japanese() {
    // Anthy loads its library and dictionary lazily on first conversion.
    allow_slow_work();
    composition_.convert();
}
void App::commit_japanese() {
    const auto text = composition_.commit_text();
    if (text.empty()) {
        return;
    }
    if (options_.input == "none") {
        composition_.cancel();
        status_ = "Preview: composition committed locally.";
    } else if (gate_.commit_text(text)) {
        composition_.cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::japanese_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!japanese() || key.action_kind != ActionKind::Key) {
        return false;
    }
    const auto& code = key.action;
    if (code == "ZenkakuHankaku" || code == "KanaMode") {
        if (execute) {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        }
        return true;
    }
    if (japanese_latin_) {
        return false;
    }
    for (const char* modifier :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(modifier))) {
            return false;
        }
    }
    const bool shift = mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
    const bool composing = !composition_.empty();
    if (code == "Convert" || (code == "Space" && composing)) {
        if (execute) {
            if (shift && composition_.converting()) {
                composition_.cycle(-1);
            } else {
                convert_japanese();
            }
        }
        return true;
    }
    if (code == "NonConvert" || (composing && (code == "F6" || code == "F7"))) {
        if (execute) {
            composition_.script(code == "F7");
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Backspace" || code == "Escape" ||
         code == "ArrowLeft" || code == "ArrowRight" || code == "ArrowUp" || code == "ArrowDown")) {
        if (execute) {
            if (code == "Enter" || code == "NumpadEnter") {
                commit_japanese();
            } else if (code == "Backspace") {
                composition_.backspace();
            } else if (code == "Escape") {
                if (composition_.converting()) {
                    composition_.unconvert();
                } else {
                    composition_.cancel();
                }
            } else if (code == "ArrowLeft" || code == "ArrowRight") {
                composition_.segment(code == "ArrowLeft" ? -1 : 1);
            } else {
                composition_.cycle(code == "ArrowUp" ? -1 : 1);
            }
        }
        return true;
    }
    const auto& language = profiles_.languages.at(settings_.active.language);
    if (language.input_method == "japanese-kana") {
        const auto& map =
            shift && language.kana_shift.contains(code) ? language.kana_shift : language.kana;
        if (auto it = map.find(code); it != map.end()) {
            if (execute) {
                if (composition_.converting()) {
                    commit_japanese();
                    if (!composition_.empty()) {
                        return true;
                    }
                }
                composition_.kana(it->second == "゛"   ? "\u3099"
                                  : it->second == "゜" ? "\u309a"
                                                       : it->second);
            }
            return true;
        }
        return false;
    }
    if (code == "Space") {
        return false; // With no preedit, Space remains ordinary Unicode output.
    }
    // Legends may be cosmetic overrides. Composition follows the actual XKB
    // symbol, including Shift/Caps, independently of the displayed label.
    const auto character =
        xkb_keysym_to_utf32(keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num()));
    if (character >= 0x20 && character <= 0x7e && !is_modifier(key_code(code))) {
        if (execute) {
            if (composition_.converting()) {
                commit_japanese();
                if (!composition_.empty()) {
                    return true;
                }
            }
            composition_.roman(static_cast<char>(character));
        }
        return true;
    }
    return false;
}
bool App::unicode_mode() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    // External JIS explicitly delegates conversion to a system IME.
    const bool external_jis = language.input_method == "xkb" && language.keymap == "jp";
    return options_.input != "uinput" && !external_jis;
}
bool App::text_key(const Key& key, const std::set<int>& mods) const {
    if (key.action_kind == ActionKind::Key && key.action == "NumLock") {
        return true; // Remember one local preference in every input mode.
    }
    if (!unicode_mode() || key.action_kind != ActionKind::Key) {
        return false;
    }
    if (key.action == "CapsLock" || key.action == "NumLock") {
        return true;
    }
    for (const auto* name : {"ControlLeft", "ControlRight", "AltLeft", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    if (mods.contains(key_code("AltRight"))) {
        Key alt;
        alt.action_kind = ActionKind::Key;
        alt.action = "AltRight";
        if (keymap_->symbol(alt, {}, false, false) != XKB_KEY_ISO_Level3_Shift) {
            return false;
        }
    }
    const auto symbol = keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num());
    return symbol == XKB_KEY_KP_Begin || LanguageMap::printable(symbol);
}
bool App::flush_text() {
    if (pending_text_.empty()) {
        return true;
    }
    if (options_.input == "none" || gate_.commit_text(pending_text_)) {
        pending_text_.clear();
        status_.clear();
        return true;
    }
    status_ = gate_.text_pending() ? "Sending text to Steam..."
                                   : "Text not sent. Release other keys, then press Enter to retry.";
    return false;
}
int App::native_code(const Key& key, const std::set<int>& mods) {
    if (!unicode_mode()) {
        return 0;
    }
    if (key.action_kind == ActionKind::Shortcut) {
        return gate_.shortcut_code(key.action == "copy" ? XKB_KEY_c : XKB_KEY_v,
                                   key_code(key.action == "copy" ? "KeyC" : "KeyV"));
    }
    if (key.action.starts_with("Numpad")) {
        // Num Lock is local in Unicode mode. Send explicit navigation codes,
        // so the receiving system's Num Lock cannot turn Home into '7'.
        switch (keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num())) {
        case XKB_KEY_KP_Home:
            return KEY_HOME;
        case XKB_KEY_KP_End:
            return KEY_END;
        case XKB_KEY_KP_Left:
            return KEY_LEFT;
        case XKB_KEY_KP_Right:
            return KEY_RIGHT;
        case XKB_KEY_KP_Up:
            return KEY_UP;
        case XKB_KEY_KP_Down:
            return KEY_DOWN;
        case XKB_KEY_KP_Prior:
            return KEY_PAGEUP;
        case XKB_KEY_KP_Next:
            return KEY_PAGEDOWN;
        case XKB_KEY_KP_Insert:
            return KEY_INSERT;
        case XKB_KEY_KP_Delete:
            return KEY_DELETE;
        default:
            break;
        }
    }
    auto shortcut_symbol = xkb_keysym_to_lower(keymap_->symbol(key, {}, false, false));
    if (!mods.empty() && (key.action.starts_with("Key") ||
                          (shortcut_symbol >= XKB_KEY_a && shortcut_symbol <= XKB_KEY_z))) {
        auto symbol = shortcut_symbol;
        // Cyrillic and IME shortcuts conventionally retain their Latin physical
        // counterparts; Latin layouts use their displayed letter (AZERTY A, etc.).
        if (symbol < XKB_KEY_a || symbol > XKB_KEY_z) {
            symbol = static_cast<xkb_keysym_t>(g_ascii_tolower(key.action.back()));
        }
        const int fallback = key_code(
            "Key" + std::string(1, static_cast<char>(g_ascii_toupper(static_cast<char>(symbol)))));
        return gate_.shortcut_code(symbol, fallback);
    }
    return 0;
}
void App::choose_cjk(int index) {
    if (!cjk_) {
        return;
    }
    cjk_->choose(index);
    if (!cjk_->has_reading()) {
        commit_cjk();
    }
}
void App::commit_cjk() {
    if (!cjk_ || cjk_->empty()) {
        return;
    }
    const auto text = cjk_->text();
    if (!text.empty() && (options_.input == "none" || gate_.commit_text(text))) {
        cjk_->cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::cjk_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!cjk_ || cjk_latin_ || key.action_kind != ActionKind::Key) {
        return false;
    }
    for (const auto* name :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    const auto& code = key.action;
    const bool composing = !cjk_->empty();
    if (cjk_->chinese() && composing && code.size() == 6 && code.starts_with("Digit") &&
        code.back() >= '1' && code.back() <= '5' && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight")) && !cjk_->candidates().empty()) {
        const int index = cjk_->selected() / 5 * 5 + code.back() - '1';
        if (execute && index < static_cast<int>(cjk_->candidates().size())) {
            choose_cjk(index);
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Escape" || code == "Backspace" ||
         (cjk_->chinese() && (code == "Space" || code == "ArrowUp" || code == "ArrowDown")))) {
        if (execute) {
            if (code == "Backspace") {
                cjk_->backspace();
            } else if (code == "Escape") {
                cjk_->cancel();
            } else if (code == "ArrowUp" || code == "ArrowDown") {
                cjk_->cycle(code == "ArrowUp" ? -1 : 1);
            } else if (code == "Space" && !cjk_->candidates().empty()) {
                choose_cjk(cjk_->selected());
            } else {
                commit_cjk();
            }
        }
        return true;
    }
    // Use physical US letter positions for Pinyin and two-set Hangul. Caps Lock
    // does not choose doubled Korean jamo; only Shift does.
    if (code.starts_with("Key") && code.size() == 4) {
        char ch = g_ascii_tolower(code.back());
        if (!cjk_->chinese() &&
            (mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight")))) {
            ch = g_ascii_toupper(ch);
        }
        if (execute) {
            cjk_->type(ch);
        }
        return true;
    }
    if (cjk_->chinese() && code == "Quote" && composing && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight"))) {
        if (execute) {
            cjk_->type('\'');
        }
        return true;
    }
    return false;
}
void App::action(const std::string& id) {
    const std::map<std::string, PlacementAction> adjustments = {
        {"size-smaller", PlacementAction::Smaller}, {"size-larger", PlacementAction::Larger}};
    if (const auto adjustment = adjustments.find(id); adjustment != adjustments.end()) {
        cancel();
        placement_actions_.push_back(adjustment->second);
        return;
    }
    if (gate_.text_pending() && (id.starts_with("cjk-") || id.starts_with("ime-"))) {
        if (id != "cjk-cancel" && id != "ime-cancel" && id != "ime-toggle") {
            return;
        }
        gate_.cancel_text();
    }
    if (id.starts_with("cjk-") && cjk_) {
        if (id == "cjk-toggle") {
            if (!cjk_->empty()) {
                commit_cjk();
                if (!cjk_->empty()) {
                    return;
                }
            }
            cjk_latin_ = !cjk_latin_;
        } else if (id == "cjk-commit") {
            commit_cjk();
        } else if (id == "cjk-cancel") {
            cjk_->cancel();
        } else if (id == "cjk-next" || id == "cjk-prev") {
            cjk_->cycle(id == "cjk-next" ? 1 : -1);
        } else if (id.starts_with("cjk-candidate-")) {
            choose_cjk(std::stoi(id.substr(14)));
        }
        return;
    }
    if (id.starts_with("ime-")) {
        if (id == "ime-toggle") {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        } else if (id == "ime-convert") {
            convert_japanese();
        } else if (id == "ime-commit") {
            commit_japanese();
        } else if (id == "ime-cancel") {
            composition_.cancel();
        } else if (id == "ime-hiragana" || id == "ime-katakana") {
            composition_.script(id == "ime-katakana");
        } else if (id == "ime-next" || id == "ime-prev") {
            composition_.cycle(id == "ime-next" ? 1 : -1);
        } else if (id == "ime-segment-next" || id == "ime-segment-prev") {
            composition_.segment(id == "ime-segment-next" ? 1 : -1);
        } else if (id.starts_with("ime-candidate-")) {
            composition_.choose(std::stoi(id.substr(14)));
        }
        return;
    }
    if (id == "updates" && settings_open_) {
        if (updates_) {
            updates_->click(monotonic_seconds());
        } else {
            report_status("Updates are available from the installed keyboard.");
        }
    } else if (id == "close") {
        close_panel();
    } else if (id == "quit" && resident_replacement()) {
        set_interaction_active(false);
        quit_ = true;
    } else if (id == "pin") {
        cancel();
        settings_.pinned = !settings_.pinned;
        status_.clear();
        save();
    } else if (id == "language-cycle") {
        const auto& entries = cycle_entries_;
        if (entries.size() > 1) {
            const auto current =
                std::find_if(entries.begin(), entries.end(),
                             [&](const Selection& entry) { return same_pair(entry, settings_.active); });
            const auto index =
                current == entries.end()
                    ? 0
                    : (static_cast<std::size_t>(current - entries.begin()) + 1) % entries.size();
            std::string failure;
            for (std::size_t offset = 0; offset < entries.size(); ++offset) {
                auto candidate = entries[(index + offset) % entries.size()];
                candidate.theme = settings_.active.theme;
                try {
                    activate(std::move(candidate));
                    return;
                } catch (const std::exception& error) {
                    // Missing optional engines must not strand the cycle at an
                    // unusable favorite. Keep the current keyboard until one works.
                    failure = error.what();
                }
            }
            status_ = failure;
        }
    } else if (id == "settings") {
        if (settings_open_) {
            back();
        } else {
            show_settings();
        }
    } else if (id == "recenter") {
        cancel();
        placement_actions_.clear();
        recenter_ = true;
    } else if (id == "apply") {
        // Prepare/activate first. A failed candidate must leave all saved state intact.
        activate(pending_);
        settings_.favorites = pending_favorites_;
        settings_.hide_numpad = pending_hide_numpad_;
        settings_.use_drag_to_move = pending_use_drag_to_move_;
        settings_.auto_leveling = pending_auto_leveling_;
        settings_.replace_native_keyboard = pending_replace_native_keyboard_;
        settings_.start_with_steamos = pending_start_with_steamos_;
        default_ = settings_.active;
        persisted_active_ = settings_.active;
        startup_status_.clear();
        rebuild_cycle_entries();
        save();
        settings_open_ = false;
    } else if (id == "reload") {
        reload();
    } else if (id == "layout" || id == "language" || id == "theme") {
        pressed_controls_.clear();
        hovered_.clear();
        settings_ui_.toggle(id);
    } else if (const auto choice = settings_ui_.choice(id)) {
        pressed_controls_.clear();
        hovered_.clear();
        if (choice->first == "layout") {
            pending_.layout = choice->second;
        } else if (choice->first == "language") {
            pending_.language = choice->second;
            pending_.layout = compatible_layout(profiles_, pending_);
        } else if (choice->first == "theme") {
            pending_.theme = choice->second;
        }
        settings_ui_.close_popup();
        update_settings_ui();
    } else if (id == "hide-numpad") {
        pending_hide_numpad_ = !pending_hide_numpad_;
        update_settings_ui();
    } else if (id == "use-drag-to-move") {
        pending_use_drag_to_move_ = !pending_use_drag_to_move_;
        update_settings_ui();
    } else if (id == "replace-native-keyboard") {
        pending_replace_native_keyboard_ = !pending_replace_native_keyboard_;
        std::erase_if(pressed_controls_,
                      [](const auto& entry) { return entry.second == "start-with-steamos"; });
        update_settings_ui();
    } else if (id == "start-with-steamos" && pending_replace_native_keyboard_) {
        pending_start_with_steamos_ = !pending_start_with_steamos_;
        update_settings_ui();
    } else if (id == "native-fallback") {
        cancel();
        native_fallback_ = true;
    } else if (id == "auto-leveling") {
        pending_auto_leveling_ = !pending_auto_leveling_;
        update_settings_ui();
    } else if (id == "favorite") {
        const bool exists =
            std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                        [&](const Favorite& f) { return same_pair(f.selection, pending_); });
        if (exists) {
            std::erase_if(pending_favorites_,
                          [&](const Favorite& f) { return same_pair(f.selection, pending_); });
        } else {
            validate_selection(profiles_, pending_);
            if (pending_favorites_.size() >= 32) {
                throw std::runtime_error("At most 32 favorites can be saved.");
            }
            // IDs are metadata only; choose one that cannot collide with legacy favorites.
            int index = 1;
            std::string favorite_id;
            do {
                favorite_id = "favorite-" + std::to_string(index++);
            } while (std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                                 [&](const Favorite& f) { return f.id == favorite_id; }));
            pending_favorites_.push_back(
                {favorite_id, profiles_.languages.at(pending_.language).name, pending_});
        }
        update_settings_ui();
    }
}
bool App::replacement_ready() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    return options_.mode == "vr" && options_.input == "ei" && options_.start_enabled && backend_ready_ &&
           text_ready_ && !(language.input_method == "xkb" && language.keymap == "jp");
}
bool App::take_native_fallback() {
    return std::exchange(native_fallback_, false);
}
void App::close_panel() {
    set_interaction_active(false);
    if (resident_replacement()) {
        settings_open_ = false;
        dismiss_requested_ = true;
    } else {
        quit_ = true;
    }
}
bool App::take_dismissal() {
    return std::exchange(dismiss_requested_, false);
}
bool App::take_recenter() {
    const bool result = recenter_;
    recenter_ = false;
    return result;
}
} // namespace framekeyboard
