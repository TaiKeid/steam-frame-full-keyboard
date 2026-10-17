#include "framekeyboard/startup.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char** argv) {
    try {
        char pattern[] = "/tmp/full-keyboard-startup-XXXXXX";
        const fs::path base = argc == 2 ? fs::path(argv[1]) : fs::path(mkdtemp(pattern));
        const auto root = base / "install";
        const auto binary = root / "releases/test/bin/framekeyboard";
        const auto units = base / "units";
        const auto unit = units / "framekeyboard-autostart.service";
        const auto link = units / "gamescope-session.target.wants/framekeyboard-autostart.service";
        const auto config = base / "config";
        fs::create_directories(binary.parent_path());
        std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
        fs::permissions(binary, fs::perms::owner_all);
        fs::create_symlink("releases/test", root / "current");
        int reloads = 0;
        auto reload = [&] { ++reloads; };
        Settings settings;
        configure_startup(settings, binary, config, units, reload);
        require(reloads == 0 && !fs::exists(unit), "defaults cannot install a startup unit");
        settings.start_with_steamos = true;
        configure_startup(settings, binary, config, units, reload);
        require(reloads == 0 && !fs::exists(unit), "startup cannot run without replacement");
        settings.replace_native_keyboard = true;
        configure_startup(settings, binary, config, units, reload);
        const auto contents = read(unit);
        require(fs::is_symlink(link) && fs::canonical(link) == unit && reloads == 1,
                "explicit opt-in installs the user-session startup link");
        require(contents.find("--autostart") != std::string::npos &&
                    contents.find("After=gamescope-session.service steamvr.service") !=
                        std::string::npos &&
                    contents.find("KillMode=process") != std::string::npos,
                "boot is hidden, waits for VR and preserves independent recovery/update helpers");
        configure_startup(settings, binary, config, units, reload);
        require(reloads == 1, "ordinary preference saves do not reload systemd repeatedly");
        if (argc == 2) {
            std::cout << unit << '\n';
            return 0;
        }
        settings.replace_native_keyboard = false;
        bool failed = false;
        try {
            configure_startup(settings, binary, config, units,
                              [] { throw std::runtime_error("injected reload error"); });
        } catch (...) {
            failed = true;
        }
        require(failed && read(unit) == contents && fs::is_symlink(link),
                "failed changes restore the old unit and link");
        configure_startup(settings, binary, config, units, reload);
        require(!fs::exists(unit) && !fs::is_symlink(link) && reloads == 2,
                "replacement off removes boot startup without stopping the keyboard");
        settings.replace_native_keyboard = true;
        configure_startup(settings, binary, config, units, reload);
        require(fs::is_symlink(link), "replacement on restores an opted-in boot startup");
        settings.start_with_steamos = false;
        configure_startup(settings, binary, config, units, reload);
        require(!fs::exists(unit) && !fs::is_symlink(link), "explicit opt-out removes startup");
        std::ofstream(unit) << "# custom unit\n";
        failed = false;
        settings.start_with_steamos = true;
        try {
            configure_startup(settings, binary, config, units, reload);
        } catch (...) {
            failed = true;
        }
        require(failed && read(unit) == "# custom unit\n", "custom startup units are never overwritten");
        fs::remove_all(base);
        std::cout
            << "Startup opt-in, replacement dependency, rollback and custom-unit preservation passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
