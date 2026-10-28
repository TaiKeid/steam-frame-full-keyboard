#include "framekeyboard/app.hpp"
#include "framekeyboard/cjk.hpp"
#include "framekeyboard/dictation_mode.hpp"
#include "framekeyboard/shell_sink.hpp"
#include "framekeyboard/text_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <map>
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
        // Like the local text path, which holds at most 32 characters at once.
        if (busy || g_utf8_strlen(value.c_str(), -1) > 32) {
            return false;
        }
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
    // A busy target holds the text being delivered, so later entries queue.
    bool busy{};
    bool text_pending() const override { return busy; }
    bool single_line_target() const override { return single_line; }
    bool steam_field{};
    bool steam_field_target() const override { return steam_field; }
    std::vector<double> gaps;
    void set_character_gap(double seconds) override { gaps.push_back(seconds); }
};
void write_config(const fs::path& directory, const std::string& extra,
                  const std::string& language = "en-us", const std::string& layout = "en-us-full") {
    std::ofstream(directory / "config.json")
        << R"({"schema_version":1,"active":{"layout":")" << layout << R"(","language":")" << language
        << R"(","theme":"graphite"},"hide_numpad":false)" << extra << "}";
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
    require(settings.dictation_send_gap == 20 && !settings.dictation_send_paste,
            "Send types at 20 ms by default");
    write_config(directory.path, "");
    require(load_settings(directory.path, errors).dictation == "device" &&
                load_settings(directory.path, errors).dictation_device_model == "small-q8_0",
            "dictation is on device by default, with the 8-bit model");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"speech.local:8031")");
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
    for (const auto* local : {"192.168.1.20:8031", "10.1.2.3", "172.20.0.1", "127.0.0.1", "100.100.1.2",
                              "169.254.1.1", "[fd00::1]:9000", "[::1]", "speech", "speech.local",
                              "box.lan", "pc.tail1234.ts.net", "nas.home.arpa"}) {
        require(local_dictation_server(local), "private, Tailscale and local names count as local");
    }
    for (const auto* remote :
         {"8.8.8.8", "172.32.0.1", "100.128.0.1", "speech.example.com", "[2001:db8::1]:8031"}) {
        require(!local_dictation_server(remote), "public addresses do not");
    }
    write_config(directory.path,
                 R"(,"dictation_recent_servers":["a.local","bad host",7,"b.local:9","c","d","e","f"])");
    settings = load_settings(directory.path, errors);
    require(settings.dictation_recent_servers ==
                std::vector<std::string>{"a.local", "b.local:9", "c", "d", "e"},
            "recent servers keep five valid entries");
    require(valid_dictation_url("https://api.example.com/openai/v1") &&
                !valid_dictation_url("https://u@x/v1") && !valid_dictation_url("https://x/v1?key=1"),
            "dictation URLs");
    const auto profiles = load_profiles({}, {});
    const auto& languages = profiles.languages;
    require(languages.at("en-us").dictation_language == "en" &&
                languages.at("en-us").dictation_prompt.empty() &&
                languages.at("es-es").dictation_language == "es" &&
                languages.at("es-es").dictation_prompt.empty(),
            "English and Spanish dictate with the editor's context alone");
    for (const auto* id : {"ja-romaji", "ja-kana", "ja-flick"}) {
        require(languages.at(id).dictation_language == "ja" &&
                    languages.at(id).dictation_prompt == "はい、そうですね。",
                "Japanese prompts for punctuation");
    }
    require(languages.at("zh-cn-pinyin").dictation_prompt == "好的，没问题。" &&
                languages.at("zh-tw-pinyin").dictation_prompt == "好的，沒問題。" &&
                languages.at("zh-tw-pinyin").dictation_language == "zh",
            "Chinese prompts fix the script");
    require(languages.at("ja-jis").dictation_language.empty(),
            "external JIS, which types through a system IME, has no dictation");
    for (const auto& [id, code] : std::map<std::string, std::string>{
             {"de-de", "de"}, {"fr-fr", "fr"}, {"it-it", "it"}, {"pt-br", "pt"}, {"ru-ru", "ru"},
             {"uk-ua", "uk"}, {"ko-kr", "ko"}, {"ar", "ar"},    {"cs-cz", "cs"}, {"da-dk", "da"},
             {"el-gr", "el"}, {"et-ee", "et"}, {"fi-fi", "fi"}, {"fr-ca", "fr"}, {"he-il", "he"},
             {"hi-in", "hi"}, {"hr-hr", "hr"}, {"hu-hu", "hu"}, {"id-id", "id"}, {"lt-lt", "lt"},
             {"lv-lv", "lv"}, {"ms-my", "ms"}, {"nb-no", "no"}, {"nl-nl", "nl"}, {"pl-pl", "pl"},
             {"ro-ro", "ro"}, {"sk-sk", "sk"}, {"sl-si", "sl"}, {"sv-se", "sv"}, {"th-th", "th"},
             {"tr-tr", "tr"}, {"vi-vn", "vi"}}) {
        require(languages.at(id).dictation_language == code && languages.at(id).dictation_prompt.empty(),
                "every other profile dictates in its own language, without a prompt");
    }
    auto profile = [](const std::string& prompt) {
        return R"({"schema_version":1,"id":"xx-test","name":"Test","locale":"en-US",
            "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
            "legends":{"source":"keymap","overrides":{}},"font_families":["sans-serif"],
            "dictation":{"language":"en","prompt":")" +
               prompt + R"("}})";
    };
    std::string longest;
    for (int i = 0; i < 60; ++i) {
        longest += "é";
    }
    require(parse_language(profile(longest)).dictation_prompt == longest,
            "a prompt holds 60 characters");
    bool rejected = false;
    try {
        parse_language(profile(longest + "e"));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "a longer prompt is rejected");
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
    const auto& flick = profiles.layouts.at("ja-flick-12");
    const auto flick_mode = dictation_layout(flick, flick.width);
    const auto flick_send = std::find_if(flick_mode.layout.keys.begin(), flick_mode.layout.keys.end(),
                                         [](const Key& key) { return key.id == "Dictation-send"; });
    require(flick_send != flick_mode.layout.keys.end() && flick_send->bounds == Rect{120, 138, 150, 62},
            "without right-hand modifiers, Send takes Tab's slot");
}

// A stand-in helper: capture on starts a phrase, capture off ends it, and
// each prompt is answered with the next scripted text. `fail` reports an
// unreachable server instead, and `silent` never hears anything. It never
// touches audio or the network; every App here uses it, so no test can
// start the real helper and its recorder.
const char* fake_helper = R"PY(
import json, os, socket, sys
peer = socket.socket(fileno=3)
# Every prompt the keyboard sends, NUL-separated, for the tests to read.
prompts = open(os.path.join(os.path.dirname(sys.argv[0]), "prompts"), "ab", buffering=0)
scenario = sys.argv[sys.argv.index("--scenario") + 1]
replies = ["Hello there.", "Again."]
if scenario == "long":
    replies = ["This phrase is much longer than thirty-two characters, so it goes out in pieces."]
if scenario == "live":
    replies = ["Hello there, how are you?"]
if scenario == "liveshift":
    replies = ["I have 25 dollars and fifty cents."]
if scenario == "verylong":
    replies = [" ".join("word%d" % i for i in range(60))]
phrase = 0
def send(**m):
    peer.send(json.dumps(m).encode())
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    op = message["op"]
    if op == "capture" and not message["on"]:
        if scenario != "silent":
            send(op="ready", id=phrase)
        send(op="paused")
        continue
    if op == "test":
        if scenario == "testexit":
            sys.exit(0)  # gone before answering
        if scenario == "slowtest":
            import time
            time.sleep(0.5)  # the address is edited before the answer comes
        send(op="test", ok=scenario != "fail", **({} if scenario != "fail" else {"reason": "unreachable"}))
        continue
    if op == "configure":
        with open(os.path.join(os.path.dirname(sys.argv[0]), "configures"), "a") as log:
            log.write(json.dumps(message) + "\n")
        continue
    if op == "download" and scenario == "diedownloading":
        send(op="download", state="progress", done=50, total=100)
        import time
        time.sleep(0.2)
        sys.exit(1)
    if op == "download":
        # Halfway at once, then the verified file half a second later.
        send(op="download", state="progress", done=50, total=100)
        import time
        time.sleep(0.5)
        os.makedirs(message["directory"], exist_ok=True)
        open(os.path.join(message["directory"], "ggml-small-q8_0.bin"), "wb").write(b"model")
        send(op="download", state="ready")
        continue
    if scenario == "silent":
        continue
    if op == "capture" and message["on"]:
        if scenario == "fail":
            send(op="error", reason="unreachable")
            continue
        if scenario == "game":
            send(op="microphone", unfiltered=True)
        phrase += 1
        send(op="level", value=200)
        send(op="phrase", id=phrase)
        if scenario == "liveshift":
            # Whisper rewrites words it already guessed three times.
            for guess in ["I have twenty five dollars"] * 3 + ["I have 25 dollars and fifty"] * 3:
                send(op="partial", id=phrase, text=guess)
        if scenario == "live":
            # Growing guesses while the phrase is spoken.
            for guess in ["Hello th", "Hello there how", "Hello there how are",
                          "Hello there how are you"]:
                send(op="partial", id=phrase, text=guess)
        if scenario in ("direct", "slowdirect"):
            # The first phrase ends at a pause while capture goes on.
            send(op="ready", id=phrase)
            phrase += 1
            send(op="phrase", id=phrase)
    elif op == "prompt":
        prompts.write(message["text"].encode() + b"\0")
        if scenario == "noanswer":
            continue  # recognition that takes its time
        if scenario == "slowdirect" and message["id"] == 2:
            import time
            time.sleep(0.3)  # still being recognized when listening resumes
        send(op="text", id=message["id"], text=replies[(message["id"] - 1) % len(replies)],
             **({"device": True} if scenario == "fallback" else {}))
    elif op == "cancel":
        send(op="cancelled")
)PY";
void use_fake_helper(Options& options, const fs::path& directory, const char* scenario) {
    const auto script = directory / "fake_helper.py";
    std::ofstream(script) << fake_helper;
    options.dictation_helper = script;
    options.dictation_arguments = {"--scenario", scenario};
}
// Ticks until the helper has confirmed the last pause.
void settle_pause(App& app) {
    for (int i = 0; i < 300 && app.view().dictation && app.dictation_holds_panel() &&
                    !app.view().dictation->capturing && app.view().dictation->text.empty();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        app.tick(clock += .01);
    }
}
// Opens dictation mode with the mic key, which starts listening, and pauses.
void open_editor(App& app) {
    press(app, "Dictation");
    press(app, "Dictation");
    settle_pause(app);
}

