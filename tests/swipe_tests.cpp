#include "framekeyboard/app.hpp"
#include "framekeyboard/swipe.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-swipe-tests-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
const Profiles& profiles() {
    static const auto bundled = load_profiles({}, {});
    return bundled;
}
SwipeKeyboard keyboard_for(const std::string& layout, const std::string& language) {
    LanguageMap keymap(profiles().languages.at(language));
    auto keyboard = build_swipe_keyboard(profiles().layouts.at(layout), keymap);
    require(keyboard.has_value(), "letter keys found for " + language);
    return *keyboard;
}
int letter(const SwipeKeyboard& keyboard, char32_t c) {
    const int index = keyboard.index_of(c);
    require(index >= 0, "letter is on the keyboard");
    return index;
}
// Emits each letter of a path for two steps, separated by blanks: an ideal
// encoder output for exactly that collapsed path.
std::vector<float> emissions(const std::vector<int>& path, int& steps) {
    steps = 32;
    const int classes = swipe_max_keys + 1;
    std::vector<float> result(static_cast<std::size_t>(steps * classes), -20.0f);
    int t = 0;
    for (const int key : path) {
        for (int repeat = 0; repeat < 2 && t < steps; ++repeat, ++t) {
            result[static_cast<std::size_t>(t * classes + key)] = 0;
        }
        if (t < steps) {
            result[static_cast<std::size_t>(t++ * classes + classes - 1)] = 0;
        }
    }
    for (; t < steps; ++t) {
        result[static_cast<std::size_t>(t * classes + classes - 1)] = 0;
    }
    return result;
}
std::vector<int> path_of(const SwipeKeyboard& keyboard, const std::u32string& word) {
    std::vector<int> path;
    for (const auto c : word) {
        const int index = letter(keyboard, c);
        if (path.empty() || path.back() != index) {
            path.push_back(index);
        }
    }
    return path;
}
struct FakeEncoder : SwipeEncoder {
    std::shared_ptr<std::vector<int>> path;
    std::shared_ptr<int> calls;
    std::shared_ptr<std::atomic<bool>> hold; // While set, decoding waits.
    std::vector<float> encode(const float* features, const float*, const unsigned char* mask,
                              int& steps) override {
        while (hold && *hold) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++*calls;
        for (int i = 0; i < 2 * swipe_points; ++i) {
            require(std::isfinite(features[i]), "features are finite");
        }
        require(mask[0] == 1, "key mask marks letters");
        return emissions(*path, steps);
    }
};

// Dvorak's letters sit where its keymap puts them, not where QWERTY has them.
void dvorak_geometry() {
    const auto keyboard = keyboard_for("en-us-full", "en-us-dvorak");
    require(keyboard.key_index.at("KeyS") == letter(keyboard, U'o'), "Dvorak o is on the S key");
    require(keyboard.key_index.at("KeyD") == letter(keyboard, U'e'), "Dvorak e is on the D key");
    require(keyboard.key_index.at("KeyJ") == letter(keyboard, U'h'), "Dvorak h is on the J key");
}
void keyboard_geometry() {
    const auto us = keyboard_for("en-us-full", "en-us");
    require(us.letters.size() == 26, "US layout has 26 swipe letters");
    require(us.key_index.contains("KeyA") && !us.key_index.contains("Digit1") &&
                !us.key_index.contains("Space"),
            "only letter keys take part in swipes");
    require(us.frame.width > 9 * us.key_width && us.frame.width < 12 * us.key_width,
            "frame spans the letter block");
    const auto q = us.centers[static_cast<std::size_t>(letter(us, U'q'))];
    require(q.x > us.frame.x && q.y > us.frame.y, "letter centers lie inside the frame");
    require(letter(us, U'A') == letter(us, U'a'), "uppercase maps to its letter key");
    require(letter(us, U'é') == letter(us, U'e') && letter(us, U'ñ') == letter(us, U'n'),
            "accents without their own key fold onto the base letter");

    const auto de = keyboard_for("international-full", "de-de");
    require(letter(de, U'ä') != letter(de, U'a') && letter(de, U'ö') != letter(de, U'o') &&
                letter(de, U'ü') != letter(de, U'u'),
            "German umlauts have their own keys");
    require(letter(de, U'ß') == letter(de, U's') && !de.key_index.contains("Minus"),
            "ß on the number row is no swipe key: it is swiped on S, as on a phone");
    require(de.centers[static_cast<std::size_t>(letter(de, U'z'))].x ==
                us.centers[static_cast<std::size_t>(letter(us, U'y'))].x,
            "German keymap places Z on the QWERTZ key");
    require(letter(de, U'Ä') == letter(de, U'ä'), "German uppercase umlauts map to their key");

    const auto fr = keyboard_for("international-full", "fr-fr");
    require(letter(fr, U'é') == letter(fr, U'e') && letter(fr, U'à') == letter(fr, U'a') &&
                !fr.key_index.contains("Digit2") && !fr.key_index.contains("Digit0"),
            "AZERTY é and à on the number row are no swipe keys: swiped on E and A");
    require(letter(fr, U'ù') != letter(fr, U'u'), "accents inside the letter block keep their key");
    const auto it = keyboard_for("international-full", "it-it");
    require(letter(it, U'ì') == letter(it, U'i') && letter(it, U'ò') != letter(it, U'o'),
            "Italian ì on the number row is swiped on I; ò keeps its key");
    const auto uk = keyboard_for("en-us-full", "uk-ua");
    require(letter(uk, U'ґ') != letter(uk, U'г'), "Ukrainian ґ has its own key");
    require(letter(uk, U'ё') == letter(uk, U'е'), "an AltGr letter is swiped on the key that types it");
    const auto pl = keyboard_for("en-us-full", "pl-pl");
    require(letter(pl, U'ł') == letter(pl, U'l'),
            "Polish ł, with no plain-letter key, stays on AltGr+L");
    require(letter(pl, U'ź') == letter(pl, U'z') && letter(pl, U'ą') == letter(pl, U'a'),
            "Polish accented letters fold to their plain letter, ź to Z rather than AltGr+X");
    const auto nl = keyboard_for("en-us-full", "nl-nl");
    require(letter(nl, U'ë') == letter(nl, U'e') && letter(nl, U'ö') == letter(nl, U'o'),
            "Dutch ë folds to E, not AltGr+R on US international");
    const auto ru = keyboard_for("en-us-full", "ru-ru");
    require(letter(ru, U'ё') == letter(ru, U'е') && !ru.key_index.contains("Backquote"),
            "Russian ё on the number row is swiped on е");
    const auto es = keyboard_for("international-full", "es-es");
    require(es.index_of(U'ñ') >= 0 && es.index_of(U'ñ') != es.index_of(U'n'),
            "Spanish ñ has its own key");
    require(letter(es, U'á') == letter(es, U'a'), "Spanish dead-key accents fold");
}

void lexicon_paths() {
    const auto us = keyboard_for("en-us-full", "en-us");
    std::istringstream input("# comment\nhello\t200\nhelo\t100\ndon't\t180\nnaïve\t90\n"
                             "x\t250\ncafé\t120\nπι\t200\nbad line\n");
    const auto lexicon = SwipeLexicon::parse(input, us);
    require(lexicon.size() == 5, "unmappable, single-letter and malformed entries are skipped");
    int steps = 0;
    auto log_probs = emissions(path_of(us, U"helo"), steps);
    auto words = lexicon.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(words.size() == 2 && words[0].word == "hello" && words[1].word == "helo",
            "a doubled letter shares its collapsed path; frequency orders them");
    auto swiss = us;
    swiss.spelling = {{"ß", "ss"}};
    std::istringstream german("Masse\t200\nMaße\t150\nStraße\t100\n");
    const auto respelled = SwipeLexicon::parse(german, swiss);
    require(respelled.size() == 2, "a regional spelling merges the words it makes the same");
    log_probs = emissions(path_of(swiss, U"Strasse"), steps);
    words = respelled.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(!words.empty() && words[0].word == "Strasse", "and the word list types it");
    log_probs = emissions(path_of(us, U"dont"), steps);
    words = lexicon.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(!words.empty() && words[0].word == "don't", "apostrophes are skipped in the path");
    log_probs = emissions(path_of(us, U"cafe"), steps);
    words = lexicon.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(!words.empty() && words[0].word == "café", "accented words decode from base keys");
    require(lexicon.decode({}, 0, swipe_max_keys + 1, 5).empty(), "empty output decodes to nothing");

    const auto fr = keyboard_for("international-full", "fr-fr");
    std::istringstream french("été\t150\nêtes\t140\n");
    const auto accents = SwipeLexicon::parse(french, fr);
    log_probs = emissions(path_of(fr, U"ete"), steps);
    words = accents.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(!words.empty() && words[0].word == "été" &&
                std::count_if(words.begin(), words.end(),
                              [](const auto& c) { return c.word == "été"; }) == 1,
            "a number-row accent decodes from its plain letter, listed once");
    const auto de = keyboard_for("international-full", "de-de");
    std::istringstream sharp_words("straße\t150\n");
    const auto sharp = SwipeLexicon::parse(sharp_words, de);
    log_probs = emissions(path_of(de, U"strase"), steps);
    words = sharp.decode(log_probs, steps, swipe_max_keys + 1, 5);
    require(!words.empty() && words[0].word == "straße", "ß decodes from S");
}

void features() {
    const auto us = keyboard_for("en-us-full", "en-us");
    const auto a = us.centers[static_cast<std::size_t>(letter(us, U'a'))];
    const auto p = us.centers[static_cast<std::size_t>(letter(us, U'p'))];
    const auto values = swipe_features({{a.x, a.y, 1.0}, {p.x, p.y, 1.2}, {p.x, p.y, 1.1}}, us);
    require(values.size() == 2 * swipe_points, "features have the encoder's size");
    require(values.front() < values[swipe_points - 1], "path moves from A towards P");
    for (const auto v : values) {
        require(v > -0.1f && v < 1.1f, "letter positions normalize into the frame");
    }
    const auto still = swipe_features({{a.x, a.y, 2.0}}, us);
    require(still[0] == still[swipe_points - 1], "a single point stays put");
}

