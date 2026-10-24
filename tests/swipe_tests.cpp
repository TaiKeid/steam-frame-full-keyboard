#include "framekeyboard/app.hpp"
#include "framekeyboard/swipe.hpp"

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
    std::vector<float> encode(const float* features, const float*, const unsigned char* mask,
                              int& steps) override {
        ++*calls;
        for (int i = 0; i < 2 * swipe_points; ++i) {
            require(std::isfinite(features[i]), "features are finite");
        }
        require(mask[0] == 1, "key mask marks letters");
        return emissions(*path, steps);
    }
};

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
    for (const auto c : std::u32string(U"äöüß")) {
        require(de.index_of(c) >= 0, "German letters have their own keys");
    }
    require(de.centers[static_cast<std::size_t>(letter(de, U'z'))].x ==
                us.centers[static_cast<std::size_t>(letter(us, U'y'))].x,
            "German keymap places Z on the QWERTZ key");
    require(letter(de, U'Ä') == letter(de, U'ä'), "German uppercase umlauts map to their key");

    const auto fr = keyboard_for("international-full", "fr-fr");
    const int e_acute = letter(fr, U'é');
    require(e_acute != letter(fr, U'e') && fr.centers[static_cast<std::size_t>(e_acute)].y < fr.frame.y,
            "AZERTY é is its own key on the number row");
    require(fr.alternate[static_cast<std::size_t>(e_acute)] == letter(fr, U'e'),
            "a number-row accent also accepts its plain letter");
    require(fr.alternate[static_cast<std::size_t>(letter(fr, U'ù'))] == -1,
            "accents inside the letter block stay single keys");
    require(fr.alternate[static_cast<std::size_t>(letter(fr, U'a'))] == -1,
            "plain letters have one path");
    const auto uk = keyboard_for("en-us-full", "uk-ua");
    require(letter(uk, U'ґ') != letter(uk, U'г'), "Ukrainian ґ has its own key");
    require(letter(uk, U'ё') == letter(uk, U'е'), "an AltGr letter is swiped on the key that types it");
    const auto ru = keyboard_for("en-us-full", "ru-ru");
    require(ru.alternate[static_cast<std::size_t>(letter(ru, U'ё'))] == letter(ru, U'е'),
            "Russian ё on the number row also accepts е");
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
    for (const auto* path : {U"été", U"ete"}) {
        log_probs = emissions(path_of(fr, path), steps);
        words = accents.decode(log_probs, steps, swipe_max_keys + 1, 5);
        require(!words.empty() && words[0].word == "été" &&
                    std::count_if(words.begin(), words.end(),
                                  [](const auto& c) { return c.word == "été"; }) == 1,
                "a number-row accent decodes from either path, listed once");
    }
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
    std::vector<std::size_t> replaced;
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        text.push_back(value);
        return true;
    }
    bool replaces_text() override { return ordered_replace; }
    bool replace_text(std::size_t delete_before, const std::string& value) override {
        replaced.push_back(delete_before);
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
template <class Done> void settle(App& app, Done done) {
    for (int i = 0; i < 200 && !done(); ++i) {
        app.tick(monotonic_seconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
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
    const auto count = sink.text.size();
    swipe(app, {"KeyH", "KeyE", "KeyL", "KeyO"}, 7);
    app.cancel();
    settle(app, [&] { return false; });
    require(sink.text.size() == count, "a decode cancelled by hiding is discarded");

    // Leaving the panel mid-swipe drops the path, and the release types nothing.
    auto [hx, hy] = key_point(app, "KeyH");
    const auto [ex, ey] = key_point(app, "KeyE");
    app.down(0, hx, hy, 8);
    app.move(0, ex, ey);
    app.cancel_pointer(0);
    app.up(0, ex, ey);
    settle(app, [&] { return false; });
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
    app.cancel();
}
// Needs the Pinyin engine module, which Frame builds include.
// Unchanged word lists are not parsed again; edits and a release load them anew.
void service_reuse() {
    TemporaryDirectory directory;
    const auto path = directory.path / "words.tsv";
    std::ofstream(path) << "hello\t200\n";
    SwipeService service([] { return std::unique_ptr<SwipeEncoder>(); });
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
    require(!app.view().preedit.empty() && has_control(app.view(), "cjk-candidate-0"),
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

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "--core";
        if (mode == "--model") {
            return model();
        }
        if (mode == "--chinese") {
            return chinese_flow();
        }
        keyboard_geometry();
        lexicon_paths();
        features();
        disabled_by_default();
        service_reuse();
        loading_does_not_block();
        app_flow(false);
        app_flow(true);
        trail_repaint();
        std::cout << "swipe tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
