#include "framekeyboard/localization.hpp"
#include "bundled_profiles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iterator>
#include <json-c/json.h>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

// A file larger than this is no language file.
constexpr std::size_t file_limit = 2 << 20;

Json parse(const std::string& text) {
    auto* parser = json_tokener_new();
    Json value(json_tokener_parse_ex(parser, text.c_str(), static_cast<int>(text.size())),
               json_object_put);
    const auto error = json_tokener_get_error(parser);
    json_tokener_free(parser);
    if (error != json_tokener_success || !value || !json_object_is_type(value.get(), json_type_object)) {
        throw std::runtime_error("expected a valid JSON object");
    }
    return value;
}
json_object* member(json_object* object, const char* name, json_type type) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value)) {
        return nullptr;
    }
    if (!json_object_is_type(value, type)) {
        throw std::runtime_error(std::string("wrong type: ") + name);
    }
    return value;
}
// Interface text may hold line breaks, but no other control characters.
std::string string_of(json_object* value, const std::string& what, std::size_t limit) {
    std::string result(json_object_get_string(value),
                       static_cast<std::size_t>(json_object_get_string_len(value)));
    if (result.size() > limit ||
        !g_utf8_validate(result.c_str(), static_cast<gssize>(result.size()), nullptr) ||
        std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 32 && c != '\n'; })) {
        throw std::runtime_error("invalid text: " + what);
    }
    return result;
}
std::vector<std::string> strings_of(json_object* array, const std::string& what, std::size_t count) {
    const auto length = json_object_array_length(array);
    if (length > count) {
        throw std::runtime_error("too many entries: " + what);
    }
    std::vector<std::string> result;
    for (std::size_t i = 0; i < length; ++i) {
        auto* entry = json_object_array_get_idx(array, i);
        if (!json_object_is_type(entry, json_type_string)) {
            throw std::runtime_error("wrong type: " + what);
        }
        result.push_back(string_of(entry, what, 80));
    }
    return result;
}
// BCP 47 tags as the files use them: en, pt-BR, zh-Hant, es-419.
bool valid_tag(const std::string& tag) {
    return tag.size() >= 2 && tag.size() <= 16 && std::isalpha(static_cast<unsigned char>(tag[0])) &&
           std::all_of(tag.begin(), tag.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '-'; });
}
// Dotted, lower case: status.wait-for-typing.
bool valid_key(const std::string& key) {
    return !key.empty() && key.size() <= 96 && std::islower(static_cast<unsigned char>(key[0])) &&
           std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return std::islower(c) || std::isdigit(c) || c == '.' || c == '-';
           });
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
constexpr std::array<const char*, 5> plural_categories{"zero", "one", "two", "few", "many"};

// A CLDR plural condition for an integer count, evaluated as it is parsed:
// `or` of `and` of relations such as `i % 10 = 2..4` or `v != 0`. Sample
// lists after `@` are ignored.
class PluralRule {
  public:
    PluralRule(std::string_view text, long long count)
        : text_(text.substr(0, text.find('@'))), count_(count < 0 ? -count : count) {}
    std::optional<bool> evaluate() {
        auto value = condition();
        skip();
        if (!value || pos_ != text_.size()) {
            return std::nullopt;
        }
        return value;
    }

