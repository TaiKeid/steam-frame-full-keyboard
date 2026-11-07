#include "bundled_profiles.hpp"
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
#include <sstream>
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
    bool single_line{};
    int commits_before_failure{-1}; // -1 never fails
    std::size_t acknowledged{};     // characters of a failed commit that arrived
    std::size_t commit_attempts{};
    std::uint32_t window{}; // Gamescope's focused window; 0 names none.
    std::uint32_t focused_window() const override { return window; }
    bool desktop{}; // A nested desktop has the keys.
    bool desktop_focused() const override { return desktop; }
    EditKeys edit{}; // The focused app's Cut/Copy/Paste keys, such as a terminal's.
    EditKeys edit_keys() const override { return edit; }
    void send(int code, int value) override { keys.emplace_back(code, value); }
    Delivery deliver_key(int code, int value) override {
        send(code, value);
        return Delivery::Success;
    }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        ++commit_attempts;
        // Like the local text path, which holds at most 32 characters at once.
        if (busy || g_utf8_strlen(value.c_str(), -1) > 32) {
            return false;
        }
        if (commits_before_failure == 0) {
            // A commit that delivered some characters had been submitted, as
            // a per-character commit is: the failure comes with a later call.
            // One that delivered none is refused before submission.
            failing = acknowledged > 0 && !failing;
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
    bool busy{}, failing{};
    bool text_pending() const override { return busy || failing; }
    // A busy per-character commit waits to send its next character.
    bool between_characters{};
    bool character_in_flight() const override { return text_pending() && !between_characters; }
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
// Settings, scrolled so the Dictation card shows whole.
void open_settings(App& app) {
    app.show_settings();
    app.move(9, 800, 300);
    app.scroll(9, 4, 0);
    for (int i = 0; i < 20; ++i) {
        app.tick(clock += .05);
    }
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
// Sends what the app logs to `out` while it lives.
struct CaptureLog {
    std::streambuf* previous;
    explicit CaptureLog(std::ostream& out) : previous(std::cout.rdbuf(out.rdbuf())) {}
    ~CaptureLog() { std::cout.rdbuf(previous); }
};

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
             {"de-de", "de"}, {"fr-fr", "fr"}, {"it-it", "it"},  {"pt-br", "pt"}, {"ru-ru", "ru"},
             {"uk-ua", "uk"}, {"ko-kr", "ko"}, {"ar", "ar"},     {"cs-cz", "cs"}, {"da-dk", "da"},
             {"el-gr", "el"}, {"et-ee", "et"}, {"fi-fi", "fi"},  {"fr-ca", "fr"}, {"he-il", "he"},
             {"hi-in", "hi"}, {"hr-hr", "hr"}, {"hu-hu", "hu"},  {"id-id", "id"}, {"lt-lt", "lt"},
             {"lv-lv", "lv"}, {"ms-my", "ms"}, {"nb-no", "no"},  {"nl-nl", "nl"}, {"pl-pl", "pl"},
             {"ro-ro", "ro"}, {"sk-sk", "sk"}, {"sl-si", "sl"},  {"sv-se", "sv"}, {"th-th", "th"},
             {"tr-tr", "tr"}, {"vi-vn", "vi"}, {"fil-ph", "tl"}, {"ka-ge", "ka"}, {"hy-am", "hy"},
             {"kk-kz", "kk"}, {"az-az", "az"}, {"mk-mk", "mk"},  {"sq-al", "sq"}, {"sr-rs", "sr"},
             {"fa-ir", "fa"}, {"ca-es", "ca"}, {"is-is", "is"}}) {
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
    for (const auto* gone :
         {"F1", "F12", "PrintScreen", "Tab", "Insert", "ControlLeft", "MetaLeft", "AltRight",
          "ContextMenu", "ControlRight", "Copy", "Cut", "Paste", "Emoji"}) {
        require(!has(gone), "dictation mode drops keys that cannot edit text");
    }
    for (const auto* kept : {"Escape", "KeyA", "Backspace", "Enter", "ShiftLeft", "CapsLock", "Home",
                             "PageUp", "ArrowUp", "Numpad7", "Dictation", "Dictation-send",
                             "Dictation-undo", "Dictation-copy", "Dictation-cut", "Dictation-paste"}) {
        require(has(kept), "dictation mode keeps editing keys and adds its own");
    }
    auto bounds_in = [](const Layout& layout, const char* id) {
        return std::find_if(layout.keys.begin(), layout.keys.end(),
                            [&](const Key& key) { return key.id == id; })
            ->bounds;
    };
    require(bounds_in(mode.layout, "Dictation-cut") == bounds_in(main, "ControlLeft") &&
                bounds_in(mode.layout, "Dictation-copy") == bounds_in(main, "MetaLeft") &&
                bounds_in(mode.layout, "Dictation-paste") == bounds_in(main, "AltLeft") &&
                bounds_in(mode.layout, "Dictation-undo") == Rect{0, 186, 79, 110},
            "Cut, Copy and Paste take the left modifiers' slots; Undo sits above the mic key, as tall");
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
    // The flick layout has no function-key row: its keys move down to make
    // the editor's room. Without Tab, Send takes the emoji key's place, and
    // the mode adds Esc in the side column's free top slot.
    auto emoji = bounds_in(flick, "Emoji");
    emoji.y += function_row_room;
    const auto flick_escape =
        std::find_if(flick_mode.layout.keys.begin(), flick_mode.layout.keys.end(), [](const Key& key) {
            return key.action_kind == ActionKind::Key && key.action == "Escape";
        });
    require(flick_send != flick_mode.layout.keys.end() && flick_send->bounds == emoji &&
                flick_mode.layout.height == flick.height + function_row_room &&
                flick_mode.editor.y + flick_mode.editor.height <= emoji.y,
            "without right-hand modifiers or Tab, Send takes the emoji key's slot");
    require(flick_escape != flick_mode.layout.keys.end() &&
                flick_escape->bounds.x == bounds_in(flick, "Paste").x &&
                flick_escape->bounds.y == function_row_room &&
                flick_mode.editor.y + flick_mode.editor.height <= flick_escape->bounds.y,
            "Esc goes in the side column's top slot, below the editor");
    {
        // The English page's editor lines up with the kana page's.
        const auto& page = profiles.layouts.at("ja-flick-qwerty");
        const auto english = dictation_layout(page, page.width);
        require(english.editor.x == flick_mode.editor.x &&
                    english.editor.width == flick_mode.editor.width,
                "the flick layout's English page keeps the editor where the kana page has it");
    }
    {
        const auto& source = profiles.layouts.at("ja-flick-12");
        const auto stacked = dictation_layout(source, source.width);
        const auto copy = bounds_in(stacked.layout, "Dictation-copy");
        const auto cut = bounds_in(stacked.layout, "Dictation-cut");
        const auto paste = bounds_in(stacked.layout, "Dictation-paste");
        const auto undo = bounds_in(stacked.layout, "Dictation-undo");
        auto slot = bounds_in(source, "Paste");
        if (stacked.layout.height > source.height) {
            slot.y += function_row_room; // Moved down for the editor.
        }
        require(cut.x == slot.x && cut.width == slot.width && cut.y + cut.height < copy.y &&
                    copy.y + copy.height < paste.y && paste.y + paste.height < undo.y &&
                    undo.y + undo.height == slot.y + slot.height,
                "without left modifiers, the keys stack above Undo, ending with Paste's slot");
        require(copy.y >= 110, "the stack starts below the editor's top row");
    }
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
        with open(os.path.join(os.path.dirname(sys.argv[0]), "tests"), "a") as log:
            log.write(json.dumps(message) + "\n")
        if scenario == "testexit":
            exits = os.path.join(os.path.dirname(sys.argv[0]), "test-exits")
            with open(exits, "ab") as log:
                log.write(b"1")
            with open(os.path.join(os.path.dirname(sys.argv[0]), "test-exit-pid"), "w") as log:
                log.write(str(os.getpid()))
            if os.path.getsize(exits) == 1:
                import signal, time
                # EOF is visible while process cleanup is still in progress.
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                peer.close()
                release = os.path.join(os.path.dirname(sys.argv[0]), "test-exit-release")
                while not os.path.exists(release):
                    time.sleep(0.01)
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
        if scenario == "gameover":
            send(op="microphone", unfiltered=True)
            send(op="microphone", unfiltered=False)
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
        if scenario == "sleepwait":
            import time
            release = os.path.join(os.path.dirname(sys.argv[0]), "release-sleep-phrase")
            while not os.path.exists(release):
                time.sleep(0.01)
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

// Key flicks and dictation: a character the other hand holds as the mic
// opens the editor types into the app first, and flicks type into the editor.
void key_flick_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","key_flicks":true)");
    Sink sink;
    App app(options, sink);
    const auto [ax, ay] = key_center(app, "KeyA");
    app.down(1, ax, ay, clock);
    press(app, "Dictation");
    app.up(1, ax, ay);
    require(app.dictation_open() && sink.text == std::vector<std::string>{"a"},
            "the held a types into the app before the mic opens the editor");
    press(app, "Dictation");
    settle_pause(app);
    const auto [x, y] = key_center(app, "Digit6");
    app.down(0, x, y, clock);
    app.move(0, x, y + 60);
    app.up(0, x, y + 60);
    require(editor_text(app) == "^" && sink.text.size() == 1,
            "a flick types its Shift character into the editor");
}
// A kana the other hand holds as the mic opens the editor joins the reading,
// which goes into the app first.
void held_kana_mic_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")", "ja-flick",
                 "ja-flick-12");
    Sink sink;
    App app(options, sink);
    press(app, "Flick-ka");
    const auto [ax, ay] = key_center(app, "Flick-a");
    app.down(1, ax, ay, clock);
    press(app, "Dictation");
    app.up(1, ax, ay);
    require(app.dictation_open() && sink.text == std::vector<std::string>{"かあ"} &&
                editor_text(app).empty(),
            "the held kana joins the reading, which types into the app before the mic opens the editor");
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

