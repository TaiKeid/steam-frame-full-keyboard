#pragma once

#include "framekeyboard/clipboard.hpp"
#include "framekeyboard/input.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace framekeyboard::simulator {
// The simulator's text field: its text, caret and selection, edited as a
// desktop text box takes keys. Offsets are UTF-8 bytes and always fall on a
// character boundary (a grapheme cluster, so an emoji with its skin tone is
// one character). The text lives in memory only: nothing here logs it,
// saves it or touches the system clipboard (AGENTS.md).
class TextField {
  public:
    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }
    // The selected bytes, first to last; empty when nothing is selected.
    std::optional<std::pair<std::size_t, std::size_t>> selection() const;
    // Replaces the selection, or inserts at the caret.
    void insert(const std::string& text);
    // Backspace and Delete: the selection, else one character.
    void erase_backward();
    void erase_forward();
    // Caret moves. `extend` keeps the selection's anchor (Shift); `word`
    // jumps a word (Ctrl). Up/Down and Home/End work on lines between line
    // breaks; `whole` (Ctrl+Home/End) goes to either end of the text.
    void move_horizontal(int direction, bool extend, bool word);
    void move_vertical(int direction, bool extend);
    void move_line_edge(bool end, bool extend, bool whole);
    void select_all();
    // The field's own clipboard: Copy and Cut fill it, Paste reads it, and
    // dictation's paste Send writes it.
    void copy();
    void cut();
    void paste();
    const std::string& clipboard() const { return clipboard_; }
    void set_clipboard(std::string text);
    // A click in the field. Snaps to the character boundary at or before it.
    void place_caret(std::size_t offset);
    void clear();
    // Changes with every edit and caret move, so the window knows to redraw.
    std::uint64_t revision() const { return revision_; }

  private:
    // Character boundaries in bytes, from 0 to text_.size().
    std::vector<std::size_t> stops() const;
    std::size_t line_start(std::size_t offset) const;
    std::size_t line_end(std::size_t offset) const;
    void move_to(std::size_t offset, bool extend);
    void erase_selection();
    void changed() {
        ++revision_;
        column_.reset();
    }
    std::string text_, clipboard_;
    std::size_t caret_{};
    std::optional<std::size_t> anchor_; // The selection's other end, when one is selected.
    // Up and Down keep the column they started from across shorter lines,
    // until another edit or move.
    std::optional<std::ptrdiff_t> column_;
    std::uint64_t revision_{};
};

// Takes what the keyboard sends to an application, as a text box would:
// key presses through deliver_key() and text through commit_text() and
// replace_text(). Its checks on text are the compositor transport's
// (GamescopeText::commit), so the keyboard is held to the same limits.
//
// A slow field confirms each key press and text after `slow_delay`,
// answering Waiting until then, as Steam's own text fields do on the Frame.
// The change lands when it is confirmed. Releases always land at once.
//
// It is also dictation's clipboard: paste Send writes into the field's own
// clipboard, and the Ctrl+V after it pastes from there.
class FieldSink : public KeySink, public Clipboard {
  public:
    static constexpr double slow_delay = .03;
    explicit FieldSink(TextField& field) : field_(field) {}
    // The clock that slow confirmations count from, set before every App tick.
    void set_time(double now) { now_ = now; }
    void set_slow(bool slow) { slow_ = slow; }
    bool slow() const { return slow_; }
    // A key press or text is waiting for its confirmation.
    bool waiting() const { return pending_.has_value(); }

    void send(int code, int value) override { apply_key(code, value); }
    Delivery deliver_key(int code, int value) override;
    bool text_available() override { return true; }
    std::uint32_t focused_window() const override {
        return 1;
    } // One private receiver, not an OS window.
    bool commit_text(const std::string& text) override { return deliver_text(0, text); }
    bool replaces_text() override { return true; }
    bool replace_text(std::size_t delete_before, const std::string& text) override {
        return deliver_text(delete_before, text);
    }
    bool text_pending() const override { return pending_ && pending_->text; }
    void cancel_text() override { pending_.reset(); }

    std::uint64_t write(const std::string& text) override;
    std::optional<bool> result(std::uint64_t) const override { return true; }
    std::uint64_t served() const override { return served_; }

  private:
    // A press or text sent to a slow field, until its confirmation.
    struct Pending {
        int code{}, value{};
        std::optional<std::string> text; // Set for text, with its deletions.
        std::size_t deletions{};
        double ready{};
    };
    bool ctrl() const;
    void apply_key(int code, int value);
    void apply_text(std::size_t deletions, const std::string& text);
    bool deliver_text(std::size_t deletions, const std::string& text);
    TextField& field_;
    std::set<int> held_; // Modifier codes pressed and not yet released.
    std::optional<Pending> pending_;
    double now_{};
    bool slow_{};
    std::uint64_t writes_{}, served_{};
};
} // namespace framekeyboard::simulator
