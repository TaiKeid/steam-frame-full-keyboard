#pragma once
#include "shortcuts.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace framekeyboard {
class OutputController;

// The part of the output a button's action uses (docs/shortcut-bar-design.md,
// Actions), so tests can stand in for it. Counters are OutputController's:
// together they tell whether an entry queued earlier was delivered.
class ShortcutOutput {
  public:
    virtual ~ShortcutOutput() = default;
    // A button press is admitted as a key press is (OutputController::admit):
    // it clears a failure with nothing left to retry, and a refusal leaves
    // the status a key's would.
    virtual bool admit(double now) = 0;
    // Whether this many entries fit now, so a sequence is never cut short.
    virtual bool can_queue(std::size_t entries) const = 0;
    // One balanced chord: downs in order, then ups in reverse.
    virtual bool tap(const std::vector<int>& codes) = 0;
    // The code that types `symbol` on the target, or `fallback`.
    virtual int shortcut_code(xkb_keysym_t symbol, int fallback) = 0;
    // The window with the keys (KeySink::focused_window): 0 when unknown or
    // when the route names none, as Steam fields and streams from a PC.
    virtual std::uint32_t focused_window() const = 0;
    // Beside the window: changes on each activation inside SteamOS's Desktop
    // (KeySink::focus_serial).
    virtual std::uint64_t focus_serial() const = 0;
    virtual std::uint64_t completed() const = 0;
    virtual std::size_t pending_entries() const = 0;
    virtual std::uint64_t failures() const = 0;
    virtual std::uint64_t cancels() const = 0;
};
// The real output queue behind that view. `keymap_letters`, when given,
// says whether letters go through the target's keymap now: App's answer is
// Unicode mode, as for Cut, Copy and Paste (App::chord_code); otherwise a
// letter goes by its position.
class QueuedShortcutOutput final : public ShortcutOutput {
  public:
    explicit QueuedShortcutOutput(OutputController& output, std::function<bool()> keymap_letters = {})
        : output_(output), keymap_letters_(std::move(keymap_letters)) {}
    bool admit(double now) override;
    bool can_queue(std::size_t entries) const override;
    bool tap(const std::vector<int>& codes) override;
    int shortcut_code(xkb_keysym_t symbol, int fallback) override;
    std::uint32_t focused_window() const override;
    std::uint64_t focus_serial() const override;
    std::uint64_t completed() const override;
    std::size_t pending_entries() const override;
    std::uint64_t failures() const override;
    std::uint64_t cancels() const override;

  private:
    OutputController& output_;
    std::function<bool()> keymap_letters_;
};

// Sends a button's action over time: the only code that sends a button's
// keys. A run is a pass of steps, each queued only after the one before it
// was delivered, so a stop sends nothing more; tick() advances it. A
// shortcut is one pass of chords. A macro presses keys only: its Keys steps
// are chords, its waits steps of their own, and it runs its passes once, a
// number of times or while its trigger is held (design, Macro). Run
// actions start programs elsewhere. Nothing a run sends is ever logged.
class ShortcutRunner {
  public:
    // The least time between one combination's delivery and the next one's
    // press, until a probe on the Frame says otherwise. OutputController adds
    // its own, shorter gap for a streaming client.
    static constexpr double shortcut_key_gap = .03;
    enum class End {
        None,     // no run yet, or one still under way
        Finished, // every step was delivered
        Stopped,  // stop(), or a new start()
        Failed,   // a step failed to deliver or could not be queued
        Canceled, // the output dropped what was queued, or another window has the keys
    };
    explicit ShortcutRunner(ShortcutOutput& output) : output_(output) {}
    // Starts `action` at `now`, ending a run under way as Stopped, and sends
    // what can go at once. False, with nothing sent, when it cannot go out
    // now: the output refuses it as it would a key press, or is too full
    // for a whole shortcut or a macro's next combination, the action can't
    // be sent, holds nothing, or is a Run action.
    bool start(const ShortcutAction& action, double now);
    // Sends nothing more; what was already queued finishes.
    void stop();
    // The trigger of a While held macro let go: the pass under way
    // finishes, and no other starts. Nothing for other runs.
    void release();
    // Its trigger pressed again while that pass finishes: passes go on
    // again until the next release (R342). False, changing nothing, unless
    // a While held macro is running.
    bool hold();
    // Advances the run. True when it ended in this call, for a redraw.
    bool tick(double now);
    bool running() const { return active_; }
    // R372: how far a run planned to take longer than long_run_seconds has come, 0 to 1: over the
    // whole run for Once and for a count, over the pass under way for While held. From the run's real
    // position (passes and steps done, time into the current wait), so it never runs ahead of a slow
    // delivery and never goes back within a sweep. None for a shorter run or none running.
    std::optional<double> progress(double now) const;
    // The time progress() sweeps over, in seconds: what the run really waits (waits, and the key
    // gap between two combinations), for the whole run or, While held, for the pass under way.
    // 0 when nothing runs.
    double planned() const;
    static constexpr double long_run_seconds = .5;
    End end() const { return end_; }
    // Probe only (shortcut-target-probe): the gap between combinations for
    // this runner. The keyboard always keeps shortcut_key_gap.
    void set_key_gap(double seconds) { key_gap_ = seconds; }

  private:
    double step_time(std::size_t i, bool first_pass) const;
    double pass_time(bool first_pass) const;
    struct Step {
        enum class Kind { Chord, Wait };
        Kind kind{Kind::Chord};
        ShortcutChord chord;
        double wait{}; // Wait: seconds
    };
    // Sends every step that may go now. True when the run ended.
    bool advance(double now);
    bool finish(End end);
    // Codes for a chord as the target types it now: letters through its
    // keymap, as recorded; other keys by position. Empty when unknown.
    std::vector<int> codes(const ShortcutChord& chord);
    bool delivered() const;
    // The window with the keys, or the Desktop's active app, changed since start().
    bool focus_moved() const;

    ShortcutOutput& output_;
    std::vector<Step> steps_; // one pass
    std::size_t next_{};
    // Passes run so far, and how many to run; while `held_` passes go on.
    std::size_t pass_{}, passes_{1};
    bool held_{};
    bool holds_{}; // a While held macro: its trigger can take it again
    bool active_{}, in_flight_{};
    bool sent_{};    // a combination went: later ones check the window
    bool waiting_{}; // the wait at next_ has begun
    // The step in flight was delivered once completed() reaches this: it
    // went in behind every entry already queued, and entries complete in
    // order. An entry dropped ahead of it (a released repeat) completes
    // nothing, so an empty queue also means it is out.
    std::uint64_t delivered_at_{};
    // The output's failures and cancels when the run began: any new one
    // dropped the step in flight and ends the run.
    std::uint64_t failures_{}, cancels_{};
    // The window with the keys at start(). Between combinations the queue
    // is empty and checks no window, so the runner does: a later combination
    // goes only to the same one. A window that became unknown counts as
    // another; a route naming none (0 throughout) is not checked this way.
    // The focus serial too: inside SteamOS's Desktop the window stays one
    // while its apps and dialogs take turns (R320).
    std::uint32_t window_{};
    std::uint64_t focus_serial_{};
    double key_gap_{shortcut_key_gap};
    double ready_at_{}; // when the next step may be pressed
    double done_at_{};  // when the step before the next ended: a wait counts from here
    End end_{End::None};
};
} // namespace framekeyboard
