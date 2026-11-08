// Interface text from per-language files (docs/localization-design.md):
// plural rules, templates and branches, the fallback chain, language
// detection and the pseudo-language.
#include "framekeyboard/localization.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
using Rules = std::vector<std::pair<std::string, std::string>>;
const Rules english{{"one", "i = 1 and v = 0"}};
const Rules russian{
    {"one", "v = 0 and i % 10 = 1 and i % 100 != 11"},
    {"few", "v = 0 and i % 10 = 2..4 and i % 100 != 12..14"},
    {"many", "v = 0 and i % 10 = 0 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 11..14"}};
const Rules polish{
    {"one", "i = 1 and v = 0"},
    {"few", "v = 0 and i % 10 = 2..4 and i % 100 != 12..14"},
    {"many", "v = 0 and i != 1 and i % 10 = 0..1 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = "
             "12..14 @integer 0, 5~19, 100"}};
void plural_tests() {
    require(plural_category(english, 1) == "one" && plural_category(english, 0) == "other" &&
                plural_category(english, 2) == "other" && plural_category(english, 21) == "other" &&
                plural_category(english, -1) == "one",
            "English: one for 1 alone");
    const std::pair<long long, const char*> russian_cases[]{
        {1, "one"},  {21, "one"},  {101, "one"}, {2, "few"},   {4, "few"},   {22, "few"},  {0, "many"},
        {5, "many"}, {11, "many"}, {12, "many"}, {14, "many"}, {25, "many"}, {111, "many"}};
    for (const auto& [count, category] : russian_cases) {
        require(plural_category(russian, count) == category,
                "Russian " + std::to_string(count) + " is " + category);
    }
    const std::pair<long long, const char*> polish_cases[]{
        {1, "one"}, {2, "few"}, {22, "few"}, {0, "many"}, {5, "many"}, {12, "many"}, {21, "many"}};
    for (const auto& [count, category] : polish_cases) {
        require(plural_category(polish, count) == category,
                "Polish " + std::to_string(count) + " is " + category);
    }
    require(plural_category({{"one", "i = 0,1"}}, 0) == "one" &&
                plural_category({{"one", "i = 0,1"}}, 2) == "other",
            "French: one for 0 and 1");
    require(plural_category({}, 1) == "other", "no rules: other");
    for (const char* rule : {"i = 1", "n mod 10 = 1", "v = 0 and i % 10 = 2..4", "i = 1 @integer 1"}) {
        require(valid_plural_rule(rule), std::string("valid: ") + rule);
    }
    for (const char* rule : {"", "  ", "i = ", "x = 1", "i % = 2", "i = 1 and", "in = 1", "i = 1..",
                             "i = 1 or or i = 2", "i % 0 = 1", "i == 1"}) {
        require(!valid_plural_rule(rule), std::string("invalid: '") + rule + "'");
    }
}
void steam_tests() {
    const std::string frame = R"("Registry"
{
	"HKCU"
	{
		"Software"
		{
			"Valve"
			{
				"Steamsteamglobal"
				{
					"language"		"english"
				}
				"Steam"
				{
					"language"		"schinese"
					"SourceModInstallPath"		"/home/user/.local/share/Steam/steamapps\\sourcemods"
				}
			}
		}
	}
})";
    require(steam_language(frame) == "schinese", "Steam's own key, not Steamsteamglobal's");
    require(
        steam_language(
            R"("registry" { "hkcu" { "software" { "valve" { "steam" { "Language" "koreana" } } } } })") ==
            "koreana",
        "keys compared without case");
    require(
        steam_language(
            R"("Registry" { "HKCU" { "Software" { "Valve" { "Steam" { "x" "a\"b" "language" "tchinese" } } } } })") ==
            "tchinese",
        "escaped quotes in other values");
    require(steam_language(R"("Registry" { "HKCU" { "language" "german" } })").empty(),
            "elsewhere, ignored");
    require(steam_language("").empty() && steam_language("{{{ \"").empty() &&
                steam_language("}}}").empty(),
            "damaged text gives nothing");
}
struct Folder {
    fs::path path;
    Folder() {
        std::string pattern = (fs::temp_directory_path() / "fk-localization-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "temporary folder");
        path = pattern;
    }
    ~Folder() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    void write(const fs::path& name, const std::string& text) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream(path / name) << text;
    }
};
std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1));
}
void loading_tests() {
    Folder data, user;
    data.write("locales/xt.json", R"({
  "locale": "xt", "name": "Testish", "native_name": "Tëstish",
  "steam": ["testish"], "posix": ["xt"],
  "font_families": ["Test Sans", "sans-serif"],
  "plural": {
    "many": "v = 0 and i % 10 = 0 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 11..14",
    "one": "v = 0 and i % 10 = 1 and i % 100 != 11",
    "few": "v = 0 and i % 10 = 2..4 and i % 100 != 12..14"
  },
  "number": {"group": " ", "group_min": 5},
  "strings": {
    "a.plain": "Plain {name}",
    "a.count": {"by": "count", "=0": "no slov", "one": "{count} slovo", "few": "{count} slova",
                "many": "{count} slov", "other": "{count} slova"},
    "a.state": {"by": "state", "on": "{feature}: on", "off": "{feature}: off", "other": "{feature}: ?"},
    "a.nested": {"by": "state", "on": {"by": "count", "one": "one on", "other": "{count} on"}, "other": "off"},
    "a.brace": "{{literal}} {missing}"
  }
})");
    data.write("locales/xt-yy.json", R"j({"locale": "xt-YY", "name": "Testish (YY)", "fallback": "xt",
  "posix": ["xt_YY"], "strings": {"a.plain": "Variant {name}"}})j");
    data.write("locales/en.json", R"({"locale": "en", "strings": {
  "a.only-english": "English only", "a.hello": "Hello {name}"}})");
    data.write("locales/context.json", R"({"a.plain": {"where": "nowhere"}})");
    data.write("locales/bad.json", "{ not json");
    data.write("locales/bad-rule.json", R"({"locale": "xb", "plural": {"one": "i = "}})");
    data.write("locales/bad-branch.json",
               R"({"locale": "xc", "strings": {"a.b": {"by": "x", "one": "1"}}})");
    data.write("locales/bad-key.json", R"({"locale": "xd", "strings": {"Bad.Key": "x"}})");
    data.write("locales/bad-text.json", "{\"locale\": \"xe\", \"strings\": {\"a.b\": \"tab\\there\"}}");
    data.write("locales/bad-pseudo.json", R"({"locale": "qps"})");
    user.write("locales/xt.json", R"({"locale": "xt", "strings": {"a.state": "user override"}})");

    Localization l;
    l.load(data.path, user.path);
    require(l.errors().size() == 6, "six damaged files reported, context.json is no language: " +
                                        std::to_string(l.errors().size()));
    l.choose({"", "C"});
    require(l.tag() == "en" && l.text("a.only-english") == "English only", "C: English");
    require(l.text("a.missing") == "a.missing", "a key found nowhere shows as itself");
    require(l.fonts() == "Noto Sans,sans-serif" && l.number(1234) == "1,234" &&
                l.number(-1234567) == "-1,234,567" && l.number(999) == "999",
            "English fonts and numbers");

    l.choose({"testish", "xq_QQ.UTF-8"});
    require(l.tag() == "xt", "Steam's language first");
    require(l.text("a.state", {{"feature", std::string("F")}}) == "user override",
            "a user file replaces a key");
    require(l.text("a.plain", {{"name", std::string("N")}}) == "Plain N", "and keeps the others");
    const std::pair<long long, const char*> counts[]{
        {0, "no slov"},   {1, "1 slovo"},       {2, "2 slova"},        {5, "5 slov"},
        {21, "21 slovo"}, {1234, "1234 slova"}, {12345, "12 345 slov"}};
    for (const auto& [count, text] : counts) {
        require(l.text("a.count", {{"count", count}}) == text, std::string("count: ") + text);
    }
    require(l.text("a.nested", {{"state", std::string("on")}, {"count", 1LL}}) == "one on" &&
                l.text("a.nested", {{"state", std::string("on")}, {"count", 3LL}}) == "3 on" &&
                l.text("a.nested", {{"state", std::string("off")}, {"count", 3LL}}) == "off",
            "nested branches");
    require(l.text("a.nested") == "off", "a missing argument picks other");
    require(l.text("a.brace") == "{literal} {missing}", "literal braces; a missing placeholder stays");
    require(l.fonts() == "Test Sans,sans-serif", "the language's fonts");
    require(l.text("a.only-english") == "English only", "a key it lacks: English");

    l.choose({"", "xt_YY.UTF-8"});
    require(l.tag() == "xt-YY" && l.text("a.plain", {{"name", std::string("N")}}) == "Variant N",
            "the whole locale first");
    require(l.text("a.count", {{"count", 5LL}}) == "5 slov" && l.fonts() == "Test Sans,sans-serif",
            "then its fallback, with that file's rules and fonts");
    l.choose({"", "xt_ZZ"});
    require(l.tag() == "xt", "then the language alone");
    l.choose({"", "xq_QQ.UTF-8"});
    require(l.tag() == "en", "an unknown language: English");
    l.choose({"", "xt_YY"}, "xt");
    require(l.tag() == "xt", "an override wins");
    l.choose({"", "xt_YY"}, "nothing");
    require(l.tag() == "xt-YY", "an unknown override: the system's");

    l.choose({}, Localization::pseudo_tag);
    require(l.tag() == "qps", "the pseudo-language");
    const auto pseudo = l.text("a.only-english");
    require(pseudo.front() == '[' && pseudo.back() == ']' && pseudo.find("É") != std::string::npos &&
                characters(pseudo) * 10 >= characters("English only") * 14,
            "accented, in brackets, 40% longer: " + pseudo);
    require(l.text("a.hello", {{"name", std::string("Bob")}}).find("Bob") != std::string::npos,
            "placeholders kept");
    require(l.text("a.plain") == "a.plain", "only English keys");

    const auto list = l.locales();
    require(list.back().tag == "qps" && list.size() == 4 && list[0].name == "English" &&
                list[1].name == "Testish" && list[2].name == "Testish (YY)" &&
                list[1].native_name == "Tëstish",
            "languages by English name, the pseudo-language last");

    // Reload without the user's folder: the bundled and data files alone.
    l.load(data.path, {});
    l.choose({"testish", ""});
    require(l.text("a.state", {{"feature", std::string("F")}, {"state", std::string("off")}}) ==
                "F: off",
            "reloaded");
}
void bundled_tests() {
    Localization l;
    l.load({}, {});
    require(l.errors().empty(), "the bundled files load");
    l.choose({"", ""});
    require(l.tag() == "en" && l.fonts() == "Noto Sans,sans-serif", "English by default");
    Folder home;
    home.write(
        ".steam/registry.vdf",
        R"("Registry" { "HKCU" { "Software" { "Valve" { "Steam" { "language" "russian" } } } } })");
    setenv("LC_ALL", "", 1);
    unsetenv("LC_MESSAGES");
    setenv("LANG", "pl_PL.UTF-8", 1);
    const auto system = system_language(home.path);
    require(system.steam == "russian" && system.posix == "pl_PL.UTF-8", "Steam's language and LANG");
    require(system_language({}).steam.empty(), "no home, no Steam");
}
} // namespace

int main() {
    try {
        plural_tests();
        steam_tests();
        loading_tests();
        bundled_tests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Plural rules, Steam's language, loading, fallback, branches, numbers and the "
                 "pseudo-language passed.\n";
    return 0;
}