void mode_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    // On device is the default; a model already downloaded on this machine
    // must not make the mic key appear.
    options.models_dir = directory.path / "models";
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
    require(app.dictation_open() && app.view().dictation->capturing && app.listening(),
            "the mic key opens dictation mode and listens");
    press(app, "Dictation");
    settle_pause(app);
    require(!app.listening(), "the mic key stops listening, so VR can pulse the hand");
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
        // The VR loop keeps it held past the panel's edge until the trigger lets go.
        require(app.holds_repeat(0) && !app.holds_repeat(1),
                "the editor's held Backspace counts as repeating");
        app.up(0, x, y);
        require(!app.holds_repeat(0), "and no longer once released");
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

    app.apply({"ja-jis-full", "ja-jis", "graphite"});
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
    // A Settings row below or above its card's view scrolls into it first.
    for (int step = 0; step < 40; ++step) {
        const auto view = app.view();
        const auto control = std::find_if(view.controls.begin(), view.controls.end(),
                                          [&](const Control& c) { return c.id == id; });
        if (control == view.controls.end() || !control->clip) {
            break;
        }
        const auto& r = control->bounds;
        const auto& clip = *control->clip;
        const double y = r.y + r.height / 2;
        if (y >= clip.y && y <= clip.y + clip.height) {
            break;
        }
        app.move(0, r.x + 5, clip.y + clip.height / 2);
        app.scroll(0, 0, y > clip.y + clip.height ? 1 : -1);
        settle(app);
    }
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
        require(app.view().dictation_capturing && control_enabled(app, "settings"),
                "the mic key shows the level; Settings stays available");
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
        press(app, "Dictation"); // the pause sends the phrase to the fake helper
        wait_until(app, [&] { return !editor_text(app).empty() && !app.view().dictation->revealing; });
        require(app.dictation_holds_panel(), "text in the editor holds the panel");
        // The native keyboard button closes a Keep open panel through X's path.
        app.close_with_x();
        require(!app.dictation_open() && !app.dictation_holds_panel(),
                "closing as X does releases dictation's hold");
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

// The prompts the fake helper received, in order.
std::vector<std::string> prompts_sent(const fs::path& directory) {
    std::ifstream file(directory / "prompts", std::ios::binary);
    std::vector<std::string> result;
    for (std::string prompt; std::getline(file, prompt, '\0');) {
        result.push_back(prompt);
    }
    return result;
}
bool control_in_editor(App& app, const std::string& id) {
    const auto view = app.view();
    const auto& editor = view.dictation->bounds;
    return std::any_of(view.controls.begin(), view.controls.end(), [&](const Control& control) {
        const auto& r = control.bounds;
        return control.id == id && r.x >= editor.x && r.x + r.width <= editor.x + editor.width &&
               r.y >= editor.y && r.y + r.height <= editor.y + editor.height;
    });
}
bool has_control(App& app, const std::string& id) {
    const auto controls = app.view().controls;
    return std::any_of(controls.begin(), controls.end(),
                       [&](const Control& control) { return control.id == id; });
}

void language_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "main");
    const std::string server = R"(,"dictation":"server","dictation_server":"127.0.0.1")";
    {
        // A profile's prompt and the editor's context share Whisper's 200
        // code points; the context gives way.
        fs::create_directories(directory.path / "languages");
        std::ofstream(directory.path / "languages" / "xx-test.json")
            << R"({"schema_version":1,"id":"xx-test","name":"Test","locale":"en-US",
            "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
            "legends":{"source":"keymap","overrides":{}},"font_families":["sans-serif"],
            "dictation":{"language":"en","prompt":"Abc."}})";
        write_config(directory.path, server, "xx-test");
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return editor_text(app) == "Hello there."; });
        type_text(app, std::string(250, 'a'));
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return prompts_sent(directory.path).size() == 2; });
        const auto prompts = prompts_sent(directory.path);
        require(prompts[0] == "Abc." && prompts[1] == "Abc." + std::string(196, 'a'),
                "the prompt comes first and the context fills the rest of 200");
    }
    fs::remove(directory.path / "prompts");
    write_config(directory.path, server, "ja-romaji");
    {
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return !editor_text(app).empty(); });
        require(editor_text(app) == "H" && app.view().dictation->revealing,
                "without spaces, a phrase appears a character at a time");
        settle(app);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return editor_text(app) == "Hello there.Again."; });
        const auto prompts = prompts_sent(directory.path);
        require(prompts.size() == 2 && prompts[0] == "はい、そうですね。" &&
                    prompts[1] == "はい、そうですね。Hello there.",
                "Japanese phrases join without a space, prompted for punctuation");

        type_text(app, "kana");
        auto view = app.view();
        const auto& editor = *view.dictation;
        require(editor.text == "Hello there.Again.かな" && editor.preedit_begin == 18 &&
                    editor.preedit_end == editor.text.size() && editor.cursor == editor.preedit_end &&
                    !editor.candidates,
                "the reading shows at the cursor; the band waits for candidates");
        const auto convert = std::find_if(view.controls.begin(), view.controls.end(),
                                          [](const Control& c) { return c.id == "ime-convert"; });
        require(convert != view.controls.end() &&
                    convert->bounds.x + convert->bounds.width < editor.bounds.x &&
                    convert->label == "変換\nConvert",
                "Convert moves beside the editor, its label on two lines");
        press(app, "Escape");
        require(app.dictation_open() && editor_text(app) == "Hello there.Again." &&
                    app.view().dictation->notice.empty(),
                "Esc cancels the reading, not the mode");
        type_text(app, "kana");
        press(app, "Enter");
        view = app.view();
        require(view.dictation->text == "Hello there.Again.かな" &&
                    view.dictation->preedit_end == view.dictation->preedit_begin &&
                    !view.dictation->candidates && !has_control(app, "ime-convert"),
                "Enter commits the reading into the editor");
        type_text(app, "ne");
        press(app, "Dictation");
        require(app.view().dictation->capturing && editor_text(app) == "Hello there.Again.かなね",
                "the mic key commits the reading before listening");
        press(app, "Escape");
        type_text(app, "no");
        require(sink.text.empty() && sink.keys.empty(), "nothing reached the target");
        press(app, "Dictation-send");
        wait_until(app, [&] { return !app.dictation_open(); });
        require(sink.text == std::vector<std::string>{"Hello there.Again.かなねの"},
                "Send commits the reading, then sends everything");
    }
    write_config(directory.path, server, "ja-flick", "ja-flick-12");
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        wait_until(app, [&] { return editor_text(app) == "Hello there."; });
        settle(app);
        key_center(app, "Dictation-send"); // shown
        press(app, "Flick-ka");
        press(app, "Flick-a");
        auto view = app.view();
        require(view.dictation->text == "Hello there.かあ" &&
                    view.dictation->preedit_end > view.dictation->preedit_begin,
                "flick keys compose in the editor");
        press(app, "Enter");
        view = app.view();
        require(view.dictation->text == "Hello there.かあ" &&
                    view.dictation->preedit_end == view.dictation->preedit_begin,
                "and commit into it");
        // The other hand's press types the held flick first, whichever hand
        // lets go first.
        for (const bool reversed : {false, true}) {
            const auto [kx, ky] = key_center(app, "Flick-ka");
            const auto [sx, sy] = key_center(app, "Flick-sa");
            require(app.down(0, kx, ky, clock) && app.down(1, sx, sy, clock + .05),
                    "both hands' flick keys accept a press in the editor");
            if (reversed) {
                app.up(1, sx, sy);
                app.up(0, kx, ky);
            } else {
                app.up(0, kx, ky);
                app.up(1, sx, sy);
            }
            clock += .2;
            app.tick(clock);
        }
        require(editor_text(app) == "Hello there.かあかさかさ",
                "overlapping flicks keep press order in the editor");
    }
    write_config(directory.path, server, "ko-cheonjiin", "ko-12key");
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        wait_until(app, [&] { return editor_text(app) == "Hello there."; });
        settle(app);
        const auto [gx, gy] = key_center(app, "Flick-g");
        const auto [ix, iy] = key_center(app, "Flick-i");
        require(app.down(0, gx, gy, clock) && app.down(1, ix, iy, clock + .05),
                "both hands' 천지인 keys accept a press in the editor");
        app.up(1, ix, iy);
        app.up(0, gx, gy);
        press(app, "Enter");
        require(editor_text(app) == "Hello there.기" && sink.text.empty() && sink.keys.empty(),
                "천지인 composes into the editor in press order");
    }
    if (!CjkComposer::supported("chinese-pinyin-simplified")) {
        std::cout << "Pinyin in dictation mode skipped: engine absent.\n";
        return;
    }
    write_config(directory.path, server, "zh-cn-pinyin");
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        wait_until(app, [&] { return editor_text(app) == "Hello there."; });
        settle(app);
        type_text(app, "nihao");
        require(control_in_editor(app, "cjk-candidate-0") && app.view().dictation->candidates,
                "Pinyin candidates sit in the band");
        press(app, "Space");
        const auto text = editor_text(app);
        require(text.size() > 12 && text.starts_with("Hello there.") &&
                    !app.view().dictation->candidates,
                "Space chooses the first candidate into the editor");
    }
}