struct TextSink : KeySink {
    std::vector<std::pair<int, int>> events;
    std::vector<std::string> text;
    bool ordered_replace{}; // Local Gamescope text; false models acknowledged Steam-field keys.
    bool reject_replace{};  // The replacement fails without being applied.
    // Asynchronous results: while `hold` is set, a replacement or key press
    // stays pending; it fails once `polls_until_failure` more checks reach 0.
    bool hold{}, held_pending{};
    int polls_until_failure{-1};
    bool offline{}; // The input connection is lost.
    std::vector<std::size_t> replaced;
    bool pump() override { return !offline; }
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool held_failed() {
        if (polls_until_failure == 0) {
            held_pending = false;
            polls_until_failure = -1;
            return true;
        }
        if (polls_until_failure > 0) {
            --polls_until_failure;
        }
        return false;
    }
    bool text_pending() const override { return held_pending; }
    Delivery deliver_key(int code, int value) override {
        if (!hold || value != 1) {
            send(code, value);
            return Delivery::Success;
        }
        if (held_pending) {
            return held_failed() ? Delivery::Failure : Delivery::Waiting;
        }
        send(code, value);
        held_pending = true;
        return Delivery::Waiting;
    }
    bool text_available() override { return true; }
    bool refuse{}; // The target refuses text, as a full field does.
    bool commit_text(const std::string& value) override {
        if (refuse) {
            return false;
        }
        text.push_back(value);
        return true;
    }
    bool replaces_text() override { return ordered_replace; }
    bool replace_text(std::size_t delete_before, const std::string& value) override {
        if (held_pending) {
            held_failed();
            return false; // Still pending, or failed once no longer pending.
        }
        replaced.push_back(delete_before);
        if (reject_replace) {
            return false;
        }
        if (hold) {
            held_pending = true;
            return false;
        }
        text.push_back(value);
        return ordered_replace;
    }
};
std::pair<double, double> key_point(App& app, const std::string& action) {
    const auto view = app.view();
    // Scan from the bottom so the point is clear of the toolbar row.
    for (double y = panel_height - 4; y > 96; y -= 4) {
        for (double x = 0; x < panel_width; x += 4) {
            if (const auto* key = app.renderer.hit_key(view, x, y); key && key->action == action) {
                return {x, y};
            }
        }
    }
    throw std::runtime_error("missing visible key " + action);
}
bool has_control(const PanelView& view, const std::string& id) {
    return std::any_of(view.controls.begin(), view.controls.end(),
                       [&](const Control& control) { return control.id == id; });
}
Control control(const PanelView& view, const std::string& id) {
    for (const auto& c : view.controls) {
        if (c.id == id) {
            return c;
        }
    }
    throw std::runtime_error("missing control " + id);
}
void click(App& app, const std::string& id) {
    const auto c = control(app.view(), id);
    const double x = c.bounds.x + c.bounds.width / 2, y = c.bounds.y + c.bounds.height / 2;
    app.down(0, x, y, 1);
    app.up(0, x, y);
}
void swipe(App& app, const std::vector<std::string>& actions, double now) {
    auto [x, y] = key_point(app, actions.front());
    require(app.down(0, x, y, now), "swipe starts on a letter");
    for (std::size_t i = 1; i < actions.size(); ++i) {
        const auto [nx, ny] = key_point(app, actions[i]);
        for (int step = 1; step <= 4; ++step) {
            app.move(0, x + (nx - x) * step / 4, y + (ny - y) * step / 4);
        }
        x = nx;
        y = ny;
    }
    app.up(0, x, y);
}
// Long enough for a sanitizer build, whose word lists load several times slower.
template <class Done> void settle(App& app, Done done, int tries = 2000) {
    for (int i = 0; i < tries && !done(); ++i) {
        app.tick(monotonic_seconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Swipe taps, swiped words and alternatives all type through App's text
// output, so a replacement output (a local editor) receives every one of them.
struct RecordedText : TextOutput {
    std::vector<std::string> calls;
    bool text_press(unsigned, const std::string& text, double) override {
        calls.push_back("press:" + text);
        return true;
    }
    bool commit_text(const std::string& text) override {
        calls.push_back("commit:" + text);
        return true;
    }
    bool replaces_text() override { return true; }
    bool replace_text(std::size_t delete_before, const std::string& text) override {
        calls.push_back("replace:" + std::to_string(delete_before) + ":" + text);
        return true;
    }
    bool taps_then_text(int, std::size_t count, const std::string& text) override {
        calls.push_back("taps:" + std::to_string(count) + ":" + text);
        return true;
    }
};
void text_output_capture() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    RecordedText recorded;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.text_output = &recorded;
    options.swipe_encoder = [path] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = std::make_shared<int>(0);
        return encoder;
    };
    TextSink sink;
    {
        App app(options, sink);
        settle(app, [&] { return app.swipe_ready(); });
        *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
        auto [ax, ay] = key_point(app, "KeyA");
        app.down(0, ax, ay, 1);
        app.up(0, ax, ay);
        auto [dx, dy] = key_point(app, "Digit1");
        app.down(0, dx, dy, 1.1);
        app.up(0, dx, dy);
        swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
        settle(app, [&] { return recorded.calls.size() == 3; });
        click(app, "swipe-candidate-1");
        require(recorded.calls ==
                    std::vector<std::string>{"commit:a", "press:1", "commit:hello", "replace:5:helo"},
                "taps, text keys, swiped words and alternatives use the text output");
        require(sink.text.empty() && sink.replaced.empty(),
                "nothing reaches the target past the text output");
        // The next input keeps the chosen word, which typing suggestions learn.
        auto [sx, sy] = key_point(app, "Space");
        app.down(0, sx, sy, 3);
        app.up(0, sx, sy);
    }
    std::ifstream learned(directory.path / "words/en-us.tsv");
    std::string line;
    require(std::getline(learned, line) && line.starts_with("helo\t1\t") && !std::getline(learned, line),
            "the swiped word that stayed is learned, not the one replaced");
}
void app_flow(bool ordered_replace) {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    // A user profile without a "swipe" entry stands in for any unsupported language.
    fs::create_directories(directory.path / "languages");
    std::ofstream(directory.path / "languages/en-plain.json")
        << R"({"schema_version":1,"id":"en-plain","name":"Plain English","locale":"en-US",)"
        << R"("keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},)"
        << R"("legends":{"source":"keymap","overrides":{}},"font_families":["Noto Sans"]})";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    sink.ordered_replace = ordered_replace;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");

    // A tap types on release while swipe is active.
    auto [ax, ay] = key_point(app, "KeyA");
    require(app.down(0, ax, ay, 1) && sink.text.empty(), "swipe mode defers a letter until release");
    app.up(0, ax, ay);
    require(sink.text == std::vector<std::string>{"a"} && *calls == 0, "a tap is not decoded");
    {
        // Taps type on release, so two hands overlap: the other hand's press
        // types the held tap first, and neither is dropped.
        const auto [tx, ty] = key_point(app, "KeyT");
        const auto [hx, hy] = key_point(app, "KeyH");
        app.down(0, tx, ty, 1.2);
        app.down(1, hx, hy, 1.3);
        require(sink.text.back() == "t", "the other hand's press types the held tap first");
        app.up(0, tx, ty);
        app.up(1, hx, hy);
        const auto [sx, sy] = key_point(app, "Space");
        app.down(0, tx, ty, 1.4);
        app.down(1, sx, sy, 1.5);
        app.up(1, sx, sy);
        app.up(0, tx, ty);
        require(sink.text == std::vector<std::string>{"a", "t", "h", "t", " "},
                "overlapping presses from two hands type in press order");
        sink.text = {"a"};
    }
    {
        // A trembling tap that drifts just past its key into the next one
        // still types its letter; it is not decoded as a swipe.
        const auto [sx, sy] = key_point(app, "KeyS");
        const double drift = ax + (sx - ax) * .6;
        app.down(0, ax, ay, 1.6);
        app.move(0, drift, ay);
        app.up(0, drift, ay);
        require(sink.text == std::vector<std::string>{"a", "a"} && *calls == 0,
                "a tap that drifts over the key edge still types");
        sink.text = {"a"};
    }

    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    require(sink.text.size() == 1, "a swipe sends nothing before its result");
    settle(app, [&] { return sink.text.size() == 2; });
    require(sink.text.back() == "hello", "the swiped word is committed");
    require(has_control(app.view(), "swipe-candidate-1"), "alternatives are offered");
    require(app.view().swipe_trail.empty(), "the trail clears when the swipe ends");

    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 3);
    settle(app, [&] { return sink.text.size() == 3; });
    require(sink.text.back() == " hello", "consecutive swiped words get a space");

    sink.events.clear();
    click(app, "swipe-candidate-1");
    int backspaces = 0;
    for (const auto& [code, value] : sink.events) {
        backspaces += code == KEY_BACKSPACE && value == 1;
    }
    if (ordered_replace) {
        require(backspaces == 0 && sink.replaced == std::vector<std::size_t>{5} &&
                    sink.text.back() == "helo",
                "local text replaces the word in one ordered request, without libei Backspace");
    } else {
        require(backspaces == 5 && sink.text.back() == "helo",
                "an alternative replaces only the swiped word");
    }
    require(control(app.view(), "swipe-candidate-1").selected, "the chosen alternative is marked");

    const auto [sx, sy] = key_point(app, "Space");
    app.down(0, sx, sy, 4);
    app.up(0, sx, sy);
    require(!has_control(app.view(), "swipe-candidate-0"), "other input discards alternatives");

    // Shift capitalizes; a cancelled decode never types.
    const auto [shx, shy] = key_point(app, "ShiftLeft");
    app.down(0, shx, shy, 5);
    app.up(0, shx, shy);
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 6);
    settle(app, [&] { return sink.text.back() == "Hello"; });
    require(sink.text.back() == "Hello", "a latched Shift capitalizes the swiped word");
    app.down(0, shx, shy, 6.5);
    app.up(0, shx, shy);
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 6.6);
    settle(app, [&] { return sink.text.back() == " Hello"; });
    require(sink.text.back() == " Hello", "a Shift tap between swiped words keeps the space");
    app.down(0, shx, shy, 6.7);
    app.tick(7.3); // held: Shift locks
    app.up(0, shx, shy);
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 7.4);
    settle(app, [&] { return sink.text.back() == " HELLO"; });
    require(sink.text.back() == " HELLO", "a locked Shift types the swiped word in capitals");
    app.down(0, shx, shy, 7.5);
    app.up(0, shx, shy);
    const auto count = sink.text.size();
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 7);
    app.cancel();
    settle(app, [&] { return false; }, 200);
    require(sink.text.size() == count, "a decode cancelled by hiding is discarded");

    // Leaving the panel mid-swipe drops the path, and the release types nothing.
    auto [hx, hy] = key_point(app, "KeyH");
    const auto [ex, ey] = key_point(app, "KeyE");
    app.down(0, hx, hy, 8);
    app.move(0, ex, ey);
    require(!app.view().swipe_trail.empty() && app.pointer_pressed(0),
            "a swipe still captures its pointer, so lost tracking can cancel it");
    app.cancel_pointer(0);
    app.up(0, ex, ey);
    settle(app, [&] { return false; }, 200);
    require(sink.text.size() == count, "a cancelled pointer leaves no swipe behind");
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 9);
    settle(app, [&] { return sink.text.size() == count + 1; });
    app.cancel_pointer(0); // The laser leaves, perhaps to click another field.
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 10);
    settle(app, [&] { return sink.text.size() == count + 2; });
    require(sink.text.back() == "hello", "pointing away clears the automatic space");

    // Settings: enabled for a supported language, disabled otherwise.
    app.show_settings();
    auto swipe_box = control(app.view(), "swipe-typing");
    require(swipe_box.enabled && swipe_box.selected, "swipe checkbox reflects the saved choice");
    bool note = false;
    for (const auto& label : app.view().field_labels) {
        note |= label.text == "Powered by FUTO Swipe";
    }
    require(note, "the FUTO notice is shown beside the checkbox");
    click(app, "language");
    {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, -100); // Start at the top of the list, whatever is selected.
    }
    for (int i = 0; i < 30 && !has_control(app.view(), "choose:language:en-plain"); ++i) {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, 1);
    }
    click(app, "choose:language:en-plain");
    swipe_box = control(app.view(), "swipe-typing");
    require(!swipe_box.enabled && !swipe_box.selected, "unsupported languages disable swipe");
    note = false;
    for (const auto& label : app.view().field_labels) {
        note |= label.text == "Powered by FUTO Swipe" && !label.enabled && label.size < 22;
    }
    require(note, "the smaller notice stays visible and dims with its option");
    click(app, "apply");
    app.flush_settings();
    std::vector<std::string> errors;
    require(load_settings(directory.path, errors).swipe_typing,
            "the saved choice survives a disabled language");

    // A profile without a word list types on press again.
    const auto before = sink.text.size();
    const auto [fx, fy] = key_point(app, "KeyQ");
    app.down(0, fx, fy, 9);
    require(sink.text.size() == before + 1, "without swipe, letters still type on press");
    app.up(0, fx, fy);
}

