#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace framekeyboard {
// Japanese phrases committed straight after one another, which the candidate
// row offers again after the first is committed (docs/japanese.md,
// Prediction). Kept as plain text in one file, like Anthy's own learning, and
// cleared with it. An empty path keeps nothing.
class NextWords {
  public:
    explicit NextWords(std::filesystem::path file = {}) : file_(std::move(file)) {}
    void record(const std::string& previous, const std::string& next);
    // The phrases that followed `previous`, most often first, then most recent.
    std::vector<std::string> after(const std::string& previous, std::size_t most) const;
    // The phrase recorded last after `previous`; empty without one.
    std::string latest(const std::string& previous) const;
    bool empty() const;
    // Forgets them all and removes the file.
    void clear();
    // Whether a change waits to be written, and since when (seconds, as given).
    bool dirty() const { return dirty_; }
    double changed_at() const { return changed_at_; }
    void touch(double now) { changed_at_ = now; }
    // Writes the changes, replacing the file whole; false if it could not.
    bool save();

    static constexpr std::size_t most_pairs = 2000;
    static constexpr std::size_t longest = 32; // Characters in either phrase.

  private:
    struct Pair {
        std::string previous, next;
        std::uint32_t count{};
        std::uint64_t stamp{}; // When last recorded, in records.
    };
    void load() const;
    std::filesystem::path file_;
    mutable bool loaded_{};
    mutable std::vector<Pair> pairs_;
    mutable std::uint64_t clock_{};
    bool dirty_{};
    double changed_at_{};
};
} // namespace framekeyboard
