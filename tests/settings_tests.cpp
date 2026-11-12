#include "framekeyboard/app.hpp"
#include "framekeyboard/cjk.hpp"
#include "framekeyboard/key_sounds.hpp"
#include "framekeyboard/startup.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <stdexcept>
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
void click(App& app, const std::string& id, unsigned pointer = 0) {
    if (!has(app.view(), id) && app.view().settings) {
        // The Integration card is cut off at the right until the row scrolls.
        app.move(pointer, 800, 300);
        app.scroll(pointer, 1000, 0);
        static double now = 100;
        for (int i = 0; i < 40; ++i) {
            app.tick(now += .05);
        }
    }
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
                !control(settings_view, "use-drag-to-move").selected,
            "General card has an unchecked movement preference");
    const auto numpad_bounds = control(settings_view, "hide-numpad").bounds;
    const auto movement_bounds = control(settings_view, "use-drag-to-move").bounds;
    const auto leveling_bounds = control(settings_view, "auto-leveling").bounds;
    require(numpad_bounds.x == movement_bounds.x && movement_bounds.x == leveling_bounds.x &&
                movement_bounds.y - numpad_bounds.y == leveling_bounds.y - movement_bounds.y,
            "General checkboxes form one evenly spaced vertical column");
    const auto theme_bounds = control(settings_view, "theme").bounds;
    require(movement_bounds.x + movement_bounds.width == theme_bounds.x + theme_bounds.width,
            "General controls share one right edge");
    for (const auto* text : {"Theme", "Hide numpad", "Grab to move", "Auto leveling"}) {
        require(std::any_of(settings_view.field_labels.begin(), settings_view.field_labels.end(),
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
    require(control(app.view(), "use-drag-to-move").selected && !app.use_drag_to_move(),
            "movement preference remains staged until Apply");
    app.back();
    app.show_settings();
    require(!control(app.view(), "use-drag-to-move").selected, "Back discards a staged movement change");
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
    require(control(app.view(), "auto-leveling").selected, "Auto leveling starts checked");
    click(app, "auto-leveling");
    require(app.auto_leveling() && !control(app.view(), "auto-leveling").selected,
            "Auto leveling stays pending until Apply");
    app.back();
    app.show_settings();
    require(control(app.view(), "auto-leveling").selected, "Back discards Auto leveling edits");
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
    require(app.auto_leveling() && control(app.view(), "auto-leveling").selected,
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
    require(described("You will see Unicode codes"), "Type is described in the legend colour");
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
    // When the check failed at Steam's start, the card says that instead of asking for a restart.
    status.failed = true;
    app.back();
    click(app, "confirm-cancel");
    open();
    click(app, "replace-native-keyboard");
    click(app, "streaming-support");
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
int main() {
    try {
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
        native_replacement_settings();
        streaming_support_setting();
        keep_open_setting();
        favorite_schema();
        fractional_dropdown_scroll();
        drag_dropdown();
        scroll_cards();
        std::cout << "Settings, global numpad and pin persistence, favorites, popup cancellation and "
                     "card scrolling passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
