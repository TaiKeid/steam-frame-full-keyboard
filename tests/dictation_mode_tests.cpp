#include "framekeyboard/app.hpp"
#include "framekeyboard/dictation_mode.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-dictation-mode-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
// Records everything that would reach the target, and fails on request.
struct Sink : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    bool single_line{}, fail_enter{};
    int commits_before_failure{-1}; // -1 never fails
    std::size_t acknowledged{};     // characters of a failed commit that arrived
    void send(int code, int value) override { keys.emplace_back(code, value); }
    Delivery deliver_key(int code, int value) override {
        if (fail_enter && code == KEY_ENTER && value == 1) {
            return Delivery::Failure;
        }
        send(code, value);
        return Delivery::Success;
    }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        if (commits_before_failure == 0) {
            return false;
        }
        if (commits_before_failure > 0) {
            --commits_before_failure;
        }
        text.push_back(value);
        return true;
    }
    std::size_t delivered_characters() const override { return acknowledged; }
    bool single_line_target() const override { return single_line; }
    std::vector<double> gaps;
    void set_character_gap(double seconds) override { gaps.push_back(seconds); }
};
void write_config(const fs::path& directory, const std::string& extra) {
    std::ofstream(directory / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},)"
        << R"("hide_numpad":false)" << extra << "}";
}
const Key* find(const PanelView& view, const std::string& id) {
    for (const auto& key : view.layout->keys) {
        if (key.id == id) {
            return &key;
        }
    }
    return nullptr;
}
std::pair<double, double> key_center(App& app, const std::string& id) {
    const auto view = app.view();
    const auto* key = find(view, id);
    require(key && !hidden_key(view, *key), "the key is shown");
    const auto r = panel_rect(view, key->bounds);
    return {r.x + r.width / 2, r.y + r.height / 2};
}
double clock = 10;
void press(App& app, const std::string& id, unsigned pointer = 0) {
    const auto [x, y] = key_center(app, id);
    app.down(pointer, x, y, clock);
    app.up(pointer, x, y);
    clock += .1;
    app.tick(clock);
}
void settle(App& app) {
    for (int i = 0; i < 100; ++i) {
        app.tick(clock += .02);
    }
}
std::string editor_text(App& app) {
    const auto view = app.view();
    require(view.dictation.has_value(), "dictation mode shows its editor");
    return view.dictation->text;
}

void settings_tests() {
    TemporaryDirectory directory;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"speech.local:8031")");
    std::vector<std::string> errors;
    auto settings = load_settings(directory.path, errors);
    require(settings.dictation_send_gap == 40, "Send's pace defaults to 40 ms");
    require(errors.empty() && settings.dictation == "server" &&
                settings.dictation_server == "speech.local:8031",
            "dictation settings load");
    write_config(directory.path,
                 R"(,"hide_numpad":true,"dictation":"server","dictation_url":"ftp://x")");
    settings = load_settings(directory.path, errors);
    require(errors.size() == 1 && settings.dictation == "off" && settings.dictation_url.empty(),
            "an invalid dictation field turns dictation off with an error");
    require(settings.hide_numpad && settings.active.layout == "en-us-full",
            "an invalid dictation field leaves every other setting alone");
    for (const auto* server : {"host", "host:8031", "192.168.1.20", "[fd00::1]:9000", "a-b.local"}) {
        require(valid_dictation_server(server), "valid server addresses pass");
    }
    for (const auto* server : {"", "host:", "host:99999", "a b", "[fd00::1", "http://host"}) {
        require(!valid_dictation_server(server), "invalid server addresses fail");
    }
    require(valid_dictation_url("https://api.example.com/openai/v1") &&
                !valid_dictation_url("https://u@x/v1") && !valid_dictation_url("https://x/v1?key=1"),
            "dictation URLs");
    const auto profiles = load_profiles({}, {});
    require(profiles.languages.at("en-us").dictation_language == "en" &&
                profiles.languages.at("ja-romaji").dictation_language.empty(),
            "English profiles offer dictation; others wait for later milestones");
}

