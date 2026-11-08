// Renders the keyboard's interface in one interface language to PNG files,
// and lists the interface labels that shrank, wrapped or were cut in
// fits.tsv (docs/localization-design.md):
//   interface-tour OUTPUT_DIR [LANGUAGE]
// LANGUAGE is a tag from locales/ or qps, the pseudo-language; English by
// default. Other languages need a debug build: the Debug card picks them.
#include "framekeyboard/app.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
struct Sink : KeySink {
    void send(int, int) override {}
};
double now = 100;
fs::path output;
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
        options.mode = Mode::Render;
        options.debug = true;
        Sink sink;
        App app(options, sink);
        if (language != "en") {
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
        settle(app);
        shot(app, "keyboard");
        app.show_settings();
        settings_cards(app);
        app.move(0, 800, 300);
        app.scroll(0, -5000, 0);
        settle(app);
        click(app, "theme");
        shot(app, "settings-dropdown");
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
        app.show_settings(); // Settings closes the picker; Back returns to the keys.
        app.back();
        app.show_dictation();
        shot(app, "dictation");
        std::error_code ignored;
        fs::remove_all(config, ignored);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
