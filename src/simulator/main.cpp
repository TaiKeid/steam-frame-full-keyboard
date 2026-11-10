// The desktop simulator's window (docs/simulator.md): SDL events become the
// Simulator's pointer calls, and its canvas is shown scaled to the window.
#include "candidate_profile.hpp"
#include "framekeyboard/chinese_candidates.hpp"
#include "framekeyboard/cjk.hpp"
#include "framekeyboard/emoji_words.hpp"
#include "framekeyboard/japanese_candidates.hpp"
#include "framekeyboard/mozc.hpp"
#include "framekeyboard/swipe.hpp"
#include "platform.hpp"
#include "simulator.hpp"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>

namespace fk = framekeyboard;
namespace fs = std::filesystem;
namespace sim = framekeyboard::simulator;
namespace {
void help() {
    std::cout << "Full Keyboard simulator " << FRAMEKEYBOARD_VERSION << "\n\n"
              << "The keyboard types into the text field in its own window, never into\n"
              << "other applications. The field's text stays in memory and is never saved;\n"
              << "Japanese learning keeps the phrases it learns in the settings folder.\n\n"
              << "  --layout ID               Start with this layout\n"
              << "  --language ID             Start with this language, and a layout that can type it\n"
              << "  --theme ID                Start with this theme\n"
              << "  --list                    List the layouts, languages and themes\n"
              << "  --slow-field              Confirm each key after 30 ms, as Steam's own fields do\n"
              << "  --anthy-dictionary FILE   Anthy dictionary for Japanese conversion\n"
              << "                            (or FRAMEKEYBOARD_ANTHY_DICTIONARY)\n"
              << "  --config-dir PATH         Settings folder, by default " << sim::config_dir().string()
              << "\n"
              << "  --data-dir PATH           Extra profile folder\n"
              << "  --duration SECONDS        Quit after this time\n"
              << "  --japanese-candidates     Read phrases from standard input, one a line, kana or\n"
              << "                            romaji, and print what Japanese input offers for each,\n"
              << "                            without a window or learning\n"
              << "  --chinese-candidates      Read Pinyin or Zhuyin readings from standard input, one\n"
              << "                            a line, and print the conversion and candidates of each\n"
              << "                            method, without a window or learning\n"
              << "  --chinese-engine NAME     Compare this engine there (pyzy, libime, mcbopomofo,\n"
              << "                            or all); again for more\n"
              << "  --chinese-method ID       Type the readings with this method only, such as\n"
              << "                            chinese-zhuyin or chinese-quick; again for more\n"
              << "  --candidates N            How many candidates it lists, 12 live and 6 a segment\n"
              << "  --mozc-server PATH        Convert with this mozc_server, not the bundled one\n"
              << "  --anthy-only              Convert with Anthy alone, to compare\n"
#ifdef FRAMEKEYBOARD_DEBUG_MODE
              << "  --debug                   Debug card in Settings, always on in this build\n"
#endif
              << "  --version                 Print the version\n\n"
              << "The selection is saved, so later runs start with it.\n";
}
void handle_signal(int) {
    fk::interrupted = 1;
}
void require(bool ok) {
    if (!ok) {
        throw std::runtime_error(SDL_GetError());
    }
}
struct QuitSDL {
    ~QuitSDL() { SDL_Quit(); }
};
// Where the canvas sits in the window's pixels: as large as fits, centred.
struct Placement {
    double x{}, y{}, scale{1};
};
Placement place(int width, int height) {
    const double scale =
        std::min(double(width) / sim::Simulator::width, double(height) / sim::Simulator::height);
    return {(width - sim::Simulator::width * scale) / 2, (height - sim::Simulator::height * scale) / 2,
            std::max(scale, 1e-3)};
}
} // namespace

