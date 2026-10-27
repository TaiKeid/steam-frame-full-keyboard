#pragma once

#include "input.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// The text sink (docs/integration.md, Text sink). It sits between App's
// output queue and the real input backend. Off, it passes everything
// through. On, it is the only destination: committed text and named keys
// become numbered events for the shell, and nothing reaches libei, uinput or
// the clipboard. Composition, candidates and the dictation editor never come
// this far, so only committed output can leave the keyboard.
//
// One event is in flight at a time: the output queue waits for the shell's
// `ack` before the next, exactly as it waits for an asynchronous transport.
// A `reject` fails that entry, which the queue keeps for an explicit retry.
class ShellRouter : public KeySink {
  public:
    explicit ShellRouter(KeySink& inner) : inner_(inner) {}
    struct Event {
        enum class Kind { Commit, Key } kind{Kind::Commit};
        std::uint64_t seq{};
        std::string context;
        std::string text; // Commit only; never logged.
        std::string key;  // Logical key ID, such as "Enter" or "KeyA".
        std::vector<std::string> modifiers;
        bool repeat{};
    };
    bool on() const { return on_; }
    // Callers cancel output through the old destination first; anything
    // still in flight is dropped here.
    void set_on(bool on);
    // Every event carries the context it was typed for.
    void set_context(std::string context);
    // Events not yet sent to the shell, in order.
    std::vector<Event> take_events();
    // The shell accepted every event up to `seq`, or rejected one. Answers
    // for another context, or for events not in flight, are ignored.
    void acknowledge(const std::string& context, std::uint64_t seq);
    void reject(const std::string& context, std::uint64_t seq);
    // Every sent event acknowledged and nothing waiting.
    bool settled() const { return !in_flight_ && pending_.empty(); }

    void send(int code, int value) override;
    Delivery deliver_key(int code, int value) override;
    void cancel_delivery() override;
    // On, the sink owns readiness: the system backend keeps its connection
    // up, but its pauses and resets do not stop typing into the shell.
    bool pump() override {
        const bool ready = inner_.pump();
        return on_ || ready;
    }
    bool take_input_reset() override {
        const bool reset = inner_.take_input_reset();
        return !on_ && reset;
    }
    bool can_resume() const override { return !on_ && inner_.can_resume(); }
    bool text_available() override { return on_ || inner_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool replaces_text() override { return !on_ && inner_.replaces_text(); }
    bool replace_text(std::size_t count, const std::string& text) override {
        return !on_ && inner_.replace_text(count, text);
    }
    bool text_pending() const override { return on_ ? in_flight_.has_value() : inner_.text_pending(); }
    void cancel_text() override;
    std::size_t delivered_characters() const override { return on_ ? 0 : inner_.delivered_characters(); }
    bool output_blocked() const override { return !on_ && inner_.output_blocked(); }
    // A shortcut keeps the keyboard's own key ID; the target app's
    // physical keymap applies only to normal output.
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return on_ ? fallback : inner_.shortcut_code(symbol, fallback);
    }
    bool single_line_target() const override { return !on_ && inner_.single_line_target(); }
    bool steam_field_target() const override { return !on_ && inner_.steam_field_target(); }
    void set_character_gap(double seconds) override { inner_.set_character_gap(seconds); }
    void set_desktop_paste(Clipboard* clipboard) override { inner_.set_desktop_paste(clipboard); }
    std::optional<bool> num_lock_state() override {
        return on_ ? std::nullopt : inner_.num_lock_state();
    }

  private:
    struct InFlight {
        Event event;
        enum class Answer { None, Accepted, Rejected } answer{Answer::None};
    };
    // The same request offered again consumes its answer; a new one starts.
    std::optional<bool> answer(const Event& offered);
    std::vector<std::string> modifier_names() const;
    KeySink& inner_;
    bool on_{};
    std::string context_;
    std::uint64_t seq_{};
    std::optional<InFlight> in_flight_;
    std::vector<Event> pending_; // Not yet written to the shell.
    std::vector<int> modifiers_; // Held or latched, in press order.
};
} // namespace framekeyboard
