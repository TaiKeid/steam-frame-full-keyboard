#include "framekeyboard/app.hpp"

#include <algorithm>
#include <chrono>
#include <glib.h>
#include <stdexcept>
#include <tuple>

namespace framekeyboard {
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
bool same_keymap(const Language& a, const Language& b) {
    return std::tie(a.rules, a.model, a.keymap, a.variant, a.options) ==
           std::tie(b.rules, b.model, b.keymap, b.variant, b.options);
}
template <class ProfilesMap> void cycle(const ProfilesMap& map, std::string& id, bool next) {
    auto it = map.find(id);
    if (it == map.end()) {
        id = map.begin()->first;
        return;
    }
    if (next) {
        if (++it == map.end()) {
            it = map.begin();
        }
    } else {
        if (it == map.begin()) {
            it = map.end();
        }
        --it;
    }
    id = it->first;
}
} // namespace
App::App(const Options& options, KeySink& sink)
    : options_(options), profiles_(load_profiles(options.data_dir, options.config_dir)),
      settings_(load_settings(options.config_dir, profiles_.errors)), gate_(sink), keyboard_(gate_) {
    try {
        validate_selection(profiles_, settings_.active);
    } catch (const std::exception& error) {
        profiles_.errors.push_back(error.what());
        settings_.active = {};
    }
    keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
    pending_ = settings_.active;
    status_ = profiles_.errors.empty() ? "" : profiles_.errors.front();
    if (const auto target = profiles_.languages.find(options_.target_language);
        target != profiles_.languages.end()) {
        target_language_ = target->second;
    }
    // A saved language mismatch must leave Settings reachable, not abort launch.
    refresh_typing();
}
std::vector<Control> App::controls() const {
    std::vector<Control> result = {
        {"settings", settings_open_ ? "Back" : "Settings", {18, 12, 132, 42}},
        {"release", "Release all", {160, 12, 155, 42}},
        {"recenter", "Recenter", {325, 12, 125, 42}},
        {"size-smaller", "Smaller keyboard", {460, 12, 60, 42}, false, ControlIcon::ScaleDown},
        {"size-larger", "Larger keyboard", {530, 12, 60, 42}, false, ControlIcon::ScaleUp},
        {"close", "Close", {1450, 12, 132, 42}}};
    if (!settings_open_) {
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
                    for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size()));
                         ++i) {
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
        return result;
    }
    result.push_back({"preset-ja-romaji", "日本語 Romaji", {610, 12, 230, 42}});
    result.push_back({"preset-ja-kana", "日本語 Kana", {850, 12, 230, 42}});
    result.push_back({"preset-ja-jis", "JIS (system IME)", {1090, 12, 250, 42}});
    auto row = [&](const std::string& kind, const std::string& name, double y) {
        result.push_back({kind + "-prev", "<", {40, y, 65, 64}});
        result.push_back({kind + "-label", name, {115, y, 1370, 64}});
        result.push_back({kind + "-next", ">", {1495, y, 65, 64}});
    };
    auto source = [&](const std::string& kind, const std::string& id) {
        const auto found = profiles_.sources.find(kind + "/" + id);
        return found == profiles_.sources.end()
                   ? " [built-in]"
                   : " [" + fs::path(found->second).filename().string() + "]";
    };
    row("layout",
        "Layout: " + profiles_.layouts.at(pending_.layout).name + source("layouts", pending_.layout),
        96);
    row("language",
        "Language: " + profiles_.languages.at(pending_.language).name +
            source("languages", pending_.language),
        182);
    row("theme", "Theme: " + profiles_.themes.at(pending_.theme).name + source("themes", pending_.theme),
        268);
    if (!settings_.favorites.empty()) {
        row("favorite", "Favorite: " + settings_.favorites.at(favorite_index_).name, 354);
    }
    result.push_back({"apply", "Apply and save", {350, 446, 280, 60}});
    result.push_back({"reload", "Reload profiles", {650, 446, 280, 60}});
    if (!settings_.favorites.empty()) {
        result.push_back({"favorite-use", "Use favorite", {950, 446, 280, 60}});
    }
    return result;
}
PanelView App::view() const {
    PanelView v;
    v.layout = &profiles_.layouts.at(settings_.active.layout);
    v.theme = &profiles_.themes.at(settings_.active.theme);
    v.language = &profiles_.languages.at(settings_.active.language);
    v.keymap = keymap_.get();
    v.keyboard = &keyboard_;
    v.controls = controls();
    v.status = dragging_ ? "Release grab to place keyboard" : status_;
    if (v.status.empty() && !gate_.enabled) {
        v.status = options_.input == "none"
                       ? "Preview only: this launch cannot type."
                       : "Typing disabled: choose a profile matching the target keymap used at launch.";
    }
    v.settings = settings_open_;
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
    if (japanese() && options_.input != "none" && !text_ready_) {
        v.status =
            "Japanese text delivery unavailable; conversion preview only. English remains available.";
    }
    for (const auto& [pointer, id] : hovered_) {
        (void)pointer;
        v.hovered.insert(id);
    }
    return v;
}
bool App::move(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    const auto v = view();
    std::string id;
    bool keyboard_key = false;
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            id = control.id;
        }
    }
    if (id.empty()) {
        if (const auto* key = renderer.hit_key(v, x, y)) {
            id = key->id;
            keyboard_key = true;
        }
    }
    if (hovered_[pointer] != id) {
        hovered_[pointer] = id;
        dirty = true;
        return keyboard_key && !dragging_ && !keyboard_.pointer_pressed(pointer);
    }
    return false;
}

