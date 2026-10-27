#include "framekeyboard/app.hpp"
#include "framekeyboard/cheonjiin.hpp"
#include "framekeyboard/cjk.hpp"
#include <algorithm>
#include <cstdlib>
#include <glib.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
namespace {
void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Sink : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text, delivery;
    void send(int code, int value) override {
        keys.emplace_back(code, value);
        delivery.push_back("key");
    }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        delivery.push_back("commit");
        text.push_back(value);
        return true;
    }
};
// Adds space-separated strokes, jamo or punctuation, as the keys would.
std::string compose(const std::string& input) {
    CheonjiinComposer composer;
    std::istringstream units(input);
    for (std::string unit; units >> unit;) {
        composer.add(unit);
    }
    return composer.text();
}
void composer_rules() {
    // Every vowel from its strokes, after the silent ㅇ.
    for (const auto& [strokes, syllable] :
         std::vector<std::pair<std::string, std::string>>{{"ㅣ ㆍ", "아"},
                                                          {"ㅣ ㆍ ㅣ", "애"},
                                                          {"ㅣ ㆍ ㆍ", "야"},
                                                          {"ㅣ ㆍ ㆍ ㅣ", "얘"},
                                                          {"ㆍ ㅣ", "어"},
                                                          {"ㆍ ㅣ ㅣ", "에"},
                                                          {"ㆍ ㆍ ㅣ", "여"},
                                                          {"ㆍ ㆍ ㅣ ㅣ", "예"},
                                                          {"ㆍ ㅡ", "오"},
                                                          {"ㆍ ㅡ ㅣ ㆍ", "와"},
                                                          {"ㆍ ㅡ ㅣ ㆍ ㅣ", "왜"},
                                                          {"ㆍ ㅡ ㅣ", "외"},
                                                          {"ㆍ ㆍ ㅡ", "요"},
                                                          {"ㅡ ㆍ", "우"},
                                                          {"ㅡ ㆍ ㆍ ㅣ", "워"},
                                                          {"ㅡ ㆍ ㆍ ㅣ ㅣ", "웨"},
                                                          {"ㅡ ㆍ ㅣ", "위"},
                                                          {"ㅡ ㆍ ㆍ", "유"},
                                                          {"ㅡ", "으"},
                                                          {"ㅡ ㅣ", "의"},
                                                          {"ㅣ", "이"}}) {
        require(compose("ㅇ " + strokes) == syllable, "strokes " + strokes + " give " + syllable);
    }
    require(compose("ㅣ ㆍ") == "ㅏ" && compose("ㆍ ㅡ ㅣ ㆍ") == "ㅘ", "a vowel alone stays a jamo");
    require(compose("ㆍ") == "ㆍ" && compose("ㆍ ㆍ") == "ㆍㆍ" && compose("ㄱ ㆍ") == "ㄱㆍ",
            "unfinished strokes show in the reading");
    require(compose("ㄱ ㆍ ㅣ") == "거" && compose("ㄱ ㆍ ㆍ ㅣ") == "겨",
            "a later stroke finishes the pending vowel");
    require(compose("ㄱ ㅣ ㆍ ㄱ ㆍ") == "각ㆍ" && compose("ㄱ ㅣ ㆍ ㄱ ㆍ ㅣ") == "가거",
            "a final moves on once the pending strokes become a vowel");
    // Whole vowels, as flicks type them, join only where two-set joins them.
    require(compose("ㅇ ㅗ ㅏ") == "와" && compose("ㅇ ㅜ ㅓ") == "워", "flicked vowels join");
    require(compose("ㅇ ㅡ ㅓ") == "으ㅓ" && compose("ㅇ ㅏ ㅓ") == "아ㅓ",
            "flicked vowels join only as compound vowels");
    require(compose("ㅇ ㅏ ㆍ") == "야" && compose("ㅇ ㅓ ㅣ") == "에" && compose("ㅇ ㅗ ㅣ") == "외",
            "strokes extend a flicked vowel");
    // Syllables, finals and clusters.
    require(compose("ㅎ ㅣ ㆍ ㄴ") == "한", "한");
    require(compose("ㄱ ㅣ ㆍ ㄱ ㅣ ㆍ") == "가가", "a final moves to the next syllable");
    require(compose("ㄷ ㅣ ㆍ ㄹ ㄱ") == "닭" && compose("ㄷ ㅣ ㆍ ㄹ ㄱ ㅣ") == "달기",
            "a cluster splits when a vowel follows");
    require(compose("ㄱ ㅣ ㆍ ㅂ ㅅ") == "값" && compose("ㄱ ㅣ ㆍ ㅂ ㅅ ㆍ ㅣ") == "갑서",
            "ㅄ splits too");
    require(compose("ㅇ ㅣ ㆍ ㄴ ㅎ") == "않" && compose("ㅇ ㅣ ㆍ ㄴ ㅎ ㅣ ㆍ") == "안하",
            "ㄶ splits too");
    require(compose("ㄱ ㅣ ㆍ ㄸ") == "가ㄸ" && compose("ㄱ ㅣ ㆍ ㄸ ㅣ ㆍ") == "가따",
            "ㄸ, ㅃ and ㅉ never end a syllable");
    require(compose("ㄱ ㅣ ㆍ ㄲ") == "갂" && compose("ㄱ ㅣ ㆍ ㅆ ㆍ ㅡ") == "가쏘",
            "ㄲ and ㅆ end a syllable and move on");
    require(compose("ㄱ ㄴ") == "ㄱㄴ" && compose("ㄱ ㅣ ㆍ ㄱ ㄱ") == "각ㄱ",
            "consonants that cannot join stay apart");
    require(compose("ㅎ ㅣ ㆍ . ㄴ") == "하.ㄴ", "punctuation ends the syllable");

    CheonjiinComposer c;
    for (const auto* unit : {"ㅇ", "ㅣ", "ㆍ", "ㆍ"}) {
        c.add(unit);
    }
    require(c.text() == "야" && c.last() == "ㅑ", "야");
    c.backspace();
    require(c.text() == "아", "Backspace removes the last stroke");
    c.backspace();
    require(c.text() == "이", "Backspace removes strokes one at a time");
    c.backspace();
    require(c.text() == "ㅇ" && c.last() == "ㅇ", "then the vowel");
    c.add("ㆍ");
    c.add("ㆍ");
    c.backspace();
    require(c.text() == "ㅇㆍ", "Backspace removes a pending stroke");
    c.cancel();
    for (const auto* unit : {"ㄱ", "ㅣ", "ㆍ", "ㄱ", "ㅣ", "ㆍ"}) {
        c.add(unit);
    }
    c.backspace();
    require(c.text() == "가기", "Backspace keeps the moved final with its new vowel");
    c.backspace();
    require(c.text() == "각", "and returns it to the previous syllable");
    c.cancel();
    c.add("ㅗ");
    c.add("ㅏ");
    c.backspace();
    require(c.text() == "ㅗ", "Backspace removes a flicked vowel whole");
    c.cancel();
    c.add("ㄱ");
    c.replace("ㅋ");
    c.replace("ㄲ");
    require(c.text() == "ㄲ" && c.last() == "ㄲ", "a toggle replaces the last jamo");
    for (int i = 2; i < 30; ++i) {
        c.add("ㄴ");
    }
    c.add("ㅣ");
    bool full = false;
    try {
        c.add("ㄴ");
    } catch (const std::exception&) {
        full = true;
    }
    require(full, "the reading is bounded at 30 jamo");
    c.add("ㆍ");
    require(c.text().ends_with("나"), "strokes still extend the last vowel in a full reading");
    // The reading as shown is bounded too: ㆍㆍ is one unit but two characters,
    // and one commit carries at most 30 characters of it.
    c.cancel();
    for (int i = 0; i < 10; ++i) {
        c.add("ㆍ");
        c.add("ㆍ");
        c.add(".");
    }
    const auto shown = c.text();
    require(g_utf8_strlen(shown.c_str(), -1) == 30, "30 characters fit");
    auto refused = [&](auto change) {
        try {
            change();
        } catch (const std::exception&) {
            return c.text() == shown;
        }
        return false;
    };
    require(refused([&] { c.add("ㆍ"); }) && refused([&] { c.add("ㄱ"); }),
            "a key past 30 characters is refused and the reading stays");
    require(refused([&] { c.replace("……"); }), "a longer toggle choice is refused too");
    c.replace(",");
    require(c.last() == "," && g_utf8_strlen(c.text().c_str(), -1) == 30,
            "a toggle that keeps the length still works");

    require(CjkComposer::supported("korean-cheonjiin"), "천지인 needs no engine module");
    CjkComposer composer("korean-cheonjiin");
    require(composer.flick_input() && !composer.chinese() && composer.candidates().empty(),
            "the 천지인 composer has no candidates");
    composer.flick("ㅎ", false);
    composer.flick("ㅣ", false);
    composer.flick("ㆍ", false);
    composer.flick("", false);
    require(composer.preedit() == "하" && composer.text() == "하" && composer.has_reading(),
            "an unused direction types nothing");
    composer.cancel();
    require(composer.empty(), "cancel clears the reading");
}
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
    require(right > 0, "key is visible: " + id);
    return {(left + right) / 2, (top + bottom) / 2};
}
void press(App& app, const std::string& id, double now) {
    const auto [x, y] = key_centre(app, id);
    app.down(0, x, y, now);
    app.up(0, x, y);
}
// Presses a 12-key slot at time `now`, drags by (dx, dy) panel pixels and releases.
void flick(App& app, const std::string& slot, double now, double dx = 0, double dy = 0) {
    const auto [x, y] = key_centre(app, "Flick-" + slot);
    require(app.down(0, x, y, now), "12-key key accepts a press");
    if (dx != 0 || dy != 0) {
        app.move(0, x + dx / 2, y + dy / 2);
        app.move(0, x + dx, y + dy);
    }
    app.up(0, x + dx, y + dy);
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
void choose(App& app, const std::string& field, const std::string& choice) {
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
    control(app, id);
}
void app_input() {
    const auto profiles = load_profiles({}, {});
    auto fits = [&](const char* layout, const char* language) {
        try {
            validate_selection(profiles, {layout, language, "graphite"});
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    require(fits("ko-12key", "ko-cheonjiin") && !fits("ko-12key", "ja-flick") &&
                !fits("ja-flick-12", "ko-cheonjiin") && !fits("en-us-full", "ko-cheonjiin") &&
                !fits("ko-12key", "ko-kr") && !fits("ko-12key", "en-us") &&
                fits("ja-flick-12", "ja-flick"),
            "the 12-key layouts and their languages only fit each other");
    char directory[] = "/tmp/framekeyboard-korean12-XXXXXX";
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
    app.apply({"ko-12key", "ko-cheonjiin", "graphite"});
    auto preedit = [&] { return app.view().preedit; };
    double now = 100;
    auto clear = [&] {
        press(app, "Escape", now += 1);
        require(preedit().empty(), "Escape clears the reading");
    };
    // Types slots spaced out in time; "+" taps the same slot again quickly,
    // and ">" presses → to end a toggle.
    auto type = [&](const std::string& sequence) {
        std::istringstream slots(sequence);
        for (std::string slot; slots >> slot;) {
            if (slot == ">") {
                press(app, "ArrowRight", now += 2);
            } else if (slot.starts_with('+')) {
                flick(app, slot.substr(1), now += .3);
            } else {
                flick(app, slot, now += 2);
            }
        }
    };
    require(app.view().key_labels.at("Flick-g") == "ㄱ" && app.view().key_labels.at("Flick-i") == "ㅣ",
            "12-key keys show their first jamo");

    type("s +s i dot n");
    require(preedit() == "한", "한: ㅅ toggles to ㅎ, then ㅣㆍ and ㄴ");
    press(app, "Enter", now += 1);
    require(sink.text.back() == "한" && preedit().empty() && sink.keys.empty(),
            "Enter commits the reading without a key");
    type("o i dot n > n dot dot i o s +s i dot s dot i i o dot dot eu");
    require(preedit() == "안녕하세요", "안녕하세요");
    clear();
    type("s +s i dot n g eu dot g o dot i");
    require(preedit() == "한국어", "한국어");
    clear();
    type("g +g +g i dot");
    require(preedit() == "까", "three quick taps type ㄲ");
    clear();
    type("g +g +g +g");
    require(preedit() == "ㄱ", "the toggle cycle wraps");
    clear();
    type("g g");
    require(preedit() == "ㄱㄱ", "a tap after the toggle time adds a jamo");
    clear();
    flick(app, "g", now += 2);
    press(app, "ArrowRight", now += .1);
    flick(app, "g", now += .1);
    require(preedit() == "ㄱㄱ", "→ ends a toggle so the same key types again");
    require(sink.keys.empty(), "→ that ends a toggle sends nothing");
    clear();
    type("o +o i");
    require(preedit() == "미", "ㅇ toggles to ㅁ");
    clear();

    // Flicks: a consonant's right is its second jamo and up its double; the
    // ㅣ and ㅡ keys flick to the side their dot is on.
    flick(app, "g", now += 2, 0, -60);
    flick(app, "i", now += 2, 60, 0);
    flick(app, "d", now += 2, 60, 0);
    flick(app, "i", now += 2, -60, 0);
    flick(app, "b", now += 2, 0, -60);
    flick(app, "eu", now += 2, 0, -60);
    flick(app, "s", now += 2, 60, 0);
    flick(app, "eu", now += 2, 0, 60);
    require(preedit() == "까터뽀후", "consonant and vowel flicks");
    flick(app, "o", now += 2);
    flick(app, "eu", now += 2, 0, -60);
    flick(app, "i", now += 2, 60, 0);
    require(preedit() == "까터뽀후와", "flicked ㅗ and ㅏ join into ㅘ");
    flick(app, "dot", now += 2, 60, 0);
    require(preedit() == "까터뽀후와", "a stroke key has no flicks");
    clear();
    {
        const auto [x, y] = key_centre(app, "Flick-g");
        flick(app, "g", now += 2);
        app.down(0, x, y, now += .2);
        app.move(0, x, y - 60);
        const auto preview = app.view().flick_preview;
        require(preview && preview->key == "Flick-g" && preview->selected == 2 &&
                    preview->text == std::array<std::string, 5>{"ㅋ", "", "ㄲ", "ㅋ", ""},
                "a held key previews its next toggle jamo and its flicks");
        if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
            app.paint(now + .3);
            app.renderer.write_png(fs::path(render) / "framekeyboard-ko-12key-preview.png");
        }
        app.cancel_pointer(0);
        require(preedit() == "ㄱ" && !app.view().flick_preview, "a cancelled flick types nothing");
        clear();
    }

    // Punctuation toggles like a consonant and joins the reading.
    type("s +s i dot punct +punct");
    require(preedit() == "하,", "punctuation toggles and joins the reading");
    sink.delivery.clear();
    press(app, "Tab", now += 1);
    require(sink.text.back() == "하," && sink.delivery.size() >= 2 &&
                sink.delivery.front() == "commit" && sink.delivery[1] == "key" && preedit().empty(),
            "the reading commits before another key");

    // A full reading still commits whole: ㆍㆍ counts as two characters.
    for (int i = 0; i < 15; ++i) {
        type("dot dot punct");
    }
    const auto full = preedit();
    require(g_utf8_strlen(full.c_str(), -1) == 30 && full.ends_with("ㆍㆍ.") &&
                app.view().status.find("Commit this composition") != std::string::npos,
            "typing stops at 30 characters and says why");
    press(app, "Enter", now += 1);
    require(sink.text.back() == full && preedit().empty(), "Enter commits the full reading at once");

    type("o i dot dot");
    require(preedit() == "야", "야");
    press(app, "Backspace", now += 1);
    require(preedit() == "아", "Backspace removes the last stroke");
    press(app, "Backspace", now += 1);
    require(preedit() == "이", "Backspace removes one stroke at a time");
    type("dot");
    require(preedit() == "아", "the vowel grows again");
    // Moving, resizing, pinning or recentring the panel keeps the reading.
    app.set_dragging(true);
    app.set_dragging(false);
    app.set_dragging(true, true);
    app.set_dragging(false);
    control(app, "recenter");
    require(app.take_recenter() && preedit() == "아", "moving or resizing the panel keeps the reading");
    type("n g i dot");
    require(preedit() == "안가", "the reading continues after a move");
    {
        // A held Backspace repeats on the reading and stops once it is empty,
        // never reaching the target.
        const auto sent = sink.keys.size();
        const auto [x, y] = key_centre(app, "Backspace");
        app.down(0, x, y, now += 1);
        require(preedit() == "안기", "one deletion on press");
        app.tick(now + .6);
        require(preedit() == "안ㄱ" && app.holds_repeat(0), "held Backspace repeats on the reading");
        for (double t = now + .65; t < now + 1.5; t += .05) {
            app.tick(t);
        }
        require(preedit().empty() && !app.holds_repeat(0) && sink.keys.size() == sent,
                "the repeat stops at an empty reading and sends nothing");
        app.up(0, x, y);
        now += 2;
    }

    type("g i dot");
    control(app, "cjk-toggle");
    require(sink.text.back() == "가" && !app.view().composing, "한 / A commits and stops composition");
    flick(app, "n", now += 2);
    require(app.view().composing && preedit() == "ㄴ", "a 12-key key returns to composition");
    if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
        clear();
        type("o i dot n > n dot dot i o s +s i dot s dot i i o dot dot eu");
        app.paint(now + 1);
        app.renderer.write_png(fs::path(render) / "framekeyboard-ko-12key.png");
    }
    clear();

    app.show_settings();
    choose(app, "layout", "en-us-full");
    choose(app, "layout", "ko-12key");
    control(app, "apply");
    require(app.selection().layout == "ko-12key" && app.selection().language == "ko-cheonjiin",
            "choosing the 12-key layout selects 천지인");
    app.show_settings();
    choose(app, "layout", "en-us-full");
    control(app, "apply");
    if (CjkComposer::supported("korean-2set")) {
        require(app.selection().language == "ko-kr", "leaving 천지인 keeps Korean, as two-set");
    } else {
        // Without libhangul two-set Korean cannot apply, and the selection stays.
        require(app.selection().language == "ko-cheonjiin" &&
                    app.view().status.find("hangul engine unavailable") != std::string::npos,
                "leaving 천지인 chooses two-set Korean, which needs its engine");
    }
}
} // namespace
int main() {
    try {
        composer_rules();
        app_input();
        std::cout << "Korean 천지인 strokes, toggles, flicks, syllables and editing passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
