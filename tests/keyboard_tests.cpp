#include "framekeyboard/app.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
struct Capture : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
struct TemporaryDirectory {
    fk::fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-tests-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fk::fs::remove_all(path, ignored);
    }
};
void write(const fk::fs::path& path, const std::string& text) {
    fk::fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
const fk::Key& key(const fk::Layout& layout, const std::string& id) {
    for (const auto& key : layout.keys) {
        if (key.id == id) {
            return key;
        }
    }
    throw std::runtime_error("missing test key " + id);
}
void state_tests(const fk::Layout& layout) {
    Capture sink;
    fk::KeyboardState state(sink);
    const auto left_super = key(layout, "MetaLeft");
    const auto right_super = key(layout, "MetaRight");
    require(left_super.sticky && !right_super.sticky, "Super keys have distinct tap behavior");
    state.down(0, left_super, 0);
    state.up(0);
    require(sink.events.empty() && state.modifiers().contains(KEY_LEFTMETA),
            "left Super tap latches without opening a menu");
    state.down(0, key(layout, "KeyA"), .1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTMETA, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTMETA, 0}},
            "latched Super sends a native chord");
    sink.events.clear();
    state.down(0, right_super, 0);
    state.tick(20);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}},
            "right Super sends immediate down without repeating");
    state.up(0);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                state.modifiers().empty(),
            "right Super sends up without latching");
    for (bool cancel_all : {false, true}) {
        sink.events.clear();
        state.down(0, right_super, 0);
        state.down(1, right_super, 0);
        state.up(0);
        require(sink.events.size() == 1, "overlapping Super holds share native ownership");
        if (cancel_all) {
            state.cancel_all();
        } else {
            state.cancel_pointer(1);
        }
        require(sink.events ==
                        std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                    state.modifiers().empty(),
                "canceling Super releases its native hold without a latch");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 0);
    state.up(0);
    require(sink.events.empty(), "a latched modifier must not reach the OS until used");
    state.down(0, key(layout, "KeyA"), 1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTCTRL, 0}},
            "Ctrl+A order");
    require(state.modifiers().empty(), "one-shot modifier consumed");
    sink.events.clear();
    state.down(0, key(layout, "Copy"), 2);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
            "Copy order");
    sink.events.clear();
    state.down(0, key(layout, "Cut"), 2);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_X, 1}, {KEY_X, 0}, {KEY_LEFTCTRL, 0}},
            "Cut order");
    sink.events.clear();
    state.down(0, key(layout, "KeyA"), 3);
    state.down(1, key(layout, "KeyA"), 3);
    state.up(0);
    require(sink.events.size() == 1, "two pointers share one down");
    state.up(1);
    require(sink.events.size() == 2 && sink.events.back().second == 0, "last pointer releases key");
    for (const auto* id : {"CapsLock", "NumLock"}) {
        const auto lock = key(layout, id);
        const int code = fk::key_code(lock.action);
        auto locked = [&] { return code == KEY_CAPSLOCK ? state.caps() : state.num(); };
        // Exercise both release orders, and a new click while the other hand
        // still holds the key. Only the final release permits another toggle.
        for (unsigned first : {0u, 1u}) {
            sink.events.clear();
            require(!locked(), "lock starts off");
            state.down(0, lock, 3);
            state.down(1, lock, 3.1);
            require(locked(), "overlapping lock presses toggle only once");
            state.up(first);
            state.down(first, lock, 3.2);
            require(locked() && sink.events.size() == 1, "remaining hold prevents a second toggle");
            state.up(first);
            state.up(1 - first);
            require(sink.events == std::vector<std::pair<int, int>>{{code, 1}, {code, 0}},
                    "overlapping lock holds share one backend press and release");
            state.down(first, lock, 3.3);
            state.up(first);
            require(!locked() && sink.events.size() == 4, "next separate click toggles lock off");
        }
    }
    sink.events.clear();
    state.down(0, key(layout, "ShiftLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4);
    state.up(0);
    state.up(1);
    require(state.modifiers().empty(), "a modifier used while held must not latch on release");
    for (const auto* modifier : {"ControlLeft", "ShiftLeft", "AltLeft", "MetaLeft", "MetaRight"}) {
        sink.events.clear();
        const int code = fk::key_code(modifier);
        state.down(0, key(layout, modifier), 4);
        state.down(1, key(layout, "KeyA"), 4.1);
        state.up(0);
        require(sink.events.back() == std::pair<int, int>{code, 0},
                "releasing held modifier reaches backend before ordinary key release");
        require(!state.modifiers().contains(code), "released modifier does not affect next key");
        state.down(0, key(layout, "KeyB"), 4.2);
        state.up(0);
        state.up(1);
        require(std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 1}) == 1 &&
                    std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 0}) ==
                        1,
                "next key neither reacquires nor double-releases the old modifier");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4.1);
    state.down(2, key(layout, "ControlLeft"), 4.2);
    state.up(0);
    require(state.modifiers().contains(KEY_LEFTCTRL), "another hold retains the same modifier");
    state.up(2);
    require(!state.modifiers().contains(KEY_LEFTCTRL), "last modifier hold releases without latching");
    state.up(1);
    sink.events.clear();
    state.down(0, key(layout, "Backspace"), 5);
    require(state.repeats(0) && !state.repeats(1), "a held Backspace repeats, from the press on");
    state.tick(5.49);
    require(sink.events.size() == 1, "repeat delay");
    state.tick(5.51);
    require(sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2}, "repeat event");
    state.tick(40);
    require(state.active() && sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2},
            "long hold keeps repeating without replaying a backlog");
    state.up(0);
    require(!state.repeats(0), "a released key no longer repeats");
    state.down(0, key(layout, "CapsLock"), 40.5);
    require(!state.repeats(0), "a lock key does not repeat");
    state.up(0);
    state.down(0, key(layout, "ShiftLeft"), 41);
    state.down(1, key(layout, "ArrowLeft"), 41);
    state.tick(80);
    require(state.modifiers().contains(KEY_LEFTSHIFT), "modifier survives a long two-hand chord");
    state.cancel_pointer(1);
    require(state.pointer_pressed(0) && state.modifiers().contains(KEY_LEFTSHIFT),
            "leaving with the other pointer does not release the held modifier");
    state.down(1, key(layout, "ArrowRight"), 81);
    state.cancel_pointer(0);
    require(!state.modifiers().contains(KEY_LEFTSHIFT) && state.pointer_pressed(1),
            "canceling a modifier releases borrowed references without canceling the other key");
    state.up(1);
    state.down(0, key(layout, "ControlLeft"), 82);
    state.cancel_pointer(0);
    require(state.modifiers().empty(), "canceled modifier tap must not latch");
    // Holding Shift locks it, like a phone keyboard's caps lock: it applies to
    // every key until the next Shift press, for selecting text with arrows.
    using Events = std::vector<std::pair<int, int>>;
    const auto shift = key(layout, "ShiftLeft");
    state.down(0, shift, 90);
    state.tick(90.4);
    require(!state.locked(KEY_LEFTSHIFT), "a short hold does not lock");
    state.tick(90.5);
    require(state.locked(KEY_LEFTSHIFT), "a half-second hold locks while still held");
    state.up(0);
    require(state.locked(KEY_LEFTSHIFT) && state.modifiers().contains(KEY_LEFTSHIFT),
            "the release keeps it locked");
    for (const auto* arrow : {"ArrowLeft", "ArrowLeft"}) {
        sink.events.clear();
        state.down(0, key(layout, arrow), 91);
        state.up(0);
        require(sink.events ==
                    Events{{KEY_LEFTSHIFT, 1}, {KEY_LEFT, 1}, {KEY_LEFT, 0}, {KEY_LEFTSHIFT, 0}},
                "every key gets Shift, released after it");
    }
    state.down(0, shift, 92);
    state.tick(93); // a long press unlocks as well
    state.up(0);
    require(!state.locked(KEY_LEFTSHIFT) && state.modifiers().empty(),
            "the next Shift press unlocks, without latching");
    state.down(0, shift, 94);
    state.down(1, key(layout, "ArrowLeft"), 94.1);
    state.tick(95);
    state.up(1);
    state.up(0);
    require(!state.locked(KEY_LEFTSHIFT), "Shift held for a chord does not lock");
    state.down(0, shift, 96);
    state.tick(97);
    state.up(0);
    // Ending the presses for ↶ lets held keys up but keeps the lock, even
    // through a Shift press that would have unlocked it.
    state.down(0, key(layout, "ArrowLeft"), 97.1);
    state.down(1, shift, 97.2);
    sink.events.clear();
    state.release_presses();
    require(state.locked(KEY_LEFTSHIFT) && !state.active() &&
                sink.events == Events{{KEY_LEFT, 0}, {KEY_LEFTSHIFT, 0}},
            "ending the presses keeps Shift locked and lets the keys up");
    state.cancel_all();
    require(!state.locked(KEY_LEFTSHIFT), "hiding the keyboard ends the lock");
    state.down(0, key(layout, "ControlLeft"), 98);
    state.tick(99);
    state.up(0);
    require(!state.locked(KEY_LEFTCTRL) && state.modifiers().contains(KEY_LEFTCTRL),
            "only Shift locks; other modifiers still latch");
    state.cancel_all();
    // Releasing the Shift that just locked keeps Shift on a key the other
    // hand holds, through the output queue as the app uses it.
    for (const auto* name : {"ShiftLeft", "ShiftRight"}) {
        const int code = fk::key_code(name);
        Capture target;
        fk::OutputController output(target);
        output.set_enabled(true);
        fk::KeyboardState hands(output);
        const auto held = [&](int key) {
            int count = 0;
            for (const auto& [event, value] : target.events) {
                count += event != key ? 0 : value == 1 ? 1 : value == 0 ? -1 : 0; // 2 repeats
            }
            return count;
        };
        // Shift first: it locks, the arrow takes it, then Shift's hand lets go.
        hands.down(0, key(layout, name), 100);
        hands.tick(100.6);
        hands.down(1, key(layout, "ArrowLeft"), 100.7);
        output.tick(100.7);
        hands.up(0);
        output.tick(100.8);
        require(hands.locked(code) && held(KEY_LEFT) == 1 && held(code) == 1,
                "the held arrow keeps the locked Shift");
        hands.up(1);
        output.tick(100.9);
        require(held(KEY_LEFT) == 0 && held(code) == 0 && target.events.back() == std::pair{code, 0},
                "and releases it after itself");
        // Arrow first: it was pressed without Shift and stays that way.
        hands.cancel_all();
        target.events.clear();
        hands.down(1, key(layout, "ArrowLeft"), 101);
        hands.down(0, key(layout, name), 101.1);
        hands.tick(101.7);
        output.tick(101.7);
        hands.up(0);
        output.tick(101.8);
        require(hands.locked(code) && held(KEY_LEFT) == 1 && held(code) == 0,
                "an arrow already held is not changed by the lock");
        hands.up(1);
        output.tick(101.9);
        require(held(KEY_LEFT) == 0 && held(code) == 0, "and nothing is left held");
    }
    sink.events.clear();
    state.down(0, key(layout, "Enter"), 41);
    state.cancel_all();
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_ENTER, 1}, {KEY_ENTER, 0}},
            "Enter and cancellation");
}
void profile_tests(const fk::Profiles& defaults) {
    TemporaryDirectory directory;
    write(
        directory.path / "layouts/custom.json",
        R"({"schema_version":1,"id":"compact","name":"Compact","width":200,"height":100,"keys":[{"id":"Enter","label":"Enter","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"Enter"}}]})");
    auto loaded = fk::load_profiles({}, directory.path);
    require(loaded.layouts.contains("compact"), "discover user layout");
    write(directory.path / "layouts/custom.json", "{broken");
    auto reloaded = fk::load_profiles({}, directory.path, &loaded);
    require(reloaded.layouts.contains("compact") && !reloaded.errors.empty(),
            "retain valid profile after malformed edit");
    fk::fs::remove(directory.path / "layouts/custom.json");
    reloaded = fk::load_profiles({}, directory.path, &reloaded);
    require(!reloaded.layouts.contains("compact"), "deleted user profile disappears");
    write(
        directory.path / "config.json",
        R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"custom":42,"favorites":[{"id":"de","name":"Deutsch","layout":"en-us-full","language":"de-de","theme":"midnight"}]})");
    fk::save_selection(directory.path, {"en-us-full", "de-de", "midnight"});
    std::vector<std::string> errors;
    auto saved = fk::load_settings(directory.path, errors);
    require(saved.active.language == "de-de" && saved.favorites.size() == 1 && errors.empty(),
            "settings preserve selection and favorites");
    std::ifstream file(directory.path / "config.json");
    const std::string content((std::istreambuf_iterator<char>(file)), {});
    require(content.find("custom") != std::string::npos, "preserve unrelated settings");
    write(directory.path / "config.json", "{broken");
    rejects([&] { fk::save_selection(directory.path, {}); }, "do not overwrite malformed settings");
    rejects([] { fk::parse_layout(R"({"schema_version":2})"); }, "reject future schema");
    rejects(
        [] {
            fk::parse_layout(
                R"({"schema_version":1,"id":"bad","name":"Bad","width":200,"height":100,"keys":[{"id":"A","label":"A","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyA"}},{"id":"B","label":"B","x":50,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyB"}}]})");
        },
        "reject overlaps");
    fk::LanguageMap english(defaults.languages.at("en-us")), german(defaults.languages.at("de-de"));
    const auto& layout = defaults.layouts.at("en-us-full");
    require(english.legend(key(layout, "KeyY"), {}, false, false) == "y", "US Y legend");
    require(english.legend(key(layout, "KeyY"), {KEY_LEFTSHIFT}, false, false) == "Y",
            "Shift uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {}, true, false) == "Y",
            "Caps Lock uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {KEY_RIGHTSHIFT}, true, false) == "y",
            "Shift with Caps Lock matches lowercase key output");
    require(german.legend(key(layout, "KeyY"), {}, false, false) == "z", "German Z legend");
    require(german.legend(key(layout, "BracketLeft"), {}, false, false) == "ü", "German umlaut");
    require(german.legend(key(layout, "Backquote"), {}, false, false) == "^", "German dead-key legend");
    rejects([&] { fk::validate_selection(defaults, {"en-us-full", "de-de", "graphite"}); },
            "reject missing language key");
    require(german.legend(key(layout, "KeyQ"), {KEY_RIGHTALT}, false, false) == "@",
            "German AltGr legend");
}
// The key whose area holds a point, or "" between blocks. Throws where two do.
std::string area_key(const fk::Layout& layout, const std::vector<fk::KeyArea>& areas, double x,
                     double y) {
    std::string found;
    for (const auto& area : areas) {
        if (area.bounds.contains(x, y)) {
            require(found.empty() || found == layout.keys[area.key].id, "areas never overlap");
            found = layout.keys[area.key].id;
        }
    }
    return found;
}
// Extended key collision (key_areas.hpp).
void key_area_tests(const fk::Profiles& defaults) {
    for (const auto& [id, layout] : defaults.layouts) {
        const auto areas = fk::key_areas(layout);
        for (std::size_t i = 0; i < areas.size(); ++i) {
            for (std::size_t j = i + 1; j < areas.size(); ++j) {
                const auto &a = areas[i].bounds, &b = areas[j].bounds;
                const double across = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
                const double down = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
                require(across <= 1e-6 || down <= 1e-6, "no two areas overlap, in any bundled layout");
            }
        }
        for (std::size_t k = 0; k < layout.keys.size(); ++k) {
            const auto& r = layout.keys[k].bounds;
            require(std::any_of(areas.begin(), areas.end(),
                                [&](const fk::KeyArea& area) {
                                    const auto& b = area.bounds;
                                    return area.key == k && b.x <= r.x && b.y <= r.y &&
                                           b.x + b.width >= r.x + r.width &&
                                           b.y + b.height >= r.y + r.height;
                                }),
                    "every key lies inside its own area");
        }
    }
    const auto& us = defaults.layouts.at("en-us-full");
    const auto areas = fk::key_areas(us);
    auto at = [&](double x, double y) { return area_key(us, areas, x, y); };
    // Keys of a block split the gaps between them in the middle.
    require(at(266.5, 170) == "KeyQ" && at(269.5, 170) == "KeyW", "Q and W share their gap");
    require(at(250, 202) == "KeyQ" && at(250, 208) == "KeyA", "rows share the gap between them");
    require(at(267, 203) == "KeyQ" && at(269, 207) == "KeyA", "and the corners where gaps cross");
    require(at(221, 240) == "CapsLock" && at(224, 240) == "KeyA", "Caps Lock and A share theirs");
    require(at(130, 275) == "CapsLock" && at(130, 279) == "ShiftLeft", "so do Caps Lock and Shift");
    require(at(298, 20) == "F1" && at(1438, 131) == "NumpadSubtract",
            "within the function keys and numpad");
    // Wider gaps separate blocks and stay empty, and blocks don't grow outward.
    require(at(260, 55).empty(), "nothing between the number row and the function keys");
    require(at(1033, 100).empty(), "nothing between Backspace and Insert");
    require(at(200, 20).empty() && at(495, 20).empty(), "nor between Esc and F1 or the F-key groups");
    require(at(100, 100).empty() && at(1233, 200).empty(), "nor beside the side column or numpad");
    require(at(118, 100).empty() && at(200, 64).empty(), "a block's outer edge stays where it is");
    // A hidden key takes no area, and its neighbour does not grow toward it.
    std::vector<bool> absent(us.keys.size());
    for (std::size_t k = 0; k < us.keys.size(); ++k) {
        absent[k] = us.keys[k].id == "Dictation";
    }
    require(at(40, 299) == "Paste" && area_key(us, fk::key_areas(us, absent), 40, 299).empty(),
            "Paste reaches toward the mic key only while it shows");
    // ISO and JIS Enter: the pocket beside it is split with ].
    for (const char* id : {"en-gb-full", "ja-jis-full"}) {
        const auto& layout = defaults.layouts.at(id);
        const auto pocket = fk::key_areas(layout);
        require(area_key(layout, pocket, 935, 170) == "BracketRight" &&
                    area_key(layout, pocket, 945, 170) == "Enter" &&
                    area_key(layout, pocket, 945, 135) == "Enter" &&
                    area_key(layout, pocket, 935, 204) == "BracketRight",
                "the pocket beside a tall Enter is split between ] and Enter");
        require(std::count_if(
                    pocket.begin(), pocket.end(),
                    [&](const fk::KeyArea& area) { return layout.keys[area.key].id == "Enter"; }) == 2,
                "which makes Enter's area an L");
    }
    // The emoji picker's and dictation mode's own few keys share only their
    // gaps, never reaching into the grid, its tabs or the editor.
    auto clear_of = [](const std::vector<fk::KeyArea>& list, const fk::Rect& r) {
        return std::none_of(list.begin(), list.end(), [&](const fk::KeyArea& area) {
            const auto& b = area.bounds;
            return b.x < r.x + r.width && r.x < b.x + b.width && b.y < r.y + r.height &&
                   r.y < b.y + b.height;
        });
    };
    const auto picker = fk::emoji_layout(us, us.width);
    const auto picker_areas = fk::key_areas(picker.layout);
    require(clear_of(picker_areas, picker.grid) && clear_of(picker_areas, picker.tabs) &&
                area_key(picker.layout, picker_areas, 1362, 380) == "ArrowLeft" &&
                area_key(picker.layout, picker_areas, 1365, 380) == "ArrowDown",
            "the picker's keys share their gaps, clear of its grid and tabs");
    const auto dictation = fk::dictation_layout(us, us.width);
    require(clear_of(fk::key_areas(dictation.layout), dictation.editor),
            "dictation mode's keys stay clear of the editor");
    require(fk::flick_page(defaults.layouts.at("ja-flick-12")) &&
                !fk::flick_page(defaults.layouts.at("ja-flick-qwerty")) && !fk::flick_page(us),
            "the 12-key page is a flick page; its English page is not");
}
// A panel point for a layout point, in the view's placement.
std::pair<double, double> panel_point(fk::App& app, const fk::PanelView& view, double x, double y) {
    const auto [left, top] = app.renderer.layout_point(view, 0, 0);
    const auto [right, bottom] = app.renderer.layout_point(view, 1000, 1000);
    const double scale = 1000 / (right - left);
    return {(x - left) * scale, (y - top) * 1000 / (bottom - top)};
}
void app_key_area_tests() {
    TemporaryDirectory directory;
    fk::Options options;
    options.config_dir = directory.path;
    Capture sink;
    auto hit = [](fk::App& app, double x, double y) -> std::string {
        const auto view = app.input_view();
        const auto [px, py] = panel_point(app, view, x, y);
        const auto* key = app.renderer.hit_key(view, px, py);
        return key ? key->id : "";
    };
    {
        fk::App app(options, sink);
        require(app.input_view().key_areas && hit(app, 266.5, 170) == "KeyQ" &&
                    hit(app, 269.5, 170) == "KeyW" && hit(app, 260, 55).empty(),
                "by default a press between keys of a block reaches the nearer key");
        app.apply({"ja-flick-12", "ja-flick", "graphite"});
        require(!app.input_view().key_areas && hit(app, 465.5, 30).empty(),
                "the flick page keeps its gaps");
    }
    fk::Settings settings;
    settings.extended_key_collision = false;
    fk::save_settings(directory.path, settings);
    fk::App app(options, sink);
    require(!app.input_view().key_areas && hit(app, 266.5, 170).empty(),
            "with Extended key collision off, a gap is no key");
    require(sink.events.empty(), "hit tests send nothing");
}
void app_tests() {
    TemporaryDirectory directory;
    fk::Options options;
    options.config_dir = directory.path;
    Capture sink;
    fk::App app(options, sink);
    app.paint(1);
    const auto original = app.renderer.rgba();
    require(original.size() == static_cast<std::size_t>(fk::panel_width * fk::texture_height * 4),
            "RGBA size");
    app.apply({"international-full", "de-de", "midnight"});
    app.paint(2);
    require(app.renderer.rgba() != original, "theme and language affect rendered image");
    app.show_settings();
    app.paint(3);
    require(app.renderer.hit_key(app.view(), 100, 100) == nullptr, "settings prevent hidden key hits");
    auto click_control = [&](const std::string& id) {
        for (const auto& control : app.view().controls) {
            if (control.id == id) {
                const double x = control.bounds.x + control.bounds.width / 2;
                const double y = control.bounds.y + control.bounds.height / 2;
                app.down(0, x, y, 4);
                app.up(0, x, y);
                return;
            }
        }
        throw std::runtime_error("missing settings control " + id);
    };
    click_control("language");
    app.move(0, 300, 320);
    // Scroll the list until the choice shows, however many languages come first.
    auto shown = [&](const std::string& id) {
        const auto& controls = app.view().controls;
        return std::any_of(controls.begin(), controls.end(),
                           [&](const auto& control) { return control.id == id; });
    };
    for (int i = 0; i < 100 && !shown("choose:language:ja-romaji"); ++i) {
        app.scroll(0, 0, 1);
    }
    click_control("choose:language:ja-romaji");
    click_control("apply");
    require(app.selection().language == "ja-romaji", "dropdown activates Japanese language");
    require(sink.events.empty(), "preview settings do not emit input");
    rejects([&] { app.apply({"missing", "en-us", "graphite"}); }, "invalid selection rejected");
    require(app.selection().theme == "midnight", "failed apply retains prior selection");
}
} // namespace
int main() {
    try {
        const auto defaults = fk::load_profiles({}, {});
        require(defaults.errors.empty(), "bundled profiles valid");
        require(defaults.layouts.at("en-us-full").keys.size() == 109,
                "109 baseline keys, with the mic key");
        for (const auto& [id, layout] : defaults.layouts) {
            // The flick pages draw it as phones do, a tag with × in it.
            require(key(layout, "Backspace").icon ==
                        (id.starts_with("ja-flick") ? fk::Icon::DeleteLeft : fk::Icon::Backspace),
                    "Backspace uses the |<- icon, distinct from the left arrow");
        }
        state_tests(defaults.layouts.at("en-us-full"));
        profile_tests(defaults);
        key_area_tests(defaults);
        app_key_area_tests();
        app_tests();
        std::cout << "Key sequences, cancellation, repeat, profiles, persistence, language legends and "
                     "rendering passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
