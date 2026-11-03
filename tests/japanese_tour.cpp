// Renders the Japanese keyboard's states to PNG files for review:
//   japanese-tour OUTPUT_DIR
// It types into a capture sink only. With Anthy loadable (LD_LIBRARY_PATH,
// FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY as for the tests) candidates are real.
#include "framekeyboard/app.hpp"
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
namespace {
struct Sink : KeySink {
    void send(int, int) override {}
    bool text_available() override { return true; }
    bool commit_text(const std::string&) override { return true; }
};
fs::path output;
std::pair<double, double> centre(App& app, const std::string& id) {
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
    if (right < 0) {
        throw std::runtime_error("no key " + id);
    }
    return {(left + right) / 2, (top + bottom) / 2};
}
double clock_now = 10;
void tick(App& app, double seconds = .05) {
    for (double end = clock_now + seconds; clock_now < end; clock_now += .05) {
        app.tick(clock_now);
    }
}
void press(App& app, const std::string& id, double dx = 0, double dy = 0, bool release = true) {
    const auto [x, y] = centre(app, id);
    app.down(0, x, y, clock_now);
    if (dx != 0 || dy != 0) {
        app.move(0, x + dx / 2, y + dy / 2);
        app.move(0, x + dx, y + dy);
    }
    if (release) {
        app.up(0, x + dx, y + dy);
    }
    tick(app);
}
void flicks(App& app, const std::vector<std::pair<std::string, int>>& sequence) {
    // Directions: 0 tap, 1 left, 2 up, 3 right, 4 down.
    for (const auto& [slot, direction] : sequence) {
        const double d = 60;
        const double dx = direction == 1 ? -d : direction == 3 ? d : 0;
        const double dy = direction == 2 ? -d : direction == 4 ? d : 0;
        press(app, "Flick-" + slot, dx, dy);
        tick(app, .3); // apart from a toggle
    }
}
int problems = 0;
void shot(App& app, const std::string& name) {
    tick(app, .3); // live candidates come after a pause
    app.paint(clock_now);
    app.renderer.write_png(output / (name + ".png"));
    std::cout << name << '\n';
    // Everything that takes a press must lie on the case, which is the laser's mask.
    const auto view = app.view();
    const auto body = panel_case_bounds(view);
    for (const auto& control : view.controls) {
        const auto& r = control.bounds;
        if (control.style != ControlStyle::MoveHandle &&
            (r.x < body.x || r.x + r.width > body.x + body.width || r.y < body.y ||
             r.y + r.height > body.y + body.height)) {
            std::cout << "  outside the case: " << control.id << '\n';
            ++problems;
        }
    }
}
void control(App& app, const std::string& id) {
    for (const auto& c : app.view().controls) {
        if (c.id == id) {
            app.down(9, c.bounds.x + 4, c.bounds.y + 4, clock_now);
            app.up(9, c.bounds.x + 4, c.bounds.y + 4);
            tick(app);
            return;
        }
    }
    throw std::runtime_error("no control " + id);
}
// にほんごを: に (な left), ほ (は down), ん (わ up), ご (か down, then ゛), を (わ left).
const std::vector<std::pair<std::string, int>> nihongo{{"na", 1}, {"ha", 4},   {"wa", 2},
                                                       {"ka", 4}, {"mark", 1}, {"wa", 1}};
std::unique_ptr<App> keyboard(const fs::path& config, const std::string& extra, Mode mode, Sink& sink,
                              const std::string& theme = "graphite",
                              const std::string& layout = "ja-flick-12",
                              const std::string& language = "ja-flick") {
    fs::create_directories(config);
    std::ofstream(config / "config.json")
        << R"({"schema_version":1,"active":{"layout":")" << layout << R"(","language":")" << language
        << R"(","theme":")" << theme << R"("})" << extra << "}";
    Options options;
    options.config_dir = config;
    options.mode = mode;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    auto app = std::make_unique<App>(options, sink);
    tick(*app);
    return app;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: japanese-tour OUTPUT_DIR\n";
            return 2;
        }
        output = argv[1];
        fs::create_directories(output);
        if (const auto* dictionary = std::getenv("FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY")) {
            if (void* anthy = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL)) {
                if (auto* set = reinterpret_cast<void (*)(const char*, const char*)>(
                        dlsym(anthy, "anthy_conf_override"))) {
                    set("DIC_FILE", dictionary);
                }
            }
        }
        char scratch[] = "/tmp/framekeyboard-tour-XXXXXX";
        if (!mkdtemp(scratch)) {
            throw std::runtime_error("temporary directory");
        }
        const fs::path root = scratch;
        Sink sink;
        {
            auto app = keyboard(root / "a", "", Mode::Vr, sink);
            shot(*app, "01-kana-idle");
            flicks(*app, nihongo);
            shot(*app, "02-kana-live-candidates");
            press(*app, "ArrowLeft");
            press(*app, "ArrowLeft");
            shot(*app, "02b-kana-cursor");
            press(*app, "ArrowRight");
            press(*app, "ArrowRight");
            press(*app, "Space");
            shot(*app, "03-converting");
            press(*app, "ArrowRight");
            shot(*app, "04-converting-resized");
            const auto chips = app->view().candidates;
            if (chips && !chips->chips.empty()) {
                const auto& r = chips->chips.front().bounds;
                app->down(7, r.x + r.width / 2, r.y + r.height / 2, clock_now);
                app->up(7, r.x + r.width / 2, r.y + r.height / 2);
                shot(*app, "05-second-segment");
            }
            press(*app, "Enter");
            {
                const auto [x, y] = centre(*app, "Flick-ka");
                app->down(0, x, y, clock_now);
                app->move(0, x, y - 60);
                shot(*app, "06-flick-preview");
                app->up(0, x, y - 60);
                tick(*app);
            }
            press(*app, "Backspace");
            press(*app, "Flick-mode");
            shot(*app, "07-english-page");
            press(*app, "Flick-mode");
            shot(*app, "08-digits-page");
            {
                const auto [x, y] = centre(*app, "Flick-mode");
                app->down(0, x, y, clock_now);
                app->move(0, x - 60, y);
                shot(*app, "09-mode-key-preview");
                app->up(0, x - 60, y);
                tick(*app);
            }
            press(*app, "Emoji");
            // Wait for the emoji font to be checked and the view drawn.
            for (int i = 0; i < 400 && !app->emoji_settled(); ++i) {
                app->tick(monotonic_seconds());
                usleep(20000);
            }
            shot(*app, "10-emoji");
            press(*app, "Emoji");
        }
        {
            auto app = keyboard(root / "b", "", Mode::RenderDictation, sink);
            app->show_dictation();
            tick(*app);
            shot(*app, "11-dictation-kana");
            flicks(*app, nihongo);
            shot(*app, "12-dictation-reading");
            press(*app, "Flick-mode");
            shot(*app, "13-dictation-english");
        }
        for (const auto& [theme, extra, name] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"porcelain", "", "14-light-theme"},
                 {"graphite", R"(,"background_surface":"invisible")", "15-see-through"}}) {
            auto app = keyboard(root / name, extra, Mode::Vr, sink, theme);
            flicks(*app, nihongo);
            shot(*app, name);
        }
        {
            auto app = keyboard(root / "c", "", Mode::Vr, sink, "graphite", "en-us-full", "ja-romaji");
            for (const char key : std::string("nihongowo")) {
                press(*app, std::string("Key") + static_cast<char>(std::toupper(key)));
            }
            shot(*app, "16-romaji-live");
            press(*app, "Space");
            shot(*app, "17-romaji-converting");
        }
        fs::remove_all(root);
        return problems == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
