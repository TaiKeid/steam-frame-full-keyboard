#pragma once
#include "shortcuts.hpp"

#include <cstddef>
#include <cstdint>
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
    // Whether this many entries fit now, so a sequence is never cut short.
    virtual bool can_queue(std::size_t entries) const = 0;
    // One balanced chord: downs in order, then ups in reverse.
    virtual bool tap(const std::vector<int>& codes) = 0;
    // The code that types `symbol` on the target, or `fallback`.
    virtual int shortcut_code(xkb_keysym_t symbol, int fallback) = 0;
    virtual std::uint64_t completed() const = 0;
    virtual std::size_t pending_entries() const = 0;
    virtual std::uint64_t failures() const = 0;
    virtual std::uint64_t cancels() const = 0;
};
// The real output queue behind that view.
class QueuedShortcutOutput final : public ShortcutOutput {
  public:
    explicit QueuedShortcutOutput(OutputController& output) : output_(output) {}
    bool can_queue(std::size_t entries) const override;
    bool tap(const std::vector<int>& codes) override;
    int shortcut_code(xkb_keysym_t symbol, int fallback) override;
    std::uint64_t completed() const override;
    std::size_t pending_entries() const override;
    std::uint64_t failures() const override;
    std::uint64_t cancels() const override;

  private:
    OutputController& output_;
};

// Sends a button's action over time: the only code that sends a button's
// keys. A run is a list of steps, each queued only after the one before it
// was delivered, so a stop sends nothing more; tick() advances it. Only
// shortcuts run so far; macros (text, waits, repeats, while held) become
// further step kinds of the same run, and Run actions start programs
// elsewhere. Nothing a run sends is ever logged.
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
        Canceled, // the output dropped what was queued: hide, target change
    };
    explicit ShortcutRunner(ShortcutOutput& output) : output_(output) {}
    // Starts `action` at `now`, ending a run under way as Stopped, and sends
    // what can go at once. False, with nothing sent, when it cannot go out
    // whole now: output disabled, failed or too full, an unsendable action,
    // or a kind not run here yet.
    bool start(const ShortcutAction& action, double now);
    // Sends nothing more; what was already queued finishes.
    void stop();
    // Advances the run. True when it ended in this call, for a redraw.
    bool tick(double now);
    bool running() const { return active_; }
    End end() const { return end_; }

  private:
    struct Step {
        // Text, Wait and pass boundaries join with macros.
        enum class Kind { Chord };
        Kind kind{Kind::Chord};
        ShortcutChord chord;
    };
    // Sends every step that may go now. True when the run ended.
    bool advance(double now);
    bool finish(End end);
    // Codes for a chord as the target types it now: letters through its
    // keymap, as recorded; other keys by position. Empty when unknown.
    std::vector<int> codes(const ShortcutChord& chord);
    bool delivered() const;

    ShortcutOutput& output_;
    std::vector<Step> steps_;
    std::size_t next_{};
    bool active_{}, in_flight_{};
    // The step in flight was delivered once completed() reaches this: it
    // went in behind every entry already queued, and entries complete in
    // order. An entry dropped ahead of it (a released repeat) completes
    // nothing, so an empty queue also means it is out.
    std::uint64_t delivered_at_{};
    // The output's failures and cancels when the run began: any new one
    // dropped the step in flight and ends the run.
    std::uint64_t failures_{}, cancels_{};
    double ready_at_{}; // when the next step may be pressed
    End end_{End::None};
};
} // namespace framekeyboard
