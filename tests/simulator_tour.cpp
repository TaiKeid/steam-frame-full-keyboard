// Renders the desktop simulator's window contents in a few typed states to
// PNG files for review, driving its pointer calls as the window does:
//   simulator-tour OUTPUT_DIR
// Typed text is the tour's own. With Anthy loadable (LD_LIBRARY_PATH and
// FRAMEKEYBOARD_ANTHY_DICTIONARY, as for the simulator) candidates are real.
#include "framekeyboard/cjk.hpp"
#include "simulator/platform.hpp"
#include "simulator/simulator.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
using namespace framekeyboard::simulator;
namespace {
fs::path output;
double clock_now = 10;
void wait(Simulator& simulator, double seconds = .1) {
    for (const double end = clock_now + seconds; clock_now < end; clock_now += .01) {
        simulator.tick(clock_now);
    }
}
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
    if (right < 0) {
        throw std::runtime_error("no key " + id);
    }
    return {(left + right) / 2, (top + bottom) / 2 + popup_margin};
}
// Directions: 0 tap, 1 left, 2 up, 3 right, 4 down.
void flick(Simulator& simulator, const std::string& id, int direction = 0, double after = .3) {
    const auto [x, y] = key_point(simulator, id);
    const double dx = direction == 1 ? -60 : direction == 3 ? 60 : 0;
    const double dy = direction == 2 ? -60 : direction == 4 ? 60 : 0;
    simulator.pointer_move(Simulator::mouse, x, y);
    simulator.pointer_down(Simulator::mouse, x, y, clock_now);
    wait(simulator, .03);
    simulator.pointer_move(Simulator::mouse, x + dx / 2, y + dy / 2);
    simulator.pointer_move(Simulator::mouse, x + dx, y + dy);
    simulator.pointer_up(Simulator::mouse, x + dx, y + dy);
    wait(simulator, after);
}
void control(Simulator& simulator, const std::string& id) {
    for (const auto& c : simulator.app().view().controls) {
        if (c.id == id) {
            const double x = c.bounds.x + c.bounds.width / 2,
                         y = c.bounds.y + c.bounds.height / 2 + popup_margin;
            simulator.pointer_move(Simulator::mouse, x, y);
            simulator.pointer_down(Simulator::mouse, x, y, clock_now);
            simulator.pointer_up(Simulator::mouse, x, y);
            wait(simulator);
            return;
        }
    }
    throw std::runtime_error("no control " + id);
}
void click(Simulator& simulator, Rect r) {
    simulator.pointer_down(Simulator::mouse, r.x + 5, r.y + 5, clock_now);
    simulator.pointer_up(Simulator::mouse, r.x + 5, r.y + 5);
    wait(simulator);
}
void shot(Simulator& simulator, const std::string& name) {
    wait(simulator, .3); // live candidates come after a pause
    simulator.invalidate();
    simulator.tick(clock_now);
    cairo_surface_write_to_png(simulator.canvas(), (output / (name + ".png")).c_str());
    std::cout << name << '\n';
}
Setup setup(const fs::path& config, const char* layout, const char* language) {
    Setup result;
    result.config_dir = config;
    result.models_dir = config / "models";
    result.layout = layout;
    result.language = language;
    result.wait_for_japanese = true;
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: simulator-tour OUTPUT_DIR\n";
            return 2;
        }
        output = argv[1];
        fs::create_directories(output);
        if (const char* dictionary = std::getenv("FRAMEKEYBOARD_ANTHY_DICTIONARY")) {
            if (const auto problem = use_anthy_dictionary(dictionary); !problem.empty()) {
                std::cerr << problem << '\n';
            }
        }
        char scratch[] = "/tmp/framekeyboard-simulator-tour-XXXXXX";
        if (!mkdtemp(scratch)) {
            throw std::runtime_error("temporary directory");
        }
        const fs::path root = scratch;
        {
            Simulator simulator(setup(root / "ja", "ja-flick-12", "ja-flick"));
            wait(simulator);
            shot(simulator, "01-flick-idle");
            // にほんご: な left, は down, わ up, か down and ゛.
            auto nihongo = [&] {
                for (const auto& [slot, direction] : std::vector<std::pair<std::string, int>>{
                         {"na", 1}, {"ha", 4}, {"wa", 2}, {"ka", 4}, {"mark", 1}}) {
                    flick(simulator, "Flick-" + slot, direction);
                }
            };
            nihongo();
            shot(simulator, "02-flick-reading");
            flick(simulator, "Space");
            shot(simulator, "03-converting");
            flick(simulator, "Enter");
            shot(simulator, "04-committed");
            // The button is off the keyboard, so its click ends ↶'s chance;
            // the phrase to reopen comes after it.
            click(simulator, Simulator::slow_button());
            nihongo();
            flick(simulator, "Space");
            flick(simulator, "Enter", 0, .5);
            flick(simulator, "Flick-undo", 0, .5);
            shot(simulator, "05-reopened-slow-field");
            flick(simulator, "Space");
            shot(simulator, "05b-converting-after-text"); // docs/images/simulator.png
            flick(simulator, "Enter");
            flick(simulator, "Emoji");
            // The emoji font is checked and drawn on a worker, in real time.
            for (int i = 0; i < 400 && !simulator.app().emoji_settled(); ++i) {
                clock_now = std::max(clock_now, monotonic_seconds());
                simulator.tick(clock_now);
                usleep(20000);
            }
            shot(simulator, "05c-emoji");
            simulator.shutdown();
        }
        {
            Simulator simulator(setup(root / "en", "en-us-full", "en-us"));
            for (const char* key : {"ShiftLeft", "KeyH", "KeyE", "KeyL", "KeyL", "KeyO", "Space", "KeyW",
                                    "KeyO", "KeyR", "KeyL", "KeyD"}) {
                flick(simulator, key, 0, .05);
            }
            const auto [sx, sy] = key_point(simulator, "ShiftLeft");
            simulator.toggle_hold(sx, sy, clock_now);
            wait(simulator, .05);
            for (int i = 0; i < 5; ++i) {
                flick(simulator, "ArrowLeft", 0, .05);
            }
            shot(simulator, "06-english-selection-held-shift");
            simulator.toggle_hold(0, 0, clock_now);
            simulator.shutdown();
        }
        {
            Simulator simulator(setup(root / "ko", "en-us-full", "ko-kr"));
            // 한국어 in two-set jamo, where libhangul is there to compose it.
            if (CjkComposer::supported("korean-2set")) {
                for (const auto* key : {"KeyG", "KeyK", "KeyS", "KeyR", "KeyN", "KeyR", "KeyD", "KeyJ"}) {
                    flick(simulator, key, 0, .1);
                }
            }
            shot(simulator, "07-korean");
            if (CjkComposer::supported("korean-2set")) {
                // 한/영, in right Alt's place, types 어 and turns the keys Latin.
                for (const auto* key : {"AltRight", "KeyA", "KeyB"}) {
                    flick(simulator, key, 0, .1);
                }
                shot(simulator, "07-korean-latin");
            }
            control(simulator, "settings");
            shot(simulator, "08-settings");
            simulator.shutdown();
        }
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "simulator-tour: " << error.what() << '\n';
        return 1;
    }
}