// The editor's arrows follow the screen: in right-to-left text Right moves
// toward the start, and a letter with its marks is one step.
void direction_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    const std::string server = R"(,"dictation":"server","dictation_server":"127.0.0.1")";
    write_config(directory.path, server, "he-il");
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        for (const auto* id : {"KeyA", "KeyT", "KeyC"}) {
            press(app, id);
        }
        require(editor_text(app) == "שאב", "Hebrew types into the editor");
        press(app, "ArrowRight");
        press(app, "KeyV");
        require(editor_text(app) == "שאהב", "Right moves toward the start of Hebrew text");
        press(app, "ArrowLeft");
        press(app, "KeyB");
        require(editor_text(app) == "שאהבנ", "Left moves toward its end");
        // Brackets and digits keep their places in the right-to-left line.
        for (const auto* id : {"Space", "ShiftLeft", "Digit0", "KeyG", "KeyU", "KeyK", "KeyO",
                               "ShiftLeft", "Digit9", "Space", "Digit1", "Digit2"}) {
            press(app, id);
        }
        require(editor_text(app) == "שאהבנ (עולם) 12", "Hebrew with brackets and digits");
        // With FRAMEKEYBOARD_TEST_RENDER_DIR, an image of the right-to-left editor.
        if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
            app.paint(clock);
            app.renderer.write_png(fs::path(render) / "dictation-hebrew.png");
        }
    }
    write_config(directory.path, server, "th-th", "international-full");
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        press(app, "KeyD");
        press(app, "KeyY");
        press(app, "ArrowLeft");
        press(app, "KeyA");
        require(editor_text(app) == "ฟกั", "Left steps over a Thai letter and its vowel mark");
    }
}

// Type as I speak: phrases go straight to the target from the normal keyboard.
void direct_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "direct");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    {
        Sink sink;
        App app(options, sink);
        app.show_settings();
        click(app, "dictation-direct");
        click(app, "apply");
        app.flush_settings();
        std::vector<std::string> errors;
        require(load_settings(directory.path, errors).dictation_direct,
                "the Dictation card turns Type as I speak on");
    }
    {
        // The mic key's ring: only while a spoken phrase waits for its text.
        options.dictation_arguments = {"--scenario", "noanswer"};
        // A favorite, so the language button shows.
        write_config(
            directory.path,
            R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true,)"
            R"("favorites":[{"id":"pair","name":"Pair","layout":"international-full","language":"de-de"}])");
        Sink sink;
        App app(options, sink);
        require(control_enabled(app, "language-cycle"), "the language button shows and works");
        const auto asked = prompts_sent(directory.path).size();
        press(app, "Dictation");
        require(app.view().dictation_capturing && !app.view().dictation_recognizing,
                "no ring while the phrase is still being spoken");
        press(app, "Dictation");
        wait_until(app, [&] { return prompts_sent(directory.path).size() == asked + 1; });
        require(app.view().dictation_recognizing, "a paused phrase waiting for its text shows the ring");
        // The ring steps at 30 fps: a 60 Hz frame between steps paints nothing.
        const double step = std::floor(clock * 30) / 30 + 2.0 / 30;
        require(app.paint(step + .001) && app.renderer.animating(), "the ring paints at a step");
        require(!app.paint(step + .017), "and skips the frame before the next step");
        require(app.paint(step + 1.0 / 30 + .001), "and paints at the next step");
        // Settings covers the mic key: the phrase is still recognized, but its
        // ring repaints nothing.
        app.show_settings();
        app.paint(step + 2.0 / 30 + .001);
        require(app.view().dictation_recognizing && !app.renderer.animating(),
                "under Settings the hidden ring asks for no frames");
        app.back();
        require(!control_enabled(app, "language-cycle"), "the language waits until the phrase is in");
        press(app, "Escape");
        require(!app.view().dictation_recognizing, "Esc drops the phrase and the ring");
        // The editor shows it the same way.
        write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
        App editor(options, sink);
        press(editor, "Dictation");
        press(editor, "Dictation");
        wait_until(editor, [&] { return prompts_sent(directory.path).size() == asked + 2; });
        require(editor.dictation_open() && editor.view().dictation_recognizing,
                "in the editor, a phrase being recognized shows the ring too");
        write_config(directory.path,
                     R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)");
        options.dictation_arguments = {"--scenario", "direct"};
        fs::remove(directory.path / "prompts");
    }
    {
        // Turned on from dictation mode, it returns to the main keyboard.
        write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        require(app.dictation_open(), "the mic key opened dictation mode");
        app.show_settings();
        click(app, "dictation-direct");
        click(app, "apply");
        app.tick(clock += .1);
        require(!app.dictation_open() && !app.view().settings,
                "Type as I speak returns to the main keyboard");
    }
    const std::string direct =
        R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)";
    write_config(directory.path, direct);
    {
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(!app.dictation_open() && app.view().dictation_capturing,
                "the mic key listens on the normal keyboard");
        wait_until(app, [&] { return !sink.text.empty(); });
        require(sink.text == std::vector<std::string>{"Hello there."},
                "a phrase is typed as soon as it is recognized");
        press(app, "KeyQ");
        require(sink.text.back() == "q", "keys keep typing while listening");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 3; });
        require(sink.text.back() == " Again." && !app.view().dictation_capturing,
                "the mic key stops; the phrase being spoken still types, after a space");
        const auto prompts = prompts_sent(directory.path);
        require(prompts.size() == 2 && prompts[0].empty() && prompts[1] == "Hello there.",
                "the prompt is the session's own phrases");
        require(std::none_of(sink.keys.begin(), sink.keys.end(),
                             [](const auto& key) { return key.first == KEY_ESC; }),
                "nothing else reached the target");

        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 4; });
        require(sink.text.back() == "Hello there.", "a new session starts without a space");
        press(app, "Escape");
        require(!app.view().dictation_capturing && app.view().status == "Dictation stopped.",
                "Esc stops listening");
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        require(sink.text.size() == 4 && sink.keys.empty(),
                "Esc drops the phrase being spoken and is not sent");
    }
    fs::remove(directory.path / "prompts");
    {
        // A different Steam field: the phrases were meant for the old one.
        Sink sink;
        App app(options, sink);
        app.set_native_request(5);
        sink.busy = true;
        press(app, "Dictation");
        wait_until(app, [&] { return prompts_sent(directory.path).size() == 1; });
        app.set_native_request(6);
        require(!app.view().dictation_capturing &&
                    app.view().status == "Dictation stopped: the text field changed.",
                "a target change stops Type as I speak");
        sink.busy = false;
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        require(sink.text.empty(), "and nothing reaches the new field");
    }
    {
        // A full output queue: phrases wait, then follow the keys in order.
        Sink sink;
        App app(options, sink);
        sink.busy = true;
        for (std::size_t i = 0; i < OutputController::capacity + 2; ++i) {
            press(app, "KeyA");
        }
        press(app, "Dictation");
        wait_until(app, [&] { return prompts_sent(directory.path).size() >= 2; });
        press(app, "Dictation");
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        require(sink.text.empty(), "nothing goes out while the target is busy");
        sink.busy = false;
        wait_until(app, [&] { return sink.text.size() == OutputController::capacity + 2; });
        require(std::count(sink.text.begin(), sink.text.end(), "a") == OutputController::capacity &&
                    sink.text[OutputController::capacity] == "Hello there." &&
                    sink.text.back() == " Again.",
                "waiting phrases type after the queued keys, in order");
    }
    {
        // Uncertain delivery: listening pauses, and Enter retries the rest.
        Sink sink;
        App app(options, sink);
        sink.commits_before_failure = 0;
        press(app, "Dictation");
        wait_until(app, [&] { return !app.view().dictation_capturing; });
        require(sink.text.empty() && app.view().status.find("Enter") != std::string::npos,
                "a failed phrase pauses listening and offers a retry");
        sink.commits_before_failure = -1;
        press(app, "Enter");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} && sink.keys.empty(),
                "Enter retries the phrase, then the next follows");
    }
    {
        // Live words: typed once two guesses agree, the rest with the final text.
        options.dictation_arguments = {"--scenario", "live"};
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello", " there how"} &&
                    app.view().dictation_capturing,
                "words three guesses agree on type while still listening; the newest word waits");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 3; });
        require(sink.text.back() == " are you?",
                "the final text continues where the typed words end in it, punctuation aside");
        options.dictation_arguments = {"--scenario", "direct"};
    }
    {
        // Listening again before the last phrase is typed continues the
        // session: the phrase still gets its joining space.
        options.dictation_arguments = {"--scenario", "slowdirect"};
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 1; });
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() >= 2; });
        require(sink.text[0] == "Hello there." && sink.text[1] == " Again.",
                "a quick restart keeps the session's spacing");
        options.dictation_arguments = {"--scenario", "direct"};
    }
    {
        // Whisper rewrites words already typed ("twenty five" becomes "25"):
        // typing goes on after them, and nothing is skipped or repeated.
        options.dictation_arguments = {"--scenario", "liveshift"};
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 3; });
        std::string typed;
        for (const auto& piece : sink.text) {
            typed += piece;
        }
        require(typed == "I have twenty five dollars and fifty cents.",
                "a rewritten number neither drops the next word nor repeats itself");
        options.dictation_arguments = {"--scenario", "direct"};
    }
    {
        // A long phrase goes out in commits the target takes at once.
        options.dictation_arguments = {"--scenario", "long"};
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 3; });
        std::string typed;
        for (const auto& piece : sink.text) {
            typed += piece;
        }
        require(typed == "This phrase is much longer than thirty-two characters, so it goes out in "
                         "pieces." &&
                    app.view().status.empty(),
                "a long phrase is typed in full, 32 characters at a time");
        options.dictation_arguments = {"--scenario", "direct"};
    }
    {
        // The dashboard closing keeps the panel until the last phrase is typed.
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        require(app.dashboard_visibility(false) && app.passthrough(),
                "closing the dashboard while listening keeps the panel over the app");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        app.tick(clock += .1);
        require(!app.passthrough(), "and it lets go once the phrases are typed");
        press(app, "Dictation");
        app.panel_hidden();
        require(!app.view().dictation_capturing, "hiding the panel stops Type as I speak");
    }
    {
        // A phrase in flight fails after part of it arrived: only the rest is retried.
        Sink sink;
        App app(options, sink);
        sink.busy = true;
        press(app, "Dictation");
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        sink.busy = false;
        sink.commits_before_failure = 0;
        sink.acknowledged = 6; // "Hello " reached the target
        wait_until(app, [&] { return !app.view().dictation_capturing; });
        require(app.view().status.find("Enter") != std::string::npos,
                "the failure pauses listening and the queue offers its retry");
        sink.commits_before_failure = -1;
        press(app, "Enter");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"there.", " Again."} && sink.keys.empty(),
                "Enter types only what had not arrived, then the next phrase");
    }
    {
        // Esc drops the failed rest instead: a later Enter is only Enter.
        Sink sink;
        App app(options, sink);
        sink.busy = true;
        press(app, "Dictation");
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        sink.busy = false;
        sink.commits_before_failure = 0;
        sink.acknowledged = 6;
        wait_until(app, [&] { return !app.view().dictation_capturing; });
        sink.commits_before_failure = -1;
        press(app, "Escape");
        press(app, "Enter");
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.tick(clock += .01);
        }
        require(sink.text.empty(), "Esc drops the failed phrase and the ones waiting");
        require(std::count(sink.keys.begin(), sink.keys.end(), std::pair{KEY_ENTER, 1}) == 1 &&
                    std::count(sink.keys.begin(), sink.keys.end(), std::pair{KEY_ESC, 1}) == 0,
                "Esc is not sent, and the Enter after it is an ordinary key");
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

