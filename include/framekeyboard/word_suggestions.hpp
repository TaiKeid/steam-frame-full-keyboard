#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace framekeyboard {
// The key words are matched by: lower-cased, so Hel finds hello and HELLO.
std::string word_key(std::string_view word);
// Whether a character belongs to a word being typed: letters, combining
// marks and an apostrophe inside it (don't, l'équipe).
bool word_character(std::uint32_t character);

// A language's word list for typing suggestions, the one its swipe typing
// uses (word<TAB>log-frequency 0-255; # starts a comment). Indexed by key,
// each key once with its commonest spelling. Read-only once loaded, so the
// loading thread can hand it over whole.
class WordList {
  public:
    // `respell` replaces parts of each word, as Swiss German writes ss for ß.
    using Respelling = std::map<std::string, std::string>;
    // `offensive` lists the words completions hold back, one per line
    // (swipe/lexicons/README.md, Offensive words).
    static WordList parse(std::istream& input, const Respelling& respell = {},
                          std::istream* offensive = nullptr);
    static WordList load(const std::filesystem::path& path, const Respelling& respell = {},
                         const std::filesystem::path& offensive = {});
    std::size_t size() const { return entries_.size(); }
    struct Word {
        std::string_view key, spelling;
        int frequency{};
        bool offensive{};
    };
    // The words whose key begins with `prefix` (a key), commonest first and
    // offensive ones last.
    std::vector<Word> starting(std::string_view prefix, std::size_t most) const;
    // The word with this key, if listed.
    std::optional<Word> find(std::string_view key) const;

  private:
    // Keys and spellings live in one buffer; most spellings are their key.
    struct Entry {
        std::uint32_t key{}, spelling{};
        std::uint16_t key_length{}, spelling_length{};
        std::uint8_t frequency{};
        bool offensive{};
    };
    Word word(const Entry& entry) const;
    std::string text_;
    std::vector<Entry> entries_; // By key.
};

// The words the user finished in one language, how often and how lately,
// kept as plain text in the config folder, the owner's alone, at most
// most_words, the least recent going first. An empty path keeps nothing.
class LearnedWords {
  public:
    explicit LearnedWords(std::filesystem::path file = {}) : file_(std::move(file)) {}
    void record(std::string_view word);
    std::uint32_t uses(std::string_view key) const;
    struct Use {
        std::string key, spelling;
        std::uint32_t uses{};
        std::uint64_t stamp{};
    };
    // Learned words whose key begins with `prefix`.
    std::vector<const Use*> starting(std::string_view prefix) const;
    bool empty() const;
    std::size_t size() const;
    // Forgets them all and removes the file.
    void clear();
    bool dirty() const { return dirty_; }
    double changed_at() const { return changed_at_; }
    void touch(double now) { changed_at_ = now; }
    // Writes the changes, replacing the file whole; false if it could not.
    bool save();

    static constexpr std::size_t most_words = 5000;
    static constexpr std::size_t longest = 40; // Characters in a word.
    // A word the list lacks is suggested once finished this often.
    static constexpr std::uint32_t new_word_uses = 3;

  private:
    void load() const;
    std::filesystem::path file_;
    mutable bool loaded_{};
    mutable std::vector<Use> words_;
    mutable std::uint64_t clock_{};
    bool dirty_{};
    double changed_at_{};
};

// Up to `most` completions of `typed`, a word as typed: each is `typed` and
// the rest of a word, never changing what was typed, in capitals after two
// or more capitals. Listed words rank by frequency, raised by the user's
// uses; an offensive word ranks below all others until used once. Words the
// list lacks join after new_word_uses. The typed word itself is not offered.
std::vector<std::string> complete_word(const WordList& list, const LearnedWords& learned,
                                       std::string_view typed, std::size_t most);
} // namespace framekeyboard