void modal_repeat_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.models_dir = directory.path / "models";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    for (const auto* key : {"Backspace", "KeyX", "ArrowLeft"}) {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, "abcd");
        const auto [x, y] = key_center(app, key);
        require(app.down(0, x, y, clock) && app.holds_repeat(0), "an editing key starts repeating");
        const auto text = editor_text(app);
        const auto cursor = app.view().dictation->cursor;
        press(app, "Escape", 1);
        require(app.view().dictation->notice == "Discard text?", "the second hand opens the question");
        require(!app.holds_repeat(0), "opening the modal ends the first hand's repeat");
        app.tick(clock += .6);
        require(editor_text(app) == text && app.view().dictation->cursor == cursor,
                "neither text nor cursor changes behind the question");
        const auto controls = app.view().controls;
        const auto keep = std::find_if(controls.begin(), controls.end(), [](const Control& control) {
            return control.id == "dictation-keep";
        });
        require(keep != controls.end(), "Keep is offered");
        const auto& r = keep->bounds;
        app.down(1, r.x + r.width / 2, r.y + r.height / 2, clock);
        app.up(1, r.x + r.width / 2, r.y + r.height / 2);
        app.tick(clock += .6);
        require(editor_text(app) == text && app.view().dictation->cursor == cursor,
                "Keep does not restart a key still held across the question");
        app.up(0, x, y);
        require(app.down(0, x, y, clock), "a fresh editing press works after Keep");
        const auto fresh_text = editor_text(app);
        const auto fresh_cursor = app.view().dictation->cursor;
        app.tick(clock += .6);
        require(editor_text(app) != fresh_text || app.view().dictation->cursor != fresh_cursor,
                "a fresh held key repeats normally");
        app.up(0, x, y);
        require(sink.text.empty() && sink.keys.empty(), "editing stays local");
    }
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
        require(!has_control(app, "ime-convert"), "Space converts: no Convert button");
        press(app, "Escape");
        require(app.dictation_open() && editor_text(app) == "Hello there.Again." &&
                    app.view().dictation->notice.empty(),
                "Esc cancels the reading, not the mode");
        type_text(app, "kana");
        press(app, "Enter");
        view = app.view();
        require(view.dictation->text == "Hello there.Again.かな" &&
                    view.dictation->preedit_end == view.dictation->preedit_begin &&
                    !view.dictation->candidates,
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
    if (CjkComposer::supported("korean-2set")) {
        // A reading that starts over a selection replaces it at once, as on
        // a PC, instead of leaving it lit until its first syllable is done.
        write_config(directory.path, server, "ko-kr");
        Sink sink;
        App app(options, sink);
        open_editor(app);
        wait_until(app, [&] { return editor_text(app) == "Hello there."; });
        settle(app);
        for (int i = 0; i < 6; ++i) {
            press(app, "ShiftLeft");
            press(app, "ArrowLeft");
        }
        require(app.view().dictation->selection_begin != app.view().dictation->selection_end,
                "there. is selected");
        press(app, "KeyG");
        // The editor shows the reading in its text, at the cursor.
        require(editor_text(app) == "Hello ㅎ" &&
                    app.view().dictation->selection_begin == app.view().dictation->selection_end,
                "the first jamo replaces the selection at once");
        type_text(app, "ks");
        press(app, "Space");
        require(editor_text(app) == "Hello 한 ", "and the syllable goes where it was");
        // A held Backspace that took the open syllable edits nothing behind
        // the discard question the other hand opens.
        type_text(app, "g");
        const auto [bx, by] = key_center(app, "Backspace");
        app.down(0, bx, by, clock);
        app.tick(clock += .05);
        require(editor_text(app) == "Hello 한 ", "the held Backspace takes ㅎ");
        press(app, "Escape", 1);
        for (int i = 0; i < 30; ++i) {
            app.tick(clock += .05);
        }
        app.up(0, bx, by);
        app.tick(clock += .05);
        require(editor_text(app) == "Hello 한 ", "and edits nothing behind the discard question");
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
        app.tick(clock += .1);
        {
            const auto view = app.view();
            const auto& editor = view.dictation->bounds;
            const auto& strip = view.candidates;
            require(strip && !strip->chips.empty() && strip->area.x >= editor.x &&
                        strip->area.x + strip->area.width <= editor.x + editor.width &&
                        strip->area.y >= editor.y &&
                        strip->area.y + strip->area.height <= editor.y + editor.height,
                    "Pinyin candidates sit in the band over the editor's foot");
        }
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
    {
        // Ending a selection follows the screen too, as the arrows do.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        for (const auto* id : {"KeyA", "KeyT", "KeyC"}) {
            press(app, id);
        }
        auto select_toward_start = [&](int steps) {
            for (int i = 0; i < steps; ++i) {
                press(app, "ShiftLeft");
                press(app, "ArrowRight");
            }
        };
        select_toward_start(3);
        require(app.view().dictation->selection_begin == 0 && app.view().dictation->selection_end == 6,
                "Shift and Right select toward the start of Hebrew text");
        press(app, "ArrowLeft");
        press(app, "KeyV");
        require(editor_text(app) == "שאבה", "Left ends the selection at its left edge, the text's end");
        select_toward_start(4);
        press(app, "ArrowRight");
        press(app, "KeyB");
        require(editor_text(app) == "נשאבה", "Right ends it at its right edge, the text's start");
    }
    {
        // Mixed directions: digits run left to right inside a Hebrew line.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        for (const auto* id : {"KeyC", "Space", "Digit1", "Digit2"}) {
            press(app, id);
        }
        require(editor_text(app) == "ב 12", "Hebrew with digits");
        const auto view = app.view();
        // In this line a caret after the digits shows left of them, and one
        // before them right of them, however the digits themselves read.
        require(editor_selection_edge(view, 3, 5, -1) == 5 && editor_selection_edge(view, 3, 5, 1) == 3,
                "the caret lands at that edge of the digits");
        require(editor_selection_edge(view, 0, 2, -1) == 2 && editor_selection_edge(view, 0, 2, 1) == 0,
                "and of the Hebrew beside them");
        require(editor_selection_edge(view, 0, 5, -1) == 5 && editor_selection_edge(view, 0, 5, 1) == 0,
                "a selection of both ends where the line does");
    }
    {
        // Across lines, the direction of the text decides, not the columns.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        for (const auto* id : {"KeyA", "KeyT", "Enter", "KeyC"}) {
            press(app, id);
        }
        press(app, "ShiftLeft");
        press(app, "ArrowUp");
        require(app.view().dictation->selection_end == editor_text(app).size(),
                "the selection reaches the end of the second line");
        press(app, "ArrowLeft");
        press(app, "KeyV");
        require(editor_text(app) == "שא\nבה", "Left ends a right-to-left selection at the text's end");
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
        open_settings(app);
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
        open_settings(app);
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
        open_settings(app);
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
        // Another window takes focus mid-session (R19, R44): its phrases wait,
        // listening paused, until Enter types them where focus is then.
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return !sink.text.empty(); });
        sink.window = 9;
        press(app, "Dictation");
        wait_until(app, [&] {
            return sink.text.size() > 1 || app.view().status.starts_with("Another window took focus.");
        });
        require(sink.text == std::vector<std::string>{"Hello there."},
                "once the session typed into a window, a phrase for it never reaches the one clicked");
        press(app, "Enter");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} && sink.keys.empty(),
                "Enter types it into the window focused now");
    }
    {
        // A mic tap before the window is chosen, as Steam's page asks while
        // it has the keys: the session is for the window focused when its
        // first phrase goes out. Focus that moves while the page has the
        // keys stops nothing either; the window picked next decides.
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        const auto asked = prompts_sent(directory.path).size();
        const auto recognized = [&](std::size_t phrases) {
            return prompts_sent(directory.path).size() == asked + phrases &&
                   !app.view().dictation_recognizing;
        };
        app.set_dashboard_focus(true);
        press(app, "Dictation");
        wait_until(app, [&] { return recognized(1); });
        sink.window = 9; // the window clicked as asked
        app.set_dashboard_focus(false);
        settle(app);
        require(sink.text == std::vector<std::string>{"Hello there."} &&
                    app.view().dictation_capturing && app.view().status.empty(),
                "the first phrase goes to the window clicked after the mic tap");
        app.set_dashboard_focus(true);
        sink.window = 7; // opening the dashboard moved the focus by itself
        press(app, "Dictation");
        wait_until(app, [&] { return recognized(2); });
        sink.window = 9; // back to the session's window
        app.set_dashboard_focus(false);
        settle(app);
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} &&
                    app.view().status.empty(),
                "a focus change while the page had the keys stops nothing");
    }
    {
        // The dashboard opened to pick the window after a phrase was stopped
        // midway: Steam's page has the keys meanwhile, so nothing goes out
        // and Enter stays local; the rest waits for Enter once a window has them.
        options.dictation_arguments = {"--scenario", "long"};
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        sink.busy = true; // its first 32 characters are still being typed
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.commit_attempts > 0; });
        sink.window = 9;
        sink.acknowledged = 5; // "This " reached the window
        wait_until(app, [&] { return app.view().status.starts_with("Another window took focus."); });
        sink.busy = false;
        const auto card = app.view().status;
        app.set_dashboard_focus(true);
        press(app, "Enter");
        settle(app);
        require(sink.text.empty() && sink.keys.empty() && app.view().status == card,
                "while Steam's page has the keys nothing goes out, Enter stays local, the card stays");
        sink.window = 7;
        app.set_dashboard_focus(false);
        settle(app);
        require(sink.text.empty() && app.view().status == card,
                "the rest waits for Enter once the window has the keys");
        press(app, "Enter");
        settle(app);
        std::string typed;
        for (const auto& piece : sink.text) {
            typed += piece;
        }
        require(typed == "phrase is much longer than thirty-two characters, so it goes out in pieces." &&
                    sink.keys.empty(),
                "Enter types the rest into the window picked");
        options.dictation_arguments = {"--scenario", "direct"};
    }
    {
        // An Enter to send the message, and the next phrase, queued behind
        // the phrase being typed: the click drops both, and the card says so.
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        const auto asked = prompts_sent(directory.path).size();
        sink.busy = true; // "Hello there." is still being typed
        press(app, "Dictation");
        wait_until(app, [&] { return sink.commit_attempts > 0; });
        press(app, "Enter");
        press(app, "Dictation");
        wait_until(app, [&] {
            return prompts_sent(directory.path).size() == asked + 2 && !app.view().dictation_recognizing;
        });
        sink.window = 9;
        sink.acknowledged = 6; // "Hello " reached the window
        wait_until(app, [&] { return app.view().status.starts_with("Another window took focus."); });
        sink.busy = false;
        const auto card = app.view().status;
        require(card.ends_with("Queued actions and later text were discarded."),
                "the card says the queued Enter and the next phrase were dropped");
        // Measured larger than the card draws it: it fits with room to spare.
        require(candidate_text_width(card, app.view().language->font) + 40 <= panel_width - 16,
                "and it fits on the card");
        press(app, "Enter");
        settle(app);
        require(sink.text == std::vector<std::string>{"there."} && sink.keys.empty(),
                "Enter types the rest alone");
    }
    {
        // A phrase waiting behind text still being typed, here a full queue
        // of keys: one click stops both. One Enter types the text's rest,
        // then the phrase, into the window focused then; while Steam's page
        // has the keys, it types neither.
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        const auto asked = prompts_sent(directory.path).size();
        press(app, "Dictation");
        wait_until(app, [&] { return !sink.text.empty(); });
        sink.busy = true; // the first key's text is still being typed
        for (std::size_t i = 0; i < OutputController::capacity; ++i) {
            press(app, "KeyA");
        }
        press(app, "Dictation");
        wait_until(app, [&] {
            return prompts_sent(directory.path).size() == asked + 2 && !app.view().dictation_recognizing;
        });
        sink.window = 9;
        settle(app);
        sink.busy = false;
        const auto card = app.view().status;
        require(sink.text == std::vector<std::string>{"Hello there."} &&
                    card.starts_with("Another window took focus."),
                "the click stops the keys' text and the phrase behind it");
        app.set_dashboard_focus(true);
        press(app, "Enter");
        settle(app);
        require(sink.text.size() == 1 && app.view().status == card,
                "while Steam's page has the keys, Enter types nothing and the card stays");
        app.set_dashboard_focus(false);
        press(app, "Enter");
        settle(app);
        require(sink.text.size() == OutputController::capacity + 2 &&
                    std::count(sink.text.begin(), sink.text.end(), "a") == OutputController::capacity &&
                    sink.text.back() == " Again." && sink.keys.empty(),
                "one Enter types the rest, then the phrase");
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
    bool desktop{true}; // the nested desktop's own clipboard took it too
    std::uint64_t write(const std::string& text) override {
        written.push_back(text);
        return written.size();
    }
    std::optional<bool> result(std::uint64_t) const override { return available; }
    std::optional<bool> desktop_result(std::uint64_t) const override { return available && desktop; }
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
        // A nested desktop pastes from its own clipboard: taken only on
        // Gamescope's, Ctrl+V would paste what the desktop held before.
        Sink sink;
        sink.desktop = true;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        clipboard.desktop = false;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} && sink.keys.empty(),
                "in a desktop whose clipboard isn't ours, the phrases are typed, not pasted");
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

