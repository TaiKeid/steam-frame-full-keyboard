#include "framekeyboard/app.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
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
    const auto c = control(app.view(), id);
    const double x = c.bounds.x + c.bounds.width / 2, y = c.bounds.y + c.bounds.height / 2;
    require(c.hit(x, y), "test clicks a visible control");
    app.down(pointer, x, y, 1);
    app.up(pointer, x, y);
}
void select(App& app, const std::string& field, const std::string& choice) {
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
    click(app, id);
}
Settings saved(const fs::path& dir) {
    std::vector<std::string> errors;
    auto value = load_settings(dir, errors);
    require(errors.empty(), "saved config parses");
    return value;
}
std::pair<double, double> key_point(App& app, const std::string& action) {
    const auto view = app.view();
    for (double y = 96; y < panel_height; y += 4) {
        for (double x = 0; x < panel_width; x += 4) {
            if (const auto* key = app.renderer.hit_key(view, x, y); key && key->action == action) {
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
    for (const auto* input : {"uinput", "ei"}) {
        for (bool saved_num : {false, true}) {
            for (bool target_num : {false, true}) {
                TemporaryDirectory directory;
                Settings settings;
                settings.num_lock = saved_num;
                if (std::string(input) == "ei") {
                    settings.active = {"ja-jis-full", "ja-jis", "graphite"};
                }
                save_settings(directory.path, settings);
                Options options;
                options.config_dir = directory.path;
                options.mode = "vr";
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
                app.set_interaction_active(false);
                sink.lock = !saved_num;
                require(!app.down(0, x, y, 4) && sink.events.empty(),
                        "hidden input cannot synchronize target locks");
            }
        }
    }
}
void numpad_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = "render";
    CaptureSink sink;
    App app(options, sink);
    const auto full_body = panel_case_bounds(app.view());
    require(full_body.width == panel_width && !app.view().keyboard->num(), "default numpad state");
    const auto [x, y] = key_point(app, "NumLock");
    require(app.down(0, x, y, 1), "Num Lock accepted");
    require(app.down(1, x, y, 1), "second Num Lock pointer accepted");
    require(app.view().keyboard->num() && saved(directory.path).num_lock,
            "Num Lock saves once during simultaneous controller holds");
    app.up(0, x, y);
    app.up(1, x, y);
    {
        App reopened(options, sink);
        require(reopened.view().keyboard->num(), "Num Lock survives restart without target input");
    }
    app.show_settings();
    click(app, "hide-numpad");
    require(control(app.view(), "hide-numpad").selected && !saved(directory.path).hide_numpad,
            "Hide numpad stays pending until Apply");
    app.back();
    app.show_settings();
    require(!control(app.view(), "hide-numpad").selected, "Back discards Hide numpad edits");
    click(app, "hide-numpad");
    click(app, "apply");
    require(saved(directory.path).hide_numpad && app.view().hide_numpad,
            "Apply saves global numpad visibility");
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
        app.apply({id,
                   id == "ja-jis-full"     ? "ja-jis"
                   : id == "br-abnt2-full" ? "pt-br"
                                           : "en-us",
                   "graphite"});
        require(app.view().hide_numpad && app.view().keyboard->num() &&
                    panel_case_bounds(app.view()).width < full_body.width,
                "global visibility and Num Lock survive every bundled layout change");
    }
    app.apply({"en-us-full", "en-us", "graphite"});
    auto settings = saved(directory.path);
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
    require(!saved(directory.path).num_lock, "Num Lock off is persisted too");
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
void app_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = "render-settings";
    CaptureSink sink;
    App app(options, sink);
    require(!has(app.view(), "language-cycle"), "no favorites hides language cycle");
    require(control(app.view(), "settings").bounds.x < control(app.view(), "size-larger").bounds.x &&
                control(app.view(), "size-larger").bounds.x < control(app.view(), "pin").bounds.x,
            "pin follows zoom buttons");
    const auto unpinned_icon = control(app.view(), "pin").icon;
    click(app, "pin");
    require(app.pinned() && saved(directory.path).pinned, "pin is immediately persisted");
    require(!control(app.view(), "pin").selected, "pin active state changes icon without background");
    require(control(app.view(), "pin").icon != unpinned_icon, "pin has distinct on/off icons");
    app.set_dragging(true);
    require(app.view().status.empty(), "pin refuses drag capture");
    {
        App reopened(options, sink);
        require(reopened.pinned(), "pin survives restart");
    }
    click(app, "pin");
    require(!app.pinned(), "pin can be disabled");

    app.show_settings();
    require(app.view().cards.size() == 2, "production settings contains exactly two cards");
    require(has(app.view(), "apply") && has(app.view(), "reload"), "settings toolbar actions");
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
    select(app, "language", "de-de");
    require(control(app.view(), "layout").label != "US full-size with left clipboard keys",
            "language selection chooses compatible geometry");
    select(app, "layout", "international-full");
    click(app, "favorite");
    require(control(app.view(), "favorite").selected, "favorite checkbox reflects pending pair");
    app.back();
    require(saved(directory.path).favorites.empty(), "Back discards unsaved favorite edits");
    app.show_settings();
    select(app, "language", "de-de");
    select(app, "layout", "international-full");
    click(app, "favorite");
    select(app, "theme", "midnight");
    // Save English as default with German retained in the pending favorite set.
    select(app, "language", "en-us");
    select(app, "layout", "en-us-full");
    click(app, "apply");
    auto settings = saved(directory.path);
    require(settings.favorites.size() == 1 && settings.active.language == "en-us" &&
                settings.active.theme == "midnight",
            "Apply persists default, theme and favorite changes together");
    require(has(app.view(), "language-cycle"), "favorite exposes main cycle button");
    require(control(app.view(), "pin").bounds.x < control(app.view(), "language-cycle").bounds.x,
            "language cycle follows pin and zoom buttons");
    click(app, "language-cycle");
    require(app.selection().language == "de-de" && app.selection().theme == "midnight",
            "cycle activates favorite pair while retaining current theme");
    require(saved(directory.path).active.language == "en-us",
            "cycling does not overwrite saved default");
    click(app, "pin");
    require(saved(directory.path).active.language == "en-us",
            "pin save preserves default during favorite cycle");
    click(app, "language-cycle");
    require(app.selection().language == "en-us", "cycle returns to saved default");
    settings = saved(directory.path);
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
    select(app, "language", "de-de");
    select(app, "layout", "international-full");
    click(app, "apply");
    settings = saved(directory.path);
    settings.favorites.push_back(
        {"duplicate", "duplicate", {"international-full", "de-de", "graphite"}});
    save_settings(directory.path, settings);
    app.reload();
    const auto same = app.selection();
    click(app, "language-cycle");
    require(app.selection() == same, "one unique favorite equal to default does not cycle");
    app.show_settings();
    require(control(app.view(), "favorite").selected,
            "legacy favorites recognized by layout/language pair");
    click(app, "favorite");
    click(app, "apply");
    require(saved(directory.path).favorites.empty() && !has(app.view(), "language-cycle"),
            "uncheck removes all duplicates and hides main cycle");

    // Reload closes a menu, cancels a pressed choice, and discards staged edits.
    app.show_settings();
    click(app, "favorite");
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
    require(!app.view().popup && !control(app.view(), "favorite").selected && !app.pointer_pressed(1),
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
    std::ifstream updated(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(updated), {});
    require(data.find("custom_setting") != std::string::npos, "settings save preserves unknown fields");
    std::ofstream(directory.path / "config.json") << "{broken";
    click(app, "settings"); // Back to main.
    click(app, "pin");
    require(app.view().status.find("not saved") != std::string::npos, "pin reports failed persistence");
    std::ifstream malformed(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(malformed), {});
    require(data == "{broken", "malformed config is never overwritten");
}
void favorite_schema() {
    TemporaryDirectory directory;
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"favorites":[{"id":"pair","name":"Pair only","layout":"international-full","language":"de-de"}]})";
    const auto config = saved(directory.path);
    require(config.favorites.size() == 1 && config.favorites.front().selection.language == "de-de",
            "favorite pair does not require a legacy theme field");
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
    auto snapshot = [&] {
        PanelView view;
        ui.append(view);
        return view;
    };
    auto view = snapshot();
    require(view.cards.size() == 3 && !has(view, "test-3-field-0"),
            "offscreen cards have no hit controls");
    require(!has(view, "test-0-field-9"), "vertically clipped fields have no controls");
    require(ui.scroll(200, 200, 0, 2), "card scrolls vertically");
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
    ui.toggle("test-0-field-4");
    view = snapshot();
    require(view.popup && view.popup->y + view.popup->height > panel_height,
            "dropdown can extend beyond keyboard border");
    require(view.popup->height == 5 * 52, "overflow popup still shows five choices");
    const auto outside_case = control(view, "choose:test-0-field-4:choice-4");
    require(outside_case.hit(outside_case.bounds.x + 5, outside_case.bounds.y + 5),
            "overflow option remains clickable");
    require(ui.choice(outside_case.id)->second == "choice-4", "overflow option resolves to choice");
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
        numpad_settings();
        app_settings();
        favorite_schema();
        scroll_cards();
        std::cout << "Settings, global numpad and pin persistence, favorites, popup cancellation and "
                     "card scrolling passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
