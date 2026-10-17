#pragma once
#include "config.hpp"
#include <functional>

namespace framekeyboard {
// Runs on the preference writer, never the VR loop. Tests supply a private unit
// directory and reload callback; previews never configure session startup.
// Persist runs after configuration succeeds; its failure restores the old unit.
void configure_startup(const Settings& settings, const fs::path& executable, const fs::path& config_dir,
                       fs::path unit_directory = {}, std::function<void()> reload = {},
                       std::function<void()> persist = {});
} // namespace framekeyboard