// Records what Send writes and counts an application's request for it once
// Ctrl+V reaches the sink, as the X clipboard's served() would.
struct FakeClipboard : Clipboard {
    const Sink* sink{};
    std::vector<std::string> written;
    bool available{true}, taken{true};
    std::uint64_t write(const std::string& text) override {
        written.push_back(text);
        return written.size();
    }
    std::optional<bool> result(std::uint64_t) const override { return available; }
    std::uint64_t served() const override {
        std::uint64_t pastes = 0;
        bool control = false;
        for (const auto& [code, value] : sink->keys) {
            if (code == KEY_LEFTCTRL) {
                control = value != 0;
            } else if (code == KEY_V && value == 1 && control && taken) {
                ++pastes;
            }
        }
        return pastes;
    }
};

// Type as I speak with Send set to Paste: each phrase is pasted.
void direct_paste_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                                 R"("dictation_send_paste":true,"dictation_direct":true)");
    use_fake_helper(options, directory.path, "direct");
    const Keys paste{{KEY_LEFTCTRL, 1}, {KEY_V, 1}, {KEY_V, 0}, {KEY_LEFTCTRL, 0}};
    {
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.keys.size() == 4; });
        press(app, "Dictation");
        wait_until(app, [&] { return sink.keys.size() == 8; });
        settle(app);
        Keys twice = paste;
        twice.insert(twice.end(), paste.begin(), paste.end());
        require(clipboard.written == std::vector<std::string>{"Hello there.", " Again."} &&
                    sink.keys == twice && sink.text.empty(),
                "each phrase is written to the clipboard and pasted, nothing typed");
    }
    {
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        clipboard.available = false;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} && sink.keys.empty(),
                "without a clipboard the phrases are typed instead");
    }
    {
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        clipboard.taken = false;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        for (int i = 0; i < 200 && app.view().dictation_capturing; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            app.tick(clock += .02);
        }
        require(!app.view().dictation_capturing &&
                    app.view().status == "The app did not take a pasted phrase. Dictation paused.",
                "a paste nobody takes pauses listening and says so");
    }
}

void paste_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_send_paste":true)");
    use_fake_helper(options, directory.path, "silent");
    {
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "\nhi\n\nthere\n");
        press(app, "Dictation-send");
        settle(app);
        require(clipboard.written == std::vector<std::string>{"hi\n\nthere"},
                "paste writes the trimmed text, line breaks and all, to the clipboard");
        const Keys expected{{KEY_LEFTCTRL, 1}, {KEY_V, 1},     {KEY_V, 0},
                            {KEY_LEFTCTRL, 0}, {KEY_ENTER, 1}, {KEY_ENTER, 0}};
        require(sink.keys == expected && sink.text.empty(),
                "Ctrl+V, then Enter once the application took the text; nothing typed");
        require(!app.dictation_open(), "a pasted text returns to the normal keyboard");
    }
    {
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        clipboard.taken = false;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "kept");
        press(app, "Dictation-send");
        for (int i = 0; i < 200; ++i) {
            app.tick(clock += .02);
        }
        require(app.dictation_open() && editor_text(app) == "kept" &&
                    std::none_of(sink.keys.begin(), sink.keys.end(),
                                 [](const auto& key) { return key.first == KEY_ENTER; }),
                "a paste nobody takes keeps the text and never presses Enter");
    }
    {
        Sink sink;
        sink.single_line = true;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        clipboard.available = false;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "kept");
        press(app, "Dictation-send");
        settle(app);
        require(app.dictation_open() && editor_text(app) == "kept" && sink.keys.empty(),
                "without a clipboard nothing is pressed and the text stays");
    }
    {
        Sink sink;
        sink.steam_field = true;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "steam");
        press(app, "Dictation-send");
        settle(app);
        require(clipboard.written.empty() && sink.text == std::vector<std::string>{"steam"},
                "Steam's own fields keep the direct path");
    }
    options.clipboard = nullptr;
}

void passthrough_tests() {
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
        require(!app.dashboard_visibility(false) && !app.passthrough(),
                "a paused editor hides with the dashboard as usual");
        press(app, "Dictation");
        require(!app.dashboard_visibility(true) && !app.passthrough(),
                "nothing while the dashboard shows");
        require(app.dashboard_visibility(false) && app.passthrough(),
                "closing the dashboard while listening keeps the panel open");
        press(app, "Dictation");
        require(app.passthrough(), "pausing keeps it open until the mode ends");
        settle_pause(app);
        press(app, "Escape");
        require(!app.dictation_open() && !app.passthrough(),
                "leaving the mode ends dictation's passthrough");
    }
    {
        Sink sink;
        App app(options, sink);
        app.set_passthrough(true);
        press(app, "Dictation");
        require(!app.dashboard_visibility(false), "the user's own passthrough already keeps it open");
        press(app, "Escape");
        press(app, "Escape");
        require(!app.dictation_open() && app.passthrough(), "and stays on after the mode");
    }
    {
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        app.set_passthrough(false);
        require(!app.passthrough(), "the user can still turn it off");
    }
}

void field_tests() {
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
    require(!app.dictation_holds_panel(), "no hold outside dictation");
    press(app, "Dictation");
    require(app.dictation_holds_panel(), "listening holds the panel");
    press(app, "Dictation");
    settle_pause(app);
    require(!app.dictation_holds_panel(), "a paused, empty editor does not");
    type_text(app, "hi");
    require(app.dictation_holds_panel(), "text in the editor does");
    app.set_field_needed(true);
    app.tick(clock += .1);
    require(app.view().disabled_keys.contains("Dictation-send") &&
                app.view().dictation->notice == "The text field closed. Select a text field to send.",
            "without a field Send waits and says why");
    type_text(app, "a");
    require(editor_text(app) == "hia", "the editor keeps working");
    app.set_interaction_active(false);
    app.set_interaction_active(true);
    press(app, "Dictation");
    app.set_interaction_active(false); // a handoff pauses input, not listening
    require(app.view().dictation->capturing, "listening continues through a handoff");
    app.set_interaction_active(true);
    app.panel_hidden();
    require(!app.view().dictation->capturing, "hiding the panel pauses listening");
    app.set_field_needed(false);
    app.tick(clock += .1);
    require(!app.view().disabled_keys.contains("Dictation-send"), "a new field enables Send again");
    press(app, "Dictation-send");
    // Send first waits for the helper to confirm the pause, in real time.
    wait_until(app, [&] { return !sink.text.empty(); });
    require(sink.text == std::vector<std::string>{"hia"}, "and the text goes there");
}

