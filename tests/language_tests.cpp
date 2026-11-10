#include "../src/cjk_module.hpp"
#include "../src/pinyin_reranker.hpp"
#include "../src/table_engine.hpp"
#include "framekeyboard/app.hpp"
#include "framekeyboard/chinese_candidates.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
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
// Where the keys start on the panel.
double keys_top(const PanelView& view) {
    double top = panel_height;
    for (const auto& key : view.layout->keys) {
        top = std::min(top, panel_rect(view, key.bounds).y);
    }
    return top;
}
// Pull for Shift: character keys type on release, and pulled down their Shift
// character, never repeating; Space and Backspace still act on press.
void key_flicks() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-key-flicks-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    std::ofstream(path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},)"
        << R"("key_flicks":true})";
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.target_language = "en-us";
    options.start_enabled = true;
    Sink sink;
    App app(options, sink);
    const auto centre = [&](const std::string& action) {
        const auto view = app.view();
        for (const auto& key : view.layout->keys) {
            if (key.action == action) {
                const auto r = panel_rect(view, key.bounds);
                return std::pair{r.x + r.width / 2, r.y + r.height / 2};
            }
        }
        throw std::runtime_error("no key " + action);
    };
    double now = 1;
    // Presses, pulls down by `dy` panel pixels, holds for `hold` seconds and lets go.
    const auto flick = [&](const std::string& action, double dy, double hold = .05) {
        const auto [x, y] = centre(action);
        require(app.down(0, x, y, now), "a key flick starts with a press");
        app.move(0, x, y + dy / 2);
        app.move(0, x, y + dy);
        for (double t = 0; t < hold; t += .05) {
            app.tick(now += .05);
        }
        app.up(0, x, y + dy);
        app.tick(now += .05);
    };
    const auto typed = [&] {
        std::string text;
        for (const auto& event : sink.events) {
            if (event.find(':') == std::string::npos) {
                text += event;
            }
        }
        return text;
    };
    {
        const auto [x, y] = centre("Digit6");
        app.down(0, x, y, now);
        require(typed().empty(), "a character key waits for release");
        app.up(0, x, y);
    }
    require(typed() == "6", "and types its character then");
    flick("Digit6", 60);
    require(typed() == "6^", "pulled down, the Shift character: 6 then ^, as on an iPad");
    flick("KeyK", 0);
    flick("KeyK", 60);
    require(typed() == "6^kK", "a letter flicks to its capital while swipe isn't on");
    flick("Digit6", 60, 1.5);
    require(typed() == "6^kK^", "held, it types once: character keys don't repeat");
    sink.events.clear();
    {
        // Backspace keeps acting on press, held down for the target to repeat.
        const auto [x, y] = centre("Backspace");
        app.down(0, x, y, now);
        require(sink.events == std::vector<std::string>{"14:1"}, "Backspace acts on press");
        app.up(0, x, y);
    }
    sink.events.clear();
    click(app, "CapsLock");
    flick("KeyK", 0);
    flick("KeyK", 60);
    flick("Digit6", 60);
    require(typed() == "KK^", "with Caps Lock a flick still types what the key shows");
    click(app, "CapsLock");
    sink.events.clear();
    {
        // A flick past the threshold that comes back up types the key itself.
        const auto [x, y] = centre("Digit6");
        app.down(0, x, y, now);
        app.move(0, x, y + 60);
        app.move(0, x, y + 2);
        app.up(0, x, y + 2);
    }
    require(typed() == "6", "pulled back up, the flick is undone");
    // German: ´ flicks to its Shift character `, a dead grave that composes.
    app.apply({"international-full", "de-de", "graphite"});
    sink.events.clear();
    flick("Equal", 60);
    flick("KeyE", 0);
    require(typed() == "è", "a dead Shift character composes with the next key");
    if (CjkComposer::supported("korean-2set")) {
        // Korean: ㅂ flicks to ㅃ, the jamo its Shift gives.
        app.apply({"en-us-full", "ko-kr", "graphite"});
        flick("KeyQ", 60);
        require(app.view().preedit == "ㅃ", "ㅂ flicks to ㅃ");
        click(app, "Escape");
        sink.events.clear();
        flick("Digit6", 60);
        flick("Slash", 60);
        require(typed() == "^?", "and its digits and punctuation flick as on US QWERTY");
    }
    if (CjkComposer::supported("chinese-zhuyin")) {
        // Zhuyin's ㄅ composes and its pull types ！: without a target only
        // the pull is refused (R143).
        app.apply({"en-us-full", "zh-tw-zhuyin", "graphite"});
        app.set_dashboard_focus(true);
        sink.events.clear();
        const auto shaken = app.view().focus_shake_since;
        flick("Digit1", 0);
        require(app.view().preedit == "ㄅ" && app.view().focus_shake_since == shaken,
                "a tap that only composes needs no target");
        click(app, "Escape");
        flick("Digit1", 60);
        require(typed().empty() && app.view().focus_shake_since != shaken,
                "a pull that types waits for a target");
        app.set_dashboard_focus(false);
        flick("Digit1", 60);
        require(typed() == "！", "with one, the pull types its punctuation");
    }
    // Text the field refuses at release waits for Enter's retry, as a press's does.
    app.apply({"en-us-full", "en-us", "graphite"});
    sink.events.clear();
    sink.accept = false;
    flick("Digit6", 60);
    sink.accept = true;
    require(typed().empty(), "a refused flick types nothing yet");
    click(app, "Enter");
    app.tick(now += .1);
    require(typed() == "^", "and Enter types it");
    // Kana ろ and ー have no symbol in the keymap below, and still wait for release.
    app.apply({"ja-jis-full", "ja-kana", "graphite"});
    {
        const auto [x, y] = centre("IntlRo");
        app.down(0, x, y, now);
        require(app.view().preedit.empty(), "ろ waits for release");
        app.up(0, x, y);
        require(app.view().preedit == "ろ", "and types then");
        click(app, "Escape");
    }
    {
        // A kana key pulled shows its small kana; in Latin mode the keys are
        // letters, each flicking to its capital, whatever their kana do.
        const auto pulled = [&](const std::string& action) {
            const auto [x, y] = centre(action);
            app.down(0, x, y, now);
            app.move(0, x, y + 30);
            const auto shown = app.view().key_flick;
            app.cancel_pointer(0);
            return shown ? shown->legend : std::string{};
        };
        require(pulled("KeyE") == "ぃ" && pulled("KeyK").empty(), "kana keys flick to their small kana");
        click(app, "KanaMode");
        require(pulled("KeyE") == "E" && pulled("KeyK") == "K",
                "in Latin mode, letters to their capitals");
        sink.events.clear();
        flick("KeyK", 60);
        flick("KeyE", 60);
        require(typed() == "KE", "and type them");
        click(app, "KanaMode");
    }
    {
        // The toolbar's switch pauses flicks: keys type on press again.
        app.apply({"en-us-full", "en-us", "graphite"});
        const auto toggle = [&] {
            for (const auto& control : app.view().controls) {
                if (control.id == "key-flicks-toggle") {
                    const double x = control.bounds.x + control.bounds.width / 2,
                                 y = control.bounds.y + control.bounds.height / 2;
                    app.down(2, x, y, now);
                    app.up(2, x, y);
                    return;
                }
            }
            throw std::runtime_error("no key flick switch");
        };
        require(app.view().key_flick_hints, "with flicks on, keys show their flick hints");
        toggle();
        require(!app.view().key_flick_hints, "paused, the hints go, as ㅃ over ㅂ would");
        sink.events.clear();
        const auto [x, y] = centre("Digit6");
        app.down(0, x, y, now);
        require(typed() == "6", "paused, a key types on press");
        app.move(0, x, y + 60);
        app.up(0, x, y + 60);
        require(typed() == "6", "and pulling it down types nothing more");
        toggle();
        sink.events.clear();
        flick("Digit6", 60);
        require(typed() == "^", "switched back on, it flicks again");
    }
    // Romaji composes lowercase: its letters don't flick, its digits do.
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    flick("KeyK", 60);
    flick("KeyA", 0);
    require(app.view().preedit == "か", "a romaji letter types its romaji, flicked or not");
    click(app, "Escape");
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
// Cangjie and Quick logic against a small table laid out as the system's are.
void table_engine() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-table-test-" + std::to_string(getpid()) + ".db");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove(path); }
    } cleanup{path};
    const auto script = "import sqlite3,sys\n"
                        "c=sqlite3.connect(sys.argv[1])\n"
                        "c.execute('create table phrases(id integer primary key,tabkeys text,"
                        "phrase text,freq integer,user_freq integer)')\n"
                        "c.executemany('insert into phrases(tabkeys,phrase,freq,user_freq) "
                        "values(?,?,?,0)',[('a','日',10),('a','曰',5),('ab','明',90),('b','月',1),"
                        "('e','𠮷',1),('f','明日',1)]"
                        "+[('c',str(i),1) for i in range(60)]+[('c'+chr(97+i%26)+chr(97+i//26),"
                        "'x'+str(i),1) for i in range(120)]+[('d'+chr(97+i%26)+chr(97+i//26),"
                        "'y'+str(i),1) for i in range(120)])\n"
                        "c.commit()\n";
    const auto file = path.string() + ".py";
    std::ofstream(file) << script;
    const auto command = "python3 -I " + file + " " + path.string();
    const int result = std::system(command.c_str());
    fs::remove(file);
    if (result != 0 || !TableEngine::available(path)) {
        std::cout << "Table engine test skipped: no SQLite.\n";
        return;
    }
    TableEngine table(TableEngine::load(path), 2);
    table.type('a');
    require(table.preedit() == "日", "keys show as radicals");
    require(table.candidates() == std::vector<std::string>({"日", "曰", "明"}),
            "exact codes lead, then longer ones by frequency");
    table.cycle(1);
    require(table.selected() == 1 && table.text() == "曰", "the selection moves");
    table.cycle(-2);
    require(table.selected() == 2, "and wraps");
    table.type('z');
    require(table.candidates().empty() && table.text().empty() && table.has_reading(),
            "a reading that matches nothing has no text");
    table.type('b');
    require(table.preedit() == "日重", "the table's key limit holds");
    table.backspace();
    table.type('b');
    require(table.text() == "明" && table.preedit() == "日月", "a corrected reading finds it");
    table.choose(0);
    require(!table.has_reading() && !table.empty() && table.text() == "明", "choosing ends the reading");
    table.type('a');
    require(table.preedit() == "明日" && table.text() == "明日",
            "a choice not yet committed stays ahead of the next reading");
    table.type('z');
    require(table.text().empty() && table.preedit() == "明日重", "and waits while nothing matches");
    table.backspace();
    table.backspace();
    require(table.text() == "明" && !table.has_reading(), "Backspace returns to it");
    table.cancel();
    require(table.empty() && table.text().empty(), "cancel clears it");
    table.type('1');
    require(table.empty(), "only letters take part");
    table.type('c');
    require(table.candidates().size() == 60 && table.candidates().back() == "59",
            "a long list of exact matches is typed in full");
    table.cancel();
    table.type('d');
    require(table.candidates().size() == 50, "a prefix alone stops at enough to page through");
    table.cancel();
    for (const char ch : std::string("abe")) {
        table.type(ch);
        table.choose(0);
    }
    require(table.text() == "日月𠮷", "retained choices include four-byte Unicode characters");
    table.backspace();
    require(table.text() == "日月", "Backspace removes one complete retained code point");
    table.backspace();
    require(table.text() == "日", "earlier retained choices survive another Backspace");
    table.backspace();
    require(table.empty(), "Backspace can remove the final retained choice");

    constexpr std::size_t retained_limit = 1024;
    for (std::size_t i = 0; i < retained_limit - 1; ++i) {
        table.type('a');
        table.choose(0);
    }
    table.type('f');
    const auto before = table.preedit();
    const auto candidates = table.candidates();
    bool refused = false;
    try {
        table.choose(0); // This two-character choice would overflow the bound.
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused && table.preedit() == before && table.candidates() == candidates &&
                table.has_reading(),
            "overflow refuses the choice without losing its reading or accepted text");
    table.backspace(); // Remove only the unchosen reading.
    table.type('a');
    table.choose(0);
    require(static_cast<std::size_t>(g_utf8_strlen(table.text().c_str(), -1)) == retained_limit,
            "the last fitting retained character is accepted");
    const auto full = table.text();
    refused = false;
    try {
        table.type('a');
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused && !table.has_reading() && table.text() == full,
            "a full retained buffer stays ready for an explicit commit");
    table.backspace();
    table.type('b');
    table.choose(0);
    require(g_utf8_validate(table.text().c_str(), -1, nullptr) && table.text().ends_with("月") &&
                static_cast<std::size_t>(g_utf8_strlen(table.text().c_str(), -1)) == retained_limit,
            "shortening full text frees room without corrupting UTF-8");
    table.cancel();
    require(!TableEngine::available(path.string() + ".missing"), "a missing table is unavailable");
}
// Caps Lock is the keyboard's state, whatever a layout does with its key, and
// every dead key shows its accent.
void locks_and_accents(const Profiles& profiles) {
    const auto find = [](const Layout& layout, const std::string& id) -> const Key& {
        return *std::find_if(layout.keys.begin(), layout.keys.end(),
                             [&](const Key& key) { return key.id == id; });
    };
    const auto& us = profiles.layouts.at("en-us-full");
    LanguageMap colemak(profiles.languages.at("en-us-colemak"));
    require(colemak.symbol(find(us, "KeyA"), {}, true, false) == XKB_KEY_A,
            "Caps Lock works on a layout that makes its key a Backspace");
    require(colemak.legend(find(us, "KeyA"), {}, true, false) == "A", "and the keys show it");
    LanguageMap english(profiles.languages.at("en-us"));
    require(english.symbol(find(us, "KeyA"), {key_code("ShiftLeft")}, true, false) == XKB_KEY_a,
            "Shift still undoes it");
    LanguageMap serbian(profiles.languages.at("sr-rs"));
    const std::set<int> altgr{key_code("AltRight")};
    require(serbian.legend(find(us, "Digit8"), altgr, false, false) == "◌̏" &&
                serbian.legend(find(us, "Digit0"), altgr, false, false) == "◌̑",
            "a dead accent shows on its key, not the digit");
}
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
        // Keyboard options of a language: the letters move, its own stay.
        {"fr-be", "international-full", {"KeyQ", "KeyW", "Digit2", "Semicolon"}, {"a", "z", "é", "m"}},
        {"nl-be", "international-full", {"KeyQ", "KeyW", "Semicolon"}, {"a", "z", "m"}},
        {"fr-fr-qwerty", "en-us-full", {"KeyQ", "KeyW", "Quote", "KeyE"}, {"q", "w", "é"}},
        {"de-de-qwerty", "en-us-full", {"KeyY", "KeyZ", "AltGr+KeyA"}, {"y", "z", "ä"}},
        {"cs-cz-qwerty", "international-full", {"KeyY", "KeyZ", "Digit2"}, {"y", "z", "ě"}},
        {"sk-sk-qwerty", "international-full", {"KeyY", "KeyZ", "Digit3"}, {"y", "z", "š"}},
        {"hu-hu-qwerty", "international-full", {"KeyY", "KeyZ"}, {"y", "z"}},
        {"hr-hr-qwerty", "en-us-full", {"KeyY", "KeyZ", "AltGr+BracketLeft"}, {"y", "z", "š"}},
        {"pl-pl-qwertz", "international-full", {"KeyY", "KeyZ"}, {"z", "y"}},
        {"tr-tr-f", "international-full", {"KeyQ", "KeyW", "KeyE"}, {"f", "g", "ğ"}},
        {"en-us-dvorak", "en-us-full", {"KeyS", "KeyD", "KeyJ", "Semicolon"}, {"o", "e", "h", "s"}},
        {"en-us-colemak", "en-us-full", {"KeyS", "KeyD", "KeyJ", "KeyK"}, {"r", "s", "n", "e"}},
        {"en-us-colemak-dh",
         "en-us-full",
         {"KeyS", "KeyG", "KeyH", "KeyC", "KeyB", "KeyM"},
         {"r", "g", "m", "d", "z", "h"}},
        {"en-us-intl", "en-us-full", {"KeyQ", "Quote", "KeyE"}, {"q", "é"}},
        {"bg-bg", "en-us-full", {"KeyD", "KeyF", "KeyJ"}, {"а", "о", "т"}},
        {"bg-bg-phonetic", "en-us-full", {"KeyQ", "KeyW", "KeyA"}, {"ч", "ш", "а"}},
        {"bg-bg-traditional", "en-us-full", {"KeyQ", "KeyW", "KeyA"}, {"я", "в", "а"}},
        {"pt-pt", "international-full", {"KeyQ", "Semicolon"}, {"q", "ç"}},
        {"es-419", "international-full", {"KeyQ", "Semicolon"}, {"q", "ñ"}},
        {"de-ch",
         "international-full",
         {"KeyY", "Semicolon", "Quote", "BracketLeft"},
         {"z", "ö", "ä", "ü"}},
        {"fr-ch",
         "international-full",
         {"KeyY", "Semicolon", "Quote", "BracketLeft"},
         {"z", "é", "à", "è"}},
        {"ca-es", "international-full", {"KeyQ", "Semicolon"}, {"q", "ñ"}},
        {"sr-rs", "en-us-full", {"KeyQ", "KeyW", "KeyA"}, {"љ", "њ", "а"}},
        {"sr-rs-latin",
         "en-us-full",
         {"KeyY", "BracketLeft", "Semicolon", "Quote"},
         {"z", "š", "č", "ć"}},
        {"fa-ir", "en-us-full", {"KeyQ", "KeyA"}, {"ض", "ش"}},
        {"fil-ph", "en-us-full", {"KeyQ", "AltGr+KeyN"}, {"q", "ñ"}},
        {"is-is", "international-full", {"Semicolon", "BracketLeft", "Slash"}, {"æ", "ð", "þ"}},
        {"ka-ge", "en-us-full", {"KeyQ", "KeyA"}, {"ქ", "ა"}},
        {"hy-am", "international-full", {"KeyQ", "KeyA"}, {"ճ", "ջ"}},
        {"kk-kz", "international-full", {"KeyQ", "Digit2"}, {"й", "ә"}},
        {"az-az", "international-full", {"KeyQ", "BracketRight", "Quote"}, {"q", "ğ", "ə"}},
        {"mk-mk", "international-full", {"KeyQ", "KeyA"}, {"љ", "а"}},
        {"sq-al", "en-us-full", {"KeyY", "Semicolon", "BracketLeft"}, {"z", "ë", "ç"}},
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
        require(map.right_to_left() == (id == "ar" || id == "fa-ir" || id == "he-il"),
                "Arabic, Persian and Hebrew legends are laid out right to left");
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
// A shell preview builds its engine on a worker thread while the keyboard
// types with its own. Both share module and engine state; creating, typing
// and destroying them at once, including the last Pinyin engine ending as the
// other thread makes a new one, must neither race nor crash.
void concurrent_engines(const std::string& method, const std::string& keys) {
    std::exception_ptr failure;
    std::mutex failure_mutex;
    auto work = [&] {
        try {
            for (int i = 0; i < 20; ++i) {
                CjkComposer composer(method);
                type(composer, keys);
                require(!composer.empty() && !composer.preedit().empty(), "each thread composes");
                (void)composer.candidates();
                composer.cancel();
            }
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            failure = failure ? failure : std::current_exception();
        }
    };
    std::thread preview(work);
    work();
    preview.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
void engines(const std::string& group = "") {
    if (group != "--chinese") {
        if (CjkComposer::supported("korean-2set")) {
            CjkComposer korean("korean-2set");
            type(korean, "gksrmf");
            require(korean.text() == "한글" && korean.finished() == "한",
                    "two-set Korean finishes 한 as 글 starts");
            korean.drop_finished();
            require(korean.text() == "글", "a sent syllable leaves only the open one");
            korean.backspace();
            require(korean.text() == "그", "Korean final-consonant backspace");
            korean.cancel();
            type(korean, "rkrk");
            require(korean.text() == "가가" && korean.finished() == "가",
                    "Korean trailing consonant moves to next syllable");
            korean.backspace();
            require(korean.text() == "가ㄱ", "Backspace edits only the open syllable");
            korean.cancel();
            type(korean, "ekfr");
            korean.backspace();
            require(korean.text() == "달", "Backspace takes one jamo of a double final");
            korean.cancel();
            type(korean, "Rk");
            require(korean.text() == "까", "Korean Shift doubles consonant");
        } else {
            require(!std::getenv("FRAMEKEYBOARD_TEST_CJK_REQUIRED"), "Korean engine required");
            std::cout << "Korean integration skipped: build lacks libhangul\n";
        }
    }
    if (group != "--korean") {
        // The real tables on a system that has them, or in
        // FRAMEKEYBOARD_TEST_TABLE_DIRECTORY; the fixture test covers logic.
        require(
            !std::getenv("FRAMEKEYBOARD_TEST_TABLE_DIRECTORY") ||
                (CjkComposer::supported("chinese-cangjie") && CjkComposer::supported("chinese-quick")),
            "the given tables open");
        if (CjkComposer::supported("chinese-cangjie")) {
            CjkComposer cangjie("chinese-cangjie");
            type(cangjie, "cnkq");
            require(cangjie.preedit() == "金弓大手" && cangjie.text() == "鍵",
                    "Cangjie looks up a code");
            cangjie.choose(0);
            require(!cangjie.has_reading() && cangjie.text() == "鍵", "a choice ends the reading");
            cangjie.cancel();
        }
        if (CjkComposer::supported("chinese-quick")) {
            CjkComposer quick("chinese-quick");
            type(quick, "cq");
            const auto keys = quick.candidates();
            require(std::find(keys.begin(), keys.end(), "鍵") != keys.end(), "Quick looks up two keys");
            const auto first = keys.front();
            type(quick, "a");
            require(quick.finished() == first && quick.preedit() == first + "日",
                    "a third Quick key types the highlighted character and starts the next code");
            quick.cancel();
        }
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
                // libime shows the syllables apart.
                require(chinese.preedit() == "ni hao", "undo chosen Pinyin segment");
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
            std::cout << "Chinese integration skipped: build lacks libime\n";
        }
    }
}
// The retained phonetic engines, each where its module was built:
// FRAMEKEYBOARD_CHINESE_ENGINE picks one. Each converts, chooses a phrase's
// start, and learns into its folder only when allowed.
void chinese_engines() {
    std::vector<std::vector<std::string>> remembered;
    const auto remember = [&](const auto& words) { remembered.push_back(words); };
    require(remember_picked_words({"甲", "乙", "丙"}, {true, false, true}, remember) &&
                remembered == std::vector<std::vector<std::string>>{{"甲"}, {"丙"}},
            "pick, accept, pick never learns adjacency across the accepted word");
    remembered.clear();
    require(remember_picked_words({"来", "一局", "马"}, {true, true}, remember) &&
                remembered == std::vector<std::vector<std::string>>{{"来"}, {"一局"}},
            "an accepted tail is not part of a picked run");
    remembered.clear();
    require(!remember_picked_words({"你好"}, {}, remember) && remembered.empty(),
            "an unpicked default after an earlier choice writes no history");
    {
        // Picks keep their Pinyin, so 得 picked for de isn't raised for dei (R139).
        using WordWithCode = std::pair<std::string, std::string>;
        std::vector<std::vector<WordWithCode>> coded;
        require(remember_picked_words(std::vector<WordWithCode>{{"得", "de"}, {"到", "dao"}},
                                      {true, false},
                                      [&](const auto& words) { coded.push_back(words); }) &&
                    coded == std::vector<std::vector<WordWithCode>>{{{"得", "de"}}},
                "a pick is remembered with its Pinyin");
    }
    const auto dir =
        fs::temp_directory_path() / ("framekeyboard-chinese-test-" + std::to_string(getpid()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            unsetenv("FRAMEKEYBOARD_CHINESE_ENGINE");
            fs::remove_all(path);
        }
    } cleanup{dir};
    const auto learned = [&](const char* engine) {
        std::error_code error;
        return !fs::is_empty(dir / engine, error) && !error;
    };
    setenv("FRAMEKEYBOARD_CHINESE_ENGINE", "libime", 1);
    if (CjkComposer::supported("chinese-pinyin-simplified")) {
        for (const auto* method : {"chinese-pinyin-simplified", "chinese-pinyin-traditional"}) {
            CjkComposer pinyin(method, dir);
            require(pinyin.engine() == "libime", "the override picks libime");
            type(pinyin, "woxiangmaiyitaidiannao");
            const bool simplified = std::string(method).ends_with("simplified");
            // OpenCC's Taiwan phrases write the official 臺, not the common 台.
            require(pinyin.text() == (simplified ? "我想买一台电脑" : "我想買一臺電腦"),
                    "libime converts a sentence, and OpenCC makes it Traditional");
            const auto candidates = pinyin.candidates();
            const auto first = std::find(candidates.begin(), candidates.end(), "我");
            require(first != candidates.end(), "libime offers a sentence's first word");
            pinyin.choose(static_cast<int>(first - candidates.begin()));
            require(pinyin.has_reading() && pinyin.preedit() == "我xiang mai yi tai dian nao",
                    "the rest of the reading stays after a partial choice, its syllables apart");
            pinyin.as_typed();
            require(pinyin.text() == "我xiangmaiyitaidiannao", "Enter types the rest as typed");
            pinyin.backspace();
            // libime splits dianna as 狄安娜's di an na.
            require(pinyin.preedit() == "我xiang mai yi tai di an na" &&
                        pinyin.text() != "我xiangmaiyitaidianna",
                    "Backspace takes a key of the rest first, and the conversion is back");
            pinyin.cancel();
            type(pinyin, "nihao");
            pinyin.choose(0);
            pinyin.backspace();
            require(pinyin.preedit() == "ni hao", "with nothing left, Backspace undoes the choice");
            pinyin.cancel();
            // A reading past 30 keys sends its converted front and goes on.
            type(pinyin, "wojintianwanshangyaoqupengyoujiachifan");
            require(!pinyin.finished().empty() && pinyin.text().starts_with(pinyin.finished()) &&
                        pinyin.preedit().ends_with("chi fan"),
                    "no key of a long reading is lost");
            const auto front = pinyin.finished();
            pinyin.cancel_open();
            require(pinyin.finished() == front && pinyin.text() == front,
                    "Esc keeps front text the field refused (R149)");
            type(pinyin, "ni");
            pinyin.cancel_open();
            require(pinyin.finished() == front && pinyin.text() == front,
                    "a reading typed after it goes first");
            pinyin.cancel_open();
            require(pinyin.empty(), "with nothing else left, Esc drops it too (R156)");
            type(pinyin, "wojintianwanshangyaoqupengyoujiachifan");
            pinyin.committed(false);
            require(!learned("libime"), "nothing is learned where learning isn't allowed");
            pinyin.cancel();
        }
        CjkComposer pinyin("chinese-pinyin-simplified", dir);
        type(pinyin, "steam");
        pinyin.as_typed();
        pinyin.committed(true);
        require(!learned("libime"), "letters typed as they are teach nothing");
        pinyin.cancel();
        type(pinyin, "nihao");
        pinyin.committed(true);
        require(!fs::exists(dir / "libime/user.history"), "a commit is not saved on its own");
        CjkComposer::save_learning();
        require(fs::exists(dir / "libime/user.history"), "a commit is learned into the folder");
        pinyin.cancel();
        type(pinyin, "laiyijuma");
        for (const auto* word : {"来", "一局", "马"}) {
            const auto offered = pinyin.candidates();
            const auto found = std::find(offered.begin(), offered.end(), word);
            require(found != offered.end(), "the mixed pick/default regression words are available");
            pinyin.choose(static_cast<int>(found - offered.begin()));
        }
        pinyin.committed(true);
        pinyin.cancel();
        type(pinyin, "laiyijuma");
        require(pinyin.text() != "来一局马", "an accepted default tail does not teach a whole word");
        pinyin.cancel();
        // Clear learned words: the composer forgets in memory and keeps its
        // reading (R145).
        const auto rank = [&](const char* word) {
            pinyin.cancel();
            type(pinyin, "ni");
            const auto offered = pinyin.candidates();
            const auto found = std::find(offered.begin(), offered.end(), word);
            require(found != offered.end(), "the clearing fixture offers 倪");
            return found - offered.begin();
        };
        const auto cold = rank("倪");
        pinyin.choose(static_cast<int>(cold));
        pinyin.committed(true);
        require(rank("倪") < cold, "a pick rises once learned");
        CjkComposer::clear_learning(dir);
        fs::remove_all(dir / "libime"); // As the app does next.
        require(pinyin.preedit() == "ni", "clearing keeps the open reading");
        require(rank("倪") == cold, "cleared, the pick is forgotten without a reload");
        pinyin.cancel();
        CjkComposer::save_learning();
        require(!fs::exists(dir / "libime/user.history"), "nothing forgotten is written back");
        CjkComposer elsewhere("chinese-pinyin-simplified");
        type(elsewhere, "nihao");
        elsewhere.committed(true); // No folder: nothing is written anywhere.
        // Exercise the private proposer too: cufan has complete two- and
        // three-character sentences interleaved before its prefix words.
        const auto module_path =
            fs::read_symlink("/proc/self/exe").parent_path() / "engines" / "framekeyboard-libime.so";
        void* module = dlopen(module_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(module != nullptr, "the tested libime module can be opened");
        const auto create =
            reinterpret_cast<CreateCjkEngine>(dlsym(module, "framekeyboard_create_cjk_v3"));
        require(create != nullptr, "the libime module exposes its factory");
        {
            std::unique_ptr<CjkEngine> sentences(create("chinese-pinyin-traditional-sentences", ""));
            sentences->type_reading("cufan");
            const auto offered = sentences->candidates();
            require(std::find(offered.begin(), offered.end(), "醋飯") != offered.end(),
                    "a complete shorter sentence survives other syllable splits");
            require(std::find(offered.begin(), offered.end(), "醋") == offered.end(),
                    "the proposer excludes words that consume only an input prefix");
        }
        dlclose(module);
    } else {
        std::cout << "libime skipped: build lacks the module\n";
    }
    setenv("FRAMEKEYBOARD_CHINESE_ENGINE", "mcbopomofo", 1);
    if (CjkComposer::supported("chinese-zhuyin")) {
        CjkComposer zhuyin("chinese-zhuyin", dir);
        require(zhuyin.engine() == "mcbopomofo" && zhuyin.phonetic_keys(),
                "the override picks McBopomofo");
        // ㄋㄧˇ ㄏㄠˇ: a tone finishes each syllable.
        type(zhuyin, "su3cl3");
        require(zhuyin.text() == "你好" && !zhuyin.takes_space(), "Zhuyin converts a phrase");
        const auto candidates = zhuyin.candidates();
        require(!candidates.empty() && candidates.front() == "你好",
                "the conversion comes first, for Space");
        // The cursor starts at the end, under the phrase just typed.
        auto shown = zhuyin.highlight();
        require(zhuyin.has_cursor() && shown && shown->begin == 0 && shown->end == 6 &&
                    shown->caret == 6,
                "the last phrase is highlighted, the cursor after it");
        require(std::find(candidates.begin(), candidates.end(), "你") == candidates.end(),
                "the candidates are for the phrase at the cursor");
        zhuyin.move(-1);
        const auto at_ni = zhuyin.candidates();
        const auto first = std::find(at_ni.begin(), at_ni.end(), "你");
        require(at_ni.front() == "你好" && first != at_ni.end() && zhuyin.highlight()->caret == 3,
                "← moves the cursor a syllable; the phrase over it, then its first character alone");
        zhuyin.choose(static_cast<int>(first - at_ni.begin()));
        shown = zhuyin.highlight();
        require(zhuyin.has_reading() && zhuyin.text() == "你好" && shown->begin == 0 && shown->end == 3,
                "a pick replaces that phrase and the reading stays open");
        zhuyin.move(1);
        zhuyin.move(1); // At the end: it stays.
        require(zhuyin.highlight()->caret == 6, "→ moves back to the end");
        zhuyin.set_choosing(true);
        require(zhuyin.choosing() && zhuyin.selected() == 0, "choosing starts on the phrase there");
        zhuyin.choose(0);
        require(zhuyin.has_reading() && !zhuyin.choosing() && zhuyin.text() == "你好",
                "a pick never ends the reading: only Enter sends it");
        // Punctuation goes into the reading at the cursor, as McBopomofo's.
        require(zhuyin.insert_symbol('<', "，") && zhuyin.text() == "你好，" &&
                    zhuyin.highlight()->caret == 9,
                "Shift+, puts ， in the reading");
        zhuyin.move(-1000);
        zhuyin.delete_forward();
        require(zhuyin.text() == "好，" && zhuyin.highlight()->caret == 0,
                "Home goes to the start, and Delete takes the syllable after the cursor");
        zhuyin.move(1000);
        type(zhuyin, "a8");
        zhuyin.cancel_open();
        require(zhuyin.text() == "好，", "Esc takes the open syllable first");
        zhuyin.cancel_open();
        require(zhuyin.empty(), "then the whole reading");
        // Typing goes in at the cursor, and Backspace takes the syllable before it.
        type(zhuyin, "su3cl3");
        zhuyin.move(-1);
        type(zhuyin, "cl3");
        require(g_utf8_strlen(zhuyin.text().c_str(), -1) == 3 && zhuyin.highlight()->caret == 6,
                "a syllable typed in the middle goes in at the cursor");
        zhuyin.backspace();
        require(zhuyin.text() == "你好", "Backspace takes the syllable before the cursor");
        zhuyin.cancel();
        // ㄇㄚ with Space as its first tone.
        type(zhuyin, "a8");
        require(zhuyin.takes_space() && zhuyin.preedit() == "ㄇㄚ",
                "an open syllable shows its symbols");
        zhuyin.type(' ');
        require(!zhuyin.takes_space() && !zhuyin.text().empty() && zhuyin.text() != "ㄇㄚ",
                "Space finishes the syllable in the first tone");
        zhuyin.backspace();
        require(zhuyin.empty(), "Backspace removes the syllable");
        // ㄨㄛˇ: rare characters (Extension A and later) come after common ones.
        type(zhuyin, "ji3");
        bool rare = false;
        for (const auto& candidate : zhuyin.candidates()) {
            const auto point = g_utf8_get_char(candidate.c_str());
            const bool this_rare = (point >= 0x3400 && point <= 0x4DBF) || point >= 0x20000;
            require(!rare || this_rare || g_utf8_strlen(candidate.c_str(), -1) > 1,
                    "no common character comes after a rare one");
            rare = rare || this_rare;
        }
        zhuyin.cancel();
        // A pick is learned once its text reached the field, and only then.
        const auto pick = [&](bool learn) {
            type(zhuyin, "su3cl3");
            const auto offered = zhuyin.candidates();
            const auto other = std::find(offered.begin(), offered.end(), "妳好");
            require(other != offered.end(), "Zhuyin offers 妳好 beside 你好");
            zhuyin.choose(static_cast<int>(other - offered.begin()));
            zhuyin.committed(learn);
            zhuyin.cancel();
        };
        pick(false);
        type(zhuyin, "su3cl3");
        require(zhuyin.text() == "你好" && !learned("mcbopomofo"),
                "a pick not allowed to teach teaches nothing");
        zhuyin.cancel();
        pick(true);
        require(fs::exists(dir / "mcbopomofo/picks.tsv"), "a committed pick is saved");
        type(zhuyin, "su3cl3");
        require(zhuyin.text() == "妳好", "the learned pick comes back for the same reading");
        zhuyin.cancel();
    } else {
        std::cout << "McBopomofo skipped: build lacks the module\n";
    }
}
void zhuyin_edited_picks() {
    if (!CjkComposer::supported("chinese-zhuyin")) {
        return;
    }
    const auto root =
        fs::temp_directory_path() / ("framekeyboard-zhuyin-edits-" + std::to_string(getpid()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directories(root);
    for (int edit = 0; edit < 5; ++edit) {
        const auto dir = root / std::to_string(edit);
        fs::create_directories(dir);
        const std::string expected = edit >= 3 ? "妳好" : "你好";
        {
            CjkComposer composer("chinese-zhuyin", dir);
            const auto choose = [&](const std::string& word) {
                const auto offered = composer.candidates();
                const auto found = std::find(offered.begin(), offered.end(), word);
                require(found != offered.end(), "the Zhuyin edit fixture offers its candidate");
                composer.choose(static_cast<int>(found - offered.begin()));
            };
            type(composer, "su3cl3");
            choose("妳好");
            if (edit == 0) {
                composer.move(-30);
                composer.delete_forward();
                type(composer, "su3");
            } else if (edit == 1) {
                composer.move(-1);
                type(composer, "8 ");
                composer.backspace();
            } else if (edit == 2) {
                choose("好"); // A later overlapping choice supersedes the whole phrase.
            } else if (edit == 4) {
                choose("妳好"); // Accepting the same row again must keep its learning intent.
            } else {
                composer.move(-30);
                type(composer, "8 ");
                composer.backspace(); // An edit before the phrase keeps the pick intact.
            }
            composer.move(30);
            require(composer.text() == expected, "the edit leaves the expected final phrase");
            composer.committed(true);
            composer.cancel();
            type(composer, "su3cl3");
            require(composer.text() == expected, "only surviving Zhuyin picks affect later conversion");
        }
        CjkComposer reloaded("chinese-zhuyin", dir);
        type(reloaded, "su3cl3");
        require(reloaded.text() == expected, "reloaded learning agrees with the committed phrase");
        if (edit < 3) {
            std::ifstream in(dir / "mcbopomofo/picks.tsv");
            const std::string saved{std::istreambuf_iterator<char>(in), {}};
            require(saved.find("妳好") == std::string::npos,
                    "edited or superseded whole-phrase picks are not persisted");
        }
    }
}
void traditional_selected_display(const fs::path& config) {
    CjkComposer composer("chinese-pinyin-traditional");
    const auto read = [&](const std::string& keys) {
        composer.cancel();
        type(composer, keys);
    };
    const auto choose = [&](const std::string& word) {
        const auto offered = composer.candidates();
        const auto found = std::find(offered.begin(), offered.end(), word);
        require(found != offered.end(), "the Traditional selection fixture offers its displayed word");
        composer.choose(static_cast<int>(found - offered.begin()));
    };
    read("huanghou");
    require(composer.text() == "皇后", "the complete Traditional phrase keeps its contextual spelling");
    choose("皇");
    if (!std::getenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY")) {
        require(composer.text() == "皇后" && composer.candidates().front() == "后",
                "a selected Traditional prefix supplies context to its default suffix");
    }
    choose("后");
    require(composer.text() == "皇后" && composer.preedit() == "皇后",
            "accepting the contextual suffix keeps both chosen surfaces");
    read("hulianwanghuanghou");
    choose("網際網路");
    choose("皇");
    choose("后");
    require(composer.text() == "網際網路皇后",
            "an expanded selected prefix does not shift suffix ownership");
    read("hulianwang");
    choose("互");
    choose("聯網");
    require(composer.text() == "互聯網",
            "an ambiguous expansion across the pick converts its suffix separately");
    read("faxingnihao");
    choose("發");
    choose("型");
    require(composer.text().starts_with("發型") && composer.preedit().starts_with("發型"),
            "later suffix selection preserves the exact displayed Traditional prefix");
    composer.type('a');
    require(composer.text().starts_with("發型") && composer.preedit().starts_with("發型"),
            "continued typing cannot reinterpret selected Traditional characters");
    composer.backspace();
    composer.as_typed();
    require(composer.text() == "發型nihao", "Enter's letters follow the selected display unchanged");
    for (int i = 0; i < 10 && composer.has_reading(); ++i) {
        composer.backspace();
    }
    require(composer.preedit() == "發型", "Backspace in the suffix retains selected display segments");
    composer.backspace();
    require(composer.preedit().starts_with("發") && !composer.preedit().starts_with("發型"),
            "undoing a selection removes only its display checkpoint");
    composer.cancel();
    require(composer.text().empty() && composer.preedit().empty(),
            "cancellation releases all selected display segments");
    type(composer, "faxing");
    const auto cold = composer.text();
    choose("髮型");
    require(composer.text() == "髮型" && composer.preedit() == "髮型",
            "the ordinary whole-word spelling still converts normally");
    composer.backspace();
    require(composer.text() == cold, "undoing the whole choice restores the cold conversion");
    read("faxingnihao");
    choose("發");
    choose("型");
    type(composer, "nihaonihaonihaonihaonihaonihao");
    require(composer.finished().starts_with("發型") && composer.text().starts_with(composer.finished()),
            "long-reading front sending preserves selected Traditional display");
    composer.drop_finished();
    require(!composer.preedit().starts_with("發型"), "a sent selected prefix is not duplicated");

    Options options;
    options.config_dir = config;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.target_language = "en-us";
    options.start_enabled = true;
    Sink sink;
    App app(options, sink);
    app.apply({"en-us-full", "zh-tw-pinyin", "graphite"});
    const auto app_choose = [&](const std::string& word) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            app.tick(2);
            const auto view = app.view();
            if (view.candidates) {
                for (const auto& chip : view.candidates->chips) {
                    if (chip.text == word || chip.text.ends_with(" " + word)) {
                        const auto x = chip.bounds.x + chip.bounds.width / 2;
                        const auto y = chip.bounds.y + chip.bounds.height / 2;
                        app.down(0, x, y, 3);
                        app.up(0, x, y);
                        return;
                    }
                }
            }
            for (const auto& control : view.controls) {
                if (control.id == "ime-more") {
                    const auto x = control.bounds.x + control.bounds.width / 2;
                    const auto y = control.bounds.y + control.bounds.height / 2;
                    app.down(0, x, y, 3);
                    app.up(0, x, y);
                    break;
                }
            }
        }
        throw std::runtime_error("the Traditional App fixture cannot find its displayed choice");
    };
    const auto app_read = [&] {
        sink.events.clear();
        for (const auto* key :
             {"KeyF", "KeyA", "KeyX", "KeyI", "KeyN", "KeyG", "KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}) {
            click(app, key);
        }
        app_choose("發");
        app_choose("型");
        app.tick(2);
    };
    const auto default_text = [&] {
        const auto view = app.view();
        require(view.candidates && !view.candidates->chips.empty(), "the remaining default is visible");
        const auto& label = view.candidates->chips.front().text;
        return "發型" + label.substr(label.find(' ') + 1);
    };
    app_read();
    const auto punctuation_text = default_text();
    click(app, "Comma");
    require(sink.events == std::vector<std::string>{punctuation_text, "，"},
            "implicit punctuation delivers selected Traditional surfaces and the visible suffix");
    app_read();
    click(app, "Enter");
    require(sink.events == std::vector<std::string>{"發型nihao"},
            "App Enter preserves selected Traditional display before the remaining letters");
    app_read();
    const auto space_text = default_text();
    click(app, "Space");
    require(sink.events == std::vector<std::string>{space_text},
            "App default commit preserves selected Traditional display");
}
void zhuyin_reranking() {
    const auto dir =
        fs::temp_directory_path() / ("framekeyboard-zhuyin-reranking-" + std::to_string(getpid()));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            unsetenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES");
            unsetenv("FRAMEKEYBOARD_ZHUYIN_RERANK_MODEL");
            fs::remove_all(path);
        }
    } cleanup{dir};
    const auto write_model = [&](std::string_view magic, float own_bonus = 0,
                                 std::string_view character = "妳") {
        std::vector<float> weights(PinyinReranker::weight_count);
        weights[PinyinReranker::feature_index("1:" + std::string(character))] = 100;
        weights[3] = own_bonus;
        std::ofstream file(dir / "model.fkzr", std::ios::binary);
        file << magic;
        for (const float weight : weights) {
            const auto bits = std::bit_cast<std::uint32_t>(weight);
            for (int shift = 0; shift < 32; shift += 8) {
                file.put(static_cast<char>(bits >> shift));
            }
        }
    };
    write_model("FKZYR001", 7);
    const auto model = PinyinReranker::load(dir / "model.fkzr", "FKZYR001");
    require(model && model->score("妳好", {}, 20, -20, 0, true) == 87 &&
                !PinyinReranker::load(dir / "model.fkzr"),
            "Zhuyin has a strict separate identity and source-indicator feature");
    write_model("FKPYR001");
    require(!PinyinReranker::load(dir / "model.fkzr", "FKZYR001"),
            "a Pinyin model cannot silently enable a different feature contract");
    write_model("FKZYR001");
    const auto module_path =
        fs::read_symlink("/proc/self/exe").parent_path() / "engines" / "framekeyboard-mcbopomofo.so";
    void* module = dlopen(module_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::cout << "McBopomofo reranking skipped: module unavailable\n";
        return;
    }
    const auto create = reinterpret_cast<CreateCjkEngine>(dlsym(module, "framekeyboard_create_cjk_v3"));
    require(create != nullptr, "McBopomofo exposes its existing factory");
    const auto make = [&] { return std::unique_ptr<CjkEngine>(create("chinese-zhuyin", "")); };
    const auto read = [](CjkEngine& engine, std::string_view keys) {
        for (char key : keys) {
            engine.type(key);
        }
    };
    setenv("FRAMEKEYBOARD_ZHUYIN_RERANK_MODEL", (dir / "model.fkzr").c_str(), 1);
    unsetenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES");
    {
        auto off = make();
        read(*off, "su3cl3");
        off->prefer({{"你好", "妳好"}});
        require(off->text() == "你好", "the model path alone leaves the hybrid unchanged");
    }
    setenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES", "1", 1);
    {
        auto engine = make();
        read(*engine, "su3cl3");
        engine->prefer({{"你好", "妳好"}});
        require(engine->text() == "妳好" && engine->candidates().front() == "妳好",
                "the synthetic model wins consistently in text and candidates");
        read(*engine, "a8");
        require(engine->text() == "妳好ㄇㄚ", "an ordinary unfinished syllable keeps the ranked prefix");
        engine->backspace();
        engine->cancel_open();
        require(engine->text() == "妳好", "clearing only the partial syllable keeps that prefix");
        engine->prefer({{"你好"}});
        require(engine->text() == "你好", "native own is rebuilt without stale preferred feedback");
        engine->prefer({{"你好", "妳好"}});
        const auto choices = engine->candidates();
        const auto ni = std::find(choices.begin(), choices.end(), "你好");
        require(ni != choices.end(), "the native correction remains selectable");
        engine->choose(static_cast<int>(ni - choices.begin()));
        engine->prefer({{"妳好", "你好"}});
        require(engine->text() == "你好", "an explicit choice outranks the global residual");
        engine->move(-1);
        read(*engine, "a8");
        require(engine->text() == "你ㄇㄚ好", "raw partial symbols stay at the middle cursor");
        engine->cancel_open();
        engine->move(1000);
        require(engine->insert_symbol('<', "，"), "punctuation can separate ranked runs");
        read(*engine, "su3cl3");
        engine->prefer({{"你好", "妳好"}, {"你好", "妳好"}});
        require(engine->text() == "你好，妳好", "fixed choices and punctuation isolate each run");
        engine->cancel();
        require(engine->text().empty() && engine->pinyin_runs().empty(), "cancel discards ranked state");
    }
    write_model("FKZYR001", 1000);
    {
        auto engine = make();
        read(*engine, "su3cl3");
        engine->prefer({{"你好", "妳好"}});
        require(engine->text() == "妳好",
                "own already proposed keeps proposal features without duplication");
        engine->prefer({{"妳好"}});
        require(engine->text() == "你好",
                "a winning separate own hypothesis leaves the native grid unforced");
        engine->prefer({{"wrong reading"}});
        require(engine->text() == "你好", "no fitting proposal keeps the exact-fit native conversion");
    }
    write_model("FKZYR001");
    {
        // Pick and persist a native correction, then make the global model
        // prefer its competitor. The saved native override still wins.
        const auto learning = dir / "learning";
        {
            std::unique_ptr<CjkEngine> engine(create("chinese-zhuyin", learning.c_str()));
            read(*engine, "su3cl3");
            const auto choices = engine->candidates();
            const auto other = std::find(choices.begin(), choices.end(), "妳好");
            require(other != choices.end(), "the learned-choice fixture offers its correction");
            engine->choose(static_cast<int>(other - choices.begin()));
            engine->committed(true);
        }
        write_model("FKZYR001", 0, "你");
        std::string native_learned;
        unsetenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES");
        {
            std::unique_ptr<CjkEngine> native(create("chinese-zhuyin", learning.c_str()));
            read(*native, "su3cl3");
            native->prefer({{"你好", "妳好"}});
            native_learned = native->text();
        }
        setenv("FRAMEKEYBOARD_ZHUYIN_SENTENCES", "1", 1);
        {
            std::unique_ptr<CjkEngine> reloaded(create("chinese-zhuyin", learning.c_str()));
            read(*reloaded, "su3cl3");
            reloaded->prefer({{"你好", "妳好"}});
            require(reloaded->text() == native_learned && native_learned == "妳好",
                    "reloaded native learning bypasses the opposed residual");
        }
        fs::remove_all(learning);
        {
            std::unique_ptr<CjkEngine> cleared(create("chinese-zhuyin", learning.c_str()));
            read(*cleared, "su3cl3");
            cleared->prefer({{"你好", "妳好"}});
            require(cleared->text() == "你好", "clearing learned words restores the cold conversion");
        }
        write_model("FKZYR001");
    }
    {
        auto long_reading = make();
        for (int i = 0; i < 29; ++i) {
            read(*long_reading, "su3");
        }
        long_reading->prefer({{std::string()}}); // No fitting proposal: native own remains usable.
        read(*long_reading, "a");
        const auto before = long_reading->text();
        read(*long_reading, "8"); // Sends a completed front while the raw syllable remains open.
        long_reading->prefer({{}});
        require(long_reading->text() == before + "ㄚ" && !long_reading->finished().empty(),
                "long-front processing preserves all completed text and the separate partial tail");
        const auto sent = long_reading->finished();
        long_reading->drop_finished();
        require(sent + long_reading->text() == before + "ㄚ",
                "dropping the delivered front never duplicates or loses ranked text");
    }
    write_model("FKPYR001");
    {
        auto invalid = make();
        read(*invalid, "su3cl3");
        invalid->prefer({{"你好", "妳好"}});
        require(invalid->text() == "你好", "invalid model identity retains old hybrid behavior");
    }
    dlclose(module);
}
void pinyin_reranking() {
    const auto dir =
        fs::temp_directory_path() / ("framekeyboard-pinyin-reranking-" + std::to_string(getpid()));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            unsetenv("FRAMEKEYBOARD_PINYIN_NBEST");
            unsetenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY");
            unsetenv("FRAMEKEYBOARD_CHINESE_ENGINE");
            fs::remove_all(path);
        }
    } cleanup{dir};
    require(PinyinReranker::feature_index("123456789") ==
                32 + (0xcbf43926U % (PinyinReranker::weight_count - 32)),
            "residual features use the specified CRC32 mapping");
    const auto write_model = [&](const char* name, std::string_view character,
                                 std::size_t count = PinyinReranker::weight_count,
                                 bool counted = false) {
        std::vector<float> weights(count);
        weights[PinyinReranker::feature_index("1:" + std::string(character), count)] = 100;
        std::ofstream file(dir / name, std::ios::binary);
        if (counted || count != PinyinReranker::weight_count) {
            file << PinyinReranker::counted_magic;
            for (int shift = 0; shift < 32; shift += 8) {
                file.put(static_cast<char>(count >> shift));
            }
        } else {
            file << PinyinReranker::magic;
        }
        for (const float weight : weights) {
            const auto bits = std::bit_cast<std::uint32_t>(weight);
            for (int shift = 0; shift < 32; shift += 8) {
                file.put(static_cast<char>(bits >> shift));
            }
        }
    };
    write_model("simplified.fkpr", "拟");
    write_model("traditional.fkpr", "擬");
    write_model("wide.fkpr", "擬", PinyinReranker::wide_weight_count);
    write_model("counted32.fkpr", "拟", PinyinReranker::weight_count, true);
    auto wide = PinyinReranker::load(dir / "wide.fkpr");
    auto counted = PinyinReranker::load(dir / "counted32.fkpr");
    require(wide && wide->score("擬好", {"拟好"}, 0, -3., -3.) == 100. && counted &&
                counted->score("拟好", {"拟好"}, 0, -3., -3.) == 100.,
            "counted models use their actual feature width while supporting both frozen sizes");
    require(PinyinReranker::feature_index("1:擬", PinyinReranker::wide_weight_count) !=
                PinyinReranker::feature_index("1:擬"),
            "the wide fixture distinguishes actual-width hashing from legacy modulo");
    {
        std::ofstream file(dir / "bad-count.fkpr", std::ios::binary);
        file << PinyinReranker::counted_magic;
        for (const unsigned char byte : {0, 0, 1, 0}) {
            file.put(static_cast<char>(byte)); // 65536 is not a frozen supported width.
        }
    }
    require(!PinyinReranker::load(dir / "bad-count.fkpr"),
            "unsupported widths are rejected before allocation");
    fs::copy_file(dir / "wide.fkpr", dir / "wide-trailing.fkpr");
    {
        std::ofstream file(dir / "wide-trailing.fkpr", std::ios::binary | std::ios::app);
        file.put('x');
    }
    require(!PinyinReranker::load(dir / "wide-trailing.fkpr"), "wide trailing bytes are rejected");
    fs::copy_file(dir / "wide.fkpr", dir / "wide-nan.fkpr");
    {
        std::fstream file(dir / "wide-nan.fkpr", std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(12);
        for (const unsigned char byte : {0, 0, 0xc0, 0x7f}) {
            file.put(static_cast<char>(byte));
        }
    }
    require(!PinyinReranker::load(dir / "wide-nan.fkpr"), "nonfinite wide model weights are rejected");
    {
        std::ofstream file(dir / "wide-truncated.fkpr", std::ios::binary);
        file << PinyinReranker::counted_magic;
        file.write("\0\0\2\0", 4);
        file << "short";
    }
    require(!PinyinReranker::load(dir / "wide-truncated.fkpr"),
            "truncated counted payloads are rejected");
    auto model = PinyinReranker::load(dir / "simplified.fkpr");
    require(model && model->score("拟好", {"拟好"}, 0, -3., -3.) == 100.,
            "the fixed scorer matches the synthetic UTF-8 feature model");
    std::ofstream(dir / "broken.fkpr") << PinyinReranker::magic << "short";
    require(!PinyinReranker::load(dir / "missing.fkpr") && !PinyinReranker::load(dir / "broken.fkpr"),
            "missing and truncated experimental models are rejected");
    {
        std::ofstream file(dir / "simplified.fkpr", std::ios::binary | std::ios::app);
        file.put('x');
    }
    require(!PinyinReranker::load(dir / "simplified.fkpr"), "a model with trailing data is rejected");
    write_model("simplified.fkpr", "拟");
    setenv("FRAMEKEYBOARD_CHINESE_ENGINE", "libime", 1);
    if (!CjkComposer::supported("chinese-pinyin-simplified")) {
        return;
    }
    for (const auto* method : {"chinese-pinyin-simplified", "chinese-pinyin-traditional"}) {
        const bool simplified = std::string(method).ends_with("simplified");
        const std::vector<std::string> homophones = simplified
                                                        ? std::vector<std::string>{"师", "诗", "使"}
                                                        : std::vector<std::string>{"師", "詩", "使"};
        std::vector<std::pair<std::string, std::string>> native_short_defaults;
        for (const auto* keys : {"shi", "ni", "wo", "de", "le", "hao", "zai"}) {
            CjkComposer native(method);
            type(native, keys);
            require(utf8_length(native.text()) == 1, "the isolated-character fixture is one character");
            native_short_defaults.emplace_back(keys, native.text());
        }
        const auto preserved_short_defaults = [&] {
            for (const auto& [keys, expected] : native_short_defaults) {
                CjkComposer isolated(method);
                type(isolated, keys);
                require(isolated.text() == expected && isolated.candidates().front() == expected,
                        "isolated characters keep the native frequency/preference default");
            }
        };
        std::vector<std::pair<std::string, std::vector<std::string>>> baseline_choices;
        for (const auto* keys : {"shi", "shishi", "woxiangmaiyitaidiannao"}) {
            CjkComposer baseline(method);
            type(baseline, keys);
            baseline_choices.emplace_back(keys, baseline.candidates());
        }
        const auto retained_choices = [&] {
            for (const auto& [keys, baseline] : baseline_choices) {
                CjkComposer expanded(method);
                type(expanded, keys);
                const auto offered = expanded.candidates();
                for (const auto& choice : baseline) {
                    require(std::find(offered.begin(), offered.end(), choice) != offered.end(),
                            "extra sentence hypotheses never displace baseline correction choices");
                }
            }
        };
        const auto short_choices = [&] {
            CjkComposer short_reading(method);
            type(short_reading, "shi");
            for (const auto& word : homophones) {
                const auto offered = short_reading.candidates();
                const auto found = std::find(offered.begin(), offered.end(), word);
                require(found != offered.end(),
                        "non-leading short-reading homophones remain offered in every search mode");
                short_reading.choose(static_cast<int>(found - offered.begin()));
                require(short_reading.text() == word && !short_reading.has_reading(),
                        "a non-leading short-reading homophone remains selectable");
                short_reading.cancel();
                type(short_reading, "shi");
            }
        };
        short_choices();
        if (!simplified) {
            traditional_selected_display(dir / "traditional-display-normal");
            // At the limit, after the selected 我, the first word goes ahead
            // only if it has a prefix row of its own: whether it does depends
            // on the language model. Either way each key stays exactly once:
            // what went ahead is out of the reading, the rest is in it.
            const std::string reading = "wodiangaonihaonihaonihaonihaon";
            CjkComposer fuzzy("chinese-pinyin-traditional");
            type(fuzzy, reading);
            const auto offered = fuzzy.candidates();
            const auto prefix = std::find(offered.begin(), offered.end(), "我");
            require(prefix != offered.end(), "the fuzzy-front fixture offers its selected prefix");
            fuzzy.choose(static_cast<int>(prefix - offered.begin()));
            fuzzy.type('a');
            const auto front = fuzzy.finished();
            require(front.starts_with("我"), "the selected prefix goes ahead");
            fuzzy.drop_finished();
            auto remaining = fuzzy.preedit();
            std::erase(remaining, ' ');
            const auto keys = reading + "a";
            require(keys.ends_with(remaining) && (front == "我" ? remaining.size() == keys.size() - 2
                                                                : remaining.size() < keys.size() - 2),
                    "front sending never appends an unconsumed fuzzy word; each key remains once");
        }
        setenv("FRAMEKEYBOARD_PINYIN_NBEST", "20", 1);
        short_choices();
        preserved_short_defaults();
        retained_choices();
        if (!simplified) {
            traditional_selected_display(dir / "traditional-display-wide");
        }
        {
            CjkComposer search(method);
            type(search, "nihao");
            require(search.text() == "你好", "wider search alone preserves the familiar default");
            const auto offered = search.candidates();
            require(std::find(offered.begin(), offered.end(), "你") != offered.end(),
                    "wider search preserves prefix-word correction choices");
        }
        setenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY", dir.c_str(), 1);
        short_choices();
        preserved_short_defaults();
        retained_choices();
        if (!simplified) {
            traditional_selected_display(dir / "traditional-display-ranked");
        }
        {
            CjkComposer prefixed(method);
            type(prefixed, "woni");
            const auto offered = prefixed.candidates();
            const auto prefix = std::find(offered.begin(), offered.end(), "我");
            require(prefix != offered.end(), "the boundary fixture offers its explicit prefix");
            prefixed.choose(static_cast<int>(prefix - offered.begin()));
            require(prefixed.text() == (simplified ? "我拟" : "我擬"),
                    "the isolated guard does not silently apply to a selected-prefix composition");
            prefixed.cancel();
            type(prefixed, "ni");
            require(prefixed.text() == "你", "cancellation restores isolated-character handling");
        }
        const auto short_learning = dir / method / "short-preference";
        fs::create_directories(short_learning);
        {
            CjkComposer personal(method, short_learning);
            type(personal, "ni");
            const auto offered = personal.candidates();
            const auto correction = std::find(offered.begin(), offered.end(), "倪");
            require(correction != offered.end(), "a nondefault isolated character remains selectable");
            personal.choose(static_cast<int>(correction - offered.begin()));
            personal.committed(true);
        }
        {
            CjkComposer personal(method, short_learning);
            type(personal, "ni");
            require(personal.text() == "倪", "the isolated guard preserves native learning on reload");
            CjkComposer other(method);
            type(other, "ni");
            require(other.text() == "你", "isolated personal preference stays in its own profile");
        }
        fs::remove_all(short_learning);
        {
            CjkComposer cleared(method, short_learning);
            type(cleared, "ni");
            require(cleared.text() == "你", "clearing native learning restores the cold default");
        }
        const auto whole_learning = dir / method / "whole-preference";
        fs::create_directories(whole_learning);
        {
            CjkComposer personal(method, whole_learning);
            type(personal, "nihao");
            require(personal.text() != "你好", "the synthetic global model opposes this correction");
            const auto offered = personal.candidates();
            const auto correction = std::find(offered.begin(), offered.end(), "你好");
            require(correction != offered.end(), "the whole-word correction remains selectable");
            personal.choose(static_cast<int>(correction - offered.begin()));
            personal.committed(true);
            personal.cancel();
            type(personal, "nihao");
            require(personal.text() == "你好", "global residuals respect a learned whole native winner");
        }
        {
            CjkComposer reloaded(method, whole_learning);
            type(reloaded, "nihao");
            require(reloaded.text() == "你好", "whole-reading preference survives native reload");
            CjkComposer other(method);
            type(other, "nihao");
            require(other.text() != "你好", "whole-reading preference stays inside its profile");
        }
        fs::remove_all(whole_learning);
        {
            CjkComposer cleared(method, whole_learning);
            type(cleared, "nihao");
            require(cleared.text() != "你好", "native clearing removes whole-reading protection");
        }
        const auto component_learning = dir / method / "component-preference";
        fs::create_directories(component_learning);
        {
            CjkComposer component(method, component_learning);
            type(component, "ni");
            component.committed(true);
            component.cancel();
            type(component, "hao");
            component.committed(true);
            component.cancel();
            type(component, "nihao");
            require(component.text() != "你好",
                    "known components and boundaries cannot replace a missing internal transition");
        }
        for (const auto& [keys, phrase] : std::vector<std::pair<std::string, std::string>>{
                 {"wonihao", "我你好"}, {"nihaowo", "你好我"}}) {
            const auto learning = dir / method / ("boundary-" + keys);
            fs::create_directories(learning);
            CjkComposer boundary(method, learning);
            type(boundary, keys);
            const auto offered = boundary.candidates();
            const auto picked = std::find(offered.begin(), offered.end(), phrase);
            require(picked != offered.end(), "the boundary fixture offers its complete phrase");
            boundary.choose(static_cast<int>(picked - offered.begin()));
            boundary.committed(true);
            boundary.cancel();
            type(boundary, "nihao");
            require(boundary.text() != "你好",
                    "an internal learned transition without both sentence boundaries is insufficient");
        }
        const auto legacy_learning = dir / method / "legacy-preference";
        fs::create_directories(legacy_learning);
        {
            CjkComposer legacy(method, legacy_learning);
            type(legacy, "nirenma");
            const auto offered = legacy.candidates();
            const auto phrase = std::find(offered.begin(), offered.end(), "泥人");
            require(phrase != offered.end(), "the legacy-history fixture offers its picked word");
            legacy.choose(static_cast<int>(phrase - offered.begin()));
            require(legacy.has_reading() && !legacy.candidates().empty(),
                    "the legacy fixture has a default tail to accept");
            legacy.choose(0);
            legacy.committed(true); // Mixed picks save 泥人 alone without a Pinyin code.
        }
        {
            CjkComposer legacy(method, legacy_learning);
            type(legacy, "niren");
            require(legacy.text() == "泥人", "whole-reading protection includes legacy uncoded history");
        }
        CjkComposer ranked(method, dir / method);
        type(ranked, "nihao");
        const auto delivered = ranked.text();
        require(delivered != "你好" && delivered == ranked.candidates().front(),
                "the synthetic residual changes the actual default and its visible row together");
        ranked.choose(0);
        require(ranked.text() == delivered && !ranked.has_reading(),
                "choosing the first visible candidate selects its mapped source sentence");
        ranked.cancel();
        type(ranked, "nihao");
        require(ranked.text() == delivered, "the fresh default is stable");
        ranked.committed(true);
        require(ranked.text() == delivered && !ranked.has_reading(),
                "implicit delivery learns the delivered residual sentence rather than native row zero");
        ranked.cancel();
        type(ranked, "nihaoma");
        const auto offered = ranked.candidates();
        const auto prefix = std::find(offered.begin(), offered.end(), "你");
        require(prefix != offered.end(), "a deliberate prefix correction remains accessible");
        ranked.choose(static_cast<int>(prefix - offered.begin()));
        require(ranked.text().starts_with("你"), "reranking preserves a selected prefix");
        ranked.type('n');
        require(ranked.text().starts_with("你"), "later input cannot reinterpret that prefix");
        ranked.cancel();
        type(ranked, "nihaonihaonihaonihaonihaonihao");
        const auto before_front = ranked.text();
        ranked.type('n');
        require(!ranked.finished().empty() && before_front.starts_with(ranked.finished()),
                "front sending consumes the same default sentence shown before the limit");
        {
            Options options;
            options.config_dir = dir / method / "app";
            options.input = InputBackend::Ei;
            options.mode = Mode::Vr;
            options.target_language = "en-us";
            options.start_enabled = true;
            Sink sink;
            App app(options, sink);
            app.apply({"en-us-full",
                       std::string(method).ends_with("simplified") ? "zh-cn-pinyin" : "zh-tw-pinyin",
                       "graphite"});
            const auto reading = [&] {
                for (const auto* key : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}) {
                    click(app, key);
                }
                app.tick(2);
            };
            reading();
            click(app, "Space");
            require(sink.events == std::vector<std::string>{delivered},
                    "App Space delivers the visible residual default");
            sink.events.clear();
            reading();
            const auto view = app.view();
            require(view.candidates && !view.candidates->chips.empty(),
                    "App displays the residual candidate row");
            const auto bounds = view.candidates->chips.front().bounds;
            app.down(0, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2, 3);
            app.up(0, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
            require(sink.events == std::vector<std::string>{delivered},
                    "clicking the first App candidate delivers the same sentence as Space");
            sink.events.clear();
            reading();
            click(app, "Comma");
            require(sink.events == std::vector<std::string>{delivered, "，"},
                    "implicit punctuation delivers the residual sentence before its punctuation");
            sink.events.clear();
            reading();
            click(app, "Enter");
            require(sink.events == std::vector<std::string>{"nihao"},
                    "Enter still sends Pinyin as typed while residual ranking is enabled");
        }
        unsetenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY");
        unsetenv("FRAMEKEYBOARD_PINYIN_NBEST");
    }
    const auto mixed = dir / "mixed-models";
    fs::create_directory(mixed);
    write_model("mixed-models/simplified.fkpr", "拟");
    write_model("mixed-models/traditional.fkpr", "擬", PinyinReranker::wide_weight_count);
    setenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY", mixed.c_str(), 1);
    {
        CjkComposer small("chinese-pinyin-simplified", dir / "mixed-learning");
        CjkComposer large("chinese-pinyin-traditional", dir / "mixed-learning");
        type(small, "nihao");
        type(large, "nihao");
        require(small.text() == "拟好" && large.text() == "擬好",
                "a mixed-size bundle loads both scripts");
        const auto offered = small.candidates();
        const auto pick = std::find(offered.begin(), offered.end(), "你好");
        require(pick != offered.end(), "the mixed-size correction remains offered");
        small.choose(static_cast<int>(pick - offered.begin()));
        small.committed(true);
        large.cancel();
        type(large, "nihao");
        require(large.text() == "你好", "mixed sizes share native mutable learning ownership");
    }
    const auto partial = dir / "partial-models";
    fs::create_directory(partial);
    fs::copy_file(dir / "simplified.fkpr", partial / "simplified.fkpr");
    setenv("FRAMEKEYBOARD_PINYIN_RERANK_DIRECTORY", partial.c_str(), 1);
    // A live Traditional context must observe Simplified's learning when
    // both methods use one folder, even with a half-present model bundle.
    CjkComposer simplified("chinese-pinyin-simplified", dir / "shared-learning");
    CjkComposer traditional("chinese-pinyin-traditional", dir / "shared-learning");
    CjkComposer isolated("chinese-pinyin-traditional", dir / "other-learning");
    type(simplified, "nihao");
    type(traditional, "nihao");
    require(simplified.text() == "你好" && traditional.text() == "你好",
            "a half-present model bundle disables residual ranking atomically");
    traditional.cancel();
    const auto offered = simplified.candidates();
    const auto corrected = std::find(offered.begin(), offered.end(), "拟好");
    require(corrected != offered.end(), "the shared-learning fixture offers its explicit correction");
    simplified.choose(static_cast<int>(corrected - offered.begin()));
    simplified.committed(true);
    type(traditional, "nihao");
    type(isolated, "nihao");
    require(traditional.text() == "擬好" && isolated.text() == "你好",
            "partial-bundle fallback shares live learning by folder while isolating other profiles");
    std::ofstream(partial / "traditional.fkpr") << "invalid";
    CjkComposer invalid_simplified("chinese-pinyin-simplified", dir / "invalid-learning");
    CjkComposer invalid_traditional("chinese-pinyin-traditional", dir / "invalid-learning");
    type(invalid_simplified, "nihao");
    type(invalid_traditional, "nihao");
    require(invalid_simplified.text() == "你好" && invalid_traditional.text() == "你好",
            "an invalid model in either script disables the whole residual bundle");
}
// The simulator's --chinese-candidates: each method by the line's script, then
// every engine where it has the method.
void chinese_report() {
    const auto report = [](const ChineseCandidatesOptions& options) {
        std::istringstream readings("nihao\n# a note\n\nㄋㄧˇ ㄏㄠˇ\n");
        std::ostringstream out;
        report_chinese_candidates(readings, out, options);
        return out.str();
    };
    const auto plain = report({});
    require(plain.find("# a note\n\n") != std::string::npos, "notes and empty lines are copied");
    require(plain.find("  chinese-pinyin-simplified (") != std::string::npos &&
                plain.find("  chinese-pinyin-traditional (") != std::string::npos,
            "a Latin line is typed with both Pinyin methods");
    const auto zhuyin = plain.find("ㄋㄧˇ ㄏㄠˇ\n  chinese-zhuyin (");
    require(zhuyin != std::string::npos && plain.find("你好", zhuyin) != std::string::npos,
            "Zhuyin symbols are typed as Zhuyin, and convert");
    const auto all = report({{}, {"libime", "mcbopomofo"}, 3});
    for (const auto* engine : {"libime", "mcbopomofo"}) {
        setenv("FRAMEKEYBOARD_CHINESE_ENGINE", engine, 1);
        const bool built = CjkComposer::supported(
            std::string(engine) == "libime" ? "chinese-pinyin-simplified" : "chinese-zhuyin");
        unsetenv("FRAMEKEYBOARD_CHINESE_ENGINE");
        require(!built || all.find("(" + std::string(engine) + ")  ") != std::string::npos,
                "each engine built is compared");
    }
    require(all.find("chinese-zhuyin (libime)") == std::string::npos &&
                all.find("chinese-pinyin-simplified (mcbopomofo)") == std::string::npos,
            "an engine without the method is left out");
    require(!std::getenv("FRAMEKEYBOARD_CHINESE_ENGINE"), "the report puts the engine choice back");
}
// Overflow text still belongs to the composer until the output accepts it.
void chinese_pending_front() {
    for (const auto* method : {"chinese-pinyin-simplified", "chinese-zhuyin"}) {
        if (!CjkComposer::supported(method)) {
            continue;
        }
        CjkComposer composer(method);
        if (composer.has_cursor()) {
            for (int i = 0; i < 29; ++i) {
                type(composer, "8 ");
            }
            require(composer.insert_symbol('<', "，"), "punctuation fits after a long Zhuyin reading");
        } else {
            type(composer, "wojintianwanshangyaoqupengyoujiachifan");
        }
        const auto pending = composer.finished();
        require(!pending.empty(), "the long reading produces an unsent prefix");
        for (int i = 0; i < 100 && composer.preedit() != pending; ++i) {
            composer.backspace();
        }
        require(composer.text() == pending && !composer.empty(),
                "deleting the remaining reading keeps the unsent prefix committable");
        composer.backspace();
        require(utf8_length(composer.text()) + 1 == utf8_length(pending),
                "Backspace can edit the last character of an unsent prefix");
        composer.drop_finished();
        require(composer.empty(), "acknowledging the prefix empties the composer");
    }
}
// Chinese keys as a PC input method's: Chinese punctuation after the reading,
// plain . , : after a digit, Pinyin's letters as typed with Enter, Shift
// letters at once, and - = paging the candidates.
// A terminal the output detects, with no shell hint, learns nothing (R138).
void chinese_terminal_learning() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-terminal-learning-" + std::to_string(getpid()));
    fs::create_directories(path);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    struct TerminalSink : Sink {
        EditKeys keys{EditKeys::CtrlShift};
        EditKeys edit_keys() const override { return keys; }
    };
    Options options;
    options.config_dir = path;
    options.input = InputBackend::Ei;
    options.mode = Mode::Vr;
    options.target_language = "en-us";
    options.start_enabled = true;
    TerminalSink sink;
    App app(options, sink);
    app.apply({"en-us-full", "zh-cn-pinyin", "graphite"});
    const auto learned = path / "chinese" / "libime" / "user.history";
    const auto type_and_pick = [&] {
        for (const auto* action : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}) {
            click(app, action);
        }
        click(app, "Digit2"); // a pick, not the top candidate
        if (!app.view().preedit.empty()) {
            click(app, "Space"); // the rest as offered (Enter would type the letters)
        }
    };
    type_and_pick();
    app.tick(10);
    require(!sink.events.empty() && !fs::exists(learned), "a detected terminal learns nothing");
    sink.keys = EditKeys::Standard;
    type_and_pick();
    // Saved once typing pauses for 3 s, not on the commit (R142).
    app.tick(11);
    require(!fs::exists(learned), "a learned pick waits for the pause");
    app.tick(13.5);
    require(fs::exists(learned), "an ordinary field learns the pick");
    // Opening Settings writes learning still waiting, for Clear (R155).
    fs::remove(learned);
    type_and_pick();
    require(!fs::exists(learned), "the next pick waits for the pause");
    app.show_settings();
    require(fs::exists(learned), "Settings opens with what was learned written");
    app.apply({"en-us-full", "en-us", "graphite"});
}
void chinese_keys(bool flicks = false) {
    const auto path = fs::temp_directory_path() / ("framekeyboard-chinese-keys-" +
                                                   std::to_string(getpid()) + (flicks ? "-flicks" : ""));
    fs::create_directories(path);
    std::ofstream(path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"key_flicks":)"
        << (flicks ? "true}" : "false}");
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
    Sink sink;
    App app(options, sink);
    // Keys in order; "Shift+" latches Shift first.
    require(app.view().key_flick_hints == flicks,
            "the punctuation fixture uses the requested input path");
    const auto press = [&](std::initializer_list<std::string> keys) {
        sink.events.clear();
        for (auto key : keys) {
            if (key.starts_with("Shift+")) {
                click(app, "ShiftLeft");
                key.erase(0, 6);
            }
            click(app, key);
        }
    };
    using Typed = std::vector<std::string>;
    app.apply({"en-us-full", "zh-cn-pinyin", "graphite"});
    press({"KeyN", "KeyI", "Comma"});
    require(sink.events == Typed{"你", "，"}, "a comma types the reading, then ，");
    press({"Shift+Quote", "Shift+Quote", "Shift+Digit1", "Backslash", "Shift+Digit4"});
    require(sink.events == Typed{"“", "”", "！", "、", "￥"}, "quotes take turns; ！ 、 ￥");
    press({"Digit3", "Period", "Digit5", "Shift+Semicolon", "Comma"});
    require(sink.events == Typed{"3", ".", "5", ":", "，"}, "after a digit, . and : type themselves");
    press({"Digit1", "Comma", "Digit2", "Comma", "Period"});
    require(sink.events == Typed{"1", ",", "2", ",", "。"},
            "a numeric comma consumes the exception before ordinary Chinese punctuation");
    press({"Digit3", "KeyN", "KeyI", "Space", "Period"});
    require(sink.events == Typed{"3", "你", "。"}, "composition clears the preceding digit context");
    press({"Digit3", "Shift+Period", "Digit4", "Shift+Comma"});
    require(sink.events == Typed{"3", "》", "4", "《"},
            "after a digit, Shift+. and Shift+, stay 》《 (R136)");
    press({"Digit3"});
    app.set_interaction_active(false);
    app.set_interaction_active(true);
    press({"Period"});
    require(sink.events == Typed{"。"},
            "a digit before the keyboard was hidden no longer counts (R137)");
    press({"Digit3"});
    app.set_dragging(true, false);
    app.set_dragging(false, false);
    press({"Period", "Digit5"});
    require(sink.events == Typed{".", "5"}, "moving the panel keeps the digit before (R158)");
    press({"Shift+Quote"});
    app.set_interaction_active(false);
    app.set_interaction_active(true);
    press({"Shift+Quote", "Shift+Quote"});
    require(sink.events == Typed{"“", "”"}, "reopened, the first quote opens again (R144)");
    press({"KeyS", "KeyT", "KeyE", "KeyA", "KeyM", "Enter"});
    require(sink.events == Typed{"steam"} && app.view().preedit.empty(), "Enter types Pinyin's letters");
    press({"Shift+KeyO", "KeyK"});
    require(sink.events == Typed{"O"} && app.view().preedit == "k",
            "a Shift letter types itself at once");
    click(app, "Escape");
    press({"KeyS", "KeyH", "KeyI", "Equal", "Equal", "Minus"});
    require(sink.events.empty() && app.view().preedit == "shi", "- and = page, typing nothing");
    click(app, "Escape");
    if (CjkComposer::supported("chinese-zhuyin")) {
        app.apply({"en-us-full", "zh-tw-zhuyin", "graphite"});
        press({"Shift+Comma", "Quote", "Shift+Period", "BracketLeft"});
        require(sink.events == Typed{"，", "、", "。", "「"}, "Zhuyin's punctuation, as McBopomofo's");
        // ← moves Zhuyin's cursor inside the reading, sending nothing.
        press({"KeyS", "KeyU", "Digit3", "KeyC", "KeyL", "Digit3", "ArrowLeft"});
        const auto view = app.view();
        require(sink.events.empty() && view.preedit == "你好" && view.preedit_cursor == 3u &&
                    view.preedit_active_begin == 0 && view.preedit_active_end == 6,
                "← moves the cursor, and the phrase over it is highlighted");
        press({"Space"});
        app.tick(2); // The row follows on the next frame.
        require(sink.events.empty() && app.view().candidates &&
                    app.view().candidates->chips.front().text == "1 你好",
                "Space starts choosing, numbered for 1–5, and sends nothing");
        press({"Escape", "Shift+Comma", "End"});
        require(sink.events.empty() && app.view().preedit == "你，好",
                "Esc stops choosing; ， goes into the reading at the cursor");
        press({"Enter"});
        require(sink.events == Typed{"你，好"}, "Enter sends the whole line");
        // ㄅㄩ can't finish: ， can't go in at the cursor, nor after the
        // whole reading (R146).
        press({"KeyS", "KeyU", "Digit3", "KeyC", "KeyL", "Digit3", "ArrowLeft", "Digit1", "KeyM"});
        const auto open = app.view().preedit;
        press({"Shift+Comma"});
        require(sink.events.empty() && app.view().preedit == open && !app.view().status.empty(),
                "punctuation that can't go in at the cursor waits, saying why");
        press({"Escape", "End", "Enter"});
        require(sink.events == Typed{"你好"}, "the reading is as it was");
        for (int i = 0; i < 29; ++i) {
            press({"Digit8", "Space"});
        }
        press({"Shift+Comma"});
        require(!sink.events.empty() && app.view().preedit.ends_with("，"),
                "punctuation flushes Zhuyin's overflow prefix immediately");
        press({"Escape"});
    }
    app.apply({"en-us-full", "zh-tw-pinyin", "graphite"});
    press({"BracketLeft", "Shift+BracketRight"});
    require(sink.events == Typed{"「", "』"}, "Traditional quotes are corner brackets");
    app.apply({"en-us-full", "en-us", "graphite"});
}
// Traditional Chinese through real key presses: digits are Zhuyin symbols, but
// choose a candidate in Cangjie and Quick.
void app_traditional() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-traditional-test-" + std::to_string(getpid()));
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
    struct Case {
        const char* method;
        const char* language;
        std::vector<std::string> keys;
        const char* preedit;
        const char* finish;
        const char* typed;
    };
    const std::vector<Case> cases{
        // McBopomofo's Space chooses; only Enter sends.
        {"chinese-zhuyin", "zh-tw-zhuyin", {"KeyS", "KeyU", "Digit3"}, "你", "Enter", "你"},
        {"chinese-cangjie",
         "zh-tw-cangjie",
         {"KeyC", "KeyN", "KeyK", "KeyQ"},
         "金弓大手",
         "Space",
         "鍵"},
        {"chinese-cangjie", "zh-tw-cangjie", {"KeyL"}, "中", "Digit1", "中"},
        {"chinese-quick", "zh-tw-quick", {"KeyL"}, "中", "Digit1", "中"},
    };
    for (const auto& test : cases) {
        if (!CjkComposer::supported(test.method)) {
            continue;
        }
        Sink sink;
        App app(options, sink);
        app.apply({"en-us-full", test.language, "graphite"});
        for (const auto& key : test.keys) {
            click(app, key);
        }
        require(app.view().preedit == test.preedit, "keys compose into the reading");
        require(sink.events.empty(), "nothing is typed while composing");
        // Settings, left with Apply in the same language, keeps the reading.
        app.show_settings();
        for (const auto& c : app.view().controls) {
            if (c.id == "apply") {
                app.down(9, c.bounds.x + 2, c.bounds.y + 2, 1);
                app.up(9, c.bounds.x + 2, c.bounds.y + 2);
            }
        }
        require(!app.view().settings && app.view().preedit == test.preedit && sink.events.empty(),
                "the reading waits through Settings");
        click(app, test.finish);
        for (int i = 0; i < 40 && sink.events.empty(); ++i) {
            app.tick(1 + i * .05);
        }
        require(sink.events == std::vector<std::string>{test.typed}, "the choice is typed");
        app.apply({"en-us-full", "en-us", "graphite"}); // Saved; the next case starts clean.
    }
}
// The method's symbols on the keys.
void traditional_legends(const Profiles& profiles) {
    const auto& layout = profiles.layouts.at("en-us-full");
    const auto key = std::find_if(layout.keys.begin(), layout.keys.end(),
                                  [](const Key& item) { return item.id == "KeyA"; });
    const auto digit = std::find_if(layout.keys.begin(), layout.keys.end(),
                                    [](const Key& item) { return item.id == "Digit1"; });
    LanguageMap cangjie(profiles.languages.at("zh-tw-cangjie"));
    require(cangjie.legend(*key, {}, false, false) == "日", "Cangjie shows radicals");
    LanguageMap quick(profiles.languages.at("zh-tw-quick"));
    require(quick.legend(*key, {}, false, false) == "日", "Quick shares the radicals");
    LanguageMap zhuyin(profiles.languages.at("zh-tw-zhuyin"));
    require(zhuyin.legend(*key, {}, false, false) == "ㄇ" &&
                zhuyin.legend(*digit, {}, false, false) == "ㄅ",
            "Zhuyin's symbols cover the digits too");
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
        const auto english = app.view();
        app.apply({"en-us-full", id, "graphite"});
        const auto controls = app.view().controls;
        require(std::none_of(controls.begin(), controls.end(),
                             [](const auto& control) {
                                 return control.id.starts_with("cjk-") || control.id == "ime-toggle";
                             }),
                "Chinese and Korean type their own script only, and the keys commit and cancel");
        // The reading has a line of its own and the candidates the toolbar's
        // row; the case fits the keys, with no room left at the sides.
        require(app.view().ime_band &&
                    panel_case_bounds(app.view()).width <= panel_case_bounds(english).width &&
                    keys_top(app.view()) >
                        ime_band(app.view()).band.y + ime_band(app.view()).band.height,
                "the keys follow the reading line, in a case that fits them");
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
            app.tick(2);
            const auto strip = app.view().candidates;
            const auto line = ime_band(app.view());
            require(strip && !strip->chips.empty() && strip->area.y == line.candidates.y &&
                        strip->area.x >= line.candidates.x &&
                        strip->area.x + strip->area.width <= line.candidates.x + line.candidates.width &&
                        strip->area.y < line.band.y &&
                        line.band.y + line.band.height < keys_top(app.view()),
                    "candidates scroll in the toolbar row, above the reading line and the keys");
            require(strip->chips.front().text == "1 你好" && strip->chips.front().selected,
                    "the first five are numbered for 1–5, the first selected for Space");
            {
                // More than the row holds: ∨ after them drops the rest down in a
                // menu over the keys under it, and the other keys stay.
                const auto tap = [&](const std::string& id) {
                    for (const auto& control : app.view().controls) {
                        if (control.id == id) {
                            const double x = control.bounds.x + control.bounds.width / 2,
                                         y = control.bounds.y + control.bounds.height / 2;
                            app.down(4, x, y, 2.5);
                            app.up(4, x, y);
                            return true;
                        }
                    }
                    return false;
                };
                // The other hand holds Settings as ∨ opens: it lets go, opening nothing.
                const auto held = [&] {
                    for (const auto& control : app.view().controls) {
                        if (control.id == "settings") {
                            return std::pair{control.bounds.x + control.bounds.width / 2,
                                             control.bounds.y + control.bounds.height / 2};
                        }
                    }
                    throw std::runtime_error("no settings control");
                }();
                app.move(5, held.first, held.second);
                app.down(5, held.first, held.second, 2.4);
                require(tap("ime-more") && app.view().candidates->list, "∨ opens the menu");
                app.up(5, held.first, held.second);
                require(!app.view().settings && app.view().candidates->list,
                        "a toolbar button held as it opens does nothing on release");
                const auto view = app.view();
                const auto& list = *view.candidates->list;
                const auto key_rect = [&](const char* id) {
                    const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                                  [&](const Key& item) { return item.id == id; });
                    return panel_rect(view, key->bounds);
                };
                const auto key_at = [&](const char* id) {
                    const auto r = key_rect(id);
                    return app.renderer.hit_key(view, r.x + r.width / 2, r.y + r.height / 2);
                };
                require(list.y < keys_top(view) + 60 && !key_at("Digit5") && !key_at("ControlLeft"),
                        "while the menu is open, no key takes a press, under it or not");
                {
                    // It opens under the reading line, which stays in view, and
                    // ends after its last line.
                    const auto reading = ime_band(view).reading;
                    double last = 0;
                    for (const auto& chip : view.candidates->chips) {
                        if (chip.listed) {
                            last = std::max(last, chip.bounds.y + chip.bounds.height);
                        }
                    }
                    require(list.y >= reading.y + reading.height && last > list.y &&
                                list.y + list.height - last < CandidateStrip::chip_height &&
                                list.height < 5 * CandidateStrip::chip_height,
                            "the menu opens under the reading line, as tall as its lines");
                }
                {
                    // A press outside the menu only closes it.
                    const auto r = key_rect("ControlLeft");
                    app.down(4, r.x + r.width / 2, r.y + r.height / 2, 2.6);
                    app.up(4, r.x + r.width / 2, r.y + r.height / 2);
                }
                require(!app.view().candidates->list &&
                            (app.view().preedit == "nihao" || app.view().preedit == "ni hao") &&
                            sink.events.empty(),
                        "a press outside the menu closes it, choosing nothing");
                require(tap("ime-more") && app.view().candidates->list, "∨ opens it again");
                require(tap("ime-more") && !app.view().candidates->list, "∧ closes it");
            }
            {
                const auto& chip = strip->chips.front().bounds;
                const double x = chip.x + chip.width / 2, y = chip.y + chip.height / 2;
                app.down(3, x, y, 3);
                app.up(3, x, y);
            }
            require(sink.events == std::vector<std::string>{"你好"} && app.view().preedit.empty(),
                    "a tapped candidate commits");
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
            // As on a PC, a syllable goes in as the next one starts; only the
            // open one waits, and Backspace takes its jamo before the target's.
            sink.events.clear();
            for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR", "KeyM", "KeyF"}) {
                click(app, action);
            }
            require(sink.events == std::vector<std::string>{"한"} && app.view().preedit == "글",
                    "a finished syllable goes in at once");
            for (int i = 0; i < 3; ++i) {
                click(app, "Backspace");
            }
            require(sink.events.size() == 1 && app.view().preedit.empty(),
                    "Backspace takes the open syllable's jamo");
            click(app, "Backspace");
            require(sink.events == std::vector<std::string>{"한", "14:1", "14:0"},
                    "then the target's Backspace deletes the sent syllable");
            {
                // Held, Backspace takes the open syllable's jamo, pauses at its
                // end, then goes on deleting in the field, as on a PC.
                sink.events.clear();
                for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR", "KeyM", "KeyF"}) {
                    click(app, action);
                }
                const auto view = app.view();
                const auto& keys = view.layout->keys;
                const auto backspace =
                    panel_rect(view, std::find_if(keys.begin(), keys.end(), [](const Key& k) {
                                         return k.action == "Backspace";
                                     })->bounds);
                const double x = backspace.x + backspace.width / 2,
                             y = backspace.y + backspace.height / 2;
                double now = 10;
                app.down(0, x, y, now);
                const auto hold = [&](double until) {
                    for (; now < until; now += .02) {
                        app.tick(now);
                    }
                };
                hold(10.56);
                require(app.view().preedit.empty() && sink.events == std::vector<std::string>{"한"},
                        "a held Backspace takes the open syllable, then pauses at its end");
                hold(11.0);
                require(sink.events == std::vector<std::string>{"한"}, "for a fresh repeat delay");
                hold(11.3);
                require(sink.events.size() > 3 && sink.events[1] == "14:1" && sink.events[2] == "14:2",
                        "then it deletes on in the field, repeating");
                app.up(0, x, y);
                app.tick(now);
                require(sink.events.back() == "14:0", "and lets go with the press");
            }
            sink.events.clear();
            click(app, "KeyE");
            click(app, "KeyK");
            click(app, "Enter");
            require(sink.events == std::vector<std::string>{"다", "28:1", "28:0"} &&
                        app.view().preedit.empty(),
                    "Enter commits the open syllable and goes on, in one press");
            sink.events.clear();
            for (const auto* action : {"KeyW", "KeyJ", "KeyS", "KeyG", "KeyH", "KeyK"}) {
                click(app, action);
            }
            require(sink.events == std::vector<std::string>{"전"} && app.view().preedit == "화",
                    "전 goes in as 화 starts");
            click(app, "Escape");
            require(sink.events == std::vector<std::string>{"전", "1:1", "1:0"} &&
                        app.view().preedit.empty(),
                    "Esc reaches the target at once, dropping only the open syllable");
            sink.events.clear();
            for (int i = 0; i < 40; ++i) {
                click(app, "KeyZ");
            }
            require(sink.events.size() == 39 && app.view().preedit == "ㅋ",
                    "a run of 40 ㅋ goes in as it is typed, with no limit");
            click(app, "Space");
            // A syllable the target refuses waits ahead of the open one and
            // goes with the next commit, in order.
            sink.events.clear();
            sink.accept = false;
            for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR"}) {
                click(app, action);
            }
            require(sink.events.empty() && app.view().preedit == "한ㄱ",
                    "a refused syllable stays in the reading line");
            sink.accept = true;
            click(app, "Space");
            require(sink.events == std::vector<std::string>{"한ㄱ", " "} && app.view().preedit.empty(),
                    "the refused syllable goes in with the open one");
            // 한/영, in right Alt's place, types the open syllable and switches
            // to the layout's own letters, without latching Alt; again, back.
            sink.events.clear();
            click(app, "KeyR");
            click(app, "AltRight");
            require(sink.events == std::vector<std::string>{"ㄱ"} && app.view().preedit.empty(),
                    "한/영 types the open syllable first");
            click(app, "KeyG");
            require(sink.events == std::vector<std::string>{"ㄱ", "g"} &&
                        !app.view().key_labels.contains("KeyG"),
                    "then the keys type Latin letters, which they show");
            click(app, "AltRight");
            click(app, "KeyG");
            require(app.view().preedit == "ㅎ" && app.view().key_labels.at("KeyG") == "ㅎ",
                    "and 한/영 again goes back to Hangul");
            click(app, "Space");
            // A latched Shift waits through 한/영 for the letter after it.
            sink.events.clear();
            click(app, "ShiftLeft");
            click(app, "AltRight");
            click(app, "KeyA");
            require(sink.events == std::vector<std::string>{"A"}, "한/영 leaves a latched Shift");
            // Reload returns to Hangul: the reloaded profile may have no 한/영.
            app.reload();
            click(app, "KeyG");
            require(app.view().preedit == "ㅎ", "Reload returns Korean to Hangul");
            click(app, "Space");
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
        // Chinese types its own comma.
        require(sink.events.size() == 2 &&
                    sink.events.back() == (std::string(id).starts_with("zh-") ? "，" : ",") &&
                    app.view().preedit.empty(),
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
// Steam answers a text commit on a later frame: a refusal leaves Enter's
// retry pending while the next syllable is already open.
struct AsyncSink : Sink {
    bool pending{}, refuse{};
    bool commit_text(const std::string& value) override {
        if (!pending) {
            pending = true; // submitted; the answer comes with the next call
            return false;
        }
        pending = false;
        return !refuse && Sink::commit_text(value);
    }
    bool text_pending() const override { return pending; }
};
void korean_delivery_failures() {
    const auto path =
        fs::temp_directory_path() / ("framekeyboard-korean-retry-" + std::to_string(getpid()));
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
    double now = 2;
    const auto tap = [&](App& app, const std::string& action) {
        const auto view = app.view();
        for (const auto& key : view.layout->keys) {
            if (key.action == action) {
                const auto r = panel_rect(view, key.bounds);
                const bool accepted = app.down(0, r.x + r.width / 2, r.y + r.height / 2, now);
                app.up(0, r.x + r.width / 2, r.y + r.height / 2);
                for (int i = 0; i < 3; ++i) {
                    app.tick(now += .05);
                }
                return accepted;
            }
        }
        throw std::runtime_error("no key " + action);
    };
    {
        // 한 goes, the target refuses it, and ㄱ is open: Enter retries 한
        // and leaves ㄱ open; Esc then drops ㄱ, reaching the target.
        AsyncSink sink;
        App app(options, sink);
        app.apply({"en-us-full", "ko-kr", "graphite"});
        sink.refuse = true;
        for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR"}) {
            tap(app, action);
        }
        require(sink.events.empty() && app.view().preedit == "ㄱ",
                "한 was sent and refused, ㄱ is open");
        sink.refuse = false;
        require(tap(app, "Enter") && sink.events == std::vector<std::string>{"한"} &&
                    app.view().preedit == "ㄱ",
                "Enter retries 한 and leaves the open syllable");
        require(tap(app, "Escape") && sink.events == std::vector<std::string>{"한", "1:1", "1:0"} &&
                    app.view().preedit.empty(),
                "Esc drops the open syllable and reaches the target");
    }
    {
        // While the retry waits, Esc can't go, but still drops ㄱ.
        AsyncSink sink;
        App app(options, sink);
        app.apply({"en-us-full", "ko-kr", "graphite"});
        sink.refuse = true;
        for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR"}) {
            tap(app, action);
        }
        sink.refuse = false;
        require(!tap(app, "Escape") && app.view().preedit.empty(),
                "Esc refused by a pending retry still drops the open syllable");
        require(tap(app, "Enter") && sink.events == std::vector<std::string>{"한"},
                "and Enter then retries");
    }
    {
        // A syllable refused at once waits ahead of the open one; Esc drops
        // only the open one and the refused one goes in before the Esc.
        Sink sink;
        App app(options, sink);
        app.apply({"en-us-full", "ko-kr", "graphite"});
        sink.accept = false;
        for (const auto* action : {"KeyG", "KeyK", "KeyS", "KeyR"}) {
            tap(app, action);
        }
        require(sink.events.empty() && app.view().preedit == "한ㄱ", "한 waits, refused");
        sink.accept = true;
        require(tap(app, "Escape") && sink.events == std::vector<std::string>{"한", "1:1", "1:0"} &&
                    app.view().preedit.empty(),
                "Esc keeps the refused 한, which goes in first, and drops ㄱ");
    }
}
int main(int argc, char** argv) {
    try {
        if (const char* directory = std::getenv("FRAMEKEYBOARD_TEST_TABLE_DIRECTORY")) {
            set_table_directory(directory);
        }
        table_engine();
        const auto profiles = load_profiles({}, {});
        traditional_legends(profiles);
        const std::string group = argc > 1 ? argv[1] : "";
        if (group == "--korean" || group == "--chinese") {
            const auto method = group == "--korean" ? "korean-2set" : "chinese-pinyin-simplified";
            if (!CjkComposer::supported(method)) {
                std::cout << method << ": engine unavailable; integration not run\n";
                return std::getenv("FRAMEKEYBOARD_TEST_CJK_REQUIRED") ? 1 : 77;
            }
            engines(group);
            concurrent_engines(method, group == "--korean" ? "gksrmf" : "nihao");
            app_composition(profiles, group);
            if (group == "--korean") {
                korean_delivery_failures();
            }
            if (group == "--chinese") {
                app_traditional();
                chinese_keys();
                chinese_terminal_learning();
                chinese_keys(true);
                chinese_pending_front();
                chinese_engines();
                zhuyin_edited_picks();
                pinyin_reranking();
                zhuyin_reranking();
                chinese_report();
            }
            std::cout << method << ": engine and delivery ordering passed\n";
            return 0;
        }
        if (group == "--without-engines") {
            require(!CjkComposer::supported("korean-2set") &&
                        !CjkComposer::supported("chinese-pinyin-simplified"),
                    "isolated test must have no usable optional engines");
            // Removed engines must stay unavailable even if stale adapter
            // files are present in this executable's private module directory.
            for (const auto* retired : {"pyzy", "pinyin", "chewing"}) {
                setenv("FRAMEKEYBOARD_CHINESE_ENGINE", retired, 1);
                require(!CjkComposer::supported("chinese-pinyin-simplified") &&
                            !CjkComposer::supported("chinese-zhuyin"),
                        "retired engine overrides never load stale modules");
            }
            unsetenv("FRAMEKEYBOARD_CHINESE_ENGINE");
            unicode_input();
            app_composition(profiles);
            std::cout
                << "English/Unicode works without optional engines; failed Apply preserves selection\n";
            return 0;
        }
        require(group.empty() || group == "--core", "unknown test group");
        input_gate_recovery();
        key_flicks();
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
        // Japanese composes on a US keymap, which has nothing on the JIS ¥ and
        // ろ keys; they type and show what a JIS keyboard's do in Latin mode.
        for (const auto* id : {"ja-kana", "ja-romaji"}) {
            LanguageMap japanese(profiles.languages.at(id));
            const auto& jis = profiles.layouts.at("ja-jis-full");
            const auto yen = key(jis, "IntlYen"), ro = key(jis, "IntlRo");
            require(japanese.symbol(yen, {}, false, false) == XKB_KEY_yen &&
                        japanese.symbol(yen, {KEY_LEFTSHIFT}, false, false) == XKB_KEY_bar &&
                        japanese.symbol(ro, {}, false, false) == XKB_KEY_backslash &&
                        japanese.symbol(ro, {KEY_RIGHTSHIFT}, false, false) == XKB_KEY_underscore,
                    "the JIS ¥ and ろ keys type ¥ | and \\ _");
            require(japanese.legend(yen, {}, false, false) == "¥" &&
                        japanese.legend(ro, {}, false, false) == "\\" &&
                        japanese.legend(ro, {KEY_LEFTSHIFT}, false, false) == "_",
                    "and show them");
        }
        LanguageMap english(profiles.languages.at("en-us"));
        require(english.symbol(key(profiles.layouts.at("ja-jis-full"), "IntlRo"), {}, false, false) ==
                    XKB_KEY_NoSymbol,
                "other languages keep their keymap's keys");
        unicode_input();
        national_keymaps(profiles);
        locks_and_accents(profiles);
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
