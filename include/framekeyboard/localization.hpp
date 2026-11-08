#pragma once
// Interface text in the user's language, from per-language JSON files
// (docs/localization-design.md). Read on App's thread only.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct json_object;

namespace framekeyboard {
namespace fs = std::filesystem;

// A template argument: a count chooses plural forms and is written with the
// language's digit grouping; text chooses named options.
using UiValue = std::variant<long long, std::string>;
using UiArgs = std::vector<std::pair<std::string, UiValue>>;

// Interface text by key with its arguments, resolved when shown. Modules
// return these, so worker threads never touch the language tables.
struct UiText {
    std::string key;
    UiArgs args;
};
inline UiText ui(std::string key, UiArgs args = {}) {
    return {std::move(key), std::move(args)};
}

// Where the system's language comes from: Steam's own setting, which is
// SteamOS's system language, then the POSIX locale variables.
struct SystemLanguage {
    std::string steam; // Steam's name for it, such as "schinese"
    std::string posix; // such as "zh_CN.UTF-8"
};
// Steam's language in the text of its registry.vdf, or empty.
std::string steam_language(std::string_view registry);
// Reads <home>/.steam/registry.vdf, then LC_ALL, LC_MESSAGES and LANG.
SystemLanguage system_language(const fs::path& home);
// The system language, read again only after Steam's registry file changed:
// asked each time Settings opens, it costs one stat.
class SystemLanguageWatch {
  public:
    explicit SystemLanguageWatch(fs::path home) : home_(std::move(home)) {}
    SystemLanguage operator()();

  private:
    fs::path home_;
    std::optional<fs::file_time_type> seen_;
    SystemLanguage last_;
    bool read_{};
};

// The CLDR plural category (zero, one, two, few, many or other) that a
// language's rules pick for a count. Rules see integers only: n and i are
// the count, and v, w, f, t, c and e are 0.
std::string plural_category(const std::vector<std::pair<std::string, std::string>>& rules,
                            long long count);
bool valid_plural_rule(std::string_view rule);

struct LocaleInfo {
    std::string tag, name, native_name;
};

class Localization {
  public:
    // The bundled files, then <data_dir>/locales, then <user_dir>/locales. A
    // later file for the same language replaces the fields and keys it has.
    // A damaged file is skipped and reported in errors().
    void load(const fs::path& data_dir, const fs::path& user_dir);
    // The interface language: `tag` when it is loaded (or the pseudo-language),
    // else the language `system` names, else English. Unchanged, it does nothing.
    void choose(const SystemLanguage& system, const std::string& tag = {});
    const std::string& tag() const { return chain_.empty() ? english_ : chain_.front().tag; }
    // Every loaded language by English name, then the pseudo-language.
    std::vector<LocaleInfo> locales() const;
    // A key's text in the chosen language, else its fallback, else English;
    // a key found nowhere comes back as itself.
    std::string text(std::string_view key, const UiArgs& args = {}) const;
    std::string text(const UiText& message) const { return text(message.key, message.args); }
    std::string number(long long value) const;
    // Pango's family list for interface text in the chosen language.
    std::string fonts() const;
    const std::vector<std::string>& errors() const { return errors_; }
    // Generated from English, longer and accented, for finding cut text and
    // text that skipped the tables (docs/localization-design.md).
    static constexpr const char* pseudo_tag = "qps";

  private:
    // Plain text, or a branch choosing among named options by one argument.
    struct Node {
        std::string name; // the option's name within its branch
        std::string text;
        std::string by;
        std::vector<Node> options;
    };
    struct Locale {
        std::string tag, name, native_name, fallback, fonts, group{","};
        int group_min{4};
        std::vector<std::string> steam, posix;
        std::vector<std::pair<std::string, std::string>> plural; // category, CLDR rule
        std::unordered_map<std::string, Node> strings;
    };
    // A language's files, lowest priority first: bundled text, or a path read
    // again when that language is chosen.
    struct Source {
        std::string_view bundled;
        fs::path path;
    };
    static Node node(json_object* value, const std::string& key, int depth);
    // Applies the fields and keys a file has; with_strings false only checks them.
    static void apply(Locale& locale, json_object* root, bool with_strings);
    static std::string resolve(const Locale& locale, const Node& node, const UiArgs& args);
    static std::string format(const Locale& locale, std::string_view text, const UiArgs& args);
    static std::string number(const Locale& locale, long long value);
    static std::string source_text(const Source& source);
    Locale full(const std::string& tag) const;
    static Locale pseudo(const Locale& english);
    std::string match(const SystemLanguage& system) const;

    std::map<std::string, std::vector<Source>> sources_; // by tag
    std::map<std::string, Locale> info_;                 // by tag, without strings
    std::vector<Locale> chain_;                          // the chosen language, its fallbacks, English
    std::vector<std::string> errors_;
    std::string english_{"en"};
};
} // namespace framekeyboard
