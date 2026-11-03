#pragma once

#include <filesystem>
#include <string>

// What the simulator needs from the operating system, apart from its window
// (SDL, main.cpp). Linux in platform_linux.cpp; a Windows port adds its own.
namespace framekeyboard::simulator {
// The simulator's own settings and profiles, never an installed keyboard's
// (~/.config/framekeyboard).
std::filesystem::path config_dir();
// Where it keeps downloaded dictation models, apart from an installed keyboard's.
std::filesystem::path models_dir();
// The installed keyboard's settings folder, which the simulator refuses to use.
std::filesystem::path keyboard_config_dir();
// Points Anthy at a dictionary file before the keyboard's first conversion,
// for an Anthy that is not installed where it was built for. Empty when it
// worked, else why not. The library stays loaded until the process exits.
std::string use_anthy_dictionary(const std::filesystem::path& dictionary);
} // namespace framekeyboard::simulator
