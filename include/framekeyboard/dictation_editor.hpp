#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace framekeyboard {
// The dictation editor's text, cursor and phrase history (docs/dictation.md,
// Editor). Pure and host-testable: line layout and scrolling belong to the
// panel, which turns positions into cursor moves through set_cursor().
//
// The text is UTF-8 plain text with line breaks. The cursor is a byte offset
// that always lies on a character boundary, as Pango indexes text; limits
// count Unicode code points, as the counter shows them.
class DictationEditor {
  public:
    static constexpr std::size_t dictation_limit = 900; // dictation stops here
    static constexpr std::size_t hard_limit = 1000;     // typing and pending phrases stop here

    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    std::size_t length() const; // code points
    bool empty() const { return text_.empty(); }
    bool can_dictate() const { return length() < dictation_limit; }

    // Typed text at the cursor, cut at the hard limit. Tabs and other control
    // characters except line breaks are dropped. False when nothing changed.
    bool type(const std::string& text);
    bool backspace();
    bool erase_forward();
    void set_cursor(std::size_t offset); // moved back to a character boundary
    void left();
    void right();
    void clear();
    // Removes delivered text from the start, keeping the cursor on the same
    // character where it survives.
    void erase_front(std::size_t bytes);
    // Marks the first `bytes` as text that may already have reached the target,
    // after Send failed. An edit before the mark's end removes it.
    void mark_uncertain(std::size_t bytes);
    std::size_t uncertain() const { return uncertain_; }

    // A recognized phrase at the cursor, joined to its neighbours: a space at
    // either edge where needed in space-separated languages, and no final
    // period when text follows. Cut at the hard limit. False when nothing fit.
    bool insert_phrase(const std::string& phrase, bool spaced_language);
    // Removes the most recent phrase whose text is unchanged since it was
    // inserted; repeated calls remove earlier ones. False when none is left.
    bool undo_phrase();
    bool can_undo() const;
    // Up to `limit` code points before the cursor, as the recognizer's prompt.
    std::string context(std::size_t limit = 200) const;

  private:
    struct Phrase {
        std::size_t start, size; // bytes, including joining spaces
        std::string text;
    };
    // Replaces [start, end) with `insertion`, keeping phrase records in step:
    // a change inside a phrase makes it no longer undoable.
    void replace(std::size_t start, std::size_t end, const std::string& insertion);
    std::string text_;
    std::size_t cursor_{};
    std::size_t uncertain_{}; // bytes from the start
    std::vector<Phrase> phrases_;
};
} // namespace framekeyboard
