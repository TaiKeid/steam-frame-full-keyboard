#pragma once

#include <string>
#include <vector>

namespace framekeyboard {
// Korean 12-key 천지인 composition. Vowels are built from the strokes ㅣ, ㆍ
// and ㅡ (ㅣㆍ → ㅏ, ㆍㅡ → ㅗ, ㅏㅣ → ㅐ, ...); consonants come whole from
// their keys, toggled by the app. The reading is a list of units that is
// syllabified again after every change, so a stroke that turns a vowel into
// another, or a toggle that replaces a final, rearranges the syllables the
// way two-set Hangul does, including finals that move to the next syllable.
class CheonjiinComposer {
  public:
    // Adds what a key typed: a stroke, a vowel (a flick), a consonant, or
    // other text such as punctuation. Throws, keeping the reading, once it
    // would exceed 30 units or 30 characters as shown.
    void add(const std::string& text);
    // Replaces the last unit, for a repeated toggle tap (ㄱ → ㅋ → ㄲ).
    void replace(const std::string& text);
    // Removes the last stroke or jamo.
    void backspace();
    void cancel();
    // The composed reading. Unfinished strokes (ㆍ, ㆍㆍ) show as they are.
    std::string text() const;
    // The last unit's text, which a toggle tap must still find to replace it.
    std::string last() const;
    bool empty() const;

  private:
    struct Unit {
        enum class Kind { Consonant, Vowel, Other } kind;
        // A consonant's jamo, or other text.
        std::string text;
        // A vowel's additions in order: single strokes, or a flicked vowel's
        // strokes at once, so Backspace removes what one press added.
        std::vector<std::string> steps;
        std::string strokes() const;
    };
    std::vector<Unit> units_;
    void append(const std::string& text);
    // Throws if the shown reading is longer than one commit may be.
    void check() const;
};
} // namespace framekeyboard
