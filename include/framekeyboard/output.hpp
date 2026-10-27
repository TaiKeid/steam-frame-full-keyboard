#pragma once

#include "input.hpp"
#include <cstdint>
#include <deque>

namespace framekeyboard {
// Where App sends typed text. OutputController delivers it to the target; a
// local editor can take its place. Native key presses stay with KeyboardState.
class TextOutput {
  public:
    virtual ~TextOutput() = default;
    // A held text key: typed once now, then repeated while held.
    virtual bool text_press(unsigned pointer, const std::string& text, double now) = 0;
    virtual bool commit_text(const std::string& text) = 0;
    // Whether replace_text() is one ordered request; otherwise use taps_then_text().
    virtual bool replaces_text() = 0;
    virtual bool replace_text(std::size_t delete_before, const std::string& text) = 0;
    virtual bool taps_then_text(int code, std::size_t count, const std::string& text) = 0;
};
// One field-scoped output session. Pointer/legend state stays in KeyboardState;
// transport protocol state stays in the sinks. Only this class orders delivery.
class OutputController : public KeySink, public TextOutput {
  public:
    explicit OutputController(KeySink& target) : target_(target) {}
    ~OutputController() override;
    static constexpr std::size_t capacity = 32;
    bool admit(bool produces_output, double now);
    // Counts delivery failures, so callers can tell that text they assumed
    // was in the target may not be (a failed or timed-out request).
    std::uint64_t failures() const { return failures_; }
    // Counts entries delivered in full. With failures(), tells a sender that
    // its entry arrived, rather than being dropped by a cancel.
    std::uint64_t completed() const { return completed_; }
    bool admit(bool produces_output) { return admit(produces_output, now_); }
    void native_press(unsigned pointer, const std::vector<int>& codes, int repeat_code, double now);
    bool text_press(unsigned pointer, const std::string& text, double now) override;
    void release(unsigned pointer);
    void release_modifier(int code);
    void cancel();
    bool tick(double now);
    bool retry();
    bool retry_available() const { return !retry_.empty(); }
    // Whether a sequence of this many entries fits now, so it is never cut short.
    bool can_queue(std::size_t entries) const {
        return enabled_ && !failed_ && queue_.size() + entries <= capacity;
    }
    std::string retry_text() const;
    std::optional<std::string> take_status();
    void set_enabled(bool value);
    bool enabled() const { return enabled_; }

    // Lock synchronization uses the same ordered path as ordinary presses.
    void send(int code, int value) override;
    bool pump() override { return target_.pump(); }
    bool take_input_reset() override { return target_.take_input_reset(); }
    bool can_resume() const override { return target_.can_resume(); }
    bool text_available() override { return target_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool replaces_text() override { return target_.replaces_text(); }
    bool replace_text(std::size_t delete_before, const std::string& text) override;
    // Queues `count` taps of `code`, then `text`, as one sequence admitted
    // once. A failure found while delivering it drops the rest; no later part
    // is admitted again as fresh input. Nothing is queued if `text` is invalid.
    bool taps_then_text(int code, std::size_t count, const std::string& text) override;
    // Presses `codes` in order as one entry, then releases them in reverse
    // once all are delivered, as a chord such as Shift+Enter.
    bool tap(const std::vector<int>& codes);
    bool single_line_target() const override { return target_.single_line_target(); }
    bool steam_field_target() const override { return target_.steam_field_target(); }
    void set_character_gap(double seconds) override { target_.set_character_gap(seconds); }
    void set_desktop_paste(Clipboard* clipboard) override { target_.set_desktop_paste(clipboard); }
    bool text_pending() const override { return !queue_.empty(); }
    void cancel_text() override { cancel(); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return target_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return target_.num_lock_state(); }

  private:
    struct Entry {
        std::string text;
        std::size_t delete_before{}; // Text entries only: deletions sent with the text.
        std::vector<int> codes, acquired;
        std::set<int> released_modifiers;
        std::size_t next{};
        int repeat_code{};
        bool held{}, started{}, waiting{}, repeat{};
        double repeat_at{};
        std::weak_ptr<Entry> owner;
    };
    bool enqueue_text(const std::string& text, std::shared_ptr<Entry> entry = {});
    static bool sendable(const std::string& text);
    void drain();
    // `delivered` characters at the start of the front text entry are known
    // to have reached the target; the retry keeps only the rest.
    void fail(std::size_t delivered = 0);
    void drop_holds();
    void release_codes(Entry& entry, std::optional<int> only = {});
    void status(std::string message);
    KeySink& target_;
    std::deque<std::shared_ptr<Entry>> queue_, retry_;
    std::map<unsigned, std::shared_ptr<Entry>> active_;
    std::map<int, std::shared_ptr<Entry>> direct_;
    std::map<int, unsigned> delivered_;
    double now_{};
    bool enabled_{}, failed_{};
    std::uint64_t failures_{}, completed_{};
    std::string status_;
    bool status_changed_{};
};
} // namespace framekeyboard