// The trail repaints only its extent; that must equal a full repaint.
// The middle of the clear gap on the line from one key's point to another's.
std::pair<double, double> gap_between(App& app, const std::string& from, const std::string& to) {
    const auto view = app.view();
    const auto [ax, ay] = key_point(app, from);
    const auto [bx, by] = key_point(app, to);
    double first = -1, last = -1;
    for (int i = 0; i <= 400; ++i) {
        const double t = i / 400.0;
        if (!app.renderer.hit_key(view, ax + (bx - ax) * t, ay + (by - ay) * t)) {
            first = first < 0 ? t : first;
            last = t;
        }
    }
    require(first >= 0, "a gap lies between " + from + " and " + to);
    const double t = (first + last) / 2;
    return {ax + (bx - ax) * t, ay + (by - ay) * t};
}
// With Extended key collision off, a gap between letters only starts swipes.
void swipes_between_keys() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    settings.extended_key_collision = false;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    auto swipe_from = [&](std::pair<double, double> start, double now) {
        auto [x, y] = start;
        app.down(0, x, y, now);
        for (const auto* action : {"KeyE", "KeyL", "KeyO"}) {
            const auto [nx, ny] = key_point(app, action);
            for (int step = 1; step <= 4; ++step) {
                app.move(0, x + (nx - x) * step / 4, y + (ny - y) * step / 4);
            }
            x = nx;
            y = ny;
        }
        app.up(0, x, y);
    };
    auto tap = [&](double x, double y, double now) {
        const bool pressed = app.down(0, x, y, now);
        return app.up(0, x, y) || pressed;
    };

    const auto [gx, gy] = gap_between(app, "KeyG", "KeyH");
    const auto* nearest = app.renderer.swipe_key(app.view(), gx, gy);
    require(!app.renderer.hit_key(app.view(), gx, gy) && nearest &&
                (nearest->id == "KeyG" || nearest->id == "KeyH"),
            "a gap between letters belongs to the nearer letter");
    require(!tap(gx, gy, 1) && sink.text.empty() && *calls == 0,
            "a tap between letters presses and releases no key, and types nothing");

    swipe_from({gx, gy}, 2);
    settle(app, [&] { return !sink.text.empty(); });
    require(sink.text == std::vector<std::string>{"hello"} && *calls == 1,
            "a swipe can start between letters");
    require(has_control(app.view(), "swipe-candidate-1"), "and offers alternatives");
    tap(gx, gy, 3);
    require(has_control(app.view(), "swipe-candidate-1") && sink.text.size() == 1,
            "a tap between letters leaves the word and its alternatives");

    const auto [shift_x, shift_y] = key_point(app, "ShiftLeft");
    tap(shift_x, shift_y, 4);
    tap(gx, gy, 4.1);
    require(!app.view().keyboard->modifiers().empty(), "a tap between letters leaves Shift latched");
    swipe_from({gx, gy}, 5);
    settle(app, [&] { return sink.text.back() == "Hello"; });
    require(app.view().keyboard->modifiers().empty(), "a swipe from between letters uses the latch");

    // Half of each gap belongs to the letter, beside a key that isn't one too.
    auto first_miss = [&](double x, double y, double dx, double dy) {
        while (app.renderer.hit_key(app.view(), x, y)) {
            x += dx;
            y += dy;
        }
        return std::pair{x, y};
    };
    const auto [ax, ay] = key_point(app, "KeyA");
    const auto [below_x, below_y] = first_miss(ax, ay, 0, 1);
    const auto* below = app.renderer.swipe_key(app.view(), below_x, below_y);
    require(below && below->id == "KeyA", "the gap under a letter starts a swipe on it");
    const auto [left_x, left_y] = first_miss(ax, ay, -1, 0);
    const auto* beside = app.renderer.swipe_key(app.view(), left_x, left_y);
    require(beside && beside->id == "KeyA", "so does the gap beside it");
    double caps_x = left_x;
    while (!app.renderer.hit_key(app.view(), caps_x, left_y)) {
        caps_x -= 1;
    }
    require(!app.renderer.swipe_key(app.view(), caps_x + 1, left_y),
            "but not the half of the gap by Caps Lock");

    click(app, "swipe-toggle");
    const auto typed = sink.text;
    require(!app.renderer.swipe_key(app.view(), gx, gy) && !tap(gx, gy, 6) && sink.text == typed,
            "with swipe off a gap starts nothing");
}
// With it on, as by default, half the gap is the letter's own: a tap there
// types it, and a swipe starts there as on the key.
void swipes_from_key_areas() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    const auto view = app.input_view();
    const auto [left, top] = app.renderer.layout_point(view, 0, 0);
    const auto [right, bottom] = app.renderer.layout_point(view, 1000, 1000);
    // G ends at 521.7 and H starts at 527.7: 523 is in G's half of the gap.
    const double gx = (523 - left) * 1000 / (right - left), gy = (240 - top) * 1000 / (bottom - top);
    const auto* key = app.renderer.hit_key(view, gx, gy);
    require(key && key->id == "KeyG" && !view.swipe_reach, "G's half of the gap is G's");
    app.down(0, gx, gy, 1);
    app.up(0, gx, gy);
    settle(app, [&] { return !sink.text.empty(); });
    require(sink.text == std::vector<std::string>{"g"} && *calls == 0, "a tap there types G");
    double x = gx, y = gy;
    app.down(0, x, y, 2);
    for (const auto* action : {"KeyE", "KeyL", "KeyO"}) {
        const auto [nx, ny] = key_point(app, action);
        for (int step = 1; step <= 4; ++step) {
            app.move(0, x + (nx - x) * step / 4, y + (ny - y) * step / 4);
        }
        x = nx;
        y = ny;
    }
    app.up(0, x, y);
    settle(app, [&] { return sink.text.size() == 2; });
    require(sink.text.back() == "hello" && *calls == 1, "and a swipe starts there as on G");
}
void trail_repaint() {
    TemporaryDirectory directory;
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [] { return std::unique_ptr<SwipeEncoder>(); };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    auto identical = [&](double now) {
        const auto partial = app.renderer.rgba();
        app.renderer.paint(app.view(), now, true);
        const auto full = app.renderer.rgba();
        int count = 0, left = panel_width, top = texture_height, right = -1, bottom = -1;
        for (std::size_t i = 0; i < full.size(); i += 4) {
            if (!std::equal(full.begin() + static_cast<std::ptrdiff_t>(i),
                            full.begin() + static_cast<std::ptrdiff_t>(i + 4),
                            partial.begin() + static_cast<std::ptrdiff_t>(i))) {
                const int px = static_cast<int>(i / 4 % panel_width),
                          py = static_cast<int>(i / 4 / panel_width);
                if (!count++) {
                    std::cerr << "first difference at " << px << "," << py << ": partial "
                              << int(partial[i]) << "," << int(partial[i + 1]) << ","
                              << int(partial[i + 2]) << "," << int(partial[i + 3]) << " full "
                              << int(full[i]) << "," << int(full[i + 1]) << "," << int(full[i + 2])
                              << "," << int(full[i + 3]) << '\n';
                }
                left = std::min(left, px);
                right = std::max(right, px);
                top = std::min(top, py);
                bottom = std::max(bottom, py);
            }
        }
        if (count) {
            std::cerr << count << " pixels differ in " << left << "," << top << " - " << right << ","
                      << bottom << " (texture coordinates)\n";
        }
        return count == 0;
    };
    app.paint(1);
    auto [x, y] = key_point(app, "KeyQ");
    app.down(0, x, y, 1);
    app.paint(1.1);
    for (const auto* action : {"KeyB", "KeyP", "KeyA"}) {
        const auto [nx, ny] = key_point(app, action);
        // Laser coordinates are fractional; damage must still cover whole pixels.
        for (int step = 1; step <= 6; ++step) {
            app.move(0, x + (nx - x) * step / 6 + 0.37 * step, y + (ny - y) * step / 6 + 0.61);
            app.paint(1.2);
        }
        x = nx;
        y = ny;
    }
    require(!app.view().swipe_trail.empty() && identical(1.2),
            "a partial trail repaint matches a full one");
    app.up(0, x, y);
    app.paint(1.3);
    require(identical(1.3), "removing the trail leaves no residue");
    // A trail repaints only its newest segment while it grows, so every
    // intermediate frame must match a full repaint too, not just the last.
    std::tie(x, y) = key_point(app, "KeyZ");
    app.down(0, x, y, 2);
    for (const auto* action : {"KeyY", "KeyM", "KeyQ"}) {
        const auto [nx, ny] = key_point(app, action);
        for (int step = 1; step <= 6; ++step) {
            app.move(0, x + (nx - x) * step / 6 + 0.29 * step, y + (ny - y) * step / 6 + 0.43);
            app.paint(2.1);
            require(identical(2.1), "each frame of a growing trail matches a full repaint");
        }
        x = nx;
        y = ny;
    }
    app.up(0, x, y);
    app.paint(2.2);
    require(identical(2.2), "the ended trail leaves no residue");
    app.cancel();
}
// Needs the Pinyin engine module, which Frame builds include.
// Unchanged word lists are not parsed again; edits and a release load them
// anew. Only a release that names the encoder loads that again.
void service_reuse() {
    TemporaryDirectory directory;
    const auto path = directory.path / "words.tsv";
    std::ofstream(path) << "hello\t200\n";
    auto encoders = std::make_shared<std::atomic<int>>(0);
    SwipeService service([encoders] {
        ++*encoders;
        return std::unique_ptr<SwipeEncoder>();
    });
    const auto keyboard = std::make_shared<const SwipeKeyboard>(keyboard_for("en-us-full", "en-us"));
    auto prepare = [&](std::shared_ptr<const SwipeKeyboard> board) {
        service.prepare(std::move(board), path);
        for (int i = 0; i < 400 && !service.ready(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    prepare(keyboard);
    prepare(std::make_shared<const SwipeKeyboard>(*keyboard)); // Reload builds an equal copy.
    require(service.loads() == 1, "an unchanged word list for an equal keyboard is not parsed again");
    prepare(std::make_shared<const SwipeKeyboard>(keyboard_for("international-full", "de-de")));
    require(service.loads() == 2, "another keyboard parses the list again");
    std::ofstream(path) << "hello\t200\nhelp\t150\n";
    prepare(keyboard);
    require(service.loads() == 3, "an edited word list is parsed again");
    service.release();
    prepare(keyboard);
    require(service.loads() == 4, "a released word list loads again");
    require(*encoders == 1, "a word list release keeps the encoder");
    service.release(true);
    prepare(keyboard);
    require(service.loads() == 5 && *encoders == 2, "a released encoder loads again");
}
// The toolbar switch keeps the encoder, so switching back is quick; turning
// swipe typing off in Settings frees it, and turning it on loads it again.
void encoder_release() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto encoders = std::make_shared<std::atomic<int>>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [encoders] {
        ++*encoders;
        return std::unique_ptr<SwipeEncoder>();
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    require(*encoders == 1, "swipe typing loads the encoder");
    click(app, "swipe-toggle");
    click(app, "swipe-toggle");
    settle(app, [&] { return app.swipe_ready(); });
    require(app.swipe_ready() && *encoders == 1, "the toolbar switch keeps the encoder");
    auto set_option = [&](bool on) {
        app.show_settings();
        if (control(app.view(), "swipe-typing").selected != on) {
            click(app, "swipe-typing");
        }
        click(app, "apply");
        app.flush_settings();
        app.summon();
    };
    set_option(false);
    set_option(true);
    settle(app, [&] { return app.swipe_ready() && *encoders == 2; });
    require(app.swipe_ready() && *encoders == 2, "the option frees the encoder and loads it again");
}
// The laser leaving while a released swipe is decoding: the target's cursor
// may move before the result arrives, so the word is discarded.
void leave_while_decoding() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path =
        std::make_shared<std::vector<int>>(path_of(keyboard_for("en-us-full", "en-us"), U"helo"));
    auto calls = std::make_shared<int>(0);
    auto hold = std::make_shared<std::atomic<bool>>(false);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls, hold] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        encoder->hold = hold;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *hold = true;
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 1);
    app.cancel_pointer(0);
    *hold = false;
    settle(app, [&] { return !app.view().status.empty(); });
    require(sink.text.empty() && !has_control(app.view(), "swipe-candidate-0") &&
                app.view().status.starts_with("Swipe discarded"),
            "a word decoded after the laser left is discarded, with a notice");
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] { return sink.text.size() == 1; });
    require(sink.text.back() == "hello" && has_control(app.view(), "swipe-candidate-0"),
            "the next swipe types without an inherited space and offers alternatives");
}
// Suggestions start after the first toolbar row and end before Close, with
// the numpad shown or hidden and with or without the language button.
void suggestions_clear_the_toolbar() {
    for (const bool hide_numpad : {false, true}) {
        for (const bool favorite : {false, true}) {
            TemporaryDirectory directory;
            fs::create_directories(directory.path / "swipe/lexicons");
            std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
            Settings settings;
            settings.swipe_typing = true;
            settings.hide_numpad = hide_numpad;
            if (favorite) {
                settings.favorites.push_back(
                    {"de", "German", {"international-full", "de-de", "graphite"}});
            }
            save_settings(directory.path, settings);
            auto path = std::make_shared<std::vector<int>>(
                path_of(keyboard_for("en-us-full", "en-us"), U"helo"));
            Options options;
            options.config_dir = directory.path;
            options.mode = Mode::Vr;
            options.input = InputBackend::Ei;
            options.start_enabled = true;
            options.swipe_encoder = [path] {
                auto encoder = std::make_unique<FakeEncoder>();
                encoder->path = path;
                encoder->calls = std::make_shared<int>(0);
                return encoder;
            };
            TextSink sink;
            App app(options, sink);
            settle(app, [&] { return app.swipe_ready(); });
            swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 1);
            settle(app, [&] { return has_control(app.view(), "swipe-candidate-1"); });
            const auto view = app.view();
            const auto close = control(view, "close").bounds;
            double toolbar_end = 0;
            for (const auto& c : view.controls) {
                if (c.action.kind != ControlKind::SwipeCandidate && c.id != "close" &&
                    c.bounds.y == 12) {
                    toolbar_end = std::max(toolbar_end, c.bounds.x + c.bounds.width);
                }
            }
            require(
                !favorite || toolbar_end ==
                                 [&] {
                                     const auto language = control(view, "language-cycle").bounds;
                                     return language.x + language.width;
                                 }(),
                "the language button ends the first row");
            for (const auto& c : view.controls) {
                if (c.action.kind == ControlKind::SwipeCandidate) {
                    require(c.bounds.x > toolbar_end && c.bounds.x + c.bounds.width < close.x &&
                                c.bounds.width > 100,
                            "suggestions sit between the toolbar and Close, readable");
                }
            }
            // With the numpad hidden, passthrough moves to a second row.
            const auto passthrough = control(view, "passthrough").bounds;
            require((passthrough.y > 12) == hide_numpad,
                    "a compact toolbar moves passthrough below the first row");
            require(!hide_numpad || passthrough.x == control(view, "settings").bounds.x,
                    "the second row starts under Settings");
        }
    }
}
// After a failed replacement the field may still hold the old word, so the
// alternatives must go: replacing by the assumed word's length could delete
// text before it ("Xhelo" minus five characters).
void failed_replacement() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "helo\t200\nhello\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path =
        std::make_shared<std::vector<int>>(path_of(keyboard_for("en-us-full", "en-us"), U"helo"));
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    sink.ordered_replace = true;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 1);
    settle(app, [&] { return !sink.text.empty(); });
    require(sink.text.back() == "helo" && has_control(app.view(), "swipe-candidate-1"),
            "the swiped word and its alternatives");
    sink.reject_replace = true;
    click(app, "swipe-candidate-1");
    app.tick(monotonic_seconds());
    require(!has_control(app.view(), "swipe-candidate-0") && !app.view().status.empty(),
            "a failed replacement removes the alternatives and says so");
    require(sink.replaced == std::vector<std::size_t>{4}, "only the one replacement was attempted");
    sink.reject_replace = false;
    const auto [ax, ay] = key_point(app, "KeyA");
    app.down(0, ax, ay, 2);
    app.up(0, ax, ay);
    require(sink.text.back() == "a", "typing continues after the failure");
}
// A failure can become known inside the next suggestion click, before any
// tick: that click must not replace by the length of the undelivered word.
// `polls` is how many more delivery checks pass before the failure shows.
void failure_found_by_click(bool ordered, int polls) {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "helo\t200\nhello\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path =
        std::make_shared<std::vector<int>>(path_of(keyboard_for("en-us-full", "en-us"), U"helo"));
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    sink.ordered_replace = ordered;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 1);
    settle(app, [&] { return !sink.text.empty(); });
    sink.hold = true;
    click(app, "swipe-candidate-1"); // "hello": its first request stays pending.
    auto backspaces = [&] {
        return std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{KEY_BACKSPACE, 1});
    };
    require(has_control(app.view(), "swipe-candidate-0") &&
                (ordered ? sink.replaced.size() == 1 : backspaces() == 1),
            "the chosen alternative is pending");
    sink.polls_until_failure = polls; // The result is a failure; no tick sees it first.
    click(app, "swipe-candidate-0");
    require(ordered ? sink.replaced == std::vector<std::size_t>{4} : backspaces() == 1,
            "no second deletion follows a failure found by the click");
    app.tick(monotonic_seconds());
    require(!has_control(app.view(), "swipe-candidate-0") && !app.view().status.empty(),
            "the alternatives are gone and the failure is reported");
}
// Resting and very long gestures keep bounded history; a rest still decodes.
void long_gestures() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path =
        std::make_shared<std::vector<int>>(path_of(keyboard_for("en-us-full", "en-us"), U"helo"));
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    auto [x, y] = key_point(app, "KeyH");
    const auto [ex, ey] = key_point(app, "KeyE");
    app.down(0, x, y, 1);
    app.move(0, ex, ey);
    for (int i = 0; i < 5000; ++i) {
        app.move(0, ex + (i % 2) * .1, ey); // Resting, with sub-pixel jitter.
    }
    require(app.view().swipe_trail.size() < 8, "a resting laser does not grow the gesture");
    for (const auto* action : {"KeyL", "KeyO"}) {
        std::tie(x, y) = key_point(app, action);
        app.move(0, x, y);
    }
    app.up(0, x, y);
    settle(app, [&] { return !sink.text.empty(); });
    require(sink.text == std::vector<std::string>{"hello"}, "a swipe with a long rest still decodes");

    std::tie(x, y) = key_point(app, "KeyH");
    const auto [lx, ly] = key_point(app, "KeyL");
    app.down(0, x, y, 2);
    for (int i = 0; i < 2100; ++i) {
        app.move(0, i % 2 ? lx : x, i % 2 ? ly : y);
    }
    require(app.view().swipe_trail.empty() && app.view().status.find("too long") != std::string::npos,
            "an endless gesture ends without typing");
    app.up(0, x, y);
    settle(app, [&] { return false; }, 200);
    require(sink.text.size() == 1, "nothing types for the ended gesture");
}
// A word list still loading must not hold up keys on the VR thread.
void loading_does_not_block() {
    TemporaryDirectory directory;
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    // The encoder loads inside the first prepare job, so this stands in for a slow load.
    options.swipe_encoder = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return std::unique_ptr<SwipeEncoder>();
    };
    TextSink sink;
    App app(options, sink);
    require(!app.swipe_ready(), "swipe waits for its word list");
    const auto [x, y] = key_point(app, "KeyA");
    const auto started = std::chrono::steady_clock::now();
    app.down(0, x, y, 1);
    app.up(0, x, y);
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    app.down(0, x, y, 3);
    app.up(0, x, y);
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(150),
            "keys pressed while the word list loads do not wait for it");
    require(!sink.text.empty() && sink.text.front() == "a", "letters type on press while loading");
    settle(app, [&] { return app.swipe_ready(); });
    require(app.swipe_ready(), "swipe becomes ready after loading");
}
int chinese_flow() {
    if (!CjkComposer::supported("chinese-pinyin-simplified")) {
        std::cout << "Pinyin engine not built\n";
        return 77;
    }
    TemporaryDirectory directory;
    Settings settings;
    settings.swipe_typing = true;
    settings.active = {"en-us-full", "zh-cn-pinyin", "graphite"};
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    require(app.selection().language == "zh-cn-pinyin", "Pinyin profile is active");
    *path = path_of(keyboard_for("en-us-full", "zh-cn-pinyin"), U"nihao");
    swipe(app, {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}, 1);
    settle(app, [&] { return !app.view().preedit.empty(); });
    settle(app, [&] { return app.view().candidates.has_value(); });
    require(!app.view().preedit.empty() && app.view().candidates &&
                !app.view().candidates->chips.empty(),
            "a Pinyin swipe fills the composer and offers characters");
    require(sink.text.empty() && !has_control(app.view(), "swipe-candidate-0"),
            "nothing is sent and no Latin alternatives appear");
    const auto before = app.view().preedit;
    const auto [x, y] = key_point(app, "KeyA");
    app.down(0, x, y, 2);
    require(app.view().preedit == before, "a Pinyin tap waits for release");
    app.up(0, x, y);
    require(app.view().preedit != before && sink.text.empty(), "a Pinyin tap types into the composer");
    // The composer holds 30 keys. Swiped words beyond that are refused whole,
    // and taps at the limit report it; neither may end the process.
    for (int i = 0; i < 8; ++i) {
        const auto full = app.view().preedit;
        swipe(app, {"KeyN", "KeyI", "KeyH", "KeyA", "KeyO"}, 3 + i);
        settle(app, [&] { return app.view().preedit != full || !app.view().status.empty(); });
    }
    require(!app.view().status.empty(), "a swipe past the composition limit is refused with a notice");
    for (int i = 0; i < 10; ++i) {
        app.down(0, x, y, 20 + i); // Fill the remaining room with taps.
        app.up(0, x, y);
    }
    const auto full = app.view().preedit;
    app.down(0, x, y, 40);
    app.up(0, x, y);
    require(app.view().preedit == full && !app.view().status.empty(), "a tap at the limit is refused");
    app.cancel();
    return 0;
}
void disabled_by_default() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    auto calls = std::make_shared<int>(0);
    options.swipe_encoder = [calls] {
        ++*calls;
        return std::unique_ptr<SwipeEncoder>();
    };
    TextSink sink;
    App app(options, sink);
    const auto [x, y] = key_point(app, "KeyA");
    app.down(0, x, y, 1);
    require(sink.text == std::vector<std::string>{"a"}, "default typing sends on press");
    app.up(0, x, y);
    require(*calls == 0, "the swipe engine is not loaded while swipe is off");
}

