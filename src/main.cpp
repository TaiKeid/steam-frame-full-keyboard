#include "default_layout.hpp"

#include <iostream>
#include <string_view>

namespace {
void print_help() {
    std::cout << "FrameKeyboard " << FRAMEKEYBOARD_VERSION << "\n"
              << "Native Steam Frame keyboard project bootstrap.\n\n"
              << "Usage: framekeyboard [--help | --version | --describe-layout]\n\n"
              << "This build inspects the compiled layout. Rendering, VR integration,\n"
              << "and native input delivery are not implemented yet.\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "framekeyboard " << FRAMEKEYBOARD_VERSION << '\n';
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--describe-layout") {
        namespace layout = framekeyboard::default_layout;
        std::size_t shortcuts = 0;
        for (const auto& key : layout::keys) {
            if (key.action_kind == framekeyboard::ActionKind::Shortcut) ++shortcuts;
        }
        std::cout << layout::name << '\n'
                  << layout::keys.size() - shortcuts << " keyboard keys + "
                  << shortcuts << " shortcut keys\n"
                  << "Design area: " << layout::width << " x " << layout::height << '\n';
        return 0;
    }
    std::cerr << "Unknown arguments. Run framekeyboard --help.\n";
    return 2;
}