// The keys a chord presses and releases, modifiers first and last.
Keys chord_keys(const std::vector<int>& modifiers, int letter) {
    Keys keys;
    for (const int modifier : modifiers) {
        keys.emplace_back(modifier, 1);
    }
    keys.emplace_back(letter, 1);
    keys.emplace_back(letter, 0);
    for (auto modifier = modifiers.rbegin(); modifier != modifiers.rend(); ++modifier) {
        keys.emplace_back(*modifier, 0);
    }
    return keys;
}
Keys operator+(Keys first, const Keys& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}
// Cut, Copy and Paste use a terminal's own keys, and nothing changes
// elsewhere (docs/terminal-edit-keys-design.md). Keys go to the test's own
// sink, never to a focused application.
void terminal_edit_key_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, "");
    const auto press_edit_keys = [&](Sink& sink) {
        App app(options, sink);
        for (const auto* id : {"Cut", "Copy", "Paste"}) {
            press(app, id);
        }
        settle(app);
    };
    {
        Sink sink;
        press_edit_keys(sink);
        require(sink.keys == chord_keys({KEY_LEFTCTRL}, KEY_X) + chord_keys({KEY_LEFTCTRL}, KEY_C) +
                                 chord_keys({KEY_LEFTCTRL}, KEY_V),
                "outside terminals: Ctrl+X, Ctrl+C, Ctrl+V as before");
    }
    {
        Sink sink;
        sink.edit = EditKeys::CtrlShift;
        press_edit_keys(sink);
        const std::vector<int> shift{KEY_LEFTCTRL, KEY_LEFTSHIFT};
        require(sink.keys == chord_keys(shift, KEY_C) + chord_keys(shift, KEY_V),
                "in a terminal: Cut sends nothing, then Ctrl+Shift+C and Ctrl+Shift+V; never Ctrl+C");
    }
    {
        Sink sink;
        sink.edit = EditKeys::CtrlAlt;
        press_edit_keys(sink);
        const std::vector<int> alt{KEY_LEFTCTRL, KEY_LEFTALT};
        require(sink.keys == chord_keys(alt, KEY_C) + chord_keys(alt, KEY_V),
                "rxvt-unicode: Ctrl+Alt+C and Ctrl+Alt+V");
    }
    {
        Sink sink;
        sink.edit = EditKeys::SelectionOnly;
        App app(options, sink);
        for (const auto* id : {"Cut", "Copy", "Paste"}) {
            press(app, id);
        }
        settle(app);
        require(sink.keys.empty() && sink.text.empty(), "xterm: no key reaches its program");
        const auto disabled = app.view().disabled_keys;
        require(disabled.contains("Cut") && disabled.contains("Copy") && disabled.contains("Paste"),
                "and all three show disabled there");
        press(app, "KeyA");
        settle(app);
        require(sink.text == std::vector<std::string>{"a"}, "and the keyboard still types");
    }
    {
        // Cut shows disabled in a terminal, which has none; elsewhere all three stay usable.
        Sink sink;
        App app(options, sink);
        settle(app);
        const auto none = app.view().disabled_keys;
        require(!none.contains("Cut") && !none.contains("Copy") && !none.contains("Paste"),
                "outside terminals the three keys are usable");
        sink.edit = EditKeys::CtrlShift;
        const auto cut = app.view().disabled_keys;
        require(cut.contains("Cut") && !cut.contains("Copy") && !cut.contains("Paste"),
                "in a terminal Cut alone shows disabled");
    }
    {
        // A shell's terminal hint applies where the program isn't known,
        // and ends with its context.
        Sink sink;
        App app(options, sink);
        App::HostHints terminal;
        terminal.purpose = "terminal";
        app.set_context(std::string("c-1"), terminal);
        press(app, "Copy");
        settle(app);
        require(sink.keys == chord_keys({KEY_LEFTCTRL, KEY_LEFTSHIFT}, KEY_C),
                "a field the shell calls a terminal gets Ctrl+Shift+C");
        sink.keys.clear();
        app.set_context(std::string("c-2"), {});
        press(app, "Copy");
        settle(app);
        require(sink.keys == chord_keys({KEY_LEFTCTRL}, KEY_C), "the next field gets Ctrl+C again");
    }
    {
        // A terminal detected after the press changes nothing already held:
        // the release matches the press.
        Sink sink;
        App app(options, sink);
        const auto [x, y] = key_center(app, "Copy");
        app.down(0, x, y, clock);
        app.tick(clock += .05);
        sink.edit = EditKeys::CtrlShift;
        app.up(0, x, y);
        settle(app);
        require(sink.keys == chord_keys({KEY_LEFTCTRL}, KEY_C),
                "a focus change while held releases exactly what went down");
    }
}
// Dictation's paste Send and Type as I speak paste with a terminal's own
// key, and type into a terminal that has none.
void terminal_paste_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1","dictation_send_paste":true)");
    use_fake_helper(options, directory.path, "silent");
    const std::vector<int> shift{KEY_LEFTCTRL, KEY_LEFTSHIFT};
    {
        Sink sink;
        sink.edit = EditKeys::CtrlShift;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "hi there");
        press(app, "Dictation-send");
        settle(app);
        require(clipboard.written == std::vector<std::string>{"hi there"} &&
                    sink.keys == chord_keys(shift, KEY_V) && sink.text.empty(),
                "paste Send into a terminal presses Ctrl+Shift+V");
        require(!app.dictation_open(), "and the pasted text returns to the keyboard");
    }
    {
        Sink sink;
        sink.edit = EditKeys::SelectionOnly;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "hi there");
        press(app, "Dictation-send");
        settle(app);
        std::string typed;
        for (const auto& piece : sink.text) {
            typed += piece;
        }
        require(clipboard.written.empty() && sink.keys.empty() && typed == "hi there",
                "xterm has no paste key: Send types the text and leaves the clipboard alone");
    }
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                                 R"("dictation_send_paste":true,"dictation_direct":true)");
    use_fake_helper(options, directory.path, "direct");
    {
        Sink sink;
        sink.edit = EditKeys::CtrlShift;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.keys.size() == 6; });
        press(app, "Dictation");
        wait_until(app, [&] { return sink.keys.size() == 12; });
        settle(app);
        require(clipboard.written == std::vector<std::string>{"Hello there.", " Again."} &&
                    sink.keys == chord_keys(shift, KEY_V) + chord_keys(shift, KEY_V) &&
                    sink.text.empty(),
                "Type as I speak pastes each phrase with Ctrl+Shift+V");
    }
    {
        Sink sink;
        sink.edit = EditKeys::SelectionOnly;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        require(sink.text == std::vector<std::string>{"Hello there.", " Again."} && sink.keys.empty() &&
                    clipboard.written.empty(),
                "in xterm the phrases are typed");
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
        const Keys expected{{KEY_LEFTCTRL, 1}, {KEY_V, 1}, {KEY_V, 0}, {KEY_LEFTCTRL, 0}};
        require(sink.keys == expected && sink.text.empty(), "Ctrl+V and no Enter; nothing typed");
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
        require(app.dictation_open() && editor_text(app) == "kept",
                "a paste nobody takes keeps the text");
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
        app.set_passthrough(false); // It is saved; the next launch must start without it.
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

void passthrough_message_tests() {
    const std::string message = "The keyboard stayed open because dictation is active.";
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "silent");
    auto notice = [](const App& app) { return app.view().dictation->notice; };
    {
        // Nothing switched, so there is nothing to explain.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        require(!app.dashboard_visibility(false) && notice(app).empty(),
                "a paused editor closes with the dashboard, without the message");
        press(app, "Escape");
        app.set_passthrough(true);
        press(app, "Dictation");
        require(!app.dashboard_visibility(false) && notice(app).empty(),
                "the user's own passthrough needs no message");
        press(app, "Escape");
        press(app, "Escape");
        app.set_passthrough(false); // It is saved; the next launch must start without it.
    }
    {
        // The editor shows it in its band, for five seconds from the next tick.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dashboard_visibility(false) && notice(app) == message,
                "the editor explains why the keyboard stayed");
        require(!app.dashboard_visibility(false), "the message is shown once");
        app.tick(clock += .01);
        app.tick(clock += 4.9);
        require(notice(app) == message, "it is still there just before five seconds");
        app.dirty = false;
        app.tick(clock += .2);
        require(notice(app).empty() && app.dirty, "and clears itself, repainting the panel");
        require(app.passthrough(), "while passthrough stays on");
    }
    {
        // A newer message in the editor outlives the timer.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        app.tick(clock += .01);
        app.stop_listening("Dictation stopped: the headset was taken off.");
        app.tick(clock += 6);
        require(notice(app) == "Dictation stopped: the headset was taken off.",
                "a newer notice stays past the five seconds");
    }
    {
        // The field usually closes with the dashboard: its own notice follows.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        app.set_field_needed(true);
        require(app.dashboard_visibility(false) && notice(app) == message,
                "the message comes first when the field closes too");
        app.tick(clock += .01);
        app.tick(clock += 5.1);
        require(notice(app) == "The text field closed. Select a text field to send.",
                "and the field's notice takes over");
    }
    {
        // Reopening the dashboard leaves nothing to explain, and passthrough
        // being on already, closing it again is no new switch.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        app.tick(clock += .01);
        require(!app.dashboard_visibility(true) && notice(app).empty(),
                "reopening the dashboard removes the message");
        require(!app.dashboard_visibility(false) && notice(app).empty(),
                "closing it again does not repeat it");
    }
    {
        // Turning passthrough off makes the message wrong at once.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        app.tick(clock += .01);
        app.set_passthrough(false);
        app.tick(clock += .01);
        require(notice(app).empty(), "it leaves with passthrough");
    }
    options.dictation_arguments = {"--scenario", "direct"};
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")"
                                 R"(,"dictation_direct":true)");
    {
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        app.set_passthrough(true);
        press(app, "Dictation");
        require(!app.dashboard_visibility(false) && app.view().status != message,
                "nor does Type as I speak with the user's own passthrough");
        app.set_passthrough(false);
    }
    {
        // Type as I speak has no editor: the status line shows it.
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        require(app.dashboard_visibility(false) && app.view().status == message,
                "the status line explains why the keyboard stayed");
        app.tick(clock += .01);
        app.tick(clock += 5.1);
        require(app.view().status != message, "and clears itself");
    }
    {
        // A newer status outlives the timer.
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        app.tick(clock += .01);
        app.report_status("Another message");
        app.tick(clock += 6);
        require(app.view().status == "Another message", "a newer status stays past the five seconds");
    }
    {
        // Once the last phrase is typed the keyboard closes, so the message
        // must not wait there for the next time it opens.
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        require(app.dashboard_visibility(false), "listening with the dashboard closed");
        press(app, "Dictation");
        wait_until(app, [&] { return sink.text.size() == 2; });
        app.tick(clock += .1);
        require(!app.passthrough() && app.view().status != message,
                "it leaves with the session's passthrough");
    }
}