void settings_and_update_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "main");
    Sink sink;
    App app(options, sink);
    press(app, "Dictation");
    require(control_enabled(app, "settings"), "Settings stays available while listening");
    app.show_settings();
    require(!app.view().dictation_capturing, "opening Settings pauses dictation");
    app.back();
    wait_until(app, [&] { return !editor_text(app).empty() && !app.view().dictation->revealing; });
    require(app.dictation_open() && editor_text(app) == "Hello there.",
            "the phrase spoken before Settings is kept, and the mode is still open");
    press(app, "Dictation");
    require(app.view().dictation->capturing && app.dictation_helper_running(), "listening again");
    app.end_dictation_for_update();
    require(!app.dictation_open() && !app.dictation_helper_running(),
            "an update discards dictation and stops its helper");
    press(app, "Dictation");
    require(editor_text(app).empty(), "nothing of the old text survives");
    require(sink.text.empty() && sink.keys.empty(), "nothing reached the target");
}

bool field_label(App& app, const std::string& text) {
    const auto view = app.view();
    return std::any_of(view.field_labels.begin(), view.field_labels.end(),
                       [&](const FieldLabel& label) { return label.text == text; });
}

// The configure messages the fake helper received, one JSON object a line.
std::string configures(const fs::path& directory) {
    std::ifstream file(directory / "configures");
    return {std::istreambuf_iterator<char>(file), {}};
}

// On-device dictation: the model download after confirmation, progress on
// the mic key, the device target, Delete, the fallback and its mark.
void device_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "main");
    write_config(directory.path, R"(,"dictation":"off","dictation_server":"127.0.0.1")");
    Sink sink;
    App app(options, sink);
    app.show_settings();
    click(app, "dictation");
    click(app, "choose:dictation:device");
    require(field_label(app, "Model") && !field_label(app, "Server"),
            "On device shows the model row instead of the server");
    click(app, "apply");
    auto view = app.view();
    require(view.settings && view.confirm && view.confirm->title == "Download the speech model?" &&
                view.confirm->body.find("264 MB") != std::string::npos,
            "Apply asks before downloading, naming the size");
    click(app, "confirm-cancel");
    click(app, "settings"); // Back
    std::vector<std::string> errors;
    require(load_settings(directory.path, errors).dictation == "off" &&
                !fs::exists(directory.path / "models"),
            "Cancel downloads and applies nothing");

    app.show_settings();
    click(app, "dictation");
    click(app, "choose:dictation:device");
    click(app, "apply");
    click(app, "confirm-accept");
    app.flush_settings();
    require(!app.view().settings && load_settings(directory.path, errors).dictation == "device",
            "Download applies the settings");
    require(app.view().dictation_download == 0.0, "the download starts at once");
    wait_until(app, [&] { return app.view().dictation_download == .5; });
    view = app.view();
    require(*view.dictation_download == .5 && !hidden_key(view, *find(view, "Dictation")) &&
                view.disabled_keys.contains("Dictation") &&
                view.status == "Downloading the speech model: 50%",
            "the mic key shows the progress and waits; the keyboard keeps working");
    press(app, "KeyA");
    require(sink.text == std::vector<std::string>{"a"}, "keys type during the download");
    wait_until(app, [&] { return app.view().dictation_available; });
    require(app.view().dictation_where == Icon::Headset, "on device, the mic key shows the headset");
    require(!app.view().disabled_keys.contains("Dictation") &&
                app.view().status == "The speech model is ready.",
            "a verified model makes the mic key usable");
    open_editor(app);
    wait_until(app, [&] { return editor_text(app) == "Hello there."; });
    require(configures(directory.path)
                        .find(R"("device": ")" + (directory.path / "models").string() +
                              "/ggml-small-q8_0.bin\"") != std::string::npos &&
                configures(directory.path).find(R"("server")") == std::string::npos,
            "on device, phrases go to the downloaded model and no server");
    press(app, "Escape");
    press(app, "Escape"); // asked, then discarded

    app.show_settings();
    require(field_label(app, "Ready"), "the model row says the model is ready");
    click(app, "model-action");
    require(!fs::exists(directory.path / "models/ggml-small-q8_0.bin") && field_label(app, "264 MB"),
            "Delete removes the model");
    click(app, "apply");
    require(!app.view().confirm && !app.view().dictation_available,
            "an unchanged mode does not ask again; without the model the mic key is hidden");

    // With On device already saved, an unrelated Apply never offers the download.
    write_config(directory.path, "");
    {
        App fresh(options, sink);
        fresh.show_settings();
        click(fresh, "hide-numpad");
        click(fresh, "apply");
        require(!fresh.view().settings && !fresh.view().confirm && !fresh.view().dictation_available,
                "the default needs the model row's Download, not every Apply");
    }
    // A weaker language says so under the selector; English does not.
    auto warns = [](App& shown) {
        const auto labels = shown.view().field_labels;
        return std::any_of(labels.begin(), labels.end(), [](const FieldLabel& label) {
            return label.warning && label.text.find("low accuracy") != std::string::npos;
        });
    };
    app.show_settings();
    require(!warns(app) && !field_label(app, "Format"),
            "English has no warning, and the model format is a development option");
    click(app, "settings"); // Back
    for (const auto* language : {"ja-romaji", "ar", "hi-in", "nb-no", "vi-vn", "es-es"}) {
        write_config(directory.path, R"(,"dictation":"device")", language,
                     std::string(language) == "ja-romaji" ? "en-us-full" : "international-full");
        App other(options, sink);
        other.show_settings();
        require(warns(other) == (std::string(language) != "es-es"),
                "a language other than English or Spanish warns on device");
    }
}

// The server's fallback to the device: offered as a switch, it needs the
// model, configures the helper with both, and marks fallback text quietly.
void fallback_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "fallback");
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","developer_options":true)");
    fs::create_directories(directory.path / "models");
    std::ofstream(directory.path / "models/ggml-small-q8_0.bin") << "model";
    {
        // From On device, as a fresh install has it: choose Self-hosted,
        // turn the fallback on and Apply.
        write_config(directory.path, R"(,"dictation":"device","dictation_server":"127.0.0.1")");
        Sink sink;
        App app(options, sink);
        app.show_settings();
        click(app, "dictation");
        click(app, "choose:dictation:server");
        click(app, "dictation-fallback");
        click(app, "apply");
        app.flush_settings();
        std::vector<std::string> errors;
        const auto saved = load_settings(directory.path, errors);
        require(!app.view().settings && saved.dictation == "server" && saved.dictation_fallback,
                "Self-hosted with the fallback is saved");
        open_editor(app);
        wait_until(app, [&] { return !editor_text(app).empty(); });
        require(configures(directory.path).find(R"("fallback": true)") != std::string::npos,
                "and the helper is told to fall back");
        fs::remove(directory.path / "configures");
    }
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","developer_options":true)");
    Sink sink;
    App app(options, sink);
    app.show_settings();
    require(field_label(app, "Fall back to on device") && !field_label(app, "Model"),
            "the server offers the fallback; the model row waits for it");
    click(app, "dictation-fallback");
    require(field_label(app, "Model") && field_label(app, "Format"),
            "the fallback shows the model row, and development builds its format");
    click(app, "device-model");
    click(app, "choose:device-model:small-q4_0");
    require(field_label(app, "488 → 145 MB"), "4-bit shows what it downloads and what it keeps");
    click(app, "device-model");
    click(app, "choose:device-model:small-q8_0");
    click(app, "apply");
    require(!app.view().confirm, "a model already there needs no download");
    require(app.view().dictation_where == Icon::Computer, "self-hosted, the mic key shows the computer");
    open_editor(app);
    wait_until(app, [&] { return !editor_text(app).empty(); });
    require(configures(directory.path).find(R"("fallback": true)") != std::string::npos &&
                configures(directory.path).find(R"("server": "127.0.0.1")") != std::string::npos,
            "the helper gets the server and the model for its fallback");
    require(app.view().dictation->where == Icon::Headset && app.view().dictation->counter == "12/900",
            "a phrase from the fallback shows the headset by the counter");
}

// Esc in Type as I speak drops text already queued but not yet typed, and
// is not sent itself, whether or not listening was still on.
void direct_esc_tests() {
    for (const bool paused : {false, true}) {
        TemporaryDirectory directory;
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        write_config(directory.path,
                     R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)");
        use_fake_helper(options, directory.path, "direct");
        Sink sink;
        sink.busy = true; // the target is still taking earlier characters
        App app(options, sink);
        press(app, "Dictation");
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            app.tick(clock += .01);
        }
        if (paused) {
            press(app, "Dictation");
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                app.tick(clock += .01);
            }
        }
        const auto keys = sink.keys.size();
        press(app, "Escape");
        sink.busy = false;
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            app.tick(clock += .01);
        }
        require(sink.text.empty() && sink.keys.size() == keys,
                paused ? "Esc after pausing drops the queued phrases and is not sent"
                       : "Esc while listening drops the queued phrases and is not sent");
    }
}