// The Additional languages popup marks swipe support, and only while swiping
// is on; otherwise its rows say nothing about it.
void languages_popup_marks() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [] { return std::unique_ptr<SwipeEncoder>(); };
    TextSink sink;
    App app(options, sink);
    auto press = [&](const std::string& id) {
        const auto view = app.view();
        const auto control = std::find_if(view.controls.begin(), view.controls.end(),
                                          [&](const Control& item) { return item.id == id; });
        require(control != view.controls.end(), "the control shows");
        const double x = control->bounds.x + control->bounds.width / 2;
        const double y = control->bounds.y + control->bounds.height / 2;
        app.move(0, x, y);
        app.down(0, x, y, 1);
        app.up(0, x, y);
    };
    // How many listed rows say each thing, as the list runs past what shows.
    auto rows = [&](std::optional<bool> swipe) {
        const auto languages = app.view().languages->languages;
        return std::count_if(languages.begin(), languages.end(),
                             [&](const auto& item) { return item.swipe == swipe; });
    };
    app.show_settings();
    press("additional-languages");
    require(rows(true) == 0 && rows(false) == 0, "no icon while swiping is off");
    require(!app.view().languages->main.swipe, "on the main language either");
    press("languages-close");
    press("swipe-typing");
    press("additional-languages");
    require(rows(true) > 0, "languages with a word list can swipe");
    require(rows(false) > 0, "those without are marked as unsupported");
    require(rows(std::nullopt) == 0, "and every row says which");
    require(app.view().languages->main.swipe == true, "the main language shows it too");
}