void leaving_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")"
                                 R"(,"replace_native_keyboard":true)");
    use_fake_helper(options, directory.path, "silent");
    auto open_with_text = [](App& app) {
        open_editor(app);
        press(app, "KeyA");
        require(editor_text(app) == "a", "the editor has text");
        open_settings(app);
    };
    {
        Sink sink;
        App app(options, sink);
        open_with_text(app);
        click(app, "quit");
        auto view = app.view();
        require(view.confirm && view.confirm->title == "Dictation is active" &&
                    view.confirm->body.find("cancel the current dictation") != std::string::npos &&
                    !app.quitting(),
                "Quit asks before ending dictation");
        require(has_control(app, "confirm-accept"), "the question has its answers");
        click(app, "confirm-cancel");
        require(!app.view().confirm && !app.quitting() && app.dictation_open(),
                "Go back changes nothing");
        click(app, "quit");
        click(app, "confirm-accept");
        require(app.quitting(), "and the answer quits");
    }
    {
        Sink sink;
        App app(options, sink);
        open_editor(app);
        open_settings(app);
        click(app, "quit");
        require(!app.view().confirm && app.quitting(), "an empty, paused editor has nothing to lose");
    }
    {
        // Listening counts, though nothing has been said.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        settle(app);
        open_settings(app);
        require(app.view().dictation_capturing && editor_text(app).empty(),
                "the microphone stays on in Settings");
        click(app, "quit");
        require(app.view().confirm && !app.quitting(), "a silent microphone asks too");
        click(app, "confirm-cancel");
        require(app.view().dictation_capturing, "and goes on listening after Go back");
        app.back();
        require(app.view().dictation_capturing, "back on the keyboard too");
    }
    {
        // What a capture was started with pauses it; the rest does not.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        open_settings(app);
        click(app, "apply");
        app.flush_settings();
        require(app.view().dictation_capturing, "an unrelated Apply keeps listening");
        open_settings(app);
        click(app, "reload");
        require(!app.view().dictation_capturing, "Reload pauses it");
    }
    {
        // Settings covers the editor's band, so the status line explains.
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        open_settings(app);
        require(app.dashboard_visibility(false) && !app.view().status.empty() &&
                    app.view().status.find("dictation is active") != std::string::npos,
                "closing the dashboard in Settings explains itself");
        app.tick(clock += .01);
        app.tick(clock += 5.1);
        require(app.view().status.find("dictation is active") == std::string::npos,
                "and the message leaves by itself");
        app.set_passthrough(false);
    }
    {
        // Only the fallback's own question and its answer are tested here;
        // the button itself needs a native session.
        Sink sink;
        App app(options, sink);
        open_with_text(app);
        app.set_native_session(true);
        click(app, "native-fallback");
        require(app.view().confirm && !app.take_native_fallback(), "Use Steam keyboard asks too");
        click(app, "confirm-cancel");
        require(app.dictation_open() && !app.take_native_fallback(), "and keeps dictation on no");
        click(app, "native-fallback");
        click(app, "confirm-accept");
        require(app.take_native_fallback() && !app.dictation_open() && !app.dictation_holds_panel(),
                "on yes the dictation is discarded and Steam's keyboard takes over");
        app.back();
        open_editor(app);
        require(editor_text(app).empty(), "its text is gone");
    }
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")"
                                 R"(,"replace_native_keyboard":true,"dictation_direct":true)");
    options.dictation_arguments = {"--scenario", "direct"};
    {
        // Type as I speak: phrases still on their way are at stake.
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        settle(app);
        open_settings(app);
        require(app.view().dictation_capturing, "Type as I speak keeps listening in Settings");
        click(app, "quit");
        require(app.view().confirm && !app.quitting(), "Type as I speak asks too");
        click(app, "confirm-accept");
        require(app.quitting(), "and quits on yes");
    }
    {
        Sink sink;
        App app(options, sink);
        app.set_interaction_active(true);
        press(app, "Dictation");
        settle(app);
        open_settings(app);
        app.set_native_session(true);
        click(app, "native-fallback");
        require(app.view().confirm && !app.take_native_fallback(), "the Steam keyboard asks too");
        click(app, "confirm-accept");
        require(app.take_native_fallback() && !app.view().dictation_capturing,
                "and on yes the session stops");
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
    open_settings(app);
    require(app.view().dictation_capturing, "opening Settings keeps listening");
    app.back();
    require(app.view().dictation_capturing, "and it is still listening after Settings");
    press(app, "Dictation");
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

bool field_label(App& app, const std::string& text);
// Turned off, the helper exits once Settings has closed and nothing it does
// is under way, and turned back on it starts again.
void helper_off_tests() {
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
    open_editor(app);
    wait_until(app, [&] { return editor_text(app) == "Hello there."; });
    require(app.dictation_helper_running(), "dictating starts the helper");
    const auto turn = [&](const char* to) {
        open_settings(app);
        click(app, "dictation");
        click(app, std::string("choose:dictation:") + to);
        click(app, "apply");
        settle(app);
    };
    turn("off");
    require(!app.dictation_open() && !app.dictation_helper_running(),
            "turning dictation off stops the helper");
    // Test, before Apply, starts it; it stays while Settings is open.
    open_settings(app);
    click(app, "dictation");
    click(app, "choose:dictation:server");
    wait_until(app, [&] { return control_enabled(app, "dictation-test"); });
    click(app, "dictation-test");
    wait_until(app, [&] { return !field_label(app, "Testing…"); });
    settle(app);
    require(app.dictation_helper_running(), "a Test keeps it while Settings is open");
    app.back();
    settle(app);
    require(!app.dictation_helper_running(), "and leaving with dictation off stops it");
    turn("server");
    press(app, "Dictation");
    require(app.dictation_open() && app.listening() && app.dictation_helper_running(),
            "turned back on, the mic starts it again");
    const auto before = editor_text(app);
    press(app, "Dictation");
    wait_until(app, [&] { return editor_text(app).size() > before.size(); });
    require(editor_text(app).size() > before.size(), "and it recognizes again");
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
    open_settings(app);
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

    open_settings(app);
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

    open_settings(app);
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
    open_settings(app);
    require(!warns(app) && !field_label(app, "Format"),
            "English has no warning, and the model format is a development option");
    click(app, "settings"); // Back
    for (const auto* language : {"ja-romaji", "ar", "hi-in", "nb-no", "vi-vn", "es-es"}) {
        write_config(directory.path, R"(,"dictation":"device")", language,
                     std::string(language) == "ja-romaji" ? "en-us-full" : "international-full");
        App other(options, sink);
        open_settings(other);
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
        open_settings(app);
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
    open_settings(app);
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
    open_settings(app);
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
    open_settings(app);
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

// A play session runs SteamOS's voice filters and the helper records around
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
    {
        options.dictation_arguments = {"--scenario", "gameover"};
        Sink ended_sink;
        App ended(options, ended_sink);
        press(ended, "Dictation");
        for (int i = 0; i < 60; ++i) {
            ended.tick(monotonic_seconds());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(ended.listening() && ended.view().status.empty(),
                "and when the play session ends while listening");
        press(ended, "Dictation");
        settle_pause(ended);
        options.dictation_arguments = {"--scenario", "game"};
    }
    struct Case {
        const char* language;
        const char* layout;
        const char* text;
        // The flick layout and Japanese dictation keep the keys' scale, whose
        // case leaves the status enough room; render_tests' status_line covers
        // one too long for its gap.
        bool crowded;
    };
    for (const auto& [language, layout, text, crowded] :
         std::array<Case, 2>{{{"ja-flick", "ja-flick-12", "ゲームの音", false},
                              {"ja-romaji", "ja-jis-full", "ゲームの音", false}}}) {
        write_config(directory.path,
                     R"(,"hide_numpad":true,"dictation":"server","dictation_server":"127.0.0.1",)"
                     R"("haptic_sounds":"both","swipe_typing":true,"replace_native_keyboard":true,)"
                     R"("favorites":[{"id":"a","name":"a","layout":"en-us-full",)"
                     R"("language":"en-us"}])",
                     language, layout);
        Sink local_sink;
        App local(options, local_sink);
        press(local, "Dictation");
        wait_until(local, [&] { return !local.view().status.empty(); });
        require(local.view().status.find(text) != std::string::npos,
                "Japanese and Korean users read it in their language");
        // Too long for its gap on this toolbar: still until the laser points at it.
        const auto line = dictation_status_bounds(local.view());
        auto strip = [&](double now) {
            local.paint(now);
            const auto pixels = local.renderer.rgba();
            std::vector<unsigned char> result;
            for (int y = static_cast<int>(line.y); y < static_cast<int>(line.y + line.height); ++y) {
                const auto start =
                    static_cast<std::size_t>((y + popup_margin) * panel_width + line.x) * 4;
                result.insert(result.end(), pixels.begin() + static_cast<std::ptrdiff_t>(start),
                              pixels.begin() + static_cast<std::ptrdiff_t>(
                                                   start + static_cast<std::size_t>(line.width) * 4));
            }
            return result;
        };
        const double base = clock + 100;
        const auto at_rest = strip(base);
        require(strip(base + 3) == at_rest, "the status stays still by itself");
        const double cx = line.x + line.width / 2, cy = line.y + line.height / 2;
        local.move(0, cx, cy);
        require(local.view().status_hovered, "the laser on it counts");
        strip(base + 3.1);
        // A status that fits has nothing to scroll.
        require((strip(base + 5.5) != at_rest) == crowded, "a long one scrolls under the laser");
        const auto [kx, ky] = key_center(local, "Dictation");
        local.move(0, kx, ky);
        require(!local.view().status_hovered, "the laser left it");
        require((strip(base + 5.6) != at_rest) == crowded, "the pass under way goes on");
        strip(base + 60);
        require(strip(base + 61) == at_rest && strip(base + 70) == at_rest,
                "then it returns to its start and stays");
        if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
            local.paint(clock);
            local.renderer.write_png(fs::path(render) / (std::string("notice-") + language + ".png"));
        }
    }
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
    open_settings(app);
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

    // EOF can precede process exit: Test reports it, but cannot retry until
    // normal ticks finish cleanup. The first fixture waits for our release.
    use_fake_helper(options, directory.path, "testexit");
    App lost(options, sink);
    open_settings(lost);
    click(lost, "dictation-test");
    wait_until(lost, [&] { return field_label(lost, "Helper stopped"); });
    require(!control_enabled(lost, "dictation-test"), "Test waits for helper cleanup after EOF");
    require(fs::file_size(directory.path / "test-exits") == 1, "the first helper received Test");
    int exited_pid{};
    std::ifstream(directory.path / "test-exit-pid") >> exited_pid;
    require(exited_pid > 0 && fs::exists(fs::path("/proc") / std::to_string(exited_pid)),
            "the EOF fixture is still cleaning up");
    click(lost, "dictation-test");
    require(fs::file_size(directory.path / "test-exits") == 1 &&
                !control_enabled(lost, "dictation-test"),
            "a disabled Test does not attempt a restart");
    require(lost.host_dictation_request("dictation.test_server", nullptr).first == "busy" &&
                fs::file_size(directory.path / "test-exits") == 1,
            "a shell's Test meanwhile is answered busy, not accepted and left unanswered");
    std::ofstream(directory.path / "test-exit-release") << "1";
    wait_until(lost, [&] { return control_enabled(lost, "dictation-test"); });
    require(!fs::exists(fs::path("/proc") / std::to_string(exited_pid)),
            "Test becomes available after the old helper is reaped");
    click(lost, "dictation-test");
    // The retry can exit before click's tick, especially on the Frame. Check
    // that it received Test rather than requiring a transient pending label.
    wait_until(lost, [&] {
        return fs::file_size(directory.path / "test-exits") == 2 &&
               field_label(lost, "Helper stopped") && control_enabled(lost, "dictation-test");
    });

    // A helper that never finishes its cleanup is killed after a grace period,
    // so dictation and Test are not blocked until the keyboard restarts.
    fs::remove(directory.path / "test-exits");
    fs::remove(directory.path / "test-exit-release");
    App stuck(options, sink);
    open_settings(stuck);
    click(stuck, "dictation-test");
    wait_until(stuck, [&] { return field_label(stuck, "Helper stopped"); });
    int stuck_pid{};
    std::ifstream(directory.path / "test-exit-pid") >> stuck_pid;
    require(stuck_pid > 0 && fs::exists(fs::path("/proc") / std::to_string(stuck_pid)) &&
                !control_enabled(stuck, "dictation-test"),
            "the helper ignores its stop and stays");
    stuck.tick(clock += .1);
    require(fs::exists(fs::path("/proc") / std::to_string(stuck_pid)), "it is given time first");
    stuck.tick(clock += Dictation::kill_after);
    wait_until(stuck, [&] { return control_enabled(stuck, "dictation-test"); });
    require(!fs::exists(fs::path("/proc") / std::to_string(stuck_pid)),
            "after the grace period it is killed and reaped, and Test returns");
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
    open_settings(app);
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
    // The shell maps a terminal field's Copy itself: the sink keeps Ctrl+C's key IDs.
    App::HostHints shell_terminal;
    shell_terminal.purpose = "terminal";
    app.set_context(std::string("t"), shell_terminal);
    press(app, "Copy");
    events = router.take_events();
    require(events.size() == 1 && events[0].key == "KeyC" && events[0].modifiers.size() == 1,
            "Copy reaches the sink as Ctrl+KeyC, also for a terminal field");
    router.acknowledge("t", events[0].seq);
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
    open_settings(app);
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
    auto surface = json_ptr(json_tokener_parse(R"({"background_surface":"clear"})"));
    require(app.host_settings_set(surface.get(), 6) ==
                std::pair<std::string, std::string>{"invalid_setting", "background_surface"},
            "a background surface outside visible, semi-transparent and invisible is refused");
    auto uppercase = json_ptr(json_tokener_parse(R"({"background_surface":"DISABLED"})"));
    require(app.host_settings_set(uppercase.get(), 6) ==
                std::pair<std::string, std::string>{"invalid_setting", "background_surface"},
            "background surface values are case sensitive");
    auto sounds = json_ptr(json_tokener_parse(R"({"haptic_sounds":"loud"})"));
    require(app.host_settings_set(sounds.get(), 6) ==
                std::pair<std::string, std::string>{"invalid_setting", "haptic_sounds"},
            "a sound choice outside off, press, release and both is refused");
    auto good = json_ptr(json_tokener_parse(
        R"({"auto_leveling":false,"dictation_send_gap":40,"background_surface":"invisible",)"
        R"("haptic_sounds":"press"})"));
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
    require(!saved.auto_leveling && saved.dictation_send_gap == 40 &&
                saved.background_surface == "invisible" && saved.haptic_sounds == "press" &&
                saved.replace_native_keyboard,
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
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                                 R"("dictation_direct":true,"haptic_sounds":"both")");
    Sink sink;
    App app(options, sink);
    require(app.pops(KeyFeedback::Press), "keys pop before listening");
    press(app, "Dictation");
    require(app.listening() && !app.dictation_open(), "Type as I speak listens on the keyboard");
    require(!app.pops(KeyFeedback::Press) && !app.pops(KeyFeedback::Release),
            "the microphone never hears a pop");
    const auto sounds_button = [](const PanelView& view) {
        return *std::find_if(view.controls.begin(), view.controls.end(),
                             [](const Control& item) { return item.id == "sounds-toggle"; });
    };
    const auto toolbar = sounds_button(app.view());
    require(!toolbar.enabled && !toolbar.warning && toolbar.icon == Icon::HapticSoundOff,
            "the toolbar button waits, crossed but not orange");
    require(!app.haptic_sounds(), "and the sound stream closes, as nothing could play");
    open_settings(app);
    const auto under_settings = app.view();
    const auto effect = std::find_if(under_settings.controls.begin(), under_settings.controls.end(),
                                     [](const Control& item) { return item.id == "haptic-effect:b"; });
    require(effect != under_settings.controls.end(), "listening continues under Settings");
    const double x = effect->bounds.x + effect->bounds.width / 2,
                 y = effect->bounds.y + effect->bounds.height / 2;
    app.down(0, x, y, 1);
    app.up(0, x, y);
    require(app.listening() && !app.take_sound_preview() && !app.haptic_sounds(), "nor a preview");
    app.back();
    app.stop_listening(reason);
    require(!app.listening() && app.view().status == reason, "Type as I speak stops and says why");
    require(app.pops(KeyFeedback::Release) && app.haptic_sounds(), "keys pop again once it stops");
    const auto after = sounds_button(app.view());
    require(after.enabled && !after.warning && after.icon == Icon::HapticSound,
            "and the button returns to normal");
    require(sink.text.empty() && sink.keys.empty(), "stopping types nothing");
}

// The toolbar's second row of a narrow keyboard stays clear of the editor.
void compact_toolbar_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    use_fake_helper(options, directory.path, "silent");
    const std::string extra = R"(,"hide_numpad":true,"dictation":"server",)"
                              R"("dictation_server":"127.0.0.1","haptic_sounds":"both",)"
                              R"("swipe_typing":true,"replace_native_keyboard":true,)"
                              R"("favorites":[{"id":"a","name":"a","layout":"international-full",)"
                              R"("language":"de-de"}])";
    struct Case {
        const char* name;
        const char* language;
        const char* layout;
    };
    for (const auto& [name, language, layout] :
         std::array<Case, 3>{{{"english", "en-us", "en-us-full"},
                              {"romaji", "ja-romaji", "ja-jis-full"},
                              {"flick", "ja-flick", "ja-flick-12"}}}) {
        write_config(directory.path, extra, language, layout);
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        settle(app);
        require(app.dictation_open(), "the narrow keyboard opens the editor");
        if (const auto* render = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
            app.paint(clock);
            app.renderer.write_png(fs::path(render) / (std::string("compact-") + name + ".png"));
        }
        const auto view = app.view();
        const auto editor = view.dictation->bounds;
        require(editor.y >= 64, "the editor keeps a gap under the first toolbar row");
        double cycle = -1, others = 0;
        for (const auto& control : view.controls) {
            if (control.id == "language-cycle") {
                cycle = control.bounds.x;
            } else if (control.id == "settings" || control.id == "recenter" || control.id == "pin" ||
                       control.id == "passthrough" || control.id == "keep-open" ||
                       control.id == "swipe-toggle" || control.id == "sounds-toggle") {
                others = std::max(others, control.bounds.x);
            }
        }
        require(cycle > others, "the language button stays last");
        for (const auto& control : view.controls) {
            if (control.style != ControlStyle::MoveHandle && control.bounds.y < 110) {
                const bool clear = control.bounds.y + control.bounds.height <= editor.y ||
                                   control.bounds.x + control.bounds.width <= editor.x ||
                                   control.bounds.x >= editor.x + editor.width;
                require(clear, "no toolbar control sits on the editor");
            }
        }
    }
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
    open_settings(app);
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
    open_settings(app);
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
        require(sink.keys == expected, "inner breaks are Shift+Enter; Send presses no plain Enter");
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
        require(sink.text == std::vector<std::string>{"a", " ", "b"} && sink.keys.empty(),
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
        // The laser over a long text finds room to scroll it, asked every
        // frame; once the text is short again it finds none. The measured
        // height follows the text even though it is kept between frames.
        Sink sink;
        App app(options, sink);
        open_editor(app);
        for (int i = 0; i < 12; ++i) {
            type_text(app, "line\n");
        }
        const auto bounds = app.view().dictation->bounds;
        app.move(0, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
        const auto room = app.scroll_room(0);
        require(room && room->back && !room->forward && app.scroll_room(0) == room,
                "a text taller than the editor, scrolled to its cursor, scrolls back");
        for (int i = 0; i < 60; ++i) {
            press(app, "Backspace");
        }
        app.move(0, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
        require(editor_text(app).empty() && !app.scroll_room(0),
                "an emptied editor has nothing to scroll");
    }
    {
        // Between pieces the target can change, or the panel hide and return.
        // The rest was meant for the old target, so Send stops with it kept.
        for (const bool hide : {false, true}) {
            Sink sink;
            App app(options, sink);
            app.set_native_request(5);
            open_editor(app);
            type_text(app, std::string(40, 'a'));
            press(app, "Dictation-send");
            require(sink.text.size() == 1 && app.view().dictation->sending,
                    "the first piece is sent and the second waits for the next tick");
            if (hide) {
                app.set_interaction_active(false);
                app.tick(clock += .02);
                app.set_interaction_active(true);
            } else {
                app.set_native_request(6);
            }
            settle(app);
            require(sink.text.size() == 1 && app.dictation_open() && !app.view().dictation->sending &&
                        editor_text(app) == std::string(8, 'a'),
                    "nothing more reaches the new target; the rest stays in the editor");
            require(app.view().status.starts_with("Sending stopped.") && app.view().status_card,
                    "its message shows on the card above the keyboard, as every Send stop's does");
        }
    }
    {
        // Another window takes focus mid-Send (R19, R44), between pieces or
        // while one goes out: nothing more reaches it, and the rest stays in
        // the editor, its start marked when that may have arrived.
        for (const bool in_flight : {false, true}) {
            Sink sink;
            sink.window = 7;
            sink.busy = in_flight; // still typing the first piece
            sink.acknowledged = in_flight ? 5 : 0;
            App app(options, sink);
            open_editor(app);
            type_text(app, std::string(40, 'a'));
            press(app, "Dictation-send");
            require(sink.text.size() == (in_flight ? 0u : 1u) && app.view().dictation->sending,
                    "the first piece is out or going out");
            sink.window = 9;
            settle(app);
            sink.busy = false;
            settle(app);
            require(sink.text.size() == (in_flight ? 0u : 1u) && app.dictation_open() &&
                        !app.view().dictation->sending &&
                        editor_text(app) == std::string(in_flight ? 35 : 8, 'a') &&
                        app.view().status.starts_with("Another window took focus.") &&
                        app.view().status_card,
                    in_flight ? "a click mid-piece stops it; the rest stays, its start marked"
                              : "a click between pieces stops Send; the rest stays in the editor");
        }
    }
    {
        // The piece going out finishes in the frame another window takes
        // focus: it arrived, so it leaves the editor instead of being sent again.
        Sink sink;
        sink.window = 7;
        sink.busy = true;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'a'));
        press(app, "Dictation-send");
        sink.busy = false;
        sink.window = 9;
        settle(app);
        require(sink.text == std::vector<std::string>{std::string(32, 'a')} &&
                    editor_text(app) == std::string(8, 'a') && app.view().dictation->uncertain == 0 &&
                    app.view().status.starts_with("Another window took focus."),
                "only the piece not yet sent stays, unmarked");
    }
    {
        // Another window takes focus as Send types a piece one character at
        // a time. Between two of them, after an acknowledgement and before
        // the next goes out, none can have arrived, so none is marked; a
        // character sent and not yet acknowledged is.
        for (const bool between : {true, false}) {
            Sink sink;
            sink.window = 7;
            sink.busy = true;
            sink.acknowledged = 5;
            sink.between_characters = between;
            App app(options, sink);
            open_editor(app);
            type_text(app, std::string(40, 'a'));
            press(app, "Dictation-send");
            sink.window = 9;
            std::ostringstream log;
            {
                CaptureLog capture(log);
                settle(app);
            }
            const auto view = app.view();
            require(editor_text(app) == std::string(35, 'a') &&
                        view.dictation->uncertain == (between ? 0u : 1u) &&
                        view.status.ends_with(between ? "Send continues."
                                                      : "the marked start may already have arrived."),
                    between ? "a stop between characters marks nothing"
                            : "a stop with a character in flight marks it");
            // The log counts the piece's acknowledged characters, as the editor erased them.
            require(log.str().find(between ? "took focus, 5 of 40 characters sent.\n"
                                           : "took focus, 5 of 40 characters sent, 1 uncertain.\n") !=
                        std::string::npos,
                    "the log counts what the editor erased and marked");
        }
    }
    {
        // The same with a piece already sent: the log adds the one in flight.
        Sink sink;
        sink.window = 7;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'a'));
        press(app, "Dictation-send");
        sink.busy = true;
        sink.acknowledged = 3;
        app.tick(clock += .02);
        require(sink.text.size() == 1 && sink.commit_attempts == 2 && app.view().dictation->sending,
                "the first piece is out and the second going out");
        sink.window = 9;
        std::ostringstream log;
        {
            CaptureLog capture(log);
            settle(app);
        }
        require(editor_text(app) == std::string(5, 'a') &&
                    log.str().find("took focus, 35 of 40 characters sent, 1 uncertain.\n") !=
                        std::string::npos,
                "the log counts the sent piece and the acknowledged start of the next");
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
}

