#pragma once
// Helpers shared by the libime and McBopomofo engine modules.
#include <cerrno>
#include <cstddef>
#include <dlfcn.h>
#include <sys/stat.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace framekeyboard {
// The engines' libraries and data sit in chinese/ beside the module:
// engines/chinese in a build tree, lib/framekeyboard/chinese in a package
// (tools/build-chinese.sh makes them). The engines were built with no path of
// their own compiled in, so every file is named from here.
inline std::filesystem::path engine_data_dir() {
    static const auto dir = [] {
        Dl_info info{};
        // Any symbol of this module tells where it was loaded from.
        dladdr(reinterpret_cast<void*>(&engine_data_dir), &info);
        return std::filesystem::path(info.dli_fname ? info.dli_fname : "").parent_path() / "chinese";
    }();
    return dir;
}
// Makes the engine's learning folder and the shared chinese/ one above it,
// each the owner's alone; the config folder itself already exists.
inline bool make_learning_dir(const std::filesystem::path& dir) {
    for (const auto& folder : {dir.parent_path(), dir}) {
        if (mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}
// Characters in UTF-8 text; the engines count positions in characters.
inline std::size_t utf8_length(std::string_view text) {
    std::size_t count = 0;
    for (const char byte : text) {
        count += (static_cast<unsigned char>(byte) & 0xC0) != 0x80;
    }
    return count;
}
} // namespace framekeyboard
