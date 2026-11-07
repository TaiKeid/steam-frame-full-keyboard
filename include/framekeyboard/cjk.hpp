#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// Local Pinyin, Zhuyin, Cangjie, Quick and Hangul composition. The app owns delivery and only clears
// this state after a successful text commit. No engine learning API is used.
class CjkComposer {
  public:
    explicit CjkComposer(const std::string& method);
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
    // Two-set Hangul finishes a syllable once the next one starts. The app
    // sends it at once and drops it; until then it stays part of text().
    std::string finished() const;
    void drop_finished();
    // Drops only the open syllable; refused finished text still commits.
    void cancel_open();
    static bool supported(const std::string& method);

  private:
    struct State;
    std::unique_ptr<State> state_;
    bool phonetic_keys_{}, syllables_{};
};
// Cangjie and Quick read the tables SteamOS installs for IBus, in
// /usr/share/ibus-table/tables. Another folder holding cangjie5.db and
// quick5.db, for the simulator and tests on a desktop without them; before
// the first composer.
void set_table_directory(const std::filesystem::path& directory);
} // namespace framekeyboard
