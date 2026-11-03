// The desktop simulator's text field, its slow confirmations, and the real
// keyboard typing into it through the simulator's pointer routing.
#include "simulator/simulator.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
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
    require(!has_control(simulator, "close") && !has_control(simulator, "move-handle"),
            "nothing closes or moves the simulator's keyboard");
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

int main() {
    try {
        editing();
        slow_field();
        japanese_reopen(false);
        japanese_reopen(true);
        english();
        bad_selection();
        std::cout << "simulator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "simulator tests failed: " << error.what() << '\n';
        return 1;
    }
}