void layout_tests() {
    const auto profiles = load_profiles({}, {});
    const auto& main = profiles.layouts.at("en-us-full");
    const auto mode = dictation_layout(main, main.width);
    auto has = [&](const char* id) {
        return std::any_of(mode.layout.keys.begin(), mode.layout.keys.end(),
                           [&](const Key& key) { return key.id == id; });
    };
    for (const auto* gone : {"F1", "F12", "PrintScreen", "Tab", "Insert", "ControlLeft", "MetaLeft",
                             "AltRight", "ContextMenu", "ControlRight", "Copy", "Paste", "Emoji"}) {
        require(!has(gone), "dictation mode drops keys that cannot edit text");
    }
    for (const auto* kept :
         {"Escape", "KeyA", "Backspace", "Enter", "ShiftLeft", "CapsLock", "Home", "PageUp", "ArrowUp",
          "Numpad7", "Dictation", "Dictation-send", "Dictation-undo"}) {
        require(has(kept), "dictation mode keeps editing keys and adds its own");
    }
    const auto& zero = *std::find_if(mode.layout.keys.begin(), mode.layout.keys.end(),
                                     [](const Key& key) { return key.id == "Numpad0"; });
    const auto& decimal = *std::find_if(mode.layout.keys.begin(), mode.layout.keys.end(),
                                        [](const Key& key) { return key.id == "NumpadDecimal"; });
    require(zero.secondary_label.empty() && decimal.secondary_label == "Del",
            "the numpad keeps Del but loses Ins, which the editor cannot use");
    const auto& send = *std::find_if(mode.layout.keys.begin(), mode.layout.keys.end(),
                                     [](const Key& key) { return key.id == "Dictation-send"; });
    require(send.bounds.x == 724 && send.bounds.x + send.bounds.width == 1020,
            "Send spans right Alt to Ctrl");
    require(mode.editor.y < 0 && mode.editor.x > 174 && mode.editor.x + mode.editor.width == main.width,
            "the editor runs from Esc to the right edge and rises above the keys");
}

// A stand-in helper: capture on starts a phrase, capture off ends it, and
// each prompt is answered with the next scripted text. `fail` reports an
// unreachable server instead, and `silent` never hears anything. It never
// touches audio or the network; every App here uses it, so no test can
// start the real helper and its recorder.
const char* fake_helper = R"PY(
import json, socket, sys
peer = socket.socket(fileno=3)
scenario = sys.argv[sys.argv.index("--scenario") + 1]
replies = ["Hello there.", "Again."]
phrase = 0
def send(**m):
    peer.send(json.dumps(m).encode())
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    op = message["op"]
    if scenario == "silent":
        continue
    if op == "capture" and message["on"]:
        if scenario == "fail":
            send(op="error", reason="unreachable")
            continue
        phrase += 1
        send(op="level", value=200)
        send(op="phrase", id=phrase)
    elif op == "capture":
        send(op="ready", id=phrase)
    elif op == "prompt":
        send(op="text", id=message["id"], text=replies[(message["id"] - 1) % len(replies)])
    elif op == "cancel":
        send(op="cancelled")
)PY";
void use_fake_helper(Options& options, const fs::path& directory, const char* scenario) {
    const auto script = directory / "fake_helper.py";
    std::ofstream(script) << fake_helper;
    options.dictation_helper = script;
    options.dictation_arguments = {"--scenario", scenario};
}
// Opens dictation mode with the mic key, which starts listening, and pauses.
void open_editor(App& app) {
    press(app, "Dictation");
    press(app, "Dictation");
}