  private:
    std::optional<bool> condition() {
        auto value = all();
        while (value && word("or")) {
            const auto next = all();
            if (!next) {
                return std::nullopt;
            }
            value = *value || *next;
        }
        return value;
    }
    std::optional<bool> all() {
        auto value = relation();
        while (value && word("and")) {
            const auto next = relation();
            if (!next) {
                return std::nullopt;
            }
            value = *value && *next;
        }
        return value;
    }
    std::optional<bool> relation() {
        const auto left = expression();
        if (!left) {
            return std::nullopt;
        }
        bool negate = false;
        if (symbol("!=")) {
            negate = true;
        } else if (!symbol("=")) {
            return std::nullopt;
        }
        bool found = false;
        do {
            const auto low = integer();
            auto high = low;
            if (low && symbol("..")) {
                high = integer();
            }
            if (!low || !high) {
                return std::nullopt;
            }
            found |= *left >= *low && *left <= *high;
        } while (symbol(","));
        return negate ? !found : found;
    }
    // An operand, optionally modulo a number. Counts are integers: n and i
    // are the count, every fraction operand is 0.
    std::optional<long long> expression() {
        skip();
        if (pos_ >= text_.size() || !std::strchr("nivwftce", text_[pos_]) ||
            (pos_ + 1 < text_.size() && std::isalpha(static_cast<unsigned char>(text_[pos_ + 1])))) {
            return std::nullopt;
        }
        const char operand = text_[pos_++];
        long long value = operand == 'n' || operand == 'i' ? count_ : 0;
        if (symbol("%") || word("mod")) {
            const auto divisor = integer();
            if (!divisor || *divisor == 0) {
                return std::nullopt;
            }
            value %= *divisor;
        }
        return value;
    }
    std::optional<long long> integer() {
        skip();
        long long value = 0;
        std::size_t digits = 0;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) &&
               digits < 18) {
            value = value * 10 + (text_[pos_++] - '0');
            ++digits;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        return value;
    }
    bool word(std::string_view expected) {
        skip();
        if (text_.substr(pos_, expected.size()) != expected ||
            (pos_ + expected.size() < text_.size() &&
             std::isalpha(static_cast<unsigned char>(text_[pos_ + expected.size()])))) {
            return false;
        }
        pos_ += expected.size();
        return true;
    }
    bool symbol(std::string_view expected) {
        skip();
        if (text_.substr(pos_, expected.size()) != expected) {
            return false;
        }
        pos_ += expected.size();
        return true;
    }
    void skip() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }
    std::string_view text_;
    long long count_;
    std::size_t pos_{};
};

// English made about 40% longer and accented, in brackets, with its
// placeholders kept: text that overflows or never went through the tables
// shows at once.
std::string pseudo_text(const std::string& text) {
    static const std::map<char, const char*> accents{
        {'a', "á"}, {'b', "ƀ"}, {'c', "ç"}, {'d', "ð"}, {'e', "é"}, {'f', "ƒ"}, {'g', "ĝ"},
        {'h', "ĥ"}, {'i', "î"}, {'j', "ĵ"}, {'k', "ķ"}, {'l', "ļ"}, {'m', "ɱ"}, {'n', "ñ"},
        {'o', "ö"}, {'p', "þ"}, {'q', "ǫ"}, {'r', "ŕ"}, {'s', "š"}, {'t', "ţ"}, {'u', "ü"},
        {'v', "ṽ"}, {'w', "ŵ"}, {'x', "ẋ"}, {'y', "ý"}, {'z', "ž"}, {'A', "Å"}, {'C', "Ç"},
        {'E', "É"}, {'I', "Î"}, {'N', "Ñ"}, {'O', "Ö"}, {'S', "Š"}, {'U', "Ü"}, {'Y', "Ý"}};
    std::string result = "[";
    std::size_t characters = 0;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            result.append(2, c);
            i += 2;
            ++characters;
            continue;
        }
        if (const auto end = text.find('}', i); c == '{' && end != std::string::npos) {
            result.append(text, i, end - i + 1);
            characters += 4; // A value's likely width.
            i = end + 1;
            continue;
        }
        const auto accent = accents.find(c);
        result += accent != accents.end() ? accent->second : std::string(1, c);
        characters += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        ++i;
    }
    // Words that wrap like text.
    const auto extra = (characters * 2 + 4) / 5;
    for (std::size_t i = 0; i < extra; ++i) {
        result += i % 5 == 0 ? " " : "ŵ";
    }
    return result + "]";
}
} // namespace

std::string plural_category(const std::vector<std::pair<std::string, std::string>>& rules,
                            long long count) {
    for (const auto& [category, rule] : rules) {
        if (PluralRule(rule, count).evaluate().value_or(false)) {
            return category;
        }
    }
    return "other";
}
bool valid_plural_rule(std::string_view rule) {
    const auto body = rule.substr(0, rule.find('@'));
    return body.find_first_not_of(" \t") != std::string_view::npos &&
           PluralRule(rule, 0).evaluate().has_value();
}

