#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"
#include <utility>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <tuple>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
bool App::updating() const {
    return updates_ && updates_->prepared();
}
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
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
bool fits(const Profiles& profiles, const Selection& selection) {
    try {
        validate_selection_profiles(profiles, selection);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
// The language a newly chosen layout most likely serves: the current one if it
// fits, then a favorite pair, a variant of the same language (ja-flick ->
// ja-romaji, the layout's own one first), the layout's own language, English,
// and finally any that fits.
std::string likely_language(const Profiles& profiles, const std::vector<Favorite>& favorites,
                            const Selection& selection) {
    auto fit = [&](const std::string& language) {
        return profiles.languages.contains(language) &&
               fits(profiles, {selection.layout, language, selection.theme});
    };
    if (fit(selection.language)) {
        return selection.language;
    }
    for (const auto& favorite : favorites) {
        if (favorite.selection.layout == selection.layout && fit(favorite.selection.language)) {
            return favorite.selection.language;
        }
    }
    const auto& layout = profiles.layouts.at(selection.layout);
    auto family = [](const std::string& id) { return id.substr(0, id.find('-')); };
    if (family(layout.language) == family(selection.language) && fit(layout.language)) {
        return layout.language;
    }
    for (const auto& [id, language] : profiles.languages) {
        (void)language;
        if (family(id) == family(selection.language) && fit(id)) {
            return id;
        }
    }
    if (fit(layout.language)) {
        return layout.language;
    }
    if (fit("en-us")) {
        return "en-us";
    }
    for (const auto& [id, language] : profiles.languages) {
        (void)language;
        if (fit(id)) {
            return id;
        }
    }
    return selection.language; // Apply reports the validation error if none fit.
}
// Layout units a press must travel to become a flick, about 40% of a key's height.
constexpr double flick_distance = 24;
// A repeated tap within this time cycles the toggle kana instead of adding one.
constexpr double flick_toggle_seconds = 1.0;
// A swipe longer than this many samples (over 30 s of motion) is not a word,
// or its capture is stuck: it ends without typing.
constexpr std::size_t max_swipe_points = 2000;
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
      output_(sink), keyboard_(output_) {
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
    pending_swipe_typing_ = settings_.swipe_typing;
    prepare_swipe();
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
    if (options.mode == Mode::Vr && settings_errors.empty()) {
        settings_writer_.reconcile(settings_);
    }
}
std::vector<Control> App::controls() const {
    std::vector<Control> result;
    auto add = [&](ControlAction action, Control control) {
        control.action = std::move(action);
        result.push_back(std::move(control));
    };
    PanelView geometry;
    geometry.layout = &profiles_.layouts.at(settings_.active.layout);
    geometry.theme = &profiles_.themes.at(settings_.active.theme);
    geometry.language = &profiles_.languages.at(settings_.active.language);
    geometry.hide_numpad = settings_.hide_numpad;
    geometry.settings = settings_open_;
    const auto body = panel_case_bounds(geometry);
    auto append_move_handle = [&] {
        if (!settings_.pinned && !settings_.use_drag_to_move) {
            add({ControlKind::MoveHandle}, {"move-handle",
                                            "Move keyboard",
                                            {body.x + body.width / 2 - 220, panel_height + 12, 440, 60},
                                            dragging_,
                                            Icon::None,
                                            ControlStyle::MoveHandle});
        }
    };
    if (settings_open_) {
        add({ControlKind::Settings}, {"settings", "Back (Esc)", {18, 12, 60, 42}, false, Icon::Back});
        add({ControlKind::Apply}, {"apply", "Apply and save", {88, 12, 60, 42}, false, Icon::Apply});
        add({ControlKind::Reload}, {"reload", "Reload config", {158, 12, 60, 42}, false, Icon::Reload});
        add({ControlKind::Updates},
            {"updates", updates_ ? updates_->label() : "Check for updates", {550, 12, 500, 42}});
        if (settings_.replace_native_keyboard && native_session_) {
            add({ControlKind::NativeFallback},
                {"native-fallback", "Use Steam keyboard", {250, 12, 245, 42}});
        }
        if (resident_replacement()) {
            add({ControlKind::Quit},
                {"quit", "Quit Full Keyboard", {body.x + body.width - 268, 12, 250, 42}});
        }
        append_move_handle();
        return result;
    }
    const double x = body.x + 18;
    add({ControlKind::Settings}, {"settings", "Settings", {x, 12, 60, 42}, false, Icon::Settings});
    add({ControlKind::Recenter}, {"recenter", "Recenter", {x + 70, 12, 60, 42}, false, Icon::Recenter});
    add({ControlKind::Smaller},
        {"size-smaller", "Smaller keyboard", {x + 140, 12, 60, 42}, false, Icon::ScaleDown});
    add({ControlKind::Larger},
        {"size-larger", "Larger keyboard", {x + 210, 12, 60, 42}, false, Icon::ScaleUp});
    add({ControlKind::Pin}, {"pin",
                             settings_.pinned ? "Unpin keyboard" : "Pin keyboard",
                             {x + 280, 12, 60, 42},
                             false,
                             settings_.pinned ? Icon::PinOn : Icon::PinOff});
    if (!settings_.favorites.empty()) {
        add({ControlKind::LanguageCycle},
            {"language-cycle",
             language_short_name(profiles_.languages.at(settings_.active.language)),
             {x + 350, 12, 100, 42}});
    }
    add({ControlKind::Close}, {"close",
                               resident_replacement() ? "Hide keyboard" : "Close",
                               {body.x + body.width - 78, 12, 60, 42},
                               false,
                               Icon::Close});
    const auto ime_start = result.size();
    if (japanese()) {
        add({ControlKind::ImeToggle},
            {"ime-toggle", japanese_latin_ ? "A / あ" : "あ / A", {610, 12, 115, 42}});
        if (!japanese_latin_) {
            add({ControlKind::ImeHiragana}, {"ime-hiragana", "ひらがな", {735, 12, 120, 42}});
            add({ControlKind::ImeKatakana}, {"ime-katakana", "カタカナ", {865, 12, 120, 42}});
            add({ControlKind::ImeCommit}, {"ime-commit", "確定 / Commit", {995, 12, 180, 42}});
            add({ControlKind::ImeCancel}, {"ime-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = composition_.candidates();
            if (!candidates.empty()) {
                add({ControlKind::ImePrev}, {"ime-prev", "↑", {20, 148, 55, 42}});
                const int page = composition_.selected() / 5 * 5;
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    add({ControlKind::ImeCandidate, i},
                        {"ime-candidate-" + std::to_string(i),
                         std::to_string(i + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == composition_.selected()});
                }
                add({ControlKind::ImeNext}, {"ime-next", "↓", {1290, 148, 55, 42}});
                add({ControlKind::ImeSegmentPrev}, {"ime-segment-prev", "←", {1355, 148, 55, 42}});
                add({ControlKind::ImeSegmentNext}, {"ime-segment-next", "→", {1420, 148, 55, 42}});
                add({ControlKind::None}, {"ime-segment-label",
                                          std::to_string(composition_.active_segment() + 1) + "/" +
                                              std::to_string(composition_.segment_count()),
                                          {1485, 148, 90, 42}});
            } else {
                add({ControlKind::ImeConvert}, {"ime-convert", "変換 / Convert", {20, 148, 230, 42}});
            }
        }
    }
    if (cjk_) {
        add({ControlKind::CjkToggle},
            {"cjk-toggle",
             cjk_latin_ ? "A / native" : (cjk_->chinese() ? "中文 / A" : "한 / A"),
             {610, 12, 150, 42}});
        if (!cjk_latin_) {
            add({ControlKind::CjkCommit}, {"cjk-commit", "Commit", {995, 12, 180, 42}});
            add({ControlKind::CjkCancel}, {"cjk-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = cjk_->candidates();
            if (!candidates.empty()) {
                const int page = cjk_->selected() / 5 * 5;
                add({ControlKind::CjkPrev}, {"cjk-prev", "↑", {20, 148, 55, 42}});
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    add({ControlKind::CjkCandidate, i},
                        {"cjk-candidate-" + std::to_string(i),
                         std::to_string(i - page + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == cjk_->selected()});
                }
                add({ControlKind::CjkNext}, {"cjk-next", "↓", {1290, 148, 55, 42}});
            }
        }
    }
    for (std::size_t i = 0; i < swipe_candidates_.size(); ++i) {
        add({ControlKind::SwipeCandidate, static_cast<int>(i)},
            {"swipe-candidate-" + std::to_string(i),
             swipe_candidates_[i],
             {610.0 + static_cast<double>(i) * 180, 12, 170, 42},
             static_cast<int>(i) == swipe_selected_});
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
    auto field = [](ControlKind kind, SettingField value) {
        value.action.kind = kind;
        return value;
    };
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
    // Labels have no trailing colon: every row reads label left, control right.
    auto dropdown = [](std::string id, std::string label, std::string value,
                       std::vector<SettingChoice> list) {
        return SettingField{std::move(id), std::move(label), std::move(value), ControlStyle::Dropdown,
                            false,         std::move(list)};
    };
    auto checkbox = [](std::string id, std::string label, bool checked, bool enabled = true,
                       std::string note = {}) {
        return SettingField{
            std::move(id),  std::move(label), "", ControlStyle::Checkbox, checked, {}, enabled, {},
            std::move(note)};
    };
    const bool swipe = swipe_supported(pending_.language);
    // Languages the pending layout cannot type stay listed but dimmed.
    auto language_choices = choices(profiles_.languages);
    for (auto& choice : language_choices) {
        choice.enabled = fits(profiles_, {pending_.layout, choice.id, pending_.theme});
    }
    settings_ui_.set_cards(
        {{"languages",
          "Languages and Layouts",
          {field(ControlKind::ChooseLayout,
                 dropdown("layout", "Layout", pending_.layout, choices(profiles_.layouts))),
           field(ControlKind::ChooseLanguage,
                 dropdown("language", "Language", pending_.language, language_choices)),
           field(ControlKind::Favorite, checkbox("favorite", "Favorite", favorite)),
           field(ControlKind::SwipeTyping,
                 checkbox("swipe-typing", "Swipe typing", pending_swipe_typing_ && swipe, swipe,
                          "Powered by FUTO Swipe"))}},
         {"general",
          "General",
          {field(ControlKind::ChooseTheme,
                 dropdown("theme", "Theme", pending_.theme, choices(profiles_.themes))),
           field(ControlKind::HideNumpad, checkbox("hide-numpad", "Hide numpad", pending_hide_numpad_)),
           field(ControlKind::UseDragToMove,
                 checkbox("use-drag-to-move", "Use Drag to Move", pending_use_drag_to_move_)),
           field(ControlKind::AutoLeveling,
                 checkbox("auto-leveling", "Auto leveling", pending_auto_leveling_))}},
         {"experimental",
          "Experimental",
          {field(ControlKind::ReplaceNativeKeyboard,
                 checkbox("replace-native-keyboard", "Replace native keyboard",
                          pending_replace_native_keyboard_)),
           field(ControlKind::StartWithSteamos,
                 checkbox("start-with-steamos", "Start with SteamOS",
                          pending_start_with_steamos_ && pending_replace_native_keyboard_,
                          pending_replace_native_keyboard_))}}});
}
PanelView App::geometry() const {
    PanelView v;
    v.layout = &profiles_.layouts.at(settings_.active.layout);
    v.theme = &profiles_.themes.at(settings_.active.theme);
    v.language = &profiles_.languages.at(settings_.active.language);
    v.hide_numpad = settings_.hide_numpad;
    v.settings = settings_open_;
    v.composing = (japanese() && !japanese_latin_) || (cjk_ && !cjk_latin_);
    return v;
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
    if (settings_open_ && v.status.empty() && settings_ui_.open_field() == "language" &&
        std::any_of(profiles_.languages.begin(), profiles_.languages.end(), [&](const auto& item) {
            return !fits(profiles_, {pending_.layout, item.first, pending_.theme});
        })) {
        v.status = "Dimmed languages need another layout. Choose the layout first.";
    }
    // Image exports show the layout without the interactive preview's input notice.
    const bool image_export = options_.mode == Mode::Render || options_.mode == Mode::RenderSettings;
    if (v.status.empty() && !typing_ready_ && !image_export) {
        // External JIS sends physical keys to a system IME, which a replaced
        // Steam keyboard request cannot receive.
        v.status = options_.input == InputBackend::None ? "Preview only: this launch cannot type."
                   : native_session_ && external_jis()
                       ? "External JIS cannot type into this request. Choose another language."
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
    if (v.language->input_method == "japanese-flick") {
        // Flick keys show their tap kana even in Latin mode, where a press returns to kana.
        for (const auto& key : v.layout->keys) {
            if (const auto it = v.language->flick.find(key.action);
                key.action_kind == ActionKind::Flick && it != v.language->flick.end()) {
                v.key_labels[key.id] = it->second.tap.front();
            } else if (v.composing && !composition_.empty() && key.action == "Space") {
                v.key_labels[key.id] = composition_.converting() ? "次候補" : "変換";
            } else if (v.composing && !composition_.empty() && key.action == "Enter") {
                v.key_labels[key.id] = "確定";
            }
        }
        if (flick_track_) {
            const auto& key = v.language->flick.at(flick_track_->slot);
            auto tap = *flick_track_;
            tap.direction = FlickDirection::Tap;
            v.flick_preview =
                PanelView::FlickPreview{flick_track_->key,
                                        {flick_text(tap), key.left, key.up, key.right, key.down},
                                        static_cast<int>(flick_track_->direction)};
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
    } else if ((unicode_mode() || japanese() || cjk_) && options_.input != InputBackend::None &&
               !text_ready_) {
        v.status = "Text delivery unavailable. Waiting for the compositor connection to recover.";
    }
    if (swipe_track_ && swipe_track_->swiping) {
        v.swipe_trail = swipe_track_->trail;
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
    if (swipe_track_ && swipe_track_->pointer == pointer) {
        swipe_move(x, y);
    }
    if (flick_track_ && flick_track_->pointer == pointer) {
        flick_move(x, y);
    }
    if (settings_open_ && settings_ui_.move(pointer, x, y)) {
        pressed_controls_.clear();
        hovered_.clear();
        dirty = true;
        return false;
    }
    const auto v = view();
    HoverTarget target;
    for (auto it = v.controls.rbegin(); it != v.controls.rend(); ++it) {
        if (v.popup && it->style != ControlStyle::MenuItem &&
            it->action.kind != ControlKind::ScrollMenu) {
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
        const auto control =
            std::find_if(v.controls.rbegin(), v.controls.rend(),
                         [&](const Control& item) { return item.id == hovered_[pointer].id; });
        if (control != v.controls.rend()) {
            pressed_controls_[pointer] = {control->id, control->action};
        }
        return false;
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        // Taps and flicks type on release, so the other hand often presses
        // before this one lets go. Type the held one first to keep press order.
        if (flick_track_ && flick_track_->pointer != pointer) {
            finish_flick();
        }
        if (swipe_track_ && swipe_track_->pointer != pointer && !swipe_track_->swiping) {
            const auto track = std::move(*swipe_track_);
            swipe_track_.reset();
            type_swipe_tap(track);
        }
        if (key->action_kind == ActionKind::Flick) {
            return flick_down(pointer, *key, x, y, now);
        }
        const auto modifiers = keyboard_.modifiers();
        // Pinyin swipes feed the composer; its Latin mode types ordinary keys.
        // While a word list loads, letters type as without swipe: a swipe would
        // make the next key wait for the whole load on the VR thread.
        if (swipe_enabled() && swipe_->ready() && !(cjk_ && cjk_latin_) &&
            swipe_keyboard_->key_index.contains(key->id) && !keymap_->composing() &&
            std::none_of(modifiers.begin(), modifiers.end(), [](int code) {
                return code != key_code("ShiftLeft") && code != key_code("ShiftRight");
            })) {
            return swipe_down(pointer, *key, x, y, now, modifiers);
        }
        // Any other key ends the swiped word: deliver it first, then forget its alternatives.
        if (swipe_track_ && swipe_track_->pointer != pointer) {
            return false; // Only one pointer types while another is swiping.
        }
        finish_swipe(true);
        // A latching Shift or Caps Lock sends nothing, so the swiped word, its
        // alternatives and the space before the next word all stay valid.
        const bool latch_only =
            key->action_kind == ActionKind::Key &&
            ((key->sticky && is_modifier(key_code(key->action))) || key->action == "CapsLock");
        if (!latch_only) {
            discard_swipe();
            swipe_space_ = false;
        }
        const bool ime_local = japanese_key(*key, false, modifiers) || cjk_key(*key, false, modifiers);
        const bool text_local = !ime_local && text_key(*key, modifiers);
        const bool sends_text = text_local && key->action != "CapsLock" && key->action != "NumLock";
        const bool cancel_accent = unicode_mode() && keymap_->composing() &&
                                   (key->action == "Escape" || key->action == "Backspace");
        const bool retry_text = output_.retry_available() && key->action == "Enter";
        const bool local = ime_local || text_local || cancel_accent || retry_text;
        if (!backend_ready_ || keyboard_.pointer_pressed(pointer)) {
            return false;
        }
        if (!output_.admit((!local || sends_text) && !retry_text, now)) {
            update_output_status();
            dirty = true;
            return false;
        }
        const bool num_dependent = key->action == "NumpadDecimal" ||
                                   (key->action.starts_with("Numpad") && key->action.size() == 7 &&
                                    g_ascii_isdigit(key->action.back()));
        if (!unicode_mode() && num_dependent && output_.enabled()) {
            // Never guess a native target's lock state or blindly toggle it at
            // launch. Wait for feedback before forwarding a lock-dependent key.
            const auto target_num = output_.num_lock_state();
            if (target_num && native_num_requested_ == target_num) {
                native_num_requested_.reset();
            }
            if (native_num_requested_ || !target_num || *target_num != keyboard_.num()) {
                if (target_num && !native_num_requested_) {
                    output_.send(KEY_NUMLOCK, 1);
                    output_.send(KEY_NUMLOCK, 0);
                    native_num_requested_ = !*target_num;
                }
                status_ = "Waiting for target Num Lock. Press the keypad key again.";
                dirty = true;
                return false;
            }
        }
        const bool sends_action = key->action_kind == ActionKind::Shortcut ||
                                  !is_modifier(key_code(key->action)) || !key->sticky;
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
            if (!output_.admit(!local || sends_text, now)) {
                update_output_status();
                return false; // The preedit commit may have filled the final queue slot.
            }
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
                if (retry_text) {
                    retry_output();
                } else if (ime_local) {
                    japanese_key(*key, true, modifiers);
                    cjk_key(*key, true, modifiers);
                } else if (cancel_accent) {
                    keymap_->cancel_compose();
                    status_.clear();
                } else if (sends_text) {
                    const auto text = keymap_->compose(symbol);
                    if (options_.input == InputBackend::None) {
                        status_.clear();
                    } else {
                        output_.text_press(pointer, text, now);
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
        update_output_status();
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
    if (swipe_track_ && swipe_track_->pointer == pointer) {
        return swipe_up(pointer, x, y);
    }
    if (flick_track_ && flick_track_->pointer == pointer) {
        return flick_up(pointer, x, y);
    }
    const bool key_released = keyboard_.up(pointer);
    move(pointer, x, y);
    const auto it = pressed_controls_.find(pointer);
    if (it != pressed_controls_.end()) {
        const auto pressed = it->second;
        pressed_controls_.erase(it);
        if (hovered_[pointer] == HoverTarget{HoverTarget::Kind::Control, pressed.id}) {
            try {
                action(pressed.action);
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
    }
    dirty = true;
    return key_released;
}

void App::cancel_pointer(unsigned pointer) {
    if (swipe_track_ && swipe_track_->pointer == pointer) {
        swipe_track_.reset();
    }
    if (flick_track_ && flick_track_->pointer == pointer) {
        flick_track_.reset(); // A flick that left the panel types nothing.
    }
    // The laser may now click elsewhere in the target, so replacing is unsafe,
    // and the next swiped word may start a new field without a space. A word
    // still being decoded is discarded when ready (Pinyin still composes).
    discard_swipe();
    swipe_space_ = false;
    swipe_detached_ = swipe_waiting_;
    settings_ui_.cancel_pointer(pointer);
    pointer_positions_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    settings_ui_.cancel();
    pointer_positions_.clear();
    // An unfinished swipe or flick and an undelivered decode are discarded like preedit.
    flick_track_.reset();
    flick_toggle_.reset();
    swipe_track_.reset();
    swipe_waiting_ = false;
    ++swipe_request_;
    discard_swipe();
    swipe_space_ = false;
    if (discard_composition) {
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
            std::erase_if(pressed_controls_, [](const auto& entry) {
                return entry.second.action.kind == ControlKind::Updates;
            });
            dirty = true;
        }
        if (updates_->prepared()) {
            set_interaction_active(false);
            quit_ = true;
        }
    }
    update_save_status();
    if (reload_requested_ && !quit_ && settings_writer_.idle()) {
        try {
            reload();
        } catch (const std::exception& error) {
            status_ = error.what();
            dirty = true;
        }
    }
    refresh_typing();
    finish_swipe(false);
    dirty |= output_.tick(now);
    forget_failed_swipe();
    update_output_status();
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
        output_.set_enabled(false);
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
    pending_swipe_typing_ = settings_.swipe_typing;
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
    prepare_swipe();
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
        if (!result->startup_warning.empty()) {
            save_status_ = status_ = result->startup_warning;
            save_status_is_startup_ = true;
        } else if (result->error.empty()) {
            // Reconciliation does not save JSON, so it cannot resolve a prior
            // storage failure merely because Reload configured startup.
            if (!result->reconcile || save_status_is_startup_) {
                if (status_ == save_status_) {
                    status_.clear();
                }
                save_status_.clear();
                save_status_is_startup_ = false;
            }
        } else {
            save_status_is_startup_ = result->reconcile;
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
    if (!settings_writer_.idle()) {
        reload_requested_ = true;
        status_ = "Waiting for settings to finish saving.";
        dirty = true;
        return;
    }
    // Only App's thread queues snapshots, so none can appear between the idle
    // check and this read. A newer save while waiting also stays ahead of Reload.
    // Clear the request before preparation so a failed profile is not retried
    // every frame, and consume completion errors before reconciliation queues.
    reload_requested_ = false;
    update_save_status();
    allow_slow_work();
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
        settings_.swipe_typing = updated_settings.swipe_typing;
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
    pending_swipe_typing_ = settings_.swipe_typing;
    swipe_prepared_.clear(); // Reread an edited word list.
    prepare_swipe();
    rebuild_cycle_entries();
    update_settings_ui();
    settings_ui_.reset();
    refresh_typing();
    status_ = profiles_.errors.empty() ? save_status_ : profiles_.errors.front();
}
bool App::japanese() const {
    return profiles_.languages.at(settings_.active.language).input_method.starts_with("japanese-");
}
void App::refresh_typing() {
    const bool ready = output_.pump();
    const bool reset = output_.take_input_reset();
    const bool can_resume = output_.can_resume();
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
    text_transport_ready_ = output_.text_available();
    text_ready_ = (unicode_mode() || japanese() || cjk_) && text_transport_ready_;
    // Unicode mode owns character mapping. Only physical-only backends and
    // external JIS still rely on a declared receiving-session keymap.
    const bool matching = target_language_ && same_keymap(*target_language_, language) &&
                          (japanese() || cjk_ || options_.target_language == settings_.active.language);
    // Hidden panels and native handoffs deliberately pause interaction. That is
    // separate from an unavailable typing backend and must not show a warning.
    const bool typing_ready =
        options_.start_enabled && options_.mode == Mode::Vr &&
        (options_.input == InputBackend::Ei || options_.input == InputBackend::Uinput) &&
        (unicode_mode() ? text_ready_ : matching && (!(japanese() || cjk_) || text_ready_)) &&
        backend_ready_ && !(native_session_ && external_jis());
    dirty |= typing_ready_ != typing_ready;
    typing_ready_ = typing_ready;
    if (output_.enabled() && !(interaction_active_ && typing_ready_)) {
        // Transport loss ends key ownership, but unsent composition stays for
        // an explicit retry once text delivery is available again.
        cancel(false);
    }
    output_.set_enabled(interaction_active_ && typing_ready_);
}
void App::convert_japanese() {
    // Anthy loads its library and dictionary lazily on first conversion.
    allow_slow_work();
    composition_.convert();
}
void App::commit_japanese() {
    flick_toggle_.reset();
    const auto text = composition_.commit_text();
    if (text.empty()) {
        return;
    }
    if (options_.input == InputBackend::None) {
        composition_.cancel();
        status_ = "Preview: composition committed locally.";
    } else if (output_.commit_text(text)) {
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
    // → ends a toggle, so the same flick key can type its first kana again.
    if (code == "ArrowRight" && flick_toggle_ && composing) {
        if (execute) {
            flick_toggle_.reset();
        }
        return true;
    }
    if (execute) {
        flick_toggle_.reset();
    }
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
    return options_.input != InputBackend::Uinput && !external_jis;
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
void App::update_output_status() {
    if (auto status = output_.take_status()) {
        // The queue clears its own failure message once it drains again; keep
        // the notice that the swipe alternatives were removed until news.
        if (status->empty() && std::exchange(swipe_notice_, false)) {
            return;
        }
        swipe_notice_ = false;
        status_ = std::move(*status);
        dirty = true;
    }
}
bool App::retry_output() {
    const bool accepted = output_.retry();
    update_output_status();
    return accepted;
}
int App::native_code(const Key& key, const std::set<int>& mods) {
    if (!unicode_mode()) {
        return 0;
    }
    if (key.action_kind == ActionKind::Shortcut) {
        return output_.shortcut_code(key.action == "copy" ? XKB_KEY_c : XKB_KEY_v,
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
        return output_.shortcut_code(symbol, fallback);
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
    if (!text.empty() && (options_.input == InputBackend::None || output_.commit_text(text))) {
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
void App::action(const ControlAction& command) {
    const auto kind = command.kind;
    if (kind == ControlKind::Smaller || kind == ControlKind::Larger) {
        cancel();
        placement_actions_.push_back(kind == ControlKind::Smaller ? PlacementAction::Smaller
                                                                  : PlacementAction::Larger);
        return;
    }
    if (output_.text_pending() &&
        ((kind >= ControlKind::CjkToggle && kind <= ControlKind::CjkCandidate) ||
         (kind >= ControlKind::ImeToggle && kind <= ControlKind::ImeCandidate))) {
        if (kind != ControlKind::CjkCancel && kind != ControlKind::ImeCancel &&
            kind != ControlKind::ImeToggle) {
            return;
        }
        output_.cancel_text();
    }
    if ((kind >= ControlKind::CjkToggle && kind <= ControlKind::CjkCandidate) && cjk_) {
        if (kind == ControlKind::CjkToggle) {
            if (!cjk_->empty()) {
                commit_cjk();
                if (!cjk_->empty()) {
                    return;
                }
            }
            cjk_latin_ = !cjk_latin_;
        } else if (kind == ControlKind::CjkCommit) {
            if (!output_.retry_available()) {
                commit_cjk();
            } else {
                retry_output();
            }
        } else if (kind == ControlKind::CjkCancel) {
            cjk_->cancel();
        } else if (kind == ControlKind::CjkNext || kind == ControlKind::CjkPrev) {
            cjk_->cycle(kind == ControlKind::CjkNext ? 1 : -1);
        } else if (kind == ControlKind::CjkCandidate) {
            choose_cjk(command.index);
        }
        return;
    }
    if ((kind >= ControlKind::ImeToggle && kind <= ControlKind::ImeCandidate)) {
        if (kind == ControlKind::ImeToggle) {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        } else if (kind == ControlKind::ImeConvert) {
            convert_japanese();
        } else if (kind == ControlKind::ImeCommit) {
            if (!output_.retry_available()) {
                commit_japanese();
            } else {
                retry_output();
            }
        } else if (kind == ControlKind::ImeCancel) {
            composition_.cancel();
        } else if (kind == ControlKind::ImeHiragana || kind == ControlKind::ImeKatakana) {
            composition_.script(kind == ControlKind::ImeKatakana);
        } else if (kind == ControlKind::ImeNext || kind == ControlKind::ImePrev) {
            composition_.cycle(kind == ControlKind::ImeNext ? 1 : -1);
        } else if (kind == ControlKind::ImeSegmentNext || kind == ControlKind::ImeSegmentPrev) {
            composition_.segment(kind == ControlKind::ImeSegmentNext ? 1 : -1);
        } else if (kind == ControlKind::ImeCandidate) {
            composition_.choose(command.index);
        }
        return;
    }
    if (kind == ControlKind::Updates && settings_open_) {
        if (updates_) {
            updates_->click(monotonic_seconds());
        } else {
            report_status("Updates are available from the installed keyboard.");
        }
    } else if (kind == ControlKind::Close) {
        close_panel();
    } else if (kind == ControlKind::Quit && resident_replacement()) {
        set_interaction_active(false);
        quit_ = true;
    } else if (kind == ControlKind::Pin) {
        cancel();
        settings_.pinned = !settings_.pinned;
        status_.clear();
        save();
    } else if (kind == ControlKind::LanguageCycle) {
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
    } else if (kind == ControlKind::Settings) {
        if (settings_open_) {
            back();
        } else {
            show_settings();
        }
    } else if (kind == ControlKind::Recenter) {
        cancel();
        placement_actions_.clear();
        recenter_ = true;
    } else if (kind == ControlKind::Apply) {
        // Prepare/activate first. A failed candidate must leave all saved state intact.
        activate(pending_);
        settings_.favorites = pending_favorites_;
        settings_.hide_numpad = pending_hide_numpad_;
        settings_.use_drag_to_move = pending_use_drag_to_move_;
        settings_.auto_leveling = pending_auto_leveling_;
        settings_.replace_native_keyboard = pending_replace_native_keyboard_;
        settings_.start_with_steamos = pending_start_with_steamos_;
        settings_.swipe_typing = pending_swipe_typing_;
        prepare_swipe();
        default_ = settings_.active;
        persisted_active_ = settings_.active;
        startup_status_.clear();
        rebuild_cycle_entries();
        save();
        settings_open_ = false;
    } else if (kind == ControlKind::Reload) {
        reload();
    } else if (kind == ControlKind::ToggleDropdown) {
        pressed_controls_.clear();
        hovered_.clear();
        settings_ui_.toggle(command.value);
    } else if (kind == ControlKind::ChooseLayout || kind == ControlKind::ChooseLanguage ||
               kind == ControlKind::ChooseTheme) {
        pressed_controls_.clear();
        hovered_.clear();
        if (kind == ControlKind::ChooseLayout) {
            pending_.layout = command.value;
            pending_.language = likely_language(profiles_, pending_favorites_, pending_);
        } else if (kind == ControlKind::ChooseLanguage) {
            pending_.language = command.value;
            pending_.layout = compatible_layout(profiles_, pending_);
        } else if (kind == ControlKind::ChooseTheme) {
            pending_.theme = command.value;
        }
        settings_ui_.close_popup();
        update_settings_ui();
    } else if (kind == ControlKind::HideNumpad) {
        pending_hide_numpad_ = !pending_hide_numpad_;
        update_settings_ui();
    } else if (kind == ControlKind::UseDragToMove) {
        pending_use_drag_to_move_ = !pending_use_drag_to_move_;
        update_settings_ui();
    } else if (kind == ControlKind::ReplaceNativeKeyboard) {
        pending_replace_native_keyboard_ = !pending_replace_native_keyboard_;
        std::erase_if(pressed_controls_, [](const auto& entry) {
            return entry.second.action.kind == ControlKind::StartWithSteamos;
        });
        update_settings_ui();
    } else if (kind == ControlKind::StartWithSteamos && pending_replace_native_keyboard_) {
        pending_start_with_steamos_ = !pending_start_with_steamos_;
        update_settings_ui();
    } else if (kind == ControlKind::NativeFallback) {
        cancel();
        native_fallback_ = true;
    } else if (kind == ControlKind::SwipeTyping && swipe_supported(pending_.language)) {
        pending_swipe_typing_ = !pending_swipe_typing_;
        update_settings_ui();
    } else if (kind == ControlKind::SwipeCandidate) {
        choose_swipe(command.index);
    } else if (kind == ControlKind::AutoLeveling) {
        pending_auto_leveling_ = !pending_auto_leveling_;
        update_settings_ui();
    } else if (kind == ControlKind::Favorite) {
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
bool App::flick_down(unsigned pointer, const Key& key, double x, double y, double now) {
    const auto& language = profiles_.languages.at(settings_.active.language);
    if (!backend_ready_ || keyboard_.pointer_pressed(pointer) || flick_track_ ||
        !language.flick.contains(key.action)) {
        return false;
    }
    // Kana keys always compose; Latin mode only changes what the number row types.
    japanese_latin_ = false;
    // A local press shows the key and owns the pointer; nothing is sent.
    if (!keyboard_.down(pointer, key, now, true, 0)) {
        return false;
    }
    const auto [lx, ly] = renderer.layout_point(geometry(), x, y);
    flick_track_ = FlickTrack{pointer, key.id, key.action, lx, ly, now};
    dirty = true;
    return true;
}
void App::flick_move(double x, double y) {
    auto& track = *flick_track_;
    const auto [lx, ly] = renderer.layout_point(geometry(), x, y);
    const double dx = lx - track.x, dy = ly - track.y;
    auto direction = FlickDirection::Tap;
    if (std::hypot(dx, dy) >= flick_distance) {
        direction = std::abs(dx) > std::abs(dy) ? (dx < 0 ? FlickDirection::Left : FlickDirection::Right)
                                                : (dy < 0 ? FlickDirection::Up : FlickDirection::Down);
    }
    if (direction != track.direction) {
        track.direction = direction;
        dirty = true;
    }
}
std::string App::flick_text(const FlickTrack& track, bool* cycles) const {
    const auto& key = profiles_.languages.at(settings_.active.language).flick.at(track.slot);
    if (cycles) {
        *cycles = false;
    }
    switch (track.direction) {
    case FlickDirection::Left:
        return key.left;
    case FlickDirection::Up:
        return key.up;
    case FlickDirection::Right:
        return key.right;
    case FlickDirection::Down:
        return key.down;
    case FlickDirection::Tap:
        break;
    }
    // A quick repeated tap replaces the kana the previous tap typed, if it is
    // still the last one in the reading.
    if (flick_toggle_ && flick_toggle_->slot == track.slot && key.tap.size() > 1 &&
        track.pressed - flick_toggle_->at <= flick_toggle_seconds &&
        composition_.last_kana() == key.tap[flick_toggle_->index]) {
        if (cycles) {
            *cycles = true;
        }
        return key.tap[(flick_toggle_->index + 1) % key.tap.size()];
    }
    return key.tap.front();
}
bool App::flick_up(unsigned pointer, double x, double y) {
    finish_flick();
    move(pointer, x, y);
    dirty = true;
    return true;
}
void App::finish_flick() {
    const auto track = *flick_track_;
    flick_track_.reset();
    keyboard_.up(track.pointer);
    const auto& key = profiles_.languages.at(settings_.active.language).flick.at(track.slot);
    bool cycles = false;
    const auto text = flick_text(track, &cycles);
    try {
        if (cycles) {
            composition_.backspace();
        }
        flick_type(text);
        if (track.direction == FlickDirection::Tap && key.tap.size() > 1 && !composition_.converting()) {
            const auto index = cycles ? (flick_toggle_->index + 1) % key.tap.size() : 0;
            flick_toggle_ = FlickToggle{track.slot, index, track.pressed};
        } else {
            flick_toggle_.reset();
        }
    } catch (const std::exception& error) {
        flick_toggle_.reset();
        status_ = error.what();
    }
}
void App::flick_type(const std::string& text) {
    if (text.empty()) {
        return; // A direction this key does not use.
    }
    if (text == "゛" || text == "゜" || text == "小" || text == "゛゜小") {
        composition_.mark(text);
        return;
    }
    if (composition_.converting()) {
        // Like typing, a new kana first commits the chosen phrase.
        commit_japanese();
        if (!composition_.empty()) {
            return; // A failed commit keeps the phrase and types nothing.
        }
    }
    composition_.kana(text);
    status_.clear();
}
bool App::swipe_supported(const std::string& language_id) const {
    const auto language = profiles_.languages.find(language_id);
    if (language == profiles_.languages.end() || language->second.swipe_lexicon.empty() ||
        (language->second.input_method != "xkb" &&
         !language->second.input_method.starts_with("chinese-pinyin-")) ||
        language->second.keymap == "jp" || options_.input == InputBackend::Uinput) {
        return false; // Swipe words are Unicode text; physical-key modes cannot send them.
    }
    if (!options_.swipe_encoder && !swipe_engine_installed(options_.data_dir)) {
        return false;
    }
    return !swipe_lexicon_path(options_.data_dir, options_.config_dir, language->second.swipe_lexicon)
                .empty();
}
void App::prepare_swipe() {
    swipe_track_.reset();
    discard_swipe();
    if (!settings_.swipe_typing) {
        // Turning swipe off returns the word list's memory; the encoder stays.
        swipe_keyboard_.reset();
        swipe_prepared_.clear();
        if (swipe_) {
            swipe_->release();
        }
        return;
    }
    if (!swipe_supported(settings_.active.language)) {
        // The service keeps its word list, so returning to this language is quick.
        swipe_keyboard_.reset();
        return;
    }
    const auto& layout = profiles_.layouts.at(settings_.active.layout);
    const auto& language = profiles_.languages.at(settings_.active.language);
    const auto lexicon =
        swipe_lexicon_path(options_.data_dir, options_.config_dir, language.swipe_lexicon);
    const auto prepared = layout.id + '\n' + language.id + '\n' + lexicon.string();
    if (swipe_keyboard_ && prepared == swipe_prepared_) {
        return;
    }
    auto keyboard = build_swipe_keyboard(layout, *keymap_);
    if (!keyboard) {
        swipe_keyboard_.reset();
        return;
    }
    swipe_keyboard_ = std::make_shared<const SwipeKeyboard>(std::move(*keyboard));
    swipe_prepared_ = prepared;
    if (!swipe_) {
        auto factory = options_.swipe_encoder;
        if (!factory) {
            factory = [data_dir = options_.data_dir] {
                return load_swipe_encoder(swipe_model_path(data_dir));
            };
        }
        swipe_ = std::make_unique<SwipeService>(std::move(factory));
    }
    swipe_->prepare(swipe_keyboard_, lexicon);
}
bool App::swipe_down(unsigned pointer, const Key& key, double x, double y, double now,
                     const std::set<int>& mods) {
    if (!backend_ready_ || keyboard_.pointer_pressed(pointer) || swipe_track_) {
        return false;
    }
    finish_swipe(true);
    discard_swipe();
    // Pinyin letters only reach the local composer; they need no output slot.
    if (!output_.admit(!cjk_, now)) {
        update_output_status();
        dirty = true;
        return false;
    }
    SwipeTrack track;
    track.pointer = pointer;
    track.key = key.id;
    track.modifiers = mods;
    // Capture the tap's character before down() consumes a latched Shift.
    track.symbol = keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num());
    track.shifted = mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
    // A local press shows the key and owns the pointer; nothing is sent until release.
    if (!keyboard_.down(pointer, key, now, true, 0)) {
        return false;
    }
    const auto [lx, ly] = renderer.layout_point(geometry(), x, y);
    track.points.push_back({lx, ly, monotonic_seconds()});
    track.trail.emplace_back(x, y);
    swipe_track_ = std::move(track);
    dirty = true;
    return true;
}
void App::swipe_move(double x, double y) {
    auto& track = *swipe_track_;
    const auto [lx, ly] = renderer.layout_point(geometry(), x, y);
    const double now = monotonic_seconds();
    // A pointer at rest keeps reporting its position. Keep only when it
    // arrived and the latest time: the decoder resamples by time, so it sees
    // the same pause, and history stays bounded however long the rest.
    const auto n = track.points.size();
    auto near = [](const SwipePoint& a, double bx, double by) {
        return std::hypot(a.x - bx, a.y - by) < .5;
    };
    if (n >= 2 && near(track.points[n - 1], lx, ly) &&
        near(track.points[n - 2], track.points[n - 1].x, track.points[n - 1].y)) {
        track.points.back().t = now;
        return;
    }
    if (n >= max_swipe_points) {
        swipe_track_.reset();
        status_ = "Swipe was too long and was cancelled.";
        dirty = true;
        return;
    }
    track.points.push_back({lx, ly, now});
    track.trail.emplace_back(x, y);
    const auto& start = track.points.front();
    if (!track.swiping && std::hypot(lx - start.x, ly - start.y) > swipe_keyboard_->key_width / 2) {
        // Leaving the start key turns the press into a swipe. Release only the
        // local visual press; it never sent anything.
        track.swiping = true;
        keyboard_.cancel_pointer(track.pointer);
    }
    // Every point feeds the decoder, but the trail only needs repainting once
    // it has visibly moved: about 2 px since the last painted point.
    if (track.swiping &&
        (track.trail.size() < 2 || track.painted >= track.trail.size() ||
         std::hypot(x - track.trail[track.painted].first, y - track.trail[track.painted].second) >= 2)) {
        track.painted = track.trail.size() - 1;
        dirty = true;
    }
}
bool App::swipe_up(unsigned pointer, double x, double y) {
    auto track = std::move(*swipe_track_);
    swipe_track_.reset();
    if (!track.swiping) {
        type_swipe_tap(track);
    } else {
        const auto [lx, ly] = renderer.layout_point(geometry(), x, y);
        track.points.push_back({lx, ly, monotonic_seconds()});
        swipe_shifted_ = track.shifted;
        swipe_caps_ = keyboard_.caps();
        swipe_->decode(++swipe_request_, std::move(track.points));
        swipe_waiting_ = true;
        swipe_detached_ = false;
    }
    move(pointer, x, y);
    dirty = true;
    return true;
}
void App::type_swipe_tap(const SwipeTrack& track) {
    keyboard_.up(track.pointer);
    if (cjk_) {
        // A Pinyin tap types into the composer, as it does without swipe.
        const auto& keys = profiles_.layouts.at(settings_.active.layout).keys;
        const auto key =
            std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return k.id == track.key; });
        if (key != keys.end()) {
            try {
                cjk_key(*key, true, track.modifiers);
            } catch (const std::exception& error) {
                status_ = error.what(); // As for presses: the composition is full.
            }
        }
    } else {
        const auto text = keymap_->compose(track.symbol);
        if (!text.empty() && options_.input != InputBackend::None && !output_.commit_text(text)) {
            status_ = "Text was not sent. Release other keys and check the target/input connection.";
        }
        swipe_space_ = false;
    }
    dirty = true;
}
void App::finish_swipe(bool wait) {
    if (!swipe_waiting_ || !swipe_) {
        return;
    }
    const auto result = wait ? swipe_->wait_result() : swipe_->take_result();
    if (!result) {
        if (wait) {
            swipe_waiting_ = false;
        }
        return;
    }
    if (result->id != swipe_request_) {
        return; // A cancelled request; the current one is still outstanding.
    }
    swipe_waiting_ = false;
    apply_swipe(*result);
}
void App::apply_swipe(const SwipeService::Result& result) {
    dirty = true;
    if (!result.error.empty()) {
        status_ = "Swipe typing unavailable: " + result.error;
        return;
    }
    if (result.words.empty()) {
        status_ = "No word matched the swipe.";
        return;
    }
    if (cjk_) {
        // The decoded Pinyin joins the composition; its own candidate row then
        // offers the characters. Separators only matter after a syllable. A
        // word that does not fit is removed again, so it goes in whole or not at all.
        const auto before = cjk_->preedit();
        std::size_t typed = 0;
        try {
            for (const char c : result.words.front()) {
                if (c != '\'' || !cjk_->empty()) {
                    cjk_->type(c);
                    ++typed;
                }
            }
        } catch (const std::exception& error) {
            for (std::size_t i = 0; i <= typed && cjk_->preedit() != before; ++i) {
                cjk_->backspace();
            }
            status_ = error.what();
            return;
        }
        status_.clear();
        return;
    }
    if (std::exchange(swipe_detached_, false)) {
        // The laser left before the word was ready; the target's cursor may
        // have moved since, so the word is not typed at all.
        status_ = "Swipe discarded: the laser left the keyboard before the word was ready.";
        return;
    }
    swipe_candidates_.clear();
    for (const auto& word : result.words) {
        std::string cased = word;
        if (swipe_caps_) {
            auto* upper = g_utf8_strup(word.c_str(), -1);
            cased = upper;
            g_free(upper);
        } else if (swipe_shifted_) {
            char first[8]{};
            g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(word.c_str())), first);
            cased = first + std::string(g_utf8_next_char(word.c_str()));
        }
        // Case variants of one word (help/Help) are a single choice; keep the better one.
        auto* folded = g_utf8_casefold(cased.c_str(), -1);
        const bool duplicate = std::any_of(swipe_candidates_.begin(), swipe_candidates_.end(),
                                           [&](const std::string& other) {
                                               auto* other_folded = g_utf8_casefold(other.c_str(), -1);
                                               const bool same = g_strcmp0(folded, other_folded) == 0;
                                               g_free(other_folded);
                                               return same;
                                           });
        g_free(folded);
        if (!duplicate) {
            swipe_candidates_.push_back(std::move(cased));
        }
    }
    swipe_selected_ = 0;
    const auto text = (swipe_space_ ? " " : "") + swipe_candidates_.front();
    if (options_.input != InputBackend::None && !(output_.admit(true) && output_.commit_text(text))) {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
        swipe_candidates_.clear();
        return;
    }
    status_.clear();
    swipe_committed_ = swipe_candidates_.front();
    swipe_failures_ = output_.failures();
    swipe_space_ = true;
}
void App::forget_failed_swipe() {
    // The alternatives replace the word this keyboard believes it typed. Once
    // any delivery since then failed or timed out, that word may not be in
    // the target, and replacing by its length could delete text before it.
    if (!swipe_candidates_.empty() && output_.failures() != swipe_failures_) {
        discard_swipe();
        status_ = "Text was not sent, so the swipe alternatives were removed. Check the text.";
        swipe_notice_ = true;
        dirty = true;
    }
}
void App::choose_swipe(int index) {
    // Settle earlier deliveries before trusting the typed word: admission can
    // discover that one failed, and it then clears the failure for new input.
    const bool admitted = options_.input == InputBackend::None || output_.admit(true);
    forget_failed_swipe();
    if (index < 0 || index >= static_cast<int>(swipe_candidates_.size()) || index == swipe_selected_) {
        return;
    }
    // A copy: refusing below clears the alternatives.
    const std::string word = swipe_candidates_[static_cast<std::size_t>(index)];
    if (options_.input != InputBackend::None) {
        if (!admitted) {
            update_output_status(); // Busy or full; the alternatives stay valid.
            return;
        }
        auto refuse = [&] {
            status_ = "Text was not sent. Release other keys and check the target/input connection.";
            discard_swipe();
        };
        // Replace exactly the characters this keyboard just sent.
        const auto count = static_cast<std::size_t>(g_utf8_strlen(swipe_committed_.c_str(), -1));
        const auto failures = output_.failures();
        if (output_.replaces_text()) {
            // Gamescope types text through a temporary keymap. Separate libei
            // Backspace keys could interleave with it and type wrong characters,
            // so deletion travels in the same ordered text request.
            if (!output_.replace_text(count, word)) {
                refuse();
                return;
            }
            swipe_committed_ = word;
            swipe_selected_ = index;
            dirty = true;
            forget_failed_swipe(); // A synchronous rejection already counts.
            return;
        }
        // Steam fields acknowledge each Backspace before the text. Each is one
        // queue entry; refuse rather than leave a partial deletion.
        if (!output_.can_queue(count + 1)) {
            status_ = "Wait for typing to finish, then choose again.";
            return;
        }
        // One sequence: separate presses would each be admitted on their own,
        // and one that found an earlier failure would clear it and still go out.
        if (!output_.taps_then_text(KEY_BACKSPACE, count, word) || output_.failures() != failures) {
            refuse();
            return;
        }
    }
    swipe_committed_ = word;
    swipe_selected_ = index;
    dirty = true;
}
void App::discard_swipe() {
    if (!swipe_candidates_.empty()) {
        dirty = true;
    }
    swipe_candidates_.clear();
    swipe_committed_.clear();
}
bool App::replacement_ready() const {
    // Readiness follows the connections, not the profile. A profile that cannot
    // type into the request keeps the panel open with a notice; handing the
    // request back to Steam's keyboard would look like a crash.
    return options_.mode == Mode::Vr && options_.input == InputBackend::Ei && options_.start_enabled &&
           backend_ready_ && text_transport_ready_;
}
const char* App::replacement_block() const {
    if (!backend_ready_) {
        return "input connection unavailable";
    }
    if (!text_transport_ready_) {
        return "text delivery unavailable";
    }
    return "launch options";
}
bool App::external_jis() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    return language.input_method == "xkb" && language.keymap == "jp";
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