// Type as I speak turned off while a phrase is still being recognized: the
// editor that the mic key opens next ends that session, so neither the late
// phrase nor speech in the editor reaches the target without Send.
void direct_to_editor_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)");
    use_fake_helper(options, directory.path, "slowdirect"); // the second phrase takes 0.3 s
    Sink sink;
    App app(options, sink);
    auto settle = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            app.tick(clock += .01);
        }
    };
    press(app, "Dictation");
    settle(40);
    press(app, "Dictation"); // the second phrase ends and waits for its text
    app.tick(clock += .01);
    const auto typed = sink.text;
    app.show_settings();
    click(app, "dictation-direct");
    click(app, "apply");
    press(app, "Dictation");
    require(app.dictation_open() && app.listening(), "the mic key now opens the editor and listens");
    settle(200);
    press(app, "Dictation");
    settle(100);
    require(sink.text == typed, "after the switch nothing reaches the target without Send");
    require(!editor_text(app).empty(), "speech after the switch goes into the editor");
}

// Type as I speak types at the Send pace, and ordinary typing gets its own
// pace back once the phrase is out.
void direct_pace_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                                 R"("dictation_direct":true,"dictation_send_gap_ms":40)");
    use_fake_helper(options, directory.path, "direct");
    Sink sink;
    App app(options, sink);
    press(app, "Dictation");
    wait_until(app, [&] { return !sink.text.empty(); });
    press(app, "Dictation");
    settle_pause(app);
    for (int i = 0; i < 100 && app.listening(); ++i) {
        app.tick(clock += .01);
    }
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        app.tick(clock += .01);
    }
    require(!sink.gaps.empty() && sink.gaps.front() == .04, "typed phrases go out at the Send pace");
    require(sink.gaps.back() == GamescopeText::default_character_gap,
            "ordinary typing returns to its own pace afterwards");
}

// A helper that exits mid-download leaves nothing to report on it: the
// download ends at once, the mic key returns and Settings can start again.
void download_helper_exit_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "diedownloading");
    write_config(directory.path, R"(,"dictation":"off","dictation_server":"127.0.0.1")");
    Sink sink;
    App app(options, sink);
    app.show_settings();
    click(app, "dictation");
    click(app, "choose:dictation:device");
    click(app, "apply");
    click(app, "confirm-accept");
    wait_until(app, [&] { return app.view().dictation_download == .5; });
    wait_until(app, [&] { return !app.view().dictation_download; });
    require(app.view().status == "The speech model download stopped. Try again.",
            "a helper that exits mid-download ends it and says so");
}

// Send waits for a slow phrase as long as recognition may take: on the
// device 30 s, which a game can make necessary, not a fixed 20 s.
void send_wait_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    fs::create_directories(directory.path / "models");
    std::ofstream(directory.path / "models/ggml-small-q8_0.bin") << "model";
    write_config(directory.path, R"(,"dictation":"device")");
    use_fake_helper(options, directory.path, "noanswer");
    Sink sink;
    App app(options, sink);
    press(app, "Dictation");
    press(app, "Dictation-send");
    wait_until(app, [&] { return app.view().dictation->notice == "Waiting for recognition…"; });
    app.tick(clock += 25);
    require(app.view().dictation->notice != "Recognition took too long. Send again when the text is in.",
            "after 25 s on the device Send still waits");
    app.tick(clock += 11);
    require(app.view().dictation->notice == "Recognition took too long. Send again when the text is in.",
            "past the device's limit it gives up and says so");
}

// Send pressed while listening waits for the last phrase, then starts in
// the tick that phrase arrives, while it is still appearing word by word.
// Sending erases delivered text, so the reveal must end first.
void send_during_reveal_tests() {
    for (const bool paste : {true, false}) {
        TemporaryDirectory directory;
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        write_config(
            directory.path,
            paste ? R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_send_paste":true)"
                  : R"(,"dictation":"server","dictation_server":"127.0.0.1")");
        use_fake_helper(options, directory.path, "verylong");
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation-send");
        for (int i = 0; i < 2000 && app.dictation_open(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            app.tick(clock += .01);
            app.view();
        }
        require(paste ? clipboard.written.size() == 1 && clipboard.written[0].starts_with("word0 ")
                      : !sink.text.empty() && sink.text.front().starts_with("word0"),
                paste ? "a long phrase still revealing is pasted whole, without a crash"
                      : "a long phrase still revealing is typed, without a crash");
    }
}

// A game turned SteamOS's voice filters on and the helper records around
// them: the status line says game sound may come through, without blocking.
void microphone_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "game");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    Sink sink;
    App app(options, sink);
    const std::string notice = "Game sounds may be picked up during a play session.";
    press(app, "Dictation");
    wait_until(app, [&] { return app.view().status == notice; });
    require(app.view().dictation && !app.view().dictation->notice_error,
            "while listening in a game, the status line says game sound may come through");
    press(app, "Dictation");
    settle_pause(app);
    require(app.view().status.empty(), "and it goes when listening stops");
}

// A Test answers for the address it checked: editing the field meanwhile
// leaves the new address untested, and only the tested one is remembered.
void test_race_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "slowtest");
    Sink sink;
    App app(options, sink);
    app.show_settings();
    click(app, "dictation-test");
    require(field_label(app, "Testing…") && !control_enabled(app, "dictation-test"),
            "one Test runs at a time");
    click(app, "dictation-server");
    for (int i = 0; i < 9; ++i) {
        click(app, "address-key-backspace");
    }
    for (char c : std::string("10.0.0.2")) {
        click(app, std::string("address-key-") + c);
    }
    click(app, "address-key-enter");
    require(!app.view().address && field_label(app, "Not tested"), "a new address is untested");
    wait_until(app, [&] { return control_enabled(app, "dictation-test"); });
    require(field_label(app, "Not tested"), "the old address's answer does not label the new one");
    app.flush_settings();
    std::vector<std::string> errors;
    require(load_settings(directory.path, errors).dictation_recent_servers ==
                std::vector<std::string>{"127.0.0.1"},
            "only the address that answered is remembered");

    // The helper exits before it answers: Test reports it and works again.
    use_fake_helper(options, directory.path, "testexit");
    App lost(options, sink);
    lost.show_settings();
    click(lost, "dictation-test");
    wait_until(lost, [&] { return field_label(lost, "Helper stopped"); });
    require(control_enabled(lost, "dictation-test"), "Test can run again after the helper exits");
    click(lost, "dictation-test");
    require(field_label(lost, "Testing…"), "and starts a new helper");
}

