#include "framekeyboard/app.hpp"
#include "framekeyboard/text_input.hpp"
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
      output_(sink), text_output_(options.text_output ? options.text_output : &output_),
      keyboard_(output_) {
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
    pending_emoji_close_on_select_ = settings_.emoji_close_on_select;
    pending_dictation_send_gap_ = settings_.dictation_send_gap;
    pending_dictation_send_paste_ = settings_.dictation_send_paste;
    pending_dictation_ = settings_.dictation;
    pending_dictation_server_ = settings_.dictation_server;
    emoji_preferences_ = emoji_saved_ = load_emoji_preferences(options_.config_dir / "emoji.json");
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
App::~App() {
    save_emoji(); // Reports failure only in a status nobody will see; never throws.
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
                                            dragging_ && !resizing_,
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
    // One row: Settings, Recenter, Pin, passthrough, language. Compact:
    // Settings, Recenter, Pin, language, with passthrough below Settings.
    const bool compact = compact_toolbar(geometry);
    const double x = body.x + 18;
    auto slot = [&](int row_one, int compact_column, int compact_row) {
        return compact ? std::pair{x + compact_column * 70.0, 12.0 + compact_row * 50}
                       : std::pair{x + row_one * 70.0, 12.0};
    };
    auto button = [&](int row_one, int compact_column, int compact_row, double width = 60) {
        const auto [left, top] = slot(row_one, compact_column, compact_row);
        return Rect{left, top, width, 42};
    };
    add({ControlKind::Settings}, {"settings", "Settings", button(0, 0, 0), false, Icon::Settings});
    // Opening Settings pauses dictation; it waits only while Send runs. The
    // language button waits while dictation listens.
    const bool listening = dictation_open_ && (capturing_ || send_wait_since_);
    result.back().enabled = !(dictation_open_ && (sending_ || send_wait_since_));
    add({ControlKind::Recenter}, {"recenter", "Recenter", button(1, 1, 0), false, Icon::Recenter});
    add({ControlKind::Pin}, {"pin", settings_.pinned ? "Unpin keyboard" : "Pin keyboard",
                             button(2, 2, 0), false, settings_.pinned ? Icon::PinOn : Icon::PinOff});
    add({ControlKind::Passthrough},
        {"passthrough", passthrough() ? "Close with the dashboard" : "Stay open over apps",
         button(3, 0, 1), passthrough(), passthrough() ? Icon::PassthroughOn : Icon::PassthroughOff});
    if (!settings_.favorites.empty()) {
        add({ControlKind::LanguageCycle},
            {"language-cycle", language_short_name(profiles_.languages.at(settings_.active.language)),
             button(4, 3, 0, 100)});
        // The picker belongs to the language it was opened from.
        result.back().enabled = !emoji_open_ && !listening;
    }
    // Where the first row's fixed buttons end; suggestions start after it.
    double toolbar_end = x;
    for (const auto& control : result) {
        if (control.bounds.y == 12) {
            toolbar_end = std::max(toolbar_end, control.bounds.x + control.bounds.width);
        }
    }
    add({ControlKind::Close}, {"close",
                               resident_replacement() ? "Hide keyboard" : "Close",
                               {body.x + body.width - 78, 12, 60, 42},
                               false,
                               Icon::Close});
    if (dictation_open_ && confirm_discard_ && !settings_open_) {
        // The answers sit at the right of the band the question shows in.
        const auto editor = panel_rect(geometry, dictation_layout_.editor);
        const double y = editor.y + editor.height - dictation_notice_height + 5;
        const double right = editor.x + editor.width - 12;
        add({ControlKind::DictationKeep}, {"dictation-keep", "Keep", {right - 110, y, 110, 36}});
        add({ControlKind::DictationDiscard},
            {"dictation-discard", "Discard", {right - 230, y, 110, 36}});
    }
    const auto ime_start = result.size();
    if (japanese() && !emoji_open_) {
        add({ControlKind::ImeToggle},
            {"ime-toggle", japanese_latin_ ? "A / あ" : "あ / A", {680, 12, 115, 42}});
        if (!japanese_latin_) {
            add({ControlKind::ImeHiragana}, {"ime-hiragana", "ひらがな", {805, 12, 120, 42}});
            add({ControlKind::ImeKatakana}, {"ime-katakana", "カタカナ", {935, 12, 120, 42}});
            add({ControlKind::ImeCommit}, {"ime-commit", "確定 / Commit", {1065, 12, 180, 42}});
            add({ControlKind::ImeCancel}, {"ime-cancel", "Cancel", {1255, 12, 110, 42}});
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
    if (cjk_ && !emoji_open_) {
        add({ControlKind::CjkToggle},
            {"cjk-toggle",
             cjk_latin_ ? "A / native" : (cjk_->chinese() ? "中文 / A" : "한 / A"),
             {680, 12, 150, 42}});
        if (!cjk_latin_) {
            add({ControlKind::CjkCommit}, {"cjk-commit", "Commit", {1065, 12, 180, 42}});
            add({ControlKind::CjkCancel}, {"cjk-cancel", "Cancel", {1255, 12, 110, 42}});
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
    // Composition controls use the available case width; the main toolbar keeps
    // its button sizes when the numpad is hidden.
    const double scale = body.width / panel_width;
    for (std::size_t i = ime_start; i < result.size(); ++i) {
        result[i].bounds.x = body.x + result[i].bounds.x * scale;
        result[i].bounds.width *= scale;
    }
    // Swipe suggestions scale the same way, but never start before the end of
    // the first toolbar row or reach the close button; many share the room.
    if (!swipe_candidates_.empty()) {
        const double start = std::max(body.x + 610 * scale, toolbar_end + 10);
        const double end = body.x + body.width - 78 - 10;
        const double pitch = std::min(180 * scale, (end - start + 10 * scale) /
                                                       static_cast<double>(swipe_candidates_.size()));
        for (std::size_t i = 0; i < swipe_candidates_.size(); ++i) {
            add({ControlKind::SwipeCandidate, static_cast<int>(i)},
                {"swipe-candidate-" + std::to_string(i),
                 swipe_candidates_[i],
                 {start + static_cast<double>(i) * pitch, 12, pitch - 10 * scale, 42},
                 static_cast<int>(i) == swipe_selected_});
        }
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
    // Send types one character at a time, slower keeping the order under
    // load, or pastes through the clipboard.
    auto send_gap = field(
        ControlKind::ChooseSendGap,
        dropdown("send-gap", "Send",
                 pending_dictation_send_paste_ ? "paste" : std::to_string(pending_dictation_send_gap_),
                 {{"20", "Type, 20 ms (default)"},
                  {"30", "Type, 30 ms"},
                  {"40", "Type, 40 ms"},
                  {"paste", "Paste"}}));
    // Dictation (docs/dictation.md, Settings): the mode, the user's own
    // server, a connection check and how Send delivers. Self-hosted only;
    // on-device recognition joins the mode list in M6.
    auto server =
        field(ControlKind::DictationServer,
              SettingField{"dictation-server", "Server",
                           pending_dictation_server_.empty() ? "Not set" : pending_dictation_server_,
                           ControlStyle::TextField});
    auto test = field(ControlKind::DictationTest,
                      SettingField{"dictation-test", "Connection", "Test", ControlStyle::Button});
    test.note = dictation_test_.empty()
                    ? (pending_dictation_server_.empty() ? "No server set" : "Not tested")
                    : dictation_test_;
    test.enabled = !pending_dictation_server_.empty();
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
                 checkbox("auto-leveling", "Auto leveling", pending_auto_leveling_)),
           field(ControlKind::EmojiCloseOnSelect,
                 checkbox("emoji-close-on-select", "Close emoji picker after choosing",
                          pending_emoji_close_on_select_))}},
         {"dictation",
          "Dictation",
          {field(ControlKind::ChooseDictation, dropdown("dictation", "Dictation", pending_dictation_,
                                                        {{"off", "Off"}, {"server", "Your server"}})),
           server, test, send_gap}},
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
    if (emoji_open_) {
        v.layout = &emoji_layout_.layout;
        v.composing = false;
        v.emoji.emplace().width = emoji_width_;
    }
    v.dictation_available = dictation_available();
    if (dictation_open_) {
        v.layout = &dictation_layout_.layout;
        v.composing = false;
        set_dictation_view(v);
    }
    return v;
}
PanelView App::build_view(bool picker) const {
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
    v.status = resizing_   ? "Release trigger to keep this size"
               : dragging_ ? (settings_.use_drag_to_move ? "Release grab to place keyboard"
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
    const bool image_export = options_.mode == Mode::Render || options_.mode == Mode::RenderSettings ||
                              options_.mode == Mode::RenderEmoji ||
                              options_.mode == Mode::RenderDictation;
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
    if (emoji_open_) {
        // The picker has its own keys; composition was committed on opening.
        v.layout = &emoji_layout_.layout;
        v.composing = false;
        v.preedit.clear();
        v.key_labels.clear();
        v.flick_preview.reset();
        if (picker) {
            v.emoji = emoji_.view();
            v.emoji_glyphs = emoji_glyphs_.get();
            if (emoji_glyphs_) {
                auto mark = [&](std::vector<EmojiPickerView::Cell>& cells) {
                    for (auto& cell : cells) {
                        cell.ready = !cell.clear && emoji_glyphs_->image(cell.text);
                    }
                };
                mark(v.emoji->cells);
                if (v.emoji->popup) {
                    mark(v.emoji->popup->choices);
                }
            }
        } else {
            v.emoji.emplace(); // Only its width, which sets the case.
        }
        v.emoji->width = emoji_width_;
    } else if (!emoji_available()) {
        for (const auto& key : v.layout->keys) {
            if (key.action_kind == ActionKind::Emoji) {
                v.disabled_keys.insert(key.id);
            }
        }
    }
    v.dictation_available = dictation_available();
    v.dictation_capturing = capturing_;
    v.dictation_levels = mic_levels_;
    v.dictation_level_since = mic_level_since_;
    if (dictation_open_) {
        // The mode has its own keys; swipe and composition are off in it.
        v.layout = &dictation_layout_.layout;
        v.composing = false;
        v.preedit.clear();
        v.key_labels.clear();
        v.flick_preview.reset();
        v.swipe_trail.clear();
        set_dictation_view(v);
        if (!can_send()) {
            v.disabled_keys.insert("Dictation-send");
        }
        if (sending_ || confirm_discard_ || !editor_.can_undo()) {
            v.disabled_keys.insert("Dictation-undo");
        }
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
    if (dictation_open_ && !settings_open_ && interaction_active_ && !dragging_ &&
        pointer_positions_.contains(pointer) && std::isfinite(dy)) {
        const auto [x, y] = pointer_positions_.at(pointer);
        const auto v = geometry();
        if (v.dictation && v.dictation->bounds.contains(x, y)) {
            // One step is one line, as in a desktop editor.
            const double old = editor_scroll_;
            editor_scroll_ =
                std::clamp(editor_scroll_ + dy * editor_line_height(), 0.0, editor_scroll_limit());
            dirty |= editor_scroll_ != old;
            return editor_scroll_ != old;
        }
    }
    if (emoji_open_ && interaction_active_ && !dragging_ && pointer_positions_.contains(pointer)) {
        const auto [x, y] = pointer_positions_.at(pointer);
        // The grid only scrolls sideways; an up/down stick scrolls it too.
        if (!emoji_.scroll(x, y, std::abs(dx) > std::abs(dy) ? dx : dy)) {
            return false;
        }
        dirty = true;
        return true;
    }
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
    if (editor_drag_ && editor_drag_->pointer == pointer) {
        // Past a small dead zone the text follows the laser.
        if (!editor_drag_->scrolling && std::abs(y - editor_drag_->y) > 8) {
            editor_drag_->scrolling = true;
        }
        if (editor_drag_->scrolling) {
            const double scroll =
                std::clamp(editor_drag_->scroll - (y - editor_drag_->y), 0.0, editor_scroll_limit());
            if (scroll != editor_scroll_) {
                editor_scroll_ = scroll;
                dirty = true;
            }
        }
        return false;
    }
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
    // The picker gives no haptics: buzzing every emoji under a sweeping laser
    // turns into a constant rattle.
    if (emoji_open_) {
        dirty |= emoji_.move(pointer, x, y, monotonic_seconds()).redraw;
        if (emoji_.pressed(pointer)) {
            return false; // Scrolling or choosing a tone: keys under the laser don't count.
        }
    }
    const auto v = input_view();
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
               !keyboard_.pointer_pressed(pointer) && !v.disabled_keys.contains(target.id);
    }
    return false;
}

bool App::down(unsigned pointer, double x, double y, double now) {
    if (dragging_ || !interaction_active_) {
        return false;
    }
    move(pointer, x, y);
    const auto v = input_view();
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
    if (emoji_open_ && emoji_.contains(x, y)) {
        dirty |= emoji_.down(pointer, x, y, now).redraw;
        return false;
    }
    if (dictation_open_ && !settings_open_ && v.dictation && v.dictation->bounds.contains(x, y)) {
        if (!sending_ && !confirm_discard_ && !editor_drag_) {
            editor_drag_ = EditorDrag{pointer, x, y, editor_scroll_};
        }
        return false;
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        if (dictation_open_) {
            return dictation_down(pointer, *key, now);
        }
        if (key->action_kind == ActionKind::Dictation) {
            // The mic key opens dictation mode on release, like a toolbar button.
            if (dictation_key_pointer_ || keyboard_.pointer_pressed(pointer) ||
                !keyboard_.down(pointer, *key, now, true, 0)) {
                return false;
            }
            dictation_key_pointer_ = pointer;
            dirty = true;
            return true;
        }
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
        if (key->action_kind == ActionKind::Emoji) {
            // A local press that acts on release, like a toolbar button.
            if (emoji_key_pointer_ || v.disabled_keys.contains(key->id) ||
                keyboard_.pointer_pressed(pointer) || !keyboard_.down(pointer, *key, now, true, 0)) {
                return false;
            }
            emoji_key_pointer_ = pointer;
            dirty = true;
            return true;
        }
        const auto modifiers = keyboard_.modifiers();
        // Pinyin swipes feed the composer; its Latin mode types ordinary keys.
        // While a word list loads, letters type as without swipe: a swipe would
        // make the next key wait for the whole load on the VR thread.
        if (!emoji_open_ && swipe_enabled() && swipe_->ready() && !(cjk_ && cjk_latin_) &&
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
                        text_output_->text_press(pointer, text, now);
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
    if (editor_drag_ && editor_drag_->pointer == pointer) {
        const auto drag = *std::exchange(editor_drag_, std::nullopt);
        // A tap places the cursor, unless the editor is locked: listening,
        // sending, asking or showing a phrase.
        if (!drag.scrolling && dictation_open_ && !capturing_ && !sending_ && !confirm_discard_ &&
            !reveal_ && !send_wait_since_) {
            const auto v = geometry();
            const auto area = editor_text_area(v);
            editor_.set_cursor(editor_index_at(v, drag.x - area.x, drag.y - area.y + drag.scroll));
            editor_goal_x_.reset();
            follow_cursor();
            dirty = true;
        }
        return false;
    }
    if (settings_open_ && settings_ui_.up(pointer)) {
        pressed_controls_.erase(pointer);
        dirty = true;
        return false;
    }
    if (emoji_open_ && emoji_.pressed(pointer)) {
        const auto event = emoji_.up(pointer, x, y, monotonic_seconds());
        apply_emoji_event(event);
        move(pointer, x, y);
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
    if (editor_repeat_ && editor_repeat_->pointer == pointer) {
        editor_repeat_.reset();
    }
    if (dictation_key_pointer_ == pointer) {
        dictation_key_pointer_.reset();
        const auto& hovered = hovered_[pointer];
        const auto view = input_view();
        const auto key =
            std::find_if(view.layout->keys.begin(), view.layout->keys.end(), [&](const Key& k) {
                return k.id == hovered.id && k.action_kind == ActionKind::Dictation;
            });
        if (hovered.kind == HoverTarget::Kind::Key && key != view.layout->keys.end() &&
            !view.disabled_keys.contains(key->id)) {
            dictation_key_up(*key);
        }
    }
    if (emoji_key_pointer_ == pointer) {
        emoji_key_pointer_.reset();
        const auto& hovered = hovered_[pointer];
        if (hovered.kind == HoverTarget::Kind::Key && emoji_key(hovered.id)) {
            if (emoji_open_) {
                close_emoji();
            } else {
                open_emoji();
            }
        }
    }
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
    if (editor_drag_ && editor_drag_->pointer == pointer) {
        editor_drag_.reset();
    }
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
    emoji_.cancel_pointer(pointer);
    if (emoji_key_pointer_ == pointer) {
        emoji_key_pointer_.reset();
    }
    if (dictation_key_pointer_ == pointer) {
        dictation_key_pointer_.reset();
    }
    if (editor_repeat_ && editor_repeat_->pointer == pointer) {
        editor_repeat_.reset();
    }
    pointer_positions_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    settings_ui_.cancel();
    emoji_.cancel();
    emoji_key_pointer_.reset();
    dictation_key_pointer_.reset();
    editor_repeat_.reset();
    editor_drag_.reset();
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
            end_dictation_for_update();
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
    update_emoji(now);
    if (dictation_open_) {
        if (!dictation_available()) {
            close_dictation(); // the profile or settings no longer allow it
        } else {
            refresh_dictation_layout();
        }
    }
    if (editor_repeat_ && now >= editor_repeat_->next) {
        if (!dictation_open_ || !keyboard_.pointer_pressed(editor_repeat_->pointer)) {
            editor_repeat_.reset();
        } else {
            if (editor_repeat_->text.empty()) {
                run_editor_command(editor_repeat_->command);
            } else {
                editor_.type(editor_repeat_->text);
                follow_cursor();
            }
            // Repeats never catch up after a stall, like the output queue's.
            editor_repeat_->next = now + .04;
            dirty = true;
        }
    }
    finish_swipe(false);
    dirty |= output_.tick(now);
    dirty |= tick_dictation(now);
    dirty |= tick_send(now);
    forget_failed_swipe();
    update_output_status();
    return dirty || renderer.animating();
}
bool App::paint(double now) {
    dirty = false;
    return renderer.paint(view(), now);
}
void App::set_interaction_active(bool active) {
    if (active == interaction_active_) {
        return;
    }
    // Release through the still-open gate; disabling first would strand held
    // modifiers in the compositor. Also discard preedit and queued UI clicks.
    // The keyboard always reopens on its main layout.
    close_emoji();
    cancel();
    interaction_active_ = active;
    if (active) {
        refresh_typing();
    } else {
        output_.set_enabled(false);
    }
}
void App::set_dragging(bool dragging, bool resizing) {
    if (dragging && settings_.pinned) {
        return;
    }
    if (dragging_ == dragging) {
        return;
    }
    cancel();
    dragging_ = dragging;
    resizing_ = dragging && resizing;
    status_.clear();
}
void App::report_status(const std::string& message) {
    status_ = message;
    dirty = true;
}
void App::show_settings() {
    close_emoji();   // Settings returns to the keyboard, not the picker.
    pause_capture(); // Dictation pauses; its phrases still arrive in the editor.
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
    pending_emoji_close_on_select_ = settings_.emoji_close_on_select;
    pending_dictation_send_gap_ = settings_.dictation_send_gap;
    pending_dictation_send_paste_ = settings_.dictation_send_paste;
    pending_dictation_ = settings_.dictation;
    pending_dictation_server_ = settings_.dictation_server;
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
    close_emoji();
    cancel();
    settings_open_ = false;
    recenter_ = true;
    status_.clear();
}
void App::activate(Selection selection) {
    allow_slow_work();
    validate_selection_profiles(profiles_, selection);
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    auto cjk = prepare_cjk(profiles_.languages.at(selection.language));
    close_emoji();
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
    close_emoji(); // Its layout comes from profiles about to be replaced.
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
        settings_.emoji_close_on_select = updated_settings.emoji_close_on_select;
        settings_.dictation_send_gap = updated_settings.dictation_send_gap;
        settings_.dictation_send_paste = updated_settings.dictation_send_paste;
        settings_.dictation = updated_settings.dictation;
        settings_.dictation_server = updated_settings.dictation_server;
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
    pending_emoji_close_on_select_ = settings_.emoji_close_on_select;
    pending_dictation_send_gap_ = settings_.dictation_send_gap;
    pending_dictation_send_paste_ = settings_.dictation_send_paste;
    pending_dictation_ = settings_.dictation;
    pending_dictation_server_ = settings_.dictation_server;
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
    if (output_.enabled() && !(interaction_active_ && typing_ready_ && !field_needed_)) {
        // Transport loss ends key ownership, but unsent composition stays for
        // an explicit retry once text delivery is available again.
        cancel(false);
    }
    // A panel held open for dictation without a Steam field sends nothing:
    // the focused application did not ask for this text.
    output_.set_enabled(interaction_active_ && typing_ready_ && !field_needed_);
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
    } else if (text_output_->commit_text(text)) {
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
    if (!text.empty() && (options_.input == InputBackend::None || text_output_->commit_text(text))) {
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
        if (dictation_open_) {
            discard_dictation(); // X discards everything, without asking.
        }
        close_panel();
    } else if (kind == ControlKind::DictationDiscard && confirm_discard_) {
        discard_dictation();
    } else if (kind == ControlKind::DictationKeep) {
        confirm_discard_ = false;
    } else if (kind == ControlKind::Quit && resident_replacement()) {
        set_interaction_active(false);
        quit_ = true;
    } else if (kind == ControlKind::Pin) {
        cancel();
        settings_.pinned = !settings_.pinned;
        status_.clear();
        save();
    } else if (kind == ControlKind::Passthrough) {
        set_passthrough(!passthrough());
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
        settings_.emoji_close_on_select = pending_emoji_close_on_select_;
        settings_.dictation_send_gap = pending_dictation_send_gap_;
        settings_.dictation_send_paste = pending_dictation_send_paste_;
        settings_.dictation = pending_dictation_;
        settings_.dictation_server = pending_dictation_server_;
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
    } else if (kind == ControlKind::ChooseDictation) {
        pressed_controls_.clear();
        hovered_.clear();
        pending_dictation_ = command.value;
        settings_ui_.close_popup();
        update_settings_ui();
    } else if (kind == ControlKind::DictationTest) {
        test_dictation_server();
    } else if (kind == ControlKind::DictationServer) {
        // The address popup arrives in the next step of M4.
    } else if (kind == ControlKind::ChooseSendGap) {
        pressed_controls_.clear();
        hovered_.clear();
        pending_dictation_send_paste_ = command.value == "paste";
        if (!pending_dictation_send_paste_) {
            pending_dictation_send_gap_ = std::stoi(command.value);
        }
        settings_ui_.close_popup();
        update_settings_ui();
    } else if (kind == ControlKind::EmojiCloseOnSelect) {
        pending_emoji_close_on_select_ = !pending_emoji_close_on_select_;
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
        if (!text.empty() && options_.input != InputBackend::None && !text_output_->commit_text(text)) {
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
    if (options_.input != InputBackend::None &&
        !(output_.admit(true) && text_output_->commit_text(text))) {
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
        if (text_output_->replaces_text()) {
            // Gamescope types text through a temporary keymap. Separate libei
            // Backspace keys could interleave with it and type wrong characters,
            // so deletion travels in the same ordered text request.
            if (!text_output_->replace_text(count, word)) {
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
        if (!text_output_->taps_then_text(KEY_BACKSPACE, count, word) ||
            output_.failures() != failures) {
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
    close_emoji();
    set_interaction_active(false);
    if (resident_replacement()) {
        settings_open_ = false;
        dismiss_requested_ = true;
    } else {
        quit_ = true;
    }
}
void App::set_passthrough(bool on) {
    const bool was = passthrough();
    passthrough_ = on;
    if (!on) {
        dictation_passthrough_ = false;
    }
    if (passthrough() == was) {
        return;
    }
    cancel(false);
    status_.clear();
    dirty = true;
}
bool App::dictation_holds_panel() const {
    return dictation_open_ &&
           (capturing_ || phrases_pending() || sending_ || send_wait_since_ || !editor_.empty());
}
void App::set_field_needed(bool needed) {
    if (needed != field_needed_) {
        field_needed_ = needed;
        dirty = true;
    }
}
void App::panel_hidden() {
    // Phrases already spoken are still recognized and inserted.
    pause_capture();
}
bool App::dashboard_visibility(bool visible) {
    if (visible || !dictation_open_ || !capturing_ || !interaction_active_ || passthrough()) {
        return false;
    }
    // The VR loop switches to the passthrough pointer and releases presses.
    dictation_passthrough_ = true;
    dirty = true;
    return true;
}
bool App::take_dismissal() {
    return std::exchange(dismiss_requested_, false);
}
bool App::take_recenter() {
    const bool result = recenter_;
    recenter_ = false;
    return result;
}
std::optional<ScrollRoom> App::scroll_room(unsigned pointer) const {
    const auto position = pointer_positions_.find(pointer);
    if (!interaction_active_ || dragging_ || position == pointer_positions_.end()) {
        return {};
    }
    const auto [x, y] = position->second;
    if (settings_open_) {
        // Settings rarely overflows; its ends are not worth tracking.
        return settings_ui_.scrolls_at(x, y) ? std::optional{ScrollRoom{true, true}} : std::nullopt;
    }
    if (dictation_open_) {
        const auto v = geometry();
        if (v.dictation && v.dictation->bounds.contains(x, y) && editor_scroll_limit() > 0) {
            return ScrollRoom{editor_scroll_ > 0, editor_scroll_ < editor_scroll_limit()};
        }
        return {};
    }
    return emoji_open_ ? emoji_.scroll_room(x, y) : std::nullopt;
}
double App::editor_scroll_limit() const {
    const auto v = geometry();
    if (!v.dictation) {
        return 0;
    }
    return std::max(0.0, editor_content_height(v) - editor_text_area(v).height);
}
double App::editor_line_height() const {
    const auto v = geometry();
    return v.dictation ? editor_caret(v, 0).height : 25;
}
bool App::emoji_available() const {
    if (!emoji_font_) {
        return false; // No color emoji font: nothing would draw.
    }
    // Previews show the picker without typing; the uinput backend and
    // external JIS send only physical keys, which cannot carry an emoji.
    return options_.input == InputBackend::None || (unicode_mode() && text_ready_ && backend_ready_);
}
bool App::emoji_key(const std::string& id) const {
    const auto& keys =
        emoji_open_ ? emoji_layout_.layout.keys : profiles_.layouts.at(settings_.active.layout).keys;
    return std::any_of(keys.begin(), keys.end(), [&](const Key& key) {
        return key.id == id && key.action_kind == ActionKind::Emoji;
    });
}
void App::open_emoji() {
    if (emoji_open_ || settings_open_ || !emoji_available()) {
        return;
    }
    // A swiped word or composition still in progress goes to the target
    // first; if it can't, the picker stays closed rather than losing it.
    finish_swipe(true);
    try {
        commit_japanese();
        commit_cjk();
    } catch (const std::exception& error) {
        status_ = error.what();
    }
    dirty = true;
    if (!composition_.empty() || (cjk_ && !cjk_->empty())) {
        return;
    }
    // Release every held key and latched modifier, and cancel a pending accent.
    cancel();
    const auto main = geometry();
    emoji_width_ = visible_layout_width(main);
    emoji_layout_ = emoji_layout(*main.layout, emoji_width_);
    emoji_open_ = true;
    const auto picker = geometry();
    emoji_.set_geometry(panel_rect(picker, emoji_layout_.tabs), panel_rect(picker, emoji_layout_.grid));
    emoji_.reset();
    if (!emoji_glyphs_) {
        emoji_glyphs_ = std::make_unique<EmojiGlyphs>();
    }
    if (!emoji_checked_) {
        std::vector<std::string> texts;
        for (const auto& entry : bundled_emoji().entries) {
            texts.push_back(entry.base);
            if (entry.toned()) {
                texts.insert(texts.end(), entry.tones.begin(), entry.tones.end());
            }
        }
        emoji_glyphs_->check(std::move(texts));
    }
    update_emoji_content();
    status_.clear();
}
void App::close_emoji() {
    if (!emoji_open_) {
        return;
    }
    emoji_open_ = false;
    cancel(); // Space, Backspace and the arrows may be held.
    save_emoji();
    // Stop drawing for a closed picker and free its images, up to 8 MB.
    // Reopening draws the first screen again in milliseconds. The worker and
    // its fonts stay: destroying Pango font maps leaks fontconfig data each
    // time.
    if (emoji_glyphs_) {
        emoji_glyphs_->release();
    }
    emoji_requested_ = {};
    dirty = true;
}
void App::update_emoji_content() {
    std::array<std::vector<const EmojiEntry*>, emoji_group_count> sections;
    if (emoji_checked_) {
        for (const auto& entry : emoji_catalog_.entries) {
            sections[static_cast<std::size_t>(entry.group)].push_back(&entry);
        }
        for (const auto& base : emoji_preferences_.recent) {
            if (sections[0].size() >= emoji_.recent_capacity()) {
                break;
            }
            if (const auto* entry = emoji_catalog_.find(base)) {
                sections[0].push_back(entry);
            }
        }
    }
    emoji_.set_content(std::move(sections), emoji_preferences_.tone, !emoji_checked_);
    ++emoji_content_;
    dirty = true;
}
void App::update_emoji(double now) {
    if (emoji_glyphs_) {
        if (!emoji_checked_ && emoji_glyphs_->checked()) {
            emoji_checked_ = true;
            // Keep what the font draws; a toned emoji missing any tone takes none.
            for (const auto& entry : bundled_emoji().entries) {
                if (!emoji_glyphs_->renders(entry.base)) {
                    continue;
                }
                auto& kept = emoji_catalog_.entries.emplace_back(entry);
                if (kept.toned() &&
                    !std::all_of(kept.tones.begin(), kept.tones.end(),
                                 [&](const auto& tone) { return emoji_glyphs_->renders(tone); })) {
                    kept.tones = {};
                }
            }
            emoji_font_ = !emoji_catalog_.entries.empty();
            if (!emoji_font_) {
                status_ = "No color emoji font found. Emoji are unavailable.";
            }
            if (emoji_open_) {
                update_emoji_content();
            }
            dirty = true;
        }
        if (emoji_glyphs_->take_ready()) {
            dirty = true;
        }
    }
    if (!emoji_open_) {
        return;
    }
    if (!emoji_available()) {
        close_emoji(); // Text delivery was lost; the status line says why.
        return;
    }
    dirty |= emoji_.tick(now).redraw;
    // Ask for images when what they show changed, or once the view has moved
    // eight columns: each request draws about three screens around the view.
    const auto wanted =
        std::tuple{emoji_.glyph_size(), emoji_content_, emoji_preferences_.tone, emoji_.popup_open()};
    if (emoji_checked_ &&
        (wanted != emoji_requested_ || std::abs(emoji_.first_column() - emoji_requested_column_) >= 8)) {
        emoji_requested_ = wanted;
        emoji_requested_column_ = emoji_.first_column();
        emoji_glyphs_->request(emoji_.glyph_size(),
                               emoji_.glyph_order(EmojiGlyphs::capacity(emoji_.glyph_size())));
    }
}
void App::apply_emoji_event(const EmojiPicker::Event& event) {
    dirty |= event.redraw;
    if (event.clear) {
        emoji_preferences_.recent.clear();
        update_emoji_content();
        save_emoji();
    }
    const bool typed = !event.type.empty() && type_emoji(event.type);
    if (event.tone) {
        // A chosen tone sticks only once its emoji was sent; the picker shows
        // it already, so put the old one back otherwise.
        if (typed) {
            emoji_preferences_.tone = *event.tone;
        }
        emoji_.set_tone(emoji_preferences_.tone);
    }
    if (typed) {
        // Recent shows it at once; choosing from Recent itself keeps its order,
        // so nothing moves under the laser there.
        if (!event.recent) {
            remember_emoji(emoji_preferences_, event.typed->base);
            update_emoji_content();
        }
        if (settings_.emoji_close_on_select) {
            close_emoji();
        }
    }
}
bool App::type_emoji(const std::string& text) {
    if (options_.input == InputBackend::None) {
        return true; // The preview cannot type; it still remembers the emoji.
    }
    // One commit for the whole sequence, so a family or flag arrives whole.
    if (!backend_ready_ || !output_.admit(true) || !text_output_->commit_text(text)) {
        update_output_status();
        dirty = true;
        return false;
    }
    status_.clear();
    return true;
}
void App::save_emoji() {
    if (emoji_preferences_ == emoji_saved_) {
        return;
    }
    try {
        save_emoji_preferences(options_.config_dir / "emoji.json", emoji_preferences_);
        emoji_saved_ = emoji_preferences_;
    } catch (const std::exception& error) {
        status_ = "Recent emoji not saved: " + std::string(error.what());
    }
}

bool App::dictation_available() const {
    if (options_.mode == Mode::RenderDictation) {
        return true; // image exports show the mode whatever the settings
    }
    const auto& language = profiles_.languages.at(settings_.active.language);
    // Composer profiles wait until their commits can go into the editor.
    return settings_.dictation == "server" &&
           (!settings_.dictation_server.empty() || !settings_.dictation_url.empty()) &&
           !language.dictation_language.empty() && language.input_method == "xkb";
}

void App::open_dictation() {
    if (dictation_open_ || settings_open_ || !dictation_available()) {
        return;
    }
    close_emoji();
    // A swiped word still being decoded goes to the target first, as before
    // any other key; the editor starts from a keyboard with nothing held.
    finish_swipe(true);
    discard_swipe();
    swipe_space_ = false;
    cancel();
    dictation_open_ = true;
    dictation_layout_key_.clear();
    refresh_dictation_layout();
    text_output_ = &editor_output_;
    editor_goal_x_.reset();
    follow_cursor();
    status_.clear();
    dirty = true;
}

void App::close_dictation() {
    if (!dictation_open_) {
        return;
    }
    if (sending_) {
        stop_send(sending_->in_flight);
    }
    if (capturing_ || phrases_pending()) {
        cancel_capture();
    }
    send_wait_since_.reset();
    reveal_.reset();
    // Dictation's passthrough ends with the mode; with the dashboard closed
    // the panel then hides, unless the user's own toggle is on.
    dictation_passthrough_ = false;
    confirm_discard_ = false;
    notice_.clear();
    notice_error_ = false;
    enter_pending_ = false;
    dictation_open_ = false;
    text_output_ = options_.text_output ? options_.text_output : &output_;
    // Held editor keys must not turn into presses on the keyboard's keys. The
    // text stays in memory until the mode opens again.
    cancel();
    dirty = true;
}

void App::show_dictation() {
    open_dictation();
    editor_.clear();
    editor_.type(
        "Dictation turns speech into text. You speak, check the text here, and send it to the "
        "focused application in one go.\nA second paragraph shows a line break. Longer text wraps "
        "at the edge and scrolls inside the editor, and the counter shows its length. Phrases join "
        "with a space where one is needed, and the thumbstick or Page Up and Page Down scroll back "
        "through what was said.\nA third paragraph makes the text taller than the editor, so the "
        "scroll indicator appears at its right edge.");
    editor_.set_cursor(editor_.text().find(" in one go"));
    follow_cursor();
}

void App::refresh_dictation_layout() {
    const auto key = settings_.active.layout + (settings_.hide_numpad ? "/numpad-hidden" : "");
    if (key == dictation_layout_key_) {
        return;
    }
    PanelView main;
    main.layout = &profiles_.layouts.at(settings_.active.layout);
    main.theme = &profiles_.themes.at(settings_.active.theme);
    main.language = &profiles_.languages.at(settings_.active.language);
    main.hide_numpad = settings_.hide_numpad;
    dictation_layout_ = dictation_layout(*main.layout, visible_layout_width(main));
    dictation_layout_key_ = key;
    dirty = true;
}

void App::set_dictation_view(PanelView& view) const {
    DictationView editor;
    editor.bounds = panel_rect(view, dictation_layout_.editor);
    editor.text = editor_.text();
    editor.cursor = editor_.cursor();
    if (reveal_) {
        // The caret follows the words as they appear.
        editor.text.erase(reveal_->shown, reveal_->end - reveal_->shown);
        editor.cursor = reveal_->shown;
        editor.revealing = true;
    }
    editor.scroll = editor_scroll_;
    editor.uncertain = editor_.uncertain();
    editor.capturing = capturing_;
    if (confirm_discard_) {
        editor.notice = "Discard text?";
    } else if (!notice_.empty()) {
        editor.notice = notice_;
        editor.notice_error = notice_error_;
    } else if (waiting_) {
        editor.notice = "Waiting for recognition…";
    } else if (field_needed_) {
        editor.notice = "The text field closed. Select a text field to send.";
    }
    if (sending_) {
        editor.sending = true;
        editor.counter =
            "Sending " + std::to_string(sending_->done) + "/" + std::to_string(sending_->total);
    } else {
        editor.counter =
            std::to_string(editor_.length()) + "/" + std::to_string(DictationEditor::dictation_limit);
        editor.at_limit = !editor_.can_dictate();
    }
    view.dictation = std::move(editor);
}

bool App::dictation_down(unsigned pointer, const Key& key, double now) {
    if (keyboard_.pointer_pressed(pointer)) {
        return false;
    }
    if (sending_) {
        // The editor is locked while it sends; Esc stops sending.
        if (key.action_kind != ActionKind::Key || key.action != "Escape" ||
            !keyboard_.down(pointer, key, now, true, 0)) {
            return false;
        }
        stop_send(sending_->in_flight);
        dirty = true;
        return true;
    }
    if (key.action_kind == ActionKind::Dictation) {
        // Mic, Send and Undo act on release, like toolbar buttons.
        if (dictation_key_pointer_ || input_view().disabled_keys.contains(key.id) ||
            !keyboard_.down(pointer, key, now, true, 0)) {
            return false;
        }
        dictation_key_pointer_ = pointer;
        dirty = true;
        return true;
    }
    if (reveal_) {
        // Typing waits while a phrase appears; Esc finishes it at once and
        // then acts, as the mode's own keys do on release.
        if (key.action_kind == ActionKind::Key && key.action == "Escape") {
            finish_reveal();
        } else {
            return false;
        }
    }
    if (confirm_discard_) {
        // The question is modal: its buttons answer it, and Esc again discards.
        if (key.action_kind != ActionKind::Key || key.action != "Escape" ||
            !keyboard_.down(pointer, key, now, true, 0)) {
            return false;
        }
        discard_dictation();
        return true;
    }
    // Capture the symbol before a latched Shift is consumed by down().
    const auto modifiers = keyboard_.modifiers();
    const auto symbol = key.action_kind == ActionKind::Key
                            ? keymap_->symbol(key, modifiers, keyboard_.caps(), keyboard_.num())
                            : XKB_KEY_NoSymbol;
    const bool old_num = keyboard_.num();
    // Every key is local here: it shows as pressed and latches, but sends nothing.
    if (!keyboard_.down(pointer, key, now, true, 0)) {
        return false;
    }
    dirty = true;
    if (keyboard_.num() != old_num) {
        settings_.num_lock = keyboard_.num();
        save();
    }
    if (key.action_kind != ActionKind::Key || is_modifier(key_code(key.action)) ||
        key.action == "CapsLock" || key.action == "NumLock") {
        return true;
    }
    if (key.action == "Escape") {
        keymap_->cancel_compose();
        if (send_wait_since_) {
            send_wait_since_.reset(); // Only the wait ends; phrases still arrive.
        } else if (capturing_ || phrases_pending()) {
            cancel_capture(); // The text stays editable.
        } else if (editor_.empty()) {
            close_dictation();
        } else {
            confirm_discard_ = true;
        }
        return true;
    }
    if (capturing_ || send_wait_since_) {
        return true; // Read-only while listening; the key only shows as pressed.
    }
    if (notice_error_) {
        notice_.clear();
        notice_error_ = false;
    }
    if (const auto command = editor_command(key, symbol); command != EditorCommand::None) {
        keymap_->cancel_compose();
        run_editor_command(command);
        if (command != EditorCommand::PageUp && command != EditorCommand::PageDown &&
            command != EditorCommand::Newline) {
            editor_repeat_ = EditorRepeat{pointer, command, {}, now + .5};
        }
        return true;
    }
    const auto codepoint = xkb_keysym_to_utf32(symbol);
    if (codepoint == 0 || g_unichar_iscntrl(codepoint)) {
        return true; // nothing an editor can hold, such as a dead numpad key
    }
    const auto text = keymap_->compose(symbol);
    if (!text.empty() && text_output_->text_press(pointer, text, now)) {
        editor_goal_x_.reset();
        follow_cursor();
        editor_repeat_ = EditorRepeat{pointer, EditorCommand::None, text, now + .5};
    }
    return true;
}

void App::dictation_key_up(const Key& key) {
    finish_reveal(); // The mode's keys act on the whole text.
    if (key.action == "mic") {
        // Start, pause, resume, pause; from the normal keyboard it opens the
        // mode and starts at once.
        if (!dictation_open_) {
            open_dictation();
            if (dictation_open_) {
                start_capture();
            }
        } else if (capturing_) {
            pause_capture();
        } else {
            start_capture();
        }
    } else if (key.action == "send") {
        if (capturing_ || phrases_pending()) {
            // Phrases still being spoken or recognized go in first.
            pause_capture();
            send_wait_since_ = -1; // Timed from the next tick.
        } else {
            start_send();
        }
    } else if (key.action == "undo") {
        if (editor_.undo_phrase()) {
            editor_goal_x_.reset();
            follow_cursor();
        }
    }
    dirty = true;
}

bool App::start_capture() {
    if (capturing_ || sending_ || send_wait_since_) {
        return false;
    }
    if (!editor_.can_dictate()) {
        notice_ = "The editor is full. Send or shorten the text to dictate more.";
        notice_error_ = true;
        dirty = true;
        return false;
    }
    if (!dictation_) {
        dictation_ =
            std::make_unique<Dictation>(options_.config_dir / "dictation-key", options_.dictation_helper,
                                        options_.dictation_arguments);
    }
    const auto& language = profiles_.languages.at(settings_.active.language);
    Dictation::Target target;
    // A URL from config.json replaces the server address.
    (settings_.dictation_url.empty() ? target.server : target.url) =
        settings_.dictation_url.empty() ? settings_.dictation_server : settings_.dictation_url;
    target.model = settings_.dictation_model;
    target.language = language.dictation_language;
    if (!dictation_->configure(target) || !dictation_->capture(true)) {
        notice_ = "Dictation could not start.";
        notice_error_ = true;
        dirty = true;
        return false;
    }
    capturing_ = true;
    mic_levels_.assign(6, 0);
    mic_incoming_.clear();
    notice_.clear();
    notice_error_ = false;
    editor_repeat_.reset();
    editor_.mark_uncertain(0);
    dirty = true;
    return true;
}

void App::pause_capture() {
    if (!capturing_) {
        return;
    }
    // The phrase being spoken ends here and is still recognized; Send waits
    // for the helper to confirm, so a phrase it is just announcing is not missed.
    if (dictation_ && dictation_->capture(false)) {
        awaiting_pause_ = true;
    }
    capturing_ = false;
    mic_levels_.clear();
    dirty = true;
}

void App::cancel_capture() {
    if (dictation_) {
        dictation_->cancel();
    }
    capturing_ = false;
    mic_levels_.clear();
    pending_phrases_.clear();
    awaiting_pause_ = false;
    waiting_ = false;
    send_wait_since_.reset();
    dirty = true;
}

void App::end_dictation_for_update() {
    // An update restarts the keyboard, which loses the editor anyway: discard
    // everything, and let the helper and its recorder start exiting now. The
    // handoff runs only after App is gone, and ~Dictation waits for the
    // helper (docs/dictation.md, M2 lifetime).
    if (dictation_open_) {
        discard_dictation();
    }
    cancel_capture();
    if (dictation_) {
        dictation_->stop();
    }
}

void App::discard_dictation() {
    if (sending_) {
        stop_send(sending_->in_flight);
    }
    cancel_capture();
    editor_.clear();
    editor_scroll_ = 0;
    status_.clear();
    close_dictation();
}

namespace {
// What the editor says about a helper error; the codes are fixed.
std::string dictation_problem(const std::string& reason) {
    static const std::map<std::string, std::string> messages{
        {"no-microphone", "No microphone. Put the headset on, then press the mic key."},
        {"capture-stopped", "The system stopped the microphone."},
        {"not-configured", "No speech server is configured."},
        {"unreachable", "The speech server cannot be reached."},
        {"timeout", "The speech server did not answer in time."},
        {"server-error", "The speech server reported an error."},
        {"bad-reply", "The speech server's reply was unusable."},
        {"key-rejected", "The API key was rejected."},
        {"rate-limited", "The provider is rate limiting requests. Try again shortly."},
        {"backlog-full", "Too much speech is waiting for recognition."},
        {"keyboard-stalled", "Dictation stopped while the keyboard was busy."},
        {"helper-exited", "The dictation helper stopped."},
    };
    const auto found = messages.find(reason);
    return found != messages.end() ? found->second : "Dictation stopped.";
}
} // namespace

void App::dictation_event(const Dictation::Event& event, double now) {
    using Kind = Dictation::Event::Kind;
    switch (event.kind) {
    case Kind::Level: {
        if (!capturing_ || mic_levels_.empty()) {
            break;
        }
        mic_incoming_.push_back(std::clamp(event.level * 9 / 256, 0, 8));
        if (mic_incoming_.size() > 3) {
            mic_incoming_.pop_front(); // Fall behind no more than a few bars.
        }
        break;
    }
    case Kind::Phrase:
        pending_phrases_.emplace(event.id, 0);
        break;
    case Kind::Ready:
        // Answered at once: the prompt is the text before the cursor now.
        dictation_->prompt(event.id, editor_.context());
        pending_phrases_[event.id] = now;
        break;
    case Kind::Text: {
        if (!pending_phrases_.erase(event.id) || !dictation_open_) {
            break; // cancelled meanwhile
        }
        const auto& language = profiles_.languages.at(settings_.active.language);
        const bool spaced = language.dictation_language != "ja" && language.dictation_language != "zh";
        finish_reveal();
        // The phrase and its joining spaces go in at the cursor in one piece,
        // so joining and Undo work as usual; the view reveals it word by word.
        const auto start = editor_.cursor();
        const auto size = editor_.text().size();
        if (editor_.insert_phrase(event.text, spaced)) {
            reveal_ = Reveal{start, start + editor_.text().size() - size, start, now};
            editor_goal_x_.reset();
            dirty = true;
        }
        if (!editor_.can_dictate() && capturing_) {
            pause_capture(); // Dictation stops at 900; the counter turns amber.
        }
        break;
    }
    case Kind::Dropped:
        pending_phrases_.erase(event.id);
        break;
    case Kind::Stopped:
        capturing_ = false;
        mic_levels_.clear();
        if (event.reason != "silence") {
            notice_ = dictation_problem(event.reason);
            notice_error_ = true;
        }
        dirty = true;
        break;
    case Kind::Error:
        // The helper dropped every pending phrase; text already in stays.
        capturing_ = false;
        mic_levels_.clear();
        pending_phrases_.clear();
        awaiting_pause_ = false;
        send_wait_since_.reset();
        if (dictation_open_) {
            notice_ = dictation_problem(event.reason);
            notice_error_ = true;
        }
        dirty = true;
        break;
    case Kind::Paused:
        awaiting_pause_ = false;
        break;
    case Kind::Test:
        // Only the Dictation card's Test asks. The line under the row label is
        // narrow, so the answer is a word or two.
        dictation_test_ = event.ok                           ? "Reachable"
                          : event.reason == "timeout"        ? "No answer"
                          : event.reason == "server-error"   ? "Server error"
                          : event.reason == "bad-reply"      ? "Bad reply"
                          : event.reason == "helper-exited"  ? "Helper stopped"
                          : event.reason == "not-configured" ? "No server set"
                                                             : "Unreachable";
        update_settings_ui();
        dirty = true;
        break;
    }
}

void App::finish_reveal() {
    if (reveal_) {
        reveal_.reset();
        follow_cursor();
        dirty = true;
    }
}

void App::test_dictation_server() {
    if (pending_dictation_server_.empty()) {
        return;
    }
    if (!dictation_) {
        dictation_ =
            std::make_unique<Dictation>(options_.config_dir / "dictation-key", options_.dictation_helper,
                                        options_.dictation_arguments);
    }
    // The pending address, before Apply; the next capture configures the
    // saved one again.
    Dictation::Target target;
    target.server = pending_dictation_server_;
    target.model = settings_.dictation_model;
    target.language = profiles_.languages.at(settings_.active.language).dictation_language;
    dictation_test_ =
        dictation_->configure(target) && dictation_->test() ? "Testing…" : "Helper stopped";
    update_settings_ui();
    dirty = true;
}

bool App::tick_dictation(double now) {
    if (!dictation_) {
        return false;
    }
    const bool was_dirty = dirty;
    dictation_->tick(now);
    while (const auto event = dictation_->next()) {
        dictation_event(*event, now);
    }
    // The next level starts sliding in once the previous one has arrived.
    if (capturing_ && mic_levels_.size() == 6 && !mic_incoming_.empty() &&
        now >= mic_level_since_ + dictation_level_period) {
        mic_levels_.erase(mic_levels_.begin());
        mic_levels_.push_back(mic_incoming_.front());
        mic_incoming_.pop_front();
        // On pace, the next starts where this one's slot began; after a pause,
        // now.
        mic_level_since_ = now - mic_level_since_ < 2 * dictation_level_period
                               ? mic_level_since_ + dictation_level_period
                               : now;
        dirty = true;
    }
    // One more word every 20 ms; after a slow frame the missed ones at once.
    while (reveal_ && now >= reveal_->next) {
        const auto& text = editor_.text();
        auto& shown = reveal_->shown;
        while (shown < reveal_->end && text[shown] == ' ') {
            ++shown;
        }
        while (shown < reveal_->end && text[shown] != ' ') {
            ++shown;
        }
        reveal_->next += .02;
        if (shown >= reveal_->end) {
            reveal_.reset();
        }
        follow_cursor();
        dirty = true;
    }
    const bool waiting =
        std::any_of(pending_phrases_.begin(), pending_phrases_.end(),
                    [&](const auto& phrase) { return phrase.second > 0 && now - phrase.second > 1; });
    if (waiting != waiting_) {
        waiting_ = waiting;
        dirty = true;
    }
    if (send_wait_since_) {
        if (*send_wait_since_ < 0) {
            send_wait_since_ = now;
        }
        if (!phrases_pending()) {
            send_wait_since_.reset();
            start_send();
        } else if (now - *send_wait_since_ > 20) {
            send_wait_since_.reset();
            notice_ = "Recognition took too long. Send again when the text is in.";
            notice_error_ = true;
        }
        dirty = true;
    }
    return dirty && !was_dirty;
}

namespace {
// Characters a piece adds to Send's counter; a line break counts as one.
std::size_t counted(const SendPiece& piece) {
    switch (piece.kind) {
    case SendPiece::Kind::Text:
    case SendPiece::Kind::Paste:
        return static_cast<std::size_t>(g_utf8_strlen(piece.text.c_str(), -1));
    case SendPiece::Kind::Enter:
        return 0;
    default:
        return 1;
    }
}
} // namespace

bool App::can_send() const {
    return !sending_ && !send_wait_since_ && !confirm_discard_ && !field_needed_ &&
           (editor_.text().find_first_not_of(" \n") != std::string::npos ||
            (enter_pending_ && editor_.empty()) || capturing_ || phrases_pending());
}

void App::start_send() {
    if (!can_send()) {
        return;
    }
    Sending send;
    if (editor_.text().find_first_not_of(" \n") == std::string::npos) {
        // Only the final Enter is left; the editor holds nothing to send.
        send.pieces = {{SendPiece::Kind::Enter, {}, 0, editor_.text().size()}};
    } else if (settings_.dictation_send_paste && !output_.steam_field_target()) {
        // Steam's own fields take text directly; the clipboard cannot reach them.
        send.pieces = paste_pieces(editor_.text());
    } else {
        send.pieces = send_pieces(editor_.text(), !output_.single_line_target());
    }
    for (const auto& piece : send.pieces) {
        send.total += counted(piece);
    }
    keymap_->cancel_compose();
    editor_repeat_.reset();
    editor_.mark_uncertain(0);
    status_.clear();
    sending_ = std::move(send);
    dirty = true;
}

bool App::tick_send(double now) {
    if (!sending_) {
        return false;
    }
    auto& s = *sending_;
    if (s.since < 0) {
        s.since = now;
    }
    const auto& piece = s.pieces[s.next];
    if (piece.kind == SendPiece::Kind::Paste) {
        return tick_paste(now);
    }
    if (!s.in_flight) {
        // A held key could turn letters into shortcuts, and earlier output
        // must arrive first. Every editor key is ignored meanwhile, so this ends.
        if (keyboard_.active() || output_.text_pending()) {
            return false;
        }
        if (s.next == 0 && output_.retry_available()) {
            output_.cancel(); // text the main keyboard failed to send stays unsent
        }
        s.completed = output_.completed();
        s.failures = output_.failures();
        bool accepted = false;
        switch (piece.kind) {
        case SendPiece::Kind::Text:
        case SendPiece::Kind::Space:
            // Only Send types at the chosen pace; it is reset when Send ends.
            output_.set_character_gap(settings_.dictation_send_gap / 1000.0);
            accepted = output_.commit_text(piece.text);
            break;
        case SendPiece::Kind::Break:
            accepted = output_.tap({KEY_LEFTSHIFT, KEY_ENTER});
            break;
        case SendPiece::Kind::Enter:
            accepted = output_.tap({KEY_ENTER});
            break;
        case SendPiece::Kind::Paste:
            break; // tick_paste
        }
        if (!accepted) {
            // A target briefly busy with an earlier request accepts it later.
            if (output_.failures() != s.failures || now - s.since > 2) {
                stop_send(false);
                return true;
            }
            return false;
        }
        s.in_flight = true;
    }
    if (output_.failures() != s.failures) {
        stop_send(true);
        return true;
    }
    if (output_.completed() != s.completed) {
        finish_piece(now);
        return true;
    }
    // Hiding the panel or losing the target cancels the queue without a
    // failure; a target that never answers is given up the same way.
    if (!output_.text_pending() || now - s.since > 5) {
        stop_send(true);
        return true;
    }
    return false;
}

void App::finish_piece(double now) {
    auto& s = *sending_;
    const auto& piece = s.pieces[s.next];
    {
        editor_.erase_front(piece.end - s.sent);
        s.sent = piece.end;
        s.done += counted(piece);
        s.in_flight = false;
        s.since = now;
        follow_cursor();
        if (++s.next == s.pieces.size()) {
            sending_.reset();
            output_.set_character_gap(GamescopeText::default_character_gap);
            editor_.clear();
            editor_scroll_ = 0;
            close_dictation();
        }
    }
}

Clipboard* App::clipboard() {
    if (options_.clipboard) {
        return options_.clipboard;
    }
    if (!clipboard_) {
        clipboard_ = make_x_clipboard();
    }
    return clipboard_.get();
}

bool App::tick_paste(double now) {
    auto& s = *sending_;
    const auto& piece = s.pieces[s.next];
    auto give_up = [&](const char* why) {
        stop_send(false);
        status_ = why;
        return true;
    };
    switch (s.paste_stage) {
    case 0:
        // As for typing: nothing held, earlier output delivered.
        if (keyboard_.active() || output_.text_pending()) {
            return false;
        }
        if (output_.retry_available()) {
            output_.cancel();
        }
        // Written, never read: the keyboard owns the clipboard with this text.
        s.paste_write = clipboard()->write(piece.text);
        s.paste_stage = 1;
        s.paste_at = now;
        return false;
    case 1:
        if (const auto owned = clipboard()->result(s.paste_write)) {
            if (!*owned) {
                return give_up("The clipboard is not available. The text stays here.");
            }
            s.paste_stage = 2;
            s.paste_at = now;
        } else if (now - s.paste_at > 1) {
            return give_up("The clipboard is not available. The text stays here.");
        }
        return false;
    case 2: {
        // Gamescope fetches a new clipboard at once; let that pass, so the
        // next request is the application's paste.
        if (now - s.paste_at < .15) {
            return false;
        }
        s.paste_served = clipboard()->served();
        s.completed = output_.completed();
        s.failures = output_.failures();
        const int v = output_.shortcut_code(XKB_KEY_v, KEY_V);
        if (!output_.tap({KEY_LEFTCTRL, v})) {
            return now - s.paste_at > 2 ? give_up("Paste could not be pressed. The text stays here.")
                                        : false;
        }
        s.paste_stage = 3;
        s.in_flight = true; // From here an interrupted paste may have arrived.
        s.paste_at = now;
        return false;
    }
    case 3:
        if (output_.failures() != s.failures) {
            return give_up("Paste could not be pressed. The text stays here.");
        }
        if (clipboard()->served() != s.paste_served) {
            s.paste_stage = 4;
            s.paste_at = now;
        } else if (now - s.paste_at > 2) {
            return give_up("The application did not take the pasted text. It stays here.");
        }
        return false;
    default:
        // The application has the text; give it a moment to insert it
        // before Enter, which would otherwise overtake it.
        if (now - s.paste_at < .1 || output_.text_pending()) {
            return false;
        }
        s.since = now;
        finish_piece(now);
        return true;
    }
}

void App::stop_send(bool attempted) {
    const auto s = std::move(*sending_);
    sending_.reset();
    const auto& piece = s.pieces[s.next];
    std::size_t keep = piece.begin, uncertain = attempted ? piece.end : piece.begin;
    if (attempted && piece.kind == SendPiece::Kind::Text) {
        // A text sent one character at a time reports which characters were
        // acknowledged: only the one that timed out may have arrived.
        const auto rest = output_.retry_text();
        if (!rest.empty() && rest.size() < piece.text.size() && piece.text.ends_with(rest)) {
            keep = piece.end - rest.size();
            uncertain = keep + static_cast<std::size_t>(g_utf8_next_char(rest.c_str()) - rest.c_str());
        }
    }
    // The editor keeps what is left, so the queue must not retry it as well.
    output_.cancel();
    output_.set_character_gap(GamescopeText::default_character_gap);
    output_.take_status();
    if (piece.kind == SendPiece::Kind::Enter) {
        editor_.erase_front(editor_.text().size());
        enter_pending_ = true;
        status_ = "The text was sent, but Enter was not confirmed. Send presses Enter again.";
    } else {
        editor_.erase_front(keep - s.sent);
        editor_.mark_uncertain(uncertain - keep);
        status_ =
            uncertain > keep
                ? "Sending stopped. The rest stays here; the marked start may already have arrived."
                : "Sending stopped. The rest stays here; Send continues.";
    }
    follow_cursor();
    dirty = true;
}

App::EditorCommand App::editor_command(const Key& key, xkb_keysym_t symbol) const {
    switch (symbol) {
    case XKB_KEY_BackSpace:
        return EditorCommand::Backspace;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        return EditorCommand::Delete;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        return EditorCommand::Newline;
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        return EditorCommand::Left;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        return EditorCommand::Right;
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
        return EditorCommand::Up;
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
        return EditorCommand::Down;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        return EditorCommand::Home;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        return EditorCommand::End;
    case XKB_KEY_Prior:
    case XKB_KEY_KP_Prior:
        return EditorCommand::PageUp;
    case XKB_KEY_Next:
    case XKB_KEY_KP_Next:
        return EditorCommand::PageDown;
    default:
        break;
    }
    // Keys whose keysym the profile does not report, by their physical name.
    static const std::map<std::string, EditorCommand> by_action{
        {"Backspace", EditorCommand::Backspace}, {"Delete", EditorCommand::Delete},
        {"Enter", EditorCommand::Newline},       {"NumpadEnter", EditorCommand::Newline},
        {"ArrowLeft", EditorCommand::Left},      {"ArrowRight", EditorCommand::Right},
        {"ArrowUp", EditorCommand::Up},          {"ArrowDown", EditorCommand::Down},
        {"Home", EditorCommand::Home},           {"End", EditorCommand::End},
        {"PageUp", EditorCommand::PageUp},       {"PageDown", EditorCommand::PageDown}};
    if (symbol == XKB_KEY_NoSymbol) {
        if (const auto it = by_action.find(key.action); it != by_action.end()) {
            return it->second;
        }
    }
    return EditorCommand::None;
}

void App::run_editor_command(EditorCommand command) {
    const auto view = geometry();
    const auto area = editor_text_area(view);
    const double content = editor_content_height(view);
    if (command != EditorCommand::Up && command != EditorCommand::Down) {
        editor_goal_x_.reset();
    }
    switch (command) {
    case EditorCommand::Backspace:
        editor_.backspace();
        break;
    case EditorCommand::Delete:
        editor_.erase_forward();
        break;
    case EditorCommand::Newline:
        editor_.type("\n");
        break;
    case EditorCommand::Left:
        editor_.left();
        break;
    case EditorCommand::Right:
        editor_.right();
        break;
    case EditorCommand::Up:
    case EditorCommand::Down: {
        const auto caret = editor_caret(view, editor_.cursor());
        const double x = editor_goal_x_.value_or(caret.x);
        editor_goal_x_ = x;
        if (command == EditorCommand::Up) {
            editor_.set_cursor(caret.y < 1 ? 0 : editor_index_at(view, x, caret.y - caret.height / 2));
        } else {
            const double y = caret.y + caret.height * 1.5;
            editor_.set_cursor(y >= content ? editor_.text().size() : editor_index_at(view, x, y));
        }
        break;
    }
    case EditorCommand::Home:
        editor_.set_cursor(editor_line(view, editor_.cursor()).first);
        break;
    case EditorCommand::End:
        editor_.set_cursor(editor_line(view, editor_.cursor()).second);
        break;
    case EditorCommand::PageUp:
    case EditorCommand::PageDown: {
        // Pages scroll the view; the cursor stays where it is.
        const double page = command == EditorCommand::PageUp ? -area.height : area.height;
        editor_scroll_ = std::clamp(editor_scroll_ + page, 0.0, std::max(0.0, content - area.height));
        dirty = true;
        return;
    }
    case EditorCommand::None:
        return;
    }
    follow_cursor();
    dirty = true;
}

void App::follow_cursor() {
    if (!dictation_open_) {
        return;
    }
    const auto view = geometry();
    const auto area = editor_text_area(view);
    const auto caret = editor_caret(view, view.dictation->cursor);
    const double content = editor_content_height(view);
    if (caret.y < editor_scroll_) {
        editor_scroll_ = caret.y;
    } else if (caret.y + caret.height > editor_scroll_ + area.height) {
        editor_scroll_ = caret.y + caret.height - area.height;
    }
    editor_scroll_ = std::clamp(editor_scroll_, 0.0, std::max(0.0, content - area.height));
}
} // namespace framekeyboard