// The Registry/HKCU/Software/Valve/Steam "language" value of Steam's
// registry.vdf: quoted keys, each followed by a quoted value or a block.
std::string steam_language(std::string_view registry) {
    std::size_t pos = 0;
    enum class Token { End, Text, Open, Close };
    const auto next = [&](std::string& text) {
        while (pos < registry.size() && std::isspace(static_cast<unsigned char>(registry[pos]))) {
            ++pos;
        }
        if (pos >= registry.size()) {
            return Token::End;
        }
        if (registry[pos] == '{' || registry[pos] == '}') {
            return registry[pos++] == '{' ? Token::Open : Token::Close;
        }
        text.clear();
        if (registry[pos] == '"') {
            for (++pos; pos < registry.size() && registry[pos] != '"'; ++pos) {
                if (registry[pos] == '\\' && pos + 1 < registry.size()) {
                    ++pos;
                }
                text += registry[pos];
            }
            ++pos;
            return Token::Text;
        }
        while (pos < registry.size() && !std::isspace(static_cast<unsigned char>(registry[pos])) &&
               !std::strchr("{}\"", registry[pos])) {
            text += registry[pos++];
        }
        return Token::Text;
    };
    static const std::vector<std::string> steam{"registry", "hkcu", "software", "valve", "steam"};
    std::vector<std::string> path;
    std::string key, value;
    for (auto token = next(key); token != Token::End; token = next(key)) {
        if (token == Token::Close) {
            if (!path.empty()) {
                path.pop_back();
            }
            continue;
        }
        if (token == Token::Open) {
            path.emplace_back(); // A block without a name.
            continue;
        }
        const auto after = next(value);
        if (after == Token::Open) {
            path.push_back(lower(key));
        } else if (after == Token::Text) {
            if (path == steam && lower(key) == "language") {
                return value;
            }
        } else if (after == Token::Close && !path.empty()) {
            path.pop_back();
        } else if (after == Token::End) {
            break;
        }
    }
    return {};
}
SystemLanguage system_language(const fs::path& home) {
    SystemLanguage result;
    if (!home.empty()) {
        std::ifstream file(home / ".steam" / "registry.vdf", std::ios::binary);
        std::string text(file_limit, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, file.gcount())));
        result.steam = steam_language(text);
    }
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* value = std::getenv(name); value && *value) {
            result.posix = value;
            break;
        }
    }
    return result;
}

