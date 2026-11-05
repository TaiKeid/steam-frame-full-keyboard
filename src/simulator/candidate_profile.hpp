#pragma once

#include "framekeyboard/mozc.hpp"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace framekeyboard::simulator {
// Candidates must not read the interactive simulator's learned history.
class CandidateProfile {
  public:
    CandidateProfile() {
        const auto pattern =
            (std::filesystem::temp_directory_path() / "framekeyboard-candidates-XXXXXX").string();
        std::vector<char> directory(pattern.begin(), pattern.end());
        directory.push_back('\0');
        if (!mkdtemp(directory.data())) {
            throw std::runtime_error("cannot create a temporary candidates profile");
        }
        path_ = directory.data();
    }
    CandidateProfile(const CandidateProfile&) = delete;
    CandidateProfile& operator=(const CandidateProfile&) = delete;
    ~CandidateProfile() {
        // Stop the writer before removing its files, including when reporting throws.
        stop_mozc();
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};
} // namespace framekeyboard::simulator
