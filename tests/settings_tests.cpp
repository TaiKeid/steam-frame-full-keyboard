#include "framekeyboard/app.hpp"
#include "framekeyboard/cjk.hpp"
#include "framekeyboard/key_sounds.hpp"
#include "framekeyboard/startup.hpp"
#include "framekeyboard/theme_previews.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
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
        std::string name = "/tmp/framekeyboard-settings-tests-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct CaptureSink : KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
Control control(const PanelView& view, const std::string& id) {
    for (const auto& c : view.controls) {
        if (c.id == id) {
            return c;
        }
    }
    throw std::runtime_error("missing control " + id);
}
bool has(const PanelView& view, const std::string& id) {
    return std::any_of(view.controls.begin(), view.controls.end(),
                       [&](const Control& c) { return c.id == id; });
}
// A Settings row wholly below its card's fold is left out of the view until
// the card scrolls: scroll each card down in turn until the row is there.
void reveal(App& app, const std::string& id, unsigned pointer = 0) {
    for (std::size_t i = 0; !has(app.view(), id) && app.view().settings && i < app.view().cards.size();
         ++i) {
        // Over the card's rows, where click() scrolls a card too, and within
        // the part of it on screen: the row of cards may have scrolled sideways.
        const auto card = app.view().cards[i].bounds;
        const double left = std::max(card.x, 0.0),
                     right = std::min(card.x + card.width, double(panel_width));
        if (right - left < 20) {
            continue;
        }
        app.move(pointer, (left + right) / 2, 300);
        app.scroll(pointer, 0, 20);
        static double now = 400;
        for (int step = 0; step < 40; ++step) {
            app.tick(now += .05);
        }
    }
}
// The control, after scrolling its card to it if it lies below the fold.
Control shown_control(App& app, const std::string& id) {
    reveal(app, id);
    return control(app.view(), id);
}
void click(App& app, const std::string& id, unsigned pointer = 0) {
    // A row below the fold of a card in view, first; then the cards cut
    // off at the right, which leave those at the left out once scrolled.
    reveal(app, id, pointer);
    if (!has(app.view(), id) && app.view().settings) {
        // The Integration card is cut off at the right until the row scrolls.
        app.move(pointer, 800, 300);
        app.scroll(pointer, 1000, 0);
        static double now = 100;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
    }
    reveal(app, id, pointer);
    auto c = control(app.view(), id);
    auto center_x = [](const Control& item) { return item.bounds.x + item.bounds.width / 2; };
    auto center_y = [](const Control& item) { return item.bounds.y + item.bounds.height / 2; };
    if (app.view().settings && !c.hit(center_x(c), center_y(c))) {
        // A card's lower rows sit below its fold until it scrolls.
        app.move(pointer, c.bounds.x, 300);
        app.scroll(pointer, 0, 20);
        static double now = 200;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
        c = control(app.view(), id);
    }
    const double x = center_x(c), y = center_y(c);
    require(c.hit(x, y), "test clicks a visible control");
    app.down(pointer, x, y, 1);
    app.up(pointer, x, y);
}
// A row of the Additional languages popup, by what it says.
const Control& languages_row(const PanelView& view, const std::string& label) {
    for (const auto& item : view.controls) {
        if (item.id.starts_with("languages-row-") && item.label == label) {
            return item;
        }
    }
    throw std::runtime_error("no row: " + label);
}
// Scrolls the languages list until the row shows.
void show_row(App& app, const std::string& label) {
    static double now = 500;
    const auto listed = [&] {
        const auto view = app.view();
        return std::any_of(view.controls.begin(), view.controls.end(), [&](const Control& item) {
            return item.id.starts_with("languages-row-") && item.label == label &&
                   item.bounds.y >= languages_list_bounds.y &&
                   item.bounds.y + item.bounds.height <=
                       languages_list_bounds.y + languages_list_bounds.height;
        });
    };
    for (int step = 0; step < 40 && !listed(); ++step) {
        app.move(0, 700, 300);
        app.scroll(0, 0, 1);
        for (int i = 0; i < 10; ++i) {
            app.tick(now += .05);
        }
    }
}
void tap_row(App& app, const std::string& label) {
    show_row(app, label);
    click(app, languages_row(app.view(), label).id);
}
void add_language(App& app, const std::string& layout, const std::string& language) {
    click(app, "additional-languages");
    tap_row(app, layout);
    tap_row(app, language);
    click(app, "languages-close");
}
// Opens the dropdown and scrolls until the choice is visible.
Control show_choice(App& app, const std::string& field, const std::string& choice) {
    click(app, field);
    const auto id = "choose:" + field + ":" + choice;
    for (int i = 0; i < 50 && !has(app.view(), id); ++i) {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, -100); // Start at the beginning, regardless of the selection.
        for (int j = 0; j < i && !has(app.view(), id); ++j) {
            app.scroll(0, 0, 1);
        }
    }
    return control(app.view(), id);
}
void select(App& app, const std::string& field, const std::string& choice) {
    if (field == "theme") {
        // Theme opens its picker: scroll its cards to the one, choose it, close.
        click(app, "theme");
        const auto id = "themes-card-" + choice;
        const auto& well = themes_well_bounds;
        app.move(0, well.x + well.width / 2, well.y + well.height / 2);
        app.scroll(0, 0, -100);
        for (int i = 0; i < 40 && !has(app.view(), id); ++i) {
            app.scroll(0, 0, 1);
            for (int j = 0; j < 20; ++j) {
                static double now = 600;
                app.tick(now += .02);
            }
        }
        click(app, id);
        app.back();
        return;
    }
    click(app, show_choice(app, field, choice).id);
}
bool choice_enabled(App& app, const std::string& field, const std::string& choice) {
    const bool enabled = show_choice(app, field, choice).enabled;
    app.back(); // Close the menu again.
    return enabled;
}
Settings saved(const fs::path& dir) {
    std::vector<std::string> errors;
    auto value = load_settings(dir, errors);
    require(errors.empty(), "saved config parses");
    return value;
}
Settings saved(App& app) {
    app.flush_settings();
    return saved(app.config_dir());
}
// A point on the key itself, not in the gap its area reaches into.
std::pair<double, double> key_point(App& app, const std::string& action) {
    const auto view = app.view();
    for (double y = 96; y < panel_height; y += 4) {
        for (double x = 0; x < panel_width; x += 4) {
            const auto* key = app.renderer.hit_key(view, x, y);
            const auto [lx, ly] = app.renderer.layout_point(view, x, y);
            if (key && key->action == action && key->bounds.contains(lx, ly)) {
                return {x, y};
            }
        }
    }
    throw std::runtime_error("missing visible key " + action);
}
void native_num_restore() {
    struct NativeSink : CaptureSink {
        std::optional<bool> lock;
        bool toggle_pending{};
        std::optional<bool> num_lock_state() override { return lock; }
        void send(int code, int value) override {
            CaptureSink::send(code, value);
            if (code == KEY_NUMLOCK && value == 1) {
                toggle_pending = true; // Feedback can arrive after multiple UI frames.
            }
        }
    };
    for (auto input : {InputBackend::Uinput, InputBackend::Ei}) {
        for (bool saved_num : {false, true}) {
            for (bool target_num : {false, true}) {
                TemporaryDirectory directory;
                Settings settings;
                settings.num_lock = saved_num;
                if (input == InputBackend::Ei) {
                    settings.active = {"ja-jis-full", "ja-jis", "graphite"};
                }
                save_settings(directory.path, settings);
                Options options;
                options.config_dir = directory.path;
                options.mode = Mode::Vr;
                options.input = input;
                options.target_language = settings.active.language;
                options.start_enabled = true;
                NativeSink sink;
                sink.lock = target_num;
                App app(options, sink);
                require(sink.events.empty(), "restoring native Num Lock sends no startup input");
                const auto [x, y] = key_point(app, "Numpad1");
                if (target_num != saved_num) {
                    require(!app.down(0, x, y, 1), "mismatched keypad waits for lock feedback");
                    require(!app.down(0, x, y, 1), "repeated press cannot undo pending lock toggle");
                    require(sink.events ==
                                std::vector<std::pair<int, int>>{{KEY_NUMLOCK, 1}, {KEY_NUMLOCK, 0}},
                            "native lock is reconciled exactly once");
                    sink.lock = saved_num;
                }
                sink.events.clear();
                require(app.down(0, x, y, 2), "keypad accepted once local and target locks agree");
                app.up(0, x, y);
                require(sink.events == std::vector<std::pair<int, int>>{{KEY_KP1, 1}, {KEY_KP1, 0}},
                        "native keypad preserves its physical code with matching Num Lock");
                sink.events.clear();
                sink.lock.reset();
                require(!app.down(0, x, y, 3) && sink.events.empty(),
                        "unknown target lock never sends a guessed keypad or toggle");
                for (const auto* action :
                     {"NumpadEnter", "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide"}) {
                    const auto [kx, ky] = key_point(app, action);
                    require(app.down(0, kx, ky, 3), "lock-independent keypad key remains usable");
                    app.up(0, kx, ky);
                    require(sink.events == std::vector<std::pair<int, int>>{{key_code(action), 1},
                                                                            {key_code(action), 0}},
                            "keypad controls do not require target Num Lock feedback");
                    sink.events.clear();
                }
                const auto [nx, ny] = key_point(app, "NumLock");
                require(app.down(0, nx, ny, 3), "native Num Lock toggles the saved local preference");
                app.up(0, nx, ny);
                require(sink.events.empty() && saved(app).num_lock == !saved_num,
                        "native local toggle is persisted without guessing a target lock");
                app.set_interaction_active(false);
                sink.lock = !saved_num;
                require(!app.down(0, x, y, 4) && sink.events.empty(),
                        "hidden input cannot synchronize target locks");
            }
        }
    }
}
void compact_toolbar() {
    for (bool favorites : {false, true}) {
        TemporaryDirectory directory;
        fs::create_directories(directory.path / "layouts");
        std::ofstream(directory.path / "layouts/compact.json")
            << R"({"schema_version":1,"id":"compact","name":"Compact regression","width":4096,"height":100,"keys":[{"id":"a","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}},{"id":"num1","label":"1","x":196,"y":0,"width":3900,"height":80,"action":{"kind":"key","value":"Numpad1"}}]})";
        Settings settings;
        settings.active.layout = "compact";
        settings.hide_numpad = true;
        if (favorites) {
            settings.favorites = {{"en", "English", {}}};
            // Replacement adds Keep open to the second row.
            settings.replace_native_keyboard = true;
        }
        save_settings(directory.path, settings);
        Options options;
        options.config_dir = directory.path;
        options.mode = favorites ? Mode::Vr : Mode::Render;
        options.input = InputBackend::Ei;
        CaptureSink sink;
        App app(options, sink);
        const auto view = app.view();
        const auto body = panel_case_bounds(view);
        for (std::size_t i = 0; i < view.controls.size(); ++i) {
            if (view.controls[i].style == ControlStyle::MoveHandle) {
                continue; // The movement handle deliberately sits below the case.
            }
            const auto& a = view.controls[i].bounds;
            require(body.contains(a.x, a.y) && body.contains(a.x + a.width - 1, a.y + a.height - 1),
                    "compact case contains every toolbar control");
            for (std::size_t j = i + 1; j < view.controls.size(); ++j) {
                const auto& b = view.controls[j].bounds;
                require(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y ||
                            b.y + b.height <= a.y,
                        "compact toolbar controls cannot overlap");
            }
        }
        require(!favorites ||
                    control(view, "keep-open").bounds.y == control(view, "passthrough").bounds.y,
                "compact Keep open shares the second row with Stay open over apps");
        click(app, favorites ? "language-cycle" : "settings");
        require(!app.quitting(), "compact Settings and language cycle cannot hit Close");
        require(favorites ? app.selection().layout == "en-us-full" : app.view().settings,
                "compact toolbar action reaches its intended destination");
    }
}
void startup_profile_fallback() {
    for (const auto& selection : {Selection{"missing-layout", "de-de", "graphite"},
                                  Selection{"international-full", "de-de", "missing-theme"}}) {
        TemporaryDirectory directory;
        Settings settings;
        settings.active = selection;
        save_settings(directory.path, settings);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        require(app.selection().language == "de-de" && app.selection().theme == "graphite",
                "startup fallback preserves the usable language and theme");
        require(selection.layout == "missing-layout" || app.selection().layout == selection.layout,
                "missing theme preserves the saved layout");
        const auto profiles = load_profiles({}, {});
        validate_selection(profiles, app.selection());
        click(app, "pin");
        const auto [x, y] = key_point(app, "NumLock");
        app.down(0, x, y, 1);
        app.up(0, x, y);
        const auto persisted = saved(app);
        require(persisted.active == selection && persisted.pinned && persisted.num_lock,
                "ordinary saves retain unavailable saved profiles and update preferences");
        require(!app.view().status.empty(), "fallback explanation survives ordinary saves");
        app.show_settings();
        click(app, "apply");
        require(saved(app).active == app.selection(), "explicit Apply commits the displayed fallback");
    }
}
void incompatible_saved_language() {
    for (const auto* id : {"xx-test", "en-us"}) {
        TemporaryDirectory directory;
        fs::create_directories(directory.path / "languages");
        const auto path = directory.path / "languages" / (std::string(id) + ".json");
        const auto json = std::string(R"({"schema_version":1,"id":")") + id +
                          R"(","name":"No compatible layout","locale":"en-US",
            "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
            "legends":{"source":"keymap","overrides":{}},"font_families":["sans-serif"],
            "required_keys":["Backquote","Convert"]})";
        std::ofstream(path) << json;
        const Selection selection{"en-us-full", id, "graphite"};
        save_selection(directory.path, selection);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        require(app.selection() == Selection{} && app.view().language->required_keys.empty(),
                "unmatchable language falls back to usable embedded English defaults");
        require(!app.view().status.empty(), "last-resort fallback explains the saved selection failure");
        click(app, "pin");
        require(saved(app).active == selection && saved(app).pinned,
                "last-resort fallback preserves saved profiles during ordinary saves");
        std::ifstream original(path);
        require(std::string(std::istreambuf_iterator<char>(original), {}) == json,
                "runtime recovery never rewrites the incompatible profile file");
        app.show_settings();
        require(app.view().settings, "Settings remains reachable to repair the saved profile");
    }
}
// The Korean 12-key layout (천지인) was removed; a saved one, the main
// language or an additional one, comes back as two-set Korean on US QWERTY
// and is gone from the file at the next save.
void removed_korean_12key() {
    TemporaryDirectory directory;
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"ko-12key","language":"ko-cheonjiin","theme":"graphite"},)"
           R"("favorites":[{"id":"a","name":"Korean 12-key","layout":"ko-12key","language":"ko-cheonjiin",)"
           R"("theme":"graphite"},{"id":"b","name":"German","layout":"international-full",)"
           R"("language":"de-de","theme":"graphite"},{"id":"c","name":"Korean","layout":"ko-12key",)"
           R"("language":"ko-kr","theme":"graphite"}]})";
    const Selection korean{"en-us-full", "ko-kr", "graphite"};
    std::vector<std::string> errors;
    const auto settings = load_settings(directory.path, errors);
    require(errors.empty() && settings.active == korean,
            "a saved 천지인 main language opens as two-set Korean");
    require(settings.favorites.size() == 3 && settings.favorites[0].selection == korean &&
                settings.favorites[1].selection.language == "de-de" &&
                settings.favorites[2].selection == korean,
            "so do its additional pairs, the others kept");
    if (!CjkComposer::supported("korean-2set")) {
        std::cout << "Removed Korean 12-key in the app skipped: Hangul engine absent.\n";
        return;
    }
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.selection() == korean && app.view().status.empty(),
            "the keyboard starts on two-set Korean without a complaint");
    click(app, "pin");
    app.flush_settings();
    std::ifstream file(directory.path / "config.json");
    const std::string saved_text(std::istreambuf_iterator<char>(file), {});
    require(saved_text.find("ko-12key") == std::string::npos &&
                saved_text.find("ko-cheonjiin") == std::string::npos,
            "the next save leaves 천지인 out of the file");
    app.show_settings();
    click(app, "additional-languages");
    std::vector<std::string> additional;
    const auto view = app.view();
    for (const auto& row : view.languages->additional) {
        additional.push_back(row.title);
    }
    require(additional == std::vector<std::string>{"German, QWERTZ"},
            "the additional languages hold no 천지인 row and no second Korean");
}
void missing_saved_language_status() {
    for (bool malformed : {false, true}) {
        TemporaryDirectory directory;
        if (malformed) {
            fs::create_directories(directory.path / "languages");
            std::ofstream(directory.path / "languages/broken.json") << "{broken";
        }
        const Selection selection{"en-us-full", "removed-language", "graphite"};
        save_selection(directory.path, selection);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        const auto message = "Saved language unavailable. Using English.";
        require(app.selection().language == "en-us" && app.view().status == message,
                "missing language reports its own readable fallback reason");
        click(app, "pin");
        require(saved(app).active == selection && app.view().status == message,
                "fallback reason and saved choice survive unrelated preference saves");
    }
}
void key_control_namespaces() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "layouts");
    std::ofstream(directory.path / "layouts/collision.json")
        << R"({"schema_version":1,"id":"collision","name":"Colliding IDs","width":300,"height":100,"keys":[
        {"id":"close","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}},
        {"id":"settings","label":"B","x":100,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyB"}},
        {"id":"apply","label":"C","x":200,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyC"}}]})";
    struct TextSink : CaptureSink {
        std::vector<std::string> text;
        bool text_available() override { return true; }
        bool commit_text(const std::string& value) override {
            text.push_back(value);
            return true;
        }
    } sink;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    App app(options, sink);
    app.apply({"collision", "en-us", "graphite"});
    app.paint(1);
    const auto idle = app.renderer.rgba();
    for (const auto* action : {"KeyA", "KeyB", "KeyC"}) {
        const auto [x, y] = key_point(app, action);
        require(app.move(0, x, y), "colliding key hover is still a keyboard target");
        const auto view = app.view();
        const auto id = app.renderer.hit_key(view, x, y)->id;
        require(view.hovered_keys.contains(id) && view.hovered_controls.empty(),
                "key hover does not select a same-named control");
        app.paint(1);
        const auto hovered = app.renderer.rgba();
        for (const auto& control : view.controls) {
            if (control.id != id) {
                continue;
            }
            for (int py = static_cast<int>(control.bounds.y);
                 py < control.bounds.y + control.bounds.height; ++py) {
                for (int px = static_cast<int>(control.bounds.x);
                     px < control.bounds.x + control.bounds.width; ++px) {
                    const auto offset =
                        static_cast<std::size_t>((py + popup_margin) * panel_width + px) * 4;
                    require(std::equal(idle.begin() + offset, idle.begin() + offset + 4,
                                       hovered.begin() + offset),
                            "colliding toolbar pixels stay idle");
                }
            }
        }
        require(app.down(0, x, y, 2), "colliding key press accepted");
        app.up(0, x, y);
        require(!app.quitting() && !app.view().settings, "key cannot invoke toolbar actions");
    }
    require(sink.text == std::vector<std::string>{"a", "b", "c"}, "colliding IDs type normally");
    const auto close = control(app.view(), "close");
    const auto [x, y] = key_point(app, "KeyA");
    app.down(0, close.bounds.x + 2, close.bounds.y + 2, 3);
    require(app.view().hovered_keys.empty() && app.view().hovered_controls.contains("close"),
            "control hover cannot highlight the same-named key");
    app.up(0, x, y);
    require(!app.quitting(), "releasing a toolbar press on a same-named key cannot activate it");
    click(app, "settings");
    require(app.view().settings, "real controls remain usable");
}
void favorite_cache_reload() {
    TemporaryDirectory directory;
    Settings settings;
    settings.favorites = {{"later", "Added later", {"later", "en-us", "graphite"}},
                          {"de", "German", {"international-full", "de-de", "graphite"}}};
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    click(app, "language-cycle");
    require(app.selection().language == "de-de", "cache skips absent favorite profile");
    click(app, "language-cycle");
    fs::create_directories(directory.path / "layouts");
    std::ofstream(directory.path / "layouts/later.json")
        << R"({"schema_version":1,"id":"later","name":"Added later","width":100,"height":100,"keys":[{"id":"a","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}}]})";
    app.reload();
    click(app, "language-cycle");
    require(app.selection().layout == "later", "reload adds newly valid cached favorite");
    app.apply({});
    app.flush_settings(); // This fixture exercises an immediate, idle Reload.
    fs::remove(directory.path / "layouts/later.json");
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de", "reload removes deleted cached favorite");
}
void background_preferences() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto unblock = release.get_future();
    std::vector<Settings> writes;
    auto app = std::make_unique<App>(options, sink, [&](const Settings& settings) {
        if (writes.empty()) {
            entered.set_value();
            unblock.wait(); // Model a stalled fsync without timing-dependent sleeps.
        }
        writes.push_back(settings);
        save_settings(directory.path, settings);
    });
    click(*app, "pin");
    if (started.wait_for(2s) != std::future_status::ready) {
        release.set_value();
        throw std::runtime_error("background save did not start");
    }
    auto input = std::async(std::launch::async, [&] {
        const auto [x, y] = key_point(*app, "NumLock");
        require(app->down(0, x, y, 1), "Num Lock accepts input during stalled save");
        app->up(0, x, y);
        click(*app, "pin");
        app->tick(1);
    });
    const bool responsive = input.wait_for(2s) == std::future_status::ready;
    release.set_value(); // Always unblock cleanup, even when the assertion fails.
    input.get();
    require(responsive, "input does not wait for blocked preference storage");
    app.reset(); // Destruction must drain the last pending snapshot.
    require(writes.size() == 2 && writes.front().pinned && !writes.back().pinned &&
                writes.back().num_lock,
            "ordered writes coalesce pending snapshots and shutdown saves the newest state");
    require(saved(directory.path).num_lock && !saved(directory.path).pinned,
            "shutdown makes the final preferences durable");

    // Failures reach App on its own thread; a later successful save clears them.
    bool fail = true;
    App errors(options, sink, [&](const Settings& settings) {
        if (fail) {
            throw std::runtime_error("test storage failure");
        }
        save_settings(directory.path, settings);
    });
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.find("Applied, but not saved: test storage failure") !=
                std::string::npos,
            "background failure reports unsaved runtime state");
    fail = false;
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.empty(), "a successful retry clears the prior save error");
    std::ofstream(directory.path / "config.json") << "{broken";
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.find("Applied, but not saved:") != std::string::npos,
            "atomic background save refuses malformed config");
    std::ifstream broken(directory.path / "config.json");
    require(std::string((std::istreambuf_iterator<char>(broken)), {}) == "{broken",
            "failed background save preserves malformed config bytes");

    std::promise<void> failing, retry;
    auto failure_started = failing.get_future();
    auto retry_ready = retry.get_future();
    int attempts = 0;
    SettingsWriter writer({}, [&](const Settings&) {
        if (++attempts == 1) {
            failing.set_value();
            retry_ready.wait();
            throw std::runtime_error("obsolete failure");
        }
    });
    writer.enqueue({});
    if (failure_started.wait_for(2s) != std::future_status::ready) {
        retry.set_value();
        throw std::runtime_error("failure test writer did not start");
    }
    writer.enqueue({});
    const bool suppressed = !writer.take_result();
    retry.set_value();
    writer.flush();
    require(suppressed, "pending retry suppresses obsolete completion errors");
    const auto result = writer.take_result();
    require(attempts == 2 && result && result->error.empty(),
            "newest successful snapshot supersedes an older failed write");
}
void deferred_reload() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    CaptureSink sink;
    std::promise<void> first_started, first_release, last_started, last_release;
    auto first_writing = first_started.get_future();
    auto first_unblock = first_release.get_future();
    auto last_writing = last_started.get_future();
    auto last_unblock = last_release.get_future();
    std::atomic<bool> timed_out{};
    std::atomic<int> writes{}, reconciliations{};
    App app(
        options, sink,
        [&](const Settings& settings) {
            const auto count = ++writes;
            if (count == 1) {
                first_started.set_value();
                if (first_unblock.wait_for(2s) != std::future_status::ready) {
                    timed_out = true;
                }
            } else if (count == 2) {
                last_started.set_value();
                if (last_unblock.wait_for(2s) != std::future_status::ready) {
                    timed_out = true;
                }
            }
            save_settings(directory.path, settings);
        },
        nullptr, [&](const Settings&) { ++reconciliations; });
    app.flush_settings();
    require(reconciliations == 1, "startup reconciliation completed before the reload fixture");
    app.apply({});
    require(first_writing.wait_for(2s) == std::future_status::ready, "first save started");
    app.frame_completed();
    app.show_settings();
    click(app, "reload");
    require(!timed_out, "Reload returns while the preference writer is blocked");
    for (int i = 0; i < 3; ++i) {
        app.tick(monotonic_seconds());
        app.paint(monotonic_seconds());
        require(app.slow_work_deadline() == 0,
                "waiting Reload keeps frames running without a slow-work allowance");
        app.frame_completed();
    }
    app.reload();
    app.reload(); // Repeated clicks coalesce while waiting.
    require(reconciliations == 1, "waiting reload never reconciles stale settings");
    app.back();
    click(app, "pin");
    app.apply({"en-us-full", "en-us", "midnight"});
    first_release.set_value();
    require(last_writing.wait_for(2s) == std::future_status::ready, "newest save started");
    app.frame_completed();
    app.tick(monotonic_seconds());
    require(!timed_out && reconciliations == 1 && app.slow_work_deadline() == 0,
            "Reload also waits without blocking for a newer queued snapshot");
    last_release.set_value();
    app.flush_settings();
    app.frame_completed();
    app.tick(monotonic_seconds());
    app.flush_settings();
    require(writes == 2 && reconciliations == 2,
            "all queued reload requests complete once after the newest save");
    require(app.pinned() && saved(directory.path).pinned &&
                saved(directory.path).active.theme == "midnight" && app.selection().theme == "midnight",
            "deferred Reload reads the final snapshot and preserves the active profile");
    app.frame_completed();
    app.tick(monotonic_seconds());
    app.flush_settings();
    require(reconciliations == 2 && !timed_out && sink.events.empty(),
            "completed Reload is not retried and sends no input");
}
void deferred_reload_errors() {
    using namespace std::chrono_literals;
    for (const bool storage_failure : {true, false}) {
        TemporaryDirectory directory;
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        CaptureSink sink;
        std::promise<void> entered, release;
        auto started = entered.get_future();
        auto unblock = release.get_future();
        std::atomic<bool> timed_out{};
        App app(
            options, sink,
            [&](const Settings& settings) {
                entered.set_value();
                timed_out = unblock.wait_for(2s) != std::future_status::ready;
                if (storage_failure) {
                    throw std::runtime_error("test deferred storage failure");
                }
                save_settings(directory.path, settings);
            },
            nullptr, [](const Settings&) {});
        app.flush_settings();
        app.apply({});
        require(started.wait_for(2s) == std::future_status::ready, "error fixture save started");
        app.reload();
        require(!timed_out, "error fixture Reload remains nonblocking");
        if (!storage_failure) {
            fs::create_directories(directory.path / "languages");
            std::ofstream(directory.path / "languages/en-us.json")
                << R"({"schema_version":1,"id":"en-us","name":"Missing physical key",
                       "locale":"en-US","keymap":{"rules":"evdev","model":"pc105",
                       "layout":"us","variant":"","options":[]},
                       "legends":{"source":"keymap","overrides":{}},
                       "font_families":["sans-serif"],
                       "required_keys":["IntlYen"]})";
        }
        release.set_value();
        app.flush_settings();
        app.frame_completed();
        app.tick(monotonic_seconds());
        app.flush_settings(); // A successful startup reconciliation is not a successful save.
        const auto status = app.view().status;
        require(status.find(storage_failure
                                ? "test deferred storage failure"
                                : "Language needs physical key IntlYen") != std::string::npos,
                "deferred Reload preserves save errors and reports preparation failures");
        app.paint(monotonic_seconds());
        app.frame_completed();
        app.tick(monotonic_seconds());
        require(!app.dirty && app.slow_work_deadline() == 0 && app.view().status == status &&
                    app.selection().language == "en-us" &&
                    app.view().language->name != "Missing physical key" && sink.events.empty(),
                "failed Reload keeps the old model and is not retried every frame");
    }
}
// One bad optional field falls back on its own when loaded. It must not stop
// every later save, or SteamOS startup; an unreadable file still does.
void preferences_with_a_bad_optional_field() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto units = directory.path / "units";
    const auto binary = directory.path / "install/releases/test/bin/framekeyboard";
    fs::create_directories(config);
    std::ofstream(config / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
           R"("theme":"graphite"},"haptic_effect":"z"})";
    std::vector<std::string> errors, field_errors;
    auto settings = load_settings(config, errors, &field_errors);
    require(errors.empty() && field_errors.size() == 1 && settings.haptic_effect == "e",
            "a bad optional field is a field error that falls back");
    settings.pinned = true;
    save_preferences_with_startup(settings, binary, config, units, [] {});
    errors.clear();
    field_errors.clear();
    const auto saved = load_settings(config, errors, &field_errors);
    require(errors.empty() && field_errors.empty() && saved.pinned && saved.haptic_effect == "e",
            "a save goes ahead and writes the field's fallback");
    std::ofstream(config / "config.json") << "{not json";
    bool refused = false;
    try {
        save_preferences_with_startup(settings, binary, config, units, [] {});
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).starts_with("Could not read saved preferences");
    }
    require(refused, "an unreadable file still blocks the save that would replace it");
}
void preferences_with_broken_startup() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto root = directory.path / "install";
    const auto binary = root / "releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
    fs::create_symlink("releases/test", root / "current");
    fs::create_directories(units);
    const std::string foreign = "# User-owned unit\n";
    std::ofstream(unit) << foreign;
    auto read = [](const fs::path& path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    Options options;
    options.mode = Mode::Render;
    options.config_dir = config;
    CaptureSink sink;
    App app(options, sink, [&](const Settings& settings) {
        save_preferences_with_startup(settings, binary, config, units, [] {});
    });
    click(app, "pin");
    require(saved(app).pinned && app.view().status.starts_with("Preferences saved."),
            "foreign startup unit does not block a pin save and reports a separate warning");
    const auto [x, y] = key_point(app, "NumLock");
    app.down(0, x, y, 1);
    app.up(0, x, y);
    require(saved(app).num_lock, "Num Lock also saves despite a startup error");
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    click(app, "auto-leveling");
    click(app, "replace-native-keyboard");
    click(app, "start-with-steamos");
    click(app, "apply");
    const auto partial = saved(app);
    require(partial.pinned && partial.num_lock && !partial.auto_leveling &&
                partial.favorites.size() == 1 && !partial.replace_native_keyboard &&
                !partial.start_with_steamos,
            "unit failure saves unrelated edits but retains both prior startup flags");
    require(app.view().status.starts_with("Other preferences saved.") && read(unit) == foreign &&
                !fs::is_symlink(link),
            "mixed-save warning identifies rejected startup changes without changing foreign unit");
    fs::remove(unit);
    app.show_settings();
    click(app, "apply");
    const auto complete = saved(app);
    require(complete.replace_native_keyboard && complete.start_with_steamos && fs::is_symlink(link) &&
                app.view().status.empty(),
            "retry commits startup flags and unit together and clears the warning");

    const auto previous_unit = read(unit);
    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_exec);
    app.show_settings();
    click(app, "start-with-steamos");
    click(app, "apply");
    app.flush_settings();
    fs::permissions(config, fs::perms::owner_all);
    require(app.view().status.starts_with("Applied, but not saved:") && saved(app).start_with_steamos &&
                read(unit) == previous_unit && fs::is_symlink(link),
            "storage failure still reports unsaved preferences and rolls startup back atomically");
    require(sink.events.empty(), "startup/save fixtures never send desktop input");

    // Development binaries also save ordinary edits when startup cannot be enabled.
    const auto development = directory.path / "development/framekeyboard";
    fs::create_directories(development.parent_path());
    std::ofstream(development) << "fixture\n";
    const auto private_config = directory.path / "development-config";
    Settings requested;
    requested.num_lock = requested.replace_native_keyboard = requested.start_with_steamos = true;
    bool warned = false;
    try {
        save_preferences_with_startup(requested, development, private_config,
                                      directory.path / "development-units", [] {});
    } catch (const StartupPreferencesWarning&) {
        warned = true;
    }
    const auto development_saved = saved(private_config);
    require(warned && development_saved.num_lock && !development_saved.replace_native_keyboard &&
                !development_saved.start_with_steamos,
            "unsupported release layout cannot block ordinary saves or persist startup flags");
}
void dictation_field_fallbacks() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    fs::create_directories(config);
    const std::string fields =
        R"("dictation":"server","dictation_server":"speech.example:8080",)"
        R"("dictation_url":"https://speech.example/v1","dictation_model":"whisper-1",)"
        R"("dictation_fallback":true,"dictation_device_model":"small-q4_0",)"
        R"("dictation_server_source":"shell","dictation_send_gap_ms":40,)"
        R"("dictation_send_paste":true,"dictation_direct":true,)"
        R"("dictation_recent_servers":["first.example:8080","second.example:8080"])";
    auto write = [&](std::string values) {
        std::ofstream(config / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full",)"
               R"("language":"en-us","theme":"graphite"},)"
            << values << '}';
    };
    // The reported pair of errors must not erase either block's valid neighbors.
    auto values = fields;
    values.replace(values.find("https://speech.example/v1"), 25, "https://bad url/v1");
    values.replace(values.find("gap_ms\":40") + 8, 2, "25");
    write(values);
    std::vector<std::string> errors, field_errors;
    auto loaded = load_settings(config, errors, &field_errors);
    auto preserved = [](const Settings& settings) {
        return settings.dictation == "server" && settings.dictation_server == "speech.example:8080" &&
               settings.dictation_model == "whisper-1" && settings.dictation_fallback &&
               settings.dictation_device_model == "small-q4_0" && settings.dictation_direct &&
               settings.dictation_send_paste && settings.dictation_server_by_shell &&
               settings.dictation_recent_servers ==
                   std::vector<std::string>{"first.example:8080", "second.example:8080"};
    };
    require(errors.empty() && preserved(loaded) && loaded.dictation_send_gap == 20 &&
                loaded.dictation_url.empty(),
            "bad URL and send gap preserve all valid dictation fields");
    require(field_errors.size() == 2 && field_errors[0].find("dictation_url") != std::string::npos &&
                field_errors[1].find("dictation_send_gap_ms") != std::string::npos,
            "each bad dictation field has its own diagnostic");
    save_preferences_with_startup(loaded, directory.path / "framekeyboard", config,
                                  directory.path / "units", [] {});
    errors.clear();
    field_errors.clear();
    loaded = load_settings(config, errors, &field_errors);
    require(errors.empty() && preserved(loaded) && loaded.dictation_send_gap == 20 &&
                field_errors.size() == 1 && field_errors[0].find("dictation_url") != std::string::npos,
            "saving and reloading retain valid dictation fields and the hand-edited URL");

    for (const auto& key :
         {"dictation", "dictation_server", "dictation_url", "dictation_model", "dictation_device_model",
          "dictation_fallback", "dictation_server_source", "dictation_send_gap_ms",
          "dictation_send_paste", "dictation_direct", "dictation_recent_servers"}) {
        values = fields;
        const auto begin = values.find(std::string("\"") + key + "\":") + std::string(key).size() + 3;
        const auto end = values[begin] == '[' ? values.find(']', begin) + 1 : values.find(',', begin);
        values.replace(begin, end == std::string::npos ? values.size() - begin : end - begin, "null");
        write(values);
        errors.clear();
        field_errors.clear();
        loaded = load_settings(config, errors, &field_errors);
        require(errors.empty() && field_errors.size() == 1 &&
                    field_errors.front().find(key) != std::string::npos,
                "a malformed dictation field reports only its own fallback");
        require(loaded.dictation == (std::string(key) == "dictation" ? "off" : "server") &&
                    loaded.dictation_device_model ==
                        (std::string(key) == "dictation_device_model" ? "small-q8_0" : "small-q4_0") &&
                    loaded.dictation_fallback == (std::string(key) != "dictation_fallback") &&
                    loaded.dictation_direct == (std::string(key) != "dictation_direct") &&
                    loaded.dictation_send_paste == (std::string(key) != "dictation_send_paste") &&
                    loaded.dictation_send_gap == (std::string(key) == "dictation_send_gap_ms" ? 20 : 40),
                "a malformed dictation field leaves its neighbors intact");
    }
    write(R"("dictation":"device","dictation_server":"bad server","dictation_url":"bad url")");
    errors.clear();
    field_errors.clear();
    loaded = load_settings(config, errors, &field_errors);
    require(errors.empty() && field_errors.size() == 2 && loaded.dictation == "device",
            "bad server fields do not disable on-device recognition");
    write(R"("dictation":"server","dictation_url":"bad url")");
    errors.clear();
    field_errors.clear();
    loaded = load_settings(config, errors, &field_errors);
    require(errors.empty() && field_errors.size() == 1 && loaded.dictation == "off",
            "a bad sole endpoint disables server recognition");
}
void reload_optional_field_errors() {
    TemporaryDirectory directory;
    Settings settings;
    settings.background_surface = "invisible";
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    CaptureSink sink;
    int reconciliations = 0;
    bool reconciled_hidden = false;
    App app(
        options, sink, [](const Settings&) {}, nullptr,
        [&](const Settings& updated) {
            ++reconciliations;
            reconciled_hidden = updated.hide_numpad;
        });
    app.flush_settings();
    require(app.view().case_opacity == 0 && !app.view().hide_numpad && reconciliations == 1,
            "reload fixture starts with a valid background and visible numpad");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full",)"
           R"("language":"en-us","theme":"graphite"},"hide_numpad":true,)"
           R"("background_surface":"bad"})";
    app.reload();
    app.flush_settings();
    require(app.view().hide_numpad && app.view().case_opacity == 1,
            "Reload applies a valid edit and the bad optional field's fallback");
    require(app.view().status.find("background_surface") != std::string::npos,
            "Reload reports optional field problems through the normal profile status");
    require(reconciliations == 2 && reconciled_hidden,
            "Reload reconciles preferences despite an optional field problem");
    std::ofstream(directory.path / "config.json") << "{broken";
    app.reload();
    app.flush_settings();
    require(app.view().hide_numpad && app.view().case_opacity == 1 && reconciliations == 2,
            "an unreadable reload retains preferences without reconciliation");
    require(sink.events.empty(), "reloading optional fallbacks sends no input");
}
void reload_startup() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto binary = directory.path / "install/releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "fixture\n";
    fs::create_symlink("releases/test", directory.path / "install/current");
    auto bytes = [&] {
        std::ifstream stream(config / "config.json");
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    Settings settings;
    settings.replace_native_keyboard = true;
    save_settings(config, settings);
    Options options;
    options.config_dir = config;
    options.mode = Mode::Vr;
    CaptureSink sink;
    int writes = 0, reconciliations = 0;
    bool fail = false;
    App app(
        options, sink, [&](const Settings&) { ++writes; }, nullptr,
        [&](const Settings& updated) {
            ++reconciliations;
            if (fail) {
                throw std::runtime_error("test manager unavailable");
            }
            configure_startup(updated, binary, config, units, [] {});
        });
    app.flush_settings();
    require(reconciliations == 1 && writes == 0 && !fs::exists(unit),
            "VR startup reconciles saved settings without writing JSON");
    const auto original = app.selection();
    const auto normal_status = app.view().status;
    settings.start_with_steamos = true;
    settings.active.layout = settings.active.language = "en-gb";
    save_settings(config, settings);
    auto before = bytes();
    app.reload();
    app.flush_settings();
    require(app.start_with_steamos() && fs::exists(unit) && fs::is_symlink(link),
            "Reload enables the startup unit for externally edited saved preferences");
    require(writes == 0 && reconciliations == 2 && bytes() == before &&
                app.selection().layout == original.layout &&
                app.selection().language == original.language,
            "Reload reconciles without saving JSON or activating an edited profile");
    settings.replace_native_keyboard = false;
    save_settings(config, settings);
    before = bytes();
    app.reload();
    app.flush_settings();
    require(!app.start_with_steamos() && !fs::exists(unit) && !fs::is_symlink(link) &&
                bytes() == before && writes == 0 && reconciliations == 3,
            "Reload also removes startup when replacement is disabled without changing JSON");
    settings.replace_native_keyboard = true;
    save_settings(config, settings);
    fail = true;
    app.reload();
    app.flush_settings();
    require(app.view().status == "SteamOS startup not configured: test manager unavailable" &&
                !fs::exists(unit),
            "Reload reports a startup reconciliation error distinctly from an unsaved config");
    fail = false;
    app.reload();
    app.flush_settings();
    require(app.view().status == normal_status && fs::exists(unit) && writes == 0 &&
                reconciliations == 5,
            "Reload retries reconciliation and clears its error");
    std::ofstream(config / "config.json") << "{broken";
    app.reload();
    app.flush_settings();
    require(reconciliations == 5 && bytes() == "{broken" && fs::exists(unit),
            "malformed external settings cannot reconfigure startup or be overwritten");
    require(sink.events.empty(), "startup reconciliation sends no input");
}
void launch_startup() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto binary = directory.path / "install/releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    const auto legacy = units / "gamescope-session.target.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "fixture\n";
    fs::create_symlink("releases/test", directory.path / "install/current");
    fs::create_directories(legacy.parent_path());
    const std::string old_unit = "# Managed by Full Keyboard\n[Service]\nRestart=on-failure\n";
    std::ofstream(unit) << old_unit;
    fs::create_symlink("../framekeyboard-autostart.service", legacy);
    Settings settings;
    settings.replace_native_keyboard = settings.start_with_steamos = true;
    save_settings(config, settings);
    auto read = [](const fs::path& path) {
        std::ifstream stream(path);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto before = read(config / "config.json");
    Options options;
    options.mode = Mode::Vr;
    options.config_dir = config;
    CaptureSink sink;
    int writes = 0, reloads = 0, reconciliations = 0;
    std::promise<void> entered, release;
    auto started = entered.get_future(), unblock = release.get_future();
    App app(
        options, sink, [&](const Settings&) { ++writes; }, nullptr,
        [&](const Settings& updated) {
            ++reconciliations;
            entered.set_value();
            unblock.wait();
            configure_startup(updated, binary, config, units, [&] { ++reloads; });
        });
    const bool queued = started.wait_for(2s) == std::future_status::ready;
    release.set_value(); // Always unblock destruction before asserting.
    app.flush_settings();
    require(queued && reconciliations == 1 && reloads == 1 && fs::is_symlink(link) &&
                !fs::is_symlink(legacy) && read(unit).find("Restart=always") != std::string::npos,
            "VR launch asynchronously migrates an owned older service without Apply or Reload");
    require(writes == 0 && read(config / "config.json") == before && sink.events.empty(),
            "startup migration preserves saved bytes and sends no input");
    auto reconcile = [&](const Settings& updated) {
        ++reconciliations;
        configure_startup(updated, binary, config, units, [&] { ++reloads; });
    };
    {
        App again(options, sink, {}, nullptr, reconcile);
        again.flush_settings();
        require(reconciliations == 2 && reloads == 1,
                "matching startup configuration skips manager reload on the next launch");
    }
    options.mode = Mode::Preview;
    {
        App preview(options, sink, {}, nullptr, reconcile);
        preview.flush_settings();
        require(reconciliations == 2, "preview launch never reconciles session startup");
    }
    options.mode = Mode::Vr;
    std::ofstream(config / "config.json") << "{broken";
    {
        App malformed(options, sink, {}, nullptr, reconcile);
        malformed.flush_settings();
        require(reconciliations == 2 && fs::is_symlink(link) &&
                    read(config / "config.json") == "{broken",
                "invalid saved settings cannot remove startup or be overwritten on launch");
    }
}
void numpad_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    const auto full_body = panel_case_bounds(app.view());
    require(full_body.width == panel_width && !app.view().keyboard->num(), "default numpad state");
    const auto [x, y] = key_point(app, "NumLock");
    require(app.down(0, x, y, 1), "Num Lock accepted");
    require(app.down(1, x, y, 1), "second Num Lock pointer accepted");
    require(app.view().keyboard->num() && saved(app).num_lock,
            "Num Lock saves once during simultaneous controller holds");
    app.up(0, x, y);
    app.up(1, x, y);
    {
        App reopened(options, sink);
        require(reopened.view().keyboard->num(), "Num Lock survives restart without target input");
    }
    app.show_settings();
    click(app, "hide-numpad");
    require(control(app.view(), "hide-numpad").selected && !saved(app).hide_numpad,
            "Hide numpad stays pending until Apply");
    app.back();
    app.show_settings();
    require(!control(app.view(), "hide-numpad").selected, "Back discards Hide numpad edits");
    click(app, "hide-numpad");
    click(app, "apply");
    require(saved(app).hide_numpad && app.view().hide_numpad, "Apply saves global numpad visibility");
    const auto narrow = panel_case_bounds(app.view());
    require(narrow.width < full_body.width && narrow.x > 0, "hidden numpad narrows centered case");
    app.paint(2);
    const auto pixels = app.renderer.rgba();
    require(pixels[static_cast<std::size_t>((300 + popup_margin) * panel_width + 20) * 4 + 3] == 0,
            "trimmed case pixels are transparent");
    const auto hidden_view = app.view();
    for (double yy = 96; yy < panel_height; yy += 4) {
        for (double xx = 0; xx < panel_width; xx += 4) {
            const auto* key = app.renderer.hit_key(hidden_view, xx, yy);
            require(!key || (key->action != "NumLock" && !key->action.starts_with("Numpad")),
                    "hidden numpad has no hit regions");
        }
    }
    const auto close = control(app.view(), "close");
    require(close.bounds.x + close.bounds.width < narrow.x + narrow.width,
            "close button follows narrower case");
    const auto profiles = load_profiles({}, {});
    for (const auto& [id, layout] : profiles.layouts) {
        if (layout.companion) {
            continue; // Another layout's English page, never chosen by itself.
        }
        app.apply({id, layout.language.empty() ? "en-us" : layout.language, "graphite"});
        require(app.view().hide_numpad && app.view().keyboard->num() &&
                    panel_case_bounds(app.view()).width < full_body.width,
                "global visibility and Num Lock survive every bundled layout change");
    }
    app.apply({"en-us-full", "en-us", "graphite"});
    auto settings = saved(app);
    settings.favorites = {{"de", "German", {"international-full", "de-de", "graphite"}}};
    save_settings(directory.path, settings);
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de" && app.view().hide_numpad && app.view().keyboard->num(),
            "favorites keep global numpad state");
    {
        App reopened(options, sink);
        require(reopened.view().hide_numpad && reopened.view().keyboard->num(),
                "both numpad settings survive restart");
    }
    app.show_settings();
    click(app, "hide-numpad");
    click(app, "apply");
    require(panel_case_bounds(app.view()).width == full_body.width && app.view().keyboard->num(),
            "showing numpad restores width and prior Num Lock state");
    const auto [nx, ny] = key_point(app, "NumLock");
    app.down(0, nx, ny, 3);
    app.up(0, nx, ny);
    require(!saved(app).num_lock, "Num Lock off is persisted too");
    require(sink.events.empty(), "numpad settings tests deliver no desktop input");
    app.paint(3);
    app.renderer.write_png("/tmp/framekeyboard-numpad-visible.png");
    app.show_settings();
    app.paint(4);
    app.renderer.write_png("/tmp/framekeyboard-numpad-settings.png");
    click(app, "hide-numpad");
    click(app, "apply");
    app.paint(5);
    app.renderer.write_png("/tmp/framekeyboard-numpad-hidden.png");
}
void legacy_background_surface_tests() {
    TemporaryDirectory directory;
    {
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        options.hosted_fd = 99; // App only needs to know it is hosted.
        CaptureSink sink;
        App app(options, sink);
        auto values =
            json_ptr(json_tokener_parse(R"({"background_surface":"disabled","hide_numpad":true})"));
        require(app.host_settings_set(values.get(), 1).first.empty(),
                "protocol 1 accepts the legacy disabled background surface");
        const auto settings = app.host_settings();
        require(std::string(json_object_get_string(
                    json_object_object_get(settings.get(), "background_surface"))) == "invisible" &&
                    app.view().case_opacity == 0 && app.view().hide_numpad,
                "the legacy surface becomes invisible and other settings in the request apply");
        app.flush_settings();
        std::vector<std::string> errors;
        const auto saved = load_settings(directory.path, errors);
        require(errors.empty() && saved.background_surface == "invisible" && saved.hide_numpad,
                "the legacy surface saves in canonical form");
        require(sink.events.empty(), "legacy settings compatibility sends no input");
    }
}