void mode_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    {
        write_config(directory.path, "");
        Sink sink;
        App app(options, sink);
        const auto view = app.view();
        require(!view.dictation_available && hidden_key(view, *find(view, "Dictation")),
                "the mic key stays hidden while dictation is off");
    }
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    Sink sink;
    App app(options, sink);
    require(app.view().dictation_available, "the mic key shows once dictation is configured");
    press(app, "Dictation");
    require(app.dictation_open() && app.view().dictation->capturing,
            "the mic key opens dictation mode and listens");
    press(app, "Dictation");
    require(app.view().disabled_keys.contains("Dictation-send") &&
                app.view().disabled_keys.contains("Dictation-undo"),
            "Send needs text; Undo needs a phrase");

    press(app, "ShiftLeft");
    for (const auto* id : {"KeyH", "KeyI", "Space", "KeyT", "KeyO"}) {
        press(app, id);
    }
    require(editor_text(app) == "Hi to", "letters, Shift and Space type into the editor");
    press(app, "Backspace");
    press(app, "Enter");
    press(app, "Digit2");
    require(editor_text(app) == "Hi t\n2", "Backspace and Enter edit the editor");
    press(app, "ArrowLeft");
    press(app, "ArrowLeft");
    press(app, "KeyX");
    require(editor_text(app) == "Hi tx\n2", "arrows move the cursor");
    press(app, "Home");
    press(app, "KeyY");
    require(editor_text(app) == "yHi tx\n2", "Home goes to the line start");
    press(app, "Delete");
    require(editor_text(app) == "yi tx\n2", "Delete removes the character after the cursor");
    press(app, "ArrowDown");
    press(app, "End");
    press(app, "Period");
    require(editor_text(app) == "yi tx\n2.", "Down and End reach the next line's end");

    // Holding Backspace repeats after half a second, without catching up.
    {
        const auto [x, y] = key_center(app, "Backspace");
        app.down(0, x, y, clock);
        app.tick(clock + .3);
        require(editor_text(app) == "yi tx\n2", "one deletion on press");
        app.tick(clock + .6);
        app.tick(clock + .65);
        require(editor_text(app) == "yi tx", "held Backspace repeats");
        app.up(0, x, y);
        clock += 1;
        app.tick(clock);
        require(editor_text(app) == "yi tx", "release stops the repeat");
    }

    // A click in the editor places the cursor.
    {
        const auto view = app.view();
        const auto area = editor_text_area(view);
        app.down(1, area.x + 1, area.y + 4, clock);
        app.up(1, area.x + 1, area.y + 4);
        press(app, "KeyZ");
        require(editor_text(app) == "zyi tx", "a click at the start puts the cursor there");
    }

    require(sink.text.empty() && sink.keys.empty(),
            "nothing typed in dictation mode reaches the target");

    press(app, "Escape");
    press(app, "Escape");
    require(!app.dictation_open() && sink.keys.empty(),
            "Esc, asked twice, discards and leaves without reaching the target");
    press(app, "KeyQ");
    require(sink.text == std::vector<std::string>{"q"},
            "after the mode, keys type into the target again");
    press(app, "Dictation");
    require(app.dictation_open() && editor_text(app).empty(), "the discarded text is gone");

    app.apply({"en-us-full", "ja-romaji", "graphite"});
    app.tick(clock += .1);
    require(!app.dictation_open() && !app.view().dictation_available,
            "a profile without dictation closes the mode and hides the mic key");
}
void type_text(App& app, const std::string& text) {
    // The editor's own typing path, as keys would type it.
    for (char c : text) {
        if (c == '\n') {
            press(app, "Enter");
        } else if (c == ' ') {
            press(app, "Space");
        } else {
            press(app, std::string("Key") + static_cast<char>(c - 'a' + 'A'));
        }
    }
}
using Keys = std::vector<std::pair<int, int>>;

