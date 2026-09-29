#include "framekeyboard/app.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace framekeyboard {
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
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
}
std::vector<Control> App::controls() const {
    std::vector<Control> result = {
        {"settings", settings_open_ ? "Back" : "Settings", {18, 12, 132, 42}},
        {"release", "Release all", {160, 12, 155, 42}},
        {"input", gate_.enabled ? "Input on" : "Input off", {325, 12, 155, 42}, gate_.enabled},
        {"recenter", "Recenter", {490, 12, 125, 42}},
        {"close", "Close", {1450, 12, 132, 42}}};
    if (!settings_open_) {
        return result;
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
    v.status = status_;
    v.settings = settings_open_;
    for (const auto& [pointer, id] : hovered_) {
        (void)pointer;
        v.hovered.insert(id);
    }
    return v;
}
void App::move(unsigned pointer, double x, double y) {
    const auto v = view();
    std::string id;
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            id = control.id;
        }
    }
    if (id.empty()) {
        if (const auto* key = renderer.hit_key(v, x, y)) {
            id = key->id;
        }
    }
    if (hovered_[pointer] != id) {
        hovered_[pointer] = id;
        dirty = true;
    }
}
void App::down(unsigned pointer, double x, double y, double now) {
    move(pointer, x, y);
    const auto v = view();
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            pressed_controls_[pointer] = control.id;
            return;
        }
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        keyboard_.down(pointer, *key, now);
        dirty = true;
    }
}
void App::up(unsigned pointer, double x, double y) {
    keyboard_.up(pointer);
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
}
void App::cancel() {
    keyboard_.cancel_all();
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
}
bool App::tick(double now) {
    dirty |= keyboard_.tick(now);
    return dirty || renderer.animating();
}
void App::paint(double now) {
    renderer.paint(view(), now);
    dirty = false;
}
void App::show_settings() {
    cancel();
    settings_open_ = true;
    pending_ = settings_.active;
    dirty = true;
}
void App::apply(Selection selection) {
    validate_selection(profiles_, selection);
    if (gate_.enabled && selection.language != options_.target_language) {
        throw std::runtime_error(
            "Turn input off before selecting another language. Target keymap must match.");
    }
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    cancel();
    settings_.active = std::move(selection);
    keymap_ = std::move(keymap);
    pending_ = settings_.active;
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
    gate_.enabled = false;
    profiles_ = std::move(candidate);
    keymap_ = std::move(keymap);
    const auto updated_settings = load_settings(options_.config_dir, profiles_.errors);
    settings_.favorites = updated_settings.favorites;
    favorite_index_ = 0;
    pending_ = settings_.active;
    status_ = profiles_.errors.empty() ? "Profiles reloaded. Input off." : profiles_.errors.front();
}
void App::action(const std::string& id) {
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
        recenter_ = true;
    } else if (id == "input") {
        cancel();
        if (gate_.enabled) {
            gate_.enabled = false;
            status_.clear();
        } else {
            if (options_.input != "uinput") {
                throw std::runtime_error("Preview only. Launch with --input uinput to enable typing.");
            }
            if (options_.target_language != settings_.active.language) {
                throw std::runtime_error(
                    "Selected language must match --target-language and the target session keymap.");
            }
            gate_.enabled = true;
            status_.clear();
        }
    } else if (id == "apply") {
        apply(pending_);
        settings_open_ = false;
    } else if (id == "reload") {
        reload();
    } else if (id == "layout-next" || id == "layout-prev") {
        cycle(profiles_.layouts, pending_.layout, id.ends_with("next"));
    } else if (id == "language-next" || id == "language-prev") {
        cycle(profiles_.languages, pending_.language, id.ends_with("next"));
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