// A shell's keyboard (docs/integration.md): standalone-only settings are
// gone, X hides, and the text sink, contexts, hints, settings and themes
// work through App alone. The router stands between App and the Sink.
void hosted_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.hosted_fd = 99; // App only needs to know it is hosted
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path,
                 R"(,"replace_native_keyboard":true,"start_with_steamos":true,"hide_numpad":true)");
    Sink sink;
    ShellRouter router(sink);
    options.shell_router = &router;
    App app(options, router);
    require(app.hosted() && !app.replace_native_keyboard() && !app.start_with_steamos() &&
                app.background_start(),
            "hosted, replacement and boot start are inactive and the panel starts hidden");
    app.show_settings();
    const auto cards = app.view().cards;
    require(std::none_of(cards.begin(), cards.end(),
                         [](const auto& card) { return card.title == "Integration"; }) &&
                has_control(app, "quit"),
            "the Integration card is gone and Settings keeps Quit");
    app.back();
    require(!has_control(app, "passthrough"), "the shell, not a toggle, decides when the panel shows");
    click(app, "close");
    require(app.take_dismissal() && app.take_user_hide() && !app.quitting(),
            "X hides; the shell is told");
    app.set_interaction_active(true);

    // The sink: committed text and named keys go to the shell, one at a time.
    app.set_sink(true, "Test Shell");
    press(app, "KeyA");
    require(router.take_events().empty(), "without a context nothing is typed");
    app.set_context(std::string("c-1"), {});
    require(app.view().status == "Typing into Test Shell", "the case names who receives the typing");
    press(app, "KeyA");
    auto events = router.take_events();
    require(events.size() == 1 && events[0].text == "a" && events[0].context == "c-1",
            "a letter is a commit");
    press(app, "KeyB");
    require(router.take_events().empty(), "the next waits for the ack");
    router.acknowledge("c-1", events[0].seq);
    app.tick(clock += .1);
    events = router.take_events();
    require(events.size() == 1 && events[0].text == "b", "and follows it");
    // A new field drops everything addressed to the old one.
    press(app, "KeyC");
    app.set_context(std::string("c-2"), {});
    router.acknowledge("c-1", events[0].seq);
    app.tick(clock += .1);
    require(router.take_events().empty(), "queued text for the old context never arrives");
    press(app, "KeyD");
    events = router.take_events();
    require(events.size() == 1 && events[0].text == "d" && events[0].context == "c-2",
            "the new context types");
    router.acknowledge("c-2", events[0].seq);
    app.tick(clock += .1);
    require(sink.text.empty() && sink.keys.empty(),
            "nothing reaches the input backend while the sink is on");
    require(app.idle_for_update(), "settled output leaves the keyboard idle for an update");
    click(app, "sink-off");
    require(!app.sink_on(), "the user can take typing back");
    press(app, "KeyE");
    require(!sink.text.empty() && sink.text.back() == "e", "typing returns to the normal destination");

    // Hints belong to their context.
    App::HostHints password;
    password.purpose = "password";
    app.set_context(std::string("pw"), password);
    require(app.show_view("emoji") == "unavailable" &&
                app.host_dictation_request("dictation.start", nullptr).first == "unavailable",
            "a password field gets no emoji or dictation");
    App::HostHints number;
    number.purpose = "number";
    number.enter = "go";
    app.set_context(std::string("n"), number);
    require(!app.view().hide_numpad &&
                json_object_get_boolean(json_object_object_get(app.host_settings().get(), "num_lock")),
            "a number field shows the numpad with Num Lock on");
    require(app.view().key_labels.at("Enter") == "Go", "Enter's legend follows the hint");
    App::HostHints german;
    german.language = "de-de";
    app.set_context(std::string("de"), german);
    require(app.view().hide_numpad && app.selection().language == "de-de",
            "the language hint applies, temporarily");
    app.set_context(std::nullopt);
    require(app.selection().language == "en-us" && !app.view().key_labels.contains("Enter"),
            "the context's end restores the language and the legend");
    require(app.check_hints({"secret", "enter", "sentences", ""}).first == "bad_request",
            "unknown hints are refused");

    // Applying unrelated settings while a field's language hint is active
    // keeps the user's own language saved.
    App::HostHints german_field;
    german_field.language = "de-de";
    app.set_context(std::string("field"), german_field);
    app.show_settings();
    click(app, "auto-leveling");
    click(app, "apply");
    app.set_context(std::nullopt);
    app.flush_settings();
    {
        std::vector<std::string> load_errors;
        require(app.selection().language == "en-us" &&
                    load_settings(directory.path, load_errors).active.language == "en-us",
                "a hinted language is never saved by an unrelated Apply");
        click(app, "settings");
        click(app, "auto-leveling");
        click(app, "apply"); // back as it was
    }

    // Changing a context's language hint twice still returns to the first selection.
    App::HostHints french_field;
    french_field.language = "fr-fr";
    app.set_context(std::string("same"), german_field);
    app.set_context(std::string("same"), french_field);
    require(app.selection().language == "fr-fr", "the newer hint applies");
    app.set_context(std::string("same"), {});
    require(app.selection().language == "en-us",
            "dropping the language hint in the same context restores it");
    app.set_context(std::string("same"), german_field);
    app.set_context(std::string("same"), french_field);
    app.set_context(std::nullopt);
    require(app.selection().language == "en-us",
            "ending the context returns to the language before the first hint");

    // A shell's theme choice during a hint saves the user's own language.
    {
        const auto theme = app.selection().theme;
        app.set_context(std::string("field"), german_field);
        require(app.host_theme_select("midnight", 1).empty(), "theme.select applies during a hint");
        app.flush_settings();
        std::vector<std::string> load_errors;
        const auto saved = load_settings(directory.path, load_errors).active;
        require(saved.language == "en-us" && saved.layout == "en-us-full" && saved.theme == "midnight",
                "a theme choice never saves a hinted language");
        app.set_context(std::nullopt);
        require(app.selection().language == "en-us", "the hint still ends as before");
        require(app.host_theme_select(theme, 2).empty(), "the theme goes back");
    }

    // A layout chosen in Settings during a hint is the user's: it is saved
    // with their own language and outlives the hint.
    {
        app.set_context(std::string("field"), german_field);
        app.show_settings();
        click(app, "layout");
        click(app, "choose:layout:international-full");
        click(app, "apply");
        require(app.selection().layout == "international-full" && app.selection().language == "de-de",
                "the chosen layout applies with the hinted language");
        app.flush_settings();
        std::vector<std::string> load_errors;
        const auto saved = load_settings(directory.path, load_errors).active;
        require(saved.layout == "international-full" && saved.language == "en-us",
                "the chosen layout is saved with the user's own language");
        app.set_context(std::nullopt);
        require(app.selection().layout == "international-full" && app.selection().language == "en-us",
                "ending the context keeps the chosen layout");
        app.apply({"en-us-full", "en-us", app.selection().theme}); // back as it was
    }

    // A view chosen while hidden is the one that shows.
    app.set_interaction_active(false);
    require(app.show_view("emoji").empty() && app.view_name() == "emoji",
            "the shell opens the picker while hidden");
    app.set_interaction_active(true);
    require(app.view_name() == "emoji", "and it is still open once the panel shows");
    require(app.show_view("keyboard").empty(), "back to the keys");

    // Hiding keeps an open reading for the same context; a new context drops it.
    App::HostHints romaji;
    romaji.language = "ja-romaji";
    app.set_context(std::string("jp"), romaji);
    press(app, "KeyK");
    press(app, "KeyA");
    require(app.view().preedit == "か", "a reading is open");
    app.set_interaction_active(false);
    app.set_interaction_active(true);
    require(app.view().preedit == "か", "hiding the panel keeps it pending");
    app.set_context(std::string("other"), {});
    require(app.view().preedit.empty(), "a context change discards it, never commits it");
    app.set_context(std::nullopt);

    // Settings: all or nothing, persisted, reported with the request they cover.
    app.take_host_events();
    auto bad = json_ptr(json_tokener_parse(R"({"auto_leveling":false,"replace_native_keyboard":true})"));
    require(app.host_settings_set(bad.get(), 6) ==
                std::pair<std::string, std::string>{"invalid_setting", "replace_native_keyboard"},
            "a standalone-only key is refused");
    require(json_object_get_boolean(json_object_object_get(app.host_settings().get(), "auto_leveling")),
            "and nothing else in the request applies");
    auto good = json_ptr(json_tokener_parse(R"({"auto_leveling":false,"dictation_send_gap":40})"));
    require(app.host_settings_set(good.get(), 7).first.empty(), "valid settings apply");
    app.flush_settings();
    app.tick(clock += .1);
    auto host_events = app.take_host_events();
    auto saw = [&](const char* name, const char* part) {
        return std::any_of(host_events.begin(), host_events.end(), [&](const App::HostEvent& event) {
            return event.name == name &&
                   std::string(json_object_to_json_string(event.fields.get())).find(part) !=
                       std::string::npos;
        });
    };
    require(saw("settings", R"("source": "shell")") && saw("saved", R"("requests": [ 7 ])"),
            "the change and its save are reported");
    // Any integer id works, in any order.
    auto again = json_ptr(json_tokener_parse(R"({"dictation_send_gap":40})"));
    app.host_settings_set(again.get(), -2);
    app.host_settings_set(again.get(), 3);
    app.flush_settings();
    app.tick(clock += .1);
    host_events = app.take_host_events();
    require(saw("saved", R"("requests": [ -2, 3 ])"), "a save names exactly the requests it covers");
    std::vector<std::string> errors;
    auto saved = load_settings(directory.path, errors);
    require(!saved.auto_leveling && saved.dictation_send_gap == 40 && saved.replace_native_keyboard,
            "the change persists; the standalone flags stay as they were");

    // The shell's theme lasts for the process and is never saved.
    require(app.host_theme_set(R"({"schema_version":1,"name":"x"})").first == "invalid_theme",
            "an incomplete theme is refused with the validator's message");
    const std::string theme = R"({"schema_version":1,"id":"x","name":"Shell","surface":"#101010",
        "surface_radius":20,"key_top":"#303030","key_middle":"#282828","key_bottom":"#202020",
        "key_side_top":"#181818","key_side_bottom":"#101010","legend":"#ffffff","key_radius":8,
        "key_depth":3.5,"key_press_travel":3,"press_duration_ms":80,"font_size":20,"small_font_size":13,
        "padding":20,"hover":"#404040","latched":"#335577","disabled":"#151515"})";
    require(app.host_theme_set(theme).first.empty() && app.selection().theme == "shell",
            "a custom theme applies");
    auto apply = json_ptr(json_tokener_parse(R"({"pinned":true})"));
    app.host_settings_set(apply.get(), 8);
    app.flush_settings();
    require(load_settings(directory.path, errors).active.theme == "graphite",
            "the saved theme stays an installed one");
    app.host_theme_reset();
    require(app.selection().theme == "graphite", "reset returns to it");
}

// A Send authorized for one field never continues into the next, even when
// the context changes before the keyboard's next tick.
void hosted_send_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.hosted_fd = 99;
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    Sink sink;
    ShellRouter router(sink);
    options.shell_router = &router;
    App app(options, router);
    app.set_interaction_active(true);
    app.set_sink(true, "Test Shell");
    app.set_context(std::string("old"), {});
    open_editor(app);
    press(app, "KeyH");
    press(app, "KeyI");
    const auto [x, y] = key_center(app, "Dictation-send");
    app.down(0, x, y, clock);
    app.up(0, x, y);
    app.set_context(std::string("new"), {}); // in the same frame as the Send
    settle(app);
    for (const auto& event : router.take_events()) {
        require(event.text.find("hi") == std::string::npos,
                "the old field's draft never reaches the new one");
    }
    require(editor_text(app) == "hi", "the draft stays in the editor for a fresh Send");
}