Localization::Node Localization::node(json_object* value, const std::string& key, int depth) {
    Node result;
    if (json_object_is_type(value, json_type_string)) {
        result.text = string_of(value, key, 2000);
        return result;
    }
    if (!json_object_is_type(value, json_type_object) || depth > 3) {
        throw std::runtime_error("invalid text: " + key);
    }
    bool other = false;
    json_object_object_foreach(value, name, option) {
        if (std::strcmp(name, "by") == 0) {
            if (!json_object_is_type(option, json_type_string)) {
                throw std::runtime_error("invalid branch: " + key);
            }
            result.by = string_of(option, key, 64);
            continue;
        }
        auto child = node(option, key, depth + 1);
        child.name = name;
        other |= child.name == "other";
        result.options.push_back(std::move(child));
    }
    if (result.by.empty() || !other) {
        throw std::runtime_error("a branch needs \"by\" and \"other\": " + key);
    }
    return result;
}
void Localization::apply(Locale& locale, json_object* root, bool with_strings) {
    if (auto* value = member(root, "locale", json_type_string)) {
        locale.tag = string_of(value, "locale", 16);
    }
    if (!valid_tag(locale.tag)) {
        throw std::runtime_error("expected a locale tag");
    }
    if (locale.tag == pseudo_tag) {
        throw std::runtime_error("the pseudo-language is generated");
    }
    if (auto* value = member(root, "name", json_type_string)) {
        locale.name = string_of(value, "name", 80);
    }
    if (auto* value = member(root, "native_name", json_type_string)) {
        locale.native_name = string_of(value, "native_name", 80);
    }
    if (auto* value = member(root, "fallback", json_type_string)) {
        locale.fallback = string_of(value, "fallback", 16);
        if (!valid_tag(locale.fallback)) {
            throw std::runtime_error("invalid fallback");
        }
    }
    if (auto* value = member(root, "steam", json_type_array)) {
        locale.steam = strings_of(value, "steam", 8);
    }
    if (auto* value = member(root, "posix", json_type_array)) {
        locale.posix = strings_of(value, "posix", 16);
    }
    if (auto* value = member(root, "font_families", json_type_array)) {
        const auto families = strings_of(value, "font_families", 8);
        if (families.empty()) {
            throw std::runtime_error("expected 1 to 8 font families");
        }
        locale.fonts.clear();
        for (const auto& family : families) {
            locale.fonts += (locale.fonts.empty() ? "" : ",") + family;
        }
    }
    if (auto* value = member(root, "plural", json_type_object)) {
        std::vector<std::pair<std::string, std::string>> rules;
        json_object_object_foreach(value, category, rule) {
            if (std::find_if(plural_categories.begin(), plural_categories.end(),
                             [&](const char* name) { return std::strcmp(name, category) == 0; }) ==
                    plural_categories.end() ||
                !json_object_is_type(rule, json_type_string)) {
                throw std::runtime_error(std::string("invalid plural category: ") + category);
            }
            auto text = string_of(rule, category, 400);
            if (!valid_plural_rule(text)) {
                throw std::runtime_error(std::string("invalid plural rule: ") + category);
            }
            rules.emplace_back(category, std::move(text));
        }
        // CLDR's order, whatever the file's.
        const auto rank = [](const std::string& category) {
            return std::find(plural_categories.begin(), plural_categories.end(), category) -
                   plural_categories.begin();
        };
        std::sort(rules.begin(), rules.end(),
                  [&](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
        locale.plural = std::move(rules);
    }
    if (auto* number = member(root, "number", json_type_object)) {
        if (auto* value = member(number, "group", json_type_string)) {
            locale.group = string_of(value, "group", 8);
        }
        if (auto* value = member(number, "group_min", json_type_int)) {
            const auto digits = json_object_get_int(value);
            if (digits < 1 || digits > 19) {
                throw std::runtime_error("invalid group_min");
            }
            locale.group_min = digits;
        }
    }
    if (auto* strings = member(root, "strings", json_type_object)) {
        json_object_object_foreach(strings, key, value) {
            if (!valid_key(key)) {
                throw std::runtime_error(std::string("invalid key: ") + key);
            }
            auto parsed = node(value, key, 0);
            if (with_strings) {
                locale.strings[key] = std::move(parsed);
            }
        }
    }
}
std::string Localization::source_text(const Source& source) {
    if (source.path.empty()) {
        return std::string(source.bundled);
    }
    std::ifstream file(source.path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read");
    }
    std::string text(file_limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (static_cast<std::size_t>(file.gcount()) > file_limit) {
        throw std::runtime_error("file too large");
    }
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}
void Localization::load(const fs::path& data_dir, const fs::path& user_dir) {
    sources_.clear();
    info_.clear();
    chain_.clear();
    errors_.clear();
    const auto add = [&](const Source& source, const std::string& origin) {
        try {
            const auto root = parse(source_text(source));
            Locale checked;
            apply(checked, root.get(), false); // the whole file, before any of it counts
            apply(info_[checked.tag], root.get(), false);
            sources_[checked.tag].push_back(source);
        } catch (const std::exception& error) {
            errors_.push_back(origin + ": " + error.what());
        }
    };
    for (const auto& resource : bundled::profiles) {
        if (resource.kind == "locales") {
            add({resource.json, {}}, "bundled language file");
        }
    }
    for (const auto& directory : {data_dir, user_dir}) {
        std::error_code error;
        const auto folder = directory / "locales";
        if (directory.empty() || !fs::is_directory(folder, error)) {
            continue;
        }
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(folder, error)) {
            const auto& path = entry.path();
            // context.json holds translators' notes, not a language.
            if (path.extension() == ".json" && path.filename() != "context.json") {
                files.push_back(path);
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& path : files) {
            add({{}, path}, path.string());
        }
    }
}
Localization::Locale Localization::full(const std::string& tag) const {
    Locale locale;
    for (const auto& source : sources_.at(tag)) {
        try {
            const auto root = parse(source_text(source));
            Locale checked;
            apply(checked, root.get(), false);
            apply(locale, root.get(), true);
        } catch (const std::exception&) {
            // Changed since loading and damaged now: the others still count.
        }
    }
    return locale;
}
Localization::Locale Localization::pseudo(const Locale& english) {
    auto result = english;
    result.tag = pseudo_tag;
    result.name = "Pseudo (long)";
    result.native_name = "[Ƥšéüðö ļöñĝ]";
    result.fallback.clear();
    std::function<void(Node&)> lengthen = [&](Node& entry) {
        if (entry.by.empty()) {
            entry.text = pseudo_text(entry.text);
        }
        for (auto& option : entry.options) {
            lengthen(option);
        }
    };
    for (auto& [key, entry] : result.strings) {
        lengthen(entry);
    }
    return result;
}
std::string Localization::match(const SystemLanguage& system) const {
    for (const auto& [tag, info] : info_) {
        if (!system.steam.empty() && contains(info.steam, system.steam)) {
            return tag;
        }
    }
    const auto posix = system.posix.substr(0, system.posix.find_first_of(".@"));
    if (posix.empty() || posix == "C" || posix == "POSIX") {
        return {};
    }
    // zh_TW before zh: the whole locale, then its language alone.
    for (const auto& wanted : {posix, posix.substr(0, posix.find('_'))}) {
        for (const auto& [tag, info] : info_) {
            if (contains(info.posix, wanted)) {
                return tag;
            }
        }
    }
    return {};
}
void Localization::choose(const SystemLanguage& system, const std::string& tag) {
    auto chosen = tag == pseudo_tag || sources_.count(tag) ? tag : match(system);
    if (chosen != pseudo_tag && !sources_.count(chosen)) {
        chosen = english_;
    }
    if (!chain_.empty() && chain_.front().tag == chosen) {
        return;
    }
    chain_.clear();
    if (chosen == pseudo_tag) {
        auto english = sources_.count(english_) ? full(english_) : Locale{};
        chain_.push_back(pseudo(english));
        chain_.push_back(std::move(english));
        return;
    }
    // The chosen language, the ones it falls back to, then English.
    std::set<std::string> seen;
    for (auto next = chosen; sources_.count(next) && seen.size() < 4 && seen.insert(next).second;) {
        chain_.push_back(full(next));
        next = chain_.back().fallback;
    }
    if (!seen.count(english_) && sources_.count(english_)) {
        chain_.push_back(full(english_));
    }
}
std::vector<LocaleInfo> Localization::locales() const {
    std::vector<LocaleInfo> result;
    for (const auto& [tag, info] : info_) {
        result.push_back({tag, info.name.empty() ? tag : info.name, info.native_name});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    result.push_back({pseudo_tag, "Pseudo (long)", "[Ƥšéüðö ļöñĝ]"});
    return result;
}
std::string Localization::text(std::string_view key, const UiArgs& args) const {
    const std::string wanted(key);
    for (const auto& locale : chain_) {
        if (const auto found = locale.strings.find(wanted); found != locale.strings.end()) {
            return resolve(locale, found->second, args);
        }
    }
    return wanted;
}
std::string Localization::resolve(const Locale& locale, const Node& node, const UiArgs& args) {
    const auto* at = &node;
    while (!at->by.empty()) {
        const auto arg = std::find_if(args.begin(), args.end(),
                                      [&](const auto& entry) { return entry.first == at->by; });
        std::vector<std::string> wanted;
        if (arg != args.end()) {
            if (const auto* count = std::get_if<long long>(&arg->second)) {
                // An exact count first ("=0": no words yet), then its category.
                wanted = {"=" + std::to_string(*count), plural_category(locale.plural, *count)};
            } else {
                wanted = {std::get<std::string>(arg->second)};
            }
        }
        wanted.push_back("other");
        const Node* next = nullptr;
        for (const auto& name : wanted) {
            const auto found = std::find_if(at->options.begin(), at->options.end(),
                                            [&](const Node& option) { return option.name == name; });
            if (found != at->options.end()) {
                next = &*found;
                break;
            }
        }
        if (!next) {
            return {}; // Never: loading requires `other`.
        }
        at = next;
    }
    return format(locale, at->text, args);
}
std::string Localization::format(const Locale& locale, std::string_view text, const UiArgs& args) {
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            result += c;
            i += 2;
            continue;
        }
        const auto end = c == '{' ? text.find('}', i) : std::string_view::npos;
        if (end == std::string_view::npos) {
            result += c;
            ++i;
            continue;
        }
        const auto name = text.substr(i + 1, end - i - 1);
        const auto arg = std::find_if(args.begin(), args.end(),
                                      [&](const auto& entry) { return entry.first == name; });
        if (arg == args.end()) {
            result.append(text, i, end - i + 1); // shows, and the check tool catches it
        } else if (const auto* count = std::get_if<long long>(&arg->second)) {
            result += number(locale, *count);
        } else {
            result += std::get<std::string>(arg->second);
        }
        i = end + 1;
    }
    return result;
}
std::string Localization::number(const Locale& locale, long long value) {
    const auto magnitude = value < 0 ? 0ULL - static_cast<unsigned long long>(value)
                                     : static_cast<unsigned long long>(value);
    const auto digits = std::to_string(magnitude);
    std::string result;
    const bool grouped = static_cast<int>(digits.size()) >= locale.group_min;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (grouped && i > 0 && (digits.size() - i) % 3 == 0) {
            result += locale.group;
        }
        result += digits[i];
    }
    return (value < 0 ? "-" : "") + result;
}
std::string Localization::number(long long value) const {
    static const Locale none;
    return number(chain_.empty() ? none : chain_.front(), value);
}
std::string Localization::fonts() const {
    for (const auto& locale : chain_) {
        if (!locale.fonts.empty()) {
            return locale.fonts;
        }
    }
    return "sans-serif";
}
} // namespace framekeyboard