bool App::down(unsigned pointer, double x, double y, double now) {
    if (dragging_ || !interaction_active_) {
        return false;
    }
    move(pointer, x, y);
    const auto v = view();
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            pressed_controls_[pointer] = control.id;
            return false;
        }
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        const auto modifiers = keyboard_.modifiers();
        const bool local = japanese_key(*key, false, modifiers);
        const bool accepted = keyboard_.down(pointer, *key, now, local);
        if (accepted && !local && key->action_kind == ActionKind::Shortcut) {
            composition_.cancel();
        }
        if (accepted && !local && key->action_kind == ActionKind::Key &&
            !is_modifier(key_code(key->action)) && !composition_.empty()) {
            composition_.cancel();
        }
        if (accepted && local) {
            try {
                japanese_key(*key, true, modifiers);
            } catch (const std::exception& error) {
                status_ = error.what();
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
    const bool key_released = keyboard_.up(pointer);
    move(pointer, x, y);
    const auto it = pressed_controls_.find(pointer);
    if (it != pressed_controls_.end()) {
        const auto id = it->second;
        pressed_controls_.erase(it);
        if (hovered_[pointer] == id) {
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

void App::cancel(bool discard_composition) {
    if (discard_composition) {
        composition_.cancel();
    }
    keyboard_.cancel_all();
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
}
bool App::tick(double now) {
    if (!gate_.pump() && gate_.enabled) {
        cancel();
        gate_.enabled = false;
        status_ = "Keyboard connection lost. Reopen the app to reconnect.";
    }
    dirty |= keyboard_.tick(now);
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
    dirty = true;
}
void App::summon() {
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
void App::apply(Selection selection) {
    validate_selection(profiles_, selection);
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    cancel();
    japanese_latin_ = false;
    settings_.active = std::move(selection);
    keymap_ = std::move(keymap);
    pending_ = settings_.active;
    refresh_typing();
    status_.clear();
    try {
        save_selection(options_.config_dir, settings_.active);
    } catch (const std::exception& error) {
        status_ = "Applied, but not saved: " + std::string(error.what());
    }
    dirty = true;
}
void App::reload() {
    auto candidate = load_profiles(options_.data_dir, options_.config_dir, &profiles_);
    validate_selection(candidate, settings_.active);
    // Preparing a keymap can fail. Finish that work before releasing the old model.
    auto keymap = std::make_unique<LanguageMap>(candidate.languages.at(settings_.active.language));
    cancel();
    profiles_ = std::move(candidate);
    keymap_ = std::move(keymap);
    const auto updated_settings = load_settings(options_.config_dir, profiles_.errors);
    settings_.favorites = updated_settings.favorites;
    favorite_index_ = 0;
    pending_ = settings_.active;
    refresh_typing();
    status_ = profiles_.errors.empty() ? "" : profiles_.errors.front();
}
bool App::japanese() const {
    return profiles_.languages.at(settings_.active.language).input_method.starts_with("japanese-");
}
void App::refresh_typing() {
    const auto& language = profiles_.languages.at(settings_.active.language);
    text_ready_ = japanese() && gate_.text_available();
    // Integrated IME consumes characters locally; raw shortcuts still require
    // the declared physical keymap. External JIS uses the ordinary strict gate.
    const bool matching = target_language_ && same_keymap(*target_language_, language) &&
                          (japanese() || options_.target_language == settings_.active.language);
    gate_.enabled = interaction_active_ && options_.start_enabled && options_.mode == "vr" &&
                    (options_.input == "ei" || options_.input == "uinput") && matching &&
                    (!japanese() || text_ready_) && gate_.pump();
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
                composition_.convert();
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
    const auto legend = keymap_->legend(key, mods, keyboard_.caps(), keyboard_.num());
    if (legend.size() == 1 && g_ascii_isprint(legend[0]) && !is_modifier(key_code(code))) {
        if (execute) {
            if (composition_.converting()) {
                commit_japanese();
                if (!composition_.empty()) {
                    return true;
                }
            }
            composition_.roman(legend[0]);
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
    if (id.starts_with("preset-ja-")) {
        pending_.language = id.substr(7);
        pending_.layout = pending_.language == "ja-romaji" ? "en-us-full" : "ja-jis-full";
        return;
    }
    if (id.starts_with("ime-")) {
        if (id == "ime-toggle") {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        } else if (id == "ime-convert") {
            composition_.convert();
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
    if (id == "close") {
        cancel();
        gate_.enabled = false;
        quit_ = true;
    } else if (id == "release") {
        cancel();
    } else if (id == "settings") {
        if (settings_open_) {
            cancel();
            settings_open_ = false;
        } else {
            show_settings();
        }
    } else if (id == "recenter") {
        cancel();
        placement_actions_.clear();
        recenter_ = true;
    } else if (id == "apply") {
        apply(pending_);
        settings_open_ = false;
    } else if (id == "reload") {
        reload();
    } else if (id == "layout-next" || id == "layout-prev") {
        cycle(profiles_.layouts, pending_.layout, id.ends_with("next"));
    } else if (id == "language-next" || id == "language-prev") {
        cycle(profiles_.languages, pending_.language, id.ends_with("next"));
        if (pending_.language == "ja-kana" || pending_.language == "ja-jis") {
            pending_.layout = "ja-jis-full";
        }
    } else if (id == "theme-next" || id == "theme-prev") {
        cycle(profiles_.themes, pending_.theme, id.ends_with("next"));
    } else if ((id == "favorite-next" || id == "favorite-prev") && !settings_.favorites.empty()) {
        const auto count = settings_.favorites.size();
        favorite_index_ = (favorite_index_ + (id.ends_with("next") ? 1 : count - 1)) % count;
    } else if (id == "favorite-use") {
        const auto& candidate = settings_.favorites.at(favorite_index_).selection;
        validate_selection(profiles_, candidate);
        pending_ = candidate;
    }
}
bool App::take_recenter() {
    const bool result = recenter_;
    recenter_ = false;
    return result;
}
} // namespace framekeyboard
