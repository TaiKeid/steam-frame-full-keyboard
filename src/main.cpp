#include "framekeyboard/app.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace fk = framekeyboard;
namespace {
void help() {
    std::cout << "FrameKeyboard " << FRAMEKEYBOARD_VERSION << "\n\n"
              << "  --preview                 Native desktop preview, no system input\n"
              << "  --vr                      Manually launched native VR panel\n"
              << "  --render FILE.png         Render the keyboard without a window\n"
              << "  --render-settings FILE    Render the profile selector\n"
              << "  --render-placement FILE   Render the move/align controls\n"
              << "  --check                   Validate and list available profiles\n"
              << "  --describe-layout         Describe the active key geometry\n"
              << "  --input uinput            Create a virtual device in VR mode\n"
              << "  --target-language ID      Confirm the target session's matching keymap\n"
              << "  --config-dir PATH         User profiles/settings directory\n"
              << "  --data-dir PATH           Extra bundled profile directory\n"
              << "  --duration SECONDS        Exit a preview/VR smoke test after this time\n\n"
              << "Input starts OFF. Use the panel's Input off button to enable typing.\n"
              << "Stock keyboard takeover and autostart are not enabled in this version.\n";
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
            } else if (argument == "--render" || argument == "--render-settings" ||
                       argument == "--render-placement") {
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
            } else if (argument == "--input") {
                options.input = value();
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
        if (options.input != "none" && options.input != "uinput") {
            throw std::runtime_error("input must be none or uinput");
        }
        if (options.input == "uinput" && (options.mode != "vr" || options.target_language.empty())) {
            throw std::runtime_error(
                "uinput requires --vr and --target-language; desktop preview never injects input");
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
        if (options.input == "uinput") {
            if (!profiles.languages.contains(options.target_language)) {
                throw std::runtime_error("unknown target-language profile");
            }
            sink = std::make_unique<fk::UInputSink>();
        } else {
            sink = std::make_unique<fk::NullSink>();
        }
        fk::App app(options, *sink);
        if (options.mode == "render" || options.mode == "render-settings" ||
            options.mode == "render-placement") {
            if (options.mode == "render-settings") {
                app.show_settings();
            }
            if (options.mode == "render-placement") {
                app.show_placement();
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
        std::cerr << "FrameKeyboard: " << error.what() << '\n';
        return 1;
    }
}