void suspend_input_tests() {
    auto options_for = [](const fs::path& directory, const std::string& extra = "",
                          const char* scenario = "silent") {
        Options options;
        options.config_dir = directory;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        write_config(directory, R"(,"dictation":"server","dictation_server":"127.0.0.1")" + extra);
        use_fake_helper(options, directory, scenario);
        return options;
    };
    auto resume_quietly = [](App& app, Sink& sink) {
        const auto text = sink.text;
        const auto keys = sink.keys;
        app.set_interaction_active(false);
        app.tick(clock += .1);
        app.set_interaction_active(true);
        settle(app);
        require(sink.text == text && sink.keys == keys,
                "resuming interaction cannot restart a suspended output producer");
    };
    {
        // Exercise real output ownership, including modifier release order.
        TemporaryDirectory directory;
        auto options = options_for(directory.path);
        Sink sink;
        App app(options, sink);
        const auto [sx, sy] = key_center(app, "ShiftLeft");
        const auto [lx, ly] = key_center(app, "ArrowLeft");
        app.down(0, sx, sy, clock);
        app.down(1, lx, ly, clock);
        require(std::find(sink.keys.begin(), sink.keys.end(), std::pair{KEY_LEFT, 1}) != sink.keys.end(),
                "the target owns the navigation key before sleep");
        app.suspend_input();
        require(sink.keys.size() >= 2 && sink.keys[sink.keys.size() - 2] == std::pair{KEY_LEFT, 0} &&
                    sink.keys.back() == std::pair{KEY_LEFTSHIFT, 0},
                "sleep releases the actual key before its modifier");
        require(!app.pointer_pressed(0) && !app.pointer_pressed(1), "sleep forgets both UI holds");
        resume_quietly(app, sink);
        const auto released = sink.keys;
        app.up(1, lx, ly);
        app.up(0, sx, sy);
        require(sink.keys == released, "late pre-sleep releases have no output to act on");
        press(app, "ArrowLeft");
        require(sink.keys.size() == released.size() + 2 && sink.keys.back() == std::pair{KEY_LEFT, 0},
                "a fresh post-sleep press still works without a stale modifier");
    }
    {
        // A synchronous first piece has finished, but the next is not queued.
        TemporaryDirectory directory;
        auto options = options_for(directory.path);
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'a'));
        press(app, "Dictation-send");
        require(sink.text == std::vector<std::string>{std::string(32, 'a')} &&
                    editor_text(app) == std::string(8, 'a') && app.view().dictation->sending,
                "Send is between pieces before sleep");
        app.suspend_input();
        require(!app.view().dictation->sending && editor_text(app) == std::string(8, 'a'),
                "sleep retains only the unsent editor remainder");
        resume_quietly(app, sink);
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.text.back() == std::string(8, 'a'),
                "only a deliberate Send continues the remainder");
    }
    {
        // The target has not answered an attempted piece. Keep it uncertain.
        TemporaryDirectory directory;
        auto options = options_for(directory.path);
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'b'));
        sink.busy = true;
        press(app, "Dictation-send");
        require(app.view().dictation->sending && sink.commit_attempts > 0 && sink.text.empty(),
                "a Send piece is pending at the target before sleep");
        app.suspend_input();
        require(!app.view().dictation->sending && editor_text(app) == std::string(40, 'b') &&
                    app.view().dictation->uncertain > 0,
                "sleep preserves an unconfirmed piece and marks its uncertainty");
        sink.busy = false;
        resume_quietly(app, sink);
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() &&
                    sink.text == std::vector<std::string>{std::string(32, 'b'), std::string(8, 'b')},
                "a deliberate retry sends the retained editor text once");
    }
    {
        // A prior failure has already established an acknowledged prefix.
        TemporaryDirectory directory;
        auto options = options_for(directory.path);
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, std::string(40, 'c'));
        sink.commits_before_failure = 1;
        sink.acknowledged = 3;
        press(app, "Dictation-send");
        settle(app);
        require(editor_text(app) == std::string(5, 'c') && app.view().dictation->uncertain == 1,
                "a failed piece retains only the unacknowledged suffix");
        app.suspend_input();
        require(editor_text(app) == std::string(5, 'c') && app.view().dictation->uncertain == 1,
                "sleep does not discard known acknowledgement progress");
        sink.commits_before_failure = -1;
        resume_quietly(app, sink);
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.text.back() == std::string(5, 'c'),
                "resending after sleep never retypes the acknowledged prefix");
    }
    {
        // Paste is waiting for clipboard preparation, before any Ctrl+V.
        TemporaryDirectory directory;
        auto options = options_for(directory.path, R"(,"dictation_send_paste":true)");
        Sink sink;
        FakeClipboard clipboard;
        clipboard.sink = &sink;
        options.clipboard = &clipboard;
        App app(options, sink);
        open_editor(app);
        type_text(app, "safe paste");
        press(app, "Dictation-send");
        require(clipboard.written == std::vector<std::string>{"safe paste"} && sink.keys.empty(),
                "Paste has prepared its text without pressing the shortcut");
        app.suspend_input();
        require(editor_text(app) == "safe paste" && !app.view().dictation->sending,
                "sleep keeps the text of a paste not yet delivered");
        resume_quietly(app, sink);
        require(clipboard.written.size() == 1, "resume cannot prepare another automatic paste");
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.keys.size() == 4,
                "only the next deliberate Send presses Ctrl+V");
    }
    {
        // A late recognition answer must update the editor, never auto-Send.
        TemporaryDirectory directory;
        auto options = options_for(directory.path, "", "sleepwait");
        Sink sink;
        App app(options, sink);
        press(app, "Dictation");
        press(app, "Dictation-send");
        wait_until(app, [&] {
            return prompts_sent(directory.path).size() == 1 && !app.listening() &&
                   app.view().dictation_recognizing &&
                   app.view().disabled_keys.contains("Dictation-send");
        });
        app.suspend_input();
        std::ofstream(directory.path / "release-sleep-phrase") << "ready";
        wait_until(app, [&] {
            return app.dictation_open() && editor_text(app) == "Hello there." &&
                   !app.view().dictation->revealing;
        });
        require(!app.view().dictation->sending && sink.text.empty(),
                "a late phrase stays in the editor after sleep cancels Send-wait");
        resume_quietly(app, sink);
        press(app, "Dictation-send");
        settle(app);
        require(!app.dictation_open() && sink.text == std::vector<std::string>{"Hello there."},
                "a fresh Send can deliver the preserved late phrase");
    }
    {
        TemporaryDirectory directory;
        auto options = options_for(directory.path, R"(,"dictation_direct":true)", "direct");
        Sink sink;
        sink.busy = true;
        App app(options, sink);
        press(app, "Dictation");
        wait_until(app, [&] { return sink.commit_attempts > 0; });
        require(app.listening() && sink.text.empty(), "direct speech is queued behind a busy target");
        app.suspend_input();
        require(!app.listening(), "sleep stops direct capture as well as its delivery queue");
        sink.busy = false;
        resume_quietly(app, sink);
        press(app, "KeyE");
        require(sink.text == std::vector<std::string>{"e"},
                "no old direct phrase returns; a fresh ordinary key works");
    }
    {
        TemporaryDirectory directory;
        auto options = options_for(directory.path);
        Sink sink;
        App app(options, sink);
        open_editor(app);
        type_text(app, "draft");
        const auto [x, y] = key_center(app, "Backspace");
        app.down(1, x, y, clock);
        const auto draft = editor_text(app);
        app.suspend_input();
        resume_quietly(app, sink);
        require(editor_text(app) == draft, "sleep stops editor repeat without erasing its draft");
        press(app, "Dictation");
        require(app.listening(), "the editor can start a new capture deliberately");
        app.suspend_input();
        require(!app.listening() && editor_text(app) == draft,
                "sleep stops listening while preserving existing editor text");
        require(app.view().dictation->notice == "Listening stopped: the headset was asleep.",
                "a system sleep says so");
        press(app, "Dictation");
        require(app.listening(), "listening starts again after the sleep");
        app.suspend_input(false);
        require(!app.listening() &&
                    app.view().dictation->notice == "Listening stopped: the headset was taken off.",
                "taking the headset off says that, not that it slept");
    }
}

