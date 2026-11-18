// The desktop simulator's text field, its slow confirmations, and the real
// keyboard typing into it through the simulator's pointer routing.
#include "simulator/simulator.hpp"
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using namespace framekeyboard;
using namespace framekeyboard::simulator;
namespace {
void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void tap(FieldSink& sink, const char* key) {
    require(sink.deliver_key(key_code(key), 1) == Delivery::Success, std::string("press ") + key);
    sink.deliver_key(key_code(key), 0);
}
void chord(FieldSink& sink, const char* modifier, const char* key) {
    sink.deliver_key(key_code(modifier), 1);
    tap(sink, key);
    sink.deliver_key(key_code(modifier), 0);
}

void editing() {
    TextField field;
    FieldSink sink(field);
    require(sink.commit_text("hello") && field.text() == "hello" && field.caret() == 5,
            "text lands at the caret");
    tap(sink, "Backspace");
    tap(sink, "ArrowLeft");
    tap(sink, "ArrowLeft");
    require(sink.commit_text("X") && field.text() == "heXll", "arrows move the caret");
    tap(sink, "Home");
    tap(sink, "Delete");
    require(field.text() == "eXll" && field.caret() == 0, "Home and Delete");
    tap(sink, "End");
    tap(sink, "Enter");
    tap(sink, "Tab");
    require(field.text() == "eXll\n\t", "Enter breaks the line and Tab types a tab");

    field.clear();
    require(sink.commit_text("👍🏽a"), "emoji with a skin tone");
    tap(sink, "ArrowLeft");
    tap(sink, "Backspace");
    require(field.text() == "a" && field.caret() == 0, "Backspace removes the whole emoji");

    field.clear();
    sink.commit_text("abcd\nef\nghijk");
    tap(sink, "ArrowUp");
    require(field.caret() == std::string("abcd\nef").size(), "Up keeps the column, or the line's end");
    tap(sink, "ArrowUp");
    require(field.caret() == 4, "Up again keeps the column it started from");
    tap(sink, "ArrowDown");
    tap(sink, "ArrowDown");
    require(field.caret() == field.text().size(), "Down returns to it");
    chord(sink, "ControlLeft", "Home");
    require(field.caret() == 0, "Ctrl+Home goes to the start");
    chord(sink, "ControlLeft", "End");
    require(field.caret() == field.text().size(), "Ctrl+End goes to the end");

    field.clear();
    sink.commit_text("one two");
    chord(sink, "ShiftLeft", "ArrowLeft");
    chord(sink, "ShiftLeft", "ArrowLeft");
    require(field.selection() == std::pair<std::size_t, std::size_t>{5, 7}, "Shift+arrows select");
    tap(sink, "Backspace");
    require(field.text() == "one t", "Backspace deletes the selection");
    chord(sink, "ControlLeft", "ArrowLeft");
    require(field.caret() == 4, "Ctrl+Left jumps to the word's start");
    chord(sink, "ControlLeft", "ArrowRight");
    require(field.caret() == 5, "Ctrl+Right jumps to the word's end");

    chord(sink, "ControlLeft", "KeyA");
    chord(sink, "ControlLeft", "KeyC");
    require(field.clipboard() == "one t", "Ctrl+C copies the selection to the field's clipboard");
    tap(sink, "ArrowRight");
    require(!field.selection() && field.caret() == 5, "an arrow ends the selection at its side");
    chord(sink, "ControlLeft", "KeyV");
    require(field.text() == "one tone t" && sink.served() == 1,
            "Ctrl+V pastes from the field's clipboard");
    chord(sink, "ControlLeft", "KeyA");
    chord(sink, "ControlLeft", "KeyX");
    require(field.text().empty() && field.clipboard() == "one tone t", "Ctrl+X cuts");
    sink.write("dictated");
    chord(sink, "ControlLeft", "KeyV");
    require(field.text() == "dictated", "dictation's paste Send writes the field's own clipboard");

    require(sink.replace_text(3, "ed!") && field.text() == "dictaed!",
            "replace deletes before the caret");
    require(!sink.commit_text("") && !sink.text_pending(), "empty text is refused");
    require(!sink.commit_text(std::string(33, 'a')),
            "more than 32 characters is refused, as by Gamescope");
    require(!sink.replace_text(65, "a"), "more than 64 deletions is refused");
    require(!sink.commit_text("\xff"), "invalid UTF-8 is refused");
    chord(sink, "AltLeft", "Backspace");
    require(field.text() == "dictaed!", "the field has no Alt shortcuts");
    field.place_caret(2);
    require(field.caret() == 2 && !field.selection(), "a click places the caret");
}

void slow_field() {
    TextField field;
    FieldSink sink(field);
    sink.set_slow(true);
    sink.set_time(10);
    require(!sink.commit_text("あ") && sink.text_pending() && field.text().empty(), "text waits");
    sink.set_time(10.02);
    require(!sink.commit_text("あ") && sink.text_pending(), "still waiting before 30 ms");
    sink.set_time(10.031);
    require(sink.commit_text("あ") && !sink.text_pending() && field.text() == "あ",
            "confirmed after 30 ms");

    const int backspace = key_code("Backspace");
    require(sink.deliver_key(backspace, 1) == Delivery::Waiting && field.text() == "あ",
            "a press waits");
    require(sink.deliver_key(backspace, 1) == Delivery::Waiting, "and waits when asked again");
    sink.set_time(10.07);
    require(sink.deliver_key(backspace, 1) == Delivery::Success && field.text().empty(), "then lands");
    require(sink.deliver_key(backspace, 0) == Delivery::Success, "a release lands at once");

    require(!sink.commit_text("x") && sink.waiting(), "another text waits");
    sink.cancel_delivery();
    require(!sink.waiting(), "a cancel drops it");
    sink.set_time(11);
    require(!sink.commit_text("x") && field.text().empty(),
            "a dropped text never lands; asking again waits anew");
    sink.cancel_delivery();

    const int shift = key_code("ShiftLeft");
    require(sink.deliver_key(shift, 1) == Delivery::Waiting, "Shift waits");
    sink.deliver_key(shift, 0);
    require(!sink.waiting(), "its release ends the unconfirmed press");
    sink.set_slow(false);
    sink.commit_text("ab");
    tap(sink, "ArrowLeft");
    require(!field.selection(), "so Shift was never held");

    sink.set_slow(true);
    require(!sink.commit_text("q"), "waiting");
    sink.set_slow(false);
    require(sink.commit_text("q") && field.text() == "aqb", "turning slow off confirms at once");
}

// Splits nothing: the reading is its own only candidate, so no Anthy is needed.
struct Reading : JapaneseConverter {
    std::vector<Segment> convert(const std::string& reading) override { return {{reading, {reading}}}; }
    std::vector<Segment> resize(int, int) override { return {}; }
};
struct Profile {
    fs::path path;
    Profile() {
        char directory[] = "/tmp/framekeyboard-simulator-XXXXXX";
        require(mkdtemp(directory) != nullptr, "temporary simulator profile");
        path = directory;
    }
    ~Profile() { fs::remove_all(path); }
};
void injected_converter() {
    Profile profile;
    Setup options;
    options.config_dir = profile.path;
    options.language = "ja-flick";
    options.japanese_converter = [] { return std::make_unique<Reading>(); };
    Simulator simulator(options);
    require(simulator.app().options().mozc_server.empty(),
            "an injected converter does not start the bundled Mozc server");
    require(!fs::exists(profile.path / "mozc"), "an injected converter creates no Mozc profile");
    simulator.shutdown();
}
void candidate_profile() {
    Profile profile;
    const auto config = profile.path / "settings";
    const auto temporary = profile.path / "temporary";
    fs::create_directories(temporary);
    const auto binary = fs::read_symlink("/proc/self/exe").parent_path() / "framekeyboard-simulator";
    auto run = [&](const char* engine, int expected) {
        const auto child = fork();
        require(child >= 0, "fork candidate report");
        if (child == 0) {
            const int null = open("/dev/null", O_RDWR);
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            close(null);
            setenv("TMPDIR", temporary.c_str(), 1);
            execl(binary.c_str(), binary.c_str(), "--japanese-candidates", "--config-dir",
                  config.c_str(), engine,
                  engine == std::string("--mozc-server") ? "/bin/false" : nullptr,
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                    WEXITSTATUS(status) == expected,
                "candidate report exits as expected");
        require(!fs::exists(config / "mozc"),
                "candidate report never uses the interactive Mozc profile");
        require(fs::is_empty(temporary), "candidate profile is removed on return and exception");
    };
    run("--mozc-server", 1); // A failed server makes the report throw after creating its profile.
    run("--anthy-only", 0);
}
// The Chinese report runs from the command line and leaves no settings
// folder: it learns nothing.
void chinese_candidate_report() {
    Profile profile;
    const auto config = profile.path / "settings";
    const auto binary = fs::read_symlink("/proc/self/exe").parent_path() / "framekeyboard-simulator";
    int input[2];
    require(pipe(input) == 0, "pipe for the readings");
    const auto child = fork();
    require(child >= 0, "fork Chinese report");
    if (child == 0) {
        dup2(input[0], STDIN_FILENO);
        close(input[0]);
        close(input[1]);
        const int null = open("/dev/null", O_RDWR);
        dup2(null, STDOUT_FILENO);
        close(null);
        execl(binary.c_str(), binary.c_str(), "--chinese-candidates", "--chinese-engine", "all",
              "--config-dir", config.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(input[0]);
    const std::string readings = "nihao\n# a note\nㄋㄧˇ ㄏㄠˇ\n";
    require(write(input[1], readings.data(), readings.size()) == static_cast<ssize_t>(readings.size()),
            "write the readings");
    close(input[1]);
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the Chinese report runs");
    require(!fs::exists(config), "the Chinese report writes no settings");
}
double clock_now = 10;
void wait(Simulator& simulator, double seconds = .1) {
    for (const double end = clock_now + seconds; clock_now < end; clock_now += .01) {
        simulator.tick(clock_now);
    }
}
// A key's centre in canvas coordinates.
std::pair<double, double> key_point(Simulator& simulator, const std::string& id) {
    auto& app = simulator.app();
    const auto view = app.view();
    double left = 1e9, right = -1, top = 1e9, bottom = -1;
    for (int y = 0; y < panel_height; y += 2) {
        for (int x = 0; x < panel_width; x += 2) {
            if (const auto* key = app.renderer.hit_key(view, x, y); key && key->id == id) {
                left = std::min<double>(left, x);
                right = std::max<double>(right, x);
                top = std::min<double>(top, y);
                bottom = std::max<double>(bottom, y);
            }
        }
    }
    require(right >= 0, "no key " + id);
    return {(left + right) / 2, (top + bottom) / 2 + popup_margin};
}
void press(Simulator& simulator, const std::string& id) {
    const auto [x, y] = key_point(simulator, id);
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    wait(simulator, .03);
    simulator.pointer_up(Simulator::mouse, x, y);
    wait(simulator);
}
Setup setup(const Profile& profile, const char* layout, const char* language, bool slow) {
    Setup result;
    result.config_dir = profile.path;
    result.models_dir = profile.path / "models";
    result.layout = layout;
    result.language = language;
    result.slow = slow;
    result.japanese_converter = [] { return std::make_unique<Reading>(); };
    return result;
}
bool has_control(Simulator& simulator, const std::string& id) {
    const auto controls = simulator.app().view().controls;
    return std::any_of(controls.begin(), controls.end(), [&](const Control& c) { return c.id == id; });
}

// Flick types kana, Enter commits it into the field, and ↶ takes the phrase
// back out into the reading: in a slow field only once its Backspace is
// confirmed.
void japanese_reopen(bool slow) {
    Profile profile;
    Simulator simulator(setup(profile, "ja-flick-12", "ja-flick", slow));
    wait(simulator);
    require(has_control(simulator, "close") && !has_control(simulator, "move-handle"),
            "the × shows, as on the headset, and nothing moves the keyboard");
    for (const auto& control : simulator.app().view().controls) {
        if (control.id == "close") {
            const double x = control.bounds.x + control.bounds.width / 2,
                         y = control.bounds.y + control.bounds.height / 2 + popup_margin;
            simulator.pointer_down(Simulator::mouse, x, y, clock_now);
            simulator.pointer_up(Simulator::mouse, x, y);
        }
    }
    wait(simulator);
    require(!simulator.quitting(), "and closes nothing");
    press(simulator, "Flick-a");
    require(simulator.app().view().preedit == "あ", "a flick tap composes あ");
    press(simulator, "Enter");
    require(simulator.field().text() == "あ", "Enter commits the reading into the field");
    press(simulator, "Flick-undo");
    wait(simulator, .2);
    require(simulator.field().text().empty() && simulator.app().view().preedit == "あ",
            std::string("↶ reopens the committed phrase") + (slow ? " in a slow field" : ""));
    press(simulator, "Enter");
    require(simulator.field().text() == "あ", "and Enter commits it again");
    // The mouse leaving the window can't change the field: the chance stays.
    simulator.pointer_exit(Simulator::mouse);
    press(simulator, "Flick-undo");
    wait(simulator, .2);
    require(simulator.field().text().empty() && simulator.app().view().preedit == "あ",
            "↶ still reopens after the mouse left the window and came back");
    press(simulator, "Enter");
    // So does a move outside the window with the button up, as a
    // pointer-driving tool parks the mouse after each click.
    simulator.pointer_move(Simulator::mouse, Simulator::width * 3, 0);
    simulator.pointer_exit(Simulator::mouse);
    press(simulator, "Flick-undo");
    wait(simulator, .2);
    require(simulator.field().text().empty() && simulator.app().view().preedit == "あ",
            "↶ still reopens after a move outside the window");
    // Clear field starts anew, the reading too.
    simulator.pointer_down(Simulator::mouse, Simulator::clear_button().x + 5,
                           Simulator::clear_button().y + 5, clock_now);
    simulator.pointer_up(Simulator::mouse, Simulator::clear_button().x + 5,
                         Simulator::clear_button().y + 5);
    require(simulator.field().text().empty() && simulator.app().view().preedit.empty(),
            "Clear field also cancels the reading");
    press(simulator, "Flick-a");
    press(simulator, "Enter");

    // The pointer leaving the keyboard is the laser leaving the panel: the
    // chance to reopen ends, and a flick that leaves types nothing.
    const auto [x, y] = key_point(simulator, "Flick-ka");
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_move(Simulator::mouse, x, 60);
    press(simulator, "Flick-undo");
    require(simulator.field().text() == "あ", "no reopen after the pointer left");
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    simulator.pointer_move(Simulator::mouse, x, 60);
    simulator.pointer_up(Simulator::mouse, x, 60);
    wait(simulator);
    require(simulator.app().view().preedit.empty(), "a flick dragged off the keyboard types nothing");
    simulator.shutdown();
}

void english() {
    Profile profile;
    Simulator simulator(setup(profile, "en-us-full", "en-us", true));
    wait(simulator);
    press(simulator, "KeyH");
    press(simulator, "KeyI");
    require(simulator.field().text() == "hi", "English letters type into the field");
    // The held second pointer is a second hand on Shift.
    const auto [sx, sy] = key_point(simulator, "ShiftLeft");
    simulator.toggle_hold(sx, sy, clock_now);
    wait(simulator, .05);
    press(simulator, "KeyA");
    simulator.toggle_hold(0, 0, clock_now);
    wait(simulator);
    press(simulator, "KeyB");
    require(simulator.field().text() == "hiAb", "a held Shift capitalizes the other pointer's key");
    // Ctrl+A, then the side keys copy and paste within the field.
    press(simulator, "ControlLeft");
    press(simulator, "KeyA");
    press(simulator, "Copy");
    press(simulator, "ArrowRight");
    press(simulator, "Paste");
    require(simulator.field().text() == "hiAbhiAb" && simulator.field().clipboard() == "hiAb",
            "Copy and Paste use the field's own clipboard");
    // A click in the field moves its caret, as in the application.
    simulator.pointer_move(Simulator::mouse, Simulator::field_bounds.x + 15,
                           Simulator::field_bounds.y + 30);
    simulator.pointer_down(Simulator::mouse, Simulator::field_bounds.x + 15,
                           Simulator::field_bounds.y + 30, clock_now);
    simulator.pointer_up(Simulator::mouse, Simulator::field_bounds.x + 15,
                         Simulator::field_bounds.y + 30);
    press(simulator, "KeyX");
    require(simulator.field().text() == "xhiAbhiAb", "a click in the field places the caret");
    // Its own buttons: Slow field toggles, Clear empties.
    const auto slow = Simulator::slow_button();
    simulator.pointer_down(Simulator::mouse, slow.x + 5, slow.y + 5, clock_now);
    simulator.pointer_up(Simulator::mouse, slow.x + 5, slow.y + 5);
    require(!simulator.sink().slow(), "the Slow field button toggles it");
    const auto clear = Simulator::clear_button();
    simulator.pointer_down(Simulator::mouse, clear.x + 5, clear.y + 5, clock_now);
    simulator.pointer_up(Simulator::mouse, clear.x + 5, clear.y + 5);
    require(simulator.field().text().empty(), "Clear field empties it");
    require(simulator.tick(clock_now), "the canvas redraws after a change");
    simulator.shutdown();
}

// Typing suggestions keep the word while the mouse rests over the field, as a
// laser pointing away with its trigger at rest; a click there forgets it.
std::vector<std::string> chips(Simulator& simulator) {
    std::vector<std::string> words;
    for (const auto& c : simulator.app().view().controls) {
        if (c.id.starts_with("typing-suggestion-")) {
            words.push_back(c.label);
        }
    }
    return words;
}
bool all_start(const std::vector<std::string>& words, const std::string& start) {
    return !words.empty() && std::all_of(words.begin(), words.end(), [&](const std::string& word) {
        return word.starts_with(start);
    });
}
void typing_suggestions() {
    Profile profile;
    Simulator simulator(setup(profile, "en-us-full", "en-us", false));
    wait(simulator);
    press(simulator, "KeyH");
    press(simulator, "KeyE");
    press(simulator, "KeyL");
    // The word list loads on a thread of its own, slower in a sanitizer build.
    for (int i = 0; i < 300 && chips(simulator).empty(); ++i) {
        wait(simulator);
    }
    require(all_start(chips(simulator), "hel"), "completions of the typed word show");
    const double x = Simulator::field_bounds.x + 15, y = Simulator::field_bounds.y + 30;
    simulator.pointer_move(Simulator::mouse, x, y);
    wait(simulator);
    press(simulator, "KeyP");
    require(all_start(chips(simulator), "help"), "the mouse over the field keeps the word");
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    simulator.pointer_up(Simulator::mouse, x, y);
    wait(simulator);
    require(chips(simulator).empty(), "a click in the field forgets it");
    press(simulator, "KeyO");
    require(all_start(chips(simulator), "o"), "and typing goes on at a word's start");
    simulator.shutdown();
}

void bad_selection() {
    Profile profile;
    auto unknown = setup(profile, "en-us-full", "xx-none", false);
    bool refused = false;
    try {
        Simulator simulator(unknown);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused, "an unknown language is refused");
    refused = false;
    try {
        Simulator simulator(setup(profile, "en-us-full", "ja-flick", false));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused, "a layout that cannot type the language is refused");
    Setup only_language;
    only_language.config_dir = profile.path;
    only_language.language = "ja-flick";
    only_language.japanese_converter = [] { return std::make_unique<Reading>(); };
    Simulator simulator(only_language);
    require(simulator.app().selection().layout == "ja-flick-12",
            "a language alone picks a layout that types it");
    simulator.shutdown();
}
} // namespace

// The shortcut bar shows above the case in the simulator's window, over the
// field's foot, and takes clicks: its button's key reaches the field.
void shortcut_bar() {
    Profile profile;
    Settings settings;
    settings.shortcuts = true;
    save_settings(profile.path, settings);
    ShortcutModel model;
    ShortcutButton home;
    home.label = "Home";
    home.action.chords = {{{}, "Home"}};
    model.profiles.push_back({"b", "Bar", {home}});
    model.free_profile = "b";
    std::ofstream(shortcuts_path(profile.path)) << serialize_shortcuts(model);
    Simulator simulator(setup(profile, "en-us-full", "en-us", false));
    wait(simulator);
    const auto view = simulator.app().view();
    require(view.shortcut_bar && view.shortcut_bar->buttons.size() == 1,
            "the bar shows in the simulator");
    press(simulator, "KeyH");
    press(simulator, "KeyI");
    const auto& button = view.shortcut_bar->buttons[0].bounds;
    const double x = button.x + button.width / 2, y = button.y + button.height / 2 + popup_margin;
    require(y > 0 && y < popup_margin, "the bar is above the case");
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    wait(simulator, .03);
    simulator.pointer_up(Simulator::mouse, x, y);
    wait(simulator);
    press(simulator, "KeyX");
    require(simulator.field().text() == "xhi", "a click on the bar's button sends its key to the field");
    simulator.shutdown();
}
// The shortcut editor opens in the simulator from the picker and takes its
// clicks; a bar's name is typed on the keyboard, reaching nothing else.
void click_rect(Simulator& simulator, const Rect& r) {
    const double x = r.x + r.width / 2, y = r.y + r.height / 2 + popup_margin;
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    wait(simulator, .03);
    simulator.pointer_up(Simulator::mouse, x, y);
    wait(simulator);
}
Rect control_bounds(Simulator& simulator, const std::string& id) {
    for (const auto& control : simulator.app().view().controls) {
        if (control.id == id) {
            return control.bounds;
        }
    }
    throw std::runtime_error("no control " + id);
}
void shortcut_editor() {
    Profile profile;
    Settings settings;
    settings.shortcuts = true;
    save_settings(profile.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"b", "Bar", {}});
    model.free_profile = "b";
    std::ofstream(shortcuts_path(profile.path)) << serialize_shortcuts(model);
    Simulator simulator(setup(profile, "en-us-full", "en-us", false));
    wait(simulator);
    click_rect(simulator, simulator.app().view().shortcut_bar->picker);
    click_rect(simulator, simulator.app().view().shortcut_bar->menu->manage);
    require(simulator.app().view().shortcut_editor.has_value(), "the editor opens in the simulator");
    click_rect(simulator, control_bounds(simulator, "shortcuts-new-profile"));
    require(simulator.app().view().shortcut_editor->popup.has_value() &&
                !simulator.app().view().shortcut_field,
            "New bar opens its name in the editor's popup");
    click_rect(simulator, control_bounds(simulator, "shortcuts-popup-key-KeyQ"));
    click_rect(simulator, control_bounds(simulator, "shortcuts-popup-done"));
    const auto view = simulator.app().view();
    bool named = false;
    for (const auto& control : view.controls) {
        named |= control.id.starts_with("shortcuts-row-") && control.label == "Profile 2q";
    }
    require(named && simulator.field().text().empty() && !view.shortcut_editor->popup,
            "the popup takes clicks: its name is typed into the editor, not the field");
    // The Macro view opens and takes clicks.
    click_rect(simulator, control_bounds(simulator, "shortcuts-add"));
    click_rect(simulator, control_bounds(simulator, "shortcuts-kind-1"));
    click_rect(simulator, control_bounds(simulator, "shortcuts-add-wait"));
    click_rect(simulator, control_bounds(simulator, "shortcuts-step-plus-0"));
    const auto macro = simulator.app().view();
    require(control_bounds(simulator, "shortcuts-kind-1").width > 0 &&
                macro.shortcut_editor->steps.size() == 1 &&
                macro.shortcut_editor->steps[0].time == "0.6 s",
            "the Macro view takes clicks: a wait added and changed");
    simulator.shutdown();
}
// The App button goes round its apps; a profile assigned to one follows it.
void app_button() {
    Profile profile;
    Settings settings;
    settings.shortcuts = true;
    save_settings(profile.path, settings);
    ShortcutModel model;
    ShortcutButton home;
    home.label = "Home";
    home.action.chords = {{{}, "Home"}};
    ShortcutButton end = home;
    end.label = "End";
    end.action.chords = {{{}, "End"}};
    model.profiles.push_back({"f", "Free", {home}});
    model.profiles.push_back({"g", "Game keys", {end}});
    model.free_profile = "f";
    std::ofstream(shortcuts_path(profile.path)) << serialize_shortcuts(model);
    Simulator simulator(setup(profile, "en-us-full", "en-us", false));
    wait(simulator);
    const auto label = [&] { return simulator.app().view().shortcut_bar->buttons[0].label; };
    const auto click_app = [&] {
        const auto r = Simulator::app_button();
        simulator.pointer_down(Simulator::mouse, r.x + 5, r.y + 5, clock_now);
        simulator.pointer_up(Simulator::mouse, r.x + 5, r.y + 5);
        wait(simulator);
    };
    require(simulator.app_label() == "App: Notes" && simulator.sink().focused_app().app &&
                simulator.sink().focused_app().app->kind == AppIdentity::Kind::Desktop,
            "it starts on Notes, a Desktop app");
    click_app();
    click_app();
    require(simulator.app_label() == "App: Game" && label() == "Home", "on to Game, without a profile");
    click_rect(simulator, simulator.app().view().shortcut_bar->picker);
    // The name comes from the lookup's thread, on a tick; an assign button's
    // tooltip names the app.
    const auto tip = [&] {
        const auto r = simulator.app().view().shortcut_bar->menu->rows[1].assign;
        simulator.pointer_move(Simulator::mouse, r.x + r.width / 2, r.y + r.height / 2 + popup_margin);
        return simulator.app().view().bar_tooltip;
    };
    for (int i = 0; i < 500 && tip() != "Show Game keys whenever Game has the keys"; ++i) {
        usleep(2000);
        wait(simulator, .01);
    }
    require(tip() == "Show Game keys whenever Game has the keys",
            "the tooltip names it with the simulator's own name");
    click_rect(simulator, simulator.app().view().shortcut_bar->menu->rows[1].assign);
    require(label() == "End", "Game keys, assigned, shows at once");
    click_rect(simulator, simulator.app().view().shortcut_bar->picker);
    click_app();
    require(simulator.app_label() == "App: Moonlight" && label() == "Home",
            "another app: the free profile");
    click_app();
    click_rect(simulator, simulator.app().view().shortcut_bar->picker);
    const auto menu = *simulator.app().view().shortcut_bar->menu;
    require(simulator.app_label() == "App: Steam" && !menu.rows[0].assign_enabled,
            "Steam's dashboard: the assign buttons disabled");
    click_rect(simulator, simulator.app().view().shortcut_bar->picker);
    click_app();
    click_app();
    click_app();
    require(simulator.app_label() == "App: Game" && label() == "End",
            "round again to Game: its profile");
    simulator.shutdown();
}
// A Run button starts its program on the computer the simulator runs on:
// here a command writing a marker in the test's own folder.
void run_button() {
    Profile profile;
    const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", profile.path.c_str(), 1);
    Settings settings;
    settings.shortcuts = true;
    save_settings(profile.path, settings);
    const auto marker = profile.path / "run-marker";
    ShortcutButton run;
    run.label = "Mark";
    run.action.kind = ShortcutAction::Kind::Run;
    run.action.command = "echo started > '" + marker.string() + "'";
    ShortcutModel model;
    model.profiles.push_back({"r", "Run", {run}});
    model.free_profile = "r";
    std::ofstream(shortcuts_path(profile.path)) << serialize_shortcuts(model);
    Simulator simulator(setup(profile, "en-us-full", "en-us", false));
    wait(simulator);
    require(simulator.app().view().shortcut_bar->buttons[0].enabled, "a Run button is enabled");
    click_rect(simulator, simulator.app().view().shortcut_bar->buttons[0].bounds);
    for (int i = 0; i < 300 && !fs::exists(marker); ++i) {
        usleep(5000);
        wait(simulator, .01);
    }
    require(fs::exists(marker) && simulator.field().text().empty(),
            "a click starts its command on this computer; nothing is typed");
    simulator.shutdown();
    setenv("HOME", old_home.c_str(), 1);
}
int main() {
    try {
        injected_converter();
        candidate_profile();
        chinese_candidate_report();
        editing();
        slow_field();
        japanese_reopen(false);
        japanese_reopen(true);
        english();
        typing_suggestions();
        bad_selection();
        shortcut_bar();
        shortcut_editor();
        app_button();
        run_button();
        std::cout << "simulator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "simulator tests failed: " << error.what() << '\n';
        return 1;
    }
}
