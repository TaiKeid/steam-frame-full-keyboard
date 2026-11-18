// Renders the keyboard's interface in one interface language to PNG files,
// and lists the interface labels that shrank, wrapped or were cut in
// fits.tsv (docs/localization-design.md):
//   interface-tour OUTPUT_DIR [LANGUAGE]
// LANGUAGE is a tag from locales/ or qps, the pseudo-language; English by
// default. Other languages need a debug build: the Debug card picks them.
#include "framekeyboard/app.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
// Sends nothing. The app with the keys is one of the simulator's made-up
// apps, for the picker's header and the profiles assigned to it.
struct Sink : KeySink {
    void send(int, int) override {}
    FocusedApp focus{1, {}, {}};
    std::uint32_t focused_window() const override { return focus.window; }
    FocusedApp focused_app() const override { return focus; }
};
double now = 100;
fs::path output;
// The scripts the Run list shows: made-up names, never the computer's own.
std::vector<std::string> tour_scripts;
std::ofstream fits_report;

void settle(App& app, int frames = 40) {
    for (int i = 0; i < frames; ++i) {
        app.tick(now += .05);
    }
}
bool has(const PanelView& view, const std::string& id) {
    return std::any_of(view.controls.begin(), view.controls.end(),
                       [&](const Control& control) { return control.id == id; });
}
Control control(const PanelView& view, const std::string& id) {
    for (const auto& item : view.controls) {
        if (item.id == id) {
            return item;
        }
    }
    throw std::runtime_error("missing control " + id);
}
void click(App& app, const std::string& id) {
    const auto item = control(app.view(), id);
    const double x = item.bounds.x + item.bounds.width / 2, y = item.bounds.y + item.bounds.height / 2;
    app.down(0, x, y, 1);
    app.up(0, x, y);
    settle(app, 10);
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
// The last card, where the Debug card is, in view.
void to_last_card(App& app) {
    app.move(0, 800, 300);
    app.scroll(0, 5000, 0);
    settle(app);
}
// One report line per label: its line breaks written as \\n.
std::string escaped(const std::string& text) {
    std::string result;
    for (const char c : text) {
        result += c == '\n' ? std::string("\\n") : std::string(1, c);
    }
    return result;
}
void shot(App& app, const std::string& name) {
    settle(app);
    std::vector<LabelFit> fits;
    record_label_fits(&fits);
    app.paint(now);
    record_label_fits(nullptr);
    app.renderer.write_png(output / (name + ".png"));
    const auto font = app.view().interface_font;
    for (const auto& fit : fits) {
        if (fit.family == font && (fit.cut || fit.scale < .999 || fit.lines > 1)) {
            fits_report << name << '\t'
                        << (fit.cut            ? "cut"
                            : fit.scale < .999 ? "shrunk"
                                               : "wrapped")
                        << '\t' << static_cast<int>(fit.scale * 100 + .5) << "%\t" << fit.lines << '\t'
                        << escaped(fit.text) << '\n';
        }
    }
}
// The interface language for the run, as the Debug card picks it.
void choose_language(App& app, const std::string& language) {
    if (language == "en") {
        return;
    }
    app.show_settings();
    to_last_card(app);
    click(app, "debug-interface-language");
    const auto choice = "choose:debug-interface-language:" + language;
    for (int i = 0; i < 40 && !has(app.view(), choice); ++i) {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, 1);
        settle(app, 2);
    }
    click(app, choice);
    click(app, "apply");
    if (app.interface_language() != language) {
        throw std::runtime_error("no language " + language);
    }
}
// Every card, a step at a time, each also with its rows scrolled down.
void settings_cards(App& app) {
    app.move(0, 800, 300);
    app.scroll(0, -5000, 0);
    settle(app);
    for (int step = 0; step < 8; ++step) {
        char name[32];
        std::snprintf(name, sizeof name, "settings-%02d", step);
        shot(app, name);
        for (const auto& card : app.view().cards) {
            app.move(0, card.bounds.x + card.bounds.width / 2, card.bounds.y + card.bounds.height / 2);
            app.scroll(0, 0, 40);
        }
        shot(app, std::string(name) + "-lower");
        for (const auto& card : app.view().cards) {
            app.move(0, card.bounds.x + card.bounds.width / 2, card.bounds.y + card.bounds.height / 2);
            app.scroll(0, 0, -40);
        }
        app.move(0, 800, 300);
        app.scroll(0, 450, 0);
        settle(app);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: interface-tour OUTPUT_DIR [LANGUAGE]\n";
        return 2;
    }
    try {
        output = argv[1];
        const std::string language = argc > 2 ? argv[2] : "en";
        fs::create_directories(output);
        fits_report.open(output / "fits.tsv");
        std::string config =
            (fs::temp_directory_path() / "framekeyboard-interface-tour-XXXXXX").string();
        if (!mkdtemp(config.data())) {
            throw std::runtime_error("temporary folder");
        }
        Options options;
        options.config_dir = config;
        options.theme_previews_dir = fs::path(config) / "theme-previews"; // not the user's cache
        options.mode = Mode::Render;
        options.debug = true;
        Sink sink;
        App app(options, sink);
        choose_language(app, language);
        settle(app);
        shot(app, "keyboard");
        app.show_settings();
        settings_cards(app);
        app.move(0, 800, 300);
        app.scroll(0, -5000, 0);
        settle(app);
        click(app, "layout");
        shot(app, "settings-dropdown");
        app.back();
        click(app, "theme");
        shot(app, "themes");
        app.back();
        click(app, "additional-languages");
        shot(app, "languages");
        click(app, "languages-close");
        app.back();
        app.show_emoji();
        for (int i = 0; i < 400 && !app.emoji_settled(); ++i) {
            settle(app, 1);
            usleep(10000);
        }
        shot(app, "emoji");
        // The shortcut bar with the Debug card's sample bars, its picker's
        // menu, and the bar with its last buttons scrolled into view.
        {
            Options bar_options = options;
            // Under a home of its own, so the scripts folder reads as a user's.
            const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
            setenv("HOME", config.c_str(), 1);
            bar_options.config_dir = fs::path(config) / ".config" / "framekeyboard";
            fs::create_directories(bar_options.config_dir);
            bar_options.script_list = [](const fs::path&) { return tour_scripts; };
            Settings settings;
            settings.shortcuts = true;
            save_settings(bar_options.config_dir, settings);
            App bars(bar_options, sink);
            choose_language(bars, language);
            bars.show_settings();
            to_last_card(bars);
            if (has(bars.view(), "debug-sample-shortcuts")) {
                click(bars, "debug-sample-shortcuts");
                bars.back();
                shot(bars, "shortcut-bar");
                const auto picker = bars.view().shortcut_bar->picker;
                const double x = picker.x + picker.width / 2, y = picker.y + picker.height / 2;
                bars.move(0, x, y);
                shot(bars, "shortcut-bar-picker-tooltip");
                bars.down(0, x, y, 1);
                bars.up(0, x, y);
                shot(bars, "shortcut-bar-menu");
                const auto row = bars.view().shortcut_bar->menu->rows[1].bounds;
                bars.move(0, row.x + 30, row.y + row.height / 2);
                bars.down(0, row.x + 30, row.y + row.height / 2, 1);
                bars.up(0, row.x + 30, row.y + row.height / 2);
                const auto plate = bars.view().shortcut_bar->plate;
                bars.move(0, plate.x + plate.width / 2, plate.y + plate.height / 2);
                bars.scroll(0, 3, 0);
                shot(bars, "shortcut-bar-scrolled");
                // The editor at both widths, with its popup for a label and for recording.
                // The picker's button: opens its menu, or closes it.
                const auto open_menu = [](App& app) {
                    const auto picker = app.view().shortcut_bar->picker;
                    const double px = picker.x + picker.width / 2, py = picker.y + picker.height / 2;
                    app.move(0, px, py);
                    app.down(0, px, py, 1);
                    app.up(0, px, py);
                    settle(app, 10);
                };
                const auto open_editor = [&](App& app) {
                    open_menu(app);
                    const auto edit = app.view().shortcut_bar->menu->manage;
                    const double ex = edit.x + edit.width / 2, ey = edit.y + edit.height / 2;
                    app.move(0, ex, ey);
                    app.down(0, ex, ey, 1);
                    app.up(0, ex, ey);
                    settle(app, 10);
                };
                // A label part typed, Shift latched; Ctrl + K recorded, Ctrl latched.
                const auto popups = [&](App& app, const std::string& suffix) {
                    click(app, "shortcuts-label");
                    click(app, "shortcuts-popup-key-Backspace");
                    click(app, "shortcuts-popup-key-ShiftLeft");
                    shot(app, "shortcut-editor-label" + suffix);
                    click(app, "shortcuts-popup-cancel");
                    click(app, "shortcuts-record");
                    click(app, "shortcuts-popup-key-ControlLeft");
                    click(app, "shortcuts-popup-key-KeyK");
                    click(app, "shortcuts-popup-key-ControlLeft");
                    shot(app, "shortcut-editor-record" + suffix);
                    click(app, "shortcuts-popup-cancel");
                };
                // The Macro view: the sample editing bar's macro.
                const auto macro_view = [&](App& app, const std::string& suffix) {
                    click(app, "shortcuts-row-0");
                    click(app, "shortcuts-button-6");
                    shot(app, "shortcut-editor-macro" + suffix);
                };
                // Run: a script chosen from the list, one gone, the empty
                // list, a command and its popup; saved with the missing
                // script, the bar's status when it can't start.
                const auto wait_for = [&](App& app, const std::function<bool()>& done) {
                    for (int i = 0; i < 300 && !done(); ++i) {
                        usleep(5000);
                        settle(app, 1);
                    }
                };
                const auto run_view = [&](App& app, const std::string& suffix) {
                    click(app, "shortcuts-row-0");
                    click(app, "shortcuts-button-0");
                    click(app, "shortcuts-kind-2");
                    tour_scripts = {"backup-notes.sh", "brightness-down.sh", "brightness-up.sh",
                                    "screenshot.sh",   "toggle-wifi.sh",     "volume-check.sh"};
                    click(app, "shortcuts-refresh");
                    wait_for(app, [&] { return app.view().shortcut_editor->scripts.size() == 6; });
                    click(app, "shortcuts-script-3");
                    shot(app, "shortcut-editor-run" + suffix);
                    tour_scripts.erase(tour_scripts.begin() + 3);
                    click(app, "shortcuts-refresh");
                    wait_for(app, [&] {
                        const auto& rows = app.view().shortcut_editor->scripts;
                        return rows.size() == 6 && rows[3].missing;
                    });
                    shot(app, "shortcut-editor-run-missing" + suffix);
                    tour_scripts.clear();
                    click(app, "shortcuts-refresh");
                    wait_for(app, [&] { return app.view().shortcut_editor->scripts.size() == 1; });
                    click(app, "shortcuts-script-0"); // the missing one, cleared
                    shot(app, "shortcut-editor-run-empty" + suffix);
                    click(app, "shortcuts-run-1");
                    click(app, "shortcuts-command");
                    for (const char* key :
                         {"KeyN", "KeyO", "KeyT", "KeyE", "KeyS", "Space", "KeyH", "KeyI"}) {
                        click(app, std::string("shortcuts-popup-key-") + key);
                    }
                    shot(app, "shortcut-editor-run-command-popup" + suffix);
                    click(app, "shortcuts-popup-done");
                    shot(app, "shortcut-editor-run-command" + suffix);
                    // Saved with the gone script: its press can't start it.
                    click(app, "shortcuts-run-0");
                    tour_scripts = {"screenshot.sh"};
                    click(app, "shortcuts-refresh");
                    wait_for(app, [&] { return app.view().shortcut_editor->scripts.size() == 1; });
                    click(app, "shortcuts-script-0");
                    click(app, "shortcuts-save");
                    const auto button = app.view().shortcut_bar->buttons[0].bounds;
                    const double bx = button.x + button.width / 2, by = button.y + button.height / 2;
                    app.move(0, bx, by);
                    app.down(0, bx, by, 1);
                    app.up(0, bx, by);
                    app.move(0, bx, by + 400);
                    wait_for(app, [&] { return !app.view().status.empty(); });
                    shot(app, "shortcut-bar-run-failed" + suffix);
                };
                open_editor(bars);
                shot(bars, "shortcut-editor");
                popups(bars, "");
                macro_view(bars, "");
                click(bars, "shortcuts-back");
                // The Desktop app the terminal profile is assigned to: its
                // profile shows, its assign button lit in the menu; Used for
                // in the editor.
                const auto assigned_app = [&](App& app, const std::string& suffix) {
                    sink.focus = {
                        3, AppIdentity{AppIdentity::Kind::Desktop, "org.example.Terminal"}, {}};
                    settle(app);
                    open_menu(app);
                    shot(app, "shortcut-bar-menu-app" + suffix);
                    open_menu(app);
                    sink.focus = {1, {}, {}};
                    settle(app);
                };
                assigned_app(bars, "");
                sink.focus = {3, AppIdentity{AppIdentity::Kind::Desktop, "org.example.Terminal"}, {}};
                settle(bars);
                open_editor(bars);
                shot(bars, "shortcut-editor-used-for");
                click(bars, "shortcuts-back");
                sink.focus = {1, {}, {}};
                bars.flush_settings();
                // A copy of the profiles for the narrow keyboard, before Run changes the first button.
                const auto saved_profiles = read_file(shortcuts_path(bar_options.config_dir));
                open_editor(bars);
                run_view(bars, "");
                bars.flush_settings();
                std::ofstream(shortcuts_path(bar_options.config_dir)) << saved_profiles;
                Settings narrow = settings;
                narrow.hide_numpad = true;
                save_settings(bar_options.config_dir, narrow);
                App small(bar_options, sink);
                choose_language(small, language);
                settle(small);
                // The bar and its menu without the numpad.
                shot(small, "shortcut-bar-no-numpad");
                open_menu(small);
                shot(small, "shortcut-bar-menu-no-numpad");
                open_menu(small);
                assigned_app(small, "-no-numpad");
                open_editor(small);
                shot(small, "shortcut-editor-no-numpad");
                popups(small, "-no-numpad");
                macro_view(small, "-no-numpad");
                run_view(small, "-no-numpad");
            } else {
                bars.back();
            }
            setenv("HOME", old_home.c_str(), 1);
        }
        // The empty two-slot bar the toolbar button opens with no profiles, at
        // both widths: its tab too.
        for (const bool numpad : {true, false}) {
            Options empty_options = options;
            empty_options.config_dir = fs::path(config) / (numpad ? "empty-bar" : "empty-bar-narrow");
            fs::create_directories(empty_options.config_dir);
            Settings settings;
            settings.shortcuts = true;
            settings.hide_numpad = !numpad;
            save_settings(empty_options.config_dir, settings);
            App empty(empty_options, sink);
            choose_language(empty, language);
            settle(empty);
            if (!empty.view().shortcut_bar) {
                click(empty, "bar-toggle");
            }
            shot(empty, numpad ? "shortcut-bar-empty" : "shortcut-bar-empty-no-numpad");
        }
        // The editor opens only in dictation's render mode, as for its image export.
        options.mode = Mode::RenderDictation;
        App dictation(options, sink);
        choose_language(dictation, language);
        dictation.show_dictation();
        shot(dictation, "dictation");
        std::error_code ignored;
        fs::remove_all(config, ignored);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