void background_surface_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(saved(app).background_surface == "visible" && app.view().case_opacity == 1,
            "the case is visible by default");
    app.show_settings();
    const auto theme = control(app.view(), "theme");
    const auto surface = control(app.view(), "background-surface");
    require(surface.bounds.y >= theme.bounds.y + theme.bounds.height &&
                surface.bounds.y < control(app.view(), "hide-numpad").bounds.y,
            "Background sits below Theme");
    require(surface.label == "Visible", "the dropdown shows the saved choice");
    select(app, "background-surface", "invisible");
    require(control(app.view(), "background-surface").label == "Invisible" &&
                saved(app).background_surface == "visible" && app.view().case_opacity == 1,
            "the choice stays pending until Apply");
    app.back();
    app.show_settings();
    require(control(app.view(), "background-surface").label == "Visible",
            "Back discards the background surface edit");
    // Semi-transparent: a half-opaque case that takes the laser everywhere.
    select(app, "background-surface", "semi-transparent");
    click(app, "apply");
    require(saved(app).background_surface == "semi-transparent" && app.view().case_opacity == .5,
            "Apply saves Semi-transparent");
    app.paint(1.2);
    require(
        std::abs(app.renderer
                     .rgba()[static_cast<std::size_t>((300 + popup_margin) * panel_width + 10) * 4 + 3] -
                 128) <= 1,
        "a semi-transparent case is drawn at half opacity");
    app.show_settings();
    // Invisible: a clear case that still takes the laser everywhere.
    select(app, "background-surface", "invisible");
    click(app, "apply");
    require(saved(app).background_surface == "invisible" && app.view().case_opacity == 0,
            "Apply saves Invisible");
    app.paint(1.5);
    require(
        app.renderer.rgba()[static_cast<std::size_t>((300 + popup_margin) * panel_width + 10) * 4 + 3] ==
            0,
        "an invisible case leaves no pixels behind the keys");
    {
        const auto pixels = app.renderer.rgba();
        const auto [kp, kq] = key_point(app, "KeyQ");
        require(pixels[static_cast<std::size_t>((static_cast<int>(kq) + popup_margin) * panel_width +
                                                static_cast<int>(kp)) *
                           4 +
                       3] > 0,
                "keys keep their theme over a transparent case");
    }
    app.renderer.write_png("/tmp/framekeyboard-transparent-background.png");
    // Disabled, which let the laser through the case's gaps, is gone.
    app.show_settings();
    click(app, "background-surface");
    require(has(app.view(), "choose:background-surface:invisible") &&
                !has(app.view(), "choose:background-surface:disabled"),
            "Background offers no Disabled choice");
    app.back();
    app.back();
    // A message shows on an opaque card above the case, centred on it, as
    // the focus tooltip does; the transparent case under it stays clear.
    {
        app.report_status(ui("status.num-lock-wait"));
        app.paint(2.5);
        const auto pixels = app.renderer.rgba();
        auto alpha = [&](int px, int py) {
            return pixels[static_cast<std::size_t>((py + popup_margin) * panel_width + px) * 4 + 3];
        };
        require(alpha(800, -37) == 255 && alpha(40, -37) == 0 && alpha(800, 74) == 0,
                "a message sits on a card above the case, fitted to its text");
        app.renderer.write_png("/tmp/framekeyboard-transparent-status.png");
        app.report_status({});
    }
    // Settings keeps its case, or its cards would float on the dashboard.
    app.show_settings();
    app.paint(3);
    {
        const auto pixels = app.renderer.rgba();
        require(pixels[static_cast<std::size_t>((300 + popup_margin) * panel_width + 10) * 4 + 3] == 255,
                "Settings keeps an opaque case");
    }
    app.back();
    {
        App reopened(options, sink);
        require(reopened.view().case_opacity == 0 && saved(reopened).background_surface == "invisible",
                "the background surface survives restart");
        reopened.paint(4);
        const auto pixels = reopened.renderer.rgba();
        require(pixels[static_cast<std::size_t>((300 + popup_margin) * panel_width + 10) * 4 + 3] == 0,
                "a restarted keyboard stays transparent");
    }
    app.show_settings();
    select(app, "background-surface", "visible");
    click(app, "apply");
    require(saved(app).background_surface == "visible" && app.view().case_opacity == 1,
            "Apply restores the visible case");
    app.paint(5);
    {
        const auto pixels = app.renderer.rgba();
        require(pixels[static_cast<std::size_t>((300 + popup_margin) * panel_width + 10) * 4 + 3] == 255,
                "Visible fills the case again");
    }
    require(sink.events.empty(), "background surface tests deliver no desktop input");
    {
        // A hand-edited value fails on its own, leaving the case visible.
        TemporaryDirectory edited;
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
               R"("theme":"graphite"},"hide_numpad":true,"background_surface":"clear"})";
        std::vector<std::string> errors;
        const auto loaded = load_settings(edited.path, errors);
        require(loaded.background_surface == "visible" && loaded.hide_numpad && errors.size() == 1,
                "an unknown background surface is reported and the rest still loads");
    }
    {
        // A saved Disabled, now removed, becomes Invisible, which looks the same.
        TemporaryDirectory edited;
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
               R"("theme":"graphite"},"background_surface":"disabled"})";
        std::vector<std::string> errors;
        const auto loaded = load_settings(edited.path, errors);
        require(loaded.background_surface == "invisible" && errors.empty(),
                "a saved Disabled background loads as Invisible");
    }
    // The reading line of a composing language: a plate behind its hint.
    {
        TemporaryDirectory japanese;
        Settings settings;
        settings.active = {"ja-jis-full", "ja-romaji", "graphite"};
        settings.background_surface = "invisible";
        save_settings(japanese.path, settings);
        Options composing = options;
        composing.config_dir = japanese.path;
        App reading(composing, sink);
        require(reading.view().composing, "romaji input composes");
        reading.paint(6);
        const auto pixels = reading.renderer.rgba();
        auto alpha = [&](double px, double py) {
            return pixels[static_cast<std::size_t>((static_cast<int>(py) + popup_margin) * panel_width +
                                                   static_cast<int>(px)) *
                              4 +
                          3];
        };
        const auto line = ime_band(reading.view()).band;
        const double middle = line.y + line.height / 2;
        require(alpha(line.x + line.width / 2, middle) >= static_cast<int>(case_plate_alpha * 255) &&
                    alpha(line.x + 4, middle) == 0,
                "the reading line's hint sits on a plate fitted to it");
        reading.renderer.write_png("/tmp/framekeyboard-transparent-reading.png");
    }
}
// Pull for Shift: off until chosen, right after swipe typing, saved by Apply.
void key_flicks_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!saved(app).key_flicks, "key flicks start off");
    app.show_settings();
    const auto flicks = control(app.view(), "key-flicks"), swipe = control(app.view(), "swipe-typing");
    require(!flicks.selected && flicks.bounds.y > swipe.bounds.y && flicks.bounds.x == swipe.bounds.x,
            "the option follows swipe typing, unchecked");
    click(app, "key-flicks");
    require(control(app.view(), "key-flicks").selected && !saved(app).key_flicks,
            "it stays pending until Apply");
    click(app, "apply");
    require(saved(app).key_flicks, "Apply saves it");
    // The toolbar's own switch pauses flicks, saved as it changes.
    require(control(app.view(), "key-flicks-toggle").icon == Icon::KeyFlick,
            "with the option on, the toolbar has a key flick switch, on");
    click(app, "key-flicks-toggle");
    require(control(app.view(), "key-flicks-toggle").icon == Icon::KeyFlickOff &&
                !saved(app).key_flicks_switch && saved(app).key_flicks,
            "a tap pauses flicks and saves that, leaving the option as it is");
    {
        App restarted(options, sink);
        require(control(restarted.view(), "key-flicks-toggle").icon == Icon::KeyFlickOff,
                "the switch stays off after a restart");
    }
    app.show_settings();
    click(app, "key-flicks");
    click(app, "apply");
    const auto controls = app.view().controls;
    require(std::none_of(controls.begin(), controls.end(),
                         [](const Control& c) { return c.id == "key-flicks-toggle"; }),
            "with the option off, the switch is gone");
}
void extended_key_collision_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(saved(app).extended_key_collision && app.input_view().key_areas,
            "Extended key collision starts on");
    app.show_settings();
    const auto row = control(app.view(), "extended-key-collision");
    const auto unicode = control(app.view(), "desktop-unicode");
    const auto labels = app.view().field_labels;
    const auto label = std::find_if(labels.begin(), labels.end(), [](const FieldLabel& l) {
        return l.text == "Extended key collision";
    });
    require(row.selected && row.bounds.y > unicode.bounds.y && label != labels.end() &&
                label->bounds.y > unicode.bounds.y,
            "it ends the Typing card, checked");
    click(app, "extended-key-collision");
    require(!control(app.view(), "extended-key-collision").selected && saved(app).extended_key_collision,
            "it stays pending until Apply");
    app.back();
    app.show_settings();
    require(control(app.view(), "extended-key-collision").selected, "Back discards it");
    click(app, "extended-key-collision");
    click(app, "apply");
    require(!saved(app).extended_key_collision && !app.input_view().key_areas,
            "Apply saves it and gaps are no keys again");
    App restarted(options, sink);
    require(!restarted.input_view().key_areas, "it stays off after a restart");
}
// Whether Display key collision drew its green line at a layout point.
bool green_at(App& app, double x, double y) {
    const auto view = app.view();
    const auto [left, top] = app.renderer.layout_point(view, 0, 0);
    const auto [right, bottom] = app.renderer.layout_point(view, 1000, 1000);
    const int px = static_cast<int>((x - left) * 1000 / (right - left));
    const int py = static_cast<int>((y - top) * 1000 / (bottom - top)) + popup_margin;
    const auto pixels = app.renderer.rgba();
    for (int dx = -2; dx <= 2; ++dx) {
        const auto at =
            (static_cast<std::size_t>(py) * panel_width + static_cast<std::size_t>(px + dx)) * 4;
        if (pixels[at + 1] > 200 && pixels[at] < 100 && pixels[at + 2] < 150) {
            return true;
        }
    }
    return false;
}
void debug_card_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    // Cards past the right edge are left out of the view until scrolled to.
    auto last_card = [](App& app) {
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        static double now = 300;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
        return app.view().cards.back().title;
    };
    {
        App app(options, sink);
        app.show_settings();
        require(last_card(app) == "Integration" && !has(app.view(), "debug-key-collision"),
                "without debug mode Settings has no Debug card");
    }
    options.debug = true;
    App app(options, sink);
    app.show_settings();
#ifdef FRAMEKEYBOARD_DEBUG_MODE
    require(last_card(app) == "Debug", "debug mode adds a Debug card, last");
    require(control(app.view(), "debug-mark-anthy").selected &&
                !control(app.view(), "debug-key-collision").selected,
            "Mark Anthy suggestions starts on, Display key collision off");
    click(app, "debug-key-collision");
    app.back();
    require(!app.view().key_outlines, "Back discards a Debug switch");
    app.show_settings();
    last_card(app);
    require(!control(app.view(), "debug-key-collision").selected, "and shows it as it was");
    click(app, "debug-key-collision");
    click(app, "apply");
    require(app.view().key_outlines, "Apply turns it on");
    app.paint(10);
    // Q and W meet 3 units right of Q, which ends at 265.
    require(green_at(app, 268, 170) && !green_at(app, 240, 170),
            "a green line runs between Q and W, not across them");
    std::ifstream file(directory.path / "config.json");
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    require(text.find("debug") == std::string::npos && text.find("outline") == std::string::npos,
            "the Debug card saves nothing");
    App restarted(options, sink);
    require(!restarted.view().key_outlines, "a restart turns it off again");
#else
    require(last_card(app) == "Integration", "a release build has no Debug card at all");
#endif
}
// The interface language (docs/localization-design.md): the system's, with
// its fonts for interface text, followed when Settings opens and on Reload;
// the Debug card picks one for the run without saving it.
void interface_language_setting() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "locales");
    std::ofstream(directory.path / "locales" / "xt.json")
        << R"({"locale": "xt", "name": "Testish", "posix": ["xt"],
               "font_families": ["Test Sans", "sans-serif"], "strings": {}})";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    {
        App app(options, sink);
        require(app.interface_language() == "en" && app.view().interface_font == "Noto Sans,sans-serif",
                "with no system language, English");
    }
    std::string posix = "xt_XT.UTF-8";
    options.system_language = [&] { return SystemLanguage{"", posix}; };
    {
        App app(options, sink);
        require(app.interface_language() == "xt" && app.view().interface_font == "Test Sans,sans-serif",
                "the system's language, with its fonts for interface text");
        require(app.view().language->font == "Noto Sans,sans-serif",
                "key legends keep the typing language's");
        app.show_settings();
        std::vector<LabelFit> fits;
        record_label_fits(&fits);
        app.paint(10);
        record_label_fits(nullptr);
        const auto title = app.view().cards.front().title;
        require(std::any_of(fits.begin(), fits.end(),
                            [&](const LabelFit& fit) {
                                return fit.text == title && fit.family == "Test Sans,sans-serif" &&
                                       fit.scale == 1 && fit.lines == 1 && !fit.cut;
                            }),
                "card titles in the interface language's fonts, each label's fit recorded");
        app.back();
        posix = "C.UTF-8";
        require(app.interface_language() == "xt", "nothing changes while Settings is closed");
        app.show_settings();
        require(app.interface_language() == "en" && app.view().interface_font == "Noto Sans,sans-serif",
                "opening Settings follows a changed system language");
        std::ofstream(directory.path / "locales" / "xu.json")
            << R"({"locale": "xu", "name": "Unish", "posix": ["xu"], "strings": {}})";
        posix = "xu_XU";
        click(app, "reload");
        require(app.interface_language() == "xu", "Reload finds a new language file");
    }
    posix = "xt";
    auto last_card = [](App& app) {
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        static double now = 300;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
        return app.view().cards.back().title;
    };
    options.debug = true;
    App app(options, sink);
    app.show_settings();
#ifdef FRAMEKEYBOARD_DEBUG_MODE
    last_card(app);
    require(control(app.view(), "debug-interface-language").label.find("System") != std::string::npos,
            "the Debug card follows the system");
    select(app, "debug-interface-language", "en");
    app.back();
    require(app.interface_language() == "xt", "Back discards the choice");
    app.show_settings();
    last_card(app);
    select(app, "debug-interface-language", "qps");
    click(app, "apply");
    require(app.interface_language() == "qps", "Apply takes it: the pseudo-language");
    app.show_settings();
    require(app.interface_language() == "qps", "and Settings opening keeps it");
    std::ifstream file(directory.path / "config.json");
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    require(text.find("qps") == std::string::npos && text.find("interface") == std::string::npos,
            "nothing saved");
    App restarted(options, sink);
    require(restarted.interface_language() == "xt", "a restart follows the system again");
#else
    require(last_card(app) == "Integration", "a release build has no Debug card at all");
#endif
}
void haptic_sounds_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(saved(app).haptic_sounds == "off" && !app.haptic_sounds() && !app.pops(KeyFeedback::Press) &&
                !app.pops(KeyFeedback::Release),
            "haptic sounds start off");
    app.show_settings();
    require(control(app.view(), "haptic-sounds").label == "Off" &&
                control(app.view(), "haptic-sounds").bounds.x >
                    control(app.view(), "hide-numpad").bounds.x &&
                control(app.view(), "haptic-sounds").bounds.y <
                    control(app.view(), "emoji-close-on-select").bounds.y &&
                control(app.view(), "emoji-close-on-select").bounds.y <
                    control(app.view(), "desktop-unicode").bounds.y,
            "the Typing card, right of General, leads with the sounds");
    select(app, "haptic-sounds", "press");
    require(saved(app).haptic_sounds == "off" && !app.pops(KeyFeedback::Press),
            "the choice stays pending until Apply");
    click(app, "apply");
    require(saved(app).haptic_sounds == "press" && app.haptic_sounds() && app.pops(KeyFeedback::Press) &&
                !app.pops(KeyFeedback::Release),
            "Press pops on press only");
    app.show_settings();
    select(app, "haptic-sounds", "release");
    click(app, "apply");
    require(!app.pops(KeyFeedback::Press) && app.pops(KeyFeedback::Release),
            "Release pops on release only");
    app.show_settings();
    select(app, "haptic-sounds", "both");
    click(app, "apply");
    require(app.pops(KeyFeedback::Press) && app.pops(KeyFeedback::Release) &&
                !app.pops(KeyFeedback::Hover),
            "Both pops on press and release, and hovering never pops");
    {
        TemporaryDirectory edited;
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
               R"("theme":"graphite"},"hide_numpad":true,"haptic_sounds":"loud"})";
        std::vector<std::string> errors;
        const auto loaded = load_settings(edited.path, errors);
        require(loaded.haptic_sounds == "off" && loaded.hide_numpad && errors.size() == 1,
                "an unknown value is reported and the rest still loads");
    }
    // The effect and volume rows show only with sounds on, and each click previews.
    app.show_settings();
    require(has(app.view(), "haptic-effect:a") && has(app.view(), "haptic-volume:100") &&
                control(app.view(), "haptic-effect:e").selected &&
                control(app.view(), "haptic-volume:75").selected && !app.take_sound_preview(),
            "E and 75% start selected, and opening Settings plays nothing");
    click(app, "haptic-effect:c");
    auto preview = app.take_sound_preview();
    require(preview && preview->press && preview->release && preview->effect == "c" &&
                preview->volume == 75 && control(app.view(), "haptic-effect:c").selected &&
                saved(app).haptic_effect == "e" && app.haptic_effect() == "e",
            "Switching effect previews it with Both, and stays pending until Apply");
    click(app, "haptic-effect:c");
    require(app.take_sound_preview().has_value(), "Clicking the selected effect previews it again");
    click(app, "haptic-volume:75");
    app.take_sound_preview();
    app.paint(5);
    app.renderer.write_png("/tmp/framekeyboard-haptic-settings.png");
    click(app, "haptic-volume:25");
    preview = app.take_sound_preview();
    require(preview && preview->effect == "c" && preview->volume == 25,
            "Volume previews the pending effect");
    select(app, "haptic-sounds", "release");
    require(!app.take_sound_preview(), "the mode dropdown itself plays nothing");
    click(app, "haptic-volume:75");
    preview = app.take_sound_preview();
    require(preview && !preview->press && preview->release, "Release previews release only");
    select(app, "haptic-sounds", "press");
    click(app, "haptic-volume:50");
    preview = app.take_sound_preview();
    require(preview && preview->press && !preview->release, "Press previews press only");
    click(app, "apply");
    require(saved(app).haptic_effect == "c" && saved(app).haptic_volume == 50 &&
                app.haptic_effect() == "c" && app.haptic_volume() == 50,
            "Apply saves the effect and volume");
    app.show_settings();
    select(app, "haptic-sounds", "off");
    require(!has(app.view(), "haptic-effect:a") && !has(app.view(), "haptic-volume:100"),
            "both rows hide with sounds off");
    require(!app.haptic_sounds(), "the pending Off choice also closes audio while in Settings");
    app.back();
    require(app.haptic_sounds(), "Back restores the saved choice");
    // The toolbar button: gone with sounds off, otherwise a switch of its own.
    app.show_settings();
    select(app, "haptic-sounds", "off");
    click(app, "apply");
    require(!has(app.view(), "sounds-toggle") && !app.pops(KeyFeedback::Press),
            "with sounds off there is no toolbar button");
    app.show_settings();
    select(app, "haptic-sounds", "both");
    click(app, "apply");
    require(control(app.view(), "sounds-toggle").enabled &&
                control(app.view(), "sounds-toggle").icon == Icon::HapticSound &&
                app.pops(KeyFeedback::Press),
            "with sounds on it shows them on");
    app.paint(6);
    app.renderer.write_png("/tmp/framekeyboard-haptic-toolbar.png");
    click(app, "sounds-toggle");
    app.flush_settings();
    require(control(app.view(), "sounds-toggle").icon == Icon::HapticSoundOff &&
                !app.pops(KeyFeedback::Press) && !app.pops(KeyFeedback::Release) &&
                !app.haptic_sounds() && !saved(app).haptic_sounds_switch &&
                saved(app).haptic_sounds == "both",
            "it mutes the keys, saved on its own, and leaves the setting alone");
    app.show_settings();
    require(!app.haptic_sounds(), "Settings cannot open audio while the toolbar is muted");
    click(app, "haptic-effect:a");
    require(!app.take_sound_preview(), "muted Settings choices do not queue a later preview");
    app.back();
    click(app, "sounds-toggle");
    app.flush_settings();
    require(app.pops(KeyFeedback::Press), "it turns back on");
    {
        TemporaryDirectory edited;
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
               R"("theme":"graphite"},"haptic_effect":"z","haptic_volume":60})";
        std::vector<std::string> errors;
        const auto loaded = load_settings(edited.path, errors);
        require(loaded.haptic_effect == "e" && loaded.haptic_volume == 75 && errors.size() == 2,
                "bad values are reported and fall back");
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"de-de",)"
               R"("theme":"graphite"},"haptic_volume":"75"})";
        errors.clear();
        const auto typed = load_settings(edited.path, errors);
        require(typed.active.language == "de-de" && typed.haptic_volume == 75 && errors.size() == 1,
                "a volume of the wrong type fails on its own");
    }
    // The pops themselves: short, level with each other, and release higher.
    const int rate = 48000;
    auto crossings = [](const std::vector<float>& samples) {
        int count = 0;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            count += (samples[i - 1] < 0) != (samples[i] < 0);
        }
        return count;
    };
    for (const std::string effect : {"a", "b", "c", "d", "e", "f"}) {
        const auto press = key_sound(KeyFeedback::Press, rate, effect);
        const auto release = key_sound(KeyFeedback::Release, rate, effect);
        for (const auto* sound : {&press, &release}) {
            float peak = 0;
            for (const float sample : *sound) {
                peak = std::max(peak, std::abs(sample));
            }
            require(sound->size() <= static_cast<std::size_t>(rate / 10) && peak > .27f && peak < .29f,
                    "every pop is short and peaks alike");
            require(std::abs(sound->back()) < .001f, "a pop ends at silence, so its end can't click");
        }
        require(crossings(release) > crossings(press), "release pops higher than press");
    }
    require(key_sound(KeyFeedback::Press, rate, "a") != key_sound(KeyFeedback::Press, rate, "b") &&
                key_sound(KeyFeedback::Press, rate, "b") != key_sound(KeyFeedback::Press, rate, "c"),
            "effects differ");
    require(key_sound(KeyFeedback::Hover, rate, "a").empty(), "hovering has no sound");
    for (const auto& [plain, clicked] : {std::pair{"a", "b"}, {"c", "d"}, {"e", "f"}}) {
        require(std::abs(key_sound(KeyFeedback::Press, rate, plain).front()) < .01f &&
                    std::abs(key_sound(KeyFeedback::Press, rate, clicked).front()) > .01f,
                "effects come in pairs, plain and then with a click");
    }
    require(sound_gain(100) == 1 && std::abs(sound_gain(75) - .62f) < .01f &&
                std::abs(sound_gain(50) - .315f) < .01f && std::abs(sound_gain(25) - .1f) < .01f,
            "volume steps follow loudness, about 10 dB for each halving");
}
void vibration_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.vibration() && saved(app).vibration, "key vibration starts on");
    app.show_settings();
    require(has(app.view(), "vibration") && control(app.view(), "vibration").selected,
            "the Typing card shows the vibration checkbox, checked");
    click(app, "vibration");
    require(app.vibration() && saved(app).vibration && !control(app.view(), "vibration").selected,
            "the choice stays pending until Apply");
    click(app, "apply");
    require(!app.vibration() && !saved(app).vibration, "Apply saves it off");
    // Back discards a pending edit; the host applies the flag at once.
    app.show_settings();
    click(app, "vibration");
    app.back();
    require(!app.vibration() && !saved(app).vibration, "Back discards the pending change");
    {
        auto flag = json_ptr(json_tokener_parse(R"({"vibration":true})"));
        require(app.host_settings_set(flag.get(), 1).first.empty(), "the host applies the flag");
    }
    app.flush_settings();
    require(app.vibration() && saved(app).vibration, "the host's change is saved");
    // A config without the field loads on.
    {
        TemporaryDirectory edited;
        std::ofstream(edited.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
               R"("theme":"graphite"}})";
        std::vector<std::string> errors;
        const auto loaded = load_settings(edited.path, errors);
        require(loaded.vibration && errors.empty(), "a config without the field loads vibration on");
    }
}

