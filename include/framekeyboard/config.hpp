#pragma once

#include "layout.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace framekeyboard {
namespace fs = std::filesystem;
// A language profile's dictation prompt, in code points; the rest of Whisper's
// 200 is the editor's context.
constexpr std::size_t max_dictation_prompt = 60;
struct Color {
    bool operator==(const Color&) const = default;
    double r{}, g{}, b{};
};
struct Theme {
    bool operator==(const Theme&) const = default;
    std::string id, name;
    Color surface, top, middle, bottom, side_top, side_bottom, legend;
    Color hover, latched, disabled;
    // Notice colours for text on the case and cards; their defaults depend on
    // whether the legend is light (dark theme) or dark (light theme).
    Color warning, error;
    // A status icon that needs attention, such as Keep open typing out of
    // sight: a vivid orange that stands apart from the legend on the keycap.
    Color alert;
    // The latched fill runs from latched_top through latched to latched_bottom
    // (flat by default); latched_legend is the text on it (the legend by
    // default). The finish is "flat" or "neon" (a dark face inside a glowing
    // tube, also drawn on pressed keys).
    Color latched_top, latched_bottom, latched_legend;
    std::string latched_finish{"flat"};
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
    // Whisper language code for dictation; empty means the profile has no dictation.
    std::string dictation_language;
    // Text put before each phrase's context in Whisper's prompt, to fix the
    // script or punctuation style (docs/dictation.md, Script prompts).
    std::string dictation_prompt;
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
    // Replacement mode: the panel stays open after its request ends (docs/configuration.md).
    bool keep_open{};
    bool num_lock{};
    bool hide_numpad{};
    // The case behind the keys (docs/configuration.md, Background):
    // "visible", "semi-transparent" (half opaque), "invisible" (clear, still
    // taking the laser) or "disabled" (clear, the laser passes through its
    // gaps). Keys keep their theme.
    std::string background_surface{"visible"};
    bool use_drag_to_move{};
    bool auto_leveling{true};
    bool replace_native_keyboard{};
    bool start_with_steamos{};
    // Active only when the current language also supports swipe.
    bool swipe_typing{};
    // Return to the keyboard after typing an emoji.
    bool emoji_close_on_select{};
    // SteamOS's Desktop: non-ASCII text is pasted instead of typed as Unicode entry.
    bool desktop_unicode_paste{};
    // Dictation (docs/dictation.md). The Dictation card in Settings edits the
    // mode and server; the URL and model are kept as they are in the file. An
    // invalid value turns dictation off without affecting any other setting.
    // "off", "server" or "device"; on device by default, which needs the
    // model downloaded first (owner, 2026-10-27).
    std::string dictation{"device"};
    std::string dictation_server; // host[:port]
    // A shell set dictation_server over the protocol (docs/integration.md,
    // Dictation): its phrases go without the editor's text as context.
    bool dictation_server_by_shell{};
    std::string dictation_url; // or a full base URL
    std::string dictation_model;
    // Milliseconds between characters while Send types: 20, 30 or 40. Slower
    // keeps their order under heavy load (docs/dictation.md, Send).
    int dictation_send_gap{20};
    // Send pastes through the clipboard instead of typing; Steam's own fields
    // keep their direct path.
    bool dictation_send_paste{};
    // Type as I speak: the mic key captures on the normal keyboard and each
    // phrase is typed into the target as soon as it is recognized.
    bool dictation_direct{};
    // In Server mode, a phrase the server cannot recognize is recognized on
    // the Frame, once its model is downloaded.
    bool dictation_fallback{};
    // The on-device model: "small-q8_0" (the default), or for comparison in
    // development builds "small-q5_1" or "small-q4_0".
    std::string dictation_device_model{"small-q8_0"};
    // Shows development-only rows in Settings; set by hand in config.json.
    bool developer_options{};
    // Up to five servers that passed the Dictation card's Test, newest first.
    std::vector<std::string> dictation_recent_servers;
};
// Whether a server address or URL is one the dictation helper accepts.
bool valid_dictation_server(const std::string& server);
bool valid_background_surface(const std::string& surface);
// The on-device models (docs/dictation.md, On-device recognition): whether a
// name is one, the file it is used from, and its download size in MB.
bool valid_device_model(const std::string& model);
std::string device_model_file(const std::string& model);
int device_model_megabytes(const std::string& model);
// The size kept once downloaded: smaller for 4-bit, which is made from f16.
int device_model_kept_megabytes(const std::string& model);
// Whether a valid server address is on the user's own network: private and
// link-local IPv4 and IPv6, loopback, Tailscale's 100.64.0.0/10 and ts.net
// names, single-label names and the local suffixes. Others get a warning,
// as speech to them would cross the internet unencrypted.
bool local_dictation_server(const std::string& server);
bool valid_dictation_url(const std::string& url);

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
// The selection's layout if it can type the language, else the smallest
// layout that can.
std::string compatible_layout(const Profiles& profiles, const Selection& selection);
// Whether the profiles of a selection fit together.
bool fits(const Profiles& profiles, const Selection& selection);
Layout parse_layout(const std::string& json);
Theme parse_theme(const std::string& json);
Language parse_language(const std::string& json);

} // namespace framekeyboard