// Ticks with real pauses until the helper's answers have arrived.
template <class Done> void wait_until(App& app, Done done) {
    for (int i = 0; i < 300 && !done(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        app.tick(clock += .01);
    }
    require(done(), "the fake helper answered");
}
void click(App& app, const std::string& id) {
    for (const auto& control : app.view().controls) {
        if (control.id == id) {
            const double x = control.bounds.x + control.bounds.width / 2;
            const double y = control.bounds.y + control.bounds.height / 2;
            app.down(0, x, y, clock);
            app.up(0, x, y);
            app.tick(clock += .1);
            return;
        }
    }
    throw std::runtime_error("missing control " + id);
}
bool control_enabled(App& app, const std::string& id) {
    for (const auto& control : app.view().controls) {
        if (control.id == id) {
            return control.enabled;
        }
    }
    return false;
}

void live_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "main");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    {
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dictation_open() && app.view().dictation->capturing,
                "the mic key opens the mode and starts listening");
        wait_until(app, [&] { return app.view().dictation_levels.back() > 0; });
        require(app.view().dictation_levels.size() == 6 && app.view().dictation_levels.front() == 0,
                "the newest level slides in on the right of five bars");
        require(app.view().dictation_capturing && !control_enabled(app, "settings"),
                "the mic key shows the level; Settings waits");
        press(app, "KeyA");
        require(editor_text(app).empty(), "the editor is read-only while listening");
        press(app, "Dictation");
        wait_until(app, [&] { return !editor_text(app).empty(); });
        require(editor_text(app) == "Hello" && app.view().dictation->revealing,
                "a phrase appears word by word");
        press(app, "KeyA"); // ignored; the press's tick shows the rest
        require(editor_text(app) == "Hello there." && !app.view().dictation->capturing &&
                    !app.view().dictation->revealing,
                "pausing recognizes the phrase and inserts it, typing waiting meanwhile");
        require(control_enabled(app, "settings"), "Settings works again when paused");
        press(app, "Dictation");
        require(app.view().dictation->capturing, "the mic key resumes");
        press(app, "Dictation-send");
        wait_until(app, [&] { return !app.dictation_open(); });
        require(sink.text == std::vector<std::string>{"Hello there. Again."},
                "Send while listening waits for the phrase, then sends everything");
    }
    {
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Escape");
        require(app.dictation_open() && !app.view().dictation->capturing && editor_text(app).empty(),
                "Esc while listening stops and drops the phrase, staying in the mode");
        press(app, "Escape");
        require(!app.dictation_open(), "Esc with an empty editor leaves");
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return !editor_text(app).empty() && !app.view().dictation->revealing; });
        const auto said = editor_text(app);
        press(app, "Escape");
        require(app.view().dictation->notice == "Discard text?", "Esc with text asks first");
        press(app, "KeyB");
        require(editor_text(app) == said, "the question ignores other keys");
        click(app, "dictation-keep");
        require(app.dictation_open() && app.view().dictation->notice.empty() && editor_text(app) == said,
                "Keep returns to the editor");
        press(app, "Escape");
        press(app, "Escape");
        require(!app.dictation_open(), "Esc again discards");
        press(app, "Dictation");
        require(editor_text(app).empty(), "the discarded text is gone");
        press(app, "Dictation");
        wait_until(app, [&] { return !editor_text(app).empty() && !app.view().dictation->revealing; });
        click(app, "close");
        require(!app.dictation_open(), "X ends the mode");
        app.set_interaction_active(true);
        press(app, "Dictation");
        require(editor_text(app).empty(), "and discards its text without asking");
        require(sink.text.empty() && sink.keys.empty(), "nothing reached the target");
    }
    {
        options.dictation_arguments = {"--scenario", "fail"};
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return !app.view().dictation->capturing; });
        const auto view = app.view();
        require(view.dictation->notice_error &&
                    view.dictation->notice == "The speech server cannot be reached.",
                "a recognizer failure stops listening and says why");
    }
}

void scroll_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "silent");
    Sink sink;
    App app(options, sink);
    open_editor(app);
    type_text(app, "a\nb\nc\nd\ne\nf\ng\nh");
    auto view = app.view();
    const auto bounds = view.dictation->bounds;
    const double x = bounds.x + bounds.width / 2, y = bounds.y + bounds.height / 2;
    app.move(0, x, y);
    require(view.dictation->scroll > 0 && app.scroll_room(0) && app.scroll_room(0)->back &&
                !app.scroll_room(0)->forward,
            "a tall text scrolls, following the cursor to the end");
    require(app.scroll(0, 0, -100) && app.view().dictation->scroll == 0,
            "stick steps scroll back to the top");
    require(app.scroll(0, 0, 1) && app.view().dictation->scroll > 0, "one step scrolls a line");
    const double line = app.view().dictation->scroll;
    // The laser drags the text like a touch screen.
    app.down(1, x, y, clock);
    app.move(1, x, y - 2 * line);
    require(app.view().dictation->scroll == 3 * line, "dragging up scrolls down");
    const auto text = editor_text(app);
    const auto cursor = app.view().dictation->cursor;
    app.up(1, x, y - 2 * line);
    require(app.view().dictation->cursor == cursor && editor_text(app) == text,
            "a drag does not move the cursor");
    app.down(1, bounds.x + 20, bounds.y + 12, clock);
    app.up(1, bounds.x + 20, bounds.y + 12);
    require(app.view().dictation->cursor != cursor, "a tap still places the cursor");
}