void app_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::RenderSettings;
    CaptureSink sink;
    App app(options, sink);
    require(!has(app.view(), "language-cycle"), "no favorites hides language cycle");
    require(control(app.view(), "settings").bounds.x < control(app.view(), "recenter").bounds.x &&
                control(app.view(), "recenter").bounds.x < control(app.view(), "pin").bounds.x,
            "pin follows Recenter");
    const auto unpinned_icon = control(app.view(), "pin").icon;
    click(app, "pin");
    require(app.pinned() && saved(app).pinned, "pin queues persistent state immediately");
    require(!control(app.view(), "pin").selected, "pin active state changes icon without background");
    require(control(app.view(), "pin").icon != unpinned_icon, "pin has distinct on/off icons");
    app.set_dragging(true);
    require(!app.dragging(), "pin refuses drag capture");
    {
        App reopened(options, sink);
        require(reopened.pinned(), "pin survives restart");
    }
    click(app, "pin");
    require(!app.pinned(), "pin can be disabled");
    click(app, "passthrough");
    require(app.passthrough() && saved(app).passthrough, "Stay open over apps is saved at once");
    {
        App reopened(options, sink);
        require(reopened.passthrough(), "Stay open over apps survives restart");
    }
    click(app, "passthrough");
    require(!app.passthrough() && !saved(app).passthrough, "and turning it off is saved too");

    app.show_settings();
    const auto cards = app.view().cards;
    require(cards.size() == 4 && cards[1].title == "General" && cards[2].title == "Typing" &&
                cards[3].title == "Dictation",
            "production settings shows Languages and Layouts, General, Typing and Dictation");
    require(cards[3].bounds.x + cards[3].bounds.width >
                    SettingsUi::viewport.x + SettingsUi::viewport.width &&
                cards[3].bounds.x < SettingsUi::viewport.x + SettingsUi::viewport.width - 80,
            "the fourth card is cut off at the right edge, showing that the row scrolls");
    require(has(app.view(), "apply") && has(app.view(), "reload"), "settings toolbar actions");
    const auto update_button = control(app.view(), "updates");
    require(update_button.label == "Check for updates" &&
                update_button.bounds.x + update_button.bounds.width / 2 == panel_width / 2 &&
                update_button.bounds.y == 12,
            "update button is centered in the settings header");
    require(!has(app.view(), "close") && !has(app.view(), "recenter"), "settings toolbar is focused");
    click(app, "language");
    require(app.view().popup.has_value(), "dropdown opens");
    auto menu_count = [](const PanelView& v) {
        return std::count_if(v.controls.begin(), v.controls.end(),
                             [](const Control& c) { return c.style == ControlStyle::MenuItem; });
    };
    require(menu_count(app.view()) == 5, "dropdown shows five choices");
    const auto popup = *app.view().popup;
    const auto before = app.view().controls;
    app.move(0, popup.x + 5, popup.y + 5);
    app.scroll(0, 0, 1);
    require(app.view().controls != before && menu_count(app.view()) == 5,
            "dropdown scroll changes visible rows");
    // Dismissing on Apply must consume the click, leaving pending edits unsaved.
    click(app, "apply");
    require(!app.view().popup && app.view().settings, "outside click dismisses without applying");
    click(app, "language");
    app.back();
    require(!app.view().popup && app.view().settings, "Escape dismisses menu first");
    app.back();
    require(!app.view().settings, "Escape then exits settings");

    app.show_settings();
    require(!choice_enabled(app, "language", "de-de") && choice_enabled(app, "language", "ko-kr"),
            "languages the layout cannot type are dimmed");
    click(app, "language");
    require(app.view().status.starts_with("Dimmed languages need another layout"),
            "the open language list explains dimmed entries");
    app.back();
    require(app.view().status.empty(), "the explanation closes with the list");
    select(app, "layout", "international-full");
    select(app, "language", "de-de");
    select(app, "layout", "en-us-full");
    require(app.view().settings && control(app.view(), "language").label == "Deutsch QWERTY",
            "a layout change keeps the language on a keyboard option that fits");
    select(app, "layout", "international-full");
    require(control(app.view(), "language").label == "Deutsch QWERTY",
            "a layout change keeps a language that still fits");
    app.back();
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    require(control(app.view(), "additional-languages").label == "Additional languages: 1",
            "the button counts additional languages");
    app.back();
    require(app.view().confirm && app.view().settings, "leaving asks about popup edits");
    click(app, "confirm-cancel");
    require(!app.view().settings && saved(app).favorites.empty(), "Discard drops them");
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    select(app, "theme", "midnight");
    click(app, "apply");
    auto settings = saved(app);
    require(settings.favorites.size() == 1 && settings.active.language == "en-us" &&
                settings.active.theme == "midnight",
            "Apply persists default, theme and favorite changes together");
    require(has(app.view(), "language-cycle"), "favorite exposes main cycle button");
    require(control(app.view(), "pin").bounds.x < control(app.view(), "language-cycle").bounds.x,
            "language cycle follows pin and zoom buttons");
    click(app, "language-cycle");
    require(app.selection().language == "de-de" && app.selection().theme == "midnight",
            "cycle activates favorite pair while retaining current theme");
    require(saved(app).active.language == "en-us", "cycling does not overwrite saved default");
    click(app, "pin");
    require(saved(app).active.language == "en-us", "pin save preserves default during favorite cycle");
    click(app, "language-cycle");
    require(app.selection().language == "en-us", "cycle returns to saved default");
    settings = saved(app);
    settings.favorites.insert(settings.favorites.begin(),
                              {"absent", "Removed profile", {"missing", "missing", "graphite"}});
    settings.favorites.insert(settings.favorites.begin(),
                              {"korean", "Korean", {"en-us-full", "ko-kr", "graphite"}});
    save_settings(directory.path, settings);
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de" || app.selection().language == "ko-kr",
            "missing favorite profile or optional engine does not block cycling");
    // Reset to the original favorite for the following deduplication check.
    settings.favorites.erase(settings.favorites.begin(), settings.favorites.begin() + 2);
    save_settings(directory.path, settings);
    app.reload();
    // A favorite of the saved default is a single cycle entry, even if duplicated
    // in a legacy file with a different theme.
    app.show_settings();
    select(app, "layout", "international-full");
    select(app, "language", "de-de");
    click(app, "apply");
    settings = saved(app);
    settings.favorites.push_back(
        {"duplicate", "duplicate", {"international-full", "de-de", "graphite"}});
    save_settings(directory.path, settings);
    app.reload();
    require(!has(app.view(), "language-cycle"),
            "one unique favorite equal to default leaves nothing to cycle");
    app.show_settings();
    require(control(app.view(), "additional-languages").label == "Additional languages",
            "a legacy favorite of the main language does not count");
    click(app, "apply");
    require(saved(app).favorites.empty() && !has(app.view(), "language-cycle"),
            "Apply removes all duplicates and hides main cycle");

    // Reload closes a menu, cancels a pressed choice, and discards staged edits.
    app.show_settings();
    add_language(app, "International", "Danish");
    click(app, "language");
    // Corrupt config reload keeps the prior valid pin/favorites state and reports
    // the error, while clearing all pending controls.
    const auto config_path = directory.path / "config.json";
    std::ifstream original_file(config_path);
    const std::string original_config((std::istreambuf_iterator<char>(original_file)), {});
    std::ofstream(config_path) << "{broken";
    const bool was_pinned = app.pinned();
    app.reload();
    require(app.pinned() == was_pinned && !app.view().status.empty(),
            "invalid reload retains working settings");
    std::ofstream(config_path) << original_config;
    click(app, "language");
    auto c = control(app.view(), "choose:language:de-de");
    app.down(1, c.bounds.x + 5, c.bounds.y + 5, 1);
    require(app.pointer_pressed(1), "settings controls participate in pointer cancellation");
    app.reload();
    app.up(1, c.bounds.x + 5, c.bounds.y + 5);
    require(!app.view().popup &&
                control(app.view(), "additional-languages").label == "Additional languages" &&
                !app.pointer_pressed(1),
            "reload clears popup, captures and staged favorites");
    click(app, "language");
    c = control(app.view(), "choose:language:de-de");
    app.down(1, c.bounds.x + 5, c.bounds.y + 5, 1);
    app.move(0, c.bounds.x + 5, c.bounds.y + 5);
    app.scroll(0, 0, 1);
    app.up(1, c.bounds.x + 5, c.bounds.y + 5);
    require(app.view().popup.has_value(), "scroll invalidates a held menu choice");
    app.set_interaction_active(false);
    require(!app.view().popup && !app.pointer_pressed(1), "hide cancels popup and all UI captures");
    app.set_interaction_active(true);
    require(sink.events.empty(), "settings interaction never emits input in previews");

    // Unknown top-level config survives writes; malformed config is preserved.
    std::ifstream input(directory.path / "config.json");
    std::string data((std::istreambuf_iterator<char>(input)), {});
    data.insert(data.find('{') + 1, "\"custom_setting\":42,");
    std::ofstream(directory.path / "config.json") << data;
    save_settings(directory.path, settings);
    click(app, "settings"); // Back to main.
    click(app, "pin");
    app.flush_settings();
    std::ifstream updated(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(updated), {});
    require(data.find("custom_setting") != std::string::npos, "settings save preserves unknown fields");
    std::ofstream(directory.path / "config.json") << "{broken";
    click(app, "pin");
    app.flush_settings();
    require(app.view().status.find("not saved") != std::string::npos, "pin reports failed persistence");
    std::ifstream malformed(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(malformed), {});
    require(data == "{broken", "malformed config is never overwritten");
}
void behavior_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!app.use_drag_to_move() && app.allows_movement(MoveSource::Laser) &&
                !app.allows_movement(MoveSource::Grip),
            "missing preference defaults to laser movement with grip disabled");
    const auto handle = panel_move_handle_bounds(app.view());
    const auto body = panel_case_bounds(app.view());
    require(handle && handle->y > body.y + body.height &&
                handle->x + handle->width / 2 == panel_width / 2,
            "default movement handle is centered below the keyboard");
    const int x = static_cast<int>(handle->x + handle->width / 2);
    const int y = static_cast<int>(handle->y + handle->height / 2);
    auto handle_alpha = [&] {
        const auto rgba = app.renderer.rgba();
        return rgba[static_cast<std::size_t>((y + popup_margin) * panel_width + x) * 4 + 3];
    };
    app.paint(1);
    require(handle_alpha() > 0 && !app.renderer.hit_key(app.view(), x, y),
            "movement line paints below the case and cannot type a key");
    app.renderer.write_png("/tmp/framekeyboard-behavior-laser.png");
    const auto resting_status = app.view().status;
    click(app, "move-handle");
    app.set_dragging(true);
    require(app.view().status == resting_status, "moving shows no instruction of its own");
    app.set_dragging(false);
    app.show_settings();
    const auto settings_view = app.view();
    require(settings_view.cards.size() == 4 && settings_view.cards[1].title == "General" &&
                !shown_control(app, "use-drag-to-move").selected,
            "General card has an unchecked movement preference");
    // Auto leveling, the General card's last row, lies below its fold until the card scrolls.
    reveal(app, "auto-leveling");
    const auto scrolled_view = app.view();
    const auto numpad_bounds = control(scrolled_view, "hide-numpad").bounds;
    const auto shortcuts_bounds = control(scrolled_view, "shortcuts").bounds;
    const auto movement_bounds = control(scrolled_view, "use-drag-to-move").bounds;
    const auto leveling_bounds = control(scrolled_view, "auto-leveling").bounds;
    require(numpad_bounds.x == movement_bounds.x && movement_bounds.x == leveling_bounds.x &&
                shortcuts_bounds.x == numpad_bounds.x &&
                shortcuts_bounds.y - numpad_bounds.y == leveling_bounds.y - movement_bounds.y &&
                movement_bounds.y - shortcuts_bounds.y == leveling_bounds.y - movement_bounds.y,
            "General checkboxes form one evenly spaced vertical column");
    const auto theme_bounds = control(settings_view, "theme").bounds;
    require(movement_bounds.x + movement_bounds.width == theme_bounds.x + theme_bounds.width,
            "General controls share one right edge");
    const std::array<std::pair<const char*, const PanelView*>, 4> labels{
        {{"Theme", &settings_view},
         {"Hide numpad", &settings_view},
         {"Grab to move", &scrolled_view},
         {"Auto leveling", &scrolled_view}}};
    for (const auto& [text, view] : labels) {
        require(std::any_of(view->field_labels.begin(), view->field_labels.end(),
                            [&](const FieldLabel& field) {
                                return field.text == text && field.left &&
                                       field.bounds.x == settings_view.cards[1].bounds.x + 30 &&
                                       field.bounds.x + field.bounds.width < movement_bounds.x;
                            }),
                "General labels start at the left inset, before their controls");
    }
    app.paint(2);
    app.renderer.write_png("/tmp/framekeyboard-behavior-settings.png");
    click(app, "use-drag-to-move");
    require(shown_control(app, "use-drag-to-move").selected && !app.use_drag_to_move(),
            "movement preference remains staged until Apply");
    app.back();
    app.show_settings();
    require(!shown_control(app, "use-drag-to-move").selected, "Back discards a staged movement change");
    click(app, "use-drag-to-move");
    click(app, "apply");
    require(app.use_drag_to_move() && saved(app).use_drag_to_move &&
                app.allows_movement(MoveSource::Grip) && !app.allows_movement(MoveSource::Laser) &&
                !panel_move_handle_bounds(app.view()),
            "Apply enables grab, saves it and removes the line hit region");
    app.paint(3);
    require(handle_alpha() == 0, "grab mode clears the old line pixels");
    {
        App reopened(options, sink);
        require(reopened.use_drag_to_move() && !panel_move_handle_bounds(reopened.view()),
                "grab preference survives restart");
    }
    click(app, "pin");
    require(!app.allows_movement(MoveSource::Grip) && !app.allows_movement(MoveSource::Laser) &&
                !panel_move_handle_bounds(app.view()),
            "pin blocks both movement methods in grab mode");
    click(app, "pin");
    require(app.allows_movement(MoveSource::Grip) && !panel_move_handle_bounds(app.view()),
            "unpinning retains grab mode");
    app.show_settings();
    click(app, "use-drag-to-move");
    click(app, "hide-numpad");
    click(app, "apply");
    require(!saved(app).use_drag_to_move && panel_move_handle_bounds(app.view()) &&
                panel_move_handle_bounds(app.view())->x + handle->width / 2 == panel_width / 2,
            "laser handle stays centered on the compact case");
    click(app, "pin");
    require(!panel_move_handle_bounds(app.view()) && !app.allows_movement(MoveSource::Laser) &&
                !app.allows_movement(MoveSource::Grip),
            "pin hides the line and blocks both methods in laser mode");
    app.paint(4);
    require(handle_alpha() == 0, "pin leaves no line pixels");
    app.renderer.write_png("/tmp/framekeyboard-behavior-pinned.png");
    click(app, "pin");
    app.paint(5);
    require(handle_alpha() > 0 && app.allows_movement(MoveSource::Laser),
            "unpinning restores the line and laser movement");
    app.renderer.write_png("/tmp/framekeyboard-behavior-compact.png");
    auto settings = saved(app);
    settings.use_drag_to_move = true;
    save_settings(directory.path, settings);
    app.reload();
    require(app.use_drag_to_move() && !panel_move_handle_bounds(app.view()),
            "Reload applies the saved movement preference");
    require(sink.events.empty(), "behavior tests deliver no desktop input");

    MoveCapture capture;
    require(capture.begin(MoveSource::Laser, 1, 0) && capture.owns_pointer(1, 0),
            "laser capture records the controller and pointer");
    require(!capture.begin(MoveSource::Grip, 2) && !capture.begin(MoveSource::Laser, 2, 1) &&
                !capture.owns_pointer(2, 0) && !capture.owns_pointer(1, 1),
            "a second hand or pointer cannot steal or release movement");
    capture.reset();
    require(!capture.active() && !capture.owns_pointer(1, 0) && capture.begin(MoveSource::Grip, 2) &&
                !capture.owns_pointer(2, 0),
            "cancellation clears capture and laser release cannot end a grip");
    capture.reset();
    require(capture.begin(MoveSource::Laser, 0, 0) && capture.owns_pointer(0, 0) &&
                !capture.owns_pointer(1, 0) && !capture.begin(MoveSource::Laser, 1, 0),
            "AUX head capture owns HMD device zero even when a controller uses the same cursor");
    capture.reset();
    require(!capture.owns_pointer(0, 0), "AUX release or tracking loss clears head capture");
}
void favorite_schema() {
    TemporaryDirectory directory;
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"favorites":[{"id":"pair","name":"Pair only","layout":"international-full","language":"de-de"}]})";
    const auto config = saved(directory.path);
    require(!config.use_drag_to_move, "legacy configs default to laser movement");
    require(config.auto_leveling, "legacy configs default to Auto leveling enabled");
    require(config.favorites.size() == 1 && config.favorites.front().selection.language == "de-de",
            "favorite pair does not require a legacy theme field");
}
void auto_leveling_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.auto_leveling(), "Auto leveling is enabled on a new install");
    app.show_settings();
    require(shown_control(app, "auto-leveling").selected, "Auto leveling starts checked");
    click(app, "auto-leveling");
    require(app.auto_leveling() && !shown_control(app, "auto-leveling").selected,
            "Auto leveling stays pending until Apply");
    app.back();
    app.show_settings();
    require(shown_control(app, "auto-leveling").selected, "Back discards Auto leveling edits");
    click(app, "auto-leveling");
    click(app, "apply");
    require(!app.auto_leveling() && !saved(app).auto_leveling, "Apply disables and saves Auto leveling");
    {
        App reopened(options, sink);
        require(!reopened.auto_leveling(), "disabled Auto leveling survives restart");
    }
    click(app, "pin");
    require(!saved(app).auto_leveling, "immediate pin save preserves disabled Auto leveling");
    auto settings = saved(app);
    settings.auto_leveling = true;
    save_settings(directory.path, settings);
    app.show_settings();
    click(app, "reload");
    require(app.auto_leveling() && shown_control(app, "auto-leveling").selected,
            "Reload applies saved Auto leveling and refreshes its checkbox");
    require(sink.events.empty(), "Auto leveling settings send no desktop input");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"auto_leveling":"false"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "Auto leveling requires a JSON boolean");
}
// Hover fade (docs/configuration.md): on unless turned off, under Background.
void hover_fade_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.view().hover_fade, "Hover fade is on for a new install");
    app.show_settings();
    const auto view = app.view();
    require(control(view, "hover-fade").selected &&
                control(view, "background-surface").bounds.y < control(view, "hover-fade").bounds.y &&
                control(view, "hover-fade").bounds.y < control(view, "hide-numpad").bounds.y,
            "Hover fade starts on, between Background and Hide numpad");
    click(app, "hover-fade");
    require(!control(app.view(), "hover-fade").selected && app.view().hover_fade,
            "Hover fade stays pending until Apply");
    click(app, "apply");
    require(!app.view().hover_fade && !saved(app).hover_fade, "Apply turns Hover fade off and saves it");
    {
        App reopened(options, sink);
        require(!reopened.view().hover_fade, "Hover fade off survives a restart");
    }
    require(sink.events.empty(), "Hover fade settings send no desktop input");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"hover_fade":"false"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "Hover fade requires a JSON boolean");
}
// Curved keyboard (docs/configuration.md): off unless turned on, under Hover
// fade; the curve bars bend only while it is on and the keyboard unpinned.
void curved_keyboard_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!app.curved_keyboard() && !app.allows_movement(MoveSource::Curve),
            "Curved keyboard is off for a new install, and its bars bend nothing");
    app.show_settings();
    const auto view = app.view();
    require(!control(view, "curved-keyboard").selected &&
                control(view, "hover-fade").bounds.y < control(view, "curved-keyboard").bounds.y &&
                control(view, "curved-keyboard").bounds.y < control(view, "hide-numpad").bounds.y,
            "Curved keyboard starts off, between Hover fade and Hide numpad");
    click(app, "curved-keyboard");
    require(control(app.view(), "curved-keyboard").selected && !app.curved_keyboard(),
            "Curved keyboard stays pending until Apply");
    click(app, "apply");
    require(app.curved_keyboard() && saved(app).curved_keyboard,
            "Apply turns Curved keyboard on and saves it");
    require(app.allows_movement(MoveSource::Curve), "with it on, an unpinned keyboard can be bent");
    {
        App reopened(options, sink);
        require(reopened.curved_keyboard(), "Curved keyboard on survives a restart");
    }
    require(sink.events.empty(), "Curved keyboard settings send no desktop input");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"curved_keyboard":"true"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "Curved keyboard requires a JSON boolean");
}
// The Additional languages popup (docs/configuration.md): the main language,
// the ordered languages after it, tap and drag, and the question on leaving.
void languages_popup() {
    TemporaryDirectory directory;
    std::string favorites;
    // A legacy repeat, the main language and a pair whose layout file is gone.
    const std::vector<std::pair<std::string, std::string>> pairs{
        {"international-full", "de-de"}, {"en-us-full", "en-us"},
        {"removed-layout", "de-de"},     {"international-full", "es-es"},
        {"international-full", "fr-fr"}, {"international-full", "de-de"}};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        favorites += (i ? "," : "") + std::string(R"({"id":"f)") + std::to_string(i) +
                     R"(","name":"Pair","layout":")" + pairs[i].first + R"(","language":")" +
                     pairs[i].second + R"("})";
    }
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"favorites":[)" +
               favorites + "]}";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    app.show_settings();
    static double now = 300;
    auto settle = [&] {
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
    };
    auto drag = [&](const Rect& from, double x, double y) {
        const double start_x = from.x + from.width / 2, start_y = from.y + from.height / 2;
        app.move(0, start_x, start_y);
        app.down(0, start_x, start_y, now);
        app.move(0, x, start_y);
        app.move(0, x, y);
        app.up(0, x, y);
    };
    auto additional = [&] {
        std::vector<std::string> titles;
        const auto view = app.view();
        for (const auto& row : view.languages->additional) {
            titles.push_back(row.title);
        }
        return titles;
    };
    using Titles = std::vector<std::string>;
    require(control(app.view(), "additional-languages").label == "Additional languages: 4",
            "the count skips legacy repeats and the main language");
    click(app, "additional-languages");
    auto view = app.view();
    require(view.languages && view.languages->layouts.size() == 6, "the layouts show as tabs");
    require(view.languages->layouts.front().title == "US QWERTY" &&
                view.languages->layouts.front().selected,
            "the common layouts lead, on the main language's");
    require(view.languages->main.title == "English (US)" && !control(view, "languages-reset").enabled,
            "the main slot starts at English, which needs no reset");
    require(additional() ==
                Titles({"German, QWERTZ", "German, QWERTZ", "Spanish (Spain)", "French, AZERTY"}),
            "the additional list");
    require(view.languages->additional[1].note.find("(missing)") != std::string::npos,
            "a pair whose layout is gone stays listed so it can go");
    tap_row(app, "Japanese flick");
    require(!languages_row(app.view(), "Arabic").enabled, "languages a layout can't type stay listed");
    {
        const auto r = languages_row(app.view(), "Arabic").bounds;
        app.move(0, r.x + 20, r.y + 20);
        app.down(0, r.x + 20, r.y + 20, now);
        app.up(0, r.x + 20, r.y + 20);
    }
    require(additional().size() == 4, "and cannot be added");
    tap_row(app, "International");
    tap_row(app, "Czech, QWERTZ");
    require(additional().back() == "Czech, QWERTZ", "a tap adds to the end");
    tap_row(app, "Czech, QWERTZ");
    require(additional().size() == 5, "only once");
    tap_row(app, "US QWERTY");
    tap_row(app, "English (US)");
    require(additional().size() == 5, "the main language is not also additional");
    // Sideways lifts a language; it lands between the rows it is over.
    tap_row(app, "International");
    view = app.view();
    show_row(app, "Danish");
    view = app.view();
    const auto danish = languages_row(view, "Danish").bounds;
    const Rect& list = additional_list_bounds;
    drag(danish, list.x + 100, list.y + additional_row_height * 1.1);
    require(additional() == Titles({"German, QWERTZ", "Danish", "German, QWERTZ", "Spanish (Spain)",
                                    "French, AZERTY", "Czech, QWERTZ"}),
            "a dropped language lands where it is held");
    settle();
    // Up and down scrolls the list instead, and the release adds nothing.
    view = app.view();
    const auto arabic = languages_row(view, "Danish").bounds;
    drag(arabic, arabic.x + arabic.width / 2, arabic.y - 80);
    require(additional().size() == 6 && languages_row(app.view(), "Danish").bounds.y < arabic.y,
            "a vertical drag scrolls");
    // A row reorders, and dropped outside goes.
    view = app.view();
    // The last row shows only in part, and is held by that part.
    auto last = view.languages->additional[5].bounds;
    last.height = std::min(last.height, list.y + list.height - last.y);
    drag(last, list.x + 100, list.y + 5);
    require(additional().front() == "Czech, QWERTZ", "the additional list sorts");
    view = app.view();
    drag(view.languages->additional[0].bounds, 500, 300);
    require(additional().front() == "German, QWERTZ" && additional().size() == 5,
            "dropped outside removes");
    view = app.view();
    drag(view.languages->additional[0].bounds, languages_main_bounds.x + 50,
         languages_main_bounds.y + 20);
    view = app.view();
    require(view.languages->main.title == "German, QWERTZ" && additional().front() == "English (US)",
            "an additional language dropped on main trades places with it");
    require(control(app.view(), "additional-languages").label == "Additional languages: 5",
            "the count follows");
    require(control(view, "languages-reset").enabled, "a non-English main can reset");
    require(control(view, "languages-reset").icon == Icon::Reset, "with an arrow turning back");
    click(app, "languages-reset");
    view = app.view();
    const auto after_reset = additional();
    require(view.languages->main.title == "English (US)" &&
                std::count(after_reset.begin(), after_reset.end(), "English (US)") == 0,
            "reset removes English from the additional list");
    require(control(app.view(), "language").label == "English (US)", "main is the dropdowns' pair");
    click(app, "languages-clear");
    require(app.view().confirm && app.view().confirm->title == "Clear all additional languages?",
            "clearing asks first");
    click(app, "confirm-cancel");
    require(!app.view().confirm && !additional().empty(), "Cancel keeps them");
    click(app, "languages-clear");
    click(app, "confirm-accept");
    require(additional().empty() && app.view().languages->main.title == "English (US)",
            "clear leaves the main language");
    require(!app.view().languages->hint.empty() && !control(app.view(), "languages-clear").enabled,
            "an empty list explains itself");
    click(app, "languages-close");
    require(!app.view().languages && app.view().settings && !app.view().confirm,
            "the arrow returns to Settings without asking");
    app.back();
    require(app.view().confirm && app.view().confirm->title == "Save your language changes?",
            "leaving Settings asks");
    require(control(app.view(), "confirm-cancel").label == "Discard" &&
                control(app.view(), "confirm-accept").label == "Save",
            "Discard or Save");
    app.back();
    require(!app.view().confirm && app.view().settings, "Esc stays in Settings");
    app.back();
    click(app, "confirm-accept");
    require(!app.view().settings && saved(app).favorites.empty(), "Save applies");

    // The dropdowns alone are ordinary pending changes: Back drops them.
    app.show_settings();
    select(app, "layout", "international-full");
    app.back();
    require(!app.view().settings && !app.view().confirm, "no question for the dropdowns alone");
    // Popup edits undone by hand leave nothing to ask about.
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    click(app, "additional-languages");
    view = app.view();
    drag(view.languages->additional[0].bounds, 500, 300);
    click(app, "languages-close");
    app.back();
    require(!app.view().settings && !app.view().confirm, "no question without a change");
    // A language dropped on main replaces it, and its layout follows; the old
    // main one stays among the additional languages.
    app.show_settings();
    click(app, "additional-languages");
    tap_row(app, "International");
    show_row(app, "German, QWERTZ");
    view = app.view();
    drag(languages_row(view, "German, QWERTZ").bounds, languages_main_bounds.x + 50,
         languages_main_bounds.y + 20);
    require(app.view().languages->main.title == "German, QWERTZ" &&
                additional() == std::vector<std::string>{"English (US)"},
            "a language from the list replaces main, which becomes an additional one");
    require(control(app.view(), "layout").label == "International", "and the layout goes with it");
    app.back();
    click(app, "apply");
    require(saved(app).active.language == "de-de", "Apply saves main like the dropdowns");
    app.show_settings();
    click(app, "additional-languages");
    app.summon();
    require(!app.view().languages, "summoning closes the popup");
}
// Cycling changes the language being typed in, never what Settings calls main.
void main_language_stays() {
    TemporaryDirectory directory;
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"favorites":[{"id":"a","name":"a","layout":"international-full","language":"de-de"},{"id":"b","name":"b","layout":"international-full","language":"fr-fr"}]})";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    auto titles = [&] {
        std::vector<std::string> result;
        const auto view = app.view();
        for (const auto& row : view.languages->additional) {
            result.push_back(row.title);
        }
        return result;
    };
    const std::vector<std::string> both{"German, QWERTZ", "French, AZERTY"};
    click(app, "language-cycle");
    require(app.selection().language == "de-de", "cycling types in the next language");
    app.show_settings();
    require(control(app.view(), "language").label == "English (US)" &&
                control(app.view(), "layout").label == "US QWERTY",
            "Settings shows the main language, not the one being typed in");
    require(control(app.view(), "additional-languages").label == "Additional languages: 2",
            "the count is unchanged");
    click(app, "additional-languages");
    require(app.view().languages->main.title == "English (US)" && titles() == both,
            "the popup keeps main and the whole list");
    click(app, "languages-close");
    app.back();
    require(!app.view().confirm && app.selection().language == "de-de",
            "looking changes nothing, and typing goes on in the same language");

    app.show_settings();
    select(app, "theme", "midnight");
    click(app, "apply");
    require(app.selection().language == "de-de" && app.selection().theme == "midnight",
            "an unrelated Apply keeps the typing language");
    require(saved(app).active.language == "en-us" && saved(app).favorites.size() == 2 &&
                saved(app).active.theme == "midnight",
            "and saves the main language, with the theme");
    click(app, "language-cycle");
    require(app.selection().language == "fr-fr", "the cycle order holds");
    click(app, "language-cycle");
    require(app.selection().language == "en-us", "and returns to main");

    click(app, "language-cycle");
    require(app.selection().language == "de-de", "typing in an additional language again");
    app.show_settings();
    click(app, "additional-languages");
    {
        const auto row = app.view().languages->additional.front().bounds;
        const double x = row.x + row.width / 2, y = row.y + row.height / 2;
        app.move(0, x, y);
        app.down(0, x, y, 1);
        app.move(0, 500, 300);
        app.up(0, 500, 300);
    }
    click(app, "languages-close");
    click(app, "apply");
    require(app.selection().language == "en-us" && saved(app).favorites.size() == 1,
            "removing the language being typed in returns to main");

    click(app, "language-cycle");
    require(app.selection().language == "fr-fr", "typing in the one left");
    app.show_settings();
    select(app, "layout", "international-full");
    select(app, "language", "de-de");
    click(app, "apply");
    require(app.selection().language == "de-de" && saved(app).active.language == "de-de",
            "a new main language takes over typing");
}
void theme_preview() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.view().theme->id == "graphite", "Graphite to start");
    app.show_settings();
    select(app, "theme", "midnight");
    require(app.view().theme->id == "midnight", "a chosen theme shows at once in Settings");
    app.back();
    require(app.view().theme->id == "graphite" && saved(app).active.theme == "graphite",
            "Back returns to the saved theme");
    app.show_settings();
    select(app, "theme", "midnight");
    click(app, "apply");
    require(app.view().theme->id == "midnight" && saved(app).active.theme == "midnight",
            "Apply keeps it");
}
// General → KDE Desktop Unicode: the sink types such text as Unicode entry
// (no clipboard) until Paste is applied, and the choice survives restart.
void desktop_unicode_setting() {
    struct DesktopSink : CaptureSink {
        std::vector<Clipboard*> routes;
        void set_desktop_paste(Clipboard* clipboard) override { routes.push_back(clipboard); }
    };
    struct NoClipboard : Clipboard {
        std::uint64_t write(const std::string&) override { return 1; }
        std::optional<bool> result(std::uint64_t) const override { return true; }
        std::uint64_t served() const override { return 0; }
    } clipboard;
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    options.clipboard = &clipboard;
    DesktopSink sink;
    App app(options, sink);
    require(!sink.routes.empty() && sink.routes.back() == nullptr, "Type by default");
    app.show_settings();
    // The row sits at the foot of the Typing card: scroll down to it.
    double now = 200;
    const auto to_row = [&] {
        app.move(1, 1200, 300);
        app.scroll(1, 0, 20);
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
    };
    to_row();
    require(control(app.view(), "desktop-unicode").label == "Type", "the row shows Type");
    const auto described = [&](const std::string& words) {
        to_row();
        const auto& labels = app.view().field_labels;
        return std::any_of(labels.begin(), labels.end(), [&](const FieldLabel& label) {
            return label.text.find(words) != std::string::npos && !label.warning;
        });
    };
    require(described("You will see Unicode codes"), "Type is described in the legend color");
    select(app, "desktop-unicode", "paste");
    require(described("replaces what is in the clipboard") && sink.routes.back() == nullptr,
            "Paste is described, and pending until Apply");
    click(app, "apply");
    require(saved(app).desktop_unicode_paste && sink.routes.back() == &clipboard,
            "Apply saves Paste and hands the sink the clipboard");
    {
        DesktopSink reopened_sink;
        App reopened(options, reopened_sink);
        require(reopened_sink.routes.back() == &clipboard, "Paste survives restart");
    }
    app.show_settings();
    to_row();
    select(app, "desktop-unicode", "type");
    click(app, "apply");
    require(!saved(app).desktop_unicode_paste && sink.routes.back() == nullptr, "back to Type");
    require(sink.events.empty(), "the setting sends no input");
}
// Settings, scrolled so the Dictation card shows whole.
void open_dictation_card(App& app) {
    app.show_settings();
    app.move(9, 800, 300);
    app.scroll(9, 4, 0);
    static double now = 300;
    for (int i = 0; i < 20; ++i) {
        app.tick(now += .05);
    }
}
void dictation_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_dictation_card(app);
    require(control(app.view(), "send-gap").label.find("20 ms") != std::string::npos,
            "Send types at 20 ms by default");
    select(app, "send-gap", "40");
    require(saved(app).dictation_send_gap == 20, "the choice stays pending until Apply");
    click(app, "apply");
    require(saved(app).dictation_send_gap == 40 && !saved(app).dictation_send_paste,
            "Apply saves the pace");
    {
        App reopened(options, sink);
        open_dictation_card(reopened);
        require(control(reopened.view(), "send-gap").label.find("40 ms") != std::string::npos,
                "the pace survives restart");
    }
    open_dictation_card(app);
    select(app, "send-gap", "paste");
    click(app, "apply");
    require(saved(app).dictation_send_paste && saved(app).dictation_send_gap == 40,
            "Paste is saved apart from the pace");
    {
        App reopened(options, sink);
        open_dictation_card(reopened);
        require(control(reopened.view(), "send-gap").label == "Paste", "Paste survives restart");
    }
    {
        std::ofstream(directory.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},)"
            << R"("dictation_send_gap_ms":10})";
        std::vector<std::string> errors;
        require(load_settings(directory.path, errors).dictation_send_gap == 10 && errors.empty(),
                "Send can type at 10 ms");
    }
    for (const auto* value : {R"("dictation_send_gap_ms":25)", R"("dictation_send_gap_ms":"30")",
                              R"("dictation_send_paste":"yes")"}) {
        std::ofstream(directory.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},)"
            << R"("dictation":"server","dictation_server":"host",)" << value << "}";
        std::vector<std::string> errors;
        const auto settings = load_settings(directory.path, errors);
        require(errors.size() == 1 && settings.dictation_send_gap == 20 &&
                    !settings.dictation_send_paste && settings.dictation == "server",
                "a bad Send setting falls back to typing at 20 ms on its own");
    }
    require(sink.events.empty(), "dictation settings send no desktop input");
}