// Type as I speak into the sink ends when the sink does: a phrase still
// being recognized never falls through to the focused application.
void hosted_sink_direct_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.hosted_fd = 99;
    use_fake_helper(options, directory.path, "slowdirect"); // the second phrase takes 0.3 s
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)");
    Sink sink;
    ShellRouter router(sink);
    options.shell_router = &router;
    App app(options, router);
    app.set_interaction_active(true);
    app.set_sink(true, "Test Shell");
    app.set_context(std::string("c"), {});
    press(app, "Dictation");
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        app.tick(clock += .01);
        for (const auto& event : router.take_events()) {
            router.acknowledge(event.context, event.seq);
        }
    }
    press(app, "Dictation"); // the second phrase waits for its text
    app.set_sink(false);
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        app.tick(clock += .01);
    }
    require(sink.text.empty(), "the late phrase does not reach the focused application");
    require(!app.listening(), "and Type as I speak has ended");
}

// A server the shell chose never receives the editor's unsent text.
void shell_server_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.hosted_fd = 99;
    use_fake_helper(options, directory.path, "default");
    write_config(directory.path, R"(,"dictation":"off")");
    Sink sink;
    ShellRouter router(sink);
    options.shell_router = &router;
    App app(options, router);
    app.set_interaction_active(true);
    auto values =
        json_ptr(json_tokener_parse(R"({"dictation":"server","dictation_server":"127.0.0.1"})"));
    require(app.host_settings_set(values.get(), 1).first.empty(), "the shell sets the server");
    open_editor(app);
    for (const auto* key : {"KeyS", "KeyE", "KeyC", "KeyR", "KeyE", "KeyT"}) {
        press(app, key);
    }
    const auto asked = prompts_sent(directory.path).size();
    press(app, "Dictation");
    press(app, "Dictation");
    wait_until(app, [&] { return prompts_sent(directory.path).size() > asked; });
    const auto prompts = prompts_sent(directory.path);
    require(prompts.back().find("ecret") == std::string::npos, "its prompt carries no editor text");
    app.flush_settings();
    std::ifstream config(directory.path / "config.json");
    const std::string text((std::istreambuf_iterator<char>(config)), std::istreambuf_iterator<char>());
    require(text.find(R"("dictation_server_source":"shell")") != std::string::npos ||
                text.find(R"("dictation_server_source": "shell")") != std::string::npos,
            "who chose the server is remembered");
}

// Correcting dictated text uses the layout's dead accents, as the keyboard does.
void accent_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")", "es-es",
                 "international-full");
    Sink sink;
    App app(options, sink);
    open_editor(app);
    press(app, "Quote"); // dead acute
    require(editor_text(app).empty(), "a dead accent waits for its letter");
    press(app, "KeyE");
    press(app, "Quote");
    press(app, "ShiftLeft");
    press(app, "KeyE");
    require(editor_text(app) == "éÉ", "a dead accent composes with the next letter, Shift included");
    require(sink.text.empty() && sink.keys.empty(), "the editor sends nothing to the target");
}

// Taking the headset off or a sleep closes the microphone at once: the
// editor keeps its text and says why; Type as I speak stops.
void away_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    const std::string reason = "Listening stopped: the headset was taken off.";
    {
        write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
        Sink sink;
        App app(options, sink);
        app.stop_listening(reason);
        require(app.view().status.empty(), "nothing to stop while not listening");
        press(app, "Dictation");
        require(app.listening(), "the mic key listens");
        app.stop_listening(reason);
        require(!app.listening() && app.dictation_open() && app.view().dictation->notice == reason &&
                    !app.view().dictation->notice_error,
                "in the editor, listening stops and the notice says why");
    }
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_direct":true)");
    Sink sink;
    App app(options, sink);
    press(app, "Dictation");
    require(app.listening() && !app.dictation_open(), "Type as I speak listens on the keyboard");
    app.stop_listening(reason);
    require(!app.listening() && app.view().status == reason, "Type as I speak stops and says why");
    require(sink.text.empty() && sink.keys.empty(), "stopping types nothing");
}

void card_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"off","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "silent");
    Sink sink;
    App app(options, sink);
    require(!app.view().dictation_available, "dictation starts off");
    app.show_settings();
    const std::array<const char*, 6> others{"dictation-server", "dictation-test", "dictation-fallback",
                                            "model-action",     "send-gap",       "dictation-direct"};
    require(has_control(app, "dictation") &&
                std::none_of(others.begin(), others.end(),
                             [&](const char* id) { return has_control(app, id); }),
            "with dictation off, the card shows only the mode");
    click(app, "dictation");
    click(app, "choose:dictation:server");
    require(field_label(app, "Not tested") && control_enabled(app, "dictation-test") &&
                has_control(app, "send-gap") && has_control(app, "dictation-direct"),
            "Self-hosted shows the server, not yet tested, and how Send delivers");
    click(app, "dictation-test");
    wait_until(app, [&] { return field_label(app, "Reachable"); });
    click(app, "apply");
    require(app.view().dictation_available, "Apply turns dictation on with the saved server");
    app.flush_settings();
    std::vector<std::string> errors;
    const auto saved = load_settings(directory.path, errors);
    require(errors.empty() && saved.dictation == "server" && saved.dictation_server == "127.0.0.1",
            "the mode and server are saved");
    require(sink.text.empty() && sink.keys.empty(), "the card sends nothing to the target");

    // The address popup: its own field and keys, Enter checks, Esc cancels.
    app.show_settings();
    auto type = [&](const std::string& keys) {
        for (char c : keys) {
            click(app, std::string("address-key-") + c);
        }
    };
    click(app, "dictation-server");
    require(app.view().address && app.view().address->text == "127.0.0.1",
            "the popup opens on the address");
    // A held Backspace repeats after half a second, without catching up.
    {
        const auto view = app.view();
        const auto key = std::find_if(view.controls.begin(), view.controls.end(),
                                      [](const Control& c) { return c.id == "address-key-backspace"; });
        require(key != view.controls.end(), "the popup has Backspace");
        const double x = key->bounds.x + key->bounds.width / 2,
                     y = key->bounds.y + key->bounds.height / 2;
        app.down(0, x, y, clock);
        app.tick(clock + .3);
        require(app.view().address->text == "127.0.0.", "one deletion on press");
        app.tick(clock + .6);
        app.tick(clock + .65);
        require(app.view().address->text == "127.0.", "held Backspace repeats");
        require(app.holds_repeat(0) && !app.holds_repeat(1),
                "it counts as repeating, off the panel too");
        app.up(0, x, y);
        require(!app.holds_repeat(0), "and no longer once released");
        clock += 1;
        app.tick(clock);
        require(app.view().address->text == "127.0.", "release stops the repeat");
    }
    require(control_enabled(app, "address-clear"), "Clear is offered while the field has text");
    click(app, "address-clear");
    require(app.view().address->text.empty() && app.view().address->cursor == 0 &&
                !control_enabled(app, "address-clear"),
            "Clear empties the field");
    type("speech.example.com");
    require(app.view().address->tone == PanelView::AddressView::Tone::Warning,
            "a public address shows the warning");
    for (int i = 0; i < 3; ++i) {
        click(app, "address-key-backspace");
    }
    type("local:");
    click(app, "address-key-enter");
    require(app.view().address && app.view().address->tone == PanelView::AddressView::Tone::Error,
            "an invalid address keeps the popup open with a reason");
    type("8031");
    click(app, "address-key-enter");
    require(!app.view().address, "a valid address closes the popup");
    const auto shown = app.view();
    require(std::any_of(shown.controls.begin(), shown.controls.end(),
                        [](const Control& c) {
                            return c.id == "dictation-server" && c.label == "speech.example.local:8031";
                        }),
            "the card shows the pending address");
    click(app, "dictation-server");
    type("x");
    click(app, "address-key-esc");
    require(!app.view().address, "Esc cancels");
    click(app, "dictation-test");
    wait_until(app, [&] { return field_label(app, "Reachable"); });
    click(app, "dictation-server");
    require(std::any_of(app.view().controls.begin(), app.view().controls.end(),
                        [](const Control& c) { return c.id == "address-recent-0"; }),
            "a tested server is offered among the recent ones");
    click(app, "address-recent-1");
    require(app.view().address->text == "127.0.0.1", "a recent server fills the field");
    click(app, "apply");
    require(app.view().address.has_value(), "the popup is modal: Settings underneath waits");
    click(app, "address-key-esc");
    click(app, "apply");
    require(!app.view().address && !app.view().settings, "Apply saves and closes Settings");
    app.flush_settings();
    const auto recent = load_settings(directory.path, errors).dictation_recent_servers;
    require(recent == std::vector<std::string>{"speech.example.local:8031", "127.0.0.1"},
            "recent servers are saved, newest first");
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
        require(!sink.gaps.empty() && sink.gaps.front() == .02 && sink.gaps.back() == .02,
                "Send sets its own pace, 20 ms by default, and restores the usual one");
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
        paste_tests();
        passthrough_tests();
        field_tests();
        settings_and_update_tests();
        card_tests();
        live_tests();
        language_tests();
        direction_tests();
        direct_tests();
        direct_paste_tests();
        device_tests();
        fallback_tests();
        microphone_tests();
        away_tests();
        accent_tests();
        hosted_tests();
        hosted_send_tests();
        hosted_sink_direct_tests();
        shell_server_tests();
        test_race_tests();
        send_during_reveal_tests();
        direct_esc_tests();
        direct_to_editor_tests();
        direct_pace_tests();
        send_wait_tests();
        download_helper_exit_tests();
        std::cout << "Dictation mode tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
