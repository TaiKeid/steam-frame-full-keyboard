#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace framekeyboard {
// The key words are matched by: lower-cased, so Hel finds hello and HELLO.
// `turkic` lowers I to ı and İ to i, as Turkish and Azerbaijani write them.
std::string word_key(std::string_view word, bool turkic = false);
// Whether a language, by its locale (tr-TR), cases I as Turkish does.
bool turkic_case(std::string_view locale);
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
    // `turkic`: its keys follow Turkish casing (word_key()).
    static WordList parse(std::istream& input, const Respelling& respell = {},
                          std::istream* offensive = nullptr, bool turkic = false);
    static WordList load(const std::filesystem::path& path, const Respelling& respell = {},
                         const std::filesystem::path& offensive = {}, bool turkic = false);
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
    // `word` as this list writes its language, respelled.
    std::string respelled(std::string_view word) const;
    bool respells() const { return !respell_.empty(); }
    // `word`'s key in this list's language.
    std::string key(std::string_view word) const { return word_key(word, turkic_); }
    bool turkic() const { return turkic_; }

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
    Respelling respell_;
    bool turkic_{};
};

// The words the user finished in one language, how often and how lately,
// kept as plain text in the config folder, the owner's alone, at most
// most_words, the least recent going first. An empty path keeps nothing.
class LearnedWords {
  public:
    // `turkic` as for its language's WordList.
    explicit LearnedWords(std::filesystem::path file = {}, bool turkic = false)
        : file_(std::move(file)), turkic_(turkic) {}
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
    // Whether the file was read: until then, empty() and size() read it.
    bool loaded() const { return loaded_; }

    static constexpr std::size_t most_words = 5000;
    static constexpr std::size_t longest = 40; // Characters in a word.
    // A word the list lacks is suggested once finished this often.
    static constexpr std::uint32_t new_word_uses = 3;

  private:
    void load() const;
    std::filesystem::path file_;
    bool turkic_{};
    mutable bool loaded_{};
    mutable std::map<std::string, Use, std::less<>> words_; // By key.
    mutable std::uint64_t clock_{};
    bool dirty_{};
    double changed_at_{};
};

// Loads a word list, and the user's words for it, on a thread of its own:
// parsing takes a quarter second, never on the VR loop. Only the latest
// request is loaded; the thread starts with the first request and ends with
// the loader, after any load under way.
class WordListLoader {
  public:
    struct Request {
        std::string id; // Names the result.
        std::filesystem::path list, offensive, learned;
        WordList::Respelling respell;
        bool turkic{};
    };
    struct Result {
        std::string id;
        std::shared_ptr<const WordList> list;
        LearnedWords learned;
    };
    WordListLoader() = default;
    WordListLoader(const WordListLoader&) = delete;
    WordListLoader& operator=(const WordListLoader&) = delete;
    ~WordListLoader();
    // Replaces a request not started yet; false if the thread can't start.
    bool request(Request request);
    // The latest finished load, once; a later request drops an earlier result.
    std::optional<Result> take();

  private:
    void run();
    std::mutex lock_;
    std::condition_variable wake_;
    std::optional<Request> waiting_;
    std::optional<Result> done_;
    bool stop_{};
    std::thread thread_;
};

// Up to `most` completions of `typed`, a word as typed: each is `typed` and
// the rest of a word, never changing what was typed, in capitals after two
// or more capitals. Listed words rank by frequency, raised by the user's
// uses; an offensive word ranks below all others until used once. Words the
// list lacks join after new_word_uses. The typed word itself is not offered.
std::vector<std::string> complete_word(const WordList& list, const LearnedWords& learned,
                                       std::string_view typed, std::size_t most);
} // namespace framekeyboard