// Clear all from the other hand takes what one hand is holding with it.
void language_drag_with_two_hands() {
    TemporaryDirectory directory;
    const auto favorite = [](const char* language) {
        return std::string(R"({"id":")") + language + R"(","name":"n","layout":"international-full",)" +
               R"("language":")" + language + R"("})";
    };
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us",)"
           R"("theme":"graphite"},"favorites":[)"
        << favorite("de-de") << ',' << favorite("fr-fr") << ',' << favorite("es-es") << "]}";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    app.show_settings();
    click(app, "additional-languages");
    const auto listed = [&] {
        const auto view = app.view();
        return view.languages->additional;
    };
    const auto row = listed().front().bounds;
    const double x = row.x + row.width / 2, y = row.y + row.height / 2;
    app.move(0, x, y);
    app.down(0, x, y, 1);
    app.move(0, x - 90, y + 2 * additional_row_height);
    require(app.view().languages->lifted.has_value(), "the first hand lifts a row");
    click(app, "languages-clear", 1);
    click(app, "confirm-accept", 1);
    require(listed().empty() && !app.view().languages->lifted, "clearing drops the held row too");
    app.up(0, x - 90, y + 2 * additional_row_height);
    require(listed().empty() && !app.view().confirm, "and letting go changes nothing");
}

void keep_open_setting() {
    TemporaryDirectory directory;
    Settings settings;
    settings.favorites = {{"de", "German", {"international-full", "de-de", "graphite"}}};
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    CaptureSink sink;
    {
        App app(options, sink);
        require(!has(app.view(), "keep-open"), "Keep open is absent without replacement");
    }
    settings.replace_native_keyboard = true;
    save_settings(directory.path, settings);
    App app(options, sink);
    const auto view = app.view();
    require(control(view, "passthrough").bounds.x < control(view, "keep-open").bounds.x &&
                control(view, "keep-open").bounds.x < control(view, "language-cycle").bounds.x,
            "Keep open sits between Stay open over apps and the language");
    const auto off = control(view, "keep-open").icon;
    require(!app.keep_open(), "Keep open defaults off");
    click(app, "keep-open");
    require(app.keep_open() && saved(app).keep_open, "Keep open is saved at once");
    require(!control(app.view(), "keep-open").selected && control(app.view(), "keep-open").icon != off,
            "Keep open shows its state with the icon only");
    {
        App reopened(options, sink);
        require(reopened.keep_open(), "Keep open survives restart");
    }
    settings.keep_open = true;
    settings.replace_native_keyboard = false;
    save_settings(directory.path, settings);
    click(app, "settings");
    click(app, "reload");
    require(!app.keep_open() && !has(app.view(), "keep-open"),
            "without replacement Keep open has no effect");
    require(sink.events.empty(), "Keep open sends no input");
}
void native_replacement_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!app.replace_native_keyboard(), "replacement defaults off");
    // Integration is the fourth card, cut off until the row scrolls.
    auto open = [&] {
        app.show_settings();
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        for (int i = 0; i < 40; ++i) {
            app.tick(100 + i * .05);
        }
    };
    open();
    const auto view = app.view();
    const auto integration = view.cards.back().bounds;
    const auto startup = control(view, "start-with-steamos");
    require(!startup.enabled && !startup.selected &&
                !startup.hit(startup.bounds.x + 4, startup.bounds.y + 4),
            "SteamOS startup defaults off and cannot be pressed without replacement");
    app.down(0, startup.bounds.x + 4, startup.bounds.y + 4, 1);
    app.up(0, startup.bounds.x + 4, startup.bounds.y + 4);
    require(!control(app.view(), "start-with-steamos").selected,
            "pressing a disabled startup checkbox cannot opt in");
    const auto checkbox = control(view, "replace-native-keyboard").bounds;
    const auto label =
        std::find_if(view.field_labels.begin(), view.field_labels.end(),
                     [](const FieldLabel& f) { return f.text == "Replace native keyboard"; });
    require(view.cards.back().title == "Integration" && label != view.field_labels.end(),
            "replacement belongs to Integration");
    require(label->left && label->bounds.x == integration.x + 30 &&
                checkbox.x > label->bounds.x + label->bounds.width &&
                checkbox.x + checkbox.width == integration.x + integration.width - 24,
            "integration label is at the left inset and its checkbox at the right edge");
    click(app, "replace-native-keyboard");
    require(!app.replace_native_keyboard(), "replacement edit stays pending");
    require(control(app.view(), "start-with-steamos").enabled,
            "pending replacement enables startup editing");
    click(app, "start-with-steamos");
    app.back();
    open();
    require(!control(app.view(), "replace-native-keyboard").selected, "Back discards replacement edit");
    require(!control(app.view(), "start-with-steamos").selected, "Back discards startup edits too");
    click(app, "replace-native-keyboard");
    click(app, "start-with-steamos");
    click(app, "apply");
    require(app.replace_native_keyboard() && saved(app).replace_native_keyboard,
            "Apply persists replacement");
    require(app.start_with_steamos() && saved(app).start_with_steamos, "Apply persists startup opt-in");
    open();
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "start-with-steamos").enabled &&
                !control(app.view(), "start-with-steamos").selected,
            "unchecking replacement disables and unchecks effective startup");
    click(app, "apply");
    require(!app.start_with_steamos(), "saved replacement-off cannot autostart");
    open();
    click(app, "replace-native-keyboard");
    require(control(app.view(), "start-with-steamos").enabled &&
                control(app.view(), "start-with-steamos").selected,
            "reenabling replacement restores the saved startup choice");
    click(app, "apply");
    app.set_native_session(true);
    open();
    click(app, "native-fallback");
    require(app.take_native_fallback() && !app.take_native_fallback(), "escape is consumed once");
    require(saved(app).replace_native_keyboard, "escape does not disable future replacement requests");
    App reopened(options, sink);
    require(reopened.replace_native_keyboard(), "replacement survives restart");
    auto settings = saved(app);
    settings.replace_native_keyboard = false;
    save_settings(directory.path, settings);
    click(app, "reload");
    require(!app.replace_native_keyboard(), "Reload disables replacement");
    require(!has(app.view(), "native-fallback"), "escape is absent when replacement is off");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"replace_native_keyboard":"true"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "replacement requires JSON boolean");
    require(sink.events.empty(), "replacement settings send no desktop input");
}
// A turn-off that fails brings the flag back on while a reopened Settings visit is asking whether to
// turn it on: the card keeps the user's answer, and Cancel leaves the box off.
void streaming_support_failed_turn_off() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    options.stream_support = [] { return StreamSupportStatus{.supported = true}; };
    CaptureSink sink;
    std::atomic<bool> hold{false}, held{false};
    App app(options, sink, [&](const Settings& settings) {
        while (hold) {
            held = true;
            usleep(1000);
        }
        auto kept = settings;
        kept.streaming_support = true; // turning off cannot be done
        save_settings(directory.path, kept);
        if (!settings.streaming_support) {
            ErrorText reason;
            reason.english = "the manager cannot be reloaded";
            throw StreamingSupportWarning("streaming.saved-without-change", reason, true);
        }
    });
    // A check that fails while the save is held must fail, not hang: App's destructor waits for the
    // writer's thread, which waits on `hold`. Declared after `app`, this goes first.
    struct Release {
        std::atomic<bool>& hold;
        ~Release() { hold = false; }
    } release{hold};
    auto open = [&] {
        app.show_settings();
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        for (int i = 0; i < 40; ++i) {
            app.tick(100 + i * .05);
        }
    };
    open();
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    click(app, "apply");
    app.flush_settings();
    require(app.streaming_support(), "on");
    // Off fails, and its result waits until the reopened visit has the question open.
    hold = true;
    open();
    click(app, "streaming-support");
    click(app, "apply");
    for (int i = 0; i < 1000 && !held; ++i) {
        usleep(1000);
    }
    open();
    click(app, "streaming-support");
    require(app.view().confirm && app.view().confirm->title == "Turn on streaming support?",
            "ticking it again asks");
    hold = false;
    app.flush_settings();
    click(app, "confirm-cancel");
    require(!control(app.view(), "streaming-support").selected,
            "Cancel leaves it off though the failed turn-off brought the saved flag back");
    app.back();
    require(app.streaming_support(), "what is saved stays on");

    // The same with Replace native keyboard unticked in the reopened visit: ticked again, streaming
    // support stays off, as unticking replacement left it, and is never on without its question.
    hold = true;
    open();
    click(app, "streaming-support");
    click(app, "apply");
    held = false;
    for (int i = 0; i < 1000 && !held; ++i) {
        usleep(1000);
    }
    open();
    click(app, "replace-native-keyboard");
    hold = false;
    app.flush_settings();
    require(app.streaming_support(), "the failed turn-off brought the saved flag back");
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "streaming-support").selected,
            "re-ticking replacement leaves it off though the saved flag came back");
    app.back();
}
// After the check failed at Steam's start, ✓ does not say streaming support starts after a restart.
void streaming_support_failed_check() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    options.stream_support = [] { return StreamSupportStatus{.supported = true, .failed = true}; };
    CaptureSink sink;
    App app(options, sink);
    app.show_settings();
    app.move(0, 800, 300);
    app.scroll(0, 1000, 0);
    for (int i = 0; i < 40; ++i) {
        app.tick(100 + i * .05);
    }
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    click(app, "apply");
    require(app.streaming_support() &&
                app.view().status != "Streaming support starts after SteamOS restarts.",
            "no restart is promised after a failed check");
}
// Streaming support that cannot be set up: its saved flag goes back, and so does the one in memory, so
// the card shows what is true and later saves don't quietly try it again.
void streaming_support_failed_setup() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    options.stream_support = [] { return StreamSupportStatus{.supported = true}; };
    CaptureSink sink;
    int attempts = 0;
    App app(options, sink, [&](const Settings& settings) {
        auto kept = settings;
        kept.streaming_support = false;
        save_settings(directory.path, kept);
        if (settings.streaming_support && settings.replace_native_keyboard) {
            ++attempts;
            ErrorText reason;
            reason.english = "the library is missing";
            throw StreamingSupportWarning("streaming.saved-without-change", reason, false);
        }
    });
    auto open = [&] {
        app.show_settings();
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        for (int i = 0; i < 40; ++i) {
            app.tick(100 + i * .05);
        }
    };
    open();
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    click(app, "apply");
    app.flush_settings();
    require(attempts == 1 && !saved(app).streaming_support && !app.streaming_support(),
            "a set-up that failed leaves the flag off on disk and in memory");
    require(app.view().status.starts_with("Other preferences saved."), "and says so");
    // Settings opened again before such a save has finished shows the flag as saved once it has.
    open();
    click(app, "streaming-support");
    click(app, "confirm-accept");
    click(app, "apply");
    open();
    app.flush_settings();
    require(attempts == 2 && !control(app.view(), "streaming-support").selected,
            "an open card follows the flag as saved");
    app.back();
    click(app, "pin");
    app.flush_settings();
    require(attempts == 2, "a later save doesn't try it again");
}
// Streaming support: its question when ticked, applied only by ✓, and asked about again when ← leaves
// Settings with it ticked, after the question about changed languages.
void streaming_support_setting() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    StreamSupportStatus status;
    options.stream_support = [&] { return status; };
    CaptureSink sink;
    App app(options, sink);
    require(!app.streaming_support(), "streaming support defaults off");
    auto open = [&] {
        app.show_settings();
        app.move(0, 800, 300);
        app.scroll(0, 1000, 0);
        for (int i = 0; i < 40; ++i) {
            app.tick(100 + i * .05);
        }
    };
    auto note = [&] {
        const auto view = app.view();
        return std::any_of(view.field_labels.begin(), view.field_labels.end(), [](const FieldLabel& f) {
            return f.text == "Takes effect after SteamOS restarts.";
        });
    };
    open();
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "streaming-support").enabled,
            "an installation without the library cannot turn it on");
    app.back();
    status.supported = true;
    open();
    require(!control(app.view(), "streaming-support").enabled, "it needs replacement");
    click(app, "replace-native-keyboard");
    require(control(app.view(), "streaming-support").enabled &&
                !control(app.view(), "streaming-support").selected,
            "with replacement it can be ticked");

    // Ticking it asks first, and says a restart is needed.
    click(app, "streaming-support");
    require(app.view().confirm && app.view().confirm->title == "Turn on streaming support?" &&
                app.view().confirm->body.find("SteamOS has to be restarted") != std::string::npos &&
                app.view().confirm->body.find("very small risk") != std::string::npos,
            "the question explains the library, the restart and the risk");
    require(control(app.view(), "confirm-cancel").label == "Cancel" &&
                control(app.view(), "confirm-accept").label == "Accept",
            "Cancel or Accept");
    click(app, "confirm-cancel");
    require(!app.view().confirm && !control(app.view(), "streaming-support").selected && !note(),
            "Cancel leaves it off");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    require(!app.view().confirm && control(app.view(), "streaming-support").selected && note(),
            "Accept ticks it and says it waits for a restart");
    require(!app.streaming_support() && !saved(app).streaming_support, "nothing is applied yet");
    // When the check failed at Steam's start, the card, the question and the status after ✓ say that
    // instead of promising a restart.
    status.failed = true;
    app.back();
    click(app, "confirm-cancel");
    open();
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
    require(app.view().confirm &&
                app.view().confirm->body.find("SteamOS has to be restarted") == std::string::npos,
            "the question promises no restart after a failed check");
    click(app, "confirm-accept");
    require(!note() && std::any_of(app.view().field_labels.begin(), app.view().field_labels.end(),
                                   [](const FieldLabel& f) {
                                       return f.text == "The streaming library did not pass its check.";
                                   }),
            "a failed check is said on the card");
    status.failed = false;

    // ← with it ticked asks whether to turn it on.
    app.back();
    require(app.view().confirm && app.view().confirm->title == "Turn on streaming support?" &&
                control(app.view(), "confirm-cancel").label == "Leave off" &&
                control(app.view(), "confirm-accept").label == "Turn on",
            "leaving Settings asks about streaming support");
    click(app, "confirm-cancel");
    require(!app.view().settings && !app.streaming_support() && !saved(app).replace_native_keyboard,
            "Leave off discards, as ← does");

    // ✓ applies it and says when it starts.
    open();
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    click(app, "apply");
    require(app.streaming_support() && saved(app).streaming_support &&
                saved(app).replace_native_keyboard,
            "the confirm button applies it");
    require(app.view().status == "Streaming support starts after SteamOS restarts.",
            "and says it waits for a restart");

    // Off needs no question; on again within the same visit neither.
    open();
    click(app, "streaming-support");
    require(!app.view().confirm && !control(app.view(), "streaming-support").selected,
            "off asks nothing");
    click(app, "streaming-support");
    require(!app.view().confirm && control(app.view(), "streaming-support").selected,
            "back on, as it is saved, asks nothing");
    app.back();
    require(!app.view().settings, "nothing changed, so ← just leaves");

    // A piece of it, or Steam's library, goes missing while it is on: the switch still shows it on,
    // says it is not available, and can turn it off, but not on again (R251).
    status.supported = false;
    open();
    const auto unavailable = [&] {
        const auto view = app.view();
        return std::any_of(view.field_labels.begin(), view.field_labels.end(), [](const FieldLabel& f) {
            return f.text == "Streaming support is not available in this installation.";
        });
    };
    require(control(app.view(), "streaming-support").selected &&
                control(app.view(), "streaming-support").enabled && unavailable() && !note(),
            "on but unavailable, it shows on, says so, and can be turned off");
    click(app, "streaming-support");
    require(!app.view().confirm && !control(app.view(), "streaming-support").selected &&
                !control(app.view(), "streaming-support").enabled && !unavailable(),
            "turned off, it cannot be turned on again while unavailable");
    app.back();
    status.supported = true;

    // Replacement off takes it along; replacement back on leaves it off until it is ticked again.
    open();
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "streaming-support").enabled &&
                !control(app.view(), "streaming-support").selected,
            "without replacement it is off");
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "streaming-support").selected, "and stays off until ticked");
    click(app, "apply");
    require(!app.streaming_support() && !saved(app).streaming_support, "saved off");

    // A Steam that already carries the variable: no restart in the question, no note.
    status.active = true;
    open();
    click(app, "streaming-support");
    require(app.view().confirm &&
                app.view().confirm->body.find("SteamOS has to be restarted") == std::string::npos,
            "no restart is mentioned when none is needed");
    click(app, "confirm-accept");
    require(!note(), "nor noted on the card");
    click(app, "apply");
    require(app.streaming_support() && app.view().status.empty(), "applied without a restart message");
    open();
    click(app, "streaming-support");
    click(app, "apply");
    status.active = false;

    // Changed languages and streaming support: one question after the other.
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    app.back();
    require(app.view().confirm && app.view().confirm->title == "Save your language changes?",
            "languages are asked about first");
    click(app, "confirm-cancel");
    require(app.view().settings && app.view().confirm &&
                app.view().confirm->title == "Turn on streaming support?",
            "then streaming support");
    click(app, "confirm-accept");
    require(!app.view().settings && app.streaming_support() && saved(app).favorites.empty(),
            "Discard then Turn on: streaming support is saved, the languages are not");
    open();
    click(app, "streaming-support");
    click(app, "apply");
    app.show_settings();
    add_language(app, "International", "German, QWERTZ");
    click(app, "streaming-support");
    click(app, "confirm-accept");
    app.back();
    click(app, "confirm-accept");
    require(app.view().confirm && app.view().confirm->title == "Turn on streaming support?",
            "Save languages also leads to the second question");
    click(app, "confirm-cancel");
    require(!app.view().settings && !app.streaming_support() && saved(app).favorites.size() == 1,
            "Save then Leave off: the languages are saved, streaming support is not");
    require(sink.events.empty(), "streaming support settings send no desktop input");
}
// The theme picker (Settings → Theme): a card per theme, their pictures kept
// on disk under the theme's hash, a card choosing its theme at once, ← keeping
// the choice and Back from Settings undoing it.
void theme_picker() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path / "config";
    options.theme_previews_dir = directory.path / "previews";
    options.mode = Mode::Render;
    options.input = InputBackend::Ei;
    CaptureSink sink;
    const auto saved = [&] {
        std::map<fs::path, fs::file_time_type> files;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(options.theme_previews_dir, error)) {
            if (entry.path().extension() == ".argb") {
                files[entry.path()] = fs::last_write_time(entry.path());
            }
        }
        return files;
    };
    const auto pictured = [](const PanelView& view) {
        return view.themes && !view.themes->cards.empty() &&
               std::all_of(view.themes->cards.begin(), view.themes->cards.end(),
                           [](const auto& card) { return card.preview != nullptr; });
    };
    const auto themes = load_profiles({}, {}).themes.size();
    const auto index = options.theme_previews_dir / "index.txt";
    const auto index_lines = [&] {
        std::ifstream in(index);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
        }
        return lines;
    };
    // Pictures from before the index (another version's, as far as it can
    // tell) go when it is first read; a file not of ours stays.
    fs::create_directories(options.theme_previews_dir);
    const auto stale = options.theme_previews_dir / "graphite~0123.argb";
    const auto png = options.theme_previews_dir / "graphite~0123.png";
    const auto other = options.theme_previews_dir / "notes.txt";
    std::ofstream(stale) << "old";
    std::ofstream(png) << "old";
    std::ofstream(other) << "other";
    std::map<fs::path, fs::file_time_type> first;
    {
        App app(options, sink);
        app.show_settings();
        const auto row = control(app.view(), "theme");
        require(row.style == ControlStyle::Dropdown && row.icon == Icon::PopOut &&
                    row.label == "Graphite",
                "Theme's row looks like a dropdown named as the theme, with a pop-out icon");
        click(app, "theme");
        auto view = app.view();
        require(view.themes && view.modal_prefix() == "themes-" && !view.popup,
                "it opens the picker, not a menu");
        require(std::any_of(view.themes->cards.begin(), view.themes->cards.end(),
                            [](const auto& card) { return card.id == "graphite" && card.selected; }),
                "the chosen theme's card is in view and marked");
        double now = 10;
        for (int i = 0; i < 100; ++i) {
            app.tick(now += .02);
        }
        require(pictured(app.view()), "every card in view gets its picture");
        require(!fs::exists(stale) && !fs::exists(png) && fs::exists(other),
                "the first picker clears pictures from before its index, and only those");
        first = saved();
        require(first.size() == themes && fs::exists(options.theme_previews_dir / "graphite.argb"),
                "every theme's picture is saved, one file each, named by its ID");
        const auto lines = index_lines();
        require(lines.size() == 2 + themes && lines[1].starts_with("drawn by "),
                "the index records the version that drew them and a hash per theme");
        // Hidden, the pictures are freed and none load; shown, they come back (R285).
        app.set_interaction_active(false);
        for (int i = 0; i < 10; ++i) {
            app.tick(now += .25);
        }
        view = app.view();
        require(view.themes && std::none_of(view.themes->cards.begin(), view.themes->cards.end(),
                                            [](const auto& card) { return card.preview != nullptr; }),
                "a hidden picker keeps no pictures and loads none");
        app.set_interaction_active(true);
        require(pictured(app.view()),
                "shown again, the cards in view have their pictures at once (R298)");
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .02);
        }
        require(pictured(app.view()) && saved() == first,
                "shown again, the pictures load from disk, none drawn again");
        // With every picture in, a shell's theme arriving still gets one (R297).
        const std::string shell = R"({"schema_version":1,"id":"x","name":"Shell","surface":"#101010",
            "surface_radius":20,"key_top":"#303030","key_middle":"#282828","key_bottom":"#202020",
            "key_side_top":"#181818","key_side_bottom":"#101010","legend":"#ffffff","key_radius":8,
            "key_depth":3.5,"key_press_travel":3,"press_duration_ms":80,"hover":"#404040",
            "latched":"#335577","disabled":"#151515"})";
        require(app.host_theme_set(shell).first.empty(), "a shell's theme applies");
        for (int i = 0; i < 10; ++i) {
            app.tick(now += .02);
        }
        require(fs::exists(options.theme_previews_dir / "shell.argb"),
                "a theme arriving with every picture in still gets its picture");
        app.host_theme_reset();
        for (int i = 0; i < 5; ++i) {
            app.tick(now += .02);
        }
        first = saved();
        click(app, "themes-card-lagoon");
        view = app.view();
        require(view.themes && view.theme->id == "lagoon" &&
                    std::any_of(view.themes->cards.begin(), view.themes->cards.end(),
                                [](const auto& card) { return card.id == "lagoon" && card.selected; }),
                "a card chooses its theme at once, and the picker stays");
        app.back();
        view = app.view();
        require(!view.themes && view.settings && control(view, "theme").label == "Lagoon",
                "← closes the picker and keeps the choice");
        app.back();
        require(!app.view().settings && app.selection().theme == "graphite",
                "leaving Settings without Apply undoes it");
    }
    {
        App app(options, sink);
        app.show_settings();
        click(app, "theme");
        double now = 10;
        for (int i = 0; i < 100; ++i) {
            app.tick(now += .02);
        }
        require(pictured(app.view()) && saved() == first,
                "a second picker loads the saved pictures instead of drawing them again");
    }
    // Opens the picker scrolled to its end and lets every picture load.
    const auto picker_at_end = [&](App& app, double& now) {
        app.show_settings();
        click(app, "theme");
        const auto& well = themes_well_bounds;
        app.move(0, well.x + well.width / 2, well.y + well.height / 2);
        app.scroll(0, 0, 100);
        for (int i = 0; i < 100; ++i) {
            app.tick(now += .02);
        }
    };
    {
        // Another version's index: every picture is drawn again.
        auto lines = index_lines();
        const auto current = lines[1];
        lines[1] = "drawn by 0.0.1, 349x160, drawing 1";
        std::ofstream out(index);
        for (const auto& line : lines) {
            out << line << '\n';
        }
        out.close();
        App app(options, sink);
        app.show_settings();
        click(app, "theme");
        double now = 10;
        for (int i = 0; i < 5; ++i) {
            app.tick(now += .02);
        }
        require(index_lines().size() == 2,
                "while the pictures are drawn and saved the index waits (R294)");
        for (int i = 0; i < 100; ++i) {
            app.tick(now += .02);
        }
        require(index_lines().size() == 2 + themes, "once all are in, the index is written whole");
        const auto again = saved();
        require(again.size() == first.size() &&
                    std::all_of(again.begin(), again.end(),
                                [&](const auto& file) { return first.at(file.first) != file.second; }) &&
                    index_lines()[1] == current,
                "pictures another version drew are all drawn again, and the index takes this one");
        first = again;
    }
    {
        // A picture the index vouches for but that was drawn from something
        // else (here another theme's, as after a crash between saving it and
        // writing the index) is drawn again, not shown (R295).
        const auto graphite = options.theme_previews_dir / "graphite.argb";
        fs::copy_file(options.theme_previews_dir / "midnight.argb", graphite,
                      fs::copy_options::overwrite_existing);
        const auto copied = fs::last_write_time(graphite);
        App app(options, sink);
        app.show_settings();
        click(app, "theme");
        double now = 10;
        for (int i = 0; i < 100; ++i) {
            app.tick(now += .02);
        }
        std::ifstream a(graphite, std::ios::binary),
            b(options.theme_previews_dir / "midnight.argb", std::ios::binary);
        const std::string drawn((std::istreambuf_iterator<char>(a)), {}),
            midnight((std::istreambuf_iterator<char>(b)), {});
        require(fs::last_write_time(graphite) != copied && drawn != midnight,
                "a picture drawn from another theme is drawn again although the index vouches for it");
        first = saved();
    }
    {
        // A picture no index line lists (its save failed, or a crash came
        // before the index) and a crash's partial file go at the next run's
        // first picker; listed pictures and other files stay (R296).
        const auto orphan = options.theme_previews_dir / "zz-gone.argb";
        const auto partial = options.theme_previews_dir / "graphite.argb.12345.partial";
        std::ofstream(orphan) << "orphan";
        std::ofstream(partial) << "partial";
        App app(options, sink);
        app.show_settings();
        click(app, "theme");
        require(!fs::exists(orphan) && !fs::exists(partial) && saved() == first &&
                    fs::exists(options.theme_previews_dir / "notes.txt"),
                "files of ours no index line lists go, and nothing else");
    }
    const auto themes_dir = options.config_dir / "themes";
    fs::create_directories(themes_dir);
    const auto user_theme = [&](const std::string& id, const std::string& latched) {
        std::ofstream(themes_dir / (id + ".json"))
            << R"({"schema_version":1,"id":")" << id << R"(","name":")" << id
            << R"(","surface":"#101010","surface_radius":20,"key_top":"#303030",)"
               R"("key_middle":"#282828","key_bottom":"#202020","key_side_top":"#181818",)"
               R"("key_side_bottom":"#101010","legend":"#ffffff","key_radius":8,"key_depth":3.5,)"
               R"("key_press_travel":3,"press_duration_ms":80,"hover":"#404040",)"
               R"("latched":")"
            << latched << R"(","disabled":"#151515"})";
    };
    {
        // An edited theme of the user's own is drawn again, alone; a deleted
        // one's picture goes the next time a picker opens.
        const auto picture = options.theme_previews_dir / "zz-edit.argb";
        user_theme("zz-edit", "#335577");
        fs::file_time_type drawn;
        {
            App app(options, sink);
            double now = 10;
            picker_at_end(app, now);
            require(fs::exists(picture), "a theme of the user's own gets its picture");
            drawn = fs::last_write_time(picture);
        }
        user_theme("zz-edit", "#aa3355");
        {
            App app(options, sink);
            double now = 10;
            picker_at_end(app, now);
            auto now_saved = saved();
            require(fs::last_write_time(picture) != drawn, "an edited theme's picture is drawn again");
            now_saved.erase(picture);
            require(now_saved == first, "and no other theme's");
        }
        fs::remove(themes_dir / "zz-edit.json");
        {
            App app(options, sink);
            app.show_settings();
            click(app, "theme");
            const auto lines = index_lines();
            require(!fs::exists(picture) &&
                        std::none_of(lines.begin(), lines.end(),
                                     [](const auto& line) { return line.starts_with("zz-edit "); }),
                    "a deleted theme's picture and index line go when a picker opens");
        }
    }
    auto theme = load_profiles({}, {}).themes.at("graphite");
    const auto hash = theme_preview_hash(theme);
    ThemePreviewStore store; // no folder: nothing saved
    ThemePreviews previews;
    const auto profiles = load_profiles({}, {});
    previews.fill_one({&theme}, profiles, store);
    require(previews.find(theme) && !previews.fill_one({&theme}, profiles, store),
            "a theme with its picture needs nothing more");
    theme.latched.r += .1;
    require(theme_preview_hash(theme) != hash && hash.size() == 16 && !previews.find(theme) &&
                previews.fill_one({&theme}, profiles, store) && previews.find(theme),
            "an edited theme's picture is drawn again in memory too");
    // A reload that removes themes keeps the picker's end in place (R289),
    // and removes their pictures.
    {
        // Enough themes of the user's own for one more row of cards.
        const auto columns = static_cast<std::size_t>(theme_columns);
        const auto rows = [&](std::size_t count) { return (count + columns - 1) / columns; };
        std::size_t extra = 1;
        while (rows(themes + extra) == rows(themes)) {
            ++extra;
        }
        for (std::size_t i = 0; i < extra; ++i) {
            user_theme("zz-user-" + std::to_string(i), "#335577");
        }
        App app(options, sink);
        double now = 50;
        picker_at_end(app, now);
        const auto& well = themes_well_bounds;
        require(fs::exists(options.theme_previews_dir / "zz-user-0.argb"),
                "the user's themes get pictures");
        for (std::size_t i = 0; i < extra; ++i) {
            fs::remove(themes_dir / ("zz-user-" + std::to_string(i) + ".json"));
        }
        app.reload();
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .02);
        }
        const auto view = app.view();
        require(
            view.themes && view.themes->thumb &&
                view.themes->thumb->y + view.themes->thumb->height <= well.y + well.height - 8 + .5 &&
                std::any_of(view.themes->cards.begin(), view.themes->cards.end(),
                            [&](const auto& card) {
                                return card.bounds.y + card.bounds.height > well.y + well.height - 20;
                            }),
            "after a reload removes a row of themes, the picker still ends at its last row");
        require(!fs::exists(options.theme_previews_dir / "zz-user-0.argb"),
                "and their pictures are removed");
    }
}
void fractional_dropdown_scroll() {
    SettingsUi ui;
    SettingField field{"field", "Field", "c0"};
    for (int i = 0; i < 20; ++i) {
        field.choices.push_back({"c" + std::to_string(i), "Choice " + std::to_string(i)});
    }
    const std::vector<SettingsCard> cards{{"card", "Card", {field}}};
    ui.set_cards(cards);
    auto snapshot = [&] {
        PanelView view;
        ui.append(view);
        return view;
    };
    ui.toggle("field");
    const auto popup = *snapshot().popup;
    auto scroll = [&](double dy) { return ui.scroll(popup.x + 5, popup.y + 5, 0, dy); };
    for (int i = 0; i < 100; ++i) {
        scroll(.1);
    }
    require(has(snapshot(), "choose:field:c10") && !has(snapshot(), "choose:field:c9"),
            "one hundred fractional events move ten rows");
    for (int i = 0; i < 100; ++i) {
        scroll(-.1);
    }
    require(has(snapshot(), "choose:field:c0"), "negative fractional input returns to first row");
    scroll(1e300);
    scroll(.9); // Overscroll must not delay reversing direction.
    require(scroll(-1) && has(snapshot(), "choose:field:c14"), "reverse responds at bottom limit");
    scroll(-1e300);
    scroll(-.9);
    require(scroll(1) && !has(snapshot(), "choose:field:c0"), "reverse responds at top limit");
    require(!scroll(std::numeric_limits<double>::quiet_NaN()) &&
                !scroll(std::numeric_limits<double>::infinity()),
            "nonfinite fractional input is ignored");
    ui.close_popup();
    ui.toggle("field");
    scroll(.6);
    ui.close_popup();
    ui.toggle("field");
    require(!scroll(.5) && has(snapshot(), "choose:field:c0"), "new menu discards old remainder");
    require(scroll(.5), "new menu accumulates its own fractional motion");
    scroll(.6);
    ui.set_cards(cards);
    require(!scroll(.5), "profile changes discard fractional remainder");
    ui.close_popup();
    ui.toggle("field");
    scroll(.6);
    const auto bar = control(snapshot(), "scroll-menu");
    require(ui.down(0, bar.bounds.x + 2, bar.bounds.y + 2), "menu scrollbar captures pointer");
    ui.up(0);
    require(!scroll(.5), "scrollbar capture discards wheel remainder");
}
// A drag on an open list scrolls it a row at a time, as one on the cards
// scrolls them, and its release chooses nothing.
void drag_dropdown() {
    SettingsUi ui;
    SettingField field{"field", "Field", "c0"}, other{"other", "Other", "o0"};
    for (int i = 0; i < 20; ++i) {
        field.choices.push_back({"c" + std::to_string(i), "Choice " + std::to_string(i)});
        other.choices.push_back({"o" + std::to_string(i), "Other " + std::to_string(i)});
    }
    ui.set_cards({{"card", "Card", {field, other}}});
    auto snapshot = [&] {
        PanelView view;
        ui.append(view);
        return view;
    };
    ui.toggle("field");
    const auto popup = *snapshot().popup;
    const double x = popup.x + 40, y = popup.y + popup.height - 20;
    require(!ui.down(0, x, y), "a press on the list leaves its choice the press");
    require(!ui.move(0, x, y - 8), "a small move is still a tap");
    require(ui.move(0, x, y - 3 * 52), "past the dead zone, the rows follow the laser");
    require(has(snapshot(), "choose:field:c3") && !has(snapshot(), "choose:field:c2"),
            "dragged up three rows, the list shows from the fourth");
    require(ui.move(0, x, y + 600) && has(snapshot(), "choose:field:c0"), "and stops at its top");
    require(ui.up(0) && ui.popup_open(), "the release chooses nothing, and the list stays open");
    // The stick scrolls under a held press, and the drag goes on from there.
    require(!ui.down(0, x, y) && ui.scroll(popup.x + 5, popup.y + 5, 0, 4) &&
                has(snapshot(), "choose:field:c4"),
            "the stick scrolls the list under a held press");
    require(ui.move(0, x, y - 52) && has(snapshot(), "choose:field:c5") &&
                !has(snapshot(), "choose:field:c4"),
            "and the drag goes on from where the stick left it");
    ui.up(0);
    // A drag ends with its list: the next one opened doesn't follow it.
    require(!ui.down(0, x, y), "a press on the list");
    ui.close_popup();
    ui.toggle("other");
    require(!ui.move(0, x, y - 3 * 52) && has(snapshot(), "choose:other:o0"),
            "the next list opened doesn't follow the old drag");
    require(!ui.up(0), "whose release is nothing either");
}
void scroll_cards() {
    SettingsUi ui;
    std::vector<SettingsCard> cards;
    for (int i = 0; i < 5; ++i) {
        SettingsCard card{"test-" + std::to_string(i), "Test card " + std::to_string(i), {}};
        for (int j = 0; j < 10; ++j) {
            SettingField field{card.id + "-field-" + std::to_string(j), "Test field:", "choice-0"};
            for (int k = 0; k < 12; ++k) {
                field.choices.push_back({"choice-" + std::to_string(k), "Choice " + std::to_string(k)});
            }
            card.fields.push_back(std::move(field));
        }
        cards.push_back(std::move(card));
    }
    ui.set_cards(cards);
    double now = 0;
    // Steps ease the cards toward their target; let them arrive.
    auto settle = [&] {
        for (int i = 0; i < 40; ++i) {
            ui.tick(now += .05);
        }
    };
    auto snapshot = [&] {
        settle();
        PanelView view;
        ui.append(view);
        return view;
    };
    auto view = snapshot();
    require(view.cards.size() == 4 && !has(view, "test-3-field-0"),
            "offscreen cards have no hit controls");
    require(!has(view, "test-0-field-9"), "vertically clipped fields have no controls");
    require(ui.scroll(200, 200, 0, 2), "card scrolls vertically");
    {
        PanelView moving;
        ui.tick(now += .016);
        ui.append(moving);
        require(has(moving, "test-0-field-0"), "a step glides instead of jumping");
        require(ui.tick(now += .016), "and keeps moving on the next frame");
    }
    view = snapshot();
    require(!has(view, "test-0-field-0") && has(view, "test-1-field-0"),
            "each card owns its vertical offset");
    const auto partial = control(view, "test-0-field-1");
    require(!partial.hit(partial.bounds.x + 2, partial.bounds.y + 2), "clipped portion cannot be hit");
    require(ui.scroll(200, 200, 1000, 0), "cards scroll horizontally");
    view = snapshot();
    require(!has(view, "test-0-field-1") && has(view, "test-4-field-0"),
            "horizontal scroll clips old cards and exposes new cards");
    require(!ui.scroll(200, 200, 1000, 0), "horizontal offset is bounded");
    require(ui.scroll(1200, 200, 0, 1000), "last card scrolls to its final rows");
    view = snapshot();
    require(has(view, "test-4-field-9") && !has(view, "test-4-field-0"),
            "vertical offset is bounded at last field");
    ui.reset();
    view = snapshot();
    const auto bar = control(view, "scroll-cards");
    const double x = bar.bounds.x + bar.bounds.width / 2, y = bar.bounds.y + 5;
    require(ui.down(1, x, y), "trigger captures horizontal scrollbar");
    require(!ui.move(2, x + 200, y), "other controller cannot move owned scrollbar");
    require(ui.move(1, x + 200, y), "captured controller scrolls cards");
    require(ui.pressed(1), "scrollbar ownership exposed for tracking loss cleanup");
    ui.cancel_pointer(1);
    require(!ui.move(1, x, y), "cancel stops scrollbar movement");
    ui.reset();
    // The laser drags the cards like a touch screen, past a dead zone.
    require(!ui.down(4, 600, 300), "a press on a card leaves its control pressed");
    require(!ui.move(4, 595, 300), "a small movement is still a press");
    require(ui.move(4, 60, 300), "a longer one scrolls the row sideways");
    view = snapshot();
    require(!has(view, "test-0-field-1"), "the row followed the laser");
    require(ui.up(4), "the drag's release is not a click");
    require(!ui.down(4, 200, 300) && !ui.up(4), "a plain press and release stays a click");
    ui.reset();
    ui.toggle("test-0-field-4");
    view = snapshot();
    require(view.popup && view.popup->y + view.popup->height > panel_height,
            "dropdown can extend beyond keyboard border");
    require(view.popup->height == 5 * 52, "overflow popup still shows five choices");
    const auto outside_case = control(view, "choose:test-0-field-4:choice-4");
    require(outside_case.hit(outside_case.bounds.x + 5, outside_case.bounds.y + 5),
            "overflow option remains clickable");
    require(outside_case.action.value == "choice-4", "overflow option resolves to choice");
    require(!ui.scroll(900, 200, 1, 1), "open popup blocks underlying card scrolling");
    require(ui.scroll(view.popup->x + 5, view.popup->y + 5, 0, 100),
            "popup scroll reaches last choices");
    view = snapshot();
    require(has(view, "choose:test-0-field-4:choice-11"), "all dropdown choices are reachable");
    const auto menu_bar = control(view, "scroll-menu");
    require(ui.down(0, menu_bar.bounds.x + 2, menu_bar.bounds.y + 2),
            "popup scrollbar captures pointer");
    ui.cancel();
    require(!ui.popup_open() && !ui.pressed(0), "cancel closes popup and releases scrollbar");

    // Paint real extra cards/menu through the production renderer and check
    // alpha at the escaped menu and beyond the card viewport.
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    ui.reset();
    ui.toggle("test-0-field-4");
    view = snapshot();
    view.settings = true;
    view.layout = &profiles.layouts.at("en-us-full");
    view.language = &profiles.languages.at("en-us");
    view.theme = &profiles.themes.at("graphite");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    PanelRenderer renderer;
    renderer.paint(view, 1);
    const auto pixels = renderer.rgba();
    auto alpha = [&](int px, int py) {
        return pixels[static_cast<std::size_t>((py + popup_margin) * panel_width + px) * 4 + 3];
    };
    require(alpha(300, 700) == 255, "escaped popup pixels render outside case");
    require(alpha(900, 700) == 0, "outside popup margin stays transparent");
    renderer.write_png("/tmp/framekeyboard-settings-scroll-popup.png");
    ui.close_popup();
    view = snapshot();
    view.settings = true;
    view.layout = &profiles.layouts.at("en-us-full");
    view.language = &profiles.languages.at("en-us");
    view.theme = &profiles.themes.at("graphite");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    renderer.paint(view, 2);
    const auto clean = renderer.rgba();
    require(clean[static_cast<std::size_t>((700 + popup_margin) * panel_width + 300) * 4 + 3] == 0,
            "closing popup clears overflow pixels");
    renderer.write_png("/tmp/framekeyboard-settings-scroll-cards.png");
}
} // namespace
// The shortcut bar (docs/shortcut-bar-design.md): its Settings card, the
// toolbar button's place and three cases, the picker's menu, scrolling, a
// button's keys and what is saved.
Rect middle(const Rect& r) {
    return {r.x + r.width / 2, r.y + r.height / 2, 0, 0};
}
void tap_at(App& app, double x, double y, unsigned pointer = 0) {
    static double now = 700;
    app.move(pointer, x, y);
    app.down(pointer, x, y, now += .1);
    app.up(pointer, x, y);
    app.tick(now += .02);
}
void tap_rect(App& app, const Rect& r) {
    const auto point = middle(r);
    tap_at(app, point.x, point.y);
}
ShortcutModel saved_shortcuts(App& app) {
    app.flush_settings();
    std::ifstream file(shortcuts_path(app.config_dir()));
    return parse_shortcuts({std::istreambuf_iterator<char>(file), {}});
}
void write_shortcuts(const fs::path& dir, const ShortcutModel& model) {
    std::ofstream(shortcuts_path(dir)) << serialize_shortcuts(model);
}
ShortcutButton shortcut_button(std::string label, std::vector<ShortcutChord> chords) {
    ShortcutButton button;
    button.label = std::move(label);
    button.action.chords = std::move(chords);
    return button;
}
void shortcut_bar_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    options.debug = true;
    CaptureSink sink;
    {
        App app(options, sink);
        require(!has(app.view(), "bar-toggle") && !app.view().shortcut_bar,
                "Shortcuts and macros starts off: no button and no bar");
        app.show_settings();
        click(app, "shortcuts");
        const auto row = control(app.view(), "shortcuts"), numpad = control(app.view(), "hide-numpad");
        require(row.bounds.x == numpad.bounds.x && row.bounds.y > numpad.bounds.y &&
                    app.view().cards[1].title == "General",
                "its switch is a General row after Hide numpad");
        require(row.selected && !saved(app).shortcuts, "the switch is pending until Apply");
        click(app, "apply");
        require(saved(app).shortcuts && saved(app).shortcuts_switch,
                "Apply saves it; the toolbar's own switch is on");
        // The toolbar button after Pull for Shift's place, before the language.
        auto view = app.view();
        const auto toggle = control(view, "bar-toggle");
        require(toggle.bounds.y == 12 && toggle.bounds.x == control(view, "passthrough").bounds.x + 70,
                "the button follows the toolbar's switches on a full case");
        // Turned on with no bars, the empty bar shows once.
        require(view.shortcut_bar && view.shortcut_bar->slots && view.shortcut_bar->buttons.empty() &&
                    toggle.icon == Icon::ShortcutBar && toggle.label == "Hide shortcut bar",
                "turned on with no bars, the empty bar shows, so the picker can be reached");
        const auto plate = view.shortcut_bar->plate;
        const auto body = panel_case_bounds(view);
        require(plate.y + plate.height == body.y - 10 && plate.height == 90,
                "the plate stands 10 px above the case, 90 tall");
        // A bar shows: it hides, and the switch is saved off.
        click(app, "bar-toggle");
        require(!app.view().shortcut_bar && !saved(app).shortcuts_switch &&
                    control(app.view(), "bar-toggle").icon == Icon::ShortcutBarOff &&
                    control(app.view(), "bar-toggle").label == "Show shortcut bar",
                "pressed while a bar shows, it hides, crossed, and stays off");
        // Nothing shows and no bar is due: the empty bar with two slots.
        click(app, "bar-toggle");
        view = app.view();
        require(view.shortcut_bar && view.shortcut_bar->slots && saved(app).shortcuts_switch,
                "pressed with no bar due, the empty bar shows");
        app.show_settings();
        require(!app.view().shortcut_bar, "the bar hides in Settings");
        // R322: ✓ with the shortcut switches unchanged keeps the empty bar.
        click(app, "apply");
        view = app.view();
        require(view.shortcut_bar && view.shortcut_bar->slots && view.shortcut_bar->buttons.empty(),
                "Settings' ✓ keeps the empty bar");
        app.show_settings();
#ifdef FRAMEKEYBOARD_DEBUG_MODE
        // The Debug card's sample bars, saved at once.
        click(app, "debug-sample-shortcuts");
        const auto samples = saved_shortcuts(app);
        require(samples.profiles.size() == 3 && samples.profiles[1].buttons.size() == 18 &&
                    samples.free_profile == samples.profiles[0].id && samples.apps.size() == 1 &&
                    samples.apps[0].profile == samples.profiles[2].id,
                "Add sample shortcuts saves three profiles, the first the free one, the last assigned");
        require(!control(app.view(), "debug-sample-shortcuts").enabled, "and only while there are none");
#else
        app.back();
        return;
#endif
        app.back();
        require(app.view().shortcut_bar && app.view().shortcut_bar->buttons.size() == 8,
                "the sample bar shows");
    }
    App app(options, sink);
    auto view = app.view();
    require(view.shortcut_bar && view.shortcut_bar->buttons.size() == 8 &&
                view.shortcut_bar->buttons[0].label == "Undo" &&
                view.shortcut_bar->buttons[4].label == "Select\nall",
            "a restart loads the bars; the free bar shows");
    const auto picker = view.shortcut_bar->picker;
    const auto spot = middle(picker);
    app.move(0, spot.x, spot.y);
    require(app.view().bar_tooltip == "Choose a profile", "the picker's tooltip");
    tap_rect(app, picker);
    view = app.view();
    require(view.shortcut_bar->menu && view.shortcut_bar->menu->rows.size() == 3 &&
                view.shortcut_bar->menu->rows[0].shown && !view.shortcut_bar->menu->rows[1].shown &&
                view.shortcut_bar->menu->title == "Profiles" &&
                view.shortcut_bar->menu->manage_label == "Manage" &&
                !view.shortcut_bar->menu->rows[0].assign_enabled,
            "the picker opens its menu: the profile showing filled, Manage in its header; no app to "
            "assign");
    {
        // Manage sits in the header, as tall as an assign button; no Edit
        // shortcuts row under the list.
        const auto& m = *view.shortcut_bar->menu;
        require(m.header.contains(m.manage.x + 1, m.manage.y + 1) &&
                    m.manage.x + m.manage.width == m.header.x + m.header.width &&
                    m.manage.height >= m.rows[0].assign.height &&
                    m.bounds.y + m.bounds.height <= m.list.y + m.list.height + 8.5,
                "Manage at the header's right; the menu ends with its last row");
        const auto at = middle(m.manage);
        app.move(0, at.x, at.y);
        require(app.view().bar_tooltip == "Edit profiles and buttons" &&
                    app.view().shortcut_bar->menu->manage_hovered,
                "Manage's hover and tooltip");
    }
    // R319: the bar's plate, the picker's tab and the menu above the case
    // take the pointer, as the case does; the margin above the plate doesn't,
    // nor the place right of the tab where the picker's own plate was.
    {
        const auto body = panel_case_bounds(view);
        const auto& bar = *view.shortcut_bar;
        const auto plate = middle(bar.plate), tab = middle(bar.picker);
        const auto menu = bar.menu->bounds;
        require(bar.picker.x == bar.plate.x + bar.plate.width && bar.picker.width == 44 &&
                    bar.picker.height == 56 &&
                    bar.picker.y == bar.plate.y + (bar.plate.height - bar.picker.height) / 2,
                "the picker is a 44 × 56 tab on the plate's right side, centered on its height");
        require(menu.y == body.y - 4 && !body.contains(menu.x + 10, body.y - 2) &&
                    takes_pointer(view, menu.x + 10, body.y - 2) &&
                    takes_pointer(view, plate.x, plate.y) && takes_pointer(view, tab.x, tab.y) &&
                    !takes_pointer(view, plate.x, bar.plate.y - 20),
                "the bar's plate, its tab and its menu take the pointer, above the case");
        require(std::abs(menu.x + menu.width - (bar.picker.x + bar.picker.width)) < .5,
                "the menu's right edge is the tab's");
        require(
            !takes_pointer(view, bar.picker.x + bar.picker.width + 20, tab.y) &&
                !takes_pointer(view, tab.x, bar.plate.y + 4),
            "right of the tab and above it, where the picker's plate was, nothing takes the pointer");
    }
    // A click outside the menu closes it and goes nowhere else.
    const auto [kx, ky] = key_point(app, "Space");
    tap_at(app, kx, ky);
    require(!app.view().shortcut_bar->menu && sink.events.empty(), "a click outside closes the menu");
    tap_rect(app, picker);
    const auto row = app.view().shortcut_bar->menu->rows[1].bounds;
    tap_at(app, row.x + 30, row.y + row.height / 2);
    view = app.view();
    require(!view.shortcut_bar->menu && view.shortcut_bar->buttons.size() == 15 &&
                view.shortcut_bar->buttons[0].label == "Home" && view.shortcut_bar->more_after &&
                !view.shortcut_bar->more_before,
            "a bar chosen shows, the menu closed; 15 slots, the rest scroll");
    require(saved_shortcuts(app).free_profile == "sample-keys",
            "a free bar chosen is saved as the free bar");
    // The stick scrolls a button at a time, eased.
    const auto first = view.shortcut_bar->buttons[0].bounds;
    const auto over = middle(view.shortcut_bar->plate);
    app.move(0, over.x, over.y);
    require(app.scroll_room(0) == ScrollRoom{false, true}, "the bar can scroll forward only");
    require(app.scroll(0, 1, 0), "a stick step scrolls the bar");
    for (double now = 800; now < 801; now += .05) {
        app.tick(now);
    }
    view = app.view();
    const double pitch = view.shortcut_bar->buttons[1].bounds.x - view.shortcut_bar->buttons[0].bounds.x;
    require(view.shortcut_bar->buttons[0].label == "End" &&
                std::abs(view.shortcut_bar->buttons[0].bounds.x - first.x) < 1 &&
                view.shortcut_bar->more_before,
            "one step moves one button");
    require(pitch > 70 && pitch < 90, "buttons are left Ctrl's size, 6 units apart");
    // A press dragged past 12 px scrolls instead of acting.
    const auto press = middle(view.shortcut_bar->buttons[2].bounds);
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 802);
    app.move(0, press.x - 5, press.y);
    app.move(0, press.x - 60, press.y);
    app.up(0, press.x - 60, press.y);
    view = app.view();
    require(view.shortcut_bar->buttons[0].bounds.x < first.x - 30, "a drag scrolls the bar");
    // Each bar keeps its own place. This bar is wider: its picker moved.
    tap_rect(app, view.shortcut_bar->picker);
    const auto back = app.view().shortcut_bar->menu->rows[0].bounds;
    tap_at(app, back.x + 30, back.y + back.height / 2);
    require(app.view().shortcut_bar->buttons[0].label == "Undo", "the other bar from its start");
    // Manage opens the editor, and ← without changes returns.
    tap_rect(app, app.view().shortcut_bar->picker);
    tap_rect(app, app.view().shortcut_bar->menu->manage);
    require(app.view().shortcut_editor && !app.view().settings, "Manage opens the editor");
    tap_rect(app, control(app.view(), "shortcuts-back").bounds);
    require(!app.view().shortcut_editor && app.view().shortcut_bar && !app.view().shortcut_bar->menu,
            "← returns to the keyboard, the menu closed");
    // Nothing shows and a bar is due: that bar.
    click(app, "bar-toggle");
    require(!app.view().shortcut_bar, "hidden by hand");
    click(app, "bar-toggle");
    require(app.view().shortcut_bar && app.view().shortcut_bar->buttons[0].label == "Undo",
            "pressed with a bar due, that bar shows");
    // A compact case: the button on the second row.
    {
        Settings settings = saved(app);
        settings.hide_numpad = true;
        TemporaryDirectory narrow;
        save_settings(narrow.path, settings);
        Options compact = options;
        compact.config_dir = narrow.path;
        App small(compact, sink);
        const auto v = small.view();
        require(control(v, "bar-toggle").bounds.y == second_row_top &&
                    control(v, "bar-toggle").bounds.x == control(v, "passthrough").bounds.x + 70,
                "on a narrow case the button joins the second row");
        const auto b = shortcut_bar_geometry(v, 30);
        require(b.slots == 15 && b.picker.x + b.picker.width - b.plate.x <= panel_case_bounds(v).width,
                "with the numpad hidden the bar keeps its 15 slots, within the case");
    }
}
// A button's keys go out through the output queue, and the buttons that
// can't send are drawn disabled.
void shortcut_buttons() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    ShortcutButton macro;
    macro.label = "Macro";
    macro.action.kind = ShortcutAction::Kind::Macro;
    macro.action.steps.push_back({MacroStep::Kind::Wait, {}, 100});
    ShortcutButton run;
    run.label = "Run";
    run.action.kind = ShortcutAction::Kind::Run;
    run.action.command = "true";
    model.profiles.push_back({"b",
                              "Bar",
                              {shortcut_button("Two", {{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}}),
                               shortcut_button("Empty", {}), macro, run}});
    model.free_profile = "b";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    CaptureSink sink;
    App app(options, sink);
    app.tick(900);
    auto view = app.view();
    const auto& buttons = view.shortcut_bar->buttons;
    require(buttons.size() == 4 && buttons[0].enabled && !buttons[1].enabled && buttons[2].enabled &&
                buttons[3].enabled,
            "a macro and Run are enabled; an empty action is disabled");
    require(!buttons[0].run && !buttons[1].run && !buttons[2].run && buttons[3].run,
            "only the Run button has the >_ mark");
    const auto press = middle(buttons[0].bounds);
    app.move(0, press.x, press.y);
    require(app.down(0, press.x, press.y, 900.1), "a button's press feels like a key's");
    require(sink.events.empty(), "it acts on release");
    require(app.up(0, press.x, press.y), "and its release too");
    require(app.view().shortcut_bar->buttons[0].running, "a running button stays lit");
    for (double now = 900.2; now < 900.5; now += .01) {
        app.tick(now);
    }
    require(
        sink.events ==
            std::vector<std::pair<int, int>>{
                {KEY_LEFTCTRL, 1}, {KEY_K, 1}, {KEY_K, 0}, {KEY_LEFTCTRL, 0}, {KEY_S, 1}, {KEY_S, 0}},
        "its combinations go out in order");
    require(!app.view().shortcut_bar->buttons[0].running, "and its light goes out");
    // A latched Ctrl is cleared, not added.
    sink.events.clear();
    const auto [cx, cy] = key_point(app, "ControlLeft");
    app.down(1, cx, cy, 901);
    app.up(1, cx, cy);
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 901.1);
    app.up(0, press.x, press.y);
    for (double now = 901.2; now < 901.5; now += .01) {
        app.tick(now);
    }
    require(sink.events.size() == 6 && sink.events[0] == std::pair{KEY_LEFTCTRL, 1} &&
                sink.events[4] == std::pair{KEY_S, 1},
            "a latched Ctrl joins nothing");
    // A press on a disabled button does nothing.
    sink.events.clear();
    const auto empty = middle(app.view().shortcut_bar->buttons[1].bounds);
    app.move(0, empty.x, empty.y);
    require(!app.down(0, empty.x, empty.y, 902), "a disabled button gives no press");
    app.up(0, empty.x, empty.y);
    app.tick(902.2);
    require(sink.events.empty(), "and sends nothing");
    // R324: a toolbar button pressed before the gap ends stops the run; the
    // first combination, already queued, finishes.
    const auto passthrough = middle(control(app.view(), "passthrough").bounds);
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 902.5);
    app.up(0, press.x, press.y);
    app.move(1, passthrough.x, passthrough.y);
    app.down(1, passthrough.x, passthrough.y, 902.51);
    app.up(1, passthrough.x, passthrough.y);
    for (double now = 902.52; now < 902.9; now += .01) {
        app.tick(now);
    }
    require(sink.events ==
                    std::vector<std::pair<int, int>>{
                        {KEY_LEFTCTRL, 1}, {KEY_K, 1}, {KEY_K, 0}, {KEY_LEFTCTRL, 0}} &&
                !app.view().shortcut_bar->buttons[0].running,
            "a toolbar button stops a run: one combination sent");
    app.move(1, passthrough.x, passthrough.y);
    app.down(1, passthrough.x, passthrough.y, 902.95);
    app.up(1, passthrough.x, passthrough.y);
    sink.events.clear();
    // Hiding stops a run: the second combination never goes.
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 903);
    app.up(0, press.x, press.y);
    app.set_interaction_active(false);
    for (double now = 903.1; now < 903.5; now += .01) {
        app.tick(now);
    }
    require(std::none_of(sink.events.begin(), sink.events.end(),
                         [](const auto& event) { return event.first == KEY_S; }),
            "hiding stops a run");
    // A Steam field takes none of it: every button is disabled there.
    struct SteamField : CaptureSink {
        bool steam_field_target() const override { return true; }
    } field;
    App steam(options, field);
    const auto shown = steam.view().shortcut_bar->buttons;
    require(std::none_of(shown.begin(), shown.end() - 1, [](const auto& b) { return b.enabled; }) &&
                shown.back().enabled,
            "key buttons are disabled in a Steam field; Run, which types nothing, is not");
    // What an assign button says, hovered in the picker menu.
    const auto assign_tooltip = [](App& on) {
        on.tick(905);
        tap_rect(on, on.view().shortcut_bar->picker);
        const auto at = middle(on.view().shortcut_bar->menu->rows[0].assign);
        on.move(0, at.x, at.y);
        return on.view().bar_tooltip;
    };
    require(assign_tooltip(steam) ==
                "Can't assign here: this is one of Steam's own text fields, not an app",
            "in a Steam field the assign buttons say so");
    // R357: a game or app streamed from a PC with Streaming support takes the
    // keyboard's own key presses, so its Shortcut and Macro buttons work. It
    // is still reached through Steam (steam_field_target), with no window
    // and no app reported.
    struct StreamSession : CaptureSink {
        bool steam_field_target() const override { return true; }
        bool stream_session_target() const override { return true; }
    } stream_sink;
    App stream(options, stream_sink);
    stream.tick(904);
    const auto streamed = stream.view().shortcut_bar->buttons;
    require(streamed[0].enabled && !streamed[1].enabled && streamed[2].enabled && streamed[3].enabled,
            "in a stream Shortcut, Macro and Run buttons are enabled; an empty one is not");
    const auto at = middle(streamed[0].bounds);
    stream.move(0, at.x, at.y);
    stream.down(0, at.x, at.y, 904.1);
    stream.up(0, at.x, at.y);
    for (double now = 904.2; now < 904.5; now += .01) {
        stream.tick(now);
    }
    require(
        stream_sink.events ==
            std::vector<std::pair<int, int>>{
                {KEY_LEFTCTRL, 1}, {KEY_K, 1}, {KEY_K, 0}, {KEY_LEFTCTRL, 0}, {KEY_S, 1}, {KEY_S, 0}},
        "a press in a stream sends its combinations' downs and ups in order");
    // Without a streaming client to read, it is the PC's stream (R359).
    require(assign_tooltip(stream) == "Show Bar whenever your PC's desktop stream is in front",
            "a stream is no Steam field: it can be assigned");
}
// R321: a button's press belongs to the bar it began on. Another window
// takes the keys while it is held and the free bar comes back: letting go in
// place sends nothing.
void shortcut_press_bar_change() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"t", "Tied", {shortcut_button("T", {{{"ControlLeft"}, "KeyT"}})}});
    model.apps.push_back({{AppIdentity::Kind::Program, "app"}, "App", "t"});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct WindowSink : CaptureSink {
        std::uint32_t window{5};
        std::uint32_t focused_window() const override { return window; }
    } sink;
    App app(options, sink);
    app.tick(900);
    tap_rect(app, app.view().shortcut_bar->picker);
    const auto row = app.view().shortcut_bar->menu->rows[1].bounds;
    tap_at(app, row.x + 30, row.y + row.height / 2);
    require(app.view().shortcut_bar->buttons[0].label == "T", "the picked bar shows");
    const auto press = middle(app.view().shortcut_bar->buttons[0].bounds);
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 900.5);
    sink.window = 6;
    app.tick(900.6);
    require(app.view().shortcut_bar->buttons[0].label == "A", "another window brings the free bar back");
    app.up(0, press.x, press.y);
    for (double now = 900.7; now < 901.2; now += .01) {
        app.tick(now);
    }
    require(sink.events.empty(), "the press held over the change sends nothing on release");
    // A press begun on the bar now showing still acts.
    app.move(0, press.x, press.y);
    app.down(0, press.x, press.y, 901.3);
    app.up(0, press.x, press.y);
    for (double now = 901.4; now < 901.9; now += .01) {
        app.tick(now);
    }
    require(sink.events.size() == 4 && sink.events[1] == std::pair{KEY_A, 1},
            "a press on the bar showing sends its own keys");
}
// The shortcut editor (docs/shortcut-bar-design.md, Settings and the
// editor): opened from the picker, its bars column and strip with drags,
// the Label and bar name fields and recording on the keyboard, the limits,
// ✓ and ← with its question, at both widths.
void tap_control(App& app, const std::string& id) {
    tap_rect(app, control(app.view(), id).bounds);
}
void tap_key(App& app, const std::string& action) {
    const auto [x, y] = key_point(app, action);
    tap_at(app, x, y);
}
void open_editor(App& app) {
    tap_rect(app, app.view().shortcut_bar->picker);
    tap_rect(app, app.view().shortcut_bar->menu->manage);
}
// A press on one of the editor popup's mini keys, by the US key's ID.
void tap_mini(App& app, const std::string& key) {
    tap_rect(app, control(app.view(), "shortcuts-popup-key-" + key).bounds);
}
std::string popup_text(App& app) {
    return app.view().shortcut_editor->popup->text;
}
std::vector<std::string> popup_chips(const PanelView& view) {
    std::vector<std::string> chips;
    for (const auto& chip : view.shortcut_editor->popup->chords) {
        chips.push_back(chip.text);
    }
    return chips;
}
// The popup sits under the strip, centered, 1051 × 380 at either width, its
// keys inside it; a letter key is 42 × 48 with its side.
void popup_fits(const PanelView& view, bool narrow) {
    const auto& p = *view.shortcut_editor->popup;
    const auto body = panel_case_bounds(view);
    const auto& well = view.shortcut_editor->strip_well;
    require(std::abs(p.panel.width - 1051) < 1 && p.panel.height == 380 &&
                std::abs(p.panel.x + p.panel.width / 2 - (body.x + body.width / 2)) < .5 &&
                p.panel.y >= well.y + well.height && p.panel.y + p.panel.height <= body.y + body.height,
            narrow ? "the popup without the numpad: under the strip, centered, 1051 × 380, in the case"
                   : "the popup: under the strip, centered, 1051 × 380, in the case");
    require(std::abs(p.scale - .771) < .001, "the mini keyboard's scale is 0.771");
    const auto letter = control(view, "shortcuts-popup-key-KeyQ").bounds;
    require(std::abs(letter.width - 42) < .5 && std::abs(letter.height - 48) < .5,
            "a letter key is 42 × 48");
    for (const auto& c : view.controls) {
        if (c.id.starts_with("shortcuts-popup-key-")) {
            require(p.keys.x - .5 <= c.bounds.x &&
                        c.bounds.x + c.bounds.width <= p.keys.x + p.keys.width + .5 &&
                        c.bounds.y + c.bounds.height <= p.panel.y + p.panel.height,
                    (c.id + " lies in the popup").c_str());
        }
    }
    require(has(view, "shortcuts-popup-key-Numpad1") && has(view, "shortcuts-popup-key-F12") &&
                !has(view, "shortcuts-popup-key-Emoji") && !has(view, "shortcuts-popup-key-Copy") &&
                !has(view, "shortcuts-popup-key-Dictation"),
            "the numpad stays, the side column goes");
}
std::vector<std::string> row_names(const PanelView& view) {
    std::vector<std::string> names;
    for (const auto& c : view.controls) {
        if (c.id.starts_with("shortcuts-row-")) {
            names.push_back(c.label);
        }
    }
    return names;
}
std::vector<std::string> strip_labels(const PanelView& view) {
    std::vector<std::string> labels;
    for (const auto& c : view.controls) {
        if (c.id.starts_with("shortcuts-button-")) {
            labels.push_back(c.label);
        }
    }
    return labels;
}
void drag(App& app, const Rect& from, double dx, double dy, unsigned pointer = 0) {
    static double now = 950;
    const auto start = middle(from);
    app.move(pointer, start.x, start.y);
    app.down(pointer, start.x, start.y, now += .1);
    for (int step = 1; step <= 6; ++step) {
        app.move(pointer, start.x + dx * step / 6, start.y + dy * step / 6);
    }
    app.up(pointer, start.x + dx, start.y + dy);
    app.tick(now += .05);
}
// R323: the note that shortcuts.json can't be read goes once a save that
// replaced the file reports success, in the editor and in Settings.
void shortcut_invalid_note() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    std::ofstream(shortcuts_path(directory.path)) << "{\"schema_version\":1,\"profiles\":[{}]}";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    // The note is a line of text under the switch, not a control, below
    // the General card's fold until the card scrolls.
    const auto noted = [&] {
        const auto cards = app.view().cards;
        const auto general = std::find_if(cards.begin(), cards.end(),
                                          [](const auto& card) { return card.title == "General"; });
        require(general != cards.end(), "the General card is in view");
        const auto card = general->bounds;
        app.move(0, std::max(card.x, 0.0) + 40, 300);
        app.scroll(0, 0, 20);
        static double now = 960;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
        const auto labels = app.view().field_labels;
        return std::any_of(labels.begin(), labels.end(), [](const FieldLabel& label) {
            return label.text.starts_with("Your shortcuts file can't be read");
        });
    };
    app.show_settings();
    require(has(app.view(), "shortcuts") && noted(), "Settings notes the unreadable file");
    app.back();
    click(app, "bar-toggle");
    open_editor(app);
    require(app.view().shortcut_editor && !app.view().shortcut_editor->notice.empty(),
            "so does the editor");
    tap_control(app, "shortcuts-new-profile");
    tap_mini(app, "Escape");
    tap_control(app, "shortcuts-save");
    require(saved_shortcuts(app).profiles.size() == 1, "✓ saves the new bar");
    app.tick(950);
    app.show_settings();
    require(has(app.view(), "shortcuts") && !noted(), "the save that worked ends the note");
}
ShortcutModel editor_model() {
    ShortcutModel model;
    model.profiles.push_back(
        {"one",
         "One",
         {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}}), shortcut_button("B", {{{}, "KeyB"}}),
          shortcut_button("C", {{{}, "KeyC"}})}});
    model.profiles.push_back({"two", "Two", {}});
    model.apps.push_back({{AppIdentity::Kind::Program, "kate"}, "Kate", "one"});
    model.free_profile = "two";
    return model;
}
// The popup is the same whatever types: with the flick layout in use its
// keyboard is still the US one, numpad included, in the no-numpad width's
// editor; typing and recording there reach neither keys nor text.
void shortcut_popup_flick() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    settings.active = {"ja-flick-12", "ja-flick", "graphite"};
    save_settings(directory.path, settings);
    write_shortcuts(directory.path, editor_model());
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct TextCapture : CaptureSink {
        std::vector<std::string> texts;
        bool text_available() override { return true; }
        bool commit_text(const std::string& text) override {
            texts.push_back(text);
            return true;
        }
    } sink;
    App app(options, sink);
    app.tick(990);
    require(app.view().layout->id == "ja-flick-12", "the flick layout is in use");
    open_editor(app);
    tap_control(app, "shortcuts-row-0");
    tap_control(app, "shortcuts-label");
    const auto view = app.view();
    require(panel_case_bounds(view).width == 1326 && panel_case_bounds(view).height == settings_height,
            "the editor takes the no-numpad width, and Settings' height");
    popup_fits(view, true);
    tap_mini(app, "KeyH");
    tap_mini(app, "KeyI");
    tap_mini(app, "Numpad7");
    tap_mini(app, "NumpadAdd");
    tap_mini(app, "Enter");
    require(control(app.view(), "shortcuts-label").label == "Ahi7+",
            "US letters, whatever the language; the numpad types its digits and signs");
    tap_control(app, "shortcuts-record");
    tap_mini(app, "ControlLeft");
    tap_mini(app, "KeyS");
    tap_mini(app, "Numpad7");
    tap_control(app, "shortcuts-popup-done");
    require(app.view().shortcut_editor->chords.size() == 2 &&
                app.view().shortcut_editor->chords[0].text == "Ctrl + S" &&
                app.view().shortcut_editor->chords[1].text == "Num 7",
            "recorded by the key's name; a numpad key as itself, never as Home");
    require(sink.events.empty() && sink.texts.empty(),
            "nothing reaches the app while the popup is open");
}
// R331: four combinations of four keys each at the no-numpad width. Record
// and clear stay where a one-key shortcut has them, inside the case; the
// chips scroll in their strip, and the popup's row shows the newest chip.
void long_combinations() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    settings.hide_numpad = true;
    save_settings(directory.path, settings);
    const std::vector<std::string> all{"ControlLeft", "ShiftLeft", "AltLeft"};
    ShortcutModel model;
    model.profiles.push_back(
        {"b",
         "Bar",
         {shortcut_button("One", {{{}, "KeyA"}}),
          shortcut_button("Long", {{all, "KeyZ"}, {all, "KeyX"}, {all, "KeyC"}, {all, "KeyV"}})}});
    model.free_profile = "b";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    auto view = app.view();
    const auto body = panel_case_bounds(view);
    const auto record = control(view, "shortcuts-record").bounds,
               clear = control(view, "shortcuts-clear").bounds;
    tap_control(app, "shortcuts-button-1");
    view = app.view();
    require(control(view, "shortcuts-record").bounds == record &&
                control(view, "shortcuts-clear").bounds == clear &&
                clear.x + clear.width <= body.x + body.width,
            "Record and clear stay in place, inside the case, whatever is recorded");
    const auto& e = *view.shortcut_editor;
    require(e.chords.size() == 4 && e.chords_after && !e.chords_before &&
                e.chords.back().bounds.x + e.chords.back().bounds.width >
                    e.chords_strip.x + e.chords_strip.width &&
                e.chords_strip.x + e.chords_strip.width <= record.x,
            "the chips overflow their strip, which ends before Record, and fade at its end");
    require(std::abs(e.chords[0].bounds.width -
                     (interface_text_width(e.chords[0].text, 20, view.interface_font) + 24)) < .5,
            "a chip is its text and 12 px on each side");
    // The stick over the strip scrolls it to its last chip.
    const auto strip = e.chords_strip;
    app.move(0, strip.x + strip.width / 2, strip.y + strip.height / 2);
    require(app.scroll_room(0) == ScrollRoom{false, true}, "the strip can scroll forward");
    for (int i = 0; i < 20; ++i) {
        app.scroll(0, 1, 0);
    }
    for (double now = 980; now < 982; now += .05) {
        app.tick(now);
    }
    view = app.view();
    const auto last = view.shortcut_editor->chords.back().bounds;
    require(std::abs(last.x + last.width - (strip.x + strip.width)) < .5 &&
                view.shortcut_editor->chords_before && !view.shortcut_editor->chords_after,
            "the strip scrolls to its last chip");
    // A drag takes it back.
    app.move(0, strip.x + 100, strip.y + 20);
    app.down(0, strip.x + 100, strip.y + 20, 983);
    app.move(0, strip.x + 900, strip.y + 20);
    app.up(0, strip.x + 900, strip.y + 20);
    require(!app.view().shortcut_editor->chords_before, "a drag scrolls it back to its start");
    // Recording the same: the popup's row follows the newest chip.
    tap_control(app, "shortcuts-record");
    for (const char* key : {"KeyZ", "KeyX", "KeyC", "KeyV"}) {
        tap_mini(app, "ControlLeft");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "AltLeft");
        tap_mini(app, key);
    }
    view = app.view();
    const auto& p = *view.shortcut_editor->popup;
    const auto newest = p.chords.back().bounds;
    require(p.chords.size() == 4 && p.chords.back().text == "Ctrl + Shift + Alt + V" &&
                newest.x >= p.well.x && newest.x + newest.width <= p.well.x + p.well.width,
            "the popup's row shows the newest chip");
}
// The Macro view's steps as their rows show them: "keys:Ctrl + A" or "wait:0.5 s".
std::vector<std::string> step_rows(const PanelView& view) {
    std::vector<std::string> rows;
    for (const auto& step : view.shortcut_editor->steps) {
        if (step.wait) {
            rows.push_back("wait:" + step.time);
            continue;
        }
        std::string keys;
        for (const auto& chip : step.chips) {
            keys += (keys.empty() ? "" : " ") + chip.text;
        }
        rows.push_back("keys:" + keys + (step.more ? " …" : ""));
    }
    return rows;
}
// The Macro view (design, Settings and the editor): switching to it and
// back, every flow of its steps, Repeat and the limits, at both widths.
void shortcut_macro_view() {
    for (const bool narrow : {false, true}) {
        TemporaryDirectory directory;
        Settings settings;
        settings.shortcuts = true;
        settings.hide_numpad = narrow;
        save_settings(directory.path, settings);
        write_shortcuts(directory.path, editor_model());
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        open_editor(app);
        tap_control(app, "shortcuts-row-0");
        // Shortcut to Macro: its combinations become one Keys step.
        tap_control(app, "shortcuts-kind-1");
        auto view = app.view();
        require(control(view, "shortcuts-kind-1").selected &&
                    step_rows(view) == std::vector<std::string>{"keys:Ctrl + A"} &&
                    has(view, "shortcuts-add-keys") && has(view, "shortcuts-add-wait") &&
                    control(view, "shortcuts-repeat-0").selected && !has(view, "shortcuts-record") &&
                    view.shortcut_editor->explanation[0] == "Sends Ctrl + A, once per press.",
                "a shortcut turned into a macro starts as one Keys step holding its combinations");
        // And back: each keeps what it was given.
        tap_control(app, "shortcuts-kind-0");
        view = app.view();
        require(view.shortcut_editor->chords.size() == 1 && view.shortcut_editor->steps.empty(),
                "back to Shortcut, its combinations are there");
        tap_control(app, "shortcuts-kind-1");
        tap_control(app, "shortcuts-add-wait");
        require(step_rows(app.view()) == std::vector<std::string>{"keys:Ctrl + A", "wait:0.5 s"},
                "+ Wait adds half a second");
        tap_control(app, "shortcuts-kind-0");
        tap_control(app, "shortcuts-kind-1");
        require(step_rows(app.view()).size() == 2, "switching loses none of the steps");
        // A wait's − and +, a tenth of a second each; held, + goes on.
        tap_control(app, "shortcuts-step-plus-1");
        tap_control(app, "shortcuts-step-minus-1");
        tap_control(app, "shortcuts-step-minus-1");
        require(step_rows(app.view())[1] == "wait:0.4 s", "− and + change a wait by 0.1 s");
        {
            const auto plus = middle(control(app.view(), "shortcuts-step-plus-1").bounds);
            app.move(0, plus.x, plus.y);
            app.down(0, plus.x, plus.y, 1000);
            for (double now = 1000; now < 1001; now += .02) {
                app.tick(now);
            }
            app.up(0, plus.x, plus.y);
            const auto held = step_rows(app.view())[1];
            require(held != "wait:0.5 s" && held != "wait:0.4 s", "a held + goes on");
        }
        // + Keys opens the recorder for a new step, added on Done.
        tap_control(app, "shortcuts-add-keys");
        require(app.view().shortcut_editor->popup &&
                    app.view().shortcut_editor->popup->kind == ShortcutPopupView::Kind::Recording,
                "+ Keys opens the recorder");
        tap_mini(app, "KeyL");
        tap_mini(app, "KeyS");
        tap_mini(app, "Enter");
        tap_control(app, "shortcuts-popup-done");
        view = app.view();
        require(view.shortcut_editor->steps.size() == 3 && step_rows(view)[2] == "keys:L S Enter",
                "Done adds the recorded step");
        tap_control(app, "shortcuts-add-keys");
        tap_mini(app, "KeyQ");
        tap_control(app, "shortcuts-popup-cancel");
        require(app.view().shortcut_editor->steps.size() == 3, "Cancel adds nothing");
        // A Keys step's row opens the recorder for it, from nothing; Done replaces.
        tap_control(app, "shortcuts-step-0");
        require(popup_chips(app.view()).empty(), "a step's recording starts from nothing");
        tap_mini(app, "F5");
        tap_control(app, "shortcuts-popup-done");
        require(step_rows(app.view())[0] == "keys:F5", "Done replaces a step's keys");
        tap_control(app, "shortcuts-step-0");
        tap_mini(app, "KeyQ");
        tap_control(app, "shortcuts-popup-cancel");
        require(step_rows(app.view())[0] == "keys:F5", "Cancel keeps them");
        // A step's recording takes up to 32 combinations.
        tap_control(app, "shortcuts-step-2");
        for (int i = 0; i < 33; ++i) {
            tap_mini(app, i % 2 ? "KeyA" : "KeyB");
        }
        require(popup_chips(app.view()).size() == 32, "a Keys step stops at 32 combinations");
        tap_control(app, "shortcuts-popup-done");
        require(step_rows(app.view())[2].ends_with(" …"), "a long step ends its row in an ellipsis");
        // A drag moves a step; ✕ deletes one.
        drag(app, control(app.view(), "shortcuts-step-2").bounds, 0, -2 * 54);
        view = app.view();
        require(step_rows(view)[1] == "keys:F5" && step_rows(view)[2].starts_with("wait:"),
                "a step dragged up moves");
        tap_control(app, "shortcuts-step-delete-0");
        require(step_rows(app.view()).size() == 2 && step_rows(app.view())[0] == "keys:F5",
                "✕ deletes a step");
        // Repeat: Times with its count, While held, Once.
        tap_control(app, "shortcuts-repeat-1");
        view = app.view();
        require(control(view, "shortcuts-repeat-1").selected && view.shortcut_editor->count == "2" &&
                    !control(view, "shortcuts-count-minus").enabled,
                "Times starts at 2, its least");
        tap_control(app, "shortcuts-count-plus");
        tap_control(app, "shortcuts-count-plus");
        require(app.view().shortcut_editor->count == "4" &&
                    app.view().shortcut_editor->explanation[0].ends_with("4 times per press."),
                "+ counts up, and the line says so");
        tap_control(app, "shortcuts-repeat-2");
        require(!has(app.view(), "shortcuts-count-plus") &&
                    app.view().shortcut_editor->explanation[0].ends_with("again and again while held."),
                "While held has no count");
        // 32 steps: + Keys and + Wait are disabled; the list scrolls.
        while (control(app.view(), "shortcuts-add-wait").enabled) {
            tap_control(app, "shortcuts-add-wait");
        }
        view = app.view();
        require(view.shortcut_editor->steps.size() == 32 &&
                    !control(view, "shortcuts-add-keys").enabled &&
                    view.shortcut_editor->steps_thumb.has_value(),
                "32 steps: + Keys and + Wait are disabled, and the list scrolls");
        // ✓ saves only the chosen action: the macro, and no shortcut beside it.
        tap_control(app, "shortcuts-save");
        const auto saved = saved_shortcuts(app);
        const auto& action = saved.profiles[0].buttons[0].action;
        require(action.kind == ShortcutAction::Kind::Macro && action.steps.size() == 32 &&
                    action.chords.empty() && action.repeat.mode == MacroRepeat::Mode::WhileHeld &&
                    saved.profiles[0].buttons[1] == editor_model().profiles[0].buttons[1],
                "✓ saves the macro alone; other buttons as they were");
    }
}
// Macro buttons on the bar (design, Macro): Times runs its passes; a While
// held one starts on press, keeps running as the laser drifts, never drags
// the bar, and lets go with the trigger, its pass finishing.
void macro_buttons() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    ShortcutButton twice;
    twice.label = "Twice";
    twice.action.kind = ShortcutAction::Kind::Macro;
    twice.action.steps.push_back({MacroStep::Kind::Keys, {{{}, "KeyA"}}, 0});
    twice.action.repeat = {MacroRepeat::Mode::Times, 2};
    ShortcutButton held = twice;
    held.label = "Held";
    held.action.steps = {{MacroStep::Kind::Keys, {{{}, "KeyB"}}, 0}, {MacroStep::Kind::Wait, {}, 100}};
    held.action.repeat = {MacroRepeat::Mode::WhileHeld, 0};
    std::vector<ShortcutButton> buttons{twice, held};
    for (int i = 0; i < 20; ++i) {
        buttons.push_back(shortcut_button("K", {{{}, "KeyK"}})); // enough to scroll
    }
    model.profiles.push_back({"b", "Bar", buttons});
    model.free_profile = "b";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    CaptureSink sink;
    App app(options, sink);
    app.tick(900);
    const auto count = [&](int code) {
        return std::count(sink.events.begin(), sink.events.end(), std::pair{code, 1});
    };
    auto view = app.view();
    require(view.shortcut_bar->buttons[0].enabled && view.shortcut_bar->buttons[1].enabled,
            "macro buttons are enabled where keys can go");
    const auto first = middle(view.shortcut_bar->buttons[0].bounds);
    app.move(0, first.x, first.y);
    app.down(0, first.x, first.y, 900.1);
    app.up(0, first.x, first.y);
    for (double now = 900.1; now < 900.6; now += .01) {
        app.tick(now);
    }
    require(count(KEY_A) == 2, "Times runs its passes from one press");
    // While held: on press, before any release.
    const auto second = middle(view.shortcut_bar->buttons[1].bounds);
    const double scrolled = view.shortcut_bar->buttons[0].bounds.x;
    app.move(0, second.x, second.y);
    require(app.down(0, second.x, second.y, 901), "a held macro's press feels like a key's");
    require(count(KEY_B) == 1 && app.view().shortcut_bar->buttons[1].running, "it starts on press, lit");
    require(app.holds_repeat(0), "a held macro is held as a repeating key is");
    // Drifting sideways off the button: no drag of the bar, and it runs on.
    app.move(0, second.x + 200, second.y);
    app.move(0, second.x + 300, second.y + 300);
    for (double now = 901; now < 901.6; now += .01) {
        app.tick(now);
    }
    require(app.view().shortcut_bar->buttons[0].bounds.x == scrolled,
            "a held macro's press drags nothing");
    require(count(KEY_B) >= 3 && app.view().shortcut_bar->buttons[1].running,
            "it keeps running while held, the laser off the button");
    app.up(0, second.x + 300, second.y + 300);
    const auto after = count(KEY_B);
    for (double now = 901.6; now < 902.5; now += .01) {
        app.tick(now);
    }
    require(count(KEY_B) == after && !app.view().shortcut_bar->buttons[1].running,
            "the trigger let go: the pass under way finishes and no other starts");
    // A cancel ends a hold too (the laser left, or the hold's own limit).
    app.move(0, second.x, second.y);
    app.down(0, second.x, second.y, 903);
    app.cancel_pointer(0);
    const auto canceled = count(KEY_B);
    for (double now = 903; now < 904; now += .01) {
        app.tick(now);
    }
    require(count(KEY_B) == canceled && !app.view().shortcut_bar->buttons[1].running,
            "a cancel lets it go");
}
// R340: any press on the bar that acts stops a run. R342: a While held
// button pressed again while its run finishes a pass takes the hold again.
void bar_presses_and_holds() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    ShortcutButton thrice;
    thrice.label = "Thrice";
    thrice.action.kind = ShortcutAction::Kind::Macro;
    thrice.action.steps = {{MacroStep::Kind::Keys, {{{}, "KeyA"}}, 0}, {MacroStep::Kind::Wait, {}, 300}};
    thrice.action.repeat = {MacroRepeat::Mode::Times, 3};
    ShortcutButton held = thrice;
    held.label = "Held";
    held.action.steps = {{MacroStep::Kind::Keys, {{{}, "KeyB"}}, 0}, {MacroStep::Kind::Wait, {}, 300}};
    held.action.repeat = {MacroRepeat::Mode::WhileHeld, 0};
    model.profiles.push_back({"b", "Bar", {thrice, held}});
    model.profiles.push_back({"o", "Other", {shortcut_button("O", {{{}, "KeyO"}})}});
    model.free_profile = "b";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    CaptureSink sink;
    App app(options, sink);
    double now = 2000;
    app.tick(now);
    const auto count = [&](int code) {
        return std::count(sink.events.begin(), sink.events.end(), std::pair{code, 1});
    };
    const auto run_to = [&](double until) {
        for (; now < until; now += .01) {
            app.tick(now);
        }
    };
    const auto press = [&](const Rect& r) {
        const auto at = middle(r);
        app.move(0, at.x, at.y);
        app.down(0, at.x, at.y, now);
    };
    const auto release = [&](const Rect& r) {
        const auto at = middle(r);
        app.up(0, at.x, at.y);
    };
    auto view = app.view();
    const auto thrice_button = view.shortcut_bar->buttons[0].bounds;
    const auto held_button = view.shortcut_bar->buttons[1].bounds;
    const auto picker = view.shortcut_bar->picker;
    // R340: the picker pressed during a Times macro's wait.
    press(thrice_button);
    release(thrice_button);
    run_to(2000.15);
    require(count(KEY_A) == 1 && app.view().shortcut_bar->buttons[0].running, "the first pass, waiting");
    press(picker);
    release(picker);
    run_to(2002);
    require(count(KEY_A) == 1 && !app.view().shortcut_bar->buttons[0].running &&
                app.view().shortcut_bar->menu,
            "the picker's press stopped it: nothing more, no button lit");
    press(picker);
    release(picker); // closes the menu
    run_to(2002.1);

    // R342: hold, let go mid-pass, press again before the pass ends.
    press(held_button);
    run_to(2002.2);
    require(count(KEY_B) == 1, "a held macro starts on press");
    release(held_button);
    run_to(2002.3);
    press(held_button);
    require(count(KEY_B) == 1 && app.view().shortcut_bar->buttons[1].running,
            "pressed again while the pass finishes: nothing sent twice, still lit");
    run_to(2003.1);
    const auto while_held = count(KEY_B);
    require(while_held >= 3, "the hold taken again: passes go on while held");
    release(held_button);
    run_to(2004);
    require(count(KEY_B) == while_held && !app.view().shortcut_bar->buttons[1].running,
            "let go: the pass finishes and no other starts");
    // Once and Times keep the rule: their own button pressed again stops them.
    press(thrice_button);
    release(thrice_button);
    run_to(now + .15);
    press(thrice_button);
    release(thrice_button);
    run_to(now + 2);
    require(count(KEY_A) == 2 && !app.view().shortcut_bar->buttons[0].running,
            "a Times button pressed again only stops its run");
}
// R337: a lifted Wait row carries its time, − and + and ✕ with it.
void lifted_step_moves_whole() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    write_shortcuts(directory.path, editor_model());
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    tap_control(app, "shortcuts-row-0");
    tap_control(app, "shortcuts-kind-1");
    tap_control(app, "shortcuts-add-wait");
    const auto before = app.view().shortcut_editor->steps.at(1);
    const auto start = middle(control(app.view(), "shortcuts-step-1").bounds);
    // Down by 30, past the dead zone, and past no other row: it is the last.
    app.move(0, start.x, start.y);
    app.down(0, start.x, start.y, 1400);
    app.move(0, start.x, start.y + 15);
    app.move(0, start.x, start.y + 30);
    const auto lifted = app.view().shortcut_editor->lifted_step;
    require(lifted && lifted->wait, "the Wait row is lifted");
    for (const auto& [moved, was] : {std::pair{lifted->time_bounds, before.time_bounds},
                                     std::pair{lifted->minus_bounds, before.minus_bounds},
                                     std::pair{lifted->plus_bounds, before.plus_bounds},
                                     std::pair{lifted->remove_bounds, before.remove_bounds}}) {
        require(std::abs(moved.y - (was.y + 30)) < .5 && moved.x == was.x,
                "its time, − and + and ✕ moved by the same 30 px");
    }
    app.up(0, start.x, start.y + 30);
}
// Phase 5 (design, Which profile shows, Picker menu and Apps): the app with
// the keys from a fake focus, assigned in the picker and saved at once, the
// bar following the focus, names looked up off the VR thread, and Used for.
void shortcut_apps() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"t", "Tied", {shortcut_button("T", {{{"ControlLeft"}, "KeyT"}})}});
    model.profiles.push_back({"u", "Other", {shortcut_button("U", {{{"ControlLeft"}, "KeyU"}})}});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    const AppIdentity notes{AppIdentity::Kind::Desktop, "org.example.Notes"},
        terminal{AppIdentity::Kind::Desktop, "org.example.Terminal"},
        game{AppIdentity::Kind::Steam, "4000010"};
    // Names from the test itself; the game's waits until the test lets it go.
    std::atomic<bool> game_named{};
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.app_names = [&](const AppIdentity& app) -> std::string {
        if (app == notes) {
            return "Example Notes";
        }
        if (app == game) {
            for (int i = 0; i < 2000 && !game_named; ++i) {
                usleep(1000);
            }
            return "Example Game";
        }
        return {};
    };
    struct FocusSink : CaptureSink {
        FocusedApp focus;
        std::uint64_t serial{};
        std::uint32_t focused_window() const override { return focus.window; }
        std::uint64_t focus_serial() const override { return serial; }
        FocusedApp focused_app() const override { return focus; }
    } sink;
    sink.focus = {7, notes, "kwin_wayland"};
    App app(options, sink);
    double now = 950;
    app.tick(now);
    const auto label = [&] {
        const auto view = app.view();
        return view.shortcut_bar && !view.shortcut_bar->buttons.empty()
                   ? view.shortcut_bar->buttons[0].label
                   : std::string();
    };
    const auto menu = [&] { return *app.view().shortcut_bar->menu; };
    const auto wait_for = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 1000 && !done(); ++i) {
            usleep(2000);
            app.tick(now += .01);
        }
        return done();
    };
    require(label() == "A", "an app without a profile: the free profile");
    const auto hover = [&](std::size_t row) {
        const auto at = middle(menu().rows[row].assign);
        app.move(0, at.x, at.y);
        return app.view().bar_tooltip;
    };
    // The tooltips name the app: its own name until the lookup lands, then
    // the one found. The menu itself names no app.
    tap_rect(app, app.view().shortcut_bar->picker);
    require(hover(0) == "Show Free whenever Notes is in front" ||
                hover(0) == "Show Free whenever Example Notes is in front",
            "first the last part of its ID");
    require(wait_for([&] { return hover(0) == "Show Free whenever Example Notes is in front"; }),
            "the name looked up arrives on a tick");
    const auto rows = menu().rows;
    require(std::all_of(rows.begin(), rows.end(),
                        [](const auto& row) { return row.assign_enabled && !row.assigned; }),
            "every assign button can tie the app; none is lit");
    require(hover(1) == "Show Tied whenever Example Notes is in front",
            "the tooltip says what the press will do, naming the profile and the app");
    tap_rect(app, menu().rows[1].assign);
    require(app.view().shortcut_bar->menu && menu().rows[1].assigned && !menu().rows[2].assigned &&
                label() == "T",
            "a tie lights its button, the menu stays open, and the profile tied shows at once");
    const std::vector<AppTie> first_tie{{notes, "Example Notes", "t"}};
    require(saved_shortcuts(app).apps == first_tie, "saved at once, with the name");
    require(hover(1) == "Stop showing Tied for Example Notes", "a lit button's tooltip");
    tap_rect(app, menu().rows[2].assign);
    const std::vector<AppTie> moved_tie{{notes, "Example Notes", "u"}};
    const auto moved_saved = saved_shortcuts(app).apps;
    require(menu().rows[2].assigned && !menu().rows[1].assigned && label() == "U" &&
                moved_saved == moved_tie,
            "another profile's button moves the tie");
    tap_rect(app, menu().rows[2].assign);
    require(!menu().rows[2].assigned && saved_shortcuts(app).apps.empty() && label() == "U",
            "a lit button unties; the bar stays as it is until the focus changes");
    tap_rect(app, menu().rows[1].assign);
    tap_rect(app, app.view().shortcut_bar->picker);
    require(!app.view().shortcut_bar->menu, "the picker closes its menu");

    // The bar follows the focus.
    sink.focus = {8, game, "game.x86_64"};
    app.tick(now += .1);
    require(label() == "A", "another app without a profile: the free one");
    sink.focus = {7, notes, "kwin_wayland"};
    app.tick(now += .1);
    require(label() == "T", "back to the tied app: its profile, on the first tick");
    sink.focus = {7, terminal, "kwin_wayland"};
    ++sink.serial;
    app.tick(now += .1);
    require(label() == "A", "inside the Desktop another app in the same window is a change of focus");
    sink.focus = {7, notes, "kwin_wayland"};
    ++sink.serial;
    app.tick(now += .1);
    require(label() == "T", "and back");
    tap_rect(app, app.view().shortcut_bar->picker);
    const auto picked = menu().rows[2].bounds;
    tap_at(app, picked.x + 30, picked.y + picked.height / 2);
    require(label() == "U", "a profile picked");
    ++sink.serial;
    app.tick(now += .1);
    require(label() == "U", "another window of the same app, such as a dialog, keeps the pick");
    sink.focus.window = 9;
    app.tick(now += .1);
    require(label() == "T", "another window: a change of focus, rule 1 again");

    // No identity: the assign buttons are disabled and say why.
    app.set_dashboard_focus(true);
    app.tick(now += .1);
    // U was picked while an app with its own profile had the keys: that pick
    // stayed temporary (R350).
    require(label() == "A", "Steam's dashboard has no profile: the one picked last for such apps");
    tap_rect(app, app.view().shortcut_bar->picker);
    require(!menu().rows[0].assign_enabled && !menu().rows[1].assigned, "the buttons disabled");
    require(hover(0) == "Can't assign here: Steam's dashboard is in front, not an app",
            "a disabled button's tooltip names the cause");
    tap_rect(app, menu().rows[0].assign);
    require(app.view().shortcut_bar->menu && saved_shortcuts(app).apps.size() == 1,
            "a press on a disabled button does nothing");
    app.set_dashboard_focus(false);
    sink.focus = {10, std::nullopt, ""};
    app.tick(now += .1);
    require(hover(0) == "Can't assign here: Full Keyboard can't see which app is in front",
            "an app that can't be told");

    // Assigned before its name arrives: saved with the program's name, then
    // the name found, once.
    sink.focus = {8, game, "game.x86_64"};
    app.tick(now += .1);
    require(hover(0) == "Show Free whenever game.x86_64 is in front",
            "a Steam game's program name until its lookup lands");
    tap_rect(app, menu().rows[2].assign);
    const auto first_save = saved_shortcuts(app);
    require(first_save.tie(game) && first_save.tie(game)->name == "game.x86_64",
            "saved with that name at once");
    game_named = true;
    require(wait_for([&] { return hover(1) == "Show Tied whenever Example Game is in front"; }),
            "the name arrives");
    const auto named = saved_shortcuts(app);
    require(named.tie(game) && named.tie(game)->name == "Example Game", "the tie takes it, saved");
    tap_rect(app, app.view().shortcut_bar->picker);

    // Used for: the profile's apps; a ✕ waits for ✓, and ← drops it.
    open_editor(app);
    tap_control(app, "shortcuts-row-1");
    auto view = app.view();
    require(view.shortcut_editor->ties.size() == 1 &&
                view.shortcut_editor->ties[0].text == "Example Notes" && has(view, "shortcuts-tie-0"),
            "Used for shows the selected profile's app, with its ✕");
    tap_control(app, "shortcuts-tie-0");
    require(app.view().shortcut_editor->ties.empty() && saved_shortcuts(app).tie(notes) != nullptr,
            "the ✕ takes it off, pending");
    tap_control(app, "shortcuts-back");
    tap_control(app, "confirm-cancel");
    require(!app.view().shortcut_editor && saved_shortcuts(app).tie(notes) != nullptr,
            "← drops the change");
    open_editor(app);
    tap_control(app, "shortcuts-row-1");
    tap_control(app, "shortcuts-tie-0");
    tap_control(app, "shortcuts-save");
    const auto stored = saved_shortcuts(app);
    require(!stored.tie(notes) && stored.tie(game), "✓ saves the profile without the app");

    // 128 apps have a tie: an untied app can't have one; a tied one can move.
    {
        TemporaryDirectory full;
        save_settings(full.path, settings);
        ShortcutModel many = model;
        for (std::size_t i = 0; i < shortcut_limits::apps; ++i) {
            many.apps.push_back({{AppIdentity::Kind::Program, "app" + std::to_string(i)}, "App", "t"});
        }
        write_shortcuts(full.path, many);
        Options limited = options;
        limited.config_dir = full.path;
        limited.app_names = {};
        FocusSink other;
        other.focus = {3, AppIdentity{AppIdentity::Kind::Program, "new"}, "new"};
        App capped(limited, other);
        capped.tick(now += .1);
        tap_rect(capped, capped.view().shortcut_bar->picker);
        const auto rows = capped.view().shortcut_bar->menu->rows;
        require(
            std::none_of(rows.begin(), rows.end(), [](const auto& row) { return row.assign_enabled; }),
            "at 128 ties an untied app's buttons are disabled");
        const auto at = middle(rows[0].assign);
        capped.move(0, at.x, at.y);
        require(capped.view().bar_tooltip ==
                    "128 apps have a profile already. Remove one in the editor first",
                "and say why");
        other.focus = {4, AppIdentity{AppIdentity::Kind::Program, "app5"}, "app5"};
        capped.tick(now += .1);
        const auto moved = capped.view().shortcut_bar->menu->rows;
        require(moved[1].assigned && moved[0].assign_enabled, "a tied app's tie can still move");
    }
}
// R339: over a macro's steps, the first row scrolls the list too; the
// Shortcut view's chip strip, which lies there, is asked only for a shortcut.
// R359: a stream from a PC is the streamed game, by the app ID Steam's
// streaming client was started with, or the PC's stream as one app. Read
// off the VR thread when a session begins, and only that session's answer
// counts. The client lists here stand for /proc.
void shortcut_stream_apps() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"g", "Game", {shortcut_button("G", {{{"ControlLeft"}, "KeyG"}})}});
    model.profiles.push_back({"p", "PC", {shortcut_button("P", {{{"ControlLeft"}, "KeyP"}})}});
    ShortcutButton slow;
    slow.label = "Slow";
    slow.action.kind = ShortcutAction::Kind::Macro;
    slow.action.steps = {{MacroStep::Kind::Keys, {{{}, "KeyJ"}}, 0},
                         {MacroStep::Kind::Wait, {}, 400},
                         {MacroStep::Kind::Keys, {{{}, "KeyL"}}, 0}};
    model.profiles[0].buttons.push_back(slow);
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    // What the client's arguments are now, and whether the read may answer.
    using Clients = std::vector<std::vector<std::string>>;
    std::mutex mutex;
    std::optional<Clients> clients;
    std::atomic<bool> hold_reads{};
    const auto set_clients = [&](std::optional<Clients> next) {
        std::lock_guard lock(mutex);
        clients = std::move(next);
    };
    const auto client = [](const char* app_id) {
        std::vector<std::string> arguments{"/usr/bin/streaming_client", "--server", "192.0.2.7:27036"};
        if (app_id) {
            arguments.insert(arguments.end(), {"--appid", app_id});
        }
        return Clients{arguments};
    };
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.stream_clients = [&]() -> std::optional<Clients> {
        for (int i = 0; i < 3000 && hold_reads; ++i) {
            usleep(1000);
        }
        std::lock_guard lock(mutex);
        return clients;
    };
    // The replacement's route: a stream session, or one of Steam's fields;
    // neither has a window or an app of Gamescope's.
    struct RouteSink : CaptureSink {
        bool stream{}, field{};
        bool steam_field_target() const override { return stream || field; }
        bool stream_session_target() const override { return stream; }
    } sink;
    App app(options, sink);
    double now = 1200;
    app.tick(now);
    const auto label = [&] { return app.view().shortcut_bar->buttons[0].label; };
    const auto menu = [&] { return *app.view().shortcut_bar->menu; };
    const auto settle_until = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 1000 && !done(); ++i) {
            usleep(1000);
            app.tick(now += .01);
        }
        return done();
    };
    const auto hover = [&](std::size_t row) {
        const auto at = middle(menu().rows[row].assign);
        app.move(0, at.x, at.y);
        return app.view().bar_tooltip;
    };
    const auto assign = [&](std::size_t row) {
        tap_rect(app, app.view().shortcut_bar->picker);
        require(menu().rows[row].assign_enabled, "the assign buttons work in a stream");
        tap_rect(app, menu().rows[row].assign);
        tap_rect(app, app.view().shortcut_bar->picker);
    };
    // A stream of a Steam game: its Steam identity, once this session's
    // answer is in.
    set_clients(client("4000020"));
    sink.stream = true;
    app.set_native_request(5);
    app.tick(now += .01);
    require(settle_until([&] {
                tap_rect(app, app.view().shortcut_bar->picker);
                const bool enabled = menu().rows[1].assign_enabled;
                tap_rect(app, app.view().shortcut_bar->picker);
                return enabled;
            }),
            "a streamed game can be assigned once its app is read");
    assign(1);
    const auto saved = saved_shortcuts(app);
    require(saved.apps.size() == 1 &&
                saved.apps[0].app == AppIdentity{AppIdentity::Kind::Steam, "4000020"} &&
                saved.apps[0].profile == "g" && label() == "G",
            "the tie is the game's Steam identity, and its profile shows");
    // The session ends: the free profile; it begins again: the game's.
    sink.stream = false;
    app.set_native_request(0);
    app.tick(now += .01);
    require(label() == "A", "after the stream the free profile shows");
    sink.stream = true;
    app.set_native_request(6);
    require(settle_until([&] { return label() == "G"; }), "the game streamed again shows its profile");

    // Another session with another game, whose read is slow: until its own
    // answer is in, nothing of the last session's game is used; then the
    // free profile, as for any app without one.
    sink.stream = false;
    app.set_native_request(0);
    app.tick(now += .01);
    set_clients(client("4000030"));
    hold_reads = true;
    sink.stream = true;
    app.set_native_request(7);
    for (int i = 0; i < 20; ++i) {
        app.tick(now += .01);
        require(label() == "A", "a new session never shows the last one's game");
    }
    tap_rect(app, app.view().shortcut_bar->picker);
    require(!menu().rows[1].assign_enabled &&
                hover(1) == "Can't assign here: Full Keyboard can't see which app is in front",
            "nothing to assign until its answer is in");
    tap_rect(app, app.view().shortcut_bar->picker);
    hold_reads = false;
    require(settle_until([&] {
                tap_rect(app, app.view().shortcut_bar->picker);
                const bool enabled = menu().rows[1].assign_enabled;
                const auto tip = hover(1);
                tap_rect(app, app.view().shortcut_bar->picker);
                return enabled && tip == "Show Game whenever App 4000030 is in front";
            }) &&
                label() == "A",
            "the other game is an app of its own, without a profile");

    // A run stops when the session changes, as for another app: J goes, L
    // never does.
    sink.events.clear();
    const auto slow_at = middle(app.view().shortcut_bar->buttons[1].bounds);
    app.move(0, slow_at.x, slow_at.y);
    app.down(0, slow_at.x, slow_at.y, now);
    app.up(0, slow_at.x, slow_at.y);
    for (int i = 0; i < 10; ++i) {
        app.tick(now += .01);
    }
    app.set_native_request(8);
    for (int i = 0; i < 80; ++i) {
        app.tick(now += .01);
    }
    require(std::count(sink.events.begin(), sink.events.end(), std::pair{KEY_J, 1}) == 1 &&
                std::none_of(sink.events.begin(), sink.events.end(),
                             [](const auto& event) { return event.first == KEY_L; }),
            "another session stops a run");

    // A stream with no usable app ID, such as the PC's desktop: the one
    // stream app, named in the tooltip's own words and the editor's.
    set_clients(client(nullptr));
    sink.stream = false;
    app.set_native_request(0);
    app.tick(now += .01);
    sink.stream = true;
    app.set_native_request(9);
    require(settle_until([&] {
                tap_rect(app, app.view().shortcut_bar->picker);
                const auto tip = hover(2);
                tap_rect(app, app.view().shortcut_bar->picker);
                return tip == "Show PC whenever your PC's desktop stream is in front";
            }),
            "the PC's stream is named in the tooltip");
    assign(2);
    const auto both = saved_shortcuts(app);
    require(both.apps.size() == 2 && both.tie(AppIdentity{AppIdentity::Kind::Stream, "pc"}) &&
                both.tie(AppIdentity{AppIdentity::Kind::Stream, "pc"})->profile == "p" &&
                both.tie(AppIdentity{AppIdentity::Kind::Stream, "pc"})->name == "PC desktop stream" &&
                label() == "P",
            "the PC's stream is tied as the stream app, its profile shows");

    // One of Steam's own fields still can't be assigned.
    sink.stream = false;
    sink.field = true;
    app.set_native_request(10);
    app.tick(now += .01);
    tap_rect(app, app.view().shortcut_bar->picker);
    require(!menu().rows[0].assign_enabled &&
                hover(0) == "Can't assign here: this is one of Steam's own text fields, not an app" &&
                label() == "A",
            "a Steam field still can't be assigned; the free profile shows");
}
// R361: a tie saved while a program's identity held its arguments is moved
// to the program's name when it has the keys, and saved at once; another
// program's that only begins the same is left alone.
void shortcut_long_program_tie() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"t", "Tied", {shortcut_button("T", {{{"ControlLeft"}, "KeyT"}})}});
    model.profiles.push_back({"b", "Beta", {shortcut_button("B", {{{"ControlLeft"}, "KeyB"}})}});
    const std::string line = "browser --enable-features=A,B --flag=x,y --mode=1";
    model.apps.push_back({{AppIdentity::Kind::Program, line}, line, "t"});
    model.apps.push_back(
        {{AppIdentity::Kind::Program, "browser-beta --flag=x"}, "browser-beta --flag=x", "b"});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct FocusSink : CaptureSink {
        FocusedApp focus;
        std::uint32_t focused_window() const override { return focus.window; }
        FocusedApp focused_app() const override { return focus; }
    } sink;
    sink.focus = {7, AppIdentity{AppIdentity::Kind::Program, "browser"}, "browser"};
    App app(options, sink);
    app.tick(1300);
    app.tick(1300.1);
    require(app.view().shortcut_bar->buttons[0].label == "T",
            "the old tie's profile shows for the program");
    const auto saved = saved_shortcuts(app);
    const auto* tie = saved.tie({AppIdentity::Kind::Program, "browser"});
    require(saved.apps.size() == 2 && tie && tie->profile == "t" && tie->name == "browser" &&
                !saved.tie({AppIdentity::Kind::Program, line}),
            "the tie is the program's name alone now, saved at once");
    require(saved.tie({AppIdentity::Kind::Program, "browser-beta --flag=x"}),
            "another program's tie that only begins the same stays");
}
// R350: an app without a profile shows the last profile picked for such
// apps; tied to an app, the one picked before it that no app is tied to;
// with every pick tied, the last pick all the same.
void shortcut_pick_history() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "First", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"b", "Second", {shortcut_button("B", {{{"ControlLeft"}, "KeyB"}})}});
    model.profiles.push_back({"c", "Never", {shortcut_button("C", {{{"ControlLeft"}, "KeyC"}})}});
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct FocusSink : CaptureSink {
        FocusedApp focus;
        std::uint32_t focused_window() const override { return focus.window; }
        FocusedApp focused_app() const override { return focus; }
    } sink;
    const auto app_named = [](std::uint32_t window, const char* id) {
        return FocusedApp{window, AppIdentity{AppIdentity::Kind::Program, id}, id};
    };
    sink.focus = app_named(1, "one");
    App app(options, sink);
    double now = 1400;
    app.tick(now);
    const auto label = [&] {
        const auto view = app.view();
        return view.shortcut_bar && !view.shortcut_bar->buttons.empty()
                   ? view.shortcut_bar->buttons[0].label
                   : std::string();
    };
    const auto menu = [&] { return *app.view().shortcut_bar->menu; };
    const auto pick = [&](std::size_t row) {
        if (!app.view().shortcut_bar) {
            tap_rect(app, control(app.view(), "bar-toggle").bounds); // the empty bar, for its picker
        }
        tap_rect(app, app.view().shortcut_bar->picker);
        const auto r = menu().rows[row].bounds;
        tap_at(app, r.x + 30, r.y + r.height / 2);
    };
    const auto assign = [&](std::size_t row) {
        require(app.view().shortcut_bar.has_value(), "a bar shows to assign from");
        tap_rect(app, app.view().shortcut_bar->picker);
        tap_rect(app, menu().rows[row].assign);
        tap_rect(app, app.view().shortcut_bar->picker);
    };
    const auto focus = [&](std::uint32_t window, const char* id) {
        sink.focus = app_named(window, id);
        app.tick(now += .1);
    };
    // Second, then First, picked in apps without a profile.
    pick(1);
    focus(2, "two");
    pick(0);
    focus(3, "three");
    require(label() == "A", "the last pick shows in another app without a profile");
    const auto saved = saved_shortcuts(app);
    require(saved.free_profile == "a" && saved.earlier_picks == std::vector<std::string>{"b"},
            "the picks are saved, newest first");
    // First is tied to this app: elsewhere, the one picked before it.
    assign(0);
    focus(4, "four");
    require(label() == "B", "the last pick tied to an app: the one picked before it");
    // Second is tied too: every pick tied, the last pick all the same; Never,
    // never picked, isn't chosen.
    assign(1);
    focus(5, "five");
    require(label() == "A", "every pick tied: the last pick stays");
}
// R362: names found are kept in a record of their own, so a game streamed
// before still has its name when its manifest is gone; a tie saved with the
// identity's own name takes the real one whenever it is found. A stream's
// game is looked up when its session's app is known.
void shortcut_name_record() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    const AppIdentity game{AppIdentity::Kind::Steam, "4000020"},
        other{AppIdentity::Kind::Steam, "4000030"}, tool{AppIdentity::Kind::Program, "tool"};
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"g", "Game", {shortcut_button("G", {{{"ControlLeft"}, "KeyG"}})}});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    const auto record = directory.path / "cache" / "app-names.json";
    // The manifests there are: the first game's only while `streaming`.
    std::atomic<bool> streaming{true};
    std::mutex mutex;
    std::string app_id = "4000020";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.app_name_record = record;
    options.app_names = [&](const AppIdentity& app) -> std::string {
        if (app == game) {
            return streaming ? "Example Game" : "";
        }
        if (app == other) {
            return "Other Game";
        }
        return app == tool ? "Some Tool" : ""; // a source may name a program; it isn't kept
    };
    options.stream_clients = [&]() -> std::optional<std::vector<std::vector<std::string>>> {
        std::lock_guard lock(mutex);
        return std::vector<std::vector<std::string>>{{"/usr/bin/streaming_client", "--appid", app_id}};
    };
    struct StreamSink : CaptureSink {
        bool stream{};
        FocusedApp focus;
        bool steam_field_target() const override { return stream; }
        bool stream_session_target() const override { return stream; }
        std::uint32_t focused_window() const override { return focus.window; }
        FocusedApp focused_app() const override { return focus; }
    } sink;
    sink.focus = {4, tool, "tool"};
    const auto tooltip = [](App& app, double& now, const std::string& wanted) {
        for (int i = 0; i < 1000; ++i) {
            app.tick(now += .01);
            tap_rect(app, app.view().shortcut_bar->picker);
            const auto at = middle(app.view().shortcut_bar->menu->rows[1].assign);
            app.move(0, at.x, at.y);
            const auto tip = app.view().bar_tooltip;
            tap_rect(app, app.view().shortcut_bar->picker);
            if (tip == wanted) {
                return true;
            }
            usleep(1000);
        }
        return false;
    };
    double now = 1500;
    {
        App app(options, sink);
        app.tick(now);
        // A program named by the source: nothing kept for it.
        require(tooltip(app, now, "Show Game whenever Some Tool is in front"), "a program's name");
        // A stream of a game whose manifest is there now: looked up when the
        // session's app is known, without the menu.
        sink.stream = true;
        app.set_native_request(1);
        require(tooltip(app, now, "Show Game whenever Example Game is in front"),
                "the streamed game's name");
        app.flush_settings();
        struct stat file{};
        require(stat(record.c_str(), &file) == 0 && (file.st_mode & 0777) == 0600,
                "the record is the user's alone");
        std::ifstream read(record);
        const auto kept = parse_app_name_record({std::istreambuf_iterator<char>(read), {}});
        require(kept == std::vector<RecordedName>{{game, "Example Game"}},
                "the game's name is kept, the program's isn't");
    }
    // Later, with the manifest gone and the game tied under its ID's own
    // name, and another game tied the same way.
    streaming = false;
    sink.stream = false;
    model.apps = {{game, "App 4000020", "g"}, {other, "App 4000030", "a"}};
    write_shortcuts(directory.path, model);
    {
        App app(options, sink);
        app.tick(now += .01);
        sink.stream = true;
        app.set_native_request(2);
        bool renamed = false;
        for (int i = 0; i < 1000 && !renamed; ++i) {
            usleep(1000);
            app.tick(now += .01);
            renamed = saved_shortcuts(app).tie(game)->name == "Example Game";
        }
        require(renamed, "a tie under its ID's own name takes the name kept before");
        require(tooltip(app, now, "Stop showing Game for Example Game"),
                "the kept name shows with the manifest gone");
        // The other game: its name found by a lookup in a later session.
        sink.stream = false;
        app.set_native_request(0);
        app.tick(now += .01);
        {
            std::lock_guard lock(mutex);
            app_id = "4000030";
        }
        sink.stream = true;
        app.set_native_request(3);
        renamed = false;
        for (int i = 0; i < 1000 && !renamed; ++i) {
            usleep(1000);
            app.tick(now += .01);
            renamed = saved_shortcuts(app).tie(other)->name == "Other Game";
        }
        require(renamed, "a tie under its ID's own name takes a name found later");
    }
}
// R313: recording no longer offers F13–F24, but a file's still load, show
// their chips and send.
void shortcut_fkeys_from_file() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back(
        {"f",
         "F keys",
         {shortcut_button("Fs", {{{}, "F13"}, {{}, "F14"}, {{"ControlLeft"}, "F20"}, {{}, "F24"}})}});
    model.free_profile = "f";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    CaptureSink sink;
    App app(options, sink);
    double now = 1600;
    app.tick(now);
    require(app.view().shortcut_bar && app.view().shortcut_bar->buttons.size() == 1 &&
                app.view().shortcut_bar->buttons[0].enabled,
            "a file with F13–F24 loads, its button enabled");
    const auto at = middle(app.view().shortcut_bar->buttons[0].bounds);
    app.move(0, at.x, at.y);
    app.down(0, at.x, at.y, now);
    app.up(0, at.x, at.y);
    for (int i = 0; i < 60; ++i) {
        app.tick(now += .02);
    }
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_F13, 1},
                                                            {KEY_F13, 0},
                                                            {KEY_F14, 1},
                                                            {KEY_F14, 0},
                                                            {KEY_LEFTCTRL, 1},
                                                            {KEY_F20, 1},
                                                            {KEY_F20, 0},
                                                            {KEY_LEFTCTRL, 0},
                                                            {KEY_F24, 1},
                                                            {KEY_F24, 0}},
            "its keys are sent");
    open_editor(app);
    tap_control(app, "shortcuts-button-0");
    const auto view = app.view();
    const auto& chords = view.shortcut_editor->chords;
    require(chords.size() == 4 && chords[0].text == "F13" && chords[2].text == "Ctrl + F20" &&
                chords[3].text == "F24",
            "the editor shows their chips");
}
void macro_first_row_scrolls() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    write_shortcuts(directory.path, editor_model());
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    tap_control(app, "shortcuts-row-0");
    const auto chips = app.view().shortcut_editor->chords_strip;
    tap_control(app, "shortcuts-kind-1");
    for (int i = 0; i < 5; ++i) {
        tap_control(app, "shortcuts-add-wait");
    }
    for (double now = 1500; now < 1501; now += .05) {
        app.tick(now);
    }
    auto view = app.view();
    require(view.shortcut_editor->steps.size() == 6, "six steps");
    // The top visible row, where it overlaps the Shortcut view's chip strip.
    const auto well = view.shortcut_editor->steps_well;
    Rect top{};
    for (const auto& step : view.shortcut_editor->steps) {
        if (step.bounds.y >= well.y - .5 && (top.height == 0 || step.bounds.y < top.y)) {
            top = step.bounds;
        }
    }
    // Where the two overlap: the row's top and the strip's foot.
    const double x = std::max(chips.x, top.x) + 20,
                 y = (std::max(top.y, chips.y) + std::min(top.y + top.height, chips.y + chips.height)) /
                     2;
    require(top.height > 0 && chips.contains(x, y) && well.contains(x, y),
            "the point lies in the first row and in the chip strip's place");
    app.move(0, x, y);
    const auto room = app.scroll_room(0);
    require(room && room->back, "the first row reports room to scroll back");
    const double was = view.shortcut_editor->steps[0].bounds.y;
    require(app.scroll(0, 0, -1), "a scroll there moves the list");
    for (double now = 1501; now < 1502; now += .05) {
        app.tick(now);
    }
    require(app.view().shortcut_editor->steps[0].bounds.y > was + 1, "the list moved down");
}
// R341: scrolling the profiles column closes the Move/Copy popover, and the
// button goes back.
void carry_closes_on_scroll() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back(
        {"one", "One", {shortcut_button("A", {{{}, "KeyA"}}), shortcut_button("B", {{{}, "KeyB"}})}});
    for (int i = 2; i <= 10; ++i) {
        model.profiles.push_back({"p" + std::to_string(i), "Profile " + std::to_string(i), {}});
    }
    model.free_profile = "one";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    const auto start = middle(control(app.view(), "shortcuts-button-0").bounds);
    const auto target = middle(control(app.view(), "shortcuts-row-1").bounds);
    app.move(0, start.x, start.y);
    app.down(0, start.x, start.y, 1600);
    for (int step = 1; step <= 8; ++step) {
        app.move(0, start.x + (target.x - start.x) * step / 8,
                 start.y + (target.y - start.y) * step / 8);
    }
    app.up(0, target.x, target.y);
    require(has(app.view(), "shortcuts-carry-move") &&
                strip_labels(app.view()) == std::vector<std::string>{"B"},
            "dropped on Profile 2: the popover");
    app.scroll(0, 0, 1);
    for (double now = 1601; now < 1602; now += .05) {
        app.tick(now);
    }
    require(!has(app.view(), "shortcuts-carry-move") && !app.view().shortcut_editor->carry_panel &&
                strip_labels(app.view()) == std::vector<std::string>{"A", "B"},
            "a scroll over the column closes it, the button back in its place");
    tap_control(app, "shortcuts-save");
    const auto saved = saved_shortcuts(app);
    require(saved.profiles[0].buttons.size() == 2 && saved.profiles[1].buttons.empty(),
            "nothing changed");
}
// R338: VR reads the stick for the bar, its menu and the editor too.
void stick_scrolls_bar() {
    TemporaryDirectory directory;
    Settings settings;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    {
        App off(options, sink);
        require(!off.view().shortcut_bar && !off.stick_scrolls(), "the feature off: no stick");
    }
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    App app(options, sink);
    require(app.view().shortcut_bar && app.stick_scrolls(), "the bar showing: the stick");
    tap_rect(app, app.view().shortcut_bar->picker);
    require(app.view().shortcut_bar->menu && app.stick_scrolls(), "the menu open: the stick");
    tap_rect(app, app.view().shortcut_bar->menu->manage);
    require(app.view().shortcut_editor && !app.view().shortcut_bar && app.stick_scrolls(),
            "the editor open: the stick");
    tap_control(app, "shortcuts-back");
    click(app, "bar-toggle");
    require(!app.view().shortcut_bar && !app.stick_scrolls(), "the bar hidden by hand: no stick");
}
// Run buttons (design, Run): never disabled for lack of a target, one start
// per press with more presses waiting for its report, and a failed start's
// status naming the button. In a temporary home only.
void run_buttons() {
    TemporaryDirectory directory, home;
    const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", home.path.c_str(), 1);
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    const auto marker = home.path / "marker";
    ShortcutButton command;
    command.label = "Mark\nit";
    command.action.kind = ShortcutAction::Kind::Run;
    command.action.command = "echo x >> '" + marker.string() + "'";
    ShortcutButton missing;
    missing.label = "Gone";
    missing.action.kind = ShortcutAction::Kind::Run;
    missing.action.script = "missing.sh";
    ShortcutButton empty;
    empty.label = "Empty";
    empty.action.kind = ShortcutAction::Kind::Run;
    ShortcutButton unlabeled = missing; // R353
    unlabeled.label.clear();
    ShortcutModel model;
    model.profiles.push_back({"r", "Run", {command, missing, empty, unlabeled}});
    model.free_profile = "r";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct SteamField : CaptureSink {
        bool steam_field_target() const override { return true; }
    } sink;
    App app(options, sink);
    double now = 3000;
    app.tick(now);
    const auto buttons = [&] { return app.view().shortcut_bar->buttons; };
    require(buttons()[0].enabled && buttons()[1].enabled && !buttons()[2].enabled,
            "in a Steam field Run buttons are enabled; an empty one is not");
    require(buttons()[0].run && buttons()[1].run && buttons()[2].run,
            "every Run button carries its mark, an empty one too");
    app.set_dashboard_focus(true);
    app.tick(now += .1);
    require(buttons()[0].enabled && buttons()[1].enabled,
            "with Steam's dashboard and no target they are enabled too");
    const auto press = [&](std::size_t index) {
        const auto at = middle(buttons()[index].bounds);
        app.move(0, at.x, at.y);
        app.down(0, at.x, at.y, now);
        app.up(0, at.x, at.y);
    };
    const auto lines = [&] {
        std::ifstream file(marker);
        return static_cast<int>(std::count(std::istreambuf_iterator<char>(file), {}, '\n'));
    };
    const auto settle_until = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            usleep(5000);
            app.tick(now += .01);
        }
        return done();
    };
    press(0);
    press(0); // before its start is reported: ignored
    require(settle_until([&] { return lines() == 1; }), "a press starts the command");
    for (int i = 0; i < 40; ++i) {
        usleep(5000);
        app.tick(now += .01);
    }
    require(lines() == 1 && sink.events.empty(),
            "the second press waited for the first's report; nothing typed");
    press(0);
    require(settle_until([&] { return lines() == 2; }), "once reported, the next press starts it again");
    press(1);
    require(settle_until([&] { return app.view().status.find("Gone") != std::string::npos; }) &&
                app.view().status ==
                    "Gone could not start. Check its script or command in the shortcut editor.",
            "a missing script's status names the button");
    press(2);
    app.tick(now += .1);
    require(lines() == 2, "an empty Run button does nothing");
    press(3);
    require(settle_until([&] { return app.view().status.starts_with("A Run button"); }) &&
                app.view().status == "A Run button could not start. Check its script or command "
                                     "in the shortcut editor.",
            "a button without a label has a sentence of its own (R353)");
    setenv("HOME", old_home.c_str(), 1);
}
// R343: the moment the keyboard opens in the dashboard, before the probe
// answers, is no change of focus for the bar: a pick and the empty bar stay.
// Only the dashboard actually having the keys drops them.
void dashboard_unknown_keeps_focus() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back({"a", "Free", {shortcut_button("A", {{{"ControlLeft"}, "KeyA"}})}});
    model.profiles.push_back({"o", "Other", {shortcut_button("O", {{{"ControlLeft"}, "KeyO"}})}});
    // Other belongs to another app, so picking it leaves the free profile.
    model.apps.push_back({{AppIdentity::Kind::Program, "viewer"}, "viewer", "o"});
    model.free_profile = "a";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.start_enabled = true;
    options.target_language = "en-us";
    struct FocusSink : CaptureSink {
        FocusedApp focus{7, AppIdentity{AppIdentity::Kind::Program, "editor"}, "editor"};
        std::uint32_t focused_window() const override { return focus.window; }
        FocusedApp focused_app() const override { return focus; }
    } sink;
    App app(options, sink);
    double now = 7000;
    app.tick(now);
    const auto label = [&] {
        const auto view = app.view();
        return view.shortcut_bar && !view.shortcut_bar->buttons.empty()
                   ? view.shortcut_bar->buttons[0].label
                   : std::string();
    };
    tap_rect(app, app.view().shortcut_bar->picker);
    const auto other = app.view().shortcut_bar->menu->rows[1].bounds;
    tap_at(app, other.x + 30, other.y + other.height / 2);
    require(label() == "O", "Other picked");
    // Opened in the dashboard: typing waits, the probe hasn't answered.
    app.set_dashboard_focus(true, false);
    app.tick(now += .1);
    require(label() == "O", "while the probe hasn't answered, the pick stays");
    app.set_dashboard_focus(false, true);
    app.tick(now += .1);
    require(label() == "O", "the answer is the same app: the pick stays, no change of focus between");
    app.set_dashboard_focus(true, false);
    app.tick(now += .1);
    app.set_dashboard_focus(true, true);
    app.tick(now += .1);
    require(label() == "A", "the dashboard having the keys is a change of focus: the free profile");
    app.set_dashboard_focus(false, true);
    app.tick(now += .1);
    require(label() == "A", "and back to the app: the rule's profile, the pick gone");
    // The empty bar likewise: no profile is due, the toolbar button opens it.
    {
        TemporaryDirectory empty_dir;
        save_settings(empty_dir.path, settings);
        ShortcutModel none = model;
        none.free_profile.clear();
        write_shortcuts(empty_dir.path, none);
        Options empty_options = options;
        empty_options.config_dir = empty_dir.path;
        App empty(empty_options, sink);
        empty.tick(now += .1);
        require(!empty.view().shortcut_bar, "no profile is due: no bar");
        click(empty, "bar-toggle");
        require(empty.view().shortcut_bar && empty.view().shortcut_bar->slots, "the empty bar");
        empty.set_dashboard_focus(true, false);
        empty.tick(now += .1);
        empty.set_dashboard_focus(false, true);
        empty.tick(now += .1);
        require(empty.view().shortcut_bar && empty.view().shortcut_bar->slots,
                "an opening over the same app keeps the empty bar");
        empty.set_dashboard_focus(true, true);
        empty.tick(now += .1);
        require(!empty.view().shortcut_bar, "the dashboard having the keys ends it");
    }
}
// The name of the script row lit as chosen, or empty.
std::string control_selected_script(App& app) {
    for (const auto& row : app.view().shortcut_editor->scripts) {
        if (row.chosen) {
            return row.name;
        }
    }
    return {};
}
// The editor's Run body (design, Settings and the editor), at both widths:
// the scripts folder made and listed off the VR thread, choosing and
// clearing, scrolling from any row, a missing script, Command's popup, each
// kept while switching, and only the chosen one saved. In a temporary home.
void run_editor() {
    for (const bool narrow : {false, true}) {
        TemporaryDirectory home;
        const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
        setenv("HOME", home.path.c_str(), 1);
        const auto config = home.path / ".config" / "framekeyboard";
        fs::create_directories(config);
        Settings settings;
        settings.shortcuts = true;
        settings.hide_numpad = narrow;
        save_settings(config, settings);
        ShortcutModel model;
        // Three buttons, the third edited: no drag may select another (R352).
        model.profiles.push_back({"p",
                                  "P",
                                  {shortcut_button("Go", {{{"ControlLeft"}, "KeyG"}}),
                                   shortcut_button("One", {{{"ControlLeft"}, "KeyO"}}),
                                   shortcut_button("Two", {{{"ControlLeft"}, "KeyT"}})}});
        model.free_profile = "p";
        write_shortcuts(config, model);
        Options options;
        options.config_dir = config;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        open_editor(app);
        tap_control(app, "shortcuts-button-2");
        require(control(app.view(), "shortcuts-button-2").selected, "the third button is edited");
        const auto folder = config / "scripts";
        const auto settle = [&](const std::function<bool()>& done) {
            static double now = 5000;
            for (int i = 0; i < 400 && !done(); ++i) {
                usleep(2000);
                app.tick(now += .02);
            }
            return done();
        };
        require(control(app.view(), "shortcuts-button-2").icon == Icon::None, "a shortcut has no mark");
        tap_control(app, "shortcuts-kind-2");
        require(control(app.view(), "shortcuts-kind-2").selected &&
                    control(app.view(), "shortcuts-run-0").selected,
                "Run is chosen, Script first");
        require(control(app.view(), "shortcuts-button-2").icon == Icon::RunMark &&
                    control(app.view(), "shortcuts-button-0").icon == Icon::None,
                "the strip shows the mark as soon as Run is chosen, pending as it is");
        require(settle([&] { return !app.view().shortcut_editor->scripts_note.empty(); }) &&
                    fs::is_directory(folder),
                "the folder is made when the list first shows");
        require(app.view().shortcut_editor->scripts_note ==
                    "No scripts in ~/.config/framekeyboard/scripts",
                "the empty list names the folder, with ~");
        for (const char* name : {"a.sh", "b.sh", "c.sh", "d.sh", "e.sh", "f.sh", "g.sh"}) {
            std::ofstream(folder / name) << "true\n";
        }
        tap_control(app, "shortcuts-refresh");
        require(settle([&] { return app.view().shortcut_editor->scripts.size() == 7; }),
                "Refresh reads the folder again");
        auto view = app.view();
        const auto body = panel_case_bounds(view);
        for (const char* id :
             {"shortcuts-refresh", "shortcuts-run-0", "shortcuts-run-1", "shortcuts-script-0"}) {
            const auto r = control(view, id).bounds;
            require(body.contains(r.x, r.y) && body.contains(r.x + r.width - 1, r.y + r.height - 1),
                    (std::string(id) + " lies in the case").c_str());
        }
        tap_control(app, "shortcuts-script-1");
        require(control(app.view(), "shortcuts-script-1").selected &&
                    app.view().shortcut_editor->explanation.front() == "Starts b.sh.",
                "a press chooses a script; the line says what it starts");
        tap_control(app, "shortcuts-script-1");
        require(!control(app.view(), "shortcuts-script-1").selected &&
                    app.view().shortcut_editor->explanation.front() ==
                        "Choose a script from ~/.config/framekeyboard/scripts.",
                "a press on the chosen one clears it");
        tap_control(app, "shortcuts-script-2");
        // Scrolling: the stick over the first row, and a drag from a row.
        const auto first = middle(control(app.view(), "shortcuts-script-0").bounds);
        app.move(0, first.x, first.y);
        const auto room = app.scroll_room(0);
        require(room && room->forward, "the list reports room under its first row");
        const double top = app.view().shortcut_editor->scripts[0].bounds.y;
        app.down(0, first.x, first.y, 6000);
        app.move(0, first.x, first.y - 20);
        require(control(app.view(), "shortcuts-button-2").selected &&
                    !app.view().shortcut_editor->lifted_button,
                "a drag lifted on the list selects no other button and carries none (R352)");
        app.move(0, first.x, first.y - 90);
        app.up(0, first.x, first.y - 90);
        require(app.view().shortcut_editor->scripts[0].bounds.y < top - 60 &&
                    control_selected_script(app) == "c.sh",
                "a drag from a row scrolls the list and chooses nothing");
        require(control(app.view(), "shortcuts-button-2").selected &&
                    control(app.view(), "shortcuts-kind-2").selected,
                "the third button stays edited, still Run");
        // The chosen script gone: it stays, marked missing.
        fs::remove(folder / "c.sh");
        tap_control(app, "shortcuts-refresh");
        require(settle([&] {
                    const auto& rows = app.view().shortcut_editor->scripts;
                    return rows.size() == 7 && rows[2].name == "c.sh" && rows[2].missing &&
                           rows[2].chosen;
                }),
                "a chosen script that is gone stays in the list, marked missing");
        require(app.view().shortcut_editor->explanation.front() ==
                    "c.sh is no longer in the scripts folder.",
                "and the line says so");
        // Command: its field opens the popup, which types one line.
        tap_control(app, "shortcuts-run-1");
        require(has(app.view(), "shortcuts-command") && !has(app.view(), "shortcuts-refresh") &&
                    app.view().shortcut_editor->explanation.front() ==
                        "Type a command for /bin/sh to run.",
                "Command shows its field");
        {
            // R348: its caption as large as Label's, the field starting after it.
            const auto v = app.view();
            const auto field = control(v, "shortcuts-command").bounds;
            require(field.x - (v.shortcut_editor->divider + 16) >=
                            interface_text_width("Command", 22, v.interface_font) + 4 &&
                        field.x + field.width <= panel_case_bounds(v).x + panel_case_bounds(v).width,
                    "the Command caption has room at full size; the field stays in the case");
        }
        tap_control(app, "shortcuts-command");
        require(app.view().shortcut_editor->popup &&
                    app.view().shortcut_editor->popup->title == "Command",
                "the field opens the popup, titled Command");
        tap_mini(app, "KeyL");
        tap_mini(app, "KeyS");
        tap_mini(app, "Enter");
        require(!app.view().shortcut_editor->popup &&
                    control(app.view(), "shortcuts-command").label == "ls" &&
                    app.view().shortcut_editor->explanation.front() == "Starts ls",
                "Enter saves the command");
        tap_control(app, "shortcuts-command");
        tap_mini(app, "KeyX");
        tap_mini(app, "Escape");
        require(control(app.view(), "shortcuts-command").label == "ls", "Esc cancels");
        tap_control(app, "shortcuts-run-0");
        require(control_selected_script(app) == "c.sh", "back to Script: its choice is kept");
        tap_control(app, "shortcuts-run-1");
        require(control(app.view(), "shortcuts-command").label == "ls", "and Command's");
        require(saved_shortcuts(app).profiles[0].buttons[2].action.kind == ShortcutAction::Kind::Keys,
                "all of it pending until ✓");
        tap_control(app, "shortcuts-save");
        const auto saved_model = saved_shortcuts(app);
        const auto saved = saved_model.profiles[0].buttons[2].action;
        require(saved.kind == ShortcutAction::Kind::Run && saved.command == "ls" &&
                    saved.script.empty() && saved.chords.empty(),
                "✓ saves the command chosen alone");
        require(saved_model.profiles[0].buttons[0] == model.profiles[0].buttons[0] &&
                    saved_model.profiles[0].buttons[1] == model.profiles[0].buttons[1],
                "the other buttons are saved unchanged");
        setenv("HOME", old_home.c_str(), 1);
    }
}
// R354: a chosen script that sorts past the 64 listed is in the folder, so
// it shows as there, not missing; gone, it is missing.
void run_script_past_list() {
    TemporaryDirectory home;
    const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", home.path.c_str(), 1);
    const auto config = home.path / ".config" / "framekeyboard";
    const auto folder = config / "scripts";
    fs::create_directories(folder);
    for (int i = 0; i < 70; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "n%02d.sh", i);
        std::ofstream(folder / name) << "true\n";
    }
    std::ofstream(folder / "zz.sh") << "true\n";
    Settings settings;
    settings.shortcuts = true;
    save_settings(config, settings);
    ShortcutButton run;
    run.label = "Z";
    run.action.kind = ShortcutAction::Kind::Run;
    run.action.script = "zz.sh";
    ShortcutModel model;
    model.profiles.push_back({"p", "P", {run}});
    model.free_profile = "p";
    write_shortcuts(config, model);
    Options options;
    options.config_dir = config;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    double now = 7000;
    const auto settle = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            usleep(2000);
            app.tick(now += .02);
        }
        return done();
    };
    const auto last = [&] {
        const auto& rows = app.view().shortcut_editor->scripts;
        return rows.empty() ? ShortcutEditorView::ScriptRow{} : rows.back();
    };
    require(settle([&] { return app.view().shortcut_editor->scripts.size() == 65; }) &&
                last().name == "zz.sh" && last().chosen && !last().missing &&
                app.view().shortcut_editor->explanation.front() == "Starts zz.sh.",
            "a chosen script past the 64 listed shows as there");
    fs::remove(folder / "zz.sh");
    tap_control(app, "shortcuts-refresh");
    require(settle([&] { return last().missing; }) && last().name == "zz.sh" &&
                app.view().shortcut_editor->explanation.front() ==
                    "zz.sh is no longer in the scripts folder.",
            "gone, it is missing");
    setenv("HOME", old_home.c_str(), 1);
}
// R332: a new user's first bar shows as soon as ✓ saves it, and becomes
// the free bar.
void first_bar_shows() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    click(app, "bar-toggle");
    require(app.view().shortcut_bar && app.view().shortcut_bar->buttons.empty(), "the empty bar");
    open_editor(app);
    tap_control(app, "shortcuts-new-profile");
    tap_mini(app, "Escape");
    tap_control(app, "shortcuts-add");
    tap_control(app, "shortcuts-save");
    const auto view = app.view();
    const auto saved = saved_shortcuts(app);
    require(!view.shortcut_editor && view.shortcut_bar && view.shortcut_bar->buttons.size() == 1 &&
                saved.profiles.size() == 1 && saved.free_profile == saved.profiles[0].id,
            "✓ shows the bar selected in the editor, saved as the free bar");
}
// R335: while one laser's press on a strip button is pending, before it
// lifts, another laser's press does nothing in the editor, so the drag
// still acts on its own bar's button: dropped outside, that one goes.
void pending_drag_two_lasers() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    write_shortcuts(directory.path, editor_model());
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    tap_control(app, "shortcuts-row-0");
    const auto button = middle(control(app.view(), "shortcuts-button-0").bounds);
    app.move(0, button.x, button.y);
    app.down(0, button.x, button.y, 1100);
    // Another laser taps the other bar's row meanwhile.
    const auto row = middle(control(app.view(), "shortcuts-row-1").bounds);
    app.move(1, row.x, row.y);
    app.down(1, row.x, row.y, 1100.1);
    app.up(1, row.x, row.y);
    require(control(app.view(), "shortcuts-row-0").selected,
            "the other laser's press waits for the drag");
    // The first laser drags its button off the strip.
    for (int step = 1; step <= 6; ++step) {
        app.move(0, button.x + 10 * step, button.y + 40 * step);
    }
    app.up(0, button.x + 60, button.y + 240);
    const auto& bars = app.view();
    require(strip_labels(bars) == std::vector<std::string>{"B", "C"} &&
                control(bars, "shortcuts-row-0").selected,
            "the drag deletes its own bar's button, no other");
    tap_control(app, "shortcuts-row-1");
    require(strip_labels(app.view()).empty(), "the other bar keeps what it had");
    // The Macro view: a held + waits for no other laser's press either.
    tap_control(app, "shortcuts-row-0");
    tap_control(app, "shortcuts-kind-1");
    tap_control(app, "shortcuts-add-wait");
    const auto plus = middle(control(app.view(), "shortcuts-step-plus-1").bounds);
    app.move(0, plus.x, plus.y);
    app.down(0, plus.x, plus.y, 1200);
    const auto other = middle(control(app.view(), "shortcuts-button-1").bounds);
    app.move(1, other.x, other.y);
    app.down(1, other.x, other.y, 1200.1);
    app.up(1, other.x, other.y);
    require(control(app.view(), "shortcuts-button-0").selected,
            "while a + is held, another laser selects nothing");
    app.up(0, plus.x, plus.y);
}
// R336: a save whose folder sync failed put the file in place: the status
// says it was saved but isn't yet safe on disk, and the note that the old
// file couldn't be read goes.
void unsynced_save() {
    if (geteuid() == 0) {
        return; // root opens any folder: the sync can't be made to fail
    }
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    std::ofstream(shortcuts_path(directory.path)) << "{\"schema_version\":1,\"profiles\":[{}]}";
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    click(app, "bar-toggle");
    open_editor(app);
    require(!app.view().shortcut_editor->notice.empty(), "the editor notes the unreadable file");
    tap_control(app, "shortcuts-new-profile");
    tap_mini(app, "Escape");
    // A folder that can't be opened for reading still takes the file.
    fs::permissions(directory.path, fs::perms::owner_write | fs::perms::owner_exec);
    tap_control(app, "shortcuts-save");
    app.flush_settings();
    fs::permissions(directory.path, fs::perms::owner_all);
    app.tick(1200);
    const auto view = app.view();
    require(view.status.find("not yet safe on disk") != std::string::npos &&
                view.status.find("not saved") == std::string::npos,
            "the status says the bars were saved, not yet safe on disk");
    open_editor(app);
    require(app.view().shortcut_editor->notice.empty() && saved_shortcuts(app).profiles.size() == 1,
            "the file was replaced: the unreadable-file note goes");
}
// Carrying a button to another profile (design, Settings and the editor):
// the row under it lights up; dropped there, Move or Copy put it at that
// profile's end, a press elsewhere puts it back; a full profile and its own
// take no drop; the selected profile stays selected.
void carry_buttons() {
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel model;
    model.profiles.push_back(
        {"one",
         "One",
         {shortcut_button("A", {{{}, "KeyA"}}), shortcut_button("B", {{{}, "KeyB"}}),
          shortcut_button("C", {{{}, "KeyC"}})}});
    model.profiles.push_back({"two", "Two", {shortcut_button("Z", {{{}, "KeyZ"}})}});
    model.profiles.push_back(
        {"full", "Full", std::vector<ShortcutButton>(64, shortcut_button("F", {{{}, "KeyF"}}))});
    model.free_profile = "one";
    write_shortcuts(directory.path, model);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    require(control(app.view(), "shortcuts-row-0").selected, "the editor opens on One");
    const auto row = [&](int i) {
        return middle(control(app.view(), "shortcuts-row-" + std::to_string(i)).bounds);
    };
    double now = 1300;
    // Lifts the strip's button `index` and carries it to (x, y), still held.
    const auto carry_to = [&](std::size_t index, double x, double y, unsigned pointer = 0) {
        const auto start =
            middle(control(app.view(), "shortcuts-button-" + std::to_string(index)).bounds);
        app.move(pointer, start.x, start.y);
        app.down(pointer, start.x, start.y, now += .1);
        for (int step = 1; step <= 8; ++step) {
            app.move(pointer, start.x + (x - start.x) * step / 8, start.y + (y - start.y) * step / 8);
        }
    };
    const auto lit = [&] { return app.view().shortcut_editor->drop_row.has_value(); };
    const auto popover = [&] {
        return has(app.view(), "shortcuts-carry-move") && has(app.view(), "shortcuts-carry-copy");
    };
    const auto labels = [&](int profile) {
        tap_control(app, "shortcuts-row-" + std::to_string(profile));
        return strip_labels(app.view());
    };
    // Over Two the row lights; over Full, and over its own One, it doesn't.
    carry_to(0, row(1).x, row(1).y);
    require(lit(), "another profile's row lights up under a carried button");
    app.move(0, row(2).x, row(2).y);
    require(!lit(), "a full profile does not light up");
    app.move(0, row(0).x, row(0).y);
    require(!lit(), "nor its own profile");
    // Let go on its own row: nothing happens.
    app.up(0, row(0).x, row(0).y);
    require(!popover() && strip_labels(app.view()) == std::vector<std::string>{"A", "B", "C"},
            "dropped on its own profile's row, the button stays");
    // Let go on Full: no drop, nothing deleted.
    carry_to(0, row(2).x, row(2).y);
    app.up(0, row(2).x, row(2).y);
    require(!popover() && strip_labels(app.view()) == std::vector<std::string>{"A", "B", "C"},
            "a full profile takes no drop");
    // Let go on Two: the popover, the button waiting at the row's end.
    carry_to(0, row(1).x, row(1).y);
    app.up(0, row(1).x, row(1).y);
    require(popover() && lit() && control(app.view(), "shortcuts-row-0").selected &&
                strip_labels(app.view()) == std::vector<std::string>{"B", "C"},
            "dropped on Two: Move and Copy beside it, One still selected");
    // A press anywhere else puts it back.
    tap_at(app, row(0).x, row(0).y + 300);
    require(!popover() && strip_labels(app.view()) == std::vector<std::string>{"A", "B", "C"} &&
                control(app.view(), "shortcuts-row-0").selected,
            "a press elsewhere closes the popover, the button back in its place");
    // Move: at Two's end, gone from One; One stays selected.
    carry_to(0, row(1).x, row(1).y);
    app.up(0, row(1).x, row(1).y);
    tap_control(app, "shortcuts-carry-move");
    require(!popover() && control(app.view(), "shortcuts-row-0").selected &&
                strip_labels(app.view()) == std::vector<std::string>{"B", "C"},
            "Move takes it from One, which stays selected");
    // Copy, with the other laser pressing Copy.
    carry_to(0, row(1).x, row(1).y);
    app.up(0, row(1).x, row(1).y);
    {
        const auto copy = middle(control(app.view(), "shortcuts-carry-copy").bounds);
        app.move(1, copy.x, copy.y);
        app.down(1, copy.x, copy.y, now += .1);
        app.up(1, copy.x, copy.y);
    }
    require(strip_labels(app.view()) == std::vector<std::string>{"B", "C"}, "Copy leaves it in One");
    require(labels(1) == std::vector<std::string>{"Z", "A", "B"}, "Move and Copy put it at Two's end");
    // The other laser's press elsewhere closes a popover too.
    tap_control(app, "shortcuts-row-0");
    carry_to(1, row(1).x, row(1).y);
    app.up(0, row(1).x, row(1).y);
    require(popover(), "dropped again");
    app.move(1, row(0).x, row(0).y + 300);
    app.down(1, row(0).x, row(0).y + 300, now += .1);
    app.up(1, row(0).x, row(0).y + 300);
    require(!popover() && labels(1).size() == 3,
            "another laser's press elsewhere closes it, nothing carried");
}
void shortcut_editor_settings() {
    for (const bool narrow : {false, true}) {
        TemporaryDirectory directory;
        Settings settings;
        settings.shortcuts = true;
        settings.hide_numpad = narrow;
        save_settings(directory.path, settings);
        write_shortcuts(directory.path, editor_model());
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        require(app.view().shortcut_bar && app.view().shortcut_bar->buttons.empty(),
                "the free bar, empty");
        const auto keyboard_case = panel_case_bounds(app.view());
        tap_rect(app, app.view().shortcut_bar->picker);
        require(app.view().shortcut_bar->menu->manage_enabled, "Manage is enabled");
        tap_rect(app, app.view().shortcut_bar->menu->manage);
        auto view = app.view();
        require(panel_case_bounds(view) == keyboard_case,
                "R329: the editor's case is the keyboard's, its edge and handle in place");
        require(view.shortcut_editor && !view.shortcut_bar && has(view, "shortcuts-back") &&
                    has(view, "shortcuts-save") && !has(view, "settings"),
                "Manage opens the editor, its own screen, in place of the toolbar and bar");
        require(view.shortcut_editor->wide == !narrow &&
                    panel_case_bounds(view).width == keyboard_case.width &&
                    std::abs(keyboard_case.width - (narrow ? 1326 : panel_width)) < 1,
                "the editor keeps the keyboard's width, full or without the numpad");
        require(control(view, "shortcuts-row-1").selected && view.shortcut_editor->strip.width > 0 &&
                    strip_labels(view).empty() && !view.shortcut_editor->empty_hint.empty(),
                "it opens on the bar showing: Two, without buttons");
        require(!app.renderer.hit_key(view, 300, 300), "no key takes a press under the editor");
        require(control(view, "shortcuts-row-0").bounds.width == (narrow ? 236 : 286),
                "the bars column is 286 wide, 236 without the numpad");
        tap_control(app, "shortcuts-row-0");
        view = app.view();
        require(strip_labels(view) == std::vector<std::string>{"A", "B", "C"} &&
                    control(view, "shortcuts-button-0").selected &&
                    control(view, "shortcuts-label").label == "A" &&
                    control(view, "shortcuts-label").bounds.width == (narrow ? 250 : 300),
                "a row selects its bar; its first button is selected");
        require(view.shortcut_editor->ties.size() == 1 && view.shortcut_editor->ties[0].text == "Kate",
                "Used for shows the bar's apps");
        require(control(view, "shortcuts-kind-0").selected &&
                    control(view, "shortcuts-kind-1").enabled &&
                    control(view, "shortcuts-kind-2").enabled,
                "Shortcut is the action; Macro and Run can be chosen");
        require(view.shortcut_editor->chords.size() == 1 &&
                    view.shortcut_editor->chords[0].text == "Ctrl + A",
                "the recorded combinations as chips");
        // A row dragged down the list moves its bar; another hand meanwhile changes nothing.
        {
            const auto row = control(app.view(), "shortcuts-row-0").bounds;
            const auto start = middle(row);
            app.move(0, start.x, start.y);
            app.down(0, start.x, start.y, 960);
            app.move(0, start.x, start.y + 20);
            app.move(0, start.x, start.y + 56);
            const auto save = middle(control(app.view(), "shortcuts-save").bounds);
            app.move(1, save.x, save.y);
            app.down(1, save.x, save.y, 960.1);
            app.up(1, save.x, save.y);
            require(app.view().shortcut_editor.has_value(), "the other hand's press waits for the drag");
            app.up(0, start.x, start.y + 56);
            app.tick(960.2);
        }
        require(row_names(app.view()) == std::vector<std::string>{"Two", "One"} &&
                    control(app.view(), "shortcuts-row-1").selected,
                "a row dragged down moves its bar, selected");
        drag(app, control(app.view(), "shortcuts-row-1").bounds, 0, -56);
        require(row_names(app.view()) == std::vector<std::string>{"One", "Two"}, "and back up");
        // Buttons: dragged sideways along the strip, off it to delete.
        const double pitch = control(app.view(), "shortcuts-button-1").bounds.x -
                             control(app.view(), "shortcuts-button-0").bounds.x;
        drag(app, control(app.view(), "shortcuts-button-0").bounds, 2 * pitch, 0);
        require(strip_labels(app.view()) == std::vector<std::string>{"B", "C", "A"},
                "a button dragged sideways moves along the strip");
        drag(app, control(app.view(), "shortcuts-button-0").bounds, 20, 220);
        require(strip_labels(app.view()) == std::vector<std::string>{"C", "A"},
                "dropped off the strip, it goes");
        tap_control(app, "shortcuts-add");
        view = app.view();
        require(strip_labels(view) == std::vector<std::string>{"C", "A", ""} &&
                    control(view, "shortcuts-button-2").selected &&
                    control(view, "shortcuts-label").label.empty() &&
                    view.shortcut_editor->chords.empty(),
                "+ adds a button with an empty label and action, selected");
        // The Label popup: the mini keyboard under the strip, the editor
        // dimmed behind it, the label growing on its button as it is typed.
        tap_control(app, "shortcuts-label");
        view = app.view();
        require(view.shortcut_editor && view.shortcut_editor->popup &&
                    view.shortcut_editor->popup->kind == ShortcutPopupView::Kind::Label &&
                    !view.shortcut_field && !view.shortcut_bar,
                "Label opens the popup over the editor; the real keyboard stays away");
        popup_fits(view, narrow);
        require(!control(view, "shortcuts-popup-key-NumLock").enabled &&
                    control(view, "shortcuts-popup-key-CapsLock").enabled,
                "R330: for text Num Lock stays disabled and Caps Lock is a switch");
        require(view.modal_prefix() == "shortcuts-popup-" && has(view, "shortcuts-popup-done") &&
                    has(view, "shortcuts-popup-cancel") && !has(view, "shortcuts-popup-fkeys"),
                "row 1: Cancel and Done; the popup is modal");
        // ← under the dimmed editor takes no press.
        tap_control(app, "shortcuts-back");
        require(app.view().shortcut_editor && app.view().shortcut_editor->popup && !app.view().confirm,
                "← under the popup does nothing");
        tap_mini(app, "ShiftLeft");
        require(app.view().shortcut_editor->popup->marks.modifiers.contains(KEY_LEFTSHIFT),
                "a modifier latches on a tap and shows lit");
        tap_mini(app, "KeyH");
        tap_mini(app, "KeyI");
        require(popup_text(app) == "Hi" && app.view().shortcut_editor->popup->marks.modifiers.empty(),
                "a latched Shift capitalizes one key, then lets go");
        require(control(app.view(), "shortcuts-button-2").label == "Hi",
                "the selected button shows the label as it is typed");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "Enter");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "Digit1");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "Enter");
        require(popup_text(app) == "Hi\n!", "Shift+Enter starts a second line, once; Shift+1 is !");
        tap_mini(app, "CapsLock");
        tap_mini(app, "KeyX");
        tap_mini(app, "CapsLock");
        tap_mini(app, "KeyX");
        require(popup_text(app) == "Hi\n!Xx", "Caps Lock toggles capitals");
        tap_mini(app, "ArrowLeft");
        tap_mini(app, "ArrowLeft");
        tap_mini(app, "Backspace");
        tap_mini(app, "Minus");
        tap_mini(app, "ArrowUp");
        tap_mini(app, "End");
        tap_mini(app, "Space");
        require(popup_text(app) == "Hi \n-Xx" && sink.events.empty(),
                "the caret moves with the arrows and Home and End; nothing reaches the app");
        // A held Backspace repeats.
        {
            const auto key = middle(control(app.view(), "shortcuts-popup-key-Backspace").bounds);
            app.move(0, key.x, key.y);
            require(app.down(0, key.x, key.y, 970), "a mini key's press feels like a key's");
            for (double now = 970; now < 970.7; now += .02) {
                app.tick(now);
            }
            require(app.up(0, key.x, key.y), "and its release too");
            require(popup_text(app).size() < 5, "a held Backspace repeats");
        }
        tap_mini(app, "Escape");
        require(!app.view().shortcut_editor->popup &&
                    control(app.view(), "shortcuts-label").label.empty(),
                "Esc cancels: the label stays as it was");
        tap_control(app, "shortcuts-label");
        tap_mini(app, "KeyQ");
        tap_control(app, "shortcuts-popup-cancel");
        require(control(app.view(), "shortcuts-label").label.empty(), "Cancel cancels");
        tap_control(app, "shortcuts-label");
        tap_mini(app, "KeyH");
        tap_mini(app, "KeyI");
        tap_mini(app, "Enter");
        require(control(app.view(), "shortcuts-label").label == "hi", "Enter saves the label");
        tap_control(app, "shortcuts-label");
        tap_mini(app, "Backspace");
        tap_mini(app, "KeyZ");
        tap_control(app, "shortcuts-popup-done");
        require(control(app.view(), "shortcuts-label").label == "hz", "Done saves, and Backspace edits");
        // Recording in the popup.
        tap_control(app, "shortcuts-record");
        view = app.view();
        require(view.shortcut_editor->popup &&
                    view.shortcut_editor->popup->kind == ShortcutPopupView::Kind::Recording &&
                    has(view, "shortcuts-popup-remove-last") && !has(view, "shortcuts-popup-pending"),
                "Record: Remove last, Cancel and Done");
        // R313: F13–F24 aren't offered. Cancel and Done stand at row 1's
        // right; Remove last is an icon button at the right end of the
        // combinations' well, below Done.
        {
            const auto done = control(view, "shortcuts-popup-done").bounds,
                       cancel = control(view, "shortcuts-popup-cancel").bounds;
            const auto remove = control(view, "shortcuts-popup-remove-last");
            const auto& well = view.shortcut_editor->popup->well;
            require(std::none_of(
                        view.controls.begin(), view.controls.end(),
                        [](const Control& c) { return c.id.starts_with("shortcuts-popup-fkey"); }) &&
                        cancel.x + cancel.width + 8 == done.x && done.width == 120 &&
                        cancel.width == 104,
                    "no F13–F24 button or menu while recording");
            require(remove.icon == Icon::DeleteLeft && remove.label.empty() && !remove.enabled &&
                        remove.bounds.x + remove.bounds.width + 4 == well.x + well.width &&
                        remove.bounds.y > well.y &&
                        remove.bounds.y + remove.bounds.height < well.y + well.height,
                    "Remove last is an icon at the well's right end, disabled with nothing recorded");
        }
        popup_fits(view, narrow);
        // R330: the locks are the keyboard's own switches, never recorded.
        require(!control(view, "shortcuts-popup-key-NumLock").enabled &&
                    !control(view, "shortcuts-popup-key-CapsLock").enabled &&
                    view.shortcut_editor->popup->disabled.contains("NumLock") &&
                    view.shortcut_editor->popup->disabled.contains("CapsLock"),
                "Num Lock and Caps Lock are disabled while recording");
        tap_mini(app, "NumLock");
        tap_mini(app, "CapsLock");
        require(popup_chips(app.view()).empty(), "a press on Num or Caps records nothing");
        tap_mini(app, "ControlLeft");
        tap_mini(app, "ShiftLeft");
        require(has(app.view(), "shortcuts-popup-pending") &&
                    control(app.view(), "shortcuts-popup-pending").label == "Ctrl + Shift + …",
                "latched modifiers show as a pending chip");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "KeyK");
        tap_mini(app, "Numpad1");
        tap_mini(app, "AltLeft");
        tap_control(app, "shortcuts-popup-pending");
        tap_mini(app, "F5");
        view = app.view();
        require(popup_chips(view) == std::vector<std::string>{"Ctrl + K", "Num 1", "Alt", "F5"},
                "a second tap lets a latch go; keys, numpad keys and modifiers alone, four at most");
        {
            const auto remove = control(view, "shortcuts-popup-remove-last").bounds;
            const auto& chips = view.shortcut_editor->popup->chords;
            require(remove.x > 0 && control(view, "shortcuts-popup-remove-last").enabled &&
                        std::all_of(chips.begin(), chips.end(),
                                    [&](const ShortcutChip& chip) {
                                        return chip.bounds.x + chip.bounds.width < remove.x;
                                    }),
                    "the combinations stop short of Remove last");
        }
        tap_mini(app, "KeyA");
        require(popup_chips(app.view()).size() == 4 && sink.events.empty(),
                "a fifth is not recorded, and nothing is sent");
        tap_control(app, "shortcuts-popup-remove-last");
        tap_mini(app, "MetaLeft");
        tap_control(app, "shortcuts-popup-done");
        view = app.view();
        require(!view.shortcut_editor->popup && view.shortcut_editor->chords.size() == 4 &&
                    view.shortcut_editor->chords[3].text == "Super",
                "Done records the pending modifiers alone and keeps what was recorded");
        tap_control(app, "shortcuts-record");
        tap_mini(app, "KeyQ");
        tap_control(app, "shortcuts-popup-cancel");
        require(app.view().shortcut_editor->chords.size() == 4, "Cancel keeps the shortcut as it was");
        tap_control(app, "shortcuts-record");
        tap_mini(app, "KeyW");
        tap_control(app, "shortcuts-popup-cancel");
        tap_control(app, "shortcuts-record");
        tap_mini(app, "KeyQ");
        require(popup_chips(app.view()) == std::vector<std::string>{"Q"},
                "a recording starts from nothing");
        app.set_interaction_active(false);
        app.set_interaction_active(true);
        require(app.view().shortcut_editor && !app.view().shortcut_editor->popup &&
                    app.view().shortcut_editor->chords.size() == 4,
                "hiding the keyboard ends a recording as Cancel");
        tap_control(app, "shortcuts-clear");
        require(app.view().shortcut_editor->chords.empty(), "clear empties the shortcut");
        // The bar's name.
        tap_control(app, "shortcuts-rename");
        require(app.view().shortcut_editor->popup &&
                    app.view().shortcut_editor->popup->kind == ShortcutPopupView::Kind::Name &&
                    popup_text(app) == "One",
                "rename opens the popup with its name");
        tap_mini(app, "KeyX");
        tap_mini(app, "ShiftLeft");
        tap_mini(app, "Enter");
        require(row_names(app.view())[0] == "Onex", "Enter saves the name; Shift+Enter too, one line");
        tap_control(app, "shortcuts-new-profile");
        require(app.view().shortcut_editor->popup && popup_text(app) == "Profile 3",
                "New profile names itself");
        tap_mini(app, "Escape");
        require(row_names(app.view()).size() == 3 && row_names(app.view())[2] == "Profile 3" &&
                    control(app.view(), "shortcuts-row-2").selected,
                "the new bar stays with its name when the popup is canceled");
        tap_control(app, "shortcuts-delete");
        require(row_names(app.view()).size() == 2 && !app.view().confirm,
                "an empty bar is deleted at once");
        tap_control(app, "shortcuts-row-0");
        tap_control(app, "shortcuts-tie-0");
        require(app.view().shortcut_editor->ties.empty(), "a tie's ✕ removes it");
        tap_control(app, "shortcuts-delete");
        require(app.view().confirm.has_value(), "a bar with buttons asks first");
        tap_control(app, "confirm-cancel");
        require(row_names(app.view()).size() == 2, "Cancel keeps it");
        // ← with changes asks: Discard leaves the file as it was.
        tap_control(app, "shortcuts-back");
        require(app.view().confirm.has_value() && app.view().shortcut_editor, "← with changes asks");
        tap_control(app, "confirm-cancel");
        require(!app.view().shortcut_editor && saved_shortcuts(app) == editor_model(),
                "Discard leaves without saving");
        // ✓ saves.
        open_editor(app);
        tap_control(app, "shortcuts-row-0");
        tap_control(app, "shortcuts-add");
        tap_control(app, "shortcuts-save");
        const auto stored = saved_shortcuts(app);
        require(!app.view().shortcut_editor && stored.profiles[0].buttons.size() == 4 &&
                    stored.profiles[0].buttons[3].label.empty() &&
                    stored.profiles[0].buttons[3].action.chords.empty(),
                "✓ saves the editor's copy");
        // ← without changes leaves at once; with changes, Save saves.
        open_editor(app);
        tap_control(app, "shortcuts-back");
        require(!app.view().shortcut_editor, "← without changes leaves at once");
        open_editor(app);
        tap_control(app, "shortcuts-row-0");
        tap_control(app, "shortcuts-delete");
        require(app.view().confirm.has_value(), "deleting a bar with buttons asks");
        tap_control(app, "confirm-accept");
        tap_control(app, "shortcuts-back");
        tap_control(app, "confirm-accept");
        require(saved_shortcuts(app).profiles.size() == 1, "Save from the question saves");
    }
    // The limits: the control that would exceed one is disabled.
    TemporaryDirectory directory;
    Settings settings;
    settings.shortcuts = true;
    save_settings(directory.path, settings);
    ShortcutModel full;
    for (int i = 0; i < 32; ++i) {
        full.profiles.push_back({"b" + std::to_string(i), "Bar", {}});
    }
    full.profiles[0].buttons.assign(64, shortcut_button("abcdefghijklmnopqrstuvwx", {{{}, "KeyA"}}));
    full.free_profile = "b0";
    write_shortcuts(directory.path, full);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    open_editor(app);
    auto view = app.view();
    require(!control(view, "shortcuts-new-profile").enabled, "32 profiles: New profile is disabled");
    require(view.shortcut_editor->profiles_thumb.has_value(), "a long list of bars scrolls");
    tap_control(app, "shortcuts-label");
    tap_mini(app, "KeyY");
    tap_mini(app, "Enter");
    require(control(app.view(), "shortcuts-label").label == "abcdefghijklmnopqrstuvwx",
            "a label at 24 characters takes no more");
    // The strip scrolls to its end, where + is disabled at 64 buttons.
    const auto well = app.view().shortcut_editor->strip_well;
    app.move(0, well.x + well.width / 2, well.y + well.height / 2);
    for (int i = 0; i < 80; ++i) {
        app.scroll(0, 1, 0);
    }
    for (double now = 990; now < 992; now += .05) {
        app.tick(now);
    }
    require(has(app.view(), "shortcuts-add") && !control(app.view(), "shortcuts-add").enabled,
            "64 buttons: + is disabled");
}
int main() {
    // Theme pictures the tests draw go to a folder of their own, never to the
    // user's cache (R290).
    TemporaryDirectory cache;
    setenv("XDG_CACHE_HOME", cache.path.c_str(), 1);
    try {
        theme_picker();
        native_num_restore();
        compact_toolbar();
        startup_profile_fallback();
        incompatible_saved_language();
        removed_korean_12key();
        missing_saved_language_status();
        key_control_namespaces();
        favorite_cache_reload();
        background_preferences();
        deferred_reload();
        deferred_reload_errors();
        preferences_with_a_bad_optional_field();
        preferences_with_broken_startup();
        dictation_field_fallbacks();
        reload_optional_field_errors();
        reload_startup();
        launch_startup();
        numpad_settings();
        legacy_background_surface_tests();
        background_surface_setting();
        key_flicks_setting();
        extended_key_collision_setting();
        debug_card_setting();
        interface_language_setting();
        haptic_sounds_setting();
        vibration_setting();
        app_settings();
        dictation_settings();
        desktop_unicode_setting();
        theme_preview();
        languages_popup();
        language_drag_with_two_hands();
        main_language_stays();
        behavior_settings();
        auto_leveling_settings();
        hover_fade_settings();
        curved_keyboard_settings();
        native_replacement_settings();
        streaming_support_setting();
        streaming_support_failed_setup();
        streaming_support_failed_check();
        streaming_support_failed_turn_off();
        keep_open_setting();
        favorite_schema();
        fractional_dropdown_scroll();
        drag_dropdown();
        scroll_cards();
        shortcut_bar_settings();
        shortcut_buttons();
        shortcut_press_bar_change();
        shortcut_invalid_note();
        shortcut_editor_settings();
        shortcut_popup_flick();
        long_combinations();
        shortcut_macro_view();
        macro_buttons();
        first_bar_shows();
        shortcut_apps();
        shortcut_stream_apps();
        shortcut_long_program_tie();
        shortcut_pick_history();
        shortcut_name_record();
        shortcut_fkeys_from_file();
        stick_scrolls_bar();
        run_buttons();
        run_editor();
        run_script_past_list();
        dashboard_unknown_keeps_focus();
        macro_first_row_scrolls();
        carry_closes_on_scroll();
        bar_presses_and_holds();
        pending_drag_two_lasers();
        carry_buttons();
        lifted_step_moves_whole();
        unsynced_save();
        std::cout << "Settings, global numpad and pin persistence, favorites, popup cancellation and "
                     "card scrolling passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