// Reload replaces edited layout and theme files that keep their IDs. The open
// editor's keys must follow them, not keep copies of the old layout.
void reload_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1")");
    use_fake_helper(options, directory.path, "silent");
    const auto bundled =
        std::find_if(std::begin(bundled::profiles), std::end(bundled::profiles),
                     [](const bundled::Resource& resource) {
                         return resource.kind == "layouts" &&
                                resource.json.find(R"("id": "en-us-full")") != std::string_view::npos;
                     });
    require(bundled != std::end(bundled::profiles), "the bundled layout to edit");
    std::string json(bundled->json);
    const std::string escape = R"("id": "Escape", "label": "Esc")";
    require(json.find(escape) != std::string::npos, "the layout names its Escape key Esc");
    fs::create_directories(directory.path / "layouts");
    std::ofstream(directory.path / "layouts/en-us-full.json") << json;
    Sink sink;
    App app(options, sink);
    open_editor(app);
    const auto label = [&] { return find(app.view(), "Escape")->label; };
    require(app.dictation_open() && label() == "Esc", "the editor's Escape key comes from the file");
    json.replace(json.find(escape), escape.size(), R"("id": "Escape", "label": "Quit")");
    std::ofstream(directory.path / "layouts/en-us-full.json") << json;
    app.reload();
    app.tick(clock += .02);
    require(app.dictation_open() && label() == "Quit",
            "after Reload the open editor's keys follow the edited layout");
    // Reload also takes the config-only dictation URL edited by hand: the
    // editor's place icon turns from the user's computer to a hosted service.
    require(app.view().dictation->where == Icon::Computer, "a self-hosted server first");
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                 R"("dictation_url":"https://speech.example/v1","dictation_model":"whisper-1")");
    app.reload();
    app.tick(clock += .02);
    require(app.view().dictation->where == Icon::Cloud, "after Reload the edited URL is used");
}