// The toolbar's shortcut for Swipe typing follows the languages in the cycle.
// The editor takes swipes as the keyboard does, and its alternatives replace
// only the word they typed.
void swipes_in_the_editor() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::RenderDictation;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    auto calls = std::make_shared<int>(0);
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    app.show_dictation();
    auto text = [&] { return app.view().dictation->text; };
    const auto before = text();
    {
        const auto [hx, hy] = key_point(app, "KeyH");
        const auto [ox, oy] = key_point(app, "KeyO");
        app.down(0, hx, hy, 1);
        for (int step = 1; step <= 6; ++step) {
            app.move(0, hx + (ox - hx) * step / 6, hy + (oy - hy) * step / 6);
        }
        require(app.view().swipe_trail.size() > 1, "a swipe in the editor draws its trail");
        app.cancel_pointer(0);
        require(app.view().swipe_trail.empty(), "and the trail ends with it");
    }
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] { return text() != before; });
    const auto word = text().find(" hello in one go") != std::string::npos ? "hello" : "helo";
    require(text().find(std::string(" ") + word + " in one go") != std::string::npos,
            "a swiped word joins the editor at the cursor, with its space");
    require(sink.text.empty(), "and nothing reaches the target");
    const auto other = std::string(word) == "hello" ? "helo" : "hello";
    const auto view = app.view();
    const auto candidate =
        std::find_if(view.controls.begin(), view.controls.end(), [&](const Control& c) {
            return c.id.starts_with("swipe-candidate-") && c.label == other;
        });
    require(candidate != view.controls.end(), "its alternatives show");
    click(app, candidate->id);
    require(text().find(std::string(" ") + other + " in one go") != std::string::npos,
            "an alternative replaces just that word");
    auto [ax, ay] = key_point(app, "KeyA");
    app.down(0, ax, ay, 3);
    app.up(0, ax, ay);
    require(text().find(std::string(" ") + other + "a in one go") != std::string::npos,
            "a tap still types one letter, on release");
    const auto [bx, by] = key_point(app, "Backspace");
    app.down(0, bx, by, 3.2);
    app.up(0, bx, by);
    bool left = false;
    for (const auto& control : app.view().controls) {
        left |= control.id.starts_with("swipe-candidate-");
    }
    require(!left, "another key ends the alternatives");
    // A word that doesn't fit stays out whole, and so do its alternatives'
    // replacements of text before it. The notice goes with the text.
    for (const auto [dx, dy] = key_point(app, "KeyD");
         text().size() < DictationEditor::hard_limit - 2;) {
        app.down(0, dx, dy, 3.5);
        app.up(0, dx, dy);
    }
    const auto full = text();
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 3.6);
    settle(app, [&] { return !app.view().dictation->notice.empty(); });
    require(text() == full && app.view().dictation->notice.starts_with("The editor is full"),
            "a swiped word too long for the editor says so and types nothing");
    const auto [back_x, back_y] = key_point(app, "Backspace");
    app.down(0, back_x, back_y, 3.7);
    app.up(0, back_x, back_y);
    require(app.view().dictation->notice.empty(), "and making room clears the notice");
    sink.offline = true;
    app.tick(monotonic_seconds());
    const auto connected = text();
    const auto [cx, cy] = key_point(app, "KeyC");
    app.down(0, cx, cy, 4);
    app.up(0, cx, cy);
    require(text() != connected, "the editor is local, so its keys work without a connection");
    sink.offline = false;
    app.tick(monotonic_seconds());
}

// A swiped word replaces a selection. Its space depends on the text it follows,
// whichever way the selection was made.
void swiping_over_a_selection() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::RenderDictation;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [path] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = std::make_shared<int>(0);
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    double now = 1;
    auto tap = [&](const char* id) {
        const auto [x, y] = key_point(app, id);
        app.down(0, x, y, now);
        app.up(0, x, y);
        app.tick(now += .1);
    };
    auto select = [&](const char* arrow, int steps) {
        for (int i = 0; i < steps; ++i) {
            tap("ShiftLeft");
            tap(arrow);
        }
    };
    auto text = [&] { return app.view().dictation->text; };
    auto swiped = [&] {
        const auto before = text();
        swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, now += 1);
        settle(app, [&] { return text() != before; });
    };
    auto after = [&](const char* prefix, const char* rest) {
        return text().find(std::string(prefix) + "hello" + rest) != std::string::npos ||
               text().find(std::string(prefix) + "helo" + rest) != std::string::npos;
    };

    // The sample's cursor follows "application", between words.
    app.show_dictation();
    for (int i = 0; i < 11; ++i) {
        tap("ArrowLeft");
    }
    select("ArrowRight", 11);
    swiped();
    require(after("focused ", " in one go"), "a forward selection takes one space from the text before");

    const auto word = text().find("focused hello ") != std::string::npos ? "hello" : "helo";
    const auto other = std::string(word) == "hello" ? "helo" : "hello";
    const auto view = app.view();
    const auto candidate =
        std::find_if(view.controls.begin(), view.controls.end(), [&](const Control& c) {
            return c.id.starts_with("swipe-candidate-") && c.label == other;
        });
    require(candidate != view.controls.end(), "its alternatives show");
    click(app, candidate->id);
    require(text().find(std::string("focused ") + other + " in one go") != std::string::npos,
            "and an alternative replaces only the word");

    app.show_dictation();
    select("ArrowLeft", 11);
    swiped();
    require(after("focused ", " in one go"), "so does a backward selection");

    app.show_dictation();
    tap("Home");
    select("ArrowRight", 9);
    swiped();
    require(after("", " turns speech") && (text().starts_with("hello") || text().starts_with("helo")),
            "a forward selection of the first word leaves no space");
}

