#include "framekeyboard/app.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
const Key& key(const Layout& layout, const std::string& action) {
    for (const auto& key : layout.keys) {
        if (key.action == action) {
            return key;
        }
    }
    throw std::runtime_error("Missing test key");
}
void type(CjkComposer& composer, const std::string& text) {
    for (char ch : text) {
        composer.type(ch);
    }
}
struct Sink : KeySink {
    std::vector<std::string> events;
    bool accept{true};
    bool available{true};
    void send(int code, int value) override {
        events.push_back(std::to_string(code) + ":" + std::to_string(value));
    }
    bool text_available() override { return available; }
    bool commit_text(const std::string& value) override {
        if (!accept) {
            return false;
        }
        events.push_back(value);
        return true;
    }
};
void click(App& app, const std::string& action, bool expected = true) {
    auto view = app.view();
    for (int y = 96; y < panel_height; y += 4) {
        for (int x = 0; x < panel_width; x += 4) {
            const auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->action == action) {
                require(app.down(0, x, y, 1) == expected, "app key acceptance");
                app.up(0, x, y);
                return;
            }
        }
    }
    throw std::runtime_error("Could not find app key");
}
void unicode_input() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-unicode-test-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    Sink sink;
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.start_enabled = true;
    // Deliberately no target-language declaration: characters are resolved locally.
    App app(options, sink);
    app.apply({"en-us-full", "ru-ru", "graphite"});
    click(app, "KeyQ");
    require(sink.events == std::vector<std::string>{"й"}, "Russian text independent of target layout");
    app.apply({"international-full", "de-de", "graphite"});
    sink.events.clear();
    click(app, "KeyY");
    click(app, "BracketLeft");
    click(app, "ShiftLeft");
    click(app, "KeyY");
    click(app, "CapsLock");
    click(app, "KeyY");
    click(app, "ShiftRight");
    click(app, "KeyY");
    click(app, "CapsLock");
    click(app, "AltRight");
    click(app, "KeyQ");
    require(sink.events == std::vector<std::string>{"z", "ü", "Z", "Z", "z", "@"},
            "local Shift, Caps and AltGr produce Unicode only");
    sink.events.clear();
    click(app, "Equal");
    require(sink.events.empty(), "dead accent waits locally");
    click(app, "KeyE");
    require(sink.events == std::vector<std::string>{"é"}, "dead acute composition");
    click(app, "Equal");
    click(app, "Escape");
    click(app, "KeyE");
    require(sink.events == std::vector<std::string>{"é", "e"},
            "Escape cancels pending accent without native event");
    sink.events.clear();
    click(app, "Numpad7");
    click(app, "NumLock");
    click(app, "Numpad7");
    require(sink.events == std::vector<std::string>{"102:1", "102:0", "7"},
            "numpad navigation and digits ignore system Num Lock");
    sink.events.clear();
    click(app, "Enter");
    click(app, "ArrowLeft");
    require(sink.events == std::vector<std::string>{"28:1", "28:0", "105:1", "105:0"},
            "Enter and arrows stay native");
    app.apply({"international-full", "fr-fr", "graphite"});
    sink.events.clear();
    click(app, "ControlLeft");
    click(app, "KeyQ");
    require(sink.events == std::vector<std::string>{"29:1", "30:1", "30:0", "29:0"},
            "AZERTY Ctrl+A uses displayed A, not physical Q");
    sink.events.clear();
    click(app, "ControlLeft");
    click(app, "Semicolon");
    require(sink.events == std::vector<std::string>{"29:1", "50:1", "50:0", "29:0"},
            "AZERTY Ctrl+M works from punctuation position");
    sink.events.clear();
    sink.accept = false;
    click(app, "KeyQ");
    click(app, "Tab", false);
    require(sink.events.empty(), "failed character commit blocks focus-changing Tab");
    sink.accept = true;
    click(app, "Enter");
    require(sink.events == std::vector<std::string>{"a"},
            "Enter retries pending text without submitting a form");
    sink.events.clear();
    const auto view = app.view();
    bool held = false;
    for (int y = 96; y < panel_height && !held; y += 4) {
        for (int x = 0; x < panel_width && !held; x += 4) {
            const auto* k = app.renderer.hit_key(view, x, y);
            if (k && k->action == "KeyQ") {
                held = app.down(5, x, y, 10);
            }
        }
    }
    require(held, "capture repeat test key");
    app.tick(10.51);
    app.tick(20);
    require(sink.events == std::vector<std::string>{"a", "a", "a"},
            "Unicode repeat is bounded with no backlog");
    app.cancel_pointer(5);
    app.tick(21);
    require(sink.events.size() == 3, "pointer cancellation stops Unicode repeat");
    app.set_interaction_active(false);
    click(app, "KeyQ", false);
    require(sink.events.size() == 3, "hidden keyboard blocks Unicode");
}
// Each national keymap types its own letters through Unicode: plain keys,
// AltGr levels and dead keys composed locally.
void national_keymaps(const Profiles& profiles) {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-national-test-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    Sink sink;
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.start_enabled = true;
    App app(options, sink);
    struct Case {
        const char* language;
        const char* layout;
        // Keys in order; "Shift+" and "AltGr+" latch those modifiers first.
        std::vector<std::string> keys;
        std::vector<std::string> typed;
    };
    const std::vector<Case> cases{
        // Latching Shift keeps the pending accent for the capital.
        {"de-de", "international-full", {"Equal", "Shift+KeyE"}, {"É"}},
        {"tr-tr",
         "international-full",
         {"KeyI", "Shift+Quote", "BracketLeft", "Semicolon", "Comma"},
         {"ı", "İ", "ğ", "ş", "ö"}},
        {"pl-pl",
         "en-us-full",
         {"AltGr+KeyA", "AltGr+KeyZ", "AltGr+KeyX", "AltGr+KeyL", "Shift+AltGr+KeyS"},
         {"ą", "ż", "ź", "ł", "Ś"}},
        {"cs-cz",
         "international-full",
         {"Digit2", "Shift+Equal", "KeyD", "Equal", "KeyO", "Shift+Backquote", "KeyU", "Shift+Equal",
          "Shift+KeyD"},
         {"ě", "ď", "ó", "ů", "Ď"}},
        {"sk-sk",
         "international-full",
         {"Digit2", "Digit3", "BracketRight", "Shift+Equal", "KeyD"},
         {"ľ", "š", "ä", "ď"}},
        {"hu-hu",
         "international-full",
         {"Backquote", "Digit0", "BracketLeft", "Backslash", "IntlBackslash", "AltGr+Digit0", "KeyO"},
         {"0", "ö", "ő", "ű", "í", "ő"}},
        {"ro-ro",
         "international-full",
         {"BracketLeft", "Semicolon", "Quote", "Backquote"},
         {"ă", "ș", "ț", "„"}},
        {"hr-hr",
         "international-full",
         {"BracketLeft", "Semicolon", "Quote", "BracketRight", "Backslash"},
         {"š", "č", "ć", "đ", "ž"}},
        {"sl-si", "international-full", {"BracketLeft", "Semicolon", "Backslash"}, {"š", "č", "ž"}},
        {"et-ee", "international-full", {"BracketRight", "Backquote", "KeyS"}, {"õ", "š"}},
        {"lv-lv",
         "en-us-full",
         {"AltGr+KeyA", "AltGr+KeyG", "AltGr+KeyC", "Shift+AltGr+KeyN"},
         {"ā", "ģ", "č", "Ņ"}},
        {"lt-lt", "en-us-full", {"Digit1", "Digit2", "Equal"}, {"ą", "č", "ž"}},
        {"sv-se",
         "international-full",
         {"BracketLeft", "Semicolon", "Quote", "Equal", "KeyE"},
         {"å", "ö", "ä", "é"}},
        {"fi-fi",
         "international-full",
         {"BracketLeft", "Semicolon", "AltGr+Backslash", "KeyS"},
         {"å", "ö", "š"}},
        {"da-dk", "international-full", {"Semicolon", "Quote", "BracketLeft"}, {"æ", "ø", "å"}},
        {"nb-no",
         "international-full",
         {"Semicolon", "Quote", "BracketLeft", "Shift+Equal", "KeyE"},
         {"ø", "æ", "å", "è"}},
        {"fr-ca",
         "international-full",
         {"BracketLeft", "KeyE", "Quote", "KeyA", "BracketRight", "KeyC", "Slash", "IntlBackslash"},
         {"ê", "à", "ç", "é", "«"}},
        // The apostrophe stays plain, so "zo'n" is not composed into ń.
        {"nl-nl",
         "en-us-full",
         {"AltGr+KeyE", "Shift+AltGr+Quote", "KeyE", "Quote", "KeyN"},
         {"é", "ë", "'", "n"}},
        {"el-gr",
         "en-us-full",
         {"KeyA", "Semicolon", "KeyA", "KeyW", "Shift+Semicolon", "KeyI", "Semicolon", "Shift+Semicolon",
          "KeyI"},
         {"α", "ά", "ς", "ϊ", "ΐ"}},
        // A dead tone before the letter composes; the plain tone key after it combines.
        {"vi-vn",
         "en-us-full",
         {"Digit1", "AltGr+Digit8", "Digit2", "KeyA", "Digit8", "BracketLeft", "Digit0"},
         {"ă", "ấ", "a", "́", "ư", "đ"}},
        {"he-il", "en-us-full", {"KeyA", "KeyT", "Shift+Digit9"}, {"ש", "א", ")"}},
        {"ar", "international-full", {"KeyA", "KeyH", "KeyB", "Shift+Digit9"}, {"ش", "ا", "لا", ")"}},
        {"hi-in",
         "international-full",
         {"KeyK", "KeyD", "KeyE", "KeyJ", "Digit1"},
         {"क", "्", "ा", "र", "१"}},
        {"th-th", "international-full", {"KeyA", "KeyD", "KeyY"}, {"ฟ", "ก", "ั"}},
        {"id-id", "en-us-full", {"KeyA", "Quote"}, {"a", "'"}},
        {"ms-my", "en-us-full", {"KeyA", "Quote"}, {"a", "'"}},
    };
    for (const auto& test : cases) {
        validate_selection(profiles, {test.layout, test.language, "graphite"});
        app.apply({test.layout, test.language, "graphite"});
        sink.events.clear();
        for (const auto& step : test.keys) {
            std::string rest = step;
            for (const auto& [prefix, modifier] :
                 {std::pair<std::string, std::string>{"Shift+", "ShiftLeft"}, {"AltGr+", "AltRight"}}) {
                if (rest.starts_with(prefix)) {
                    click(app, modifier);
                    rest.erase(0, prefix.size());
                }
            }
            click(app, rest);
        }
        if (sink.events != test.typed) {
            std::string got;
            for (const auto& event : sink.events) {
                got += " " + event;
            }
            throw std::runtime_error(std::string(test.language) + " typed" + got);
        }
    }
    for (const auto& [id, language] : profiles.languages) {
        // ABNT2 and JIS languages need keys of their own layouts.
        if (language.input_method != "xkb" ||
            std::any_of(language.required_keys.begin(), language.required_keys.end(),
                        [](const std::string& name) { return name != "IntlBackslash"; })) {
            continue;
        }
        // Every other keymap language fits the international layout, and the
        // US layout exactly when it needs no extra key.
        validate_selection(profiles, {"international-full", id, "graphite"});
        bool us = true;
        try {
            validate_selection(profiles, {"en-us-full", id, "graphite"});
        } catch (const std::exception&) {
            us = false;
        }
        require(us == language.required_keys.empty(), "only languages needing a key are dimmed on US");
        // No letter or digit key is left without a legend of its own.
        LanguageMap map(language);
        for (const auto& k : profiles.layouts.at("international-full").keys) {
            if (k.action.starts_with("Key") || k.action.starts_with("Digit")) {
                require(LanguageMap::printable(map.symbol(k, {}, false, false)),
                        "every letter and digit key types something");
            }
        }
        require(map.right_to_left() == (id == "ar" || id == "he-il"),
                "Arabic and Hebrew legends are laid out right to left");
    }
}
void input_gate_recovery() {
    Sink sink;
    OutputController gate(sink);
    gate.set_enabled(true);
    gate.send(KEY_BACKSPACE, 1);
    gate.set_enabled(false);
    gate.send(KEY_BACKSPACE, 2);
    gate.send(KEY_A, 1);
    gate.send(KEY_A, 0);
    gate.send(KEY_BACKSPACE, 0);
    gate.send(KEY_BACKSPACE, 0);
    require(sink.events == std::vector<std::string>{"14:1", "14:0"},
            "disabled gate releases only owned keys and drops downs and repeats");

    const auto path =
        fs::temp_directory_path() / ("framekeyboard-gate-test-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.start_enabled = true;
    App app(options, sink);
    sink.events.clear();
    bool pressed = false;
    for (int y = 96; y < panel_height && !pressed; y += 4) {
        for (int x = 0; x < panel_width && !pressed; x += 4) {
            const auto* hit = app.renderer.hit_key(app.view(), x, y);
            if (hit && hit->action == "Backspace") {
                require(app.down(0, x, y, 1), "hold Backspace before text loss");
                sink.available = false;
                app.tick(2);
                app.up(0, x, y);
                app.cancel();
                pressed = true;
            }
        }
    }
    require(pressed && sink.events == std::vector<std::string>{"14:1", "14:0"},
            "text loss releases Backspace before disabling input and stops repeats");
    click(app, "ArrowLeft");
    require(sink.events.size() == 2, "disabled key press and release send nothing");

    sink.available = true;
    app.tick(3);
    app.apply({"ja-jis-full", "ja-romaji", "graphite"});
    click(app, "KeyK");
    const auto preedit = app.view().preedit;
    require(!preedit.empty(), "composition exists before text loss");
    sink.available = false;
    app.tick(4);
    require(app.view().preedit == preedit, "text loss retains composition for retry");
}
void engines(const std::string& group = "") {
    if (group != "--chinese") {
        if (CjkComposer::supported("korean-2set")) {
            CjkComposer korean("korean-2set");
            type(korean, "gksrmf");
            require(korean.text() == "한글", "two-set Korean syllables");
            korean.backspace();
            require(korean.text() == "한그", "Korean final-consonant backspace");
            korean.cancel();
            type(korean, "rkrk");
            require(korean.text() == "가가", "Korean trailing consonant moves to next syllable");
            korean.backspace();
            require(korean.text() == "각", "Korean undo preserves resyllabification");
            korean.cancel();
            type(korean, "Rk");
            require(korean.text() == "까", "Korean Shift doubles consonant");
        } else {
            require(!std::getenv("FRAMEKEYBOARD_TEST_CJK_REQUIRED"), "Korean engine required");
            std::cout << "Korean integration skipped: build lacks libhangul\n";
        }
    }
    if (group != "--korean") {
        if (CjkComposer::supported("chinese-pinyin-simplified")) {
            for (const auto& method : {"chinese-pinyin-simplified", "chinese-pinyin-traditional"}) {
                CjkComposer chinese(method);
                type(chinese, "nihao");
                require(chinese.text() == "你好", "Pinyin phrase conversion");
                require(!chinese.candidates().empty(), "Chinese candidates");
                const auto candidates = chinese.candidates();
                const auto partial = std::find(candidates.begin(), candidates.end(), "你");
                require(partial != candidates.end(), "partial Pinyin candidate available");
                chinese.choose(static_cast<int>(partial - candidates.begin()));
                require(chinese.has_reading() && chinese.text() == "你好",
                        "partial selection keeps remaining syllables");
                chinese.cancel();
                type(chinese, "nihao");
                chinese.choose(0);
                require(chinese.text() == "你好", "selected phrase retained without engine learning");
                chinese.backspace();
                require(chinese.preedit() == "nihao", "undo chosen Pinyin segment");
                chinese.cancel();
                type(chinese, "zhongguo");
                const std::string expected =
                    std::string(method).ends_with("simplified") ? "中国" : "中國";
                require(chinese.text() == expected, "Simplified/Traditional conversion");
                chinese.cycle(1);
                require(chinese.selected() == 1, "candidate navigation");
                chinese.cycle(-1);
                require(chinese.text() == expected, "candidate restoration");
            }
        } else {
            require(!std::getenv("FRAMEKEYBOARD_TEST_CJK_REQUIRED"), "Chinese engine required");
            std::cout << "Chinese integration skipped: build lacks PyZy\n";
        }
    }
}
void app_composition(const Profiles& profiles, const std::string& group = "") {
    auto path = fs::temp_directory_path() / ("framekeyboard-language-test-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.target_language = "en-us";
    options.start_enabled = true;
    for (const auto* id : {"zh-cn-pinyin", "zh-tw-pinyin", "ko-kr"}) {
        if ((group == "--korean" && std::string(id) != "ko-kr") ||
            (group == "--chinese" && std::string(id) == "ko-kr")) {
            continue;
        }
        if (!CjkComposer::supported(profiles.languages.at(id).input_method)) {
            Sink sink;
            App app(options, sink);
            bool rejected = false;
            try {
                app.apply({"en-us-full", id, "graphite"});
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected && app.selection().language == "en-us",
                    "unavailable engine preserves working selection");
            save_selection(path, {"en-us-full", id, "graphite"});
            App restored(options, sink);
            require(restored.selection().language == "en-us",
                    "missing saved engine falls back to English at startup");
            std::vector<std::string> errors;
            require(load_settings(path, errors).active.language == id,
                    "startup fallback preserves the user's saved language");
            for (const auto& control : restored.view().controls) {
                if (control.id == "pin") {
                    const auto x = control.bounds.x + control.bounds.width / 2;
                    const auto y = control.bounds.y + control.bounds.height / 2;
                    restored.down(0, x, y, 1);
                    restored.up(0, x, y);
                    break;
                }
            }
            restored.flush_settings();
            require(load_settings(path, errors).active.language == id,
                    "pin save preserves the unavailable saved language");
            sink.events.clear();
            click(restored, "KeyA");
            require(sink.events == std::vector<std::string>{"a"},
                    "startup fallback can still type English");
            fs::remove(path / "config.json");
            continue;
        }
        Sink sink;
        App app(options, sink);
        app.apply({"en-us-full", id, "graphite"});
        click(app, "KeyR");
        require(sink.events.empty(), "composition does not emit raw letters");
        require(!app.view().preedit.empty(), "visible local preedit");
        sink.accept = false;
        click(app, "Tab", false);
        require(!app.view().preedit.empty() && sink.events.empty(),
                "failed commit retains text and blocks Tab");
        sink.accept = true;
        click(app, "Tab");
        require(app.view().preedit.empty(), "Tab commits preedit");
        require(sink.events.size() == 3 && sink.events[1] == "15:1", "commit precedes physical Tab");
        if (std::string(id).starts_with("zh-")) {
            sink.events.clear();
            for (const auto* action : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}) {
                click(app, action);
            }
            click(app, "Digit1");
            require(sink.events == std::vector<std::string>{"你好"},
                    "candidate number commits text without a native digit");
        }
        // Ordinary Unicode characters must not overtake an unfinished IME word.
        sink.events.clear();
        if (std::string(id) == "ko-kr") {
            for (const auto* action :
                 {"KeyG", "KeyK", "KeyS", "Space", "KeyR", "Digit1", "KeyM", "Comma", "Enter"}) {
                click(app, action);
            }
            require(sink.events ==
                        std::vector<std::string>{"한", " ", "ㄱ", "1", "ㅡ", ",", "28:1", "28:0"},
                    "Korean composition precedes space, digit, punctuation and Enter");
        } else {
            for (const auto* action :
                 {"Comma", "Period", "Digit0", "Digit6", "Digit7", "Digit8", "Digit9"}) {
                sink.events.clear();
                for (const auto* letter : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}) {
                    click(app, letter);
                }
                click(app, action);
                require(sink.events.size() == 2 && sink.events.front() == "你好" &&
                            app.view().preedit.empty(),
                        "Pinyin composition precedes ordinary punctuation and digits");
            }
        }
        sink.events.clear();
        click(app, "KeyR");
        sink.accept = false;
        click(app, "Comma", false);
        require(sink.events.empty() && !app.view().preedit.empty(),
                "failed composition commit blocks following Unicode character");
        sink.accept = true;
        click(app, "Comma");
        require(sink.events.size() == 2 && sink.events.back() == "," && app.view().preedit.empty(),
                "retry commits composition before punctuation exactly once");
        click(app, "KeyR");
        app.set_interaction_active(false);
        require(app.view().preedit.empty(), "hiding clears composition");
        const auto count = sink.events.size();
        click(app, "KeyR", false);
        require(sink.events.size() == count, "hidden keyboard sends nothing");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto profiles = load_profiles({}, {});
        const std::string group = argc > 1 ? argv[1] : "";
        if (group == "--korean" || group == "--chinese") {
            const auto method = group == "--korean" ? "korean-2set" : "chinese-pinyin-simplified";
            if (!CjkComposer::supported(method)) {
                std::cout << method << ": engine unavailable; integration not run\n";
                return std::getenv("FRAMEKEYBOARD_TEST_CJK_REQUIRED") ? 1 : 77;
            }
            engines(group);
            app_composition(profiles, group);
            std::cout << method << ": engine and delivery ordering passed\n";
            return 0;
        }
        if (group == "--without-engines") {
            require(!CjkComposer::supported("korean-2set") &&
                        !CjkComposer::supported("chinese-pinyin-simplified"),
                    "isolated test must have no usable optional engines");
            unicode_input();
            app_composition(profiles);
            std::cout
                << "English/Unicode works without optional engines; failed Apply preserves selection\n";
            return 0;
        }
        require(group.empty() || group == "--core", "unknown test group");
        input_gate_recovery();
        struct Case {
            const char* id;
            const char* layout;
            const char* key;
            const char* normal;
            const char* shifted;
        };
        for (const auto& test : std::vector<Case>{{"fr-fr", "international-full", "KeyQ", "a", "A"},
                                                  {"es-es", "international-full", "Semicolon", "ñ", "Ñ"},
                                                  {"it-it", "international-full", "Semicolon", "ò", "ç"},
                                                  {"pt-br", "br-abnt2-full", "Semicolon", "ç", "Ç"},
                                                  {"ru-ru", "en-us-full", "KeyQ", "й", "Й"},
                                                  {"uk-ua", "en-us-full", "KeyS", "і", "І"}}) {
            validate_selection(profiles, {test.layout, test.id, "graphite"});
            LanguageMap map(profiles.languages.at(test.id));
            const auto k = key(profiles.layouts.at(test.layout), test.key);
            require(map.legend(k, {}, false, false) == test.normal, "native language legend");
            require(map.legend(k, {KEY_LEFTSHIFT}, false, false) == test.shifted,
                    "native shifted legend");
        }
        LanguageMap brazil(profiles.languages.at("pt-br"));
        require(brazil.legend(key(profiles.layouts.at("br-abnt2-full"), "IntlRo"), {}, false, false) ==
                    "/",
                "ABNT2 extra slash");
        require(key_code("NumpadComma") == KEY_KPCOMMA, "ABNT2 keypad separator evdev code");
        unicode_input();
        national_keymaps(profiles);
        if (group.empty()) {
            engines();
            app_composition(profiles);
        }
        std::cout << "Language profiles, CJK composition and input ordering passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