void send_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "silent");

    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, "\nhi\n\nthere\n");
        require(!app.view().disabled_keys.contains("Dictation-send"), "Send is ready with text");
        press(app, "Dictation-send");
        settle(app);
        require(sink.text == std::vector<std::string>{"hi", "there"}, "the text arrives in order");
        const Keys shift_enter{{KEY_LEFTSHIFT, 1}, {KEY_ENTER, 1}, {KEY_ENTER, 0}, {KEY_LEFTSHIFT, 0}};
        Keys expected = shift_enter;
        expected.insert(expected.end(), shift_enter.begin(), shift_enter.end());
        expected.insert(expected.end(), {{KEY_ENTER, 1}, {KEY_ENTER, 0}});
        require(sink.keys == expected, "inner breaks are Shift+Enter; only the last Enter is plain");
        require(!app.dictation_open(), "a sent text returns to the normal keyboard");
        require(!sink.gaps.empty() && sink.gaps.front() == .04 && sink.gaps.back() == .02,
                "Send types at its own pace, 40 ms by default, then restores the usual one");
        open_editor(app);
        require(editor_text(app).empty(), "and leaves the editor empty");
    }
    {
        Sink sink;
        sink.single_line = true;
        App app(options, sink);
        open_editor(app);
        type_text(app, "a\n\nb");
        press(app, "Dictation-send");
        settle(app);
        require(sink.text == std::vector<std::string>{"a", " ", "b"} &&
                    sink.keys == Keys{{KEY_ENTER, 1}, {KEY_ENTER, 0}},
                "a single-line field gets one space for the breaks");
    }
    {
        // A key held by the other hand delays Send; the editor is locked
        // while it sends, and Esc stops it with the rest kept.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'a') + " " + std::string(40, 'b'));
        const auto [hx, hy] = key_center(app, "ArrowLeft");
        app.down(1, hx, hy, clock);
        press(app, "Dictation-send");
        settle(app);
        require(sink.text.empty() && app.view().dictation->sending, "Send waits for the held key");
        app.up(1, hx, hy);
        app.tick(clock += .02);
        require(sink.text.size() == 1 && editor_text(app).size() == 81 - 32,
                "delivered text leaves the editor as it goes");
        press(app, "KeyZ");
        require(editor_text(app).size() == 81 - 64, "the editor ignores keys while it sends");
        press(app, "Escape");
        settle(app);
        require(app.dictation_open() && !app.view().dictation->sending &&
                    editor_text(app) == std::string(17, 'b'),
                "Esc stops sending; what was not sent stays");
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.text.back() == std::string(17, 'b'),
                "Send continues with the rest");
    }
    {
        // A commit sent one character at a time fails after three of them.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'c'));
        sink.commits_before_failure = 1;
        sink.acknowledged = 3;
        press(app, "Dictation-send");
        settle(app);
        const auto view = app.view();
        require(app.dictation_open() && view.dictation->text == std::string(5, 'c') &&
                    view.dictation->uncertain == 1,
                "only the unacknowledged rest stays, its first character marked");
        require(!app.view().disabled_keys.contains("Dictation-send"), "Send can try again");
        sink.commits_before_failure = -1;
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.text.back() == std::string(5, 'c'),
                "the retry sends only the rest");
    }
    {
        // Enter fails after all text arrived: a retry presses only Enter.
        Sink sink;
        sink.fail_enter = true;
        App app(options, sink);
        open_editor(app);
        type_text(app, "done");
        press(app, "Dictation-send");
        settle(app);
        require(app.dictation_open() && editor_text(app).empty() && sink.text.size() == 1,
                "the text left the editor; the mode stays");
        require(!app.view().disabled_keys.contains("Dictation-send"),
                "Send stays available for the Enter");
        sink.fail_enter = false;
        sink.keys.clear();
        press(app, "Dictation-send");
        settle(app);
        require(sink.text.size() == 1 && sink.keys == Keys{{KEY_ENTER, 1}, {KEY_ENTER, 0}} &&
                    !app.dictation_open(),
                "the retry presses Enter without the text");
    }
}
} // namespace

int main() {
    try {
        settings_tests();
        layout_tests();
        mode_tests();
        send_tests();
        scroll_tests();
        live_tests();
        std::cout << "Dictation mode tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
