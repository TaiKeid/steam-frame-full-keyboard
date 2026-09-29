#include "framekeyboard/app.hpp"
#include "framekeyboard/ei_input.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void help() {
    std::cout << "Full Keyboard for Steam Frame " << FRAMEKEYBOARD_VERSION << "\n\n"
              << "  --preview                 Native desktop preview, no system input\n"
              << "  --vr                      Manually launched native VR panel\n"
              << "  --render FILE.png         Render the keyboard without a window\n"
              << "  --render-settings FILE    Render the profile selector\n"
              << "  --check                   Validate and list available profiles\n"
              << "  --describe-layout         Describe the active key geometry\n"
              << "  --input ei|uinput         Use compositor input or a virtual kernel device\n"
              << "  --ei-socket PATH          Override the Gamescope input socket\n"
              << "  --text-socket PATH        Override its Japanese Wayland text socket\n"
              << "  --start-enabled           Enable typing at startup with an explicit backend\n"
              << "  --target-language ID      Physical uinput/external-JIS target keymap\n"
              << "  --config-dir PATH         User profiles/settings directory\n"
              << "  --data-dir PATH           Extra bundled profile directory\n"
              << "  --duration SECONDS        Exit a preview/VR smoke test after this time\n\n"
              << "Typing stays disabled unless --start-enabled is supplied.\n"
              << "Separate dashboard keyboard. Stock keyboard and autostart are unchanged.\n";
}
void signal_handler(int) {
    fk::interrupted = 1;
}
} // namespace
int main(int argc, char** argv) {
    try {
        fk::Options options;
        options.config_dir = fk::default_config_dir();
        bool mode_set = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) {
                    throw std::runtime_error("missing value for " + argument);
                }
                return argv[i];
            };
            auto mode = [&](const std::string& name) {
                if (mode_set) {
                    throw std::runtime_error("choose one operation");
                }
                options.mode = name;
                mode_set = true;
            };
            if (argument == "--help") {
                help();
                return 0;
            } else if (argument == "--version") {
                std::cout << "framekeyboard " << FRAMEKEYBOARD_VERSION << '\n';
                return 0;
            } else if (argument == "--preview") {
                mode("preview");
            } else if (argument == "--vr") {
                mode("vr");
            } else if (argument == "--render" || argument == "--render-settings") {
                mode(argument.substr(2));
                options.output = value();
            } else if (argument == "--check") {
                mode("check");
            } else if (argument == "--describe-layout") {
                mode("describe");
            } else if (argument == "--config-dir") {
                options.config_dir = value();
            } else if (argument == "--data-dir") {
                options.data_dir = value();
            } else if (argument == "--ei-socket") {
                options.ei_socket = value();
            } else if (argument == "--text-socket") {
                options.text_socket = value();
                if (options.text_socket.empty()) {
                    throw std::runtime_error("--text-socket requires a nonempty path");
                }
            } else if (argument == "--input") {
                options.input = value();
            } else if (argument == "--start-enabled") {
                options.start_enabled = true;
            } else if (argument == "--target-language") {
                options.target_language = value();
            } else if (argument == "--duration") {
                const auto seconds = value();
                std::size_t end = 0;
                options.duration = std::stod(seconds, &end);
                if (end != seconds.size() || !std::isfinite(options.duration) || options.duration <= 0) {
                    throw std::runtime_error("duration must be a positive finite number");
                }
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        if (options.mode == "help") {
            help();
            return 0;
        }
        if (options.input != "none" && options.input != "uinput" && options.input != "ei") {
            throw std::runtime_error("input must be none, ei or uinput");
        }
        if (options.input != "none" &&
            (options.mode != "vr" || (options.input == "uinput" && options.target_language.empty()))) {
            throw std::runtime_error("typing requires --vr; uinput also requires --target-language; "
                                     "desktop preview never injects input");
        }
        if (!options.text_socket.empty() && options.input != "ei") {
            throw std::runtime_error("--text-socket requires --input ei");
        }
        if (options.start_enabled && options.input == "none") {
            throw std::runtime_error("--start-enabled requires an input backend");
        }
        std::unique_ptr<fk::VrInstance> instance;
        if (options.mode == "vr") {
            instance = std::make_unique<fk::VrInstance>();
            if (!instance->is_owner()) {
                std::cout << "Recenter requested from the running keyboard.\n";
                return 0;
            }
        }
        auto profiles = fk::load_profiles(options.data_dir, options.config_dir);
        if (options.mode == "check" || options.mode == "describe") {
            const auto settings = fk::load_settings(options.config_dir, profiles.errors);
            fk::validate_selection(profiles, settings.active);
            if (options.mode == "describe") {
                const auto& layout = profiles.layouts.at(settings.active.layout);
                std::cout << layout.name << '\n'
                          << layout.keys.size() << " keys; " << layout.width << " x " << layout.height
                          << " design area\n";
            } else {
                for (const auto& [id, p] : profiles.layouts) {
                    std::cout << "layout " << id << ": " << p.name << '\n';
                }
                for (const auto& [id, p] : profiles.languages) {
                    std::cout << "language " << id << ": " << p.name << '\n';
                }
                for (const auto& [id, p] : profiles.themes) {
                    std::cout << "theme " << id << ": " << p.name << '\n';
                }
            }
            for (const auto& error : profiles.errors) {
                std::cerr << error << '\n';
            }
            return profiles.errors.empty() ? 0 : 1;
        }
        std::unique_ptr<fk::KeySink> sink;
        if (options.input != "none" && !options.target_language.empty() &&
            !profiles.languages.contains(options.target_language)) {
            throw std::runtime_error("unknown target-language profile");
        }
        if (options.input == "ei") {
            if (options.ei_socket.empty()) {
                options.ei_socket =
                    std::string("/run/user/") + std::to_string(getuid()) + "/gamescope-0-ei";
            }
            sink = std::make_unique<fk::EiSink>(options.ei_socket, options.text_socket);
        } else if (options.input == "uinput") {
            sink = std::make_unique<fk::UInputSink>();
        } else {
            sink = std::make_unique<fk::NullSink>();
        }
        fk::App app(options, *sink);
        if (options.mode == "render" || options.mode == "render-settings") {
            if (options.mode == "render-settings") {
                app.show_settings();
            }
            app.paint(fk::monotonic_seconds());
            app.renderer.write_png(options.output);
            return 0;
        }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        return options.mode == "vr" ? fk::run_vr(app, *instance, options.duration)
                                    : fk::run_preview(app, options.duration);
    } catch (const std::exception& error) {
        std::cerr << "Full Keyboard: " << error.what() << '\n';
        return 1;
    }
}
