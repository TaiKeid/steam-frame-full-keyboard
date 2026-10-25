#pragma once

#include "layout.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace framekeyboard {
namespace fs = std::filesystem;
struct Color {
    bool operator==(const Color&) const = default;
    double r{}, g{}, b{};
};
struct Theme {
    bool operator==(const Theme&) const = default;
    std::string id, name;
    Color surface, top, middle, bottom, side_top, side_bottom, legend;
    Color hover, latched, disabled;
    double surface_radius{}, radius{}, depth{}, travel{}, duration_ms{};
    double font_size{20}, small_font_size{13}, padding{20};
};
// One key of a 12-key flick layout. Repeated taps cycle through `tap` (toggle
// input); a flick types its direction's text. The marks ゛, ゜ and 小 change the
// preceding kana instead, and ゛゜小 cycles through its variants.
struct FlickKey {
    bool operator==(const FlickKey&) const = default;
    std::vector<std::string> tap;
    std::string left, up, right, down;
};
struct Language {
    bool operator==(const Language&) const = default;
    std::string id, name, locale, rules, model, keymap, variant, options, font;
    std::string input_method{"xkb"};
    std::map<std::string, std::string> kana, kana_shift;
    std::map<std::string, std::string> composition_keys, composition_shift;
    std::map<std::string, std::string> overrides;
    // Flick slot -> kana. A flick language needs every slot and a flick layout
    // serves only flick languages.
    std::map<std::string, FlickKey> flick;
    std::vector<std::string> required_keys;
    // Optional swipe word list ID; empty means the profile does not offer swipe.
    std::string swipe_lexicon;
};
struct Selection {
    bool operator==(const Selection&) const = default;
    std::string layout{"en-us-full"}, language{"en-us"}, theme{"graphite"};
};
struct Favorite {
    std::string id, name;
    Selection selection;
};
struct Profiles {
    std::map<std::string, Layout> layouts;
    std::map<std::string, Language> languages;
    std::map<std::string, Theme> themes;
    std::map<std::string, std::string> sources;
    std::vector<std::string> errors;
};
struct Settings {
    Selection active;
    std::vector<Favorite> favorites;
    bool pinned{};
    bool num_lock{};
    bool hide_numpad{};
    bool use_drag_to_move{};
    bool auto_leveling{true};
    bool replace_native_keyboard{};
    bool start_with_steamos{};
    // Active only when the current language also supports swipe.
    bool swipe_typing{};
    // Return to the keyboard after typing an emoji.
    bool emoji_close_on_select{};
};

fs::path default_config_dir();
// Defaults are embedded. Reload may retain a previous valid user override on errors.
Profiles load_profiles(const fs::path& data_dir, const fs::path& user_dir,
                       const Profiles* previous = nullptr);
Settings load_settings(const fs::path& user_dir, std::vector<std::string>& errors);
void save_selection(const fs::path& user_dir, const Selection& selection);
void save_settings(const fs::path& user_dir, const Settings& settings);
void validate_selection(const Profiles& profiles, const Selection& selection);
// Geometry/profile checks for callers that prepare the XKB map themselves.
void validate_selection_profiles(const Profiles& profiles, const Selection& selection);
Layout parse_layout(const std::string& json);
Theme parse_theme(const std::string& json);
Language parse_language(const std::string& json);

} // namespace framekeyboard
