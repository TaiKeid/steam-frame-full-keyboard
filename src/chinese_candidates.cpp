#include "framekeyboard/chinese_candidates.hpp"
#include "framekeyboard/cjk.hpp"
#include <cstdlib>
#include <glib.h>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>

namespace framekeyboard {
namespace {
// The standard Zhuyin keyboard, which every engine here reads: the key for
// each symbol.
const std::map<std::string, char> zhuyin_keys{
    {"ㄅ", '1'}, {"ㄆ", 'q'}, {"ㄇ", 'a'}, {"ㄈ", 'z'}, {"ㄉ", '2'}, {"ㄊ", 'w'}, {"ㄋ", 's'},
    {"ㄌ", 'x'}, {"ㄍ", 'e'}, {"ㄎ", 'd'}, {"ㄏ", 'c'}, {"ㄐ", 'r'}, {"ㄑ", 'f'}, {"ㄒ", 'v'},
    {"ㄓ", '5'}, {"ㄔ", 't'}, {"ㄕ", 'g'}, {"ㄖ", 'b'}, {"ㄗ", 'y'}, {"ㄘ", 'h'}, {"ㄙ", 'n'},
    {"ㄧ", 'u'}, {"ㄨ", 'j'}, {"ㄩ", 'm'}, {"ㄚ", '8'}, {"ㄛ", 'i'}, {"ㄜ", 'k'}, {"ㄝ", ','},
    {"ㄞ", '9'}, {"ㄟ", 'o'}, {"ㄠ", 'l'}, {"ㄡ", '.'}, {"ㄢ", '0'}, {"ㄣ", 'p'}, {"ㄤ", ';'},
    {"ㄥ", '/'}, {"ㄦ", '-'}, {"ˊ", '6'},  {"ˇ", '3'},  {"ˋ", '4'},  {"˙", '7'}};
// The line's characters, one string each.
std::vector<std::string> characters(const std::string& line) {
    std::vector<std::string> result;
    for (const char* c = line.c_str(); *c; c = g_utf8_next_char(c)) {
        result.emplace_back(c, static_cast<std::size_t>(g_utf8_next_char(c) - c));
    }
    return result;
}
bool zhuyin_line(const std::string& line) {
    for (const auto& c : characters(line)) {
        if (zhuyin_keys.contains(c)) {
            return true;
        }
    }
    return false;
}
// The keys a method takes for the line.
std::string keys_for(const std::string& method, const std::string& line) {
    std::string keys;
    if (method == "chinese-zhuyin") {
        for (const auto& c : characters(line)) {
            if (const auto key = zhuyin_keys.find(c); key != zhuyin_keys.end()) {
                keys += key->second;
            } else if (c == " ") {
                keys += ' '; // The first tone of the syllable before it.
            } else if (c.size() == 1 && g_ascii_isgraph(c[0])) {
                keys += static_cast<char>(g_ascii_tolower(c[0]));
            }
        }
    } else if (method == "chinese-cangjie" || method == "chinese-quick") {
        for (const char c : line) {
            if (g_ascii_isalpha(c)) {
                keys += static_cast<char>(g_ascii_tolower(c));
            }
        }
    } else {
        for (const char c : line) {
            if (g_ascii_isalpha(c) || c == '\'') {
                keys += static_cast<char>(g_ascii_tolower(c));
            }
        }
    }
    return keys;
}
// The first `count` texts, separated so that a candidate with spaces reads whole.
std::string listed(const std::vector<std::string>& texts, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < texts.size() && i < count; ++i) {
        result += (i ? " | " : "") + texts[i];
    }
    if (texts.size() > count) {
        result += " | … " + std::to_string(texts.size() - count) + " more";
    }
    return result;
}
// Each composer loads its engine by FRAMEKEYBOARD_CHINESE_ENGINE (CjkComposer);
// the report sets it for one engine at a time, then puts it back.
class EngineChoice {
  public:
    EngineChoice() {
        if (const char* value = std::getenv(variable)) {
            saved_ = value;
        }
    }
    ~EngineChoice() { restore(); }
    // Empty is the keyboard's own choice.
    void use(const std::string& engine) {
        if (engine.empty()) {
            restore();
        } else {
            setenv(variable, engine.c_str(), 1);
        }
    }

  private:
    static constexpr const char* variable = "FRAMEKEYBOARD_CHINESE_ENGINE";
    std::optional<std::string> saved_;
    void restore() {
        if (saved_) {
            setenv(variable, saved_->c_str(), 1);
        } else {
            unsetenv(variable);
        }
    }
};
// The methods each engine has (docs/languages.md, Chinese engines).
bool has_method(const std::string& engine, const std::string& method) {
    const bool zhuyin = method == "chinese-zhuyin";
    return (engine == "libime" && !zhuyin) || (engine == "mcbopomofo" && zhuyin);
}
std::string report_line(const std::string& method, const std::string& keys, std::size_t count) {
    CjkComposer composer(method);
    for (const char key : keys) {
        // As on the keyboard, Space reaches the engine only as an open
        // syllable's first tone; between finished syllables it separates.
        if (key != ' ' || composer.takes_space()) {
            composer.type(key);
        }
    }
    if (composer.takes_space()) {
        composer.type(' '); // A syllable still open takes the first tone.
    }
    return "(" + composer.engine() + ")  " + composer.text() + "  [" + listed(composer.candidates(), count) +
           "]";
}
} // namespace
void report_chinese_candidates(std::istream& readings, std::ostream& report,
                               const ChineseCandidatesOptions& options) {
    EngineChoice choice;
    // An engine's dictionaries stay loaded only while one of its composers
    // lives. As the keyboard keeps the added languages' engines
    // (App::sync_chinese()), one composer for each engine and method stays for
    // the whole report; each line still types into a fresh one.
    std::map<std::pair<std::string, std::string>, std::unique_ptr<CjkComposer>> loaded;
    const std::vector<std::string> engines = options.engines.empty() ? std::vector<std::string>{""}
                                                                    : options.engines;
    std::string line;
    while (std::getline(readings, line)) {
        if (line.empty() || line.front() == '#') {
            report << line << '\n';
            continue;
        }
        report << line << '\n';
        const auto methods = !options.methods.empty() ? options.methods
                             : zhuyin_line(line)
                                 ? std::vector<std::string>{"chinese-zhuyin"}
                                 : std::vector<std::string>{"chinese-pinyin-simplified",
                                                            "chinese-pinyin-traditional"};
        for (const auto& method : methods) {
            // Cangjie and Quick have one engine, the table, whatever is compared.
            const bool table = method == "chinese-cangjie" || method == "chinese-quick";
            for (const auto& engine : table ? std::vector<std::string>{""} : engines) {
                choice.use(engine);
                // Every engine compared, where it has the method and was built.
                if (!engine.empty() && (!has_method(engine, method) || !CjkComposer::supported(method))) {
                    continue;
                }
                if (!loaded.contains({engine, method})) {
                    try {
                        loaded[{engine, method}] = std::make_unique<CjkComposer>(method);
                    } catch (const std::exception&) {
                        // The line below reports why.
                    }
                }
                report << "  " << method << ' ';
                try {
                    report << report_line(method, keys_for(method, line), options.count) << '\n';
                } catch (const std::exception& error) {
                    report << "(" << (engine.empty() ? "default" : engine) << ")  " << error.what() << '\n';
                }
            }
        }
    }
}
} // namespace framekeyboard
