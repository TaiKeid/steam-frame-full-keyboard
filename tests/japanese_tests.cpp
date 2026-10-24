#include "framekeyboard/app.hpp"
#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Sink : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    std::vector<std::string> delivery;
    bool available = true, accept = true, blocked = false;
    void send(int code, int value) override {
        keys.emplace_back(code, value);
        delivery.push_back("key");
    }
    bool text_available() override { return available; }
    bool output_blocked() const override { return blocked; }
    bool commit_text(const std::string& value) override {
        if (!accept) {
            return false;
        }
        delivery.push_back("commit");
        text.push_back(value);
        return true;
    }
};
void click_key(App& app, const std::string& id, unsigned pointer = 0, bool release = true,
               bool expect_accepted = true) {
    const auto view = app.view();
    for (int y = 96; y < panel_height; y += 2) {
        for (int x = 0; x < panel_width; x += 2) {
            auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->id == id) {
                if (app.down(pointer, x, y, 1) != expect_accepted) {
                    throw std::runtime_error("key acceptance: " + id);
                }
                if (release) {
                    app.up(pointer, x, y);
                }
                return;
            }
        }
    }
    throw std::runtime_error("Missing test key: " + id);
}
void control(App& app, const std::string& id) {
    for (const auto& c : app.view().controls) {
        if (c.id == id) {
            app.down(9, c.bounds.x + 2, c.bounds.y + 2, 1);
            app.up(9, c.bounds.x + 2, c.bounds.y + 2);
            return;
        }
    }
    throw std::runtime_error("Missing test control: " + id);
}
bool has_control(App& app, const std::string& id) {
    const auto controls = app.view().controls;
    return std::any_of(controls.begin(), controls.end(), [&](const Control& c) { return c.id == id; });
}
// Opens a Settings dropdown and scrolls the choice into view.
Control show_choice(App& app, const std::string& field, const std::string& choice) {
    control(app, field);
    const auto id = "choose:" + field + ":" + choice;
    for (int i = 0; i < 50 && !has_control(app, id); ++i) {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, -100);
        for (int j = 0; j < i && !has_control(app, id); ++j) {
            app.scroll(0, 0, 1);
        }
    }
    for (const auto& c : app.view().controls) {
        if (c.id == id) {
            return c;
        }
    }
    throw std::runtime_error("Missing test choice: " + id);
}
void choose(App& app, const std::string& field, const std::string& choice) {
    control(app, show_choice(app, field, choice).id);
}
// Centre of a key's hit area in panel coordinates.
std::pair<double, double> key_centre(App& app, const std::string& id) {
    const auto view = app.view();
    double left = panel_width, top = panel_height, right = 0, bottom = 0;
    for (int y = 96; y < panel_height; y += 2) {
        for (int x = 0; x < panel_width; x += 2) {
            if (const auto* key = app.renderer.hit_key(view, x, y); key && key->id == id) {
                left = std::min<double>(left, x);
                right = std::max<double>(right, x);
                top = std::min<double>(top, y);
                bottom = std::max<double>(bottom, y);
            }
        }
    }
    require(right > 0, "flick key is visible");
    return {(left + right) / 2, (top + bottom) / 2};
}
// Presses a flick key at time `now`, drags by (dx, dy) panel pixels and releases.
void flick(App& app, const std::string& slot, double now, double dx = 0, double dy = 0) {
    const auto [x, y] = key_centre(app, "Flick-" + slot);
    require(app.down(0, x, y, now), "flick key accepts a press");
    if (dx != 0 || dy != 0) {
        app.move(0, x + dx / 2, y + dy / 2);
        app.move(0, x + dx, y + dy);
    }
    app.up(0, x + dx, y + dy);
}
void flick_input() {
    const auto profiles = load_profiles({}, {});
    auto fits = [&](const char* layout, const char* language) {
        try {
            validate_selection(profiles, {layout, language, "graphite"});
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    require(fits("ja-flick-12", "ja-flick") && !fits("en-us-full", "ja-flick") &&
                !fits("ja-flick-12", "en-us") && !fits("ja-flick-12", "ja-romaji"),
            "flick layout and flick language only fit each other");
    char directory[] = "/tmp/framekeyboard-flick-XXXXXX";
    require(mkdtemp(directory), "temporary flick profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    Sink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    auto preedit = [&] { return app.view().preedit; };
    auto clear = [&] {
        click_key(app, "Escape");
        require(preedit().empty(), "Escape clears the flick reading");
    };

    flick(app, "a", 1);
    require(preedit() == "あ", "a tap types the key's first kana");
    flick(app, "a", 1.5);
    flick(app, "a", 2);
    require(preedit() == "う", "quick repeated taps cycle the toggle kana");
    flick(app, "a", 4);
    require(preedit() == "うあ", "a tap after the toggle time adds a kana");
    click_key(app, "ArrowRight");
    flick(app, "a", 4.2);
    require(preedit() == "うああ", "→ ends a toggle so the same key types again");
    clear();
    for (int i = 0; i < 11; ++i) {
        flick(app, "a", 10 + i * .1);
    }
    require(preedit() == "あ", "the toggle cycle wraps through small kana");
    clear();

    flick(app, "ka", 20, -60, 0);
    flick(app, "ka", 21, 0, -60);
    flick(app, "ka", 22, 60, 0);
    flick(app, "ka", 23, 0, 60);
    require(preedit() == "きくけこ", "flicks type the left, up, right and down kana");
    clear();
    {
        const auto [x, y] = key_centre(app, "Flick-ka");
        app.down(0, x, y, 30);
        app.move(0, x, y - 60);
        const auto preview = app.view().flick_preview;
        require(preview && preview->key == "Flick-ka" && preview->selected == 2 &&
                    preview->text == std::array<std::string, 5>{"か", "き", "く", "け", "こ"},
                "a pressed flick key previews its kana and the chosen direction");
        if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
            app.paint(30.5);
            app.renderer.write_png(fs::path(render) / "framekeyboard-ja-flick-preview.png");
        }
        app.move(0, x, y);
        require(app.view().flick_preview->selected == 0, "moving back selects the tap kana");
        app.cancel_pointer(0);
        require(preedit().empty() && !app.view().flick_preview, "a cancelled flick types nothing");
    }

    {
        // The other hand's press types the held flick first, in press order.
        const auto [kx, ky] = key_centre(app, "Flick-ka");
        const auto [sx, sy] = key_centre(app, "Flick-sa");
        app.down(0, kx, ky, 35);
        app.move(0, kx, ky - 60);
        require(app.down(1, sx, sy, 35.1) && preedit() == "く",
                "a held flick types before the next press");
        app.up(0, kx, ky - 60);
        app.up(1, sx, sy);
        require(preedit() == "くさ", "neither hand's kana is dropped");
        clear();
    }
    flick(app, "ta", 40, 0, -60);
    flick(app, "mark", 41);
    require(preedit() == "っ", "゛゜小 makes the last kana small");
    flick(app, "mark", 41.2);
    require(preedit() == "づ", "゛゜小 then voices it");
    flick(app, "mark", 41.4);
    require(preedit() == "つ", "゛゜小 cycles back to the plain kana");
    flick(app, "ha", 42);
    flick(app, "mark", 43, -60, 0);
    require(preedit() == "つば", "a ゛ flick voices the last kana");
    flick(app, "mark", 44, 60, 0);
    require(preedit() == "つぱ", "a ゜ flick makes it semi-voiced");
    flick(app, "mark", 45, 0, 60);
    require(preedit() == "つぱ", "an unused direction types nothing");
    flick(app, "punct", 46, 60, 0);
    require(preedit() == "つぱ！", "punctuation flicks join the reading");
    click_key(app, "Enter");
    require(sink.text.back() == "つぱ！" && preedit().empty(), "Enter commits the flick reading");

    control(app, "ime-toggle");
    require(!app.view().composing, "Latin mode stops composition");
    flick(app, "na", 50);
    require(app.view().composing && preedit() == "な", "a flick key returns to kana composition");
    clear();

    app.show_settings();
    {
        // The case is also the VR laser mask: every Settings control must lie inside it.
        const auto view = app.view();
        const auto body = panel_case_bounds(view);
        for (const auto& c : view.controls) {
            require(c.style == ControlStyle::MoveHandle ||
                        (c.bounds.x >= body.x && c.bounds.x + c.bounds.width <= body.x + body.width),
                    "Settings controls stay inside the flick layout's case");
        }
    }
    require(!show_choice(app, "language", "en-us").enabled,
            "the flick layout dims languages without flick kana");
    if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
        app.paint(60);
        app.renderer.write_png(fs::path(render) / "framekeyboard-ja-flick-languages.png");
    }
    app.back();
    choose(app, "layout", "en-us-full");
    choose(app, "layout", "ja-flick-12");
    control(app, "apply");
    require(app.selection().layout == "ja-flick-12" && app.selection().language == "ja-flick",
            "choosing the flick layout selects flick input");
    app.show_settings();
    choose(app, "layout", "en-us-full");
    control(app, "apply");
    require(app.selection().language == "ja-romaji", "leaving flick keeps Japanese, as Romaji");
}
void romaji_legends() {
    char directory[] = "/tmp/framekeyboard-romaji-legends-XXXXXX";
    require(mkdtemp(directory), "temporary legend profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    fs::create_directories(fs::path(directory) / "languages");
    for (bool overridden : {false, true}) {
        std::ofstream(fs::path(directory) / "languages/ja-test.json")
            << R"({"schema_version":1,"id":"ja-test","name":"Test Romaji","locale":"ja-JP",
                   "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
                   "font_families":["sans-serif"],"input_method":"japanese-romaji",
                   "legends":{"source":"keymap","overrides":)"
            << (overridden ? R"({"KeyK":"s"})" : "{}") << "}}";
        for (bool caps : {false, true}) {
            for (bool shift : {false, true}) {
                Options options;
                options.config_dir = directory;
                options.mode = Mode::Vr;
                options.input = InputBackend::Ei;
                options.start_enabled = true;
                Sink sink;
                App app(options, sink);
                app.apply({"en-us-full", "ja-test", "graphite"});
                if (caps) {
                    click_key(app, "CapsLock");
                }
                if (shift) {
                    click_key(app, "ShiftLeft");
                }
                click_key(app, "KeyK");
                if (shift) {
                    click_key(app, "ShiftLeft");
                }
                click_key(app, "KeyI");
                require(app.view().preedit == "き", "Romaji ignores cosmetic legends with Shift/Caps");
                click_key(app, "Enter");
                require(sink.text == std::vector<std::string>{"き"}, "Romaji commits mapped symbols");
                click_key(app, "Space");
                require(app.view().preedit.empty() && sink.text == std::vector<std::string>{"き", " "},
                        "idle Romaji Space types directly without starting composition");
            }
        }
    }
}
int main() {
    try {
        romaji_legends();
        flick_input();
        {
            // Marks also apply to katakana, and leave kana without variants alone.
            JapaneseComposer marks;
            marks.kana("つ");
            marks.script(true);
            require(marks.mark("小") && marks.preedit() == "ッ", "小 makes katakana small");
            require(marks.mark("゛゜小") && marks.preedit() == "ヅ", "゛゜小 cycles katakana too");
            marks.kana("ん");
            require(!marks.mark("゛゜小") && marks.preedit() == "ヅん", "ん has no variant");
        }
        JapaneseComposer composer;
        for (const auto& [roman, expected] :
             std::vector<std::pair<std::string, std::string>>{{"nihon", "にほん"},
                                                              {"konnichiha", "こんにちは"},
                                                              {"gakkou", "がっこう"},
                                                              {"shin'you", "しんよう"},
                                                              {"nn", "ん"},
                                                              {"kanna", "かんな"},
                                                              {"matcha", "まっちゃ"},
                                                              {"kyou", "きょう"},
                                                              {"shashin", "しゃしん"},
                                                              {"xtsu", "っ"},
                                                              {"ca", "か"},
                                                              {"ci", "し"},
                                                              {"cu", "く"},
                                                              {"ce", "せ"},
                                                              {"co", "こ"},
                                                              {"fya", "ふゃ"},
                                                              {"fyu", "ふゅ"},
                                                              {"fyo", "ふょ"},
                                                              {"tha", "てゃ"},
                                                              {"thi", "てぃ"},
                                                              {"thu", "てゅ"},
                                                              {"the", "てぇ"},
                                                              {"tho", "てょ"},
                                                              {"dha", "でゃ"},
                                                              {"dhi", "でぃ"},
                                                              {"dhu", "でゅ"},
                                                              {"dhe", "でぇ"},
                                                              {"dho", "でょ"},
                                                              {"wu", "う"},
                                                              {"cca", "っか"},
                                                              {"fyu-jon", "ふゅーじょん"},
                                                              {"CA", "か"}}) {
            composer.cancel();
            for (char c : roman) {
                composer.roman(c);
            }
            require(composer.commit_text() == expected, "romaji conversion");
        }
        composer.cancel();
        composer.kana("か");
        composer.kana("\u3099");
        require(composer.preedit() == "が", "dakuten composes");
        composer.script(true);
        require(composer.preedit() == "ガ", "katakana");
        composer.script(false);
        require(composer.preedit() == "が", "hiragana");
        composer.backspace();
        require(composer.empty(), "UTF-8 backspace");
        composer.roman('k');
        composer.backspace();
        require(composer.empty(), "pending romaji backspace");
        // Host developers can point this test at an extracted Anthy package;
        // production code uses only the system library/dictionary.
        void* anthy = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
        if (const auto* dictionary = std::getenv("FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY")) {
            require(anthy, "test Anthy library missing");
            auto override = reinterpret_cast<void (*)(const char*, const char*)>(
                dlsym(anthy, "anthy_conf_override"));
            require(override, "Anthy config API");
            override("DIC_FILE", dictionary);
        }
        if (anthy) {
            composer.cancel();
            for (char c : std::string("nihon")) {
                composer.roman(c);
            }
            composer.convert();
            const auto candidates = composer.candidates();
            const auto found = std::find(candidates.begin(), candidates.end(), "日本");
            require(found != candidates.end(), "Anthy offers 日本");
            composer.choose(static_cast<int>(found - candidates.begin()));
            require(composer.commit_text() == "日本", "candidate commit");
            composer.cycle(1);
            composer.cycle(-1);
            require(composer.commit_text() == "日本", "candidate navigation");
            composer.convert();
            require(!composer.mark("゛゜小") && composer.converting(),
                    "a mark that changes nothing keeps the conversion");
            composer.unconvert();
            require(composer.preedit() == "にほん", "unconvert");
            dlclose(anthy);
        } else {
            require(!std::getenv("FRAMEKEYBOARD_TEST_ANTHY_REQUIRED"), "Anthy required for this run");
            std::cout << "Anthy integration skipped: library absent.\n";
        }
        char directory[] = "/tmp/framekeyboard-japanese-XXXXXX";
        require(mkdtemp(directory), "temporary profile");
        struct Cleanup {
            fs::path path;
            ~Cleanup() { fs::remove_all(path); }
        } cleanup{directory};
        Options options;
        options.config_dir = directory;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.target_language = "en-us";
        Sink sink;
        App app(options, sink);
        app.apply({"en-us-full", "ja-romaji", "graphite"});
        sink.blocked = true;
        click_key(app, "KeyK");
        click_key(app, "KeyI");
        require(app.view().preedit == "き", "output barrier permits local Romaji preedit");
        click_key(app, "Backspace");
        require(app.view().preedit.empty() && sink.keys.empty() && sink.text.empty(),
                "output barrier permits preedit editing without transport output");
        sink.blocked = false;
        for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyO", "KeyN"}) {
            click_key(app, id);
        }
        require(app.view().preedit == "にほn", "local preedit");
        require(sink.keys.empty() && sink.text.empty(), "preedit sends no raw keys or text");
        if (anthy) {
            control(app, "ime-convert");
            if (const auto* directory = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
                app.paint(2);
                app.renderer.write_png(fs::path(directory) / "framekeyboard-ja-candidates.png");
            }
            click_key(app, "Escape");
        }
        app.cancel(false);
        require(!app.view().preedit.empty(), "laser leave keeps preedit");
        click_key(app, "Enter");
        require(sink.text == std::vector<std::string>{"にほん"}, "Enter commits without submit");
        require(sink.keys.empty(), "commit does not submit form");
        click_key(app, "Enter");
        require(sink.keys == std::vector<std::pair<int, int>>{{28, 1}, {28, 0}},
                "empty Enter submits normally");
        click_key(app, "KeyA");
        sink.accept = false;
        control(app, "ime-commit");
        require(app.view().preedit == "あ", "failed delivery retains text");
        sink.accept = true;
        app.cancel();
        require(app.view().preedit.empty(), "target cancellation discards preedit");
        click_key(app, "KeyA");
        require(!app.view().preedit.empty(), "preedit exists before dashboard closes");
        const auto sent_before_hide = sink.text.size();
        app.set_interaction_active(false);
        require(app.view().preedit.empty(), "dashboard close discards Japanese preedit");
        control(app, "ime-commit");
        app.set_interaction_active(true);
        control(app, "ime-commit");
        require(sink.text.size() == sent_before_hide, "no hidden or stale Japanese commit");
        for (const auto* forwarding_key : {"Tab", "Delete", "Home", "End", "Paste"}) {
            click_key(app, "KeyA");
            sink.delivery.clear();
            const auto text_count = sink.text.size();
            click_key(app, forwarding_key);
            require(sink.text.size() == text_count + 1 && sink.text.back() == "あ",
                    "navigation and shortcuts preserve unfinished Japanese text");
            require(sink.delivery.size() >= 3 && sink.delivery.front() == "commit" &&
                        sink.delivery[1] == "key" && app.view().preedit.empty(),
                    "Japanese commit precedes every forwarded key-down");
        }
        click_key(app, "KeyA");
        click_key(app, "ControlLeft", 1, false);
        sink.delivery.clear();
        click_key(app, "KeyA", 2);
        app.up(1, 0, 0);
        require(sink.delivery.front() == "commit" && sink.delivery[1] == "key",
                "commit precedes the modifier as well as the shortcut key");
        for (const auto* forwarding_key : {"Tab", "Delete", "Paste"}) {
            click_key(app, "KeyA");
            sink.accept = false;
            sink.delivery.clear();
            click_key(app, forwarding_key, 0, true, false);
            require(app.view().preedit == "あ" && sink.delivery.empty(),
                    "failed commit retains preedit and blocks focus-changing key");
            sink.accept = true;
            click_key(app, forwarding_key);
            require(app.view().preedit.empty() && sink.delivery.front() == "commit",
                    "retry commits retained text before forwarding");
        }
        click_key(app, "KeyA");
        sink.delivery.clear();
        click_key(app, "Escape");
        require(app.view().preedit.empty() && sink.delivery.empty(),
                "Escape deliberately cancels without committing");
        app.show_settings();
        choose(app, "layout", "ja-jis-full");
        choose(app, "language", "ja-kana");
        control(app, "apply");
        require(app.selection().layout == "ja-jis-full" && app.selection().language == "ja-kana",
                "Kana preset applies with its required layout");
        click_key(app, "KeyT");
        click_key(app, "BracketLeft");
        require(app.view().preedit == "が", "direct kana voiced sound");
        app.cancel();
        click_key(app, "ShiftLeft");
        require(app.view().key_labels.at("KeyE") == "ぃ", "shift kana legend");
        click_key(app, "KeyE");
        require(app.view().preedit == "ぃ", "shift kana input");
        app.cancel();
        sink.keys.clear();
        click_key(app, "ControlLeft", 1, false);
        click_key(app, "KeyA", 2);
        app.up(1, 0, 0);
        require(sink.keys == std::vector<std::pair<int, int>>{{29, 1}, {30, 1}, {30, 0}, {29, 0}},
                "Japanese Ctrl+A remains a physical shortcut");
        app.apply({"ja-jis-full", "ja-jis", "graphite"});
        sink.keys.clear();
        click_key(app, "KeyA");
        require(sink.keys.empty(), "external JIS requires declared JP target");
        options.target_language = "ja-jis";
        App external(options, sink);
        click_key(external, "Convert");
        require(sink.keys == std::vector<std::pair<int, int>>{{92, 1}, {92, 0}},
                "JIS conversion key uses evdev");
        options.start_enabled = false;
        options.target_language = "en-us";
        App disabled(options, sink);
        disabled.apply({"en-us-full", "ja-romaji", "graphite"});
        const auto before = sink.text.size();
        click_key(disabled, "KeyA");
        click_key(disabled, "Enter");
        require(sink.text.size() == before, "IME respects opt-in");
        options.start_enabled = true;
        sink.available = false;
        App unavailable(options, sink);
        click_key(unavailable, "KeyA");
        click_key(unavailable, "Enter");
        require(sink.text.size() == before, "missing text transport cannot emit Japanese");
        unavailable.apply({"en-us-full", "en-us", "graphite"});
        sink.keys.clear();
        click_key(unavailable, "KeyA");
        require(sink.keys.empty(),
                "Unicode English never falls back to a possibly mismatched physical keymap");
        if (anthy) {
            sink.available = true;
            sink.text.clear();
            App converted(options, sink);
            converted.apply({"en-us-full", "ja-romaji", "graphite"});
            for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyO", "KeyN"}) {
                click_key(converted, id);
            }
            control(converted, "ime-convert");
            std::string choice;
            for (const auto& button : converted.view().controls) {
                if (button.id.starts_with("ime-candidate-") && button.label.ends_with(" 日本")) {
                    choice = button.id;
                }
            }
            require(!choice.empty(), "日本 candidate is clickable");
            control(converted, choice);
            control(converted, "ime-commit");
            require(sink.text == std::vector<std::string>{"日本"},
                    "candidate button commits through text sink");
        }
        std::cout
            << "Japanese composition, candidates, kana, shortcuts, gates and cancellation passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
