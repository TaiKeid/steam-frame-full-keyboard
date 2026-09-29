#include "framekeyboard/app.hpp"

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
        cjk_ = prepare_cjk(profiles_.languages.at(settings_.active.language));
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
        {"settings",
         settings_open_ ? "Back" : "Settings",
         {18, 12, 60, 42},
         false,
         settings_open_ ? Icon::Back : Icon::Settings},
        {"recenter", "Recenter", {88, 12, 60, 42}, false, Icon::Recenter},
        {"size-smaller", "Smaller keyboard", {158, 12, 60, 42}, false, Icon::ScaleDown},
        {"size-larger", "Larger keyboard", {228, 12, 60, 42}, false, Icon::ScaleUp},
        {"close", "Close", {1522, 12, 60, 42}, false, Icon::Close}};
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
                    for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size()));
                         ++i) {
                        result.push_back({"cjk-candidate-" + std::to_string(i),
                                          std::to_string(i - page + 1) + " " +
                                              candidates[static_cast<std::size_t>(i)],
                                          {85.0 + (i - page) * 240, 148, 230, 42},
                                          i == cjk_->selected()});
                    }
                    result.push_back({"cjk-next", "↓", {1290, 148, 55, 42}});
                }
            }
        }
        return result;
    }
    const auto& language = profiles_.languages.at(pending_.language);
    const auto& layout = profiles_.layouts.at(pending_.layout);
    const bool japanese_layout = std::any_of(layout.keys.begin(), layout.keys.end(), [](const Key& key) {
        return key.action_kind == ActionKind::Key && key.action == "KanaMode";
    });
    // Use the pending selection so the options follow changes before Apply.
    if (japanese_layout || language.locale == "ja" || language.locale.starts_with("ja-") ||
        language.locale.starts_with("ja_") || language.keymap == "jp" ||
        language.input_method.starts_with("japanese-")) {
        result.push_back({"preset-ja-romaji", "日本語 Romaji", {610, 12, 230, 42}});
        result.push_back({"preset-ja-kana", "日本語 Kana", {850, 12, 230, 42}});
        result.push_back({"preset-ja-jis", "JIS (system IME)", {1090, 12, 250, 42}});
    }
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
    // Image exports show the layout without the interactive preview's input notice.
    const bool image_export = options_.mode == "render" || options_.mode == "render-settings";
    if (v.status.empty() && !gate_.enabled && !image_export) {
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
        v.status = backend_can_resume_ ? "Keyboard paused by compositor. Waiting to resume."
                                       : "Keyboard connection lost. Reopen the app to reconnect.";
    } else if ((unicode_mode() || japanese() || cjk_) && options_.input != "none" && !text_ready_) {
        v.status = "Text delivery unavailable. Reopen the app after the compositor connection recovers.";
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
        const bool ime_local = japanese_key(*key, false, modifiers) || cjk_key(*key, false, modifiers);
        const bool text_local = !ime_local && text_key(*key, modifiers);
        const bool cancel_accent = unicode_mode() && keymap_->composing() &&
                                   (key->action == "Escape" || key->action == "Backspace");
        const bool retry_text = !pending_text_.empty() && key->action == "Enter";
        const bool local = ime_local || text_local || cancel_accent || retry_text;
        if (!backend_ready_ || keyboard_.pointer_pressed(pointer)) {
            return false;
        }
        const bool sends_action = key->action_kind == ActionKind::Shortcut ||
                                  !is_modifier(key_code(key->action)) || !key->sticky;
        if (!pending_text_.empty() && sends_action) {
            if (!flush_text()) {
                dirty = true;
                return false;
            }
        }
        if (!local && (!composition_.empty() || (cjk_ && !cjk_->empty())) && sends_action) {
            // Submit before key-down: Tab/shortcuts can move focus, and the text
            // transport refuses a commit while physical keys are held.
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
        // Capture the symbol before a tapped Shift/AltGr is consumed by down().
        const auto symbol = text_local
                                ? keymap_->symbol(*key, modifiers, keyboard_.caps(), keyboard_.num())
                                : XKB_KEY_NoSymbol;
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
                    if (flush_text() && !repeated.empty()) {
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
    text_repeats_.erase(pointer);
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

void App::cancel_pointer(unsigned pointer) {
    text_repeats_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    text_repeats_.clear();
    if (discard_composition) {
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
    refresh_typing();
    dirty |= keyboard_.tick(now);
    for (auto it = text_repeats_.begin(); it != text_repeats_.end();) {
        if (now >= it->second.next) {
            if (options_.input != "none" && !gate_.commit_text(it->second.text)) {
                // Stop a repeat after an interruption; never replay a backlog.
                it = text_repeats_.erase(it);
                continue;
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
    auto cjk = prepare_cjk(candidate.languages.at(settings_.active.language));
    cancel();
    cjk_ = std::move(cjk);
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
    const bool ready = gate_.pump();
    const bool reset = gate_.take_input_reset();
    const bool can_resume = gate_.can_resume();
    if (reset || (backend_ready_ && !ready)) {
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
    gate_.enabled =
        interaction_active_ && options_.start_enabled && options_.mode == "vr" &&
        (options_.input == "ei" || options_.input == "uinput") &&
        (unicode_mode() ? text_ready_ : matching && (!(japanese() || cjk_) || text_ready_)) &&
        backend_ready_;
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
bool App::unicode_mode() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    // External JIS explicitly delegates conversion to a system IME.
    const bool external_jis = language.input_method == "xkb" && language.keymap == "jp";
    return options_.input != "uinput" && !external_jis;
}
bool App::text_key(const Key& key, const std::set<int>& mods) const {
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
    status_ = "Text not sent. Release other keys, then press Enter to retry.";
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
    if (id.starts_with("preset-ja-")) {
        pending_.language = id.substr(7);
        pending_.layout = pending_.language == "ja-romaji" ? "en-us-full" : "ja-jis-full";
        return;
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
        pending_.layout = compatible_layout(profiles_, pending_);
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
