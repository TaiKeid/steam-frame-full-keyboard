#include "framekeyboard/app.hpp"
#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <glib.h>
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
    if (right <= 0) {
        throw std::runtime_error("key not visible: " + id);
    }
    return {(left + right) / 2, (top + bottom) / 2};
}
// Clicks the candidate chip showing `text`, if one is in view.
bool click_candidate(App& app, const std::string& text, double now) {
    const auto view = app.view();
    if (!view.candidates) {
        return false;
    }
    for (const auto& chip : view.candidates->chips) {
        if (chip.text == text) {
            const double x = chip.bounds.x + chip.bounds.width / 2,
                         y = chip.bounds.y + chip.bounds.height / 2;
            app.down(7, x, y, now);
            app.up(7, x, y);
            app.tick(now + .01);
            return true;
        }
    }
    return false;
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
        // The kana page has no Esc: Backspace leaves conversion, then empties the reading.
        for (int i = 0; i < 40 && !preedit().empty(); ++i) {
            click_key(app, "Backspace");
        }
        require(preedit().empty(), "Backspace clears the flick reading");
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
    // Moving, resizing, pinning or recentring the panel keeps the reading.
    app.set_dragging(true);
    app.set_dragging(false);
    app.set_dragging(true, true);
    app.set_dragging(false);
    require(preedit() == "きくけこ", "moving or resizing the panel keeps the reading");
    control(app, "recenter");
    require(app.take_recenter() && preedit() == "きくけこ", "recentring keeps the reading");
    control(app, "pin");
    control(app, "pin");
    require(preedit() == "きくけこ", "pinning keeps the reading");
    {
        // A held Backspace repeats on the reading and stops once it is empty,
        // never reaching the target.
        const auto sent = sink.keys.size();
        const auto [x, y] = key_centre(app, "Backspace");
        app.down(0, x, y, 40);
        app.tick(40.3);
        require(preedit() == "きくけ", "one deletion on press");
        app.tick(40.6);
        require(preedit() == "きく" && app.holds_repeat(0), "held Backspace repeats on the reading");
        for (double now = 40.65; now < 41.5; now += .05) {
            app.tick(now);
        }
        require(preedit().empty() && !app.holds_repeat(0) && sink.keys.size() == sent,
                "the repeat stops at an empty reading and sends nothing");
        app.up(0, x, y);
    }
    flick(app, "ka", 50, -60, 0);
    flick(app, "ka", 51, 0, -60);
    {
        const auto [x, y] = key_centre(app, "Backspace");
        app.down(0, x, y, 52);
        app.up(0, x, y);
        app.tick(53);
        require(preedit() == "き", "a released Backspace does not repeat");
    }
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
        // ↶ has a tap alone, so it shows no cross of choices.
        const auto [ux, uy] = key_centre(app, "Flick-undo");
        app.down(0, ux, uy, 31);
        require(!app.view().flick_preview, "a key without flicks shows no preview");
        app.cancel_pointer(0);
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

    // ← and → move the cursor in the reading; → first ends a toggle.
    flick(app, "a", 47);
    flick(app, "ka", 47.2);
    flick(app, "sa", 47.4);
    click_key(app, "ArrowRight");
    click_key(app, "ArrowLeft");
    require(app.view().preedit_cursor == std::string("あか").size(),
            "← moves the cursor in the reading");
    flick(app, "na", 47.6);
    require(preedit() == "あかなさ", "a kana goes in at the cursor");
    click_key(app, "ArrowRight");
    require(app.view().preedit_cursor == std::string("あかな").size(), "→ ends the toggle first");
    click_key(app, "ArrowRight");
    require(app.view().preedit_cursor == preedit().size(), "then moves the cursor");
    clear();

    // 「あA1」 replaces the toolbar's A / あ. English shows the layout's QWERTY
    // page in the same case; digits turn the grid into a number pad.
    require(!has_control(app, "ime-toggle"), "the mode key replaces A / あ");
    const auto kana_case = panel_case_bounds(app.view());
    flick(app, "mode", 49);
    require(!app.view().composing && app.view().flick_latin && app.view().ime_band &&
                app.view().layout->id == "ja-flick-qwerty" && panel_case_bounds(app.view()) == kana_case,
            "a tap on 「あA1」 shows the English page in the same case, keeping the band");
    const auto typed = sink.text.size();
    click_key(app, "KeyQ");
    click_key(app, "ShiftLeft");
    click_key(app, "KeyW");
    require(sink.text.size() == typed + 2 && sink.text[typed] == "q" && sink.text.back() == "W" &&
                preedit().empty(),
            "English letters type at once, and Shift makes a capital");
    {
        // A Shift locked on the English page stays behind when the page goes.
        const auto [x, y] = key_centre(app, "ShiftLeft");
        app.down(0, x, y, 53);
        app.tick(53.7);
        app.up(0, x, y);
    }
    flick(app, "mode", 54.2);
    require(app.view().flick_digits && !app.view().flick_latin && app.view().layout->id == "ja-flick-12",
            "the next tap switches to digits, back on the grid");
    sink.keys.clear();
    click_key(app, "ArrowLeft");
    require(sink.keys == std::vector<std::pair<int, int>>{{105, 1}, {105, 0}},
            "the kana and digit pages have no Shift, so none stays locked");
    require(app.view().key_labels.at("Flick-ka") == "2" && app.view().key_labels.at("Flick-wa") == "0",
            "the grid shows a number pad");
    flick(app, "ka", 55);
    flick(app, "ka", 55.2);
    flick(app, "ka", 55.4, -60, 0);
    require(sink.text.size() == typed + 5 && sink.text[typed + 3] == "2" && sink.text.back() == "¥" &&
                preedit().empty(),
            "digits type at once, without cycling, and flicks type their symbols");
    flick(app, "a", 55.6, 0, 60);
    flick(app, "sa", 55.8, 0, 60);
    require(sink.text[sink.text.size() - 2] == "@" && sink.text.back() == "_",
            "the down flicks give the symbols the English page has no keys for");
    flick(app, "mode", 56, 0, -60);
    require(app.view().flick_latin, "flicking 「あA1」 up picks English");
    flick(app, "mode", 56.2, 60, 0);
    require(app.view().flick_digits, "flicking it right picks digits");
    flick(app, "mode", 56.4, -60, 0);
    require(app.view().composing, "flicking it left returns to kana");
    flick(app, "na", 57);
    require(preedit() == "な", "kana mode composes again");
    flick(app, "mode", 57.2, 60, 0);
    require(sink.text.back() == "な" && app.view().flick_digits, "switching modes commits the reading");
    sink.keys.clear();
    flick(app, "undo", 57.3);
    require(preedit().empty() && sink.keys.empty(), "a reading committed to switch does not reopen");
    flick(app, "mode", 57.4, -60, 0);

    // ↶, as on Gboard: the last kana steps back through its key's taps, and
    // straight after a commit the phrase comes back as a reading.
    flick(app, "a", 60);
    flick(app, "a", 60.3);
    flick(app, "a", 60.6);
    flick(app, "undo", 60.8);
    require(preedit() == "い", "↶ steps the kana back through its key's taps");
    flick(app, "undo", 61);
    flick(app, "undo", 61.2);
    require(preedit() == "ぉ", "and past the first tap to the last");
    flick(app, "a", 61.4);
    require(preedit() == "あ", "a tap after it goes on from there");
    click_key(app, "Enter");
    require(sink.text.back() == "あ" && preedit().empty(), "Enter commits the reading");
    sink.keys.clear();
    flick(app, "undo", 62);
    require(preedit() == "あ" && sink.keys == std::vector<std::pair<int, int>>{{14, 1}, {14, 0}},
            "↶ straight after a commit takes the phrase back as a reading");
    click_key(app, "Enter");
    click_key(app, "ArrowRight");
    sink.keys.clear();
    flick(app, "undo", 63);
    require(preedit().empty() && sink.keys.empty(), "another key ends that chance");
    flick(app, "a", 64);
    click_key(app, "Enter");
    app.cancel_pointer(0);
    flick(app, "undo", 65);
    require(preedit().empty() && sink.keys.empty(), "so does the laser leaving the keyboard");
    // Paste commits the reading, then pastes: Backspace would delete the paste.
    flick(app, "a", 66);
    flick(app, "ka", 66.2);
    click_key(app, "Paste");
    sink.keys.clear();
    flick(app, "undo", 67);
    require(preedit().empty() && sink.keys.empty(), "nothing reopens once more was sent after it");
    // A long phrase comes back whole: one Backspace for each character.
    for (int i = 0; i < 18; ++i) {
        flick(app, "a", 68 + i * .01, -60, 0);
    }
    click_key(app, "Enter");
    sink.keys.clear();
    flick(app, "undo", 69);
    require(g_utf8_strlen(preedit().c_str(), -1) == 18 && sink.keys.size() == 36,
            "an 18-character phrase reopens too");
    clear();
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
// Splits a reading into two-character segments, the last possibly one; a
// segment's candidates are 〈its reading〉 and the reading itself. Resizing
// moves one boundary and splits the rest again, as Anthy does.
struct FakeConverter : JapaneseConverter {
    int* conversions;
    std::vector<std::string> characters;
    std::vector<int> lengths;
    explicit FakeConverter(int* count) : conversions(count) {}
    void split_from(std::size_t segment, int used) {
        lengths.resize(segment);
        for (int left = static_cast<int>(characters.size()) - used; left > 0; left -= 2) {
            lengths.push_back(std::min(2, left));
        }
    }
    std::vector<Segment> segments() const {
        std::vector<Segment> result;
        std::size_t at = 0;
        for (const int length : lengths) {
            std::string reading;
            for (int i = 0; i < length; ++i) {
                reading += characters[at++];
            }
            result.push_back({reading, {"〈" + reading + "〉", reading}});
        }
        return result;
    }
    std::vector<Segment> convert(const std::string& reading) override {
        ++*conversions;
        characters.clear();
        for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
            characters.emplace_back(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        }
        split_from(0, 0);
        return segments();
    }
    std::vector<Segment> resize(int index, int delta) override {
        const auto segment = static_cast<std::size_t>(index);
        int used = 0;
        for (std::size_t i = 0; i <= segment; ++i) {
            used += lengths[i];
        }
        if (lengths[segment] + delta < 1 || used + delta > static_cast<int>(characters.size())) {
            return segments();
        }
        lengths[segment] += delta;
        split_from(segment + 1, used + delta);
        return segments();
    }
};
JapaneseComposer::Factory fake_converter(int* conversions) {
    return [conversions] { return std::make_unique<FakeConverter>(conversions); };
}
// Symbol segments as Anthy returns them: half-width ！ first, a closing 」 as 』.
void symbols_as_typed() {
    struct SymbolConverter : JapaneseConverter {
        std::vector<Segment> convert(const std::string&) override {
            return {{"「", {"「", "『"}},
                    {"えっ", {"謁", "えっ"}},
                    {"」", {"』", "」", "】"}},
                    {"！", {"!", "！"}},
                    {"…", {"‥"}}};
        }
        std::vector<Segment> resize(int, int) override { return convert(""); }
    };
    JapaneseComposer composer([] { return std::make_unique<SymbolConverter>(); });
    composer.kana("「えっ」！…");
    composer.prepare_live();
    require(composer.live().front() == "「謁」！…", "the whole phrase keeps the symbols as typed");
    composer.convert();
    composer.segment(1);
    composer.segment(1);
    composer.cycle(1);
    require(composer.candidates() == std::vector<std::string>{"」", "』", "】"} &&
                composer.commit_text() == "「謁』！…",
            "the other brackets follow the typed one");
}
void progressive_conversion() {
    int conversions = 0;
    JapaneseComposer composer(fake_converter(&conversions));
    composer.kana("にほんご");
    require(composer.live().empty() && !composer.live_current(), "no live candidates before a pause");
    composer.prepare_live();
    require(composer.live_current() &&
                composer.live() == std::vector<std::string>{"〈にほ〉〈んご〉", "〈にほ〉", "にほ",
                                                            "ニホ", "にほんご", "ニホンゴ"},
            "live candidates: the whole phrase, the first segment, then both scripts, each once");
    require(conversions == 1 && composer.preedit() == "にほんご" && !composer.converting(),
            "live candidates leave the reading as typed");
    composer.prepare_live();
    require(conversions == 1, "an unchanged reading is not converted again");
    composer.convert();
    require(conversions == 1 && composer.converting() && composer.segment_count() == 2,
            "conversion reuses the live candidates' segments");
    composer.unconvert();
    require(composer.choose_live(0) && composer.commit_text() == "〈にほ〉〈んご〉",
            "the whole phrase commits at once");
    composer.cancel();
    require(composer.live().empty(), "a new phrase starts without candidates");

    composer.kana("にほんご");
    composer.prepare_live();
    require(!composer.choose_live(2) && composer.converting() && composer.active_segment() == 1 &&
                composer.candidates().front() == "〈んご〉",
            "a first segment's candidate moves on to the second segment");
    require(composer.choose(0) && composer.commit_text() == "にほ〈んご〉",
            "choosing in the last segment completes the phrase");
    composer.cancel();
    composer.kana("にほんご");
    composer.prepare_live();
    require(composer.choose_live(5) && composer.commit_text() == "ニホンゴ", "a katakana candidate");
    composer.cancel();
    composer.kana("にほんご");
    composer.prepare_live();
    composer.kana("を");
    require(!composer.live_current() && composer.live().size() == 6 && !composer.choose_live(0),
            "typing leaves the old list stale, and a stale one picks nothing");
    composer.backspace();
    require(composer.live_current(), "the list is current again for its reading");
    for (int i = 0; i < 4; ++i) {
        composer.backspace();
    }
    require(composer.empty() && composer.live().empty(), "an emptied reading drops the list");

    // Romaji: a final n counts as ん; another letter waits.
    for (const char key : std::string("nihon")) {
        composer.roman(key);
    }
    composer.prepare_live();
    require(composer.live_current() && composer.live().front() == "〈にほ〉〈ん〉",
            "a final n converts as ん");
    composer.roman('k');
    composer.prepare_live();
    require(!composer.live_current() && composer.live().front() == "〈にほ〉〈ん〉",
            "a pending consonant leaves the list for the reading before it");
    composer.backspace();
    require(composer.choose_live(0) && composer.commit_text() == "〈にほ〉〈ん〉",
            "picking with a pending n commits the ん it was converted with");
    composer.cancel();

    // Resizing: the active segment's end moves; the rest splits again.
    composer.kana("にほんごを");
    composer.convert();
    require(composer.preedit() == "〈にほ〉〈んご〉〈を〉", "three segments");
    composer.resize(1);
    require(composer.preedit() == "〈にほん〉〈ごを〉" && composer.active_segment() == 0,
            "→ lengthens the active segment");
    composer.resize(-1);
    composer.resize(-1);
    require(composer.preedit() == "〈に〉〈ほん〉〈ごを〉", "← shortens it");
    composer.resize(-1);
    require(composer.preedit() == "〈に〉〈ほん〉〈ごを〉", "a segment keeps at least one character");
    composer.cycle(1);
    require(!composer.choose(1) && composer.active_segment() == 1, "choosing moves on");
    composer.resize(1);
    require(composer.preedit() == "に〈ほんご〉〈を〉", "earlier segments keep their choices");
    composer.segment(1);
    composer.resize(1);
    require(composer.preedit() == "に〈ほんご〉〈を〉", "the last segment cannot reach past the end");
    const int before = conversions;
    composer.unconvert();
    composer.convert();
    require(conversions == before + 1 && composer.preedit() == "〈にほ〉〈んご〉〈を〉",
            "converting again after a resize starts from Anthy's own split");
    composer.cancel();
}
// The band and its candidates, with the fake converter: live candidates after
// a pause, a tap moving on through the segments, the arrows resizing, a drag
// scrolling without picking, and the keys keeping their size across A / あ.
void candidate_band() {
    char directory[] = "/tmp/framekeyboard-band-XXXXXX";
    require(mkdtemp(directory), "temporary band profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    int conversions = 0;
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.japanese_converter = fake_converter(&conversions);
    Sink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    {
        // Shells are offered layouts to choose; the English page is not one.
        const std::string listed = json_object_to_json_string(app.host_profiles().get());
        require(listed.find("\"ja-flick-12\"") != std::string::npos &&
                    listed.find("ja-flick-qwerty") == std::string::npos,
                "a shell sees the flick layout but not its English page");
    }
    for (const auto* removed :
         {"ime-toggle", "ime-convert", "ime-commit", "ime-cancel", "ime-hiragana", "ime-katakana",
          "ime-next", "ime-prev", "ime-segment-next", "ime-segment-prev"}) {
        require(!has_control(app, removed),
                "the keys convert, commit, cancel and switch modes: no duplicate buttons");
    }
    const auto key_at = [&] { return key_centre(app, "Flick-na"); };
    const auto kana_keys = key_at();
    {
        const auto view = app.view();
        const auto top = panel_rect(view, view.layout->keys.front().bounds).y;
        const auto band = ime_band(view);
        require(view.ime_band && panel_case_bounds(view).width < panel_width - 1 &&
                    band.candidates.y > band.reading.y &&
                    top > band.candidates.y + band.candidates.height,
                "the flick case fits its keys, under a reading row and a candidate row");
    }
    const auto kana_case = panel_case_bounds(app.view());
    flick(app, "mode", 1);
    require(panel_case_bounds(app.view()) == kana_case, "the English page keeps the case's size");
    flick(app, "mode", 2, -60, 0);
    require(key_at() == kana_keys, "back on the kana page, its keys are where they were");
    auto chips = [&] {
        std::vector<std::string> result;
        if (const auto strip = app.view().candidates) {
            for (const auto& chip : strip->chips) {
                result.push_back(chip.text);
            }
        }
        return result;
    };
    flick(app, "na", 10, -60, 0);
    flick(app, "ha", 10.2);
    flick(app, "na", 10.4);
    flick(app, "ka", 10.6, 0, 60);
    require(app.view().preedit == "にはなこ", "a reading");
    app.tick(10.65);
    app.tick(10.7);
    require(!app.view().candidates && conversions == 0, "no candidates while typing");
    app.tick(10.9);
    const auto live = chips();
    require(conversions == 1 && live.size() >= 4 && live.front() == "〈には〉〈なこ〉" &&
                live[1] == "〈には〉",
            "live candidates after a pause: the whole phrase, then the first segment's");
    flick(app, "ma", 11);
    app.tick(11.01);
    require(app.view().candidates && app.view().candidates->stale, "typing on leaves a stale list");
    require(click_candidate(app, "〈には〉", 11.02) && app.view().preedit == "にはなこま" &&
                app.view().candidates->stale,
            "a stale chip is shown but picks nothing");
    click_key(app, "Backspace");
    app.tick(11.1);
    require(!app.view().candidates->stale, "the list is current again for its reading");
    require(click_candidate(app, "〈には〉", 11.2) && app.view().preedit == "〈には〉〈なこ〉" &&
                app.view().preedit_active_begin == std::string("〈には〉").size(),
            "a first segment's live candidate moves on to the second segment");
    require(chips().front() == "〈なこ〉" && sink.text.empty(), "the next segment's candidates");
    require(click_candidate(app, "なこ", 11.3) && sink.text.back() == "〈には〉なこ" &&
                app.view().preedit.empty(),
            "choosing in the last segment commits the phrase");

    // Flick arrows resize the converting segment; 次候補 steps through its
    // candidates, and ↶ returns to the reading.
    flick(app, "na", 12, -60, 0);
    flick(app, "ha", 12.2);
    flick(app, "na", 12.4);
    flick(app, "ka", 12.6, 0, 60);
    click_key(app, "Space");
    app.tick(12.7);
    require(app.view().preedit == "〈には〉〈なこ〉", "変換 converts");
    click_key(app, "ArrowRight");
    require(app.view().preedit == "〈にはな〉〈こ〉", "→ lengthens the segment");
    click_key(app, "ArrowLeft");
    click_key(app, "ArrowLeft");
    require(app.view().preedit == "〈に〉〈はな〉〈こ〉", "← shortens it, splitting the rest again");
    {
        // ∨ ends the candidate row, beside its chips, not over them.
        const auto view = app.view();
        const auto band = ime_band(view);
        const auto more = std::find_if(view.controls.begin(), view.controls.end(),
                                       [&](const Control& c) { return c.id == "ime-more"; });
        require(more != view.controls.end() &&
                    more->bounds.x >= band.candidates.x + band.candidates.width &&
                    more->bounds.x + more->bounds.width <= band.band.x + band.band.width,
                "the band's ∨ sits right of the candidate row");
    }
    click_key(app, "Space");
    require(app.view().preedit == "に〈はな〉〈こ〉", "次候補 moves to the next candidate");
    click_key(app, "Space");
    click_key(app, "Space");
    require(app.view().preedit == "〈に〉〈はな〉〈こ〉", "and wraps back to the first");
    flick(app, "undo", 12.8);
    require(app.view().preedit == "にはなこ" && !app.view().candidates->stale,
            "↶ returns to the reading");
    for (int i = 0; i < 4; ++i) {
        click_key(app, "Backspace");
    }

    // A drag along the strip scrolls it without picking anything.
    for (int i = 0; i < 24; ++i) {
        flick(app, "na", 13 + i * .02, -60, 0);
    }
    app.tick(14);
    app.tick(14.2);
    const auto committed = sink.text.size();
    const auto strip = app.view().candidates;
    require(strip && !strip->chips.empty(), "candidates for a long reading");
    const auto& first = strip->chips.front().bounds;
    const double y = first.y + first.height / 2, x = first.x + first.width / 2;
    app.down(7, x, y, 14.3);
    app.move(7, x - 60, y);
    app.move(7, x - 120, y);
    app.up(7, x - 120, y);
    app.tick(14.4);
    require(sink.text.size() == committed && !app.view().preedit.empty() &&
                app.view().candidates->chips.front().bounds.x < first.x,
            "a drag scrolls the strip and picks nothing");
    while (!app.view().preedit.empty()) {
        click_key(app, "Backspace");
    }

    // Romaji on a full layout: the band too, and PC conventions for the arrows.
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    require(app.view().ime_band, "full layouts have the band too");
    for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyN", "KeyA", "KeyK", "KeyO"}) {
        click_key(app, id);
    }
    click_key(app, "Space");
    app.tick(20);
    click_key(app, "ArrowRight");
    require(app.view().preedit == "〈には〉〈なこ〉" &&
                app.view().preedit_active_begin == std::string("〈には〉").size(),
            "→ moves to the next segment");
    click_key(app, "ShiftLeft");
    click_key(app, "ArrowLeft");
    require(app.view().preedit == "〈には〉〈な〉〈こ〉", "Shift+← shortens it");
    click_key(app, "Enter");
    require(sink.text.back() == "〈には〉〈な〉〈こ〉", "Enter commits every segment");
    if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        for (int i = 0; i < 5; ++i) {
            flick(app, "na", 30 + i * .1, -60, 0);
        }
        app.tick(31);
        app.tick(31.2);
        app.paint(31.2);
        app.renderer.write_png(fs::path(render) / "framekeyboard-ja-band.png");
    }
}
void clear_reading(App& app);
// A converter that records what learning tells it, and offers 有難う for あり.
struct LearningConverter : JapaneseConverter {
    std::vector<std::vector<std::string>>* learned;
    std::vector<std::pair<std::string, std::string>>* picked;
    std::vector<Segment> convert(const std::string& reading) override {
        return {{reading, {"〈" + reading + "〉", reading}}};
    }
    std::vector<Segment> resize(int, int) override { return {}; }
    void learn(const std::vector<std::string>& chosen) override { learned->push_back(chosen); }
    std::vector<std::string> predict(const std::string& reading) override {
        return reading == "あり" ? std::vector<std::string>{"有難う"} : std::vector<std::string>{};
    }
    void learn_prediction(const std::string& reading, const std::string& text) override {
        picked->emplace_back(reading, text);
    }
};
// Learning (docs/japanese.md, Learning): with it on, a committed phrase
// teaches the converter its choices and learned words join the live
// candidates; off, the converter hears nothing.
void learning_calls() {
    std::vector<std::vector<std::string>> learned;
    std::vector<std::pair<std::string, std::string>> picked;
    JapaneseComposer composer([&] {
        auto converter = std::make_unique<LearningConverter>();
        converter->learned = &learned;
        converter->picked = &picked;
        return converter;
    });
    composer.kana("あり");
    composer.prepare_live();
    require(std::find(composer.live().begin(), composer.live().end(), "有難う") == composer.live().end(),
            "off, no learned words are offered");
    composer.convert();
    composer.commit_text();
    composer.learn();
    require(learned.empty() && picked.empty(), "off, nothing is learned");
    composer.cancel();

    composer.set_learning(true);
    composer.kana("あり");
    composer.prepare_live();
    require(composer.live() == std::vector<std::string>{"〈あり〉", "有難う", "あり", "アリ"},
            "learned words come after the whole phrase");
    require(composer.choose_live(1) && composer.commit_text() == "有難う", "a learned word commits");
    composer.learn();
    require(picked == std::vector<std::pair<std::string, std::string>>{{"あり", "有難う"}},
            "and is learned again, for what was typed");
    composer.cancel();
    composer.kana("あり");
    composer.convert();
    composer.cycle(1);
    require(composer.commit_text() == "あり", "a conversion commits");
    composer.learn();
    require(learned == std::vector<std::vector<std::string>>{{"あり"}},
            "its segments' choices are learned");
}
// The Languages card offers learning with Japanese, saves it, and clears what
// Anthy learned after asking, with a busy button meanwhile.
void learning_settings() {
    char directory[] = "/tmp/framekeyboard-learning-XXXXXX";
    require(mkdtemp(directory), "temporary learning profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::vector<std::vector<std::string>> learned;
    std::vector<std::pair<std::string, std::string>> picked;
    Options options;
    options.config_dir = directory;
    options.anthy_home = fs::path(directory) / "home";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.japanese_converter = [&] {
        auto converter = std::make_unique<LearningConverter>();
        converter->learned = &learned;
        converter->picked = &picked;
        return converter;
    };
    Sink sink;
    App app(options, sink);
    app.apply({"en-us-full", "en-us", "graphite"});
    app.show_settings();
    require(!has_control(app, "japanese-learning") && !has_control(app, "clear-learning"),
            "learning is offered only with Japanese");
    control(app, "settings"); // Back
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    app.show_settings();
    const auto find = [&](const std::string& id) {
        const auto controls = app.view().controls;
        const auto found =
            std::find_if(controls.begin(), controls.end(), [&](const Control& c) { return c.id == id; });
        if (found == controls.end()) {
            throw std::runtime_error("Missing control " + id);
        }
        return *found;
    };
    require(find("japanese-learning").selected && !find("clear-learning").enabled,
            "learning starts on, as on Steam's keyboard, with nothing to clear");
    control(app, "settings"); // Back
    flick(app, "a", 1);
    click_key(app, "Space");
    click_key(app, "Enter");
    require(sink.text.back() == "〈あ〉" && learned == std::vector<std::vector<std::string>>{{"〈あ〉"}},
            "a committed conversion is learned");
    app.show_settings();
    control(app, "japanese-learning");
    control(app, "apply");
    app.flush_settings();
    std::vector<std::string> errors;
    require(!load_settings(options.config_dir, errors).japanese_learning, "turning it off is saved");
    flick(app, "a", 1.5);
    click_key(app, "Space");
    click_key(app, "Enter");
    require(sink.text.size() == 2 && learned.size() == 1, "and nothing is learned then");

    // What Anthy learned, as its record file; Clear asks, then deletes it.
    const auto record = options.anthy_home / ".anthy" / "last-record2_framekeyboard.utf8";
    fs::create_directories(record.parent_path());
    std::ofstream(record) << "learned\n";
    {
        // A shell's preview, built beside the running keyboard, only draws.
        Options preview = options;
        preview.mode = Mode::Render;
        preview.config_dir = fs::path(directory) / "preview";
        preview.anthy_home.clear();
        fs::create_directories(preview.config_dir);
        App image(preview, sink);
        require(anthy_learned(), "a preview leaves the running keyboard's learning where it is");
    }
    app.show_settings();
    require(find("clear-learning").enabled, "something learned can be cleared");
    control(app, "clear-learning");
    require(app.view().confirm && fs::exists(record), "Clear asks first");
    control(app, "confirm-cancel");
    require(!app.view().confirm && fs::exists(record), "and cancelling keeps it");
    control(app, "clear-learning");
    control(app, "confirm-accept");
    for (int i = 0; i < 400 && find("clear-learning").busy; ++i) {
        require(!find("clear-learning").enabled, "a busy Clear can't be pressed again");
        app.tick(2 + i * .01);
        usleep(5000);
    }
    require(!fs::exists(record) && !find("clear-learning").enabled && !find("clear-learning").busy &&
                app.view().status == "Learned words cleared.",
            "clearing deletes the record, then says so");
}
// The band's ∨ opens the whole list over the keys, as a phone's does: the row
// keeps the chips that fit, the rest wrap below it and scroll up and down,
// and ∧, a choice or hiding closes it again.
void candidate_list() {
    struct ManyConverter : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            Segment segment{reading, {}};
            for (int i = 0; i < 200; ++i) {
                segment.candidates.push_back("候補" + std::to_string(i));
            }
            return {segment};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    char directory[] = "/tmp/framekeyboard-list-XXXXXX";
    require(mkdtemp(directory), "temporary list profile");
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
    options.japanese_converter = [] { return std::make_unique<ManyConverter>(); };
    Sink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    const auto [kx, ky] = key_centre(app, "Flick-ka");
    require(!has_control(app, "ime-more"), "no ∨ without candidates");
    flick(app, "ka", 1);
    app.tick(1.1);
    app.tick(1.3);
    require(has_control(app, "ime-more") && !app.view().candidates->list, "∨ comes with candidates");
    const auto row = app.view().candidates->chips;
    control(app, "ime-more");
    auto view = app.view();
    require(view.candidates && view.candidates->list && !app.renderer.hit_key(view, kx, ky),
            "∨ opens the list over the keys");
    const auto list = *view.candidates->list;
    const auto area = view.candidates->area;
    std::vector<CandidateStripView::Chip> listed;
    for (const auto& chip : view.candidates->chips) {
        if (chip.listed) {
            listed.push_back(chip);
            require(chip.bounds.x >= list.x && chip.bounds.x + chip.bounds.width <= list.x + list.width,
                    "listed chips wrap within the list");
        } else {
            require(chip.bounds.x + chip.bounds.width <= area.x + area.width,
                    "the row keeps only the chips that fit");
        }
    }
    require(!listed.empty() && listed.front().bounds.x == list.x &&
                listed.front().bounds.y > row.front().bounds.y + row.front().bounds.height &&
                view.candidates->chips.front().text == "候補0",
            "the rest go on under the row, from the start of the list");
    // A press between two lines, over what was a key, types nothing.
    const double gap = listed.front().bounds.y + listed.front().bounds.height + 4;
    app.down(0, kx, gap, 2);
    app.up(0, kx, gap);
    require(app.view().preedit == "か" && sink.text.empty() && app.view().candidates->list,
            "the covered keys take no press");
    // A drag scrolls the list up and down; the row stays.
    const auto& from = listed.front().bounds;
    const double x = from.x + from.width / 2, y = from.y + from.height / 2;
    app.down(7, x, y, 3);
    app.move(7, x, y - 60);
    app.move(7, x, y - 120);
    app.up(7, x, y - 120);
    app.tick(3.1);
    view = app.view();
    const auto first = std::find_if(view.candidates->chips.begin(), view.candidates->chips.end(),
                                    [&](const auto& chip) { return chip.text == listed.front().text; });
    require(sink.text.empty() && view.candidates->chips.front().bounds == row.front().bounds &&
                (first == view.candidates->chips.end() || first->bounds.y < from.y),
            "a drag scrolls the list, not the row, and picks nothing");
    control(app, "ime-more");
    view = app.view();
    require(!view.candidates->list && app.renderer.hit_key(view, kx, ky), "∧ closes it");
    // A choice in the list commits it and closes the list.
    control(app, "ime-more");
    view = app.view();
    std::string pick;
    for (const auto& chip : view.candidates->chips) {
        if (chip.listed && pick.empty()) {
            pick = chip.text;
        }
    }
    require(click_candidate(app, pick, 4) && sink.text == std::vector<std::string>{pick} &&
                !app.view().candidates && !has_control(app, "ime-more"),
            "a choice in the list commits it");
    flick(app, "ka", 5);
    app.tick(5.1);
    app.tick(5.3);
    require(app.view().candidates && !app.view().candidates->list, "the next reading starts closed");
    control(app, "ime-more");
    app.set_interaction_active(false);
    app.tick(5.8); // Hidden, as the VR loop keeps ticking.
    app.set_interaction_active(true);
    flick(app, "ka", 6);
    app.tick(6.1);
    app.tick(6.3);
    require(app.view().candidates && !app.view().candidates->list, "hiding closes it");

    // Keys held as ∨ opens let go: a repeating ⌫ and a flick still to land.
    clear_reading(app);
    for (const double at : {7.0, 8.5, 10.0}) {
        flick(app, "ka", at);
    }
    app.tick(10.2);
    app.tick(10.4);
    const auto [bx, by] = key_centre(app, "Backspace");
    const auto [fx, fy] = key_centre(app, "Flick-na");
    app.down(1, bx, by, 11);
    app.down(2, fx, fy, 11);
    app.move(2, fx, fy - 60);
    require(app.view().preedit == "かか", "⌫ takes one kana on press");
    control(app, "ime-more");
    for (double at = 11.1; at < 13; at += .1) {
        app.tick(at);
    }
    app.up(1, bx, by);
    app.up(2, fx, fy - 60);
    require(app.view().preedit == "かか" && app.view().candidates->list,
            "keys held under the opening list change nothing");
}
void clear_reading(App& app) {
    for (int i = 0; i < 40 && !app.view().preedit.empty(); ++i) {
        click_key(app, "Backspace");
    }
}
// ↶'s Backspaces can take a while to be acknowledged, as through a shell:
// nothing typed meanwhile may overtake the reading coming back, and a
// recentre or pin keeps it coming.
void slow_reopen() {
    struct SlowSink : Sink {
        bool hold = false, hold_text = false, text_waiting = false;
        int allow = 0; // Backspaces let through while holding them
        Delivery deliver_key(int code, int value) override {
            if (hold && code == 14 && value == 1) {
                if (allow == 0) {
                    return Delivery::Waiting;
                }
                --allow;
            }
            return Sink::deliver_key(code, value);
        }
        bool commit_text(const std::string& value) override {
            text_waiting = hold_text;
            return !hold_text && Sink::commit_text(value);
        }
        bool text_pending() const override { return text_waiting; }
    };
    char directory[] = "/tmp/framekeyboard-reopen-XXXXXX";
    require(mkdtemp(directory), "temporary reopen profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    // A dictation server shows the mic key; nothing connects to it here.
    std::ofstream(fs::path(directory) / "config.json")
        << R"({"schema_version":1,"active":{"layout":"ja-flick-12","language":"ja-flick",)"
        << R"("theme":"graphite"},"dictation":"server","dictation_server":"speech.local:8031"})";
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    SlowSink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    flick(app, "a", 1);
    click_key(app, "Enter");
    sink.hold = true;
    flick(app, "undo", 2);
    require(app.view().preedit.empty(), "the reading waits for its Backspace");
    {
        const auto [x, y] = key_centre(app, "Flick-ka");
        require(!app.down(0, x, y, 2.5), "keys wait for the reading too");
        app.up(0, x, y);
    }
    control(app, "recenter");
    sink.hold = false;
    app.tick(3);
    require(app.view().preedit == "あ", "a recentre keeps the reading coming");
    flick(app, "ka", 3.5);
    require(app.view().preedit == "あか", "typing goes on after it");
    // A field that never answers is given up on, and the keys work again.
    click_key(app, "Enter");
    sink.hold = true;
    flick(app, "undo", 4);
    app.tick(4.1);
    app.tick(6);
    require(app.view().preedit.empty(), "it is waited for a while");
    app.tick(7.2);
    {
        const auto [x, y] = key_centre(app, "Flick-ka");
        require(app.down(0, x, y, 7.3) &&
                    app.view().status.find("stopped answering") != std::string::npos,
                "after a few seconds the wait ends, saying so");
        app.up(0, x, y);
    }
    click_key(app, "Backspace");
    sink.hold = false;
    // The time counts from the Backspaces, not from pressing ↶: a long hold
    // on ↶ still leaves the field its few seconds.
    flick(app, "a", 10);
    click_key(app, "Enter");
    sink.hold = true;
    {
        const auto [x, y] = key_centre(app, "Flick-undo");
        app.down(0, x, y, 11);
        app.tick(12);
        app.tick(14.5);
        app.up(0, x, y);
    }
    app.tick(14.6);
    app.tick(15);
    sink.hold = false;
    app.tick(15.1);
    require(app.view().preedit == "あ" &&
                app.view().status.find("stopped answering") == std::string::npos,
            "a long hold on ↶ does not use up the wait");
    clear_reading(app);
    // A key held with the other hand comes up before the Backspaces go out,
    // so its repeat cannot land ahead of the reading.
    {
        // Space types text, which repeats while held without waiting in
        // the queue the way a held arrow does.
        const auto [x, y] = key_centre(app, "Space");
        require(app.down(1, x, y, 21), "the other hand holds Space");
    }
    flick(app, "a", 21.1);
    click_key(app, "Enter");
    flick(app, "undo", 21.2); // Before Space repeats, which would come after the phrase.
    require(app.view().preedit == "あ", "↶ reopens with the other hand holding a key");
    require(!app.pointer_pressed(1), "↶ lets the held key go first");
    clear_reading(app);
    // The mic acts on release, which goes with its press: letting go of it
    // after ↶ must not commit the reading that came back.
    for (bool delayed : {false, true}) {
        const auto [x, y] = key_centre(app, "Dictation");
        require(app.down(1, x, y, 22), "the other hand holds the mic");
        flick(app, "a", 22.1);
        click_key(app, "Enter");
        sink.hold = delayed;
        flick(app, "undo", 22.2);
        sink.hold = false;
        app.tick(22.3);
        app.up(1, x, y);
        require(app.view().preedit == "あ" && !app.dictation_open(),
                "letting go of the mic after ↶ keeps the reading");
        clear_reading(app);
    }
    // A key pressed while ↶ is still held lets ↶ act first, as it was
    // pressed first, and then waits for the reading like any other key.
    for (bool delayed : {false, true}) {
        flick(app, "a", 23);
        click_key(app, "Enter");
        sink.hold = delayed;
        const auto [ux, uy] = key_centre(app, "Flick-undo");
        const auto [kx, ky] = key_centre(app, "Flick-ka");
        app.down(0, ux, uy, 23.1);
        require(app.down(1, kx, ky, 23.2) != delayed,
                "a key pressed during ↶ types after its reading, or waits for it");
        app.up(1, kx, ky);
        app.up(0, ux, uy);
        sink.hold = false;
        app.tick(23.3);
        require(app.view().preedit == (delayed ? "あ" : "あか"), "the reading comes back first");
        clear_reading(app);
    }
    // Settings waits too, as opening it would give up input with the phrase
    // already deleted.
    flick(app, "a", 24);
    click_key(app, "Enter");
    sink.hold = true;
    flick(app, "undo", 24.1);
    control(app, "settings");
    require(!app.view().settings, "Settings waits for the reading");
    sink.hold = false;
    app.tick(24.2);
    require(app.view().preedit == "あ", "and the reading comes back");
    clear_reading(app);
    // The few seconds count from the last Backspace the field took, so a
    // slow field that still answers gets the reading back.
    flick(app, "a", 25);
    flick(app, "ka", 25.05);
    flick(app, "sa", 25.1);
    click_key(app, "Enter");
    sink.hold = true;
    flick(app, "undo", 25.2);
    app.tick(25.3);
    app.tick(27.8);
    sink.allow = 1;
    app.tick(27.9);
    app.tick(30.5);
    sink.hold = false;
    app.tick(30.6);
    require(app.view().preedit == "あかさ" &&
                app.view().status.find("stopped answering") == std::string::npos,
            "a slow field that still answers keeps the wait going");
    clear_reading(app);
    // ↶ tapped while the phrase is still being typed says so, and keeps
    // the chance for a tap once it is in.
    sink.hold_text = true;
    flick(app, "a", 31);
    click_key(app, "Enter");
    sink.keys.clear();
    flick(app, "undo", 31.1);
    require(app.view().preedit.empty() && sink.keys.empty() &&
                app.view().status.find("still being typed") != std::string::npos,
            "↶ during typing sends nothing and says why");
    sink.hold_text = false;
    app.tick(31.3);
    flick(app, "undo", 31.4);
    require(app.view().preedit == "あ" && !sink.text.empty() && sink.text.back() == "あ" &&
                app.view().status.find("still being typed") == std::string::npos,
            "a tap once the phrase is in brings it back");
    clear_reading(app);
    // Anything but a key, such as Settings, still ends the chance instead:
    // opening it gives up input, which would leave the phrase deleted.
    flick(app, "a", 24);
    click_key(app, "Enter");
    sink.keys.clear();
    {
        const auto [ux, uy] = key_centre(app, "Flick-undo");
        app.down(0, ux, uy, 24.1);
        control(app, "settings");
        require(app.view().settings && std::none_of(sink.keys.begin(), sink.keys.end(),
                                                    [](const auto& key) { return key.first == 14; }),
                "Settings pressed during ↶ ends its chance, deleting nothing");
        app.up(0, ux, uy);
    }
}
// Each flick page and dictation mode keep the toolbar where it is, with the
// mic key shown: only the keys change.
void flick_pages_keep_toolbar() {
    char directory[] = "/tmp/framekeyboard-flick-pages-XXXXXX";
    require(mkdtemp(directory), "temporary flick pages profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::ofstream(fs::path(directory) / "config.json")
        << R"({"schema_version":1,"active":{"layout":"ja-flick-12","language":"ja-flick",)"
        << R"("theme":"graphite"},"dictation":"server","dictation_server":"speech.local:8031",)"
        << R"("hide_numpad":true})"; // Without the numpad, romaji's keys fit by height.
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    Sink sink;
    App app(options, sink);
    struct Page {
        Rect settings, close;
        double keys_right{}, key_height{};
    };
    auto page = [&] {
        const auto view = app.view();
        Page result;
        for (const auto& control : view.controls) {
            if (control.id == "settings") {
                result.settings = control.bounds;
            } else if (control.id == "close") {
                result.close = control.bounds;
            }
        }
        for (const auto& key : view.layout->keys) {
            const auto shown = panel_rect(view, key.bounds);
            result.keys_right = std::max(result.keys_right, shown.x + shown.width);
            if (key.id == "Backspace") {
                result.key_height = shown.height;
            }
        }
        return result;
    };
    const auto kana = page();
    require(kana.close.width > 0 && kana.key_height > 0, "the kana page has a toolbar and keys");
    flick(app, "mode", 1);
    const auto english = page();
    require(english.settings == kana.settings && english.close == kana.close,
            "the English page keeps the toolbar's buttons and X in place");
    require(std::abs(english.keys_right - kana.keys_right) < .5,
            "the English page's keys reach the same right edge");
    flick(app, "mode", 2);
    flick(app, "mode", 3);
    click_key(app, "Dictation");
    require(app.dictation_open(), "the mic opens dictation");
    const auto dictation = page();
    require(dictation.settings == kana.settings && dictation.close == kana.close &&
                std::abs(dictation.key_height - kana.key_height) < .5 &&
                std::abs(dictation.keys_right - kana.keys_right) < .5,
            "dictation keeps the keys' size and the toolbar in place");
    click_key(app, "Escape"); // Stops listening,
    click_key(app, "Escape"); // then leaves.
    // So does romaji's, under its one-row band.
    app.apply({"ja-jis-full", "ja-romaji", "graphite"});
    const auto romaji = page();
    require(!app.dictation_open() && app.view().layout->id == "ja-jis-full", "romaji shows its keys");
    click_key(app, "Dictation");
    const auto romaji_dictation = page();
    require(app.dictation_open() && std::abs(romaji_dictation.key_height - romaji.key_height) < .5 &&
                std::abs(romaji_dictation.keys_right - romaji.keys_right) < .5,
            "romaji dictation keeps the keys' size");
    click_key(app, "Escape");
    click_key(app, "Escape");
    // A language change on the same layout adds or removes the band, and the
    // open mode follows it.
    app.apply({"en-us-full", "en-us", "graphite"});
    const auto english_full = page();
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    const auto romaji_full = page();
    click_key(app, "Dictation");
    require(app.dictation_open() && std::abs(page().key_height - romaji_full.key_height) < .5,
            "romaji dictation on the full layout keeps the keys' size");
    app.apply({"en-us-full", "en-us", "graphite"});
    app.tick(5);
    require(app.dictation_open() && std::abs(page().key_height - english_full.key_height) < .5,
            "switching to English in the mode takes English's size");
}
// The cursor in the reading: ← and → move it before conversion, and
// typing, ⌫, the marks and romaji act there.
void reading_cursor() {
    JapaneseComposer composer;
    auto bytes = [](const char* text) { return std::string(text).size(); };
    composer.kana("あいう");
    require(composer.cursor() == composer.preedit().size(), "the cursor starts at the end");
    require(composer.move(-1) && composer.move(-1) && composer.cursor() == bytes("あ"),
            "← moves it back a character");
    composer.kana("か");
    require(composer.preedit() == "あかいう" && composer.cursor() == bytes("あか"),
            "kana go in at the cursor");
    composer.backspace();
    require(composer.preedit() == "あいう" && composer.cursor() == bytes("あ"),
            "⌫ removes the character before it");
    composer.kana("は");
    require(composer.mark("゛") && composer.preedit() == "あばいう" && composer.last_kana() == "ば",
            "a mark changes the kana before the cursor");
    composer.roman('k');
    require(composer.preedit() == "あばkいう" && composer.cursor() == bytes("あばk"),
            "romaji still waiting shows at the cursor");
    composer.roman('a');
    require(composer.preedit() == "あばかいう" && composer.cursor() == bytes("あばか"),
            "and becomes kana there");
    composer.script(true);
    require(composer.preedit() == "アバカイウ" && composer.cursor() == bytes("アバカ"),
            "a script change keeps the cursor's place");
    require(composer.move(1) && composer.move(1) && !composer.move(1) &&
                composer.cursor() == bytes("アバカイウ"),
            "→ stops at the end");
    composer.cancel();
    composer.kana("あ");
    require(composer.move(-1) && !composer.move(-1) && composer.cursor() == 0, "← stops at the start");
    composer.roman('n');
    require(composer.preedit() == "nあ" && composer.commit_text() == "んあ",
            "a final n settles at the cursor too");
    composer.cancel();
    int conversions = 0;
    JapaneseComposer converting(fake_converter(&conversions));
    converting.kana("にほんご");
    converting.move(-1);
    converting.convert();
    require(converting.preedit() == "〈にほ〉〈んご〉" && !converting.move(-1),
            "conversion takes the whole reading, and the cursor waits");
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
// Romaji and flick commits type through App's text output, so a local editor
// can receive them instead of the target.
struct RecordedText : TextOutput {
    std::vector<std::string> commits;
    bool text_press(unsigned, const std::string&, double) override { return false; }
    bool commit_text(const std::string& text) override {
        commits.push_back(text);
        return true;
    }
    bool replaces_text() override { return false; }
    bool replace_text(std::size_t, const std::string&) override { return false; }
    bool taps_then_text(int, std::size_t, const std::string&) override { return false; }
};
void text_output_capture() {
    char directory[] = "/tmp/framekeyboard-japanese-output-XXXXXX";
    require(mkdtemp(directory), "temporary text output profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    RecordedText recorded;
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.text_output = &recorded;
    Sink sink;
    App app(options, sink);
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    click_key(app, "KeyK");
    click_key(app, "KeyA");
    click_key(app, "Enter");
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    flick(app, "a", 1);
    click_key(app, "Enter");
    require(recorded.commits == std::vector<std::string>{"か", "あ"},
            "Romaji and flick commits use the text output");
    require(sink.text.empty() && sink.keys.empty(), "nothing reaches the target past the text output");
}
int main() {
    // Anthy keeps what it learns, and its lock file, under a home of the tests' own.
    char anthy_home[] = "/tmp/framekeyboard-anthy-XXXXXX";
    require(mkdtemp(anthy_home), "temporary Anthy home");
    struct AnthyHome {
        fs::path path;
        ~AnthyHome() { fs::remove_all(path); }
    } anthy_cleanup{anthy_home};
    configure_anthy({false, anthy_home});
    try {
        symbols_as_typed();
        learning_calls();
        progressive_conversion();
        candidate_band();
        candidate_list();
        learning_settings();
        configure_anthy({false, anthy_home});
        reading_cursor();
        slow_reopen();
        flick_pages_keep_toolbar();
        romaji_legends();
        flick_input();
        text_output_capture();
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
            set_anthy_dictionary(dictionary);
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
            composer.cancel();
            for (const auto& [reading, phrase] : std::vector<std::pair<std::string, std::string>>{
                     {"ちょっとまって！", "ちょっと待って！"}, {"「こんにちは」", "「今日は」"}}) {
                composer.cancel();
                composer.kana(reading);
                composer.convert();
                require(composer.commit_text() == phrase, "Anthy's symbols stay as typed");
            }
            // Learning with the real Anthy, kept in the tests' own home.
            configure_anthy({true, anthy_home});
            restart_anthy(false);
            {
                JapaneseComposer learner;
                learner.set_learning(true);
                const auto offers = [&](const std::string& text) {
                    return std::find(learner.live().begin(), learner.live().end(), text) !=
                           learner.live().end();
                };
                learner.kana("ありがとう");
                learner.convert();
                const auto phrase = learner.commit_text();
                learner.learn();
                learner.cancel();
                require(anthy_learned() &&
                            !fs::exists(fs::path(anthy_home) / ".anthy" / "last-record2_default.utf8"),
                        "a committed phrase is learned, apart from the stock keyboard's");
                learner.kana("あり");
                learner.prepare_live();
                require(offers(phrase), "and offered again once its start is typed");
                restart_anthy(true);
                learner.cancel();
                learner.kana("あり");
                learner.prepare_live();
                require(!anthy_learned() && !offers(phrase), "clearing forgets it");
            }
            configure_anthy({false, anthy_home});
            restart_anthy(false);
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
            click_key(app, "Space");
            app.tick(2);
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
        click_key(app, "Enter");
        require(app.view().preedit == "あ", "failed delivery retains text");
        sink.accept = true;
        app.cancel();
        require(app.view().preedit.empty(), "target cancellation discards preedit");
        click_key(app, "KeyA");
        require(!app.view().preedit.empty(), "preedit exists before dashboard closes");
        const auto sent_before_hide = sink.text.size();
        app.set_interaction_active(false);
        require(app.view().preedit.empty(), "dashboard close discards Japanese preedit");
        click_key(app, "Enter", 0, true, false); // Not taken while hidden.
        app.set_interaction_active(true);
        click_key(app, "Enter");
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
            click_key(converted, "Space");
            converted.tick(2);
            require(click_candidate(converted, "日本", 2),
                    "日本 is a candidate, and choosing the last segment's commits the phrase");
            require(sink.text == std::vector<std::string>{"日本"},
                    "the candidate commits through the text sink");
            // Live candidates after a pause in typing, before any 変換.
            for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyO", "KeyN", "KeyG", "KeyO"}) {
                click_key(converted, id);
            }
            converted.tick(3);
            converted.tick(3.1);
            require(!converted.view().candidates, "no live candidates before the pause");
            converted.tick(3.3);
            const auto live = converted.view().candidates;
            require(live && !live->stale && live->chips.size() > 2, "live candidates after a pause");
            require(click_candidate(converted, "日本語", 3.4),
                    "the whole phrase is a live candidate and commits at once");
            require(sink.text.back() == "日本語" && converted.view().preedit.empty(),
                    "a live candidate commits through the text sink");
            if (const auto* directory = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
                for (const auto& id : {"KeyK", "KeyY", "KeyO", "KeyU", "KeyH", "KeyA"}) {
                    click_key(converted, id);
                }
                converted.tick(4);
                converted.tick(4.3);
                converted.paint(4.3);
                converted.renderer.write_png(fs::path(directory) / "framekeyboard-ja-live.png");
                click_key(converted, "Escape");
            }
        }
        std::cout
            << "Japanese composition, candidates, kana, shortcuts, gates and cancellation passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
