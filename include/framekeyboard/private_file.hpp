#pragma once
#include <filesystem>
#include <string>

namespace framekeyboard {
// A record of what the user typed, as learned words: written whole beside the
// file, then put in its place, the owner's alone from its creation; a failed
// write leaves nothing behind, and one left by a crash goes first. A missing
// folder is made, the owner's alone, when `make_folder` is set. No fsync:
// saves run on the VR thread, where a sync could stall a frame.
bool write_private_file(const std::filesystem::path& file, const std::string& text,
                        bool make_folder = false);
// Removes it, and what a save cut short left beside it.
void remove_private_file(const std::filesystem::path& file);
} // namespace framekeyboard