// The way a wearer gets there: the mic key opens the editor and listens, and a
// second press stops, leaving the editor to type in.
void swipes_after_listening() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    // A silent microphone. The real helper would record this machine's own
    // microphone, and any sound there would turn into phrases mid-test.
    std::ofstream(directory.path / "helper.py") << R"PY(
import json, socket
peer = socket.socket(fileno=3)
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    if message["op"] == "capture" and not message["on"]:
        peer.send(b'{"op":"paused"}')
)PY";
    Settings settings;
    settings.swipe_typing = true;
    settings.hide_numpad = true;
    settings.dictation = "server";
    settings.dictation_server = "127.0.0.1";
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.dictation_helper = directory.path / "helper.py";
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    auto press = [&](const char* id) {
        const auto view = app.view();
        const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                      [&](const Key& item) { return item.id == id; });
        require(key != view.layout->keys.end(), "the mic key shows");
        const auto r = panel_rect(view, key->bounds);
        app.move(0, r.x + r.width / 2, r.y + r.height / 2);
        app.down(0, r.x + r.width / 2, r.y + r.height / 2, 1);
        app.up(0, r.x + r.width / 2, r.y + r.height / 2);
    };
    press("Dictation");
    require(app.dictation_open() && app.listening(), "the mic key opens the editor and listens");
    const auto toggle = [&] {
        const auto view = app.view();
        return *std::find_if(view.controls.begin(), view.controls.end(),
                             [](const Control& item) { return item.id == "swipe-toggle"; });
    };
    require(!toggle().enabled && toggle().icon == Icon::SwipeOff,
            "the swipe button is disabled while the editor can't be typed in");
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] { return !app.view().dictation->text.empty(); }, 200);
    require(app.view().dictation->text.empty() && *calls == 0,
            "while listening the editor is read-only, so a swipe does nothing");
    press("Dictation");
    settle(app, [&] { return !app.listening(); });
    require(app.dictation_open() && !app.listening(), "the mic key stops listening");
    settle(app, [&] { return toggle().enabled; });
    require(toggle().enabled && toggle().icon == Icon::Swipe, "and it returns once it can");
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] { return !app.view().dictation->text.empty(); });
    require(!app.view().dictation->text.empty(), "a swipe types once listening has stopped");
}

// A tap the other hand holds types before the mic, Type as I speak's Esc or
// Enter's retry acts on this press, as it was pressed first.
void held_tap_before_dictation_keys() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    // A stand-in microphone: one phrase as listening starts, then quiet.
    std::ofstream(directory.path / "helper.py") << R"PY(
import json, socket
peer = socket.socket(fileno=3)
def send(**m):
    peer.send(json.dumps(m).encode())
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    if message["op"] == "capture" and message["on"]:
        send(op="phrase", id=1)
        send(op="ready", id=1)
    elif message["op"] == "capture":
        send(op="paused")
    elif message["op"] == "prompt":
        send(op="text", id=message["id"], text="Hello there.")
)PY";
    for (const auto* order : {"release", "mic", "esc", "retry"}) {
        const std::string which = order;
        Settings settings;
        settings.swipe_typing = true;
        settings.hide_numpad = true;
        settings.dictation = "server";
        settings.dictation_server = "127.0.0.1";
        settings.dictation_direct = which != "mic";
        save_settings(directory.path, settings);
        auto path = std::make_shared<std::vector<int>>();
        auto calls = std::make_shared<int>(0);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.dictation_helper = directory.path / "helper.py";
        options.swipe_encoder = [path, calls] {
            auto encoder = std::make_unique<FakeEncoder>();
            encoder->path = path;
            encoder->calls = calls;
            return encoder;
        };
        TextSink sink;
        App app(options, sink);
        settle(app, [&] { return app.swipe_ready(); });
        const auto tap = [&](const char* id) {
            const auto view = app.view();
            const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                          [&](const Key& item) { return item.id == id; });
            require(key != view.layout->keys.end(), std::string("the key shows: ") + id);
            const auto r = panel_rect(view, key->bounds);
            app.down(0, r.x + r.width / 2, r.y + r.height / 2, monotonic_seconds());
            app.up(0, r.x + r.width / 2, r.y + r.height / 2);
        };
        const auto [ox, oy] = key_point(app, "KeyO");
        if (which == "release") {
            // A change that keeps the target lets a held tap go as its
            // release would; one that gives the target up drops it.
            app.down(1, ox, oy, monotonic_seconds());
            app.show_settings();
            app.up(1, ox, oy);
            require(sink.text == std::vector<std::string>{"o"}, "opening Settings types a held tap");
            app.back();
            app.down(1, ox, oy, monotonic_seconds());
            app.cancel();
            app.up(1, ox, oy);
            app.down(1, ox, oy, monotonic_seconds());
            app.suspend_input();
            app.up(1, ox, oy);
            require(sink.text.size() == 1, "a cancel or a sleep drops it");
            continue;
        }
        if (which == "mic") {
            app.down(1, ox, oy, monotonic_seconds());
            tap("Dictation");
            require(sink.text == std::vector<std::string>{"o"} && app.dictation_open(),
                    "a held tap types into the app before the mic opens the editor");
            app.up(1, ox, oy);
            require(app.view().dictation->text.empty(), "and not into the editor");
            continue;
        }
        if (which == "esc") {
            tap("Dictation");
            require(app.listening(), "Type as I speak listens");
            settle(app, [&] { return !sink.text.empty(); });
            sink.text.clear();
            app.down(1, ox, oy, monotonic_seconds());
            tap("Escape");
            require(sink.text == std::vector<std::string>{"o"} && !app.listening(),
                    "a held tap types before Esc stops Type as I speak");
            app.up(1, ox, oy);
            require(sink.text.size() == 1, "and only once");
            continue;
        }
        sink.refuse = true;
        tap("Dictation");
        settle(app, [&] { return app.view().status.find("Enter") != std::string::npos; });
        require(sink.text.empty() && app.view().status.find("Enter") != std::string::npos,
                "a refused phrase waits for Enter");
        sink.refuse = false;
        app.down(1, ox, oy, monotonic_seconds());
        tap("Enter");
        settle(app, [&] { return sink.text.size() >= 2; });
        app.up(1, ox, oy);
        require(sink.text == std::vector<std::string>{"o", "Hello there."},
                "a held tap types before Enter's retried phrase");
    }
}

// A launch that cannot type still has the local editor.
void editor_in_preview() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    Settings settings;
    settings.swipe_typing = true;
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::RenderDictation;
    options.input = InputBackend::None;
    options.swipe_encoder = [path] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = std::make_shared<int>(0);
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    app.show_dictation();
    const auto text = [&] { return app.view().dictation->text; };
    const auto before = text();
    const auto [ax, ay] = key_point(app, "KeyA");
    app.down(0, ax, ay, 1);
    app.up(0, ax, ay);
    require(text().size() == before.size() + 1, "a tap types in the preview's editor");
    const auto tapped = text();
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] { return text() != tapped; });
    require(text().size() > tapped.size(), "and so does a swipe");
    const auto swiped = text();
    const auto word = swiped.find("hello") != std::string::npos ? "hello" : "helo";
    const auto other = std::string(word) == "hello" ? "helo" : "hello";
    const auto view = app.view();
    const auto candidate =
        std::find_if(view.controls.begin(), view.controls.end(), [&](const Control& c) {
            return c.id.starts_with("swipe-candidate-") && c.label == other;
        });
    require(candidate != view.controls.end(), "its alternatives show");
    click(app, candidate->id);
    require(text().find(other) != std::string::npos && text() != swiped, "and they replace it");
    require(sink.text.empty() && sink.events.empty(), "nothing leaves the keyboard");
}

// Listening has paused, and a phrase is still being recognized.
void swipes_wait_for_recognition() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv") << "hello\t200\nhelo\t100\n";
    std::ofstream(directory.path / "helper.py") << R"PY(
import json, socket, time
peer = socket.socket(fileno=3)
def send(**m):
    peer.send(json.dumps(m).encode())
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    if message["op"] == "capture" and message["on"]:
        send(op="phrase", id=1)
    elif message["op"] == "capture":
        send(op="ready", id=1)
        send(op="paused")
    elif message["op"] == "prompt":
        time.sleep(.4)
        send(op="text", id=1, text="Hello there.")
)PY";
    Settings settings;
    settings.swipe_typing = true;
    settings.hide_numpad = true;
    settings.dictation = "server";
    settings.dictation_server = "127.0.0.1";
    save_settings(directory.path, settings);
    auto path = std::make_shared<std::vector<int>>();
    auto calls = std::make_shared<int>(0);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.dictation_helper = directory.path / "helper.py";
    options.swipe_encoder = [path, calls] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->path = path;
        encoder->calls = calls;
        return encoder;
    };
    TextSink sink;
    App app(options, sink);
    settle(app, [&] { return app.swipe_ready(); });
    *path = path_of(keyboard_for("en-us-full", "en-us"), U"helo");
    auto press_mic = [&] {
        const auto view = app.view();
        const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                      [](const Key& item) { return item.id == "Dictation"; });
        require(key != view.layout->keys.end(), "the mic key shows");
        const auto r = panel_rect(view, key->bounds);
        app.move(0, r.x + r.width / 2, r.y + r.height / 2);
        app.down(0, r.x + r.width / 2, r.y + r.height / 2, 1);
        app.up(0, r.x + r.width / 2, r.y + r.height / 2);
    };
    press_mic();
    press_mic();
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 2);
    settle(app, [&] {
        const auto view = app.view();
        return view.dictation->text.find("Hello there.") != std::string::npos;
    });
    require(*calls == 0,
            "a swipe waits for the phrase, so its alternatives can't replace the phrase's end");
    require(app.view().dictation->text.find("Hello there.") != std::string::npos,
            "the phrase arrives whole");
}

