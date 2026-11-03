#include "framekeyboard/config.hpp"
#include "framekeyboard/japanese.hpp"
#include "platform.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>

namespace framekeyboard::simulator {
namespace {
// An XDG base directory, or its default under HOME.
fs::path xdg(const char* variable, const char* fallback) {
    if (const char* base = std::getenv(variable); base && *base && fs::path(base).is_absolute()) {
        return base;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / fallback;
    }
    throw std::runtime_error("HOME is unset; pass --config-dir");
}
} // namespace
fs::path config_dir() {
    return xdg("XDG_CONFIG_HOME", ".config") / "framekeyboard-simulator";
}
fs::path models_dir() {
    return xdg("XDG_DATA_HOME", ".local/share") / "framekeyboard-simulator/models";
}
fs::path keyboard_config_dir() {
    return default_config_dir();
}
std::string use_anthy_dictionary(const fs::path& dictionary) {
    if (!fs::is_regular_file(dictionary)) {
        return "no Anthy dictionary at " + dictionary.string();
    }
    // The same library the keyboard opens by name for its first conversion
    // (src/japanese.cpp); the override must come before anthy_init().
    void* anthy = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!anthy) {
        return "libanthy.so.0 not found; install Anthy or add its library folder to LD_LIBRARY_PATH";
    }
    auto* set =
        reinterpret_cast<void (*)(const char*, const char*)>(dlsym(anthy, "anthy_conf_override"));
    if (!set) {
        dlclose(anthy);
        return "this Anthy cannot take another dictionary (no anthy_conf_override)";
    }
    dlclose(anthy);
    // The keyboard gives it to Anthy at each start, restarts for learning included.
    set_anthy_dictionary(fs::absolute(dictionary));
    return {};
}
} // namespace framekeyboard::simulator
