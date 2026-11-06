#include "framekeyboard/app.hpp"
#include "framekeyboard/emoji_words.hpp"
#include "framekeyboard/japanese_candidates.hpp"
#include "framekeyboard/mozc.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
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
    {
        // The English page's side keys sit where the kana page's do, so
        // switching pages moves none of them.
        const auto bounds = [&](const char* layout, const char* id) {
            const auto& keys = profiles.layouts.at(layout).keys;
            return std::find_if(keys.begin(), keys.end(), [&](const Key& key) { return key.id == id; })
                ->bounds;
        };
        for (const auto* id : {"Cut", "Copy", "Paste", "Dictation"}) {
            require(bounds("ja-flick-12", id) == bounds("ja-flick-qwerty", id),
                    "both flick pages keep the side keys in place");
        }
    }
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
    {
        // Flick only: taps never cycle, so a quick second tap types the kana again.
        char only[] = "/tmp/framekeyboard-flick-only-XXXXXX";
        require(mkdtemp(only), "temporary flick only profile");
        Cleanup removed{only};
        Settings settings;
        settings.active = {"ja-flick-12", "ja-flick", "graphite"};
        settings.flick_only = true;
        save_settings(only, settings);
        auto flick_only = options;
        flick_only.config_dir = only;
        Sink only_sink;
        App only_app(flick_only, only_sink);
        flick(only_app, "a", 1);
        flick(only_app, "a", 1.3);
        require(only_app.view().preedit == "ああ", "with Flick only, a quick second tap types あ again");
    }

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
    require(preedit() == "22¥" && sink.text.size() == typed + 2,
            "digits join the reading, as on a phone, without cycling, and flicks add their symbols");
    flick(app, "a", 55.6, 0, 60);
    flick(app, "sa", 55.8, 0, 60);
    require(preedit() == "22¥@_", "the down flicks give the symbols the English page has no keys for");
    click_key(app, "Enter");
    require(sink.text.back() == "22¥@_" && preedit().empty(), "Enter commits them");
    flick(app, "mode", 56, 0, -60);
    require(app.view().flick_latin, "flicking 「あA1」 up picks English");
    flick(app, "mode", 56.2, 60, 0);
    require(app.view().flick_digits, "flicking it right picks digits");
    flick(app, "mode", 56.4, -60, 0);
    require(app.view().composing, "flicking it left returns to kana");
    flick(app, "na", 57);
    require(preedit() == "な", "kana mode composes again");
    flick(app, "mode", 57.2, 60, 0);
    flick(app, "sa", 57.25);
    require(preedit() == "な3" && app.view().flick_digits && sink.text.back() == "22¥@_",
            "kana and digits share the reading");
    flick(app, "mode", 57.28, 0, -60);
    require(preedit() == "な3" && app.view().flick_latin && sink.text.back() == "22¥@_",
            "the English page keeps the reading open");
    click_key(app, "KeyA");
    click_key(app, "ShiftLeft");
    click_key(app, "KeyB");
    require(preedit() == "な3aB" && sink.text.back() == "22¥@_",
            "and its letters join it as typed, not as romaji");
    flick(app, "mode", 57.29, -60, 0);
    require(preedit() == "な3aB" && app.view().composing, "back on kana, the reading goes on");
    click_key(app, "Enter");
    require(sink.text.back() == "な3aB" && preedit().empty(), "Enter commits it all");
    flick(app, "mode", 57.292);
    flick(app, "mode", 57.294, -60, 0);
    sink.keys.clear();
    flick(app, "undo", 57.3);
    require(preedit().empty() && sink.keys.empty(), "switching modes ends ↶'s chance");

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
// A final kana split off a stretched word leaves the stretch on the preceding segment.
void stretched_boundaries() {
    struct Stretched : JapaneseConverter {
        bool split;
        std::vector<std::string>* learned;
        Stretched(bool initially_split, std::vector<std::string>* chosen)
            : split(initially_split), learned(chosen) {}
        std::vector<Segment> convert(const std::string& reading) override {
            if (reading == "すごーい") {
                return {{reading, {reading}}};
            }
            return split ? divided() : std::vector<Segment>{{reading, {"凄い", reading}}};
        }
        static std::vector<Segment> divided() {
            return {{"すご", {"凄", "すご"}}, {"い", {"い", "胃"}}};
        }
        std::vector<Segment> resize(int index, int delta) override {
            require(index == 0 && delta == -1, "shortening the stretched word splits its final kana");
            return divided();
        }
        void learn(const std::vector<std::string>& chosen) override { *learned = chosen; }
    };
    for (const bool split : {false, true}) {
        std::vector<std::string> learned;
        JapaneseComposer composer([&] { return std::make_unique<Stretched>(split, &learned); });
        composer.kana("すごーい");
        if (split) {
            composer.prepare_live();
            require(composer.live().front() == "すごーい",
                    "live conversion keeps the stretch before a separate final kana");
        }
        composer.convert();
        if (!split) {
            require(composer.preedit() == "すごーい", "a whole word keeps its stretch");
            composer.resize(-1);
        }
        require(composer.segment_count() == 2, "the final kana is a separate segment");
        const auto first = composer.candidates();
        require(first == std::vector<std::string>{"すごー", "凄ー", "スゴー"},
                "the preceding segment's reading and every candidate keep the stretch");
        composer.choose(1);
        require(composer.candidates() == std::vector<std::string>{"い", "胃", "イ"},
                "the final kana keeps its reading and candidates without a stretch");
        require(composer.preedit() == "凄ーい" && composer.commit_text() == "凄ーい",
                "a split stretched word shows and commits the stretch before the final kana");
        // word_choices() reports each segment's reading when it has a listed form.
        composer.set_preferred(
            [](const std::string& reading) { return std::vector<std::string>{reading}; });
        require(composer.word_choices() ==
                    std::vector<std::pair<std::string, std::string>>{{"すごー", "凄ー"}, {"い", "い"}},
                "the segment readings are すごー and い");
        composer.set_learning(true);
        composer.learn();
        require(learned == std::vector<std::string>{"凄ー", "い"},
                "learning receives valid choices from both stretched segments");
        composer.segment(-1);
        composer.choose(0);
        require(composer.preedit() == "すごーい" && composer.commit_text() == "すごーい",
                "the kana candidates join to the typed reading, including the stretch");
    }
}
// Resizing a later segment retains the earlier candidate list and its choice.
void resized_choices(bool shorter) {
    struct Choices : JapaneseConverter {
        bool shorter;
        std::vector<std::string>* learned;
        Choices(bool fewer, std::vector<std::string>* chosen) : shorter(fewer), learned(chosen) {}
        std::vector<Segment> convert(const std::string&) override {
            return {{"きょう", {"今日", "京", "強", "教"}}, {"てんき", {"天気"}}};
        }
        std::vector<Segment> resize(int, int) override {
            return {{"きょう", shorter ? std::vector<std::string>{"今日"}
                                       : std::vector<std::string>{"今日", "強", "京"}},
                    {"てん", {"天"}},
                    {"き", {"気"}}};
        }
        void learn(const std::vector<std::string>& chosen) override { *learned = chosen; }
    };
    std::vector<std::string> learned;
    JapaneseComposer composer([&] { return std::make_unique<Choices>(shorter, &learned); });
    composer.set_learning(true);
    composer.kana("きょうてんき");
    composer.convert();
    const auto candidates = composer.candidates();
    composer.cycle(shorter ? -1 : 1);
    const auto selected = composer.selected();
    const auto chosen = candidates.at(static_cast<std::size_t>(selected));
    composer.segment(1);
    composer.resize(-1);
    composer.segment(-1);
    require(composer.selected() < static_cast<int>(composer.candidates().size()),
            "an earlier choice stays within its candidate list after resize");
    require(composer.candidates().at(static_cast<std::size_t>(composer.selected())) == chosen,
            "an earlier choice keeps its text when resize reorders candidates");
    require(composer.candidates() == candidates && composer.selected() == selected,
            "an earlier segment retains all candidates and its selection");
    require(composer.preedit() == chosen + "天気" && composer.commit_text() == chosen + "天気",
            "a resized phrase shows and commits the chosen earlier text");
    composer.learn();
    require(learned == std::vector<std::string>{chosen, "天", "気"},
            "learning receives the retained choice and the resized segments");
}
void resized_shorter_choices() {
    resized_choices(true);
}
void resized_reordered_choices() {
    resized_choices(false);
}
// Boundary changes use the same conversion rules as a fresh phrase.
void resized_rules() {
    struct Rules : JapaneseConverter {
        std::vector<Segment> divided;
        explicit Rules(std::vector<Segment> result) : divided(std::move(result)) {}
        std::vector<Segment> convert(const std::string& reading) override {
            return {{reading, {reading}}};
        }
        std::vector<Segment> resize(int, int) override { return divided; }
    };
    using Segments = std::vector<JapaneseConverter::Segment>;
    for (const auto& [reading, split, expected] :
         std::vector<std::tuple<std::string, Segments, std::string>>{
             {"くださいまって",
              {{"ください", {"下さい", "ください"}}, {"まって", {"待って"}}},
              "ください待って"},
             {"ほしよねんも", {{"ほし", {"星"}}, {"よねんも", {"四年も", "4年も"}}}, "星4年も"},
             {"しゃしんをとって",
              {{"しゃしんを", {"写真を"}}, {"とって", {"取って", "撮って"}}},
              "写真を撮って"},
             {"からかさまって", {{"からかさ", {"傘"}}, {"まって", {"待って"}}}, "から傘待って"},
             {"にまんぽまで", {{"にまん", {"二万"}}, {"ぽまで", {"歩まで"}}}, "2万歩まで"}}) {
        JapaneseComposer composer([&] { return std::make_unique<Rules>(split); });
        composer.kana(reading);
        composer.convert();
        composer.resize(-1);
        require(composer.commit_text() == expected, "resized segments receive conversion rules");
        const auto earlier = composer.candidates();
        // Keep the first segment while the rules see it for a second time.
        composer.segment(1);
        composer.resize(-1);
        composer.segment(-1);
        require(composer.commit_text() == expected && composer.candidates() == earlier,
                "rules do not reorder or duplicate an earlier segment on another resize");
    }
}
// A completion replaces the output text, but ↶ restores what was typed.
void completion_reading() {
    int conversions = 0;
    JapaneseComposer composer(fake_converter(&conversions));
    composer.set_preferred([](const std::string& reading) {
        return reading == "おもしろ" ? std::vector<std::string>{"面白い"} : std::vector<std::string>{};
    });
    composer.kana("おもしろ");
    composer.prepare_live();
    require(composer.live().front() == "面白い" && composer.choose_live(0),
            "a completion replaces the whole reading");
    require(composer.commit_text() == "面白い" && composer.typed_reading() == "おもしろ",
            "a completion commits its text while retaining the typed reading");
    require(composer.reopen_reading() == "おもしろ", "a completion reopens the typed kana");
}
void completion_reopen() {
    char directory[] = "/tmp/framekeyboard-completion-reopen-XXXXXX";
    require(mkdtemp(directory), "temporary completion profile");
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
    flick(app, "a", 1, 0, 60);
    flick(app, "ma", 1.5, 0, 60);
    flick(app, "sa", 2, -60, 0);
    flick(app, "ra", 2.5, 0, 60);
    require(app.view().preedit == "おもしろ", "the completion's kana are typed");
    app.tick(3);
    app.tick(3.3);
    require(click_candidate(app, "面白い", 4) && sink.text.back() == "面白い",
            "the live completion commits through the fake sink");
    flick(app, "undo", 5);
    require(app.view().preedit == "おもしろ", "↶ restores the kana before the completion");
}
// An unavailable prediction provider must not discard a successful conversion.
void live_without_predictions() {
    struct Unavailable : FakeConverter {
        int* predictions;
        Unavailable(int* count, int* predicted) : FakeConverter(count), predictions(predicted) {}
        std::vector<std::string> predict(const std::string&) override {
            ++*predictions;
            throw std::runtime_error("prediction provider unavailable");
        }
    };
    int conversions = 0, predictions = 0;
    JapaneseComposer composer([&] { return std::make_unique<Unavailable>(&conversions, &predictions); });
    composer.set_learning(true);
    composer.kana("にほんご");
    composer.prepare_live();
    require(predictions == 1 && composer.live_current() && composer.live().front() == "〈にほ〉〈んご〉",
            "successful conversion stays live when predictions fail");
    composer.convert();
    require(conversions == 1 && composer.commit_text() == "〈にほ〉〈んご〉",
            "conversion reuses the live segments even without predictions");
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
    // A full-width symbol's half-width form follows it, offered or not.
    struct YenConverter : JapaneseConverter {
        std::vector<Segment> convert(const std::string&) override { return {{"￥", {"\\", "￥"}}}; }
        std::vector<Segment> resize(int, int) override { return convert(""); }
    };
    JapaneseComposer yen([] { return std::make_unique<YenConverter>(); });
    yen.kana("￥");
    yen.convert();
    require(yen.candidates() == std::vector<std::string>{"￥", "¥", "\\"}, "￥ is followed by ¥");
}
// Flick Katakana (JP): a key in the side column switches the kana to
// katakana, typed and shown, until it is switched back or the language or
// layout changes. Without the option the key is hidden.
void flick_katakana() {
    char directory[] = "/tmp/framekeyboard-katakana-XXXXXX";
    require(mkdtemp(directory), "temporary katakana profile");
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
    const auto shows_key = [](App& app) {
        const auto view = app.view();
        for (int y = 96; y < panel_height; y += 4) {
            for (int x = 0; x < panel_width; x += 4) {
                if (const auto* key = app.renderer.hit_key(view, x, y);
                    key && key->id == "Flick-script") {
                    return true;
                }
            }
        }
        return false;
    };
    {
        Sink sink;
        App app(options, sink);
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        require(!shows_key(app), "without Flick Katakana (JP), no script key");
    }
    Settings settings;
    settings.active = {"ja-flick-12", "ja-flick", "graphite"};
    settings.flick_katakana = true;
    save_settings(directory, settings);
    Sink sink;
    App app(options, sink);
    auto preedit = [&] { return app.view().preedit; };
    require(shows_key(app) && !app.view().flick_katakana, "with it, the script key shows, in hiragana");
    {
        const auto [x, y] = key_centre(app, "Flick-script");
        app.down(3, x, y, .5);
        require(!app.view().flick_preview, "held, the switch shows no flick cross");
        app.up(3, x, y);
        require(app.view().flick_katakana, "a tap switches to katakana");
    }
    flick(app, "a", 2);
    flick(app, "a", 2.3);
    require(preedit() == "イ", "katakana is typed, and quick taps cycle it");
    flick(app, "undo", 2.5);
    require(preedit() == "ア", "↶ steps it back through its key, still in katakana");
    flick(app, "ha", 4);
    flick(app, "mark", 4.2);
    require(preedit() == "アバ", "゛゜小 marks it");
    flick(app, "script", 5, -60, 0);
    flick(app, "ka", 5.5);
    require(!app.view().flick_katakana && preedit() == "アバか",
            "it is a switch: a press that moves away still switches it back");
    flick(app, "script", 6);
    require(app.view().flick_katakana, "and a tap switches again");
    // The digits page has no kana: the switch shows the script, disabled.
    flick(app, "mode", 7);
    flick(app, "mode", 7.5);
    require(app.view().flick_digits && app.view().flick_katakana &&
                app.view().disabled_keys.contains("Flick-script"),
            "on the digits page the switch shows katakana, disabled");
    {
        const auto [x, y] = key_centre(app, "Flick-script");
        require(!app.down(0, x, y, 8), "and takes no press there");
        app.up(0, x, y);
    }
    flick(app, "mode", 8.5);
    require(!app.view().flick_digits && app.view().flick_katakana &&
                !app.view().disabled_keys.contains("Flick-script"),
            "back on the kana page it is still katakana, and works");
    app.apply({"en-us-full", "en-us", "graphite"});
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    require(!app.view().flick_katakana, "a change of language returns it to hiragana");
    std::vector<std::string> errors;
    require(load_settings(directory, errors).flick_katakana, "the option is saved; the script isn't");
}
// A partly typed reading completes to common words: first while it is no
// word itself, after the conversion and learned words once it is.
void word_completion() {
    char directory[] = "/tmp/framekeyboard-words-XXXXXX";
    require(mkdtemp(directory), "temporary word list");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const auto file = fs::path(directory) / "words.tsv";
    std::ofstream(file) << "# reading\tword\tcost\n"
                           "あったかい\tあったかい\t2633\n"
                           "あり\tあり\t0\n"
                           "ありがとう\tありがとう\t646\n"
                           "ありがとうございます\tありがとうございます\t1086\n"
                           "おめでとう\tおめでとう\t1842\n"
                           "おめでとう\tお目出度う\t4071\n"
                           "おめでとうございます\tおめでとうございます\t2296\n"
                           "おもしろ\tおもしろ\t2836\n"
                           "おもしろい\t面白い\t1876\n"
                           "きみ\t君\t1855\n"
                           "きみたち\t君たち\t3469\n"
                           "きみょう\t奇妙\t1000\n"
                           "きょう\t今日\t121\n"
                           "きょうはく\t脅迫\t5003\n"
                           "なみだ\t涙\t4109\n"
                           "なみだする\t涙する\t3384\n"
                           "はな\t花\t3424\n"
                           "はなし\t話\t16\n";
    const JapaneseWords words(file);
    bool first = false;
    require(
        words.complete("おめでと", 3, first) ==
                std::vector<std::string>{"おめでとう", "おめでとうございます", "お目出度う"} &&
            first,
        "completions are the longer readings' words, commonest first, before a reading that is none");
    require(words.complete("ありがとう", 3, first) == std::vector<std::string>{"ありがとうございます"} &&
                !first,
            "a reading that is a commoner word than its completions comes before them");
    require(words.complete("ん", 3, first).empty() && !first, "nothing begins with ん");
    require(words.complete("きみ", 3, first) == std::vector<std::string>{"君たち"} && !first,
            "a small kana after the reading is another sound: きみ doesn't complete to きみょう");
    require(words.complete("きょうは", 3, first) == std::vector<std::string>{"脅迫"} && !first,
            "a word and a particle after it read as the word: きょうは leads 脅迫");
    require(words.complete("はな", 3, first) == std::vector<std::string>{"話"} && !first,
            "a word of two kana leads completions however common: 花 before 話");
    require(words.complete("ありが", 1, first) == std::vector<std::string>{"ありがとう"} && first,
            "a word and a particle read less surely than the word: ありが completes to ありがとう");
    require(words.complete("おもしろ", 1, first) == std::vector<std::string>{"面白い"} && first,
            "an adjective's stem completes to it first");
    require(words.complete("なみだ", 3, first) == std::vector<std::string>{"涙する"} && !first,
            "a word read so stays before a completion that is only a little commoner");
    require(words.complete("あった", 3, first) == std::vector<std::string>{"あったかい"} && !first,
            "a verb's past, as typed, before its completions: あった before あったかい");
    require(!words.completion_leads("しんだ", 0), "also in んだ: しんだ (死んだ) before 診断");
    bool none = true;
    require(JapaneseWords{}.complete("おめでと", 3, none).empty() && !none, "no list, no completions");

    int conversions = 0;
    JapaneseComposer composer(fake_converter(&conversions));
    composer.set_words(&words);
    composer.kana("おめでと");
    composer.prepare_live();
    require(composer.live().front() == "おめでとう" && composer.live()[1] == "おめでとうございます",
            "a reading that is no word yet completes first");
    require(composer.choose_live(1) && composer.commit_text() == "おめでとうございます",
            "a completion commits as the word");
    {
        // Romaji's nn is ん, as in Mozc; the words the reading makes with the
        // n carried over come first while it makes none, after it otherwise.
        std::ofstream(fs::path(directory) / "nn.tsv") << "げんいん\t原因\t1000\n"
                                                         "げんにん\t現任\t4000\n"
                                                         "こんにちは\tこんにちは\t1276\n";
        const JapaneseWords nn(fs::path(directory) / "nn.tsv");
        JapaneseComposer romaji(fake_converter(&conversions));
        romaji.set_words(&nn);
        for (const char c : std::string("konnichiha")) {
            romaji.roman(c);
        }
        romaji.prepare_live();
        require(romaji.preedit() == "こんいちは" && romaji.live().front() == "こんにちは",
                "konnichiha reads こんいちは and offers こんにちは first");
        require(romaji.choose_live(0) && romaji.commit_text() == "こんにちは",
                "which commits as itself");
        romaji.cancel();
        for (const char c : std::string("gennin")) {
            romaji.roman(c);
        }
        romaji.prepare_live();
        const auto live = romaji.live();
        const auto other = std::find(live.begin(), live.end(), "現任");
        require(romaji.preedit() == "げんいn" && live.front() == "〈げん〉〈いん〉" &&
                    other != live.end(),
                "gennin reads げんいん, a word, so 現任 follows its conversion");
        {
            struct Whole : JapaneseConverter {
                std::vector<Segment> convert(const std::string& reading) override {
                    return {{reading, {reading == "せんえん" ? "千円" : reading}}};
                }
                std::vector<Segment> resize(int, int) override { return {}; }
            };
            std::ofstream(fs::path(directory) / "sen.tsv") << "せんねん\t専念\t4843\n";
            const JapaneseWords sen(fs::path(directory) / "sen.tsv");
            JapaneseComposer yen([] { return std::make_unique<Whole>(); });
            yen.set_words(&sen);
            for (const char c : std::string("sennenn")) {
                yen.roman(c);
            }
            yen.prepare_live();
            require(yen.live().front() == "千円",
                    "a reading Anthy converts whole to a word leads the other reading's words");
        }
        romaji.cancel();
        romaji.kana("こんいちは");
        romaji.prepare_live();
        require(romaji.live().front() == "〈こん〉〈いち〉〈は〉", "kana typed so is meant so");
    }
    {
        // Common chat phrases complete a reading too, ranked as common words.
        const auto phrase_file = fs::path(directory) / "phrases.tsv";
        std::ofstream(phrase_file) << "いただきます\tいただきます\t0\n"
                                      "きょうはありがとう\t今日はありがとう\t0\n";
        const JapaneseWords phrases(phrase_file);
        std::ofstream(fs::path(directory) / "more.tsv") << "いただき\tいただき\t4757\n"
                                                           "きょう\t今日\t121\n";
        const JapaneseWords more(fs::path(directory) / "more.tsv");
        JapaneseComposer phrasing(fake_converter(&conversions));
        phrasing.set_words(&more);
        phrasing.set_phrases(&phrases);
        phrasing.kana("いただき");
        phrasing.prepare_live();
        require(phrasing.live().front() == "いただきます",
                "a phrase completes a rare word's reading first");
        phrasing.cancel();
        phrasing.kana("きょうは");
        phrasing.prepare_live();
        require(phrasing.live().front() == "〈きょ〉〈うは〉" &&
                    std::find(phrasing.live().begin(), phrasing.live().end(), "今日はありがとう") !=
                        phrasing.live().end(),
                "but follows a word and a particle, as a common word would");
        std::ofstream(fs::path(directory) / "chat.tsv") << "こんにちは\tこんにちは\t0\n"
                                                           "こんやあいてる\t今夜空いてる\t0\n";
        const JapaneseWords chat(fs::path(directory) / "chat.tsv");
        std::ofstream(fs::path(directory) / "common.tsv") << "こんにち\t今日\t785\n"
                                                             "こんにちは\tこんにちは\t1276\n"
                                                             "こんや\t今夜\t3613\n";
        const JapaneseWords common(fs::path(directory) / "common.tsv");
        JapaneseComposer chatting(fake_converter(&conversions));
        chatting.set_words(&common);
        chatting.set_phrases(&chat);
        chatting.kana("こんに");
        chatting.prepare_live();
        require(chatting.live().front() == "こんにちは",
                "a word the phrases list too ranks ahead: こんにちは before 今日");
        chatting.cancel();
        chatting.kana("こんや");
        chatting.prepare_live();
        require(chatting.live().front() == "〈こん〉〈や〉",
                "a phrase doesn't lead a common word: 今夜 before 今夜空いてる");
    }
    {
        // The conversion committed before, offered back as learned, leads.
        struct Learned : FakeConverter {
            using FakeConverter::FakeConverter;
            std::vector<std::string> predict(const std::string&) override {
                return {"〈おめ〉〈でと〉"};
            }
        };
        JapaneseComposer learner([&] { return std::make_unique<Learned>(&conversions); });
        learner.set_words(&words);
        learner.set_learning(true);
        learner.kana("おめでと");
        learner.prepare_live();
        require(learner.live().front() == "〈おめ〉〈でと〉" && learner.live()[1] == "おめでとう",
                "a learned pick of the conversion leads the completions");
    }
    // A reading converted as one word offers its other words first: here
    // the fake converter's second candidate, before 涙する.
    composer.cancel();
    composer.kana("なみだ");
    composer.prepare_live();
    require(composer.live().size() > 2 && composer.live()[0] == "〈なみ〉〈だ〉" &&
                composer.live()[1] == "涙する",
            "a reading of two segments keeps completions before the first segment's words");
    composer.cancel();
    composer.kana("きみ");
    composer.prepare_live();
    require(composer.live() == std::vector<std::string>{"〈きみ〉", "きみ", "キミ", "君たち"},
            "a one-word reading's other words come before its completions");
    composer.cancel();
    composer.kana("ありがとう");
    composer.prepare_live();
    require(composer.live().front() == "〈あり〉〈がと〉〈う〉" &&
                composer.live()[1] == "ありがとうございます",
            "a word converts first, its completions after");
    composer.cancel();
    composer.kana("お");
    composer.prepare_live();
    require(std::find(composer.live().begin(), composer.live().end(), "おめでとう") ==
                composer.live().end(),
            "a single kana completes nothing");

    // Kaomoji come last, for the reading as typed; かおもじ offers them all.
    const auto faces = fs::path(directory) / "kaomoji.tsv";
    std::ofstream(faces) << "かおもじ\t(^o^)\t1\n"
                            "かおもじ\t(T_T)\t9\n"
                            "しくしく\t(T_T)\t9\n"
                            "にこにこ\t(^o^)\t1\n";
    const JapaneseWords kaomoji(faces);
    require(kaomoji.exact("かおもじ", 6) == std::vector<std::string>{"(^o^)", "(T_T)"} &&
                kaomoji.exact("しく", 6).empty(),
            "kaomoji are found by their whole reading, in their own order");
    composer.set_kaomoji(&kaomoji);
    composer.cancel();
    composer.kana("しくしく");
    composer.prepare_live();
    require(composer.live().back() == "(T_T)", "a reading offers its kaomoji last");
    require(composer.choose_live(static_cast<int>(composer.live().size()) - 1) &&
                composer.commit_text() == "(T_T)" && composer.reopen_reading() == "しくしく",
            "a kaomoji commits in place of the reading, which ↶ brings back");
    composer.cancel();
    composer.kana("かおもじ");
    composer.prepare_live();
    const auto& live = composer.live();
    require(std::find(live.begin(), live.end(), "(^o^)") != live.end() &&
                std::find(live.begin(), live.end(), "(T_T)") != live.end(),
            "かおもじ offers them all");
    const JapaneseWords bundled(data_resource({}, "japanese/kaomoji.tsv"));
    require(bundled.exact("にこにこ", 1000).size() == 65 &&
                bundled.exact("にこにこ", 1).front() == "＼(^o^)／" &&
                bundled.exact("かお", 1000).size() > 400 && bundled.exact("かおもじ", 1000).size() > 400,
            "the bundled kaomoji come from Mozc's emoticons, every one under かお and かおもじ");
}
// A common phrase's usual conversion comes first and is what 変換 gives,
// until the user commits something else for it.
void preferred_phrases() {
    int conversions = 0;
    {
        JapaneseComposer composer(fake_converter(&conversions));
        composer.set_preferred([](const std::string& reading) {
            return reading == "いきます" ? std::vector<std::string>{"行きます"}
                                         : std::vector<std::string>{};
        });
        composer.kana("いきます");
        composer.prepare_live();
        require(composer.live().front() == "行きます" && composer.live()[1] == "〈いき〉〈ます〉",
                "a common phrase's usual conversion comes first");
        composer.convert();
        const auto candidates = composer.candidates();
        require(composer.segment_count() == 1 && candidates.front() == "行きます" &&
                    std::find(candidates.begin(), candidates.end(), "〈いき〉〈ます〉") !=
                        candidates.end(),
                "変換 converts to it as one segment, the converter's split among its candidates");
        require(composer.commit_text() == "行きます", "and commits it");
    }
    {
        // ← cuts a phrase's last kana off into a segment of its own, its
        // text cut so too; → joins them again.
        JapaneseComposer composer(fake_converter(&conversions));
        composer.set_preferred([](const std::string& reading) {
            return reading == "きょうはいいてんきだね" ? std::vector<std::string>{"今日はいい天気だね"}
                   : reading == "きょうのよてい"       ? std::vector<std::string>{"今日の予定"}
                                                       : std::vector<std::string>{};
        });
        composer.kana("きょうはいいてんきだね");
        composer.convert();
        composer.resize(-1);
        require(composer.segment_count() == 2 && composer.candidates().front() == "今日はいい天気だ" &&
                    composer.commit_text() == "今日はいい天気だね",
                "← keeps the phrase's text, the cut kana apart as written");
        composer.resize(-1);
        require(composer.candidates().front() == "今日はいい天気" &&
                    composer.commit_text() == "今日はいい天気だね" && composer.segment_count() == 2,
                "and cuts further");
        composer.resize(1);
        composer.resize(1);
        require(composer.segment_count() == 1 && composer.commit_text() == "今日はいい天気だね",
                "→ joins them back into the phrase");
        composer.cancel();
        composer.kana("きょうのよてい");
        composer.convert();
        composer.resize(-1);
        require(composer.commit_text() != "今日の予" && composer.segment_count() > 1,
                "a phrase not ending in the kana cut goes back to the converter's split");
    }
    char directory[] = "/tmp/framekeyboard-preferred-XXXXXX";
    require(mkdtemp(directory), "temporary preferred profile");
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
    options.japanese_converter = fake_converter(&conversions);
    Sink sink;
    App app(options, sink);
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    double now = 1;
    const auto type = [&] {
        for (const auto* key : {"KeyI", "KeyK", "KeyI", "KeyM", "KeyA", "KeyS", "KeyU"}) {
            click_key(app, key);
        }
        app.tick(now += 1);
        app.tick(now += 1);
    };
    const auto first = [&] { return app.view().candidates->chips.front().text; };
    type();
    require(first() == "行きます", "the bundled list's いきます comes first");
    require(click_candidate(app, "〈いき〉〈ます〉", now += .1) &&
                sink.text.back() == "〈いき〉〈ます〉",
            "something else can be chosen");
    type();
    require(first() == "〈いき〉〈ます〉" && app.view().candidates->chips.at(1).text == "行きます",
            "and once chosen, it wins over the list, whose phrase stays next");
    for (int i = 0; i < 2; ++i) {
        require(click_candidate(app, "行きます", now += .1),
                "the list's phrase picked from the live row");
        type();
    }
    require(first() == "行きます", "counts too: picked more often, it leads again");
    click_key(app, "Escape");

    // A word's form picked in a sentence is remembered for the word, and
    // leads it in later sentences.
    const auto horaiku = [&] {
        for (const auto* key : {"KeyH", "KeyO", "KeyR", "KeyA", "KeyI", "KeyK", "KeyU"}) {
            click_key(app, key);
        }
        app.tick(now += 1);
        app.tick(now += 1);
    };
    horaiku();
    require(first() == "〈ほら〉行く", "the sentence's word takes the list's form");
    require(click_candidate(app, "〈ほら〉", now += .1) && click_candidate(app, "〈いく〉", now += .1) &&
                sink.text.back() == "〈ほら〉〈いく〉",
            "another form can be picked for it");
    horaiku();
    require(first() == "〈ほら〉〈いく〉", "and then leads the word");
    click_key(app, "Escape");
    // Also in a sentence the list has, as すぐいく: いく picked for its いく
    // more often than 〈いく〉 leads the word elsewhere.
    for (int i = 0; i < 2; ++i) {
        for (const auto* key : {"KeyS", "KeyU", "KeyG", "KeyU", "KeyI", "KeyK", "KeyU"}) {
            click_key(app, key);
        }
        app.tick(now += 1);
        app.tick(now += 1);
        require(click_candidate(app, "〈すぐ〉", now += .1) && click_candidate(app, "いく", now += .1) &&
                    sink.text.back() == "〈すぐ〉いく",
                "a word picked in a listed sentence");
    }
    horaiku();
    require(first() == "〈ほら〉いく", "counts for the word too");
    click_key(app, "Escape");
    app.tick(now += .1); // The same reading typed again is a new one.
    // In a sentence the latest pick leads; typed on its own, the most picked
    // one does, as on the iPhone. いく is picked more often here, 〈いく〉 last.
    for (const auto* form : {"いく", "〈いく〉"}) {
        horaiku();
        require(click_candidate(app, "〈ほら〉", now += .1) && click_candidate(app, form, now += .1),
                "a word picked in a sentence");
    }
    horaiku();
    require(first() == "〈ほら〉〈いく〉", "in a sentence the latest pick leads");
    click_key(app, "Escape");
    for (const auto* key : {"KeyI", "KeyK", "KeyU"}) {
        click_key(app, key);
    }
    app.tick(now += 1);
    app.tick(now += 1);
    require(first() == "いく", "on its own the one picked most often");
}
// The candidates report types each phrase, in kana or romaji, and lists what
// the live candidates and 変換 offer for it; notes stay as they are.
void candidates_report() {
    int conversions = 0;
    std::istringstream phrases("# notes\nにほんご\nnun\n\n");
    std::ostringstream report;
    JapaneseCandidatesOptions options;
    options.converter = fake_converter(&conversions);
    options.live = 3;
    options.segment = 2;
    report_japanese_candidates(phrases, report, options);
    require(report.str() == "# notes\n"
                            "にほんご\n"
                            "  live (Anthy)  〈にほ〉〈んご〉 | 〈にほ〉 | にほ | … 3 more\n"
                            "  変換 (Anthy)  [〈にほ〉 | にほ | … 1 more] [〈んご〉 | んご | … 1 more]\n"
                            "nun → ぬん\n"
                            "  live (Anthy)  〈ぬん〉 | ぬん | ヌン\n"
                            "  変換 (Anthy)  [〈ぬん〉 | ぬん | … 1 more]\n"
                            "\n",
            "each phrase with its live candidates and segments, the engine that converted them, "
            "romaji as its reading");
}
// A converter that weighs the whole sentence, as Mozc does, keeps a particle
// it reads as part of a word: 京都旅行 stays, where Anthy's gives way.
void context_converter() {
    struct Sentence : JapaneseConverter {
        std::vector<Segment> convert(const std::string&) override {
            return {{"きょうと", {"京都"}}, {"りょこう", {"旅行"}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
        bool weighs_context() const override { return true; }
    };
    JapaneseComposer composer([] { return std::make_unique<Sentence>(); });
    composer.set_preferred([](const std::string& reading) {
        return reading == "きょう" ? std::vector<std::string>{"今日"} : std::vector<std::string>{};
    });
    composer.kana("きょうとりょこう");
    composer.prepare_live();
    require(composer.live().front() == "京都旅行", "a converter weighing the sentence keeps 京都");
}
// Mozc's server runs only while Japanese is among the user's languages,
// and Anthy converts whenever Mozc can't answer. A stand-in server that
// never answers shows both. It notes the path it was started as and its
// process, which stays the same through exec.
void mozc_lifecycle() {
    char directory[] = "/tmp/framekeyboard-mozc-XXXXXX";
    require(mkdtemp(directory), "temporary Mozc profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const auto server = fs::path(directory) / "mozc_server";
    std::ofstream(server) << "#!/bin/sh\necho \"$0\" > \"$XDG_CONFIG_HOME/mozc/started-as\"\n"
                          << "echo $$ > \"$XDG_CONFIG_HOME/mozc/pid\"\nexec sleep 600\n";
    fs::permissions(server, fs::perms::owner_all);
    // What the stand-in wrote, once it has.
    auto noted = [&](const char* name) {
        const auto path = fs::path(directory) / "config/mozc" / name;
        for (int i = 0; i < 200 && (!fs::exists(path) || fs::file_size(path) == 0); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::string text;
        std::getline(std::ifstream(path), text);
        fs::remove(path);
        return text;
    };
    int conversions = 0;
    Options options;
    options.config_dir = fs::path(directory) / "config";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.japanese_converter = fake_converter(&conversions);
    options.mozc_server = server;
    options.wait_for_japanese = true; // Mozc starts and stops on the worker.
    Sink sink;
    {
        App app(options, sink);
        app.apply({"en-us-full", "en-us", "graphite"});
        require(!mozc_running(), "without Japanese, no Mozc server runs");
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        require(mozc_running() && fs::is_directory(options.config_dir / "mozc"),
                "adding Japanese starts it, with its profile in the config folder");
        const auto started_as = noted("started-as");
        require(access("/usr/bin/bwrap", X_OK) != 0 || started_as == "/usr/lib/mozc/mozc_server",
                "in bubblewrap it runs where Mozc's own client accepts its server");
        // One that dies starts again within the tick's check, not at the
        // next conversion.
        const int first = std::stoi(noted("pid"));
        require(!mozc_needs_start(), "a running server needs nothing");
        kill(first, SIGKILL);
        // The VR thread's check sees it gone without reaping it.
        bool noticed = false;
        for (int i = 0; i < 200 && !(noticed = mozc_needs_start()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(noticed, "the check notices a server that went");
        for (int i = 0; i < 200 && mozc_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(!mozc_running(), "the stand-in is gone");
        app.tick(1000);
        require(mozc_running() && std::stoi(noted("pid")) != first,
                "the keyboard's tick starts it again");
        app.apply({"en-us-full", "en-us", "graphite"});
        require(!mozc_running(), "and removing it stops it");
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
    }
    require(!mozc_running(), "the keyboard's end stops it too");

    // A Mozc that doesn't answer leaves conversion to the fallback. Our words
    // become its user dictionary as it starts.
    const auto words = fs::path(directory) / "words.tsv";
    std::ofstream(words) << "# Words.\nようきゃ\t陽キャ\n";
    start_mozc(server, fs::path(directory) / "config", words);
    {
        std::ifstream file(fs::path(directory) / "config/mozc/user_dictionary.db", std::ios::binary);
        const std::string written(std::istreambuf_iterator<char>(file), {});
        require(written.find("ようきゃ") != std::string::npos &&
                    written.find("陽キャ") != std::string::npos &&
                    written.find("Words") == std::string::npos,
                "Mozc's user dictionary holds our words, not the comment");
    }
    require(!wait_for_mozc(std::chrono::milliseconds(100)), "the stand-in never answers");
    JapaneseComposer composer(
        [&] { return mozc_converter([&] { return fake_converter(&conversions)(); }); });
    composer.kana("にほんご");
    composer.convert();
    require(composer.commit_text() == "〈にほ〉〈んご〉" && composer.fell_back(),
            "Anthy, here the fake, converts meanwhile, and says so");
    // Debug mode's mark for that (Settings → Debug, on by default) is on the
    // live candidates, and only there; a release build never marks them.
#ifdef FRAMEKEYBOARD_DEBUG_MODE
    constexpr bool debug_build = true;
#else
    constexpr bool debug_build = false;
#endif
    for (const bool debug : {false, true}) {
        const bool mark = debug && debug_build;
        Options marked = options;
        marked.debug = debug;
        marked.mozc_server.clear(); // The stand-in above stays the server.
        marked.japanese_converter = [&] {
            return mozc_converter([&] { return fake_converter(&conversions)(); },
                                  std::chrono::milliseconds(50));
        };
        App app(marked, sink);
        app.apply({"en-us-full", "ja-romaji", "graphite"});
        click_key(app, "KeyN");
        click_key(app, "KeyI");
        app.tick(1);
        app.tick(2);
        const auto view = app.view();
        require(view.candidates && !view.candidates->chips.empty() && view.candidates->fallback == mark,
                mark ? "candidates Anthy converted are marked" : "unmarked without the option");
    }
    stop_mozc();
    require(!mozc_running(), "stopped on request");
    // One that ignores SIGTERM is killed within about a second, so the
    // keyboard's end never hangs on it. In bubblewrap, bubblewrap itself
    // goes on SIGTERM and takes the server with it; only an unsandboxed
    // server needs the kill.
    const auto deaf = fs::path(directory) / "deaf_server";
    std::ofstream(deaf) << "#!/bin/sh\ntrap '' TERM\nexec sleep 600\n";
    fs::permissions(deaf, fs::perms::owner_all);
    start_mozc(deaf, fs::path(directory) / "config");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(mozc_running(), "the deaf stand-in runs");
    const auto asked = std::chrono::steady_clock::now();
    stop_mozc();
    require(!mozc_running() && std::chrono::steady_clock::now() - asked < std::chrono::seconds(3),
            "a server ignoring SIGTERM is killed soon after");
}
// The stand-in Mozc server of tests/fake_mozc.cpp, which answers as Mozc
// does, in a profile of its own; the files it reads and writes there steer
// and show it.
struct FakeMozc {
    fs::path directory, config, profile;
    FakeMozc() {
        char name[] = "/tmp/framekeyboard-fake-mozc-XXXXXX";
        require(mkdtemp(name), "temporary Mozc profile");
        directory = name;
        config = directory / "config";
        profile = config / "mozc";
    }
    ~FakeMozc() {
        stop_mozc();
        fs::remove_all(directory);
    }
    void start() { start_mozc(FRAMEKEYBOARD_FAKE_MOZC, config); }
    // Lines in one of its files.
    int count(const char* name) const {
        std::ifstream file(profile / name);
        int lines = 0;
        for (std::string line; std::getline(file, line);) {
            ++lines;
        }
        return lines;
    }
    pid_t pid() const {
        std::string text;
        std::getline(std::ifstream(profile / "fake-pid"), text);
        return text.empty() ? -1 : std::stoi(text);
    }
    static auto converter(int* conversions) {
        return mozc_converter([conversions] { return fake_converter(conversions)(); });
    }
};
// Mozc drops a session unused for an hour, though it keeps running, and
// answers its commands with an error: a new session converts at once.
void mozc_lost_session() {
    FakeMozc mozc;
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)), "the stand-in answers");
    int conversions = 0;
    const auto converter = FakeMozc::converter(&conversions);
    require(converter->convert("あい").front().candidates.front() == "MOZC" && !converter->fell_back(),
            "Mozc converts");
    const int sessions = mozc.count("fake-creates");
    std::ofstream(mozc.profile / "fake-expire").close();
    const auto again = converter->convert("あい");
    require(!again.empty() && again.front().candidates.front() == "MOZC" && !converter->fell_back() &&
                conversions == 0,
            "after Mozc dropped the session, the next conversion is still Mozc's");
    const int renewed = mozc.count("fake-creates");
    require(renewed > sessions, "in a new session");
    require(converter->convert("あい").front().candidates.front() == "MOZC" &&
                mozc.count("fake-creates") == renewed,
            "which is kept");
    // So does one started again, as after a crash.
    const pid_t first = mozc.pid();
    kill(first, SIGKILL);
    for (int i = 0; i < 200 && mozc_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mozc.start(); // The keyboard's tick
    require(wait_for_mozc(std::chrono::seconds(5)) && mozc.pid() != first, "it starts again");
    const auto restarted = converter->convert("あい");
    require(!restarted.empty() && restarted.front().candidates.front() == "MOZC" &&
                !converter->fell_back(),
            "and the next conversion is Mozc's, in a new session");
}
// A Mozc that stops answering, as one stopped or stuck, holds the keyboard
// up only a moment: Anthy converts meanwhile, and the second time it is
// killed, to be started again. One that answers again is kept.
void mozc_stuck_server() {
    FakeMozc mozc;
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)), "the stand-in answers");
    int conversions = 0;
    const auto converter = FakeMozc::converter(&conversions);
    require(converter->convert("あい").front().candidates.front() == "MOZC", "Mozc converts");
    const auto timed = [&] {
        const auto start = std::chrono::steady_clock::now();
        converter->convert("あい");
        return std::chrono::steady_clock::now() - start;
    };
    const pid_t first = mozc.pid();
    kill(first, SIGSTOP);
    require(timed() < std::chrono::milliseconds(500) && converter->fell_back(),
            "a stuck Mozc holds a conversion up a moment, and Anthy converts");
    require(timed() < std::chrono::milliseconds(50) && converter->fell_back(),
            "the next conversion doesn't wait for it");
    kill(first, SIGCONT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    require(converter->convert("あい").front().candidates.front() == "MOZC" && !converter->fell_back() &&
                kill(first, 0) == 0,
            "one that answers again converts, and is kept");
    kill(first, SIGSTOP);
    timed();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    require(timed() < std::chrono::milliseconds(500) && converter->fell_back(),
            "a conversion is held up a moment again");
    for (int i = 0; i < 200 && kill(first, 0) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(kill(first, 0) != 0, "and the server, stuck, is killed");
    mozc.start(); // The keyboard's tick
    require(wait_for_mozc(std::chrono::seconds(5)) && mozc.pid() != first, "and started again");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    require(converter->convert("あい").front().candidates.front() == "MOZC" && !converter->fell_back(),
            "which converts");
}
// A Mozc that keeps dying soon after it starts is started again later each
// time, then not at all, until it is started anew, as when Japanese is
// added again; it stays in bubblewrap, which works.
void mozc_restarts() {
    FakeMozc mozc;
    fs::create_directories(mozc.profile);
    std::ofstream(mozc.profile / "fake-exit").close(); // It exits as it starts.
    std::vector<std::chrono::steady_clock::time_point> starts;
    const auto begin = std::chrono::steady_clock::now();
    mozc.start();
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(5000)) {
        mozc_running();
        mozc.start(); // The keyboard's tick, more often.
        for (int count = mozc.count("fake-starts"); static_cast<int>(starts.size()) < count;) {
            starts.push_back(std::chrono::steady_clock::now());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(starts.size() == 5, "a server that keeps dying is started a few times, then left");
    for (std::size_t i = 2; i < starts.size(); ++i) {
        require(starts[i] - starts[i - 1] >= std::chrono::milliseconds(400) * (1 << (i - 2)),
                "each time later");
    }
    std::ifstream file(mozc.profile / "fake-starts");
    for (std::string path; std::getline(file, path);) {
        require(access("/usr/bin/bwrap", X_OK) != 0 || path == "/usr/lib/mozc/mozc_server",
                "every time in bubblewrap");
    }
    fs::remove(mozc.profile / "fake-exit");
    mozc.start();
    require(!wait_for_mozc(std::chrono::milliseconds(200)), "a repeated start doesn't try it again");
    stop_mozc();
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)), "but one after stopping does");
}
// Clear learned words forgets what Mozc learned: Mozc is asked to, or one
// that doesn't answer is stopped and its profile removed. It says so only
// once it did, and the learned mark, which offers Clear, stays until then.
void mozc_clear_learning() {
    FakeMozc mozc;
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)), "the stand-in answers");
    const auto mark = mozc.profile / ".framekeyboard-learned";
    std::ofstream(mark).close();
    const pid_t first = mozc.pid();
    require(mozc_learned(mozc.config) && clear_mozc_learning(mozc.config) &&
                fs::exists(mozc.profile / "fake-cleared") && !mozc_learned(mozc.config) &&
                mozc_running() && mozc.pid() == first,
            "a Mozc that answers clears what it learned, and keeps running");
    std::ofstream(mark).close();
    fs::remove(mozc.profile / "fake-cleared");
    kill(first, SIGSTOP);
    const auto start = std::chrono::steady_clock::now();
    require(clear_mozc_learning(mozc.config) &&
                std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500),
            "a stuck one doesn't hold Clear up");
    require(!fs::exists(mozc.profile) && !mozc_learned(mozc.config) && !mozc_running(),
            "it is stopped, and its profile goes with what it learned");
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)) && mozc.pid() != first, "it starts again anew");
    // One whose profile can't go stays learned, and Settings says so.
    stop_mozc();
    std::ofstream(mark).close();
    fs::permissions(mozc.profile, fs::perms::owner_read | fs::perms::owner_exec);
    require(!clear_mozc_learning(mozc.config) && mozc_learned(mozc.config),
            "a profile that can't be removed stays learned");
    fs::permissions(mozc.profile, fs::perms::owner_all);
    Sink sink;
    Options options;
    options.config_dir = mozc.config;
    options.anthy_home = mozc.directory / "home";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    int conversions = 0;
    options.japanese_converter = fake_converter(&conversions);
    options.mozc_server = FRAMEKEYBOARD_FAKE_MOZC;
    options.wait_for_japanese = true; // Mozc starts and stops on the worker.
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    require(wait_for_mozc(std::chrono::seconds(5)), "adding Japanese starts it");
    kill(mozc.pid(), SIGSTOP);
    fs::permissions(mozc.profile, fs::perms::owner_read | fs::perms::owner_exec);
    app.show_settings();
    // Scrolls the Languages card until Clear shows.
    double now = 200;
    const auto clear = [&] {
        for (int step = 0; step < 20; ++step) {
            const auto controls = app.view().controls;
            for (const auto& c : controls) {
                if (c.id == "clear-learning") {
                    return c;
                }
            }
            const auto row = std::find_if(controls.begin(), controls.end(), [](const Control& c) {
                return c.id == "flick-only" || c.id == "key-flicks" || c.id == "swipe-typing" ||
                       c.id == "language";
            });
            require(row != controls.end(), "the Languages card shows");
            app.move(9, row->bounds.x + 20, row->bounds.y + 20);
            app.scroll(9, 0, 1);
            for (int i = 0; i < 10; ++i) {
                app.tick(now += .05);
            }
        }
        throw std::runtime_error("Missing control clear-learning");
    };
    require(clear().enabled, "Clear is offered");
    control(app, "clear-learning");
    control(app, "confirm-accept");
    for (int i = 0; i < 400 && clear().busy; ++i) {
        app.tick(now += .01);
        usleep(5000);
    }
    require(app.view().status == "Mozc's learned words could not be cleared." && clear().enabled,
            "Settings says Mozc's weren't cleared, and still offers Clear");
    fs::permissions(mozc.profile, fs::perms::owner_all);
}
// In bubblewrap, Mozc can write its profile, which it finds where its home
// and its config folder are ours, but nothing else in our config folder.
void mozc_sandbox_profile() {
    FakeMozc mozc;
    mozc.start();
    require(wait_for_mozc(std::chrono::seconds(5)) && fs::exists(mozc.profile / "fake-starts"),
            "the stand-in starts, with its profile in our config folder");
    require(access("/usr/bin/bwrap", X_OK) != 0 || !fs::exists(mozc.config / "fake-outside"),
            "and in bubblewrap it can't write beside it");
}
// What Mozc learns is saved once the user pauses, not at each phrase: 3 s
// after the last unless a key is pressed or the laser aimed meanwhile, which
// a hand's tremor isn't, and as the panel hides; nothing unlearned is.
void mozc_learning_saves() {
    FakeMozc mozc;
    Options options;
    options.config_dir = mozc.config;
    options.anthy_home = mozc.directory / "home";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.mozc_server = FRAMEKEYBOARD_FAKE_MOZC; // Mozc converts, Anthy meanwhile.
    options.wait_for_japanese = true; // Mozc starts and stops on the worker.
    Sink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    require(wait_for_mozc(std::chrono::seconds(5)), "adding Japanese starts it");
    double now = 100;
    app.tick(now);
    // A phrase Mozc converts and learns, committed at `now`.
    const auto commit = [&] {
        const int learned = mozc.count("fake-submits");
        flick(app, "a", now);
        click_key(app, "Space");
        click_key(app, "Enter");
        app.tick(now);
        require(sink.text.back() == "MOZC" && mozc.count("fake-submits") == learned + 1,
                "Mozc learns the phrase");
    };
    // Saves so far, once a save under way would have reached the stand-in.
    const auto saves = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return mozc.count("fake-syncs");
    };
    commit();
    require(saves() == 0, "a learned phrase isn't saved at once");
    const double committed = now;
    app.tick(now = committed + 2.9);
    require(saves() == 0, "nor while the user may go on");
    app.tick(now = committed + 3.1);
    require(saves() == 1, "but 3 s after it, with nothing else done");

    // The laser aimed from key to key, at 500 px/s, postpones it.
    commit();
    const double aimed = now;
    for (int step = 1; now < aimed + 4; ++step) {
        app.tick(now = aimed + step * .05);
        app.move(5, 300 + (step % 20) * 25, 400);
    }
    require(saves() == 1, "a laser aimed about postpones the save");
    const double still = now;
    app.tick(now = still + 2.9);
    require(saves() == 1, "until 3 s after it stopped");
    app.tick(now = still + 3.1);
    require(saves() == 2, "then it is saved");

    // A hand's tremor, shaking the laser 10 px about one spot, doesn't.
    commit();
    const double shaking = now;
    for (int step = 1; now < shaking + 3.5; ++step) {
        app.tick(now = shaking + step * .011);
        app.move(5, 800 + (step % 2 ? 5 : -5), 400 + (step % 3) * 3);
    }
    require(saves() == 3, "a laser shaking in place doesn't postpone it");

    // Hiding saves at once.
    commit();
    app.set_interaction_active(false);
    require(saves() == 4, "the panel hiding saves what was learned");
    app.set_interaction_active(true);
    app.tick(now += 10);
    require(saves() == 4, "and nothing is left to save after");

    // With learning off, as in a password field, nothing is learned, and
    // nothing saved.
    int conversions = 0;
    JapaneseComposer composer([&] { return FakeMozc::converter(&conversions); });
    composer.set_learning(false);
    composer.kana("あい");
    composer.convert();
    composer.learn();
    require(mozc_unsaved_learning() == 0, "learning off, Mozc learns nothing");
    app.set_interaction_active(false);
    app.tick(now += 10);
    require(saves() == 4, "so nothing is saved");
}
// The object decides a verb Mozc reads otherwise, from its own candidates:
// 雪|振ってきた is 雪降ってきた, 風|引いた 風邪引いた; elsewhere 振って stays.
void object_verbs() {
    struct Verbs : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            const Segment shake{"ふってきた", {"振ってきた", "降ってきた"}};
            if (reading == "ゆきふってきた") {
                return {{"ゆき", {"雪"}}, shake};
            }
            if (reading == "てふってきた") {
                return {{"て", {"手"}}, shake};
            }
            if (reading == "あめがやんだ") {
                return {{"あめが", {"雨が"}}, {"やんだ", {"病んだ", "止んだ"}}};
            }
            if (reading == "せきがでる") {
                return {{"せきが", {"席が", "咳が"}}, {"でる", {"出る"}}};
            }
            if (reading == "せきをでる") {
                return {{"せきを", {"席を", "咳を"}}, {"でる", {"出る"}}};
            }
            if (reading == "しゃしんはちょうどとった") {
                return {{"しゃしんは", {"写真は"}},
                        {"ちょうど", {"ちょうど"}},
                        {"とった", {"取った", "撮った"}}};
            }
            if (reading == "しゃしんとかさをとった") {
                return {
                    {"しゃしんと", {"写真と"}}, {"かさを", {"傘を"}}, {"とった", {"取った", "撮った"}}};
            }
            if (reading == "あせかいてあせかいた") {
                return {{"あせ", {"汗"}},
                        {"かいてあせかいた",
                         {"書いて汗書いた", "書いて汗かいた", "かいて汗書いた", "かいて汗かいた"}}};
            }
            if (reading == "せんえんかして") {
                return {{"せんえんかして", {"千円化して", "千円貸して"}}};
            }
            if (reading == "しあいはさんたいいちでかった") {
                return {{"しあいは", {"試合は"}},
                        {"3たい", {"3対"}},
                        {"1で", {"1で"}},
                        {"かった", {"買った", "勝った"}}};
            }
            if (reading == "いえについた" || reading == "いえにいるとつい") {
                return reading == "いえについた"
                           ? std::vector<Segment>{{"いえに", {"家に"}}, {"ついた", {"ついた", "着いた"}}}
                           : std::vector<Segment>{{"いえに", {"家に"}},
                                                  {"いると", {"いると"}},
                                                  {"つい", {"つい", "着い"}}};
            }
            if (reading == "かぜがはやってる") {
                return {{"かぜが", {"風が", "風邪が"}}, {"はやってる", {"流行ってる"}}};
            }
            if (reading == "かみをきった") {
                return {{"かみを", {"神を", "髪を"}}, {"きった", {"切った"}}};
            }
            return {{"かぜ", {"風", "風邪"}}, {"ひいた", {"引いた"}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Verbs>(); });
    const auto as = [&](const std::string& kana) {
        composer.cancel();
        composer.kana(kana);
        composer.convert();
        return composer.commit_text();
    };
    require(as("ゆきふってきた") == "雪降ってきた", "snow falls");
    require(as("てふってきた") == "手振ってきた", "a hand still waves");
    require(as("かぜひいた") == "風邪引いた", "a cold is caught");
    require(as("あめがやんだ") == "雨がやんだ", "rain stops, in kana");
    require(as("せきがでる") == "咳が出る", "a cough comes");
    require(as("せきをでる") == "席を出る", "a seat is still left");
    require(as("しゃしんはちょうどとった") == "写真はちょうど撮った", "across a word between");
    require(as("しゃしんとかさをとった") == "写真と傘を取った", "not across another object");
    require(as("せんえんかして") == "千円貸して", "in one segment");
    require(as("あせかいてあせかいた") == "汗かいて汗かいた",
            "a promoted in-segment verb also takes its preceding object's spelling");
    require(as("かみをきった") == "髪を切った", "hair is cut");
    require(as("しあいはさんたいいちでかった") == "試合は3対1で勝った", "a game is won");
    require(as("いえについた") == "家に着いた", "a place is reached");
    require(as("いえにいるとつい") == "家にいるとつい", "but only right before");
    require(as("かぜがはやってる") == "風邪が流行ってる", "a cold spreads");
}
// Words chat writes in kana come in kana where the converter offers them so:
// お伝えください, できる, よく, つらい; not in a noun (上出来) or after a
// kanji (仲良く), and 辛い read からい stays.
void chat_kana() {
    struct Formal : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            if (reading == "おつたえください") {
                return {{reading, {"お伝え下さい", "お伝えください"}}};
            }
            if (reading == "できる") {
                return {{reading, {"出来る", "できる"}}};
            }
            if (reading == "よくなった") {
                return {{reading, {"良くなった", "よくなった"}}};
            }
            if (reading == "なかよく") {
                return {{reading, {"仲良く", "仲よく"}}};
            }
            if (reading == "つらい") {
                return {{reading, {"辛い", "つらい"}}};
            }
            if (reading == "からい") {
                return {{reading, {"辛い", "からい"}}};
            }
            if (reading == "ひまなとき") {
                return {{reading, {"暇なとき", "暇な時"}}};
            }
            if (reading == "とき") {
                return {{reading, {"とき", "時"}}};
            }
            if (reading == "うちのいぬ") {
                return {{"うちの", {"家の", "うちの"}}, {"いぬ", {"犬"}}};
            }
            if (reading == "どこにおいた" || reading == "とっておいた") {
                return {{reading.substr(0, reading.size() - std::string("おいた").size()),
                         {reading == "どこにおいた" ? "どこに" : "取って"}},
                        {"おいた", {"おいた", "置いた"}}};
            }
            if (reading == "まったほうがいい" || reading == "しないほうがいい") {
                return {{reading.substr(0, reading.size() - std::string("ほうがいい").size()),
                         {reading == "まったほうがいい" ? "待った" : "しない"}},
                        {"ほうがいい", {"ほうがいい", "方がいい"}}};
            }
            if (reading == "15ふんおき") { // じゅうごふんおき, in digits.
                return {{"15", {"15"}}, {"ふん", {"分"}}, {"おき", {"起き", "おき"}}};
            }
            if (reading == "ごーるでんうぃーく") {
                return {{reading, {"GW", "ゴールデンウィーク"}}};
            }
            if (reading == "あめだからかさ") {
                return {{"あめだ", {"雨だ"}}, {"からかさ", {"傘", "唐傘"}}};
            }
            if (reading == "やっとねたから") {
                return {{"やっと", {"やっと"}}, {"ねたから", {"ネタから", "寝たから"}}};
            }
            return {{reading, {"上出来", "上でき"}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Formal>(); });
    const auto as = [&](const std::string& kana) {
        composer.cancel();
        composer.kana(kana);
        composer.convert();
        return composer.commit_text();
    };
    require(as("おつたえください") == "お伝えください", "ください in kana");
    require(as("できる") == "できる", "できる in kana");
    require(as("じょうでき") == "上出来", "a noun keeps its kanji");
    require(as("よくなった") == "よくなった", "よく in kana");
    require(as("なかよく") == "仲良く", "but not after a kanji");
    require(as("つらい") == "つらい", "つらい in kana");
    require(as("からい") == "辛い", "からい keeps its kanji");
    require(as("ひまなとき") == "暇な時", "時 after a clause");
    require(as("とき") == "とき", "とき alone stays");
    require(as("うちのいぬ") == "うちの犬", "うち as read");
    require(as("どこにおいた") == "どこに置いた", "置いた");
    require(as("とっておいた") == "取っておいた", "but 〜ておいた in kana");
    require(as("まったほうがいい") == "待った方がいい", "方がいい after た");
    require(as("しないほうがいい") == "しないほうがいい", "not after ない");
    require(as("やっとねたから") == "やっと寝たから", "寝た ending a clause");
    require(as("じゅうごふんおき") == "15分おき", "おき after a time");
    require(as("ごーるでんうぃーく") == "ゴールデンウィーク", "a word spelled out");
    require(as("あめだからかさ") == "雨だから傘", "から kept before 傘");
}
// A typed word leads its completions where they are no adjective of it
// (協会 is no completion of a stem きょうか), and a word the list lacks
// that the converter reads whole leads all but clearly commoner ones.
void whole_words_lead() {
    char directory[] = "/tmp/framekeyboard-whole-XXXXXX";
    require(mkdtemp(directory), "temporary word list");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::ofstream(fs::path(directory) / "words.tsv")
        << "おもしろ\tおもしろ\t2836\nおもしろい\t面白い\t1876\nきょうか\t強化\t3419\n"
           "きょうかい\t協会\t3967\nねんしき\t年式\t4800\n"; // Sorted by reading.
    const JapaneseWords words(fs::path(directory) / "words.tsv");
    struct Words : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            return {{reading,
                     {reading == "きょうか" ? "強化"
                      : reading == "ねんし" ? "年始"
                                            : reading}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Words>(); });
    composer.set_words(&words);
    const auto first = [&](const std::string& kana) {
        composer.cancel();
        composer.kana(kana);
        composer.prepare_live();
        return composer.live().front();
    };
    require(first("きょうか") == "強化", "a word leads a completion that is no adjective of it");
    require(first("ねんし") == "年始", "a word the list lacks leads a rarer completion");
    require(first("おもしろ") == "面白い", "an adjective's stem still completes to it first");
}
// Numbers spelled in kana before a counter go to the converter as digits,
// which Mozc converts as chat writes them (30分, 1万円); a digit and counter
// that make a word, or a に that is a particle, stay as typed.
void spelled_numbers() {
    struct Echo : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            return {{reading, {reading}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Echo>(); });
    const auto as = [&](const std::string& kana) {
        composer.cancel();
        composer.kana(kana);
        composer.convert();
        return composer.commit_text();
    };
    for (const auto& [kana, sent] : std::vector<std::pair<std::string, std::string>>{
             {"さんじゅっぷん", "30ぷん"},
             {"いちまんえん", "1まんえん"},
             {"にまんごせんえん", "2まん5000えん"},
             {"じゅうまん", "10まん"},
             {"はっぴゃくえん", "800えん"},
             {"さんぜんえん", "3000えん"},
             {"じゅうがつはつか", "10がつ20にち"},
             {"あとさんぷん", "あと3ぷん"},
             {"あとにじゅっぷん", "あと20ぷん"},
             {"よじにあおう", "4じにあおう"},
             {"しがつ", "4がつ"},
             {"ごじゅっこ", "50こ"},
             {"ぴざをにまい", "ぴざを2まい"},
             {"よるくじに", "よる9じに"},
             {"さんじゅうにん", "30にん"},
             {"ごせんにん", "5000にん"},
             {"ゆうがたごじ", "ゆうがた5じ"},
             {"まいばんじゅうじに", "まいばん10じに"},
             {"ごじごじゅっぷん", "5じ50ぷん"},
             {"かいぎはにじからさんじ", "かいぎは2じから3じ"},
             {"にせんにじゅうろくねん", "2026ねん"},
             {"ことしはにせんにじゅうろくねん", "ことしは2026ねん"},
             {"もうよねん", "もう4ねん"},
             {"よんひゃく", "400"},
             {"いってんごばい", "1.5ばい"},
             {"ぜろきゅうぜろから", "090から"},
             {"いちいちきゅうばん", "119ばん"},
             {"じゅうよっかに", "14にちに"},
             {"にひゃくいちごうしつ", "201ごうしつ"},
             {"さんがいの", "3がいの"},
             {"せんぴーす", "1000ぴーす"},
             {"かようびのよじに", "かようびの4じに"},
             {"にほんからよじかん", "にほんから4じかん"},
             {"えいがはくじから", "えいがは9じから"},
             {"ごがつのみっかからいつかまで", "5がつの3にちから5にちまで"},
             {"いつかのごごさんじ", "5にちのごご3じ"},
             {"ひゃくてんまんてん", "100てんまんてん"},
             {"じゅってん", "10てん"},
             {"れいてんさんびょう", "0.3びょう"},
             {"こみでにまんさんぜんえん", "こみで2まん3000えん"},
             {"しんどさんだった", "しんど3だった"},
         }) {
        require(as(kana) == sent, (kana + " converts as " + sent).c_str());
    }
    for (const auto* kana : {"いちばん",
                             "いっぱい",
                             "にほん",
                             "みなさんほんとうに",
                             "けっかにまんぞく",
                             "しこみ",
                             "よこはま",
                             "しじゅうくにち",
                             "ごしゅうぎ",
                             "せんもん",
                             "きゅうじつ",
                             "じゅうぶん",
                             "まいかいちこく",
                             "ろっこつ",
                             "にくまん",
                             "こんびににかいもの",
                             "ねっとにかいた",
                             "よるにじこ",
                             "よせん",
                             "ふくせん",
                             "ろくせん",
                             "にてんさんてん",
                             "ふつかよい",
                             "よっかいち",
                             "にじゅうにはらった",
                             "べんきょうにせんねん",
                             "とこにひゃくえん",
                             "ごじゅうのとう",
                             "にはつかない",
                             "いちがいに",
                             "はっぴょう",
                             "じっぱー",
                             "ひゃくにんいっしゅ",
                             "せんねんする",
                             "くじにあたった",
                             "いつかいきたい",
                             "よじのぼる"}) {
        require(as(kana) == kana, (std::string(kana) + " stays as typed").c_str());
    }
}
// Numbers Mozc reads as kanji numerals come in digits where it offers them:
// 四年も as 4年も, 三件 as 3件, 第二回 as 第2回; not words (一度, 一体), nor
// numerals that are no spelled number (五 in 五重塔).
void numeral_digits() {
    struct Numerals : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            static const std::map<std::string, std::vector<Segment>> segments{
                {"ほしよねんも", {{"ほし", {"星"}}, {"よねんも", {"四年も", "４年も", "4年も"}}}},
                {"さんけん", {{"さんけん", {"三件", "3件"}}}},
                {"だいにかい", {{"だいにかい", {"第二回", "第2回"}}}},
                {"やくひゃくにん", {{"やく", {"約"}}, {"ひゃくにん", {"百人", "100人"}}}},
                {"いちど", {{"いちど", {"一度", "1度"}}}},
                {"いったい", {{"いったい", {"一体", "1体"}}}},
                {"ごじゅうのとう", {{"ごじゅうのとう", {"五重塔", "5重塔"}}}},
                {"さんたいいちで", {{"さんたい", {"3対"}}, {"いちで", {"一で", "1で"}}}},
                {"38どごぶ", {{"38", {"38"}}, {"ど", {"度"}}, {"ごぶ", {"五分", "5分"}}}},
                {"いちにちにまんぽ",
                 {{"いちにち", {"1日"}}, {"にまん", {"二万", "にまん"}}, {"ぽ", {"歩"}}}},
                {"6かいのえれべーたーで",
                 {{"6", {"6"}}, {"かいの", {"回の", "階の"}}, {"えれべーたーで", {"エレベーターで"}}}},
                {"6かいのしあい", {{"6", {"6"}}, {"かいの", {"回の", "階の"}}, {"しあい", {"試合"}}}},
            };
            const auto found = segments.find(reading);
            return found != segments.end() ? found->second : std::vector<Segment>{{reading, {reading}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Numerals>(); });
    const auto as = [&](const std::string& kana) {
        composer.cancel();
        composer.kana(kana);
        composer.convert();
        return composer.commit_text();
    };
    require(as("ほしよねんも") == "星4年も", "a counted numeral in digits");
    require(as("さんけん") == "3件", "with a counter only Mozc reads so");
    require(as("だいにかい") == "第2回", "after 第");
    require(as("やくひゃくにん") == "約100人", "a unit alone");
    require(as("いちど") == "一度", "一度 stays");
    require(as("いったい") == "一体", "一体 stays");
    require(as("ごじゅうのとう") == "五重塔", "a numeral of another number stays");
    require(as("さんたいいちで") == "3対1で", "a score's second number");
    require(as("さんじゅうはちどごぶ") == "38度5分", "a temperature's 分");
    require(as("いちにちにまんぽ") == "1日2万歩", "2万 where only 二万 is offered");
    require(as("ろっかいのえれべーたーで") == "6階のエレベーターで", "a floor");
    require(as("ろっかいのしあい") == "6回の試合", "times stay times");
}
// Romaji's nn is ん, but 変換 converts the reading with the n carried over
// where Anthy splits that one into fewer segments: minna as みんな, not
// 民|亜. On a tie the reading stays as typed: gennin is げんいん.
void carried_conversion() {
    struct Carried : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            if (reading == "みんあ") {
                return {{"みん", {"民"}}, {"あ", {"亜"}}};
            }
            if (reading == "じかんある") {
                return {{"じかん", {"時間"}}, {"ある", {"ある"}}};
            }
            if (reading == "じかんなる") {
                return {{"じかんなる", {"時間なる"}}};
            }
            if (reading == "みんな") {
                return {{"みんな", {"皆", "みんな"}}};
            }
            if (reading == "3にんくる") { // さんにんくる, its number in digits.
                return {{"3にん", {"3人"}}, {"くる", {"来る"}}};
            }
            if (reading == "さんいんくる" || reading == "さんにんくる") {
                return {{reading.substr(0, reading.size() - std::string("くる").size()),
                         {reading == "さんいんくる" ? "参院" : "三人"}},
                        {"くる", {"来る"}}};
            }
            return {{reading, {"〈" + reading + "〉"}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Carried>(); });
    // The carried n wins only where it makes words: みんな is one.
    char directory[] = "/tmp/framekeyboard-carried-XXXXXX";
    require(mkdtemp(directory), "temporary word list");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::ofstream(fs::path(directory) / "words.tsv") << "みんな\tみんな\t2000\n";
    const JapaneseWords words(fs::path(directory) / "words.tsv");
    composer.set_words(&words);
    const auto romaji = [&](const std::string& letters) {
        composer.cancel();
        for (const char c : letters) {
            composer.roman(c);
        }
        composer.convert();
        return composer.commit_text();
    };
    require(romaji("minna") == "皆" && composer.reading() == "みんあ",
            "the carried reading converts, the typed one stays the reading");
    require(romaji("jikannaru") == "時間ある",
            "not where it makes no word: jikannaru is 時間ある, not 時間なる");
    require(romaji("gennin") == "〈げんいん〉" && composer.candidates().at(1) == "〈げんにん〉",
            "on a tie, the reading as typed, the other's word second");
    composer.cancel();
    for (const char c : std::string("sanninkuru")) {
        composer.roman(c);
    }
    composer.prepare_live();
    require(composer.live().at(0) == "参院来る" && composer.live().at(1) == "3人来る",
            "in a sentence too, the other whole phrase second");
    composer.convert();
    require(composer.candidates().at(1) == "3人", "and each segment it reads otherwise");
    composer.cancel();
    composer.kana("みんあ");
    composer.convert();
    require(composer.commit_text() == "民亜", "kana typed so are meant so");
}
// Number rewriting changes character offsets before carried-n word checks.
void carried_number_boundaries() {
    struct Numbered : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            std::vector<Segment> result;
            std::string rest;
            if (reading.starts_with("2026ねん")) {
                result = {{"2026", {"2026"}}, {"ねん", {"年"}}};
                rest = reading.substr(std::string("2026ねん").size());
            } else if (reading.starts_with("にせんい16ねん")) {
                result = {{"にせんい", {"二千位"}}, {"16", {"16"}}, {"ねん", {"年"}}};
                rest = reading.substr(std::string("にせんい16ねん").size());
            } else {
                return {{reading, {reading}}};
            }
            if (rest == "みんな") {
                result.push_back({rest, {"皆"}});
            } else if (rest == "みんあ") {
                result.push_back({"みん", {"民"}});
                result.push_back({"あ", {"亜"}});
            } else if (rest == "じかんなる") {
                result.push_back({rest, {"時間なる"}});
            } else if (rest == "じかんある") {
                result.push_back({"じかん", {"時間"}});
                result.push_back({"ある", {"ある"}});
            }
            return result;
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Numbered>(); });
    composer.set_preferred([](const std::string& reading) {
        return reading == "みんな" ? std::vector<std::string>{"皆"} : std::vector<std::string>{};
    });
    const auto as = [&](const std::string& roman) {
        composer.cancel();
        for (const char c : roman) {
            composer.roman(c);
        }
        composer.convert();
        return composer.commit_text();
    };
    require(as("nisennijuurokunen") == "2026年",
            "a carried number wins when its rewritten reading makes fewer segments");
    require(as("nisennijuurokunenminna") == "2026年皆",
            "a carried word after a rewritten number uses its new character offset");
    require(as("nisennijuurokunenjikannaru") == "二千位16年時間ある",
            "a nonword after a rewritten number still rejects the carried reading");
}
// A reading typed in katakana, as the flick layout's ア mode types it, is
// offered as typed first; a mixed one keeps the usual order.
void typed_katakana() {
    int conversions = 0;
    JapaneseComposer composer(fake_converter(&conversions));
    composer.kana("ガッ");
    composer.prepare_live();
    require(composer.live().front() == "ガッ" && composer.live()[1] == "〈ガッ〉",
            "typed katakana comes first, then the conversion");
    composer.cancel();
    composer.kana("がッ");
    composer.prepare_live();
    require(composer.live().front() == "〈がッ〉", "a mixed reading converts first");
}
// A word's usual form from the phrase list leads its segment, before a
// particle too, and a kana word Anthy left after に, へ, で or と takes it;
// a helper verb after て stays kana.
void word_forms() {
    struct Segments : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            if (reading == "すぐいくね") {
                return {{"すぐ", {"直ぐ", "すぐ"}}, {"いくね", {"育ね", "いくね"}}};
            }
            if (reading == "すぐいく") {
                return {{"すぐ", {"直ぐ", "すぐ"}}, {"いく", {"育", "行く"}}};
            }
            if (reading == "たべにいこう") {
                return {{"たべにいこう", {"食べにいこう"}}};
            }
            if (reading == "いくから") {
                return {{"いくから", {"育から", "いくから"}}};
            }
            if (reading == "ごはんできた") {
                return {{"ごはんできた", {"ご飯できた"}}};
            }
            if (reading == "いくよ") {
                return {{"いくよ", {"幾夜", "行くよ"}}};
            }
            if (reading == "うんがいい") {
                return {{"うんが", {"運が", "うんが"}}, {"いい", {"良い", "いい"}}};
            }
            if (reading == "いいじゃん") {
                return {{"いいじゃん", {"飯井じゃん", "いいじゃん"}}};
            }
            if (reading == "がっこうからかえる") {
                return {{"がっこう", {"学校"}}, {"からかえる", {"からかえる"}}};
            }
            if (reading == "からかう") {
                return {{"からかう", {"からかう"}}};
            }
            if (reading == "はいっていい") {
                return {{"はいって", {"入って", "はいって"}}, {"いい", {"いい"}}};
            }
            if (reading == "ばしょであう") {
                return {{"ばしょ", {"場所"}}, {"であう", {"出会う"}}};
            }
            if (reading == "であう") {
                return {{"であう", {"出会う"}}};
            }
            if (reading == "しごとであった") {
                return {{"しごとであった", {"仕事であった"}}};
            }
            if (reading == "いいよー") {
                return {{"い", {"意"}}, {"いよー", {"伊予ー"}}};
            }
            if (reading == "いいよ") {
                return {{"いいよ", {"いいよ"}}};
            }
            if (reading == "かれー") {
                return {{"かれー", {"カレー", "枯れー"}}};
            }
            if (reading == "くるまで") {
                return {{"くるまで", {"車で"}}};
            }
            if (reading == "いってきまーす") {
                return {{"いって", {"言って"}}, {"きまーす", {"来マーす"}}};
            }
            if (reading == "いってきます") {
                return {{"いってきます", {"言ってきます"}}};
            }
            if (reading == "きょうとりょこう") {
                return {{"きょうと", {"京都"}}, {"りょこう", {"旅行"}}};
            }
            if (reading == "ほんけんについて") {
                return {{"ほんけんについて", {"本件について"}}};
            }
            if (reading == "ばいばーい") {
                return {{"ばい", {"倍"}}, {"ばー", {"バー"}}, {"い", {"意"}}};
            }
            if (reading == "ばいばい") {
                return {{"ばいばい", {"倍々", "ばいばい", "バイバイ"}}};
            }
            if (reading == "かわいー") {
                return {{"かわいー", {"河合ー"}}};
            }
            if (reading == "かわい") {
                return {{"かわい", {"河合"}}};
            }
            if (reading == "でーと") {
                return {{"でー", {"デー"}}, {"と", {"と"}}};
            }
            if (reading == "こーら") {
                return {{"こーら", {"コーラ"}}};
            }
            if (reading == "えきでであった") {
                return {{"えきで", {"駅で"}}, {"であった", {"出会った"}}};
            }
            if (reading == "がっこうであったはなし") {
                return {{"がっこうであった", {"学校であった"}}, {"はなし", {"話"}}};
            }
            if (reading == "くるまでいく") {
                return {{"くるまで", {"車で", "来るまで"}}, {"いく", {"育", "行く"}}};
            }
            if (reading == "じかんがあう" || reading == "あしたあう") {
                const auto first = reading.substr(0, reading.size() - std::string("あう").size());
                return {{first, {first == "じかんが" ? "時間が" : "明日"}}, {"あう", {"合う", "会う"}}};
            }
            return {{reading, {"持っていく"}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<Segments>(); });
    // The word list has 車, so Anthy's 車で for くるまで stays.
    char directory[] = "/tmp/framekeyboard-forms-XXXXXX";
    require(mkdtemp(directory), "temporary word list");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    std::ofstream(fs::path(directory) / "words.tsv") << "からかう\tからかう\t3882\n"
                                                        "かわいい\t可愛い\t1575\n"
                                                        "くるま\t車\t3659\n"
                                                        "こーらす\tコーラス\t3000\n"
                                                        "でーと\tデート\t4206\n";
    const JapaneseWords words(fs::path(directory) / "words.tsv");
    composer.set_words(&words);
    composer.set_preferred([](const std::string& reading) {
        const std::map<std::string, std::string> forms{{"いく", "行く"},
                                                       {"いこう", "行こう"},
                                                       {"きた", "来た"},
                                                       {"くる", "来る"},
                                                       {"あう", "会う"},
                                                       {"〜があう", "〜が合う"},
                                                       {"うん", "うん"},
                                                       {"いい", "いい"},
                                                       {"かえる", "帰る"},
                                                       {"かう", "買う"},
                                                       {"はい", "はい"},
                                                       {"〜であう", "〜で会う"},
                                                       {"〜であった", "〜で会った"},
                                                       {"いってきます", "行ってきます"},
                                                       {"ついて", "着いて"},
                                                       {"きょう", "今日"}};
        const auto form = forms.find(reading);
        return form != forms.end() ? std::vector<std::string>{form->second} : std::vector<std::string>{};
    });
    const auto whole = [&](const char* reading) {
        composer.cancel();
        composer.kana(reading);
        composer.prepare_live();
        return composer.live().front();
    };
    require(whole("すぐいく") == "直ぐ行く", "a segment's word takes its usual form first");
    require(whole("すぐいくね") == "直ぐ行くね", "before a particle too");
    require(whole("たべにいこう") == "食べに行こう", "a kana word after に takes it within a segment");
    require(whole("もっていく") == "持っていく", "a helper verb after て stays kana");
    require(whole("いくから") == "行くから", "longer particles too: から");
    require(whole("ごはんできた") == "ご飯できた", "not after で, which できる begins with");
    require(whole("くるまでいく") == "車で行く",
            "nor before a particle Anthy reads as part of a common word: 車で, not 来るまで");
    require(whole("いくよ") == "行くよ", "a rare one gives way: 行くよ, not 幾夜");
    require(whole("うんがいい") == "運がいい", "a kana form not before a particle a noun takes: 運が");
    require(whole("いいじゃん") == "いいじゃん", "but before one that ends a sentence");
    require(whole("じかんがあう") == "時間が合う" && whole("あしたあう") == "明日会う",
            "after a case particle, the list's form after it: が合う, else 会う");

    // What each such word was committed as, without its particle, which the
    // app remembers as the user's choice.
    using Choices = std::vector<std::pair<std::string, std::string>>;
    const auto converted = [&](const char* reading) {
        composer.cancel();
        composer.kana(reading);
        composer.convert();
    };
    converted("すぐいくね");
    require(composer.word_choices() == Choices{{"いく", "行く"}},
            "a word as committed, without its particle");
    composer.segment(1);
    composer.cycle(1);
    require(composer.commit_text() == "直ぐ育ね" && composer.word_choices() == Choices{{"いく", "育"}},
            "or as picked instead");
    converted("たべにいこう");
    require(composer.word_choices() == Choices{{"いこう", "行こう"}}, "a word after に too");
    composer.cycle(1);
    require(composer.commit_text() == "食べにいこう" &&
                composer.word_choices() == Choices{{"いこう", "いこう"}},
            "kept in kana");
    converted("もっていく");
    require(composer.word_choices().empty(), "a helper verb after て is no such word");
    converted("じかんがあう");
    require(composer.word_choices() == Choices{{"〜があう", "〜が合う"}},
            "a form after a particle with its particle");

    require(whole("がっこうからかえる") == "学校から帰る", "a kana word after から takes its form");
    require(whole("からかう") == "からかう", "but not in a word of its own: からかう");
    require(whole("はいっていい") == "入っていい", "a kana form gives way before って: 入って");
    require(whole("ばしょであう") == "場所で会う" && whole("であう") == "出会う",
            "a context entry marked 〜 applies after a segment, from the particle that begins it");
    require(whole("しごとであった") == "仕事で会った", "and inside one, after the particle");
    require(whole("いいよー") == "いいよー",
            "a word stretched with ー converts as the word, the ー kept");
    require(whole("かれー") == "カレー", "but Anthy's katakana word stays: カレー");
    require(whole("ばいばーい") == "ばいばーい", "also a ー before a final い, in what can take it");
    require(whole("かわいー") == "かわいー" && composer.live().at(1) == "可愛ー",
            "an adjective stretched in place of its い, as typed, then its stem");
    require(whole("でーと") == "デート", "a loanword typed in kana, as the word list writes it");
    require(whole("こーら") == "コーラ",
            "Anthy's katakana word before completions: コーラ, not コーラス");
    require(whole("えきでであった") == "駅で出会った", "a context entry's particle isn't doubled");
    require(whole("がっこうであったはなし") == "学校であった話",
            "nor does one within a segment apply before the phrase goes on");
    // A phrase only the list has doesn't lead a word and a particle.
    std::ofstream(fs::path(directory) / "phrases.tsv") << "くるまできた\t車で来た\t0\n";
    const JapaneseWords phrases(fs::path(directory) / "phrases.tsv");
    composer.set_phrases(&phrases);
    require(whole("くるまで") == "車で", "くるまで is 車で before 車で来た");
    require(whole("いってきまーす") == "行ってきまーす",
            "a ー before another final kana, in a listed phrase");
    require(whole("ほんけんについて") == "本件について", "について is \"about\", no 着いて");
    require(whole("きょうとりょこう") == "今日と旅行",
            "Anthy's 京都, no common word, gives way to the listed 今日 and と");
    // The user's latest pick for a word leads, the usual form next.
    composer.set_word_pick([](const std::string& reading) { return reading == "いく" ? "往く" : ""; });
    converted("すぐいく");
    composer.segment(1);
    require(composer.candidates().at(0) == "往く" && composer.candidates().at(1) == "行く",
            "a picked form first, then the usual one, then Anthy's");
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
// What may follow a phrase ends once the field's cursor may have moved: a key
// that commits the reading and goes on to the field, or the laser leaving.
// Phrases are learned to follow one another only within one visit.
void follow_ups_end_with_the_field() {
    char directory[] = "/tmp/framekeyboard-follow-XXXXXX";
    require(mkdtemp(directory), "temporary follow-up profile");
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
    const auto chips = [](App& app) {
        std::vector<std::string> result;
        if (const auto strip = app.view().candidates) {
            for (const auto& chip : strip->chips) {
                result.push_back(chip.text);
            }
        }
        return result;
    };
    const std::vector<std::string> marks{"。", "、", "！", "？"};
    Sink sink;
    App app(options, sink);
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    double now = 1;
    const auto read = [&](const char* key) {
        click_key(app, key);
        app.tick(now += .01);
    };
    read("KeyA");
    read("Tab");
    require(sink.text == std::vector<std::string>{"あ"} && sink.delivery.size() > 1 &&
                sink.delivery.front() == "commit" && sink.delivery[1] == "key",
            "Tab commits the reading, then goes to the field");
    require(chips(app).empty(), "and nothing is offered after the phrase, now behind the cursor");
    read("KeyI");
    read("Enter");
    require(chips(app) == marks, "a commit offers what may follow");
    app.cancel_pointer(0);
    app.tick(now += .01);
    require(chips(app).empty(), "the laser leaving ends the offer");
    // い, then the laser leaves and う: not learned to follow い.
    read("KeyU");
    read("Enter");
    read("KeyI");
    read("Enter");
    require(chips(app) == marks, "a phrase after the laser left isn't learned to follow");
    // Nor across a hide: お after え.
    read("KeyE");
    read("Enter");
    app.cancel();
    read("KeyO");
    read("Enter");
    read("KeyE");
    read("Enter");
    require(chips(app) == marks, "nor one after the keyboard hid");
    // Straight after one another, え after お is.
    read("KeyO");
    read("Enter");
    require(chips(app).front() == "え", "a phrase straight after another is learned");
}
// As the headset draws it, a frame after each tick: the strip, built only
// when something changed, still follows every change.
void candidates_between_frames() {
    char directory[] = "/tmp/framekeyboard-frames-XXXXXX";
    require(mkdtemp(directory), "temporary frames profile");
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
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    double now = 1;
    const auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            app.tick(now += 1.0 / 60);
            app.paint(now);
        }
    };
    const auto strip = [&] {
        std::vector<std::string> texts;
        int selected = -1;
        if (const auto view = app.view().candidates) {
            for (const auto& chip : view->chips) {
                if (chip.selected) {
                    selected = static_cast<int>(texts.size());
                }
                texts.push_back(chip.text);
            }
        }
        return std::pair{texts, selected};
    };
    frames(5);
    click_key(app, "KeyK");
    click_key(app, "KeyA");
    frames(60);
    const auto live = strip().first;
    require(!live.empty() && live.front() == "〈か〉", "live candidates come after a pause");
    click_key(app, "KeyI");
    frames(60);
    require(strip().first.front() == "〈かい〉", "and follow the reading");
    click_key(app, "Space");
    frames(5);
    const auto converting = strip();
    click_key(app, "Space");
    frames(5);
    require(strip().first == converting.first && strip().second != converting.second,
            "converting, the next choice is marked");
    click_key(app, "Enter");
    frames(5);
    require(strip().first == std::vector<std::string>{"。", "、", "！", "？"},
            "a commit offers what may follow");
    app.show_settings();
    frames(5);
    require(strip().first.empty(), "Settings covers the strip");
    app.back();
    frames(5);
    require(strip().first.empty(), "and ended the offer, as any change of view does");
    click_key(app, "KeyA");
    frames(60);
    require(strip().first.front() == "〈あ〉", "a new reading replaces the offer");
}
// The band and its candidates, with the fake converter: live candidates after
// a pause, a tap moving on through the segments, the arrows resizing, a drag
// scrolling without picking, and the keys keeping their size across A / あ.
// Phrases committed straight after one another come back first after the
// one before them, also once the keyboard restarts; something sent between
// them breaks the run. Clearing forgets them.
// A store is written beside its file and put in its place: the temporary
// file is the owner's alone, a failed save leaves none behind, and Clear
// removes one that a save cut short left.
void next_words_temporary() {
    char directory[] = "/tmp/framekeyboard-next-tmp-XXXXXX";
    require(mkdtemp(directory), "temporary next words folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const auto file = fs::path(directory) / "japanese-next-words.tsv";
    const auto temporary = fs::path(directory) / "japanese-next-words.tsv.tmp";
    {
        std::ofstream(temporary) << "あ\tか\t1\t1\n";
        NextWords store(file);
        store.record("あ", "か");
        store.clear();
        require(!fs::exists(temporary), "Clear removes a temporary file left behind");
    }
    {
        NextWords store(file);
        for (int i = 0; i < 200; ++i) {
            store.record("あ", "word" + std::to_string(i));
        }
        // A file size limit cuts the write short, as a full disk would.
        rlimit before{};
        require(getrlimit(RLIMIT_FSIZE, &before) == 0, "the file size limit");
        const auto handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit small = before;
        small.rlim_cur = 64;
        require(setrlimit(RLIMIT_FSIZE, &small) == 0, "a small file size limit");
        const bool saved = store.save();
        setrlimit(RLIMIT_FSIZE, &before);
        std::signal(SIGXFSZ, handler);
        require(!saved && !fs::exists(temporary) && !fs::exists(file),
                "a failed save leaves no temporary file");
        require(store.save() && !fs::exists(temporary) &&
                    NextWords(file).after("あ", 1) == std::vector<std::string>{"word199"},
                "and the next one writes the file whole");
    }
    {
        const auto mask = umask(0);
        NextWords store(file);
        store.record("か", "さ");
        const bool saved = store.save();
        umask(mask);
        require(saved && (fs::status(file).permissions() &
                          (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
                "the file is the owner's alone, whatever the umask");
    }
}
void next_words() {
    char directory[] = "/tmp/framekeyboard-next-XXXXXX";
    require(mkdtemp(directory), "temporary next words profile");
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
    const auto chips = [](App& app) {
        std::vector<std::string> result;
        if (const auto strip = app.view().candidates) {
            for (const auto& chip : strip->chips) {
                result.push_back(chip.text);
            }
        }
        return result;
    };
    const auto commit = [](App& app, const char* slot, double now) {
        flick(app, slot, now);
        click_key(app, "Enter");
        app.tick(now + .01);
    };
    {
        Sink sink;
        App app(options, sink);
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        commit(app, "a", 1);
        commit(app, "ka", 2); // か straight after あ
        commit(app, "a", 3);
        require(chips(app) == std::vector<std::string>{"か", "。", "、", "！", "？"},
                "a phrase committed straight after another is offered first after it");
        click_key(app, "Backspace"); // Sent to the field: the run breaks.
        commit(app, "sa", 4);
        click_key(app, "Backspace");
        commit(app, "a", 5);
        require(chips(app) == std::vector<std::string>{"か", "。", "、", "！", "？"},
                "a phrase after something else was sent isn't learned to follow the one before");
        require(click_candidate(app, "か", 5.1) && sink.text.back() == "か" &&
                    chips(app).front() == "あ",
                "picking it types it, and offers what followed it in turn");
    }
    {
        Sink sink;
        App app(options, sink);
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        commit(app, "a", 1);
        require(chips(app).front() == "か", "they are kept across a restart");
    }
    NextWords kept(fs::path(directory) / "japanese-next-words.tsv");
    require(!kept.empty() && kept.after("あ", 3) == std::vector<std::string>{"か"},
            "in the config folder");
    kept.record("あ", "さ");
    kept.record("あ", "か");
    kept.record("あ", "さ");
    kept.record("あ", "か");
    kept.record("あ", "か");
    kept.record("あ", "さ");
    require(kept.after("あ", 1) == std::vector<std::string>{"か"} && kept.latest("あ") == "さ" &&
                kept.latest("い").empty(),
            "the most frequent and the latest are told apart");
    kept.clear();
    require(kept.empty() && !fs::exists(fs::path(directory) / "japanese-next-words.tsv"),
            "clearing forgets them and removes the file");
}
// The flick case is as wide as a full keyboard's without its numpad while it
// shows the reading line, so switching between them keeps the case's sides.
void flick_width() {
    char directory[] = "/tmp/framekeyboard-width-XXXXXX";
    require(mkdtemp(directory), "temporary width profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    Settings settings;
    settings.active = {"en-us-full", "ja-romaji", "graphite"};
    settings.hide_numpad = true;
    save_settings(directory, settings);
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
    const auto romaji = panel_case_bounds(app.view());
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    const auto flick = panel_case_bounds(app.view());
    require(std::abs(flick.width - romaji.width) < 1.5 && std::abs(flick.x - romaji.x) < 1,
            "the flick case is as wide as a full keyboard's without its numpad");
}
// With a composing language among the user's languages on the same layout,
// every page keeps the scale and place the reading line gives the keys, its
// line in the second toolbar row's place, so switching to it from English
// moves nothing; without one, English keeps the whole height. Either way the
// keys have the same size: the reading line only shortens the case.
void reading_room() {
    double pitch = 0; // From Q to W, with English alone.
    for (const bool shared : {false, true}) {
        char directory[] = "/tmp/framekeyboard-room-XXXXXX";
        require(mkdtemp(directory), "temporary room profile");
        struct Cleanup {
            fs::path path;
            ~Cleanup() { fs::remove_all(path); }
        } cleanup{directory};
        Settings settings;
        settings.active = {"en-us-full", "en-us", "graphite"};
        settings.favorites = {
            shared ? Favorite{"ja", "Japanese", {"en-us-full", "ja-romaji", "graphite"}}
                   : Favorite{"de", "German", {"international-full", "de-de", "graphite"}}};
        // Narrow, so the toolbar has the second row the reading line takes.
        settings.hide_numpad = true;
        save_settings(directory, settings);
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
        const auto english = app.view();
        const auto q = key_centre(app, "KeyQ");
        const double q_to_w = key_centre(app, "KeyW").first - q.first;
        auto bounds_of = [&](const std::string& id) {
            for (const auto& c : app.view().controls) {
                if (c.id == id) {
                    return c.bounds;
                }
            }
            throw std::runtime_error("no " + id + " button");
        };
        const auto button = bounds_of("settings");
        if (!shared) {
            require(!english.ime_room && !english.ime_band, "English alone keeps the whole height");
            pitch = q_to_w;
            continue;
        }
        require(english.ime_room && !english.ime_band && english.reading_case,
                "English keeps the reading line's scale");
        require(q_to_w == pitch, "the reading line's room keeps the keys at their size");
        const auto stay = bounds_of("passthrough");
        control(app, "language-cycle");
        require(app.selection().language == "ja-romaji" && app.view().ime_band,
                "the language button switches to romaji");
        const auto romaji = panel_case_bounds(app.view());
        const auto line = ime_band(app.view()).reading;
        require(key_centre(app, "KeyQ") == q && bounds_of("settings") == button &&
                    bounds_of("passthrough") == stay && romaji == panel_case_bounds(english) &&
                    romaji.height < panel_height,
                "switching to romaji moves neither keys, case nor toolbar, in a case without "
                "the second toolbar row");
        require(line.x >= stay.x + stay.width + 10 && stay.y == line.y + 1 && stay.y > 54,
                "the reading line takes the second row's place, its switch at the line's left");
        control(app, "language-cycle");
        require(key_centre(app, "KeyQ") == q, "and back again");
    }
}
// The language button types a reading as it stands before switching, as
// 半/全 does; a reading the target refuses keeps its language.
void language_commits_reading() {
    char directory[] = "/tmp/framekeyboard-chip-XXXXXX";
    require(mkdtemp(directory), "temporary chip profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    Settings settings;
    settings.active = {"en-us-full", "ja-romaji", "graphite"};
    settings.favorites = {Favorite{"en", "English", {"en-us-full", "en-us", "graphite"}}};
    save_settings(directory, settings);
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
    auto preedit = [&] { return app.view().preedit; };
    sink.accept = false;
    click_key(app, "KeyK");
    click_key(app, "KeyA");
    require(preedit() == "か", "romaji types a reading");
    control(app, "language-cycle");
    require(app.selection().language == "ja-romaji" && preedit() == "か" && sink.text.empty(),
            "a reading the target refuses stays, in its language");
    sink.accept = true;
    control(app, "language-cycle");
    require(app.selection().language == "en-us" && preedit().empty() &&
                sink.text == std::vector<std::string>{"か"},
            "the language button types the reading before switching");
}
// On JIS, romaji takes the ¥ key's ¥ into the reading, as it takes \ from
// the ろ key, and Latin mode types both.
void jis_symbols() {
    char directory[] = "/tmp/framekeyboard-jis-XXXXXX";
    require(mkdtemp(directory), "temporary JIS profile");
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
    app.apply({"ja-jis-full", "ja-romaji", "graphite"});
    click_key(app, "IntlYen");
    click_key(app, "IntlRo");
    require(app.view().preedit == "￥＼" && sink.text.empty(),
            "romaji takes ¥ and \\ into the reading full-width, as its other symbols");
    click_key(app, "ZenkakuHankaku");
    click_key(app, "IntlYen");
    click_key(app, "IntlRo");
    require(sink.text == std::vector<std::string>{"￥＼", "¥", "\\"},
            "半/全 types the reading, then Latin ¥ and \\");
}
// While earlier text is still being typed, the language button queues the
// reading behind it and switches.
void language_behind_pending_text() {
    struct BusySink : Sink {
        bool busy = false; // the target is still taking earlier text
        bool commit_text(const std::string& value) override { return !busy && Sink::commit_text(value); }
        bool text_pending() const override { return busy; }
    };
    char directory[] = "/tmp/framekeyboard-busy-XXXXXX";
    require(mkdtemp(directory), "temporary busy profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    Settings settings;
    settings.active = {"en-us-full", "ja-romaji", "graphite"};
    settings.favorites = {Favorite{"en", "English", {"en-us-full", "en-us", "graphite"}}};
    save_settings(directory, settings);
    int conversions = 0;
    Options options;
    options.config_dir = directory;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.japanese_converter = fake_converter(&conversions);
    BusySink sink;
    App app(options, sink);
    sink.busy = true;
    click_key(app, "KeyA");
    click_key(app, "Enter");
    app.tick(1);
    click_key(app, "KeyK");
    click_key(app, "KeyA");
    require(sink.text.empty() && app.view().preedit == "か", "あ is still on its way, か is read");
    control(app, "language-cycle");
    require(app.selection().language == "en-us" && app.view().preedit.empty(),
            "the language button switches, the reading queued");
    sink.busy = false;
    for (double now = 2; now < 3 && sink.text.size() < 2; now += .05) {
        app.tick(now);
    }
    require(sink.text == std::vector<std::string>{"あ", "か"}, "and both arrive in order");
}
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
    // After a commit the row offers punctuation, as a phone does: a tap types
    // it at once, and a key press ends the offer.
    app.tick(11.32);
    require(chips() == std::vector<std::string>{"。", "、", "！", "？"},
            "after a commit, the row offers punctuation");
    require(click_candidate(app, "！", 11.35) && sink.text.back() == "！" && chips().empty(),
            "a tap types it at once, and the offer ends");
    flick(app, "a", 11.4);
    click_key(app, "Enter");
    app.tick(11.45);
    require(!chips().empty(), "the next commit offers it again");
    click_key(app, "Backspace");
    app.tick(11.5);
    require(chips().empty(), "and a key press ends it");

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
    require(!has_control(app, "ime-more"),
            "with every candidate in the row, no ∨: its list would be empty");
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
    // A still row ends on whole chips, so none is cut before ∨.
    const auto whole = [&] {
        const auto view = app.view();
        const auto& area = view.candidates->area;
        return std::all_of(view.candidates->chips.begin(), view.candidates->chips.end(),
                           [&](const CandidateStripView::Chip& chip) {
                               return chip.bounds.width > area.width ||
                                      (chip.bounds.x >= area.x &&
                                       chip.bounds.x + chip.bounds.width <= area.x + area.width);
                           });
    };
    require(has_control(app, "ime-more") && whole(), "a still row shows only whole chips");
    const auto& first = strip->chips.front().bounds;
    const double y = first.y + first.height / 2, x = first.x + first.width / 2;
    app.down(7, x, y, 14.3);
    app.move(7, x - 60, y);
    app.move(7, x - 120, y);
    app.up(7, x - 120, y);
    app.tick(14.4);
    require(sink.text.size() == committed && !app.view().preedit.empty() &&
                app.view().candidates->chips.front().text != strip->chips.front().text,
            "a drag scrolls the strip and picks nothing");
    for (double at = 14.45; at < 17; at += .05) {
        app.tick(at);
    }
    require(whole(), "and so does one come to rest after scrolling");
    while (!app.view().preedit.empty()) {
        click_key(app, "Backspace");
    }

    // Romaji on a full layout: the reading in the status line's place, the
    // candidates in the toolbar row, and PC conventions for the arrows.
    app.apply({"en-us-full", "en-us", "graphite"});
    const auto english = app.view();
    app.apply({"en-us-full", "ja-romaji", "graphite"});
    const auto key_top = [](const PanelView& view) {
        return panel_rect(view, view.layout->keys.front().bounds).y;
    };
    require(
        app.view().ime_band && !has_control(app, "ime-toggle") &&
            panel_case_bounds(app.view()).width <= panel_case_bounds(english).width &&
            key_top(app.view()) > ime_band(app.view()).band.y + ime_band(app.view()).band.height,
        "full layouts give the reading a line above the keys, in a case that fits them, with no あ / A");
    for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyA", "KeyN", "KeyA", "KeyK", "KeyO"}) {
        click_key(app, id);
    }
    click_key(app, "Space");
    app.tick(20);
    {
        const auto view = app.view();
        const auto band = ime_band(view);
        const auto& row = view.candidates->area;
        const bool fits = row.width < band.candidates.width - 72;
        require(
            row.y == band.candidates.y && band.candidates.y < band.band.y &&
                band.reading.y + band.reading.height < key_top(view) && row.x == band.candidates.x &&
                (fits ? !has_control(app, "ime-more") : has_control(app, "ime-more")),
            "candidates from the toolbar row's left, or with ∨ after them, the reading line under it");
    }
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
// Emoji from IBus's dictionary come last in the live candidates, found by
// the reading or its conversions, and a pick commits the emoji.
void emoji_candidates() {
    const char* dictionary = std::getenv("FRAMEKEYBOARD_TEST_EMOJI_DICTIONARY");
    if (!dictionary) {
        std::cout << "Emoji candidates skipped: no IBus emoji dictionary.\n";
        return;
    }
    set_emoji_dictionary(dictionary);
    struct CatConverter : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            return {{reading, {"猫", reading}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    JapaneseComposer composer([] { return std::make_unique<CatConverter>(); });
    composer.set_emoji([](const std::vector<std::string>& words) { return emoji_for(words, 4); });
    composer.kana("ねこ");
    composer.prepare_live();
    const auto& live = composer.live();
    const auto katakana = std::find(live.begin(), live.end(), "ネコ");
    require(katakana != live.end() && live.end() - katakana > 1 && live.size() <= 8,
            "a few emoji follow the reading's scripts");
    const auto emoji = *(katakana + 1);
    // An emoji named after the word, such as 🐈 for ねこ, before ones it is a keyword of.
    require(emoji_for({"ねこ"}, 4).front() == emoji && !emoji_for({"猫"}, 4).empty(),
            "the reading's own emoji come first, then its conversions'");
    require(composer.choose_live(static_cast<int>(katakana + 1 - live.begin())) &&
                composer.commit_text() == emoji && composer.reopen_reading() == "ねこ",
            "a picked emoji commits, and ↶ would bring back its reading");
    set_emoji_dictionary("/nonexistent");
    require(emoji_for({"猫"}, 4).empty(), "without the dictionary there are none");
}
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
            "Clear learned words is offered only with Japanese, and learning has no switch");
    control(app, "settings"); // Back
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    app.show_settings();
    // Scrolls the Languages card until the row shows.
    double scrolled = 200;
    const auto find = [&](const std::string& id) {
        for (int step = 0; step < 20; ++step) {
            const auto controls = app.view().controls;
            const auto found = std::find_if(controls.begin(), controls.end(),
                                            [&](const Control& c) { return c.id == id; });
            if (found != controls.end()) {
                return *found;
            }
            // Any of the card's rows in view, to scroll it under the laser.
            const auto language = std::find_if(controls.begin(), controls.end(), [](const Control& c) {
                return c.id == "clear-learning" || c.id == "flick-only" || c.id == "key-flicks" ||
                       c.id == "swipe-typing" || c.id == "language";
            });
            if (language == controls.end()) {
                break;
            }
            app.move(9, language->bounds.x + 20, language->bounds.y + 20);
            app.scroll(9, 0, 1);
            for (int i = 0; i < 10; ++i) {
                app.tick(scrolled += .05); // The card glides to its place.
            }
        }
        throw std::runtime_error("Missing control " + id);
    };
    require(!find("clear-learning").enabled && !has_control(app, "japanese-learning"),
            "learning is always on, as on Steam's keyboard, with nothing to clear yet");
    require(!find("flick-only").selected, "Flick only shows with the flick layout, off as on phones");
    control(app, "settings"); // Back
    flick(app, "a", 1);
    click_key(app, "Space");
    click_key(app, "Enter");
    require(sink.text.back() == "〈あ〉" && learned == std::vector<std::vector<std::string>>{{"〈あ〉"}},
            "a committed conversion is learned");
    // Romaji and kana learn the same way, into the same record.
    for (const auto& [layout, language, key] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"en-us-full", "ja-romaji", "KeyA"}, {"ja-jis-full", "ja-kana", "Digit3"}}) {
        app.apply({layout, language, "graphite"});
        learned.clear();
        click_key(app, key);
        click_key(app, "Space");
        click_key(app, "Enter");
        require(learned == std::vector<std::vector<std::string>>{{"〈あ〉"}},
                (language + " learns too").c_str());
    }
    app.apply({"ja-flick-12", "ja-flick", "graphite"});

    // What Anthy learned, in Full Keyboard's own record beside the stock
    // keyboard's; Clear asks, then deletes ours only.
    const auto record = options.anthy_home / ".anthy" / "last-record2_framekeyboard.utf8";
    const auto stock = options.anthy_home / ".anthy" / "last-record2_default.utf8";
    fs::create_directories(record.parent_path());
    std::ofstream(record) << "learned\n";
    std::ofstream(stock) << "learned\n";
    {
        // A shell's preview, built beside the running keyboard, only draws.
        Options preview = options;
        preview.mode = Mode::Render;
        preview.config_dir = fs::path(directory) / "preview";
        preview.anthy_home.clear();
        fs::create_directories(preview.config_dir);
        App image(preview, sink);
        require(anthy_learned() && anthy_configured(),
                "a preview leaves the running keyboard's learning where it is");
    }
    app.show_settings();
    require(find("clear-learning").enabled, "something learned can be cleared");
    find("clear-learning"); // Scrolled into view.
    control(app, "clear-learning");
    require(app.view().confirm && fs::exists(record), "Clear asks first");
    control(app, "confirm-cancel");
    require(!app.view().confirm && fs::exists(record), "and cancelling keeps it");
    find("clear-learning"); // Scrolled into view.
    control(app, "clear-learning");
    control(app, "confirm-accept");
    for (int i = 0; i < 400 && find("clear-learning").busy; ++i) {
        require(!find("clear-learning").enabled, "a busy Clear can't be pressed again");
        app.tick(2 + i * .01);
        usleep(5000);
    }
    require(!fs::exists(record) && fs::exists(stock) && !find("clear-learning").enabled &&
                !find("clear-learning").busy && app.view().status == "Learned words cleared.",
            "clearing deletes Full Keyboard's record, not the stock keyboard's, then says so");
    require(!import_stock_learning(options.config_dir / "japanese-learning-imported") &&
                !fs::exists(record),
            "the stock keyboard's record was imported at the first start only, and isn't again");
}
// Full Keyboard learns into a record of its own, which starts once from a
// copy of the stock keyboard's, read under Anthy's lock and left as it is.
void stock_learning_import() {
    char directory[] = "/tmp/framekeyboard-import-XXXXXX";
    require(mkdtemp(directory), "temporary import home");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    const fs::path home = directory, anthy = home / ".anthy";
    const auto marker = home / "config" / "japanese-learning-imported";
    const auto content = [](const fs::path& path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    configure_anthy({true, home});
    require(!import_stock_learning(marker) && fs::exists(marker), "nothing to import, and only once");
    fs::remove(marker);
    fs::create_directories(anthy);
    std::ofstream(anthy / "last-record1_default.utf8") << "base\n";
    const auto base_time = fs::last_write_time(anthy / "last-record1_default.utf8");

    // The stock keyboard writing its journal, under its lock, as the import
    // starts: the import waits for it. Only system calls after fork().
    const auto lock_path = (anthy / "lock-file_default").string();
    const auto journal_path = (anthy / "last-record2_default.utf8").string();
    int ready[2];
    require(pipe(ready) == 0, "a pipe to the stock keyboard");
    const pid_t writer = fork();
    if (writer == 0) {
        const int lock = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        struct flock range{};
        range.l_type = F_WRLCK;
        range.l_whence = SEEK_SET;
        range.l_len = 1;
        fcntl(lock, F_SETLKW, &range);
        int journal = open(journal_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        (void)!write(journal, "ha", 2);
        (void)!write(ready[1], "", 1);
        usleep(200000);
        (void)!write(journal, "lf\n", 3);
        close(journal);
        _exit(0);
    }
    char signal{};
    require(read(ready[0], &signal, 1) == 1, "the stock keyboard holds its lock");
    require(import_stock_learning(marker), "the stock keyboard's record is imported");
    int status = 0;
    waitpid(writer, &status, 0);
    close(ready[0]);
    close(ready[1]);
    require(content(anthy / "last-record1_framekeyboard.utf8") == "base\n" &&
                content(anthy / "last-record2_framekeyboard.utf8") == "half\n",
            "whole, as the stock keyboard finished writing it");
    require(content(anthy / "last-record1_default.utf8") == "base\n" &&
                fs::last_write_time(anthy / "last-record1_default.utf8") == base_time &&
                content(anthy / "last-record2_default.utf8") == "half\n",
            "the stock keyboard's record stays as it is");
    require(anthy_learned(), "what was imported can be cleared");
    restart_anthy(true);
    require(!anthy_learned() && !fs::exists(anthy / "last-record1_framekeyboard.utf8") &&
                fs::exists(anthy / "last-record1_default.utf8") &&
                fs::exists(anthy / "last-record2_default.utf8"),
            "Clear deletes Full Keyboard's record only");
    require(!import_stock_learning(marker) && !anthy_learned(), "and a cleared record stays empty");
    fs::remove(marker);
    std::ofstream(anthy / "last-record1_framekeyboard.utf8") << "own\n";
    require(!import_stock_learning(marker) &&
                content(anthy / "last-record1_framekeyboard.utf8") == "own\n",
            "a record of Full Keyboard's own from before stays as it is");
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
    require(has_control(app, "ime-more") && !app.view().candidates->list,
            "∨ comes with more candidates than the row shows");
    {
        // ∨ ends the candidate row, beside its chips, not over them.
        const auto view = app.view();
        const auto band = ime_band(view);
        const auto more = std::find_if(view.controls.begin(), view.controls.end(),
                                       [&](const Control& c) { return c.id == "ime-more"; });
        require(more->bounds.x >= band.candidates.x + band.candidates.width &&
                    more->bounds.x + more->bounds.width <= band.band.x + band.band.width,
                "the band's ∨ sits right of the candidate row");
    }
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
    // A drag scrolls the list up and down; the row stays, and the scrollbar
    // shows how far.
    const auto thumb = app.view().candidates->thumb;
    require(thumb && thumb->x >= app.view().candidates->list->x + app.view().candidates->list->width,
            "a list longer than its room has a scrollbar beside its lines");
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
    require(view.candidates->thumb && view.candidates->thumb->y > thumb->y, "and its scrollbar follows");
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
                !app.view().candidates->list && !has_control(app, "ime-more") &&
                app.view().candidates->chips.front().text == "。",
            "a choice in the list commits it, closing the list for what may follow");
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
// ↶ sends one Backspace per character, so it leaves a phrase with a
// character of several code points, which fields delete differently, and on
// the digits page still steps a kana back through its key's taps.
void reopen_counts() {
    struct EmojiConverter : JapaneseConverter {
        std::vector<Segment> convert(const std::string& reading) override {
            return {{reading, {reading == "あ" ? "🐈‍⬛" : "猫", reading}}};
        }
        std::vector<Segment> resize(int, int) override { return {}; }
    };
    char directory[] = "/tmp/framekeyboard-reopen-counts-XXXXXX";
    require(mkdtemp(directory), "temporary reopen profile");
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
    options.japanese_converter = [] { return std::make_unique<EmojiConverter>(); };
    Sink sink;
    App app(options, sink);
    app.apply({"ja-flick-12", "ja-flick", "graphite"});
    double now = 1;
    for (const auto& [slot, text, reopens] : std::vector<std::tuple<std::string, std::string, bool>>{
             {"a", "🐈‍⬛", false}, {"na", "猫", true}}) {
        flick(app, slot, now);
        click_key(app, "Space");
        click_key(app, "Enter");
        require(sink.text.back() == text, "the conversion commits");
        sink.keys.clear();
        flick(app, "undo", now + 1);
        require(app.view().preedit.empty() != reopens && sink.keys.size() == (reopens ? 2u : 0u),
                reopens ? "a phrase of one-code-point characters reopens"
                        : "a phrase with 🐈‍⬛ stays: its Backspaces can't be counted");
        if (reopens) {
            click_key(app, "Backspace"); // The reading of one kana goes.
        }
        now += 2;
    }
    require(app.view().preedit.empty(), "nothing is left to type");
    flick(app, "a", now);
    flick(app, "a", now + .2);
    flick(app, "a", now + .4);
    flick(app, "mode", now + .6, 60, 0);
    require(app.view().flick_digits && app.view().preedit == "う", "digits keep the reading");
    flick(app, "undo", now + .8);
    require(app.view().preedit == "い", "↶ on the digits page steps the kana back");
}
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
void anthy_initialization_retry() {
    const auto* dictionary = std::getenv("FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY");
    if (!dictionary) {
        return;
    }
    void* library = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
    require(library, "test Anthy library missing");
    dlclose(library);
    const auto missing = default_anthy_home() / "missing-anthy.dic";
    const auto resident = [] {
        void* handle = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle) {
            dlclose(handle);
        }
        return handle != nullptr;
    };
    const auto fails = [](auto&& conversion) {
        try {
            conversion();
        } catch (const std::runtime_error& error) {
            return std::string(error.what()) == "Cannot initialize the Anthy dictionary";
        }
        return false;
    };
    set_anthy_dictionary(missing);
    for (int i = 0; i < 3; ++i) {
        require(fails([] { anthy_converter(); }), "a failed Anthy start retries initialization");
        require(resident(), "a failed Anthy start keeps the library loaded");
    }
    set_anthy_dictionary(dictionary);
    auto converter = anthy_converter();
    require(!converter->convert("にほん").empty(), "Anthy recovers when its dictionary is restored");
    set_anthy_dictionary(missing);
    for (int i = 0; i < 3; ++i) {
        restart_anthy(false);
        require(resident(), "a failed Anthy restart keeps the library loaded");
        require(fails([&] { converter->convert("にほん"); }),
                "conversion retries initialization after a failed restart");
    }
    set_anthy_dictionary(dictionary);
    require(!converter->convert("にほん").empty(),
            "an existing converter recovers after a failed Anthy restart");
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
        stretched_boundaries();
        resized_shorter_choices();
        resized_reordered_choices();
        resized_rules();
        completion_reading();
        completion_reopen();
        live_without_predictions();
        carried_number_boundaries();
        anthy_initialization_retry();
        symbols_as_typed();
        word_completion();
        flick_katakana();
        preferred_phrases();
        typed_katakana();
        candidates_report();
        numeral_digits();
        object_verbs();
        chat_kana();
        whole_words_lead();
        spelled_numbers();
        carried_conversion();
        context_converter();
        mozc_lifecycle();
        mozc_lost_session();
        mozc_stuck_server();
        mozc_restarts();
        mozc_clear_learning();
        mozc_sandbox_profile();
        mozc_learning_saves();
        word_forms();
        emoji_candidates();
        learning_calls();
        progressive_conversion();
        candidate_band();
        reading_room();
        language_commits_reading();
        language_behind_pending_text();
        jis_symbols();
        flick_width();
        next_words();
        next_words_temporary();
        follow_ups_end_with_the_field();
        candidates_between_frames();
        candidate_list();
        learning_settings();
        stock_learning_import();
        configure_anthy({false, anthy_home});
        reading_cursor();
        slow_reopen();
        reopen_counts();
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
                                                              {"konnichiha", "こんいちは"},
                                                              {"konnnichiha", "こんにちは"},
                                                              {"honnya", "ほんや"},
                                                              {"gakkou", "がっこう"},
                                                              {"shin'you", "しんよう"},
                                                              {"nn", "ん"},
                                                              {"kanna", "かんあ"},
                                                              {"kannna", "かんな"},
                                                              {"xn", "ん"},
                                                              {"kaxnna", "かんな"},
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
                     {"ちょっとまって！", "ちょっと待って！"},
                     {"「こんにちは」", "「今日は」"},
                     {"3じに", "3時に"}}) {
                composer.cancel();
                composer.kana(reading);
                composer.convert();
                require(composer.commit_text() == phrase, "Anthy's symbols stay as typed");
            }
            {
                // The bundled common words complete a reading before Anthy converts it.
                const JapaneseWords common(data_resource({}, "japanese/words.tsv"));
                composer.set_words(&common);
                composer.cancel();
                composer.kana("おめでと");
                composer.prepare_live();
                const auto& live = composer.live();
                require(live.size() > 2 && live[0] == "おめでとう" && live[1] == "おめでとうございます",
                        "おめでと completes to おめでとう and おめでとうございます first");
                composer.cancel();
                composer.set_words(nullptr);
            }
            // Romaji types ？ and ！ full-width, as Japanese input methods do,
            // and the last segment still offers the half-width form.
            for (const auto& [typed, reading, mark, half] :
                 std::vector<std::tuple<std::string, std::string, std::string, std::string>>{
                     {"nanndesuka?", "なんですか？", "？", "?"},
                     {"chottomatte!", "ちょっとまって！", "！", "!"}}) {
                composer.cancel();
                for (const char c : typed) {
                    composer.roman(c);
                }
                require(composer.preedit() == reading, "romaji ? and ! join the reading full-width");
                composer.convert();
                require(composer.commit_text().ends_with(mark), "and conversion keeps it");
                for (int i = 1; i < composer.segment_count(); ++i) {
                    composer.segment(1);
                }
                const auto candidates = composer.candidates();
                require(std::find(candidates.begin(), candidates.end(), half) != candidates.end(),
                        "the half-width form is a candidate");
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
                            !fs::exists(fs::path(anthy_home) / ".anthy" / "last-record1_default.utf8") &&
                            !fs::exists(fs::path(anthy_home) / ".anthy" / "last-record2_default.utf8"),
                        "a committed phrase is learned into Full Keyboard's own record, not the stock "
                        "keyboard's");
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