void toolbar_toggle() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [] {
        auto encoder = std::make_unique<FakeEncoder>();
        encoder->calls = std::make_shared<int>(0);
        return encoder;
    };
    auto config = [&](const std::string& language, const std::string& favorites, bool swipe = true,
                      const std::string& layout = "en-us-full") {
        std::ofstream(directory.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":")" << layout << R"(","language":")" << language
            << R"(","theme":"graphite"},"swipe_typing":)" << (swipe ? "true" : "false")
            << R"(,"favorites":[)" << favorites << "]}";
    };
    auto button = [](App& app) {
        const auto view = app.view();
        const auto found = std::find_if(view.controls.begin(), view.controls.end(),
                                        [](const Control& item) { return item.id == "swipe-toggle"; });
        return found == view.controls.end() ? std::optional<Control>{} : std::optional{*found};
    };
    auto press = [](App& app, const Control& control) {
        const double x = control.bounds.x + control.bounds.width / 2;
        const double y = control.bounds.y + control.bounds.height / 2;
        app.move(0, x, y);
        app.down(0, x, y, 1);
        app.up(0, x, y);
        app.flush_settings();
    };
    auto saved = [&] {
        std::vector<std::string> errors;
        return load_settings(directory.path, errors);
    };
    config("en-us", "", false);
    {
        TextSink sink;
        App app(options, sink);
        require(!button(app), "with the option off there is no button");
    }
    config("en-us", "");
    {
        TextSink sink;
        App app(options, sink);
        auto toggle = button(app);
        require(toggle && toggle->enabled && toggle->icon == Icon::Swipe && !toggle->dimmed,
                "with the option on, the button shows on a language that can swipe");
        press(app, *toggle);
        toggle = button(app);
        require(toggle && toggle->icon == Icon::SwipeOff && !toggle->dimmed, "off slashes the icon");
        require(!saved().swipe_switch && saved().swipe_typing,
                "the button is saved on its own and leaves the option alone");
        app.show_settings();
        const auto view = app.view();
        const auto row = std::find_if(view.controls.begin(), view.controls.end(),
                                      [](const Control& item) { return item.id == "swipe-typing"; });
        require(row != view.controls.end() && row->selected, "Settings keeps the option on");
    }
    {
        TextSink sink;
        App app(options, sink);
        const auto toggle = button(app);
        require(toggle && toggle->icon == Icon::SwipeOff, "its state survives a restart");
        press(app, *toggle);
        require(saved().swipe_switch, "and it turns back on");
    }
    // A main language that can't swipe, with an additional one that can.
    const auto english = R"({"id":"a","name":"a","layout":"en-us-full","language":"en-us"})";
    config("bg-bg", english);
    {
        TextSink sink;
        App app(options, sink);
        auto toggle = button(app);
        require(toggle && toggle->enabled && toggle->dimmed && toggle->icon == Icon::Swipe,
                "a language that can't swipe greys the icon");
        press(app, *toggle);
        toggle = button(app);
        require(toggle && toggle->dimmed && toggle->icon == Icon::SwipeOff && !saved().swipe_switch,
                "it still toggles");
        press(app, *toggle);
        app.show_settings();
        const auto view = app.view();
        const auto row = std::find_if(view.controls.begin(), view.controls.end(),
                                      [](const Control& item) { return item.id == "swipe-typing"; });
        require(row != view.controls.end() && row->enabled && row->selected,
                "Settings can turn swiping off for the language that can swipe");
    }
    // No language that can swipe: the press shakes the icon and changes nothing.
    config("bg-bg", "");
    {
        TextSink sink;
        App app(options, sink);
        const auto toggle = button(app);
        require(toggle && toggle->enabled && toggle->dimmed && toggle->icon == Icon::SwipeOff,
                "no language can swipe: greyed out and crossed");
        require(app.view().swipe_shake_since < 0, "nothing shakes yet");
        press(app, *toggle);
        require(app.view().swipe_shake_since >= 0, "a press shakes it");
        require(saved().swipe_switch, "and toggles nothing");
        app.show_settings();
        const auto view = app.view();
        const auto row = std::find_if(view.controls.begin(), view.controls.end(),
                                      [](const Control& item) { return item.id == "swipe-typing"; });
        require(row != view.controls.end() && !row->enabled, "and the option has nothing to reach");
    }
    // A flick layout: greyed out and not reactive, even with a language that can swipe.
    config("ja-flick", english, true, "ja-flick-12");
    {
        TextSink sink;
        App app(options, sink);
        const auto toggle = button(app);
        require(toggle && !toggle->enabled && toggle->dimmed && toggle->icon == Icon::SwipeOff,
                "a flick layout disables the button");
        press(app, *toggle);
        require(app.view().swipe_shake_since < 0 && saved().swipe_switch, "and ignores presses");
    }
}

// Ideal minimum-jerk path through key centers, sampled at the overlay rate.
std::vector<SwipePoint> ideal_swipe(const SwipeKeyboard& keyboard, const std::u32string& word) {
    std::vector<SwipePoint> points;
    double t = 0;
    for (std::size_t i = 0; i < word.size(); ++i) {
        const auto& to = keyboard.centers[static_cast<std::size_t>(letter(keyboard, word[i]))];
        if (i == 0) {
            points.push_back({to.x, to.y, t});
            continue;
        }
        const auto from = points.back();
        const double distance = std::hypot(to.x - from.x, to.y - from.y) / keyboard.key_width;
        const int samples = std::max(2, static_cast<int>((0.08 + 0.06 * distance) * 90));
        for (int s = 1; s <= samples; ++s) {
            const double tau = static_cast<double>(s) / samples;
            const double f = 10 * std::pow(tau, 3) - 15 * std::pow(tau, 4) + 6 * std::pow(tau, 5);
            t += 1.0 / 90;
            points.push_back({from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, t});
        }
    }
    return points;
}
int model() {
    if (!swipe_engine_installed({})) {
        std::cout << "swipe engine not built\n";
        return 77;
    }
    struct Case {
        std::string layout, language;
        std::u32string path;
        std::string word;
    };
    const std::vector<Case> cases{{"en-us-full", "en-us", U"computer", "computer"},
                                  {"en-us-full", "en-us", U"keyboard", "keyboard"},
                                  {"en-us-full", "en-us", U"hello", "hello"},
                                  {"international-full", "de-de", U"strasse", "Straße"},
                                  {"international-full", "de-de", U"über", "über"},
                                  {"international-full", "es-es", U"tambien", "también"},
                                  {"international-full", "es-es", U"año", "año"},
                                  {"en-gb-full", "en-gb", U"colour", "colour"},
                                  {"international-full", "fr-fr", U"bonjour", "bonjour"},
                                  {"international-full", "fr-fr", U"ete", "été"},
                                  {"international-full", "it-it", U"città", "città"},
                                  {"br-abnt2-full", "pt-br", U"obrigado", "obrigado"},
                                  {"br-abnt2-full", "pt-br", U"nao", "não"},
                                  {"en-us-full", "uk-ua", U"привіт", "привіт"},
                                  {"en-us-full", "ru-ru", U"привет", "привет"},
                                  {"en-us-full", "pl-pl", U"jestem", "jestem"},
                                  {"en-us-full", "pl-pl", U"łódź", "łódź"},
                                  {"en-us-full", "nl-nl", U"bedankt", "bedankt"},
                                  {"en-us-full", "nl-nl", U"ideeën", "ideeën"},
                                  {"international-full", "sv-se", U"också", "också"},
                                  {"international-full", "sv-se", U"tack", "tack"},
                                  {"en-us-full", "zh-cn-pinyin", U"zhongguo", "zhongguo"},
                                  {"en-us-full", "zh-cn-pinyin", U"nihao", "nihao"}};
    auto encoder = load_swipe_encoder(swipe_model_path({}));
    int first = 0;
    for (const auto& item : cases) {
        const auto keyboard = keyboard_for(item.layout, item.language);
        const auto lexicon = SwipeLexicon::load(
            swipe_lexicon_path({}, {}, profiles().languages.at(item.language).swipe_lexicon), keyboard);
        // "strasse" is swiped as s-t-r-a-ß-e on QWERTZ: ß has its own key.
        auto path = item.path;
        if (item.word == "Straße") {
            path = U"straße";
        }
        std::vector<float> keys(2 * swipe_max_keys);
        std::vector<unsigned char> mask(swipe_max_keys);
        for (std::size_t i = 0; i < keyboard.centers.size(); ++i) {
            keys[2 * i] =
                static_cast<float>((keyboard.centers[i].x - keyboard.frame.x) / keyboard.frame.width);
            keys[2 * i + 1] =
                static_cast<float>((keyboard.centers[i].y - keyboard.frame.y) / keyboard.frame.height);
            mask[i] = 1;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto values = swipe_features(ideal_swipe(keyboard, path), keyboard);
        int steps = 0;
        const auto log_probs = encoder->encode(values.data(), keys.data(), mask.data(), steps);
        const auto words = lexicon.decode(log_probs, steps, swipe_max_keys + 1, 5);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        std::cout << item.language << " " << item.word << ":";
        for (const auto& candidate : words) {
            std::cout << ' ' << candidate.word << '(' << candidate.score << ')';
        }
        std::cout << " [" << ms << " ms, " << lexicon.size() << " words]\n";
        const auto found = std::find_if(words.begin(), words.end(), [&](const auto& candidate) {
            return candidate.word == item.word;
        });
        require(found != words.end() && found - words.begin() < 3, "real model offers " + item.word);
        first += found == words.begin();
    }
    // Ideal synthetic paths are not human swipes; held-out human accuracy is in
    // docs/swipe-typing.md. This guards the integration, not the model.
    require(first + 1 >= static_cast<int>(cases.size()),
            "real model ranks nearly every ideal path first");
    return 0;
}
} // namespace

void tap(App& app, const std::string& action, double now = 1) {
    const auto [x, y] = key_point(app, action);
    app.down(0, x, y, now);
    app.up(0, x, y);
}
void type(App& app, const std::string& letters) {
    for (const char letter : letters) {
        tap(app, letter == ' '    ? std::string("Space")
                 : letter == '\'' ? std::string("Quote")
                 : letter == '.'  ? std::string("Period")
                 : std::isdigit(static_cast<unsigned char>(letter))
                     ? std::string("Digit") + letter
                     : std::string("Key") + static_cast<char>(std::toupper(letter)));
    }
}
std::vector<std::string> typing_suggestions(App& app) {
    std::vector<std::string> words;
    for (const auto& c : app.view().controls) {
        if (c.id.starts_with("typing-suggestion-")) {
            words.push_back(c.label);
        }
    }
    return words;
}
// Scrolls Settings' Languages card until the control shows whole.
void scroll_to(App& app, const std::string& id) {
    static double now = 100;
    for (int step = 0; step < 20; ++step) {
        const auto view = app.view();
        if (has_control(view, id)) {
            const auto c = control(view, id);
            if (!c.clip || (c.bounds.y >= c.clip->y &&
                            c.bounds.y + c.bounds.height <= c.clip->y + c.clip->height)) {
                return;
            }
        }
        const auto row = control(view, "language");
        app.move(9, row.bounds.x + 20, row.bounds.y + 20);
        app.scroll(9, 0, 1);
        for (int i = 0; i < 10; ++i) {
            app.tick(now += .05); // The card glides to its place.
        }
    }
    throw std::runtime_error("missing control " + id);
}
std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}