int main(int argc, char** argv) {
    // Anthy would log every conversion to this file: nothing typed is logged.
    unsetenv("ANTHY_HISTORY_FILE");
    try {
        sim::Setup setup;
        bool list = false, candidates = false, mozc_chosen = false;
        fk::JapaneseCandidatesOptions candidate_options;
        fk::ChineseCandidatesOptions chinese_options;
        bool chinese_candidates = false;
        double duration = 0;
        fs::path dictionary;
        if (const char* path = std::getenv("FRAMEKEYBOARD_EMOJI_DICTIONARY"); path && *path) {
            fk::set_emoji_dictionary(path); // The Frame has IBus's; a desktop may not.
        }
        if (const char* path = std::getenv("FRAMEKEYBOARD_TABLE_DIRECTORY"); path && *path) {
            fk::set_table_directory(path); // Cangjie and Quick, as above.
        }
        if (const char* path = std::getenv("FRAMEKEYBOARD_ANTHY_DICTIONARY"); path && *path) {
            dictionary = path;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                return argv[i];
            };
            if (argument == "--help") {
                help();
                return 0;
            } else if (argument == "--version") {
                std::cout << "framekeyboard-simulator " << FRAMEKEYBOARD_VERSION << '\n';
                return 0;
            } else if (argument == "--layout") {
                setup.layout = value();
            } else if (argument == "--language") {
                setup.language = value();
            } else if (argument == "--theme") {
                setup.theme = value();
            } else if (argument == "--list") {
                list = true;
            } else if (argument == "--slow-field") {
                setup.slow = true;
#ifdef FRAMEKEYBOARD_DEBUG_MODE
            } else if (argument == "--debug") {
                // Always on in the simulator (Options::debug); accepted to match the keyboard.
#endif
            } else if (argument == "--anthy-dictionary") {
                dictionary = value();
            } else if (argument == "--config-dir") {
                setup.config_dir = value();
            } else if (argument == "--data-dir") {
                setup.data_dir = value();
            } else if (argument == "--japanese-candidates") {
                candidates = true;
            } else if (argument == "--chinese-candidates") {
                chinese_candidates = true;
            } else if (argument == "--chinese-engine") {
                const auto engine = value();
                if (engine == "all") {
                    chinese_options.engines = {"pyzy", "libime", "mcbopomofo"};
                } else {
                    chinese_options.engines.push_back(engine);
                }
            } else if (argument == "--chinese-method") {
                chinese_options.methods.push_back(value());
            } else if (argument == "--no-word-forms") {
                candidate_options.word_forms = false;
            } else if (argument == "--mozc-server") {
                candidate_options.mozc_server = value();
                mozc_chosen = true;
            } else if (argument == "--anthy-only") {
                candidate_options.mozc_server.clear();
                mozc_chosen = true;
            } else if (argument == "--candidates") {
                const auto count = value();
                std::size_t end = 0;
                const auto number = std::stoul(count, &end);
                if (end != count.size() || number == 0) {
                    throw std::runtime_error("--candidates needs a positive number");
                }
                candidate_options.live = candidate_options.segment = chinese_options.count = number;
            } else if (argument == "--duration") {
                const auto seconds = value();
                std::size_t end = 0;
                duration = std::stod(seconds, &end);
                if (end != seconds.size() || !std::isfinite(duration) || duration <= 0) {
                    throw std::runtime_error("duration must be a positive finite number");
                }
            } else {
                throw std::runtime_error("unknown argument: " + argument + " (see --help)");
            }
        }
        if (setup.config_dir.empty()) {
            setup.config_dir = sim::config_dir();
        }
        // An installed keyboard's settings are never the simulator's to change.
        // Without HOME there is no default folder of either to mix up.
        if (std::getenv("HOME") && fs::weakly_canonical(fs::absolute(setup.config_dir)) ==
                                       fs::weakly_canonical(sim::keyboard_config_dir())) {
            throw std::runtime_error(setup.config_dir.string() +
                                     " belongs to the installed keyboard; the simulator keeps its own");
        }
        try {
            setup.models_dir = sim::models_dir();
        } catch (const std::runtime_error&) {
            setup.models_dir = setup.config_dir / "models"; // no HOME, but a settings folder
        }
        if (list) {
            const auto profiles = fk::load_profiles(setup.data_dir, setup.config_dir);
            for (const auto& [id, p] : profiles.layouts) {
                std::cout << "layout " << id << ": " << p.name << '\n';
            }
            for (const auto& [id, p] : profiles.languages) {
                std::cout << "language " << id << ": " << p.name << '\n';
            }
            for (const auto& [id, p] : profiles.themes) {
                std::cout << "theme " << id << ": " << p.name << '\n';
            }
            return 0;
        }
        if (!dictionary.empty()) {
            if (const auto problem = sim::use_anthy_dictionary(dictionary); !problem.empty()) {
                std::cerr << "Full Keyboard simulator: " << problem
                          << ". Japanese conversion may be unavailable.\n";
            }
        }
        if (chinese_candidates) {
            // Nothing is learned or written: the composers get no learning folder.
            fk::set_character_ranks(fk::data_resource(setup.data_dir, "chinese/char-ranks.txt"));
            fk::report_chinese_candidates(std::cin, std::cout, chinese_options);
            return 0;
        }
        fs::create_directories(setup.config_dir);
        if (candidates) {
            // Anthy keeps its lock file in the settings folder, and learns nothing.
            fk::configure_anthy({false, setup.config_dir});
            candidate_options.data_dir = setup.data_dir;
            sim::CandidateProfile profile;
            candidate_options.mozc_profile = profile.path();
            if (!mozc_chosen) {
                candidate_options.mozc_server = fk::mozc_engine(); // As the keyboard converts.
            }
            fk::report_japanese_candidates(std::cin, std::cout, candidate_options);
            return 0;
        }

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        // The first click on an inactive window presses a key, as computer
        // use and a touch screen expect.
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
        require(SDL_Init(SDL_INIT_VIDEO) == 0);
        QuitSDL quit_sdl;
        sim::Simulator simulator(setup);
        std::cout << "Full Keyboard simulator: settings in " << setup.config_dir.string() << '\n'
                  << std::flush;

        SDL_Rect usable{0, 0, sim::Simulator::width, sim::Simulator::height};
        SDL_GetDisplayUsableBounds(0, &usable);
        const double fit = std::min(
            {1.0, .92 * usable.w / sim::Simulator::width, .92 * usable.h / sim::Simulator::height});
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("Full Keyboard simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             static_cast<int>(sim::Simulator::width * fit),
                             static_cast<int>(sim::Simulator::height * fit),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI),
            SDL_DestroyWindow);
        require(window != nullptr);
        // The physical keyboard never types: no text input, no system IME.
        SDL_StopTextInput();
        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
            SDL_CreateRenderer(window.get(), -1, 0), SDL_DestroyRenderer);
        if (!renderer) {
            renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        }
        require(renderer != nullptr);
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
            SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              sim::Simulator::width, sim::Simulator::height),
            SDL_DestroyTexture);
        require(texture != nullptr);

        // Window coordinates to canvas coordinates. Events count in window
        // units, drawing in output pixels; they differ on scaled displays.
        auto canvas_point = [&](double x, double y) {
            int window_width = 1, window_height = 1, output_width = 1, output_height = 1;
            SDL_GetWindowSize(window.get(), &window_width, &window_height);
            SDL_GetRendererOutputSize(renderer.get(), &output_width, &output_height);
            const auto p = place(output_width, output_height);
            const double ratio = double(output_width) / std::max(1, window_width);
            return std::pair{(x * ratio - p.x) / p.scale, (y * ratio - p.y) / p.scale};
        };
        // Each finger on a touch screen is a pointer of its own, numbered
        // from first_finger and reused once it lifts.
        std::map<SDL_FingerID, unsigned> fingers;
        auto finger_pointer = [&](SDL_FingerID id) {
            unsigned pointer = sim::Simulator::first_finger;
            while (std::any_of(fingers.begin(), fingers.end(),
                               [&](const auto& entry) { return entry.second == pointer; })) {
                ++pointer;
            }
            return fingers[id] = pointer;
        };
        bool mouse_pressed = false;
        // A press held as the window lost focus: some desktops then send one
        // pointer motion far outside the window, while the drag and its
        // release go on in it. Such motions are ignored; unless the pointer
        // comes back inside or lets go within half a second, the release
        // went elsewhere and the press is cancelled, as a lost laser is.
        std::optional<double> focus_lost_held;
        auto inside_window = [&](int x, int y) {
            int width = 1, height = 1;
            SDL_GetWindowSize(window.get(), &width, &height);
            return x >= 0 && y >= 0 && x < width && y < height;
        };
        auto on_window_edge = [&](int x, int y) {
            int width = 1, height = 1;
            SDL_GetWindowSize(window.get(), &width, &height);
            return x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1;
        };
        // A mouse can report a thousand moves a second, and each one makes the
        // keyboard look up what is under it. Moves wait, the latest for each
        // pointer, until another event or the end of the batch: one a frame,
        // as VR's lasers report.
        std::map<unsigned, std::pair<double, double>> motion;
        // FRAMEKEYBOARD_SIMULATOR_TRACE: the window's pointer events and when
        // ↶'s chance ends, on stderr, to see what a pointer-driving tool sends.
        // Positions and event names only, never what is typed.
        const bool trace = std::getenv("FRAMEKEYBOARD_SIMULATOR_TRACE") != nullptr;
        const double started = fk::monotonic_seconds();
        bool reopen = false;
        // Values, not a built string: nothing is formatted with the trace off.
        auto note = [&](const auto&... parts) {
            if (trace) {
                std::cerr << "trace " << std::fixed << std::setprecision(3)
                          << fk::monotonic_seconds() - started << ' ';
                (std::cerr << ... << parts) << '\n';
            }
        };
        auto watch_reopen = [&](const char* after) {
            if (trace && simulator.app().reopen_pending() != reopen) {
                reopen = !reopen;
                note("↶ chance ", reopen ? "opens" : "ends", " after ", after);
            }
        };
        auto flush_motion = [&] {
            for (const auto& [pointer, at] : motion) {
                note("move ", pointer, ' ', std::lround(at.first), ',', std::lround(at.second));
                simulator.pointer_move(pointer, at.first, at.second);
                watch_reopen("move");
            }
            motion.clear();
        };
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        const double start = fk::monotonic_seconds();
        bool done = false, present = true;
        while (!done && !simulator.quitting() && !fk::interrupted) {
            const double now = fk::monotonic_seconds();
            if (duration > 0 && now - start >= duration) {
                break;
            }
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, simulator.app().renderer.animating() ? 16 : 25)) {
                do {
                    const double at = fk::monotonic_seconds();
                    if (event.type != SDL_MOUSEMOTION && event.type != SDL_FINGERMOTION) {
                        flush_motion();
                    }
                    switch (event.type) {
                    case SDL_QUIT:
                        done = true;
                        break;
                    case SDL_MOUSEMOTION:
                        // Touches arrive as fingers below, not as their mouse copies.
                        if (event.motion.which != SDL_TOUCH_MOUSEID) {
                            if (focus_lost_held) {
                                if (!inside_window(event.motion.x, event.motion.y)) {
                                    note("ignored move ", event.motion.x, ',', event.motion.y);
                                    break;
                                }
                                focus_lost_held.reset();
                            }
                            // A pointer-driving tool parks the mouse after a click,
                            // the button up, at the window's edge, where SDL clamps
                            // a place outside it: the mouse is leaving, as LEAVE
                            // says, so only the hover ends and ↶'s chance stays.
                            if (!mouse_pressed && on_window_edge(event.motion.x, event.motion.y)) {
                                motion.erase(sim::Simulator::mouse);
                                note("edge move ", event.motion.x, ',', event.motion.y);
                                simulator.pointer_exit(sim::Simulator::mouse);
                                watch_reopen("edge move");
                                break;
                            }
                            motion[sim::Simulator::mouse] = canvas_point(event.motion.x, event.motion.y);
                        }
                        break;
                    case SDL_MOUSEBUTTONDOWN:
                    case SDL_MOUSEBUTTONUP:
                        if (event.button.which != SDL_TOUCH_MOUSEID) {
                            const auto [x, y] = canvas_point(event.button.x, event.button.y);
                            const bool down = event.type == SDL_MOUSEBUTTONDOWN;
                            note(down ? "down " : "up ", static_cast<int>(event.button.button), ' ',
                                 std::lround(x), ',', std::lround(y));
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                mouse_pressed = down;
                                focus_lost_held.reset();
                                if (down) {
                                    simulator.pointer_down(sim::Simulator::mouse, x, y, at);
                                } else {
                                    simulator.pointer_up(sim::Simulator::mouse, x, y);
                                }
                            } else if (event.button.button == SDL_BUTTON_RIGHT && down) {
                                simulator.toggle_hold(x, y, at);
                            }
                        }
                        break;
                    case SDL_MOUSEWHEEL:
                        if (event.wheel.which != SDL_TOUCH_MOUSEID) {
                            int mouse_x = 0, mouse_y = 0;
                            SDL_GetMouseState(&mouse_x, &mouse_y);
                            const auto [x, y] = canvas_point(mouse_x, mouse_y);
                            const double direction =
                                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
#if SDL_VERSION_ATLEAST(2, 0, 18)
                            double dx = -event.wheel.preciseX * direction;
                            double dy = -event.wheel.preciseY * direction;
#else
                            double dx = -event.wheel.x * direction, dy = -event.wheel.y * direction;
#endif
                            if (SDL_GetModState() & KMOD_SHIFT) {
                                dx = dy;
                                dy = 0;
                            }
                            simulator.scroll(x, y, dx, dy);
                        }
                        break;
                    case SDL_FINGERDOWN:
                    case SDL_FINGERMOTION:
                    case SDL_FINGERUP: {
                        // Only a touch screen: a touchpad's fingers move the mouse.
                        if (SDL_GetTouchDeviceType(event.tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT) {
                            break;
                        }
                        int width = 1, height = 1;
                        SDL_GetWindowSize(window.get(), &width, &height);
                        const auto [x, y] = canvas_point(double(event.tfinger.x) * width,
                                                         double(event.tfinger.y) * height);
                        if (event.type == SDL_FINGERDOWN) {
                            const auto pointer = finger_pointer(event.tfinger.fingerId);
                            note("finger down ", pointer, ' ', std::lround(x), ',', std::lround(y));
                            simulator.pointer_down(pointer, x, y, at);
                        } else if (const auto found = fingers.find(event.tfinger.fingerId);
                                   found != fingers.end()) {
                            if (event.type == SDL_FINGERMOTION) {
                                motion[found->second] = {x, y};
                            } else {
                                note("finger up ", found->second, ' ', std::lround(x), ',',
                                     std::lround(y));
                                simulator.pointer_up(found->second, x, y);
                                fingers.erase(found);
                            }
                        }
                        break;
                    }
                    case SDL_WINDOWEVENT:
                        note("window event ", static_cast<int>(event.window.event));
                        if (event.window.event == SDL_WINDOWEVENT_LEAVE && !mouse_pressed) {
                            simulator.pointer_exit(sim::Simulator::mouse);
                        } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                            // A release may never come: end the presses as a lost
                            // laser does, the mouse's only if it doesn't come back.
                            // The held second pointer stays, as shown.
                            if (mouse_pressed) {
                                focus_lost_held = at;
                            }
                            for (const auto& [id, pointer] : fingers) {
                                simulator.pointer_cancel(pointer);
                            }
                            fingers.clear();
                        } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                                   event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                            present = true;
                        }
                        break;
                    default:
                        break; // Keys and text from the physical keyboard do nothing.
                    }
                    watch_reopen(event.type == SDL_MOUSEBUTTONDOWN ? "down"
                                 : event.type == SDL_MOUSEBUTTONUP ? "up"
                                 : event.type == SDL_WINDOWEVENT   ? "a window event"
                                                                   : "another event");
                } while (SDL_PollEvent(&event));
                flush_motion();
            }
            if (focus_lost_held && fk::monotonic_seconds() - *focus_lost_held > .5) {
                focus_lost_held.reset();
                note("press cancelled after focus loss");
                if (std::exchange(mouse_pressed, false)) {
                    simulator.pointer_cancel(sim::Simulator::mouse);
                }
            }
            const bool ticked = simulator.tick(fk::monotonic_seconds());
            watch_reopen("a tick");
            if (ticked) {
                auto* canvas = simulator.canvas();
                require(SDL_UpdateTexture(texture.get(), nullptr, cairo_image_surface_get_data(canvas),
                                          cairo_image_surface_get_stride(canvas)) == 0);
                present = true;
            }
            if (present) {
                int width = 1, height = 1;
                SDL_GetRendererOutputSize(renderer.get(), &width, &height);
                const auto p = place(width, height);
                const SDL_Rect target{static_cast<int>(std::lround(p.x)),
                                      static_cast<int>(std::lround(p.y)),
                                      static_cast<int>(std::lround(sim::Simulator::width * p.scale)),
                                      static_cast<int>(std::lround(sim::Simulator::height * p.scale))};
                SDL_SetRenderDrawColor(renderer.get(), 24, 25, 27, 255);
                SDL_RenderClear(renderer.get());
                SDL_RenderCopy(renderer.get(), texture.get(), nullptr, &target);
                SDL_RenderPresent(renderer.get());
                present = false;
            }
        }
        simulator.shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Full Keyboard simulator: " << error.what() << '\n';
        return 1;
    }
}
