#pragma once

#include "input.hpp"
#include <cstdint>
#include <deque>
#include <tuple>
#include <utility>

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
    static constexpr std::size_t commit_limit = 32;
    static constexpr std::size_t max_commit_codepoints = capacity * commit_limit;
    bool admit(bool produces_output, double now);
    // Counts delivery failures, so callers can tell that text they assumed
    // was in the target may not be (a failed or timed-out request).
    std::uint64_t failures() const { return failures_; }
    // Counts entries delivered in full. With failures(), tells a sender that
    // its entry arrived, rather than being dropped by a cancel.
    std::uint64_t completed() const { return completed_; }
    // Counts entries ever queued: unchanged means nothing was sent since.
    std::uint64_t queued() const { return queued_; }
    // Counts cancels, each of which drops whatever was still queued.
    std::uint64_t cancels() const { return cancels_; }
    // Whether the retry's first character may already have arrived: it was
    // sent when delivery stopped. Not when another window took focus between
    // the characters of a commit, or before the text went out.
    bool retry_uncertain() const { return retry_uncertain_; }
    bool admit(bool produces_output) { return admit(produces_output, now_); }
    // `first_repeat`: how long a held key waits before it repeats.
    void native_press(unsigned pointer, const std::vector<int>& codes, int repeat_code, double now,
                      double first_repeat = .5);
    bool text_press(unsigned pointer, const std::string& text, double now) override;
    void release(unsigned pointer);
    void release_modifier(int code);
    // Ends every hold as if each pointer let go, for a change that keeps the
    // target: held keys come up now, and accepted text and taps still finish
    // in order. cancel() also abandons them, for a target that is gone.
    void release_holds();
    void cancel();
    // Runs `stop`, which cancels output, keeping a rest kept for an explicit
    // retry and the status that asks for it: a gate that only pauses output
    // leaves the retry for once it opens. cancel() itself still drops both.
    template <class Stop> void keeping_retry(Stop&& stop) {
        auto kept = std::exchange(retry_, {});
        auto shown = std::pair{status_, status_changed_};
        stop();
        if (!kept.empty()) {
            retry_ = std::move(kept);
            failed_ = true;
            std::tie(status_, status_changed_) = std::move(shown);
        }
    }
    bool tick(double now);
    bool retry();
    bool retry_available() const { return !retry_.empty(); }
    // Whether the last failure also dropped queued input that no retry
    // repeats: a key, or anything queued after one.
    bool discarded() const { return discarded_; }
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
    // A longer composition is admitted as one bounded batch, then delivered
    // in paced commits. True transfers all retry ownership to this queue.
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
    std::uint32_t focused_window() const override { return target_.focused_window(); }
    void set_character_gap(double seconds) override { target_.set_character_gap(seconds); }
    void set_desktop_paste(Clipboard* clipboard) override { target_.set_desktop_paste(clipboard); }
    bool desktop_focused() const override { return target_.desktop_focused(); }
    void set_stream_paste(Clipboard* clipboard) override { target_.set_stream_paste(clipboard); }
    bool stream_focused() const override { return target_.stream_focused(); }
    EditKeys edit_keys() const override { return target_.edit_keys(); }
    bool edit_keys_known() const override { return target_.edit_keys_known(); }
    void set_terminal_hint(bool terminal) override { target_.set_terminal_hint(terminal); }
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
        double first_repeat{.5};
        bool held{}, started{}, waiting{}, repeat{}, paced{};
        // The target took this text and is delivering it. Only then is its
        // delivered-character count about this entry rather than an earlier one.
        bool submitted{};
        // For a streaming client, mixed text goes one character at a time
        // (one_by_one): the bytes and characters already delivered.
        bool one_by_one{};
        std::size_t done_bytes{}, done_characters{};
        double repeat_at{};
        std::weak_ptr<Entry> owner;
    };
    bool enqueue_text(const std::string& text, std::shared_ptr<Entry> entry = {});
    static bool sendable(const std::string& text);
    bool settling() const;
    bool window_moved();
    void note_text_time(bool paced, bool fresh_tick);
    void drain(bool fresh_tick = false);
    // `delivered` characters at the start of the front text entry are known
    // to have reached the target; the retry keeps only the rest, whose first
    // character may have arrived too unless `uncertain` is false.
    void fail(std::size_t delivered = 0, bool uncertain = true);
    void drop_holds();
    void release_codes(Entry& entry, std::optional<int> only = {});
    void status(std::string message);
    KeySink& target_;
    std::deque<std::shared_ptr<Entry>> queue_, retry_;
    std::map<unsigned, std::shared_ptr<Entry>> active_;
    std::map<int, std::shared_ptr<Entry>> direct_;
    std::map<int, unsigned> delivered_;
    double now_{};
    // When the last key entry finished: a streaming client takes the next
    // only stream_key_gap later (drain).
    double last_key_at_{-1};
    static constexpr double stream_key_gap = .02;
    // A paced composition must let the compositor's temporary keymap settle
    // between chunks. Keep this debt across cancellation and explicit retries.
    double last_text_at_{-1}, text_ready_at_{-1};
    bool text_time_pending_{}, paced_time_pending_{};
    // Gamescope's focused window when the queue's current run of output
    // began. Text accepted for one window never goes on into another, which
    // a click or a closing dashboard can focus mid-run; the rest is kept for
    // a retry, which belongs to the window focused then.
    std::uint32_t window_{};
    bool retry_uncertain_{true}; // Set by every fail(), which also fills the retry.
    bool enabled_{}, failed_{};
    bool discarded_{}; // by the last fail()
    std::uint64_t failures_{}, completed_{}, queued_{}, cancels_{};
    std::string status_;
    bool status_changed_{};
};
} // namespace framekeyboard
