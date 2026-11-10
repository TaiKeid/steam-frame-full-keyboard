#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// Local Pinyin, Zhuyin, Cangjie, Quick and Hangul composition. The app owns delivery and only clears
// this state after a successful text commit.
//
// Pinyin uses libime with OpenCC for Traditional output; Zhuyin uses
// McBopomofo. FRAMEKEYBOARD_CHINESE_ENGINE can name either retained engine
// for receiver-free comparisons, where it supports the requested method.
class CjkComposer {
  public:
    // What an engine learns goes in `learning_dir`, its own subfolder for each
    // engine; empty learns nothing.
    explicit CjkComposer(const std::string& method, const std::filesystem::path& learning_dir = {});
    ~CjkComposer();
    void type(char key);
    void backspace();
    void cancel();
    void cycle(int direction);
    void choose(int index);
    std::string preedit() const;
    std::string text() const;
    std::vector<std::string> candidates() const;
    int selected() const;
    bool empty() const;
    bool has_reading() const;
    bool chinese() const;
    // Digits and punctuation are part of the reading (Zhuyin), not candidates.
    bool phonetic_keys() const;
    // A quote separates syllables (Pinyin).
    bool syllables() const;
    // Two-set Hangul finishes a syllable once the next one starts, a full
    // Quick code once the next starts, and a long Pinyin or Zhuyin reading its
    // front. The app sends it at once and drops it; until then it stays part
    // of text().
    std::string finished() const;
    void drop_finished();
    // Drops only the open syllable; refused finished text still commits.
    void cancel_open();
    // text() reached the field, before cancel(); `learn` lets the engine learn
    // from it. What is learned is written by save_learning(), or when the
    // last engine using it goes.
    void committed(bool learn);
    // Writes what every loaded engine learned and has not saved yet.
    static void save_learning();
    // Every loaded engine forgets what it learned into this folder, in
    // memory, keeping its reading; the caller removes the files.
    static void clear_learning(const std::filesystem::path& learning_dir);
    bool table_context_enabled() const;
    void set_table_context(const std::string& previous);
    // Clears future context without reordering a presented lookup.
    void clear_table_context();
    // Space is the first tone of an open Zhuyin syllable, not a choice.
    bool takes_space() const;
    // Enter types Pinyin's letters as typed, not their conversion: text()
    // gives them until the reading changes. Other methods keep the conversion.
    void as_typed();
    // Zhuyin's cursor inside the reading: move() takes it that many
    // syllables, stopping at the ends, and the candidates follow the phrase
    // there. Other methods have none.
    bool has_cursor() const;
    void move(int syllables);
    void delete_forward();
    // Choosing a candidate for the phrase at the cursor (Zhuyin), which Space
    // and ↓ start; any change to the reading ends it.
    bool choosing() const;
    void set_choosing(bool choosing);
    // Punctuation into Zhuyin's reading at the cursor, by the key's character
    // and the text it types; false where it can't go there.
    bool insert_symbol(char symbol, const std::string& text);
    // The phrase the candidates are for and the cursor, as byte offsets into
    // preedit(); none without a cursor or while a syllable is open.
    struct Highlight {
        std::size_t begin{}, end{}, caret{};
    };
    std::optional<Highlight> highlight() const;
    // The engine composing: its module's name (libime,
    // mcbopomofo, hangul) or table for Cangjie and Quick.
    const std::string& engine() const;
    static bool supported(const std::string& method);

  private:
    struct State;
    void propose();
    std::unique_ptr<State> state_;
    bool phonetic_keys_{}, syllables_{};
};
// Cangjie and Quick read the tables SteamOS installs for IBus, in
// /usr/share/ibus-table/tables. Another folder holding cangjie5.db and
// quick5.db, for the simulator and tests on a desktop without them; before
// the first composer.
void set_table_directory(const std::filesystem::path& directory);
// Cangjie and Quick offer a code's characters commonest first, by the rank
// list in chinese/char-ranks.txt (tools/build_chinese_ranks.py); before the
// first composer. Without a readable list they keep the tables' own order.
void set_character_ranks(const std::filesystem::path& file);
} // namespace framekeyboard
