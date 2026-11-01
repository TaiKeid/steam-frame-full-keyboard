#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
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

    // The selection runs between its anchor and the cursor, which stays its
    // moving end. Typing, erasing and phrases replace it; moving the cursor
    // without select_to() ends it.
    bool has_selection() const { return anchor_ && *anchor_ != cursor_; }
    std::pair<std::size_t, std::size_t> selection() const; // bytes; empty at the cursor if none
    void select_to(std::size_t offset);                    // extends from the cursor as it stands
    void clear_selection() { anchor_.reset(); }
    // Copy, Cut and Paste use a buffer of the editor's own: the system clipboard
    // is neither read nor written, and the buffer is never logged.
    bool copy();
    bool cut();
    bool paste();
    bool can_paste() const { return !copied_.empty(); }

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
    bool erase_selection();
    std::string text_;
    std::size_t cursor_{};
    std::optional<std::size_t> anchor_; // the selection's fixed end
    std::size_t uncertain_{};           // bytes from the start
    std::vector<Phrase> phrases_;
    std::string copied_;
};
} // namespace framekeyboard