// Typing suggestions complete the word being typed from the language's swipe
// word list, learn finished words, and stop where the cursor may have moved.
int typing_flow() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "swipe/lexicons");
    std::ofstream(directory.path / "swipe/lexicons/en-us.tsv")
        << "hello\t200\nhelp\t180\nhelmet\t100\nheld\t90\nhelpless\t50\nworld\t150\n";
    // Held back as an offensive word would be, for this test.
    std::ofstream(directory.path / "swipe/lexicons/en-us-offensive.tsv") << "# test\nhelmet\n";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.swipe_encoder = [] { return std::unique_ptr<SwipeEncoder>(); };
    const auto learned = directory.path / "words/en-us.tsv";
    {
        TextSink sink;
        App app(options, sink);
        type(app, "hel");
        settle(app, [&] { return !typing_suggestions(app).empty(); });
        require(typing_suggestions(app) ==
                    std::vector<std::string>{"hello", "help", "held", "helpless", "helmet"},
                "up to five completions of the typed word, commonest first, offensive words last");
        click(app, "typing-suggestion-1");
        require(sink.text.back() == "p ", "a pick types the rest of the word and a space");
        require(typing_suggestions(app).empty(), "a picked word ends the suggestions");

        type(app, "hel");
        tap(app, "Backspace");
        require(typing_suggestions(app) ==
                    std::vector<std::string>{"help", "hello", "held", "helpless", "helmet"},
                "Backspace shortens the typed word, and the picked word now ranks first");
        tap(app, "Backspace");
        tap(app, "Backspace");
        require(typing_suggestions(app).empty(), "nothing is suggested for an empty word");
        tap(app, "Backspace");
        require(typing_suggestions(app) == std::vector<std::string>{"helpless"},
                "Backspace steps back into the word before");
        type(app, " he");
        require(!typing_suggestions(app).empty(), "a space starts a new word");
        tap(app, "ArrowLeft");
        require(typing_suggestions(app).empty(), "an arrow key leaves the word");
        type(app, "wo");
        require(typing_suggestions(app) == std::vector<std::string>{"world"},
                "typing after an arrow starts a new word");
        tap(app, "ArrowRight");
        tap(app, "Backspace");
        type(app, "wo");
        require(typing_suggestions(app) == std::vector<std::string>{"world"},
                "deleting text the keys didn't type, as a field's, starts a new word too");
        {
            // Held until it repeats, Backspace deletes an unknown amount.
            const auto [bx, by] = key_point(app, "Backspace");
            app.down(0, bx, by, 5);
            app.tick(5.6);
            app.up(0, bx, by);
            require(typing_suggestions(app).empty(), "a held Backspace forgets the word");
            type(app, "wo");
            require(typing_suggestions(app) == std::vector<std::string>{"world"},
                    "and typing goes on at a word's start");
            // So does a letter held into its repeat.
            tap(app, "ArrowRight");
            type(app, "he");
            const auto [lx, ly] = key_point(app, "KeyL");
            app.down(0, lx, ly, 7);
            app.tick(7.6);
            app.up(0, lx, ly);
            require(typing_suggestions(app).empty(), "a held letter forgets the word");
            // Enter forgets it too: a single-line field has no line break to
            // take back, so Backspace after it is the field's own text.
            type(app, " hel");
            tap(app, "Enter");
            tap(app, "Backspace");
            type(app, "wo");
            require(typing_suggestions(app) == std::vector<std::string>{"world"},
                    "Backspace after Enter starts a new word");
            // Two hands: the other hand's press takes SteamVR's laser, and the
            // first hand's pointer is cancelled after it.
            tap(app, "ArrowRight");
            type(app, "he");
            const auto [hx, hy] = key_point(app, "KeyL");
            app.down(1, hx, hy, 8);
            app.up(1, hx, hy);
            app.cancel_pointer(0);
            const auto chips = typing_suggestions(app);
            require(!chips.empty() && chips.front().starts_with("hel"),
                    "the word goes on when the other hand typed last");
            app.cancel_pointer(1);
            require(typing_suggestions(app).empty(), "and ends when the hand that typed it leaves");
            // The laser leaving is watched only while a word is being typed.
            type(app, "he");
            require(app.holds_swipe_state(), "a word being typed is kept while the laser points away");
            tap(app, "ArrowRight");
            require(!app.holds_swipe_state(), "with none, there is nothing to watch");
        }
        tap(app, "ArrowRight");

        // A word the list lacks is offered once finished three times. Enter
        // and digits end a word without learning it, as in a password.
        type(app, " zork zork");
        tap(app, "Enter");
        type(app, "zork1 zork ");
        type(app, "zo");
        require(typing_suggestions(app).empty(), "Enter and digits don't count a word as used");
        type(app, "rk zo");
        require(typing_suggestions(app) == std::vector<std::string>{"zork"},
                "a new word is offered after three uses");
        type(app, " 'hel");
        require(typing_suggestions(app).front() == "help", "an opening quote isn't part of the word");
        type(app, " 'quux' 'quux' 'quux' qu");
        require(typing_suggestions(app) == std::vector<std::string>{"quux"},
                "nor is a closing quote, which isn't learned with it");
        // Punctuation waits for a space: a password ending in it, then Enter,
        // isn't learned, nor is a word a letter follows (an address).
        for (int i = 0; i < 3; ++i) {
            type(app, " pqrs.");
            tap(app, "Enter");
            type(app, "pqrs.x ");
        }
        type(app, "pq");
        require(typing_suggestions(app).empty(), "a word punctuation ended waits for a space");
        type(app, "rs. pqrs. pqrs. pq");
        require(typing_suggestions(app) == std::vector<std::string>{"pqrs"}, "which confirms it");
        // A pick counts once, also when its space is taken back and the
        // sentence ends there.
        type(app, " wo");
        click(app, "typing-suggestion-0");
        tap(app, "Backspace");
        type(app, ". ");
    }
    {
        // Clear learned words lists the language with its count, off; Clear
        // forgets only the languages switched on.
        fs::create_directories(directory.path / "words");
        std::ofstream(directory.path / "words/de-de.tsv") << "Straße\t4\t1\n";
        fs::permissions(directory.path / "words/de-de.tsv",
                        fs::perms::owner_read | fs::perms::owner_write);
        TextSink sink;
        App app(options, sink);
        app.show_settings();
        scroll_to(app, "clear-learning");
        click(app, "clear-learning");
        const auto popup = app.view().confirm;
        require(popup && popup->rows.size() == 2 && popup->rows[0].title == "English (US)" &&
                    popup->rows[1].title == "German" && popup->rows[1].note == "1 word you typed" &&
                    !control(app.view(), "confirm-accept").enabled,
                "each language's words show by name and count, none chosen");
        click(app, "confirm-row-1");
        click(app, "confirm-accept");
        require(!app.view().confirm && app.view().status == "Learned words cleared." &&
                    !fs::exists(directory.path / "words/de-de.tsv") && fs::exists(learned),
                "only the chosen language is forgotten");
    }
    const auto saved = read_file(learned);
    require(saved.find("help\t") != std::string::npos && saved.find("zork\t") != std::string::npos,
            "picked and finished words are saved on exit");
    require(saved.find("world\t1\t") != std::string::npos, "a word typed once counts once");
    require(saved.find("wo\t") == std::string::npos && saved.find("he\t") == std::string::npos,
            "words cut short by an arrow or Backspace are not learned");
    require((fs::status(learned).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
                fs::perms::none,
            "the learned words are the owner's alone");

    {
        // In a password field the swipe button is a lock, and nothing is
        // suggested.
        Settings swiping;
        swiping.swipe_typing = true;
        save_settings(directory.path, swiping);
        TextSink sink;
        App app(options, sink);
        App::HostHints password;
        password.purpose = "password";
        app.set_context(std::string("pw"), password);
        require(control(app.view(), "swipe-toggle").icon == Icon::Lock, "a lock says why");
        type(app, "hel");
        settle(app, [&] { return false; }, 100);
        require(typing_suggestions(app).empty(), "a password field gets no suggestions");
        app.set_context(std::nullopt);
        require(control(app.view(), "swipe-toggle").icon != Icon::Lock,
                "other fields get the switch back");
        fs::remove(directory.path / "config.json");
    }
    // Switched off in Settings, nothing is offered or learned.
    fs::remove(learned);
    Settings settings;
    settings.typing_suggestions = false;
    save_settings(directory.path, settings);
    {
        TextSink sink;
        App app(options, sink);
        type(app, "hel");
        settle(app, [&] { return false; }, 100);
        require(typing_suggestions(app).empty(), "suggestions switched off offer nothing");
        type(app, "lo ");
    }
    require(!fs::exists(learned), "suggestions switched off learn nothing");
    std::cout << "typing suggestion tests passed\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "--core";
        if (mode == "--typing") {
            return typing_flow();
        }
        if (mode == "--model") {
            return model();
        }
        if (mode == "--chinese") {
            return chinese_flow();
        }
        keyboard_geometry();
        dvorak_geometry();
        lexicon_paths();
        features();
        disabled_by_default();
        languages_popup_marks();
        toolbar_toggle();
        swipes_in_the_editor();
        swiping_over_a_selection();
        swipes_after_listening();
        swipes_wait_for_recognition();
        held_tap_before_dictation_keys();
        editor_in_preview();
        service_reuse();
        encoder_release();
        leave_while_decoding();
        suggestions_clear_the_toolbar();
        long_gestures();
        failed_replacement();
        failure_found_by_click(true, 0);
        failure_found_by_click(false, 0);
        failure_found_by_click(false, 1); // Found by a later delivery check of the click.
        loading_does_not_block();
        app_flow(false);
        app_flow(true);
        text_output_capture();
        swipes_between_keys();
        swipes_from_key_areas();
        trail_repaint();
        std::cout << "swipe tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
