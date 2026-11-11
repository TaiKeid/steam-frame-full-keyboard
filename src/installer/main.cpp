// Full Keyboard's installer: a window with the installer's screens. Without
// --simulate it installs for real; with it nothing is downloaded, written or
// started, for looking at the screens and their flow on any Linux PC.
#include "backend.hpp"
#include "framekeyboard/config.hpp"
#include "framekeyboard/localization.hpp"
#include "installer.hpp"
#include <SDL.h>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace fk = framekeyboard;
namespace in = framekeyboard::installer;

namespace {

void usage() {
    std::cout
        << "Full Keyboard installer " << FRAMEKEYBOARD_VERSION << "\n\n"
        << "  --simulate                Download, install and change nothing; tasks only take time\n"
        << "  --installed VERSION       With --simulate: pretend this version is installed\n"
        << "  --fail TASK               With --simulate: fail releases, download, install,\n"
        << "                            uninstall or finish\n"
        << "  --speed FACTOR            With --simulate: run tasks this many times faster\n"
        << "  --releases LIST           With --simulate: versions to offer, newest first,\n"
        << "                            comma-separated (default: this build's)\n"
        << "  --language TAG            Interface language instead of the system's\n"
        << "  --tour DIR                With --simulate: save each screen to DIR as PNG and exit\n"
        << "  --helper FILE             update_helper.py to use\n";
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// update_helper.py beside the program: usr/share/framekeyboard/update in the
// AppImage, and the build tree's copy in update/ for a development build.
fk::fs::path default_helper() {
    const auto self = fk::fs::canonical("/proc/self/exe").parent_path();
    for (const auto& candidate : {self.parent_path() / "share/framekeyboard/update/update_helper.py",
                                  self / "update/update_helper.py"}) {
        if (fk::fs::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return self.parent_path() / "share/framekeyboard/update/update_helper.py";
}

// Clicks through every screen with the simulated backend and saves each.
int tour(in::Installer& installer, const fk::fs::path& directory) {
    fk::fs::create_directories(directory);
    int number = 0;
    const auto save = [&](const std::string& name) {
        installer.tick(now_seconds());
        ++number;
        const auto path = directory / (std::to_string(number / 10) + std::to_string(number % 10) + "-" +
                                       name + ".png");
        cairo_surface_write_to_png(installer.canvas(), path.c_str());
        std::cout << path.string() << '\n';
    };
    const auto wait = [&](double seconds) {
        for (const double end = now_seconds() + seconds; now_seconds() < end;) {
            installer.tick(now_seconds());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    const auto click = [&](in::Installer::Action action, int index = 0) {
        installer.tick(now_seconds());
        const auto r = installer.control(action, index);
        if (!r) {
            throw std::runtime_error("tour: control not shown");
        }
        installer.pointer_down(r->x + r->width / 2, r->y + r->height / 2);
        installer.pointer_up(r->x + r->width / 2, r->y + r->height / 2);
        installer.pointer_move(-1, -1);
    };
    // The laser on a control, and holding it down, without letting go on it.
    const auto point = [&](in::Installer::Action action, const std::string& name, int index = 0) {
        installer.tick(now_seconds());
        const auto r = installer.control(action, index);
        if (!r) {
            throw std::runtime_error("tour: control not shown");
        }
        installer.pointer_move(r->x + r->width / 2, r->y + r->height / 2);
        save(name + "-pointed");
        installer.pointer_down(r->x + r->width / 2, r->y + r->height / 2);
        save(name + "-held");
        installer.pointer_leave();
    };
    using A = in::Installer::Action;
    // Waits for a control to take presses, for as long as a task may run.
    const auto wait_for = [&](in::Installer::Action action, double limit) {
        for (const double end = now_seconds() + limit;
             now_seconds() < end && !installer.enabled(action);) {
            wait(.05);
        }
    };
    wait(.3);
    save("loading");
    wait_for(A::Install, 5);
    save("home");
    if (!installer.enabled(A::Versions)) {
        return 0; // The list failed: Home offers only Try again.
    }
    point(A::Install, "home");
    click(A::Versions);
    installer.tick(now_seconds());
    if (const auto row = installer.control(A::Version, 1)) {
        installer.pointer_move(row->x + row->width / 2, row->y + row->height / 2);
    }
    save("versions");
    installer.pointer_leave();
    click(A::Versions);
    if (installer.control(A::Uninstall)) {
        click(A::Uninstall);
        wait(.4);
        save("uninstall-confirm");
        click(A::CancelUninstall);
    }
    click(A::Install);
    wait(.15);
    save("sliding");
    wait(.85);
    if (installer.control(A::Choice)) {
        // Each page's picture at a telling moment, in seconds after its turn.
        const std::vector<std::vector<std::pair<double, const char*>>> moments{
            {{2.4, "page-1-replace"}},
            {{1.3, "page-2-swiping"}, {2.2, "page-2-word"}},
            {{2.1, "page-3-suggestions"}},
            {{1.1, "page-4-pulling"}, {1.7, "page-4-pulled"}, {2.3, "page-4-typed"}},
            {{1.0, "page-5-numpad"}},
        };
        for (int page = 0; page < in::Installer::pages; ++page) {
            double waited = page == 0 ? 1.0 : 0; // The first page came with the slide.
            for (const auto& [at, name] : moments[static_cast<std::size_t>(page)]) {
                wait(std::max(0.0, at - waited));
                waited = at;
                save(name);
            }
            if (page == 0) {
                point(A::Choice, "page-1-no", 1);
                point(A::Next, "page-1-next");
            }
            if (page == 1) {
                click(A::Choice, 1);
                wait(.6);
                save("page-2-no"); // The picture plays on, dimmed.
                click(A::Choice, 0);
            }
            if (page == in::Installer::pages - 1) {
                click(A::Choice, 1);
                wait(.7);
                save("page-5-no-numpad");
                click(A::Choice, 0);
            }
            click(A::Next);
            if (page == 0) {
                wait(.2);
                save("page-turning");
            }
        }
        wait(.6);
    }
    save("finish-installing");
    wait_for(A::Finish, 30);
    wait(.8);
    save("finish-ready");
    if (!installer.enabled(A::Finish)) {
        return 0; // The install failed.
    }
    click(A::Finish);
    wait(.4);
    if (!installer.quitting()) {
        save("finishing");
        for (const double end = now_seconds() + 6; now_seconds() < end && !installer.quitting();) {
            wait(.05);
        }
        if (!installer.quitting()) {
            save("finish-failed");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool simulate = false;
        in::SimulatedOptions simulated;
        std::string language;
        fk::fs::path tour_dir, helper;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(argument + " needs a value");
                }
                return argv[++i];
            };
            if (argument == "--help") {
                usage();
                return 0;
            } else if (argument == "--simulate") {
                simulate = true;
            } else if (argument == "--installed") {
                simulated.installed = value();
            } else if (argument == "--fail") {
                simulated.fail = value();
            } else if (argument == "--releases") {
                std::string list = value();
                for (std::size_t start = 0; start <= list.size();) {
                    const auto end = std::min(list.find(',', start), list.size());
                    if (end > start) {
                        simulated.releases.push_back(list.substr(start, end - start));
                    }
                    start = end + 1;
                }
            } else if (argument == "--speed") {
                simulated.speed = std::stod(value());
            } else if (argument == "--language") {
                language = value();
            } else if (argument == "--tour") {
                tour_dir = value();
            } else if (argument == "--helper") {
                helper = value();
            } else {
                usage();
                return 1;
            }
        }
        if (!tour_dir.empty() && !simulate) {
            throw std::runtime_error("--tour needs --simulate");
        }
        if (!tour_dir.empty()) {
            // Slow enough for the wizard's pictures to play while it installs.
            simulated.speed *= .75;
        }
        const char* home = std::getenv("HOME");
        fk::Localization localization;
        localization.load({}, {});
        localization.choose(fk::system_language(home ? home : ""), language);
        const auto theme = fk::load_profiles({}, {}).themes.at("graphite");
        auto backend = simulate ? in::simulated_backend(simulated)
                                : in::system_backend(helper.empty() ? default_helper() : helper);
        in::Installer installer(*backend, theme, localization);
        if (!tour_dir.empty()) {
            return tour(installer, tour_dir);
        }

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            throw std::runtime_error(SDL_GetError());
        }
        struct QuitSDL {
            ~QuitSDL() { SDL_Quit(); }
        } quit_sdl;
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow(localization.text("installer.window-title").c_str(), SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, in::Installer::width, in::Installer::height,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI),
            SDL_DestroyWindow);
        if (!window) {
            throw std::runtime_error(SDL_GetError());
        }
        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
            SDL_CreateRenderer(window.get(), -1, 0), SDL_DestroyRenderer);
        if (!renderer) {
            renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        }
        if (!renderer) {
            throw std::runtime_error(SDL_GetError());
        }
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
            SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              in::Installer::width, in::Installer::height),
            SDL_DestroyTexture);
        // The canvas keeps its shape inside any window size, letterboxed.
        SDL_RenderSetLogicalSize(renderer.get(), in::Installer::width, in::Installer::height);
        const auto canvas_point = [&](int x, int y) {
            float cx = 0, cy = 0;
            SDL_RenderWindowToLogical(renderer.get(), x, y, &cx, &cy);
            return std::pair<double, double>(cx, cy);
        };
        // Presented again only when the canvas changed or the window needs it.
        bool present = true;
        while (!installer.quitting()) {
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, 16)) {
                do {
                    switch (event.type) {
                    case SDL_QUIT:
                        // The window's own close waits, like ×, while work runs.
                        if (!installer.busy()) {
                            return 0;
                        }
                        break;
                    case SDL_MOUSEMOTION: {
                        const auto [x, y] = canvas_point(event.motion.x, event.motion.y);
                        installer.pointer_move(x, y);
                        break;
                    }
                    case SDL_MOUSEBUTTONDOWN:
                    case SDL_MOUSEBUTTONUP:
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            const auto [x, y] = canvas_point(event.button.x, event.button.y);
                            if (event.type == SDL_MOUSEBUTTONDOWN) {
                                installer.pointer_down(x, y);
                            } else {
                                installer.pointer_up(x, y);
                            }
                        }
                        break;
                    case SDL_WINDOWEVENT:
                        if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                            installer.pointer_leave();
                        } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                                   event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                            present = true;
                        }
                        break;
                    }
                } while (SDL_PollEvent(&event));
            }
            if (installer.tick(now_seconds())) {
                auto* canvas = installer.canvas();
                SDL_UpdateTexture(texture.get(), nullptr, cairo_image_surface_get_data(canvas),
                                  cairo_image_surface_get_stride(canvas));
                present = true;
            }
            if (present) {
                SDL_RenderClear(renderer.get());
                SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr);
                SDL_RenderPresent(renderer.get());
                present = false;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "framekeyboard-installer: " << error.what() << '\n';
        return 1;
    }
}