// Reload can change where phrases go and with which model, but only a new
// capture configures the helper, so Reload pauses listening, as documented;
// listening again uses the reloaded settings.
void reload_pauses_capture() {
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
    press(app, "Dictation");
    wait_until(app, [&] { return !configures(directory.path).empty(); });
    app.flush_settings();
    require(app.view().dictation && app.view().dictation->capturing, "listening before Reload");
    write_config(directory.path,
                 R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                 R"("dictation_url":"https://speech.example/v1","dictation_model":"whisper-1")");
    app.reload();
    app.tick(clock += .02);
    require(!app.view().dictation->capturing, "Reload pauses listening");
    press(app, "Dictation");
    wait_until(app,
               [&] { return configures(directory.path).find("speech.example") != std::string::npos; });
    require(configures(directory.path).find("speech.example") != std::string::npos,
            "listening again sends the reloaded URL to the helper");
}

// A hosted URL from config.json is where phrases go, so the card's Test
// checks it rather than the Server field.
void url_test_tests() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    write_config(directory.path, R"(,"dictation":"server","dictation_server":"127.0.0.1",)"
                                 R"("dictation_url":"https://speech.example/v1")");
    use_fake_helper(options, directory.path, "silent");
    Sink sink;
    App app(options, sink);
    open_settings(app);
    click(app, "dictation-test");
    wait_until(app, [&] { return field_label(app, "Reachable"); });
    std::ifstream log(directory.path / "tests");
    std::string line, last;
    while (std::getline(log, line)) {
        last = line;
    }
    require(last.find(R"("url": "https://speech.example/v1")") != std::string::npos &&
                last.find(R"("server")") == std::string::npos,
            "Test checks the configured URL, not the Server field");
}

void selection_tests() {
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
    type_text(app, "one two");
    auto disabled = [&](const char* id) { return app.view().disabled_keys.contains(id); };
    require(disabled("Dictation-copy") && disabled("Dictation-cut") && disabled("Dictation-paste"),
            "the keys wait for a selection and a copy");

    for (int i = 0; i < 3; ++i) {
        press(app, "ShiftLeft");
        press(app, "ArrowLeft");
    }
    auto selected = [&] {
        const auto view = app.view();
        return std::pair{view.dictation->selection_begin, view.dictation->selection_end};
    };
    require(selected() == std::pair<std::size_t, std::size_t>{4, 7},
            "Shift and the arrows select, a step for each latched Shift");
    require(!disabled("Dictation-copy") && !disabled("Dictation-cut") && disabled("Dictation-paste"),
            "a selection enables Copy and Cut");
    press(app, "Dictation-cut");
    require(editor_text(app) == "one " && !disabled("Dictation-paste") && disabled("Dictation-cut"),
            "Cut removes the selection and enables Paste");
    press(app, "Dictation-paste");
    require(editor_text(app) == "one two", "Paste puts it back at the cursor");

    press(app, "ShiftLeft");
    press(app, "ArrowLeft");
    press(app, "ArrowLeft");
    press(app, "KeyZ");
    require(editor_text(app) == "one twzo", "Left without Shift ends a selection at its start");
    {
        Sink lines;
        App other(options, lines);
        open_editor(other);
        type_text(other, "one\ntwo");
        press(other, "ShiftLeft");
        press(other, "ArrowUp");
        press(other, "ArrowLeft");
        press(other, "KeyZ");
        require(editor_text(other) == "onez\ntwo", "across lines too, whatever the columns");
        press(other, "ShiftLeft");
        press(other, "ArrowDown");
        press(other, "ArrowRight");
        press(other, "KeyX");
        require(editor_text(other).ends_with("x"), "and Right at its end");
    }

    press(app, "ShiftLeft");
    const auto area = editor_text_area(app.view());
    app.down(1, area.x + 1, area.y + 4, clock);
    app.up(1, area.x + 1, area.y + 4);
    require(selected() == std::pair<std::size_t, std::size_t>{0, 7},
            "Shift and a click select from the cursor to there");
    press(app, "KeyX");
    require(editor_text(app) == "xo", "the click used up the Shift, and typing replaces the selection");

    press(app, "ShiftLeft");
    press(app, "ArrowLeft");
    press(app, "Dictation-send");
    settle(app);
    require(sink.text == std::vector<std::string>{"xo"} && sink.keys.empty(),
            "Send sends the whole text, whatever is selected");
}
} // namespace

int main() {
    try {
        settings_tests();
        layout_tests();
        mode_tests();
        key_flick_tests();
        held_kana_mic_tests();
        modal_repeat_tests();
        send_tests();
        suspend_input_tests();
        reload_tests();
        reload_pauses_capture();
        url_test_tests();
        selection_tests();
        scroll_tests();
        paste_tests();
        passthrough_tests();
        passthrough_message_tests();
        leaving_tests();
        field_tests();
        settings_and_update_tests();
        compact_toolbar_tests();
        card_tests();
        live_tests();
        language_tests();
        direction_tests();
        direct_tests();
        direct_paste_tests();
        terminal_edit_key_tests();
        terminal_paste_tests();
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
        helper_off_tests();
        std::cout << "Dictation mode tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
