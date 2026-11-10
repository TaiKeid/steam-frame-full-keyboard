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
    using A = in::Installer::Action;
    save("loading");
    wait(.4);
    save("home");
    click(A::Versions);
    save("versions");
    click(A::Versions);
    if (installer.control(A::Uninstall)) {
        click(A::Uninstall);
        save("uninstall-confirm");
        click(A::CancelUninstall);
    }
    click(A::Install);
    wait(.15);
    save("sliding");
    wait(.8);
    if (installer.control(A::Choice)) {
        for (int page = 0; page < in::Installer::pages; ++page) {
            save("page-" + std::to_string(page + 1));
            if (page == 1) {
                wait(1.2); // the swipe demonstration, mid-word
                save("page-2-swiping");
                wait(.9);
                save("page-2-word");
            }
            click(A::Next);
        }
    }
    save("finish-installing");
    wait(2.5);
    save("finish-ready");
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
            simulated.speed *= 4;
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
                        }
                        break;
                    }
                } while (SDL_PollEvent(&event));
            }
            installer.tick(now_seconds());
            auto* canvas = installer.canvas();
            SDL_UpdateTexture(texture.get(), nullptr, cairo_image_surface_get_data(canvas),
                              cairo_image_surface_get_stride(canvas));
            SDL_RenderClear(renderer.get());
            SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr);
            SDL_RenderPresent(renderer.get());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "framekeyboard-installer: " << error.what() << '\n';
        return 1;
    }
}
