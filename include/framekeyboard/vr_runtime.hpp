#pragma once
#include <filesystem>

namespace framekeyboard {
bool steamvr_running(const std::filesystem::path& proc = "/proc");
void require_steamvr();
} // namespace framekeyboard
