#include "framekeyboard/output.hpp"
#include "framekeyboard/shortcut_runner.hpp"

#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <map>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
using Events = std::vector<std::pair<int, int>>;
// A recording target that never reaches an application. With `slow`, each
// key down waits for `done`, as a transport that acknowledges keys does.
struct Recorder : KeySink {
    Events events;
    bool slow{}, waiting{}, done{}, succeed{true};
    std::uint32_t window{};
    std::uint64_t serial{};             // KeySink::focus_serial
    std::map<xkb_keysym_t, int> keymap; // the target's letters, when not US
    std::vector<xkb_keysym_t> asked;
    // The tick time of each event, in step with `events`.
    double now{};
    std::vector<double> times;
    void send(int code, int value) override {
        events.emplace_back(code, value);
        times.push_back(now);
    }
    Delivery deliver_key(int code, int value) override {
        if (!slow || value == 0) {
            send(code, value);
            return Delivery::Success;
        }
        if (waiting) {
            if (!done) {
                return Delivery::Waiting;
            }
            waiting = false;
            return succeed ? Delivery::Success : Delivery::Failure;
        }
        send(code, value);
        waiting = true;
        done = false;
        return Delivery::Waiting;
    }
    void cancel_delivery() override { waiting = text_waiting = false; }
    // Text, acknowledged later as keys are: `text_done`, then `text_ok`.
    bool text_waiting{}, text_done{}, text_ok{true};
    bool text_available() override { return true; }
    bool commit_text(const std::string& text) override {
        if (text_waiting) {
            if (!text_done) {
                return false;
            }
            text_waiting = false;
            return text_ok;
        }
        events.emplace_back(-static_cast<int>(text.size()), 0);
        text_waiting = true;
        text_done = false;
        return false;
    }
    bool text_pending() const override { return text_waiting; }
    void cancel_text() override { text_waiting = false; }
    std::uint32_t focused_window() const override { return window; }
    std::uint64_t focus_serial() const override { return serial; }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        asked.push_back(symbol);
        const auto found = keymap.find(symbol);
        return found == keymap.end() ? fallback : found->second;
    }
};
// The output queue as App owns it, with the runner on top.
struct Rig {
    Recorder sink;
    OutputController output{sink};
    QueuedShortcutOutput view{output};
    ShortcutRunner runner{view};
    Rig() { output.set_enabled(true); }
    bool tick(double now) {
        sink.now = now;
        output.tick(now);
        return runner.tick(now);
    }
    // Acknowledges each waiting key down in turn at `now`.
    void acknowledge(double now) {
        for (int i = 0; i < 16 && sink.waiting; ++i) {
            sink.done = true;
            tick(now);
        }
    }
};
// Ticks every 5 ms through (from, to].
void run_out(Rig& rig, double from, double to) {
    for (int i = 1; from + i * .005 <= to; ++i) {
        rig.tick(from + i * .005);
    }
}
ShortcutAction shortcut(std::vector<ShortcutChord> chords) {
    ShortcutAction action;
    action.kind = ShortcutAction::Kind::Keys;
    action.chords = std::move(chords);
    return action;
}
const Events ctrl_k{{KEY_LEFTCTRL, 1}, {KEY_K, 1}, {KEY_K, 0}, {KEY_LEFTCTRL, 0}};
const Events s{{KEY_S, 1}, {KEY_S, 0}};
Events joined(Events a, const Events& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void order_and_spacing() {
    Rig rig;
    require(rig.runner.start(shortcut({{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}}), 10),
            "a shortcut starts");
    require(rig.sink.events == ctrl_k, "the first combination goes at once, balanced");
    require(rig.runner.running(), "and the run waits for the gap");
    require(!rig.tick(10.02) && rig.sink.events == ctrl_k, "nothing within shortcut_key_gap");
    require(rig.tick(10.031), "the run ends once the last combination is delivered");
    require(rig.sink.events == joined(ctrl_k, s), "the second combination after the gap, in order");
    require(!rig.runner.running() && rig.runner.end() == ShortcutRunner::End::Finished, "finished");
    require(!rig.tick(11), "a finished run does nothing more");
    // Four combinations: each one gap after the one before.
    Rig four;
    four.runner.start(shortcut({{{}, "F13"}, {{}, "F14"}, {{}, "F15"}, {{}, "F24"}}), 0);
    run_out(four, 0, .2);
    require(four.sink.events == Events{{KEY_F13, 1},
                                       {KEY_F13, 0},
                                       {KEY_F14, 1},
                                       {KEY_F14, 0},
                                       {KEY_F15, 1},
                                       {KEY_F15, 0},
                                       {KEY_F24, 1},
                                       {KEY_F24, 0}},
            "F13 to F24 are sent by their codes, each a gap apart");
    require(four.runner.end() == ShortcutRunner::End::Finished, "four combinations finish");
    for (std::size_t press = 2; press < four.sink.events.size(); press += 2) {
        // Each combination's press at least the gap after the one before.
        require(four.sink.times[press] - four.sink.times[press - 2] >=
                    ShortcutRunner::shortcut_key_gap - 1e-9,
                "each combination goes at least shortcut_key_gap after the one before");
    }
    // Modifiers alone.
    Rig super;
    super.runner.start(shortcut({{{"MetaLeft"}, ""}}), 0);
    require(super.sink.events == Events{{KEY_LEFTMETA, 1}, {KEY_LEFTMETA, 0}} &&
                super.runner.end() == ShortcutRunner::End::Finished,
            "a tap of Super alone");
    // Modifiers go down in their recorded order and up in reverse.
    Rig both;
    both.runner.start(shortcut({{{"ShiftLeft", "ControlLeft"}, "KeyB"}}), 0);
    require(both.sink.events == Events{{KEY_LEFTSHIFT, 1},
                                       {KEY_LEFTCTRL, 1},
                                       {KEY_B, 1},
                                       {KEY_B, 0},
                                       {KEY_LEFTCTRL, 0},
                                       {KEY_LEFTSHIFT, 0}},
            "modifiers in order, released in reverse");
}
void waits_for_delivery() {
    Rig rig;
    rig.sink.slow = true;
    rig.runner.start(shortcut({{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}}), 0);
    require(rig.sink.events == Events{{KEY_LEFTCTRL, 1}}, "Ctrl waits for its acknowledgment");
    rig.tick(1);
    require(rig.sink.events.size() == 1, "nothing more while it waits, however long");
    rig.acknowledge(1);
    require(rig.sink.events == ctrl_k, "the first combination is delivered at 1 s");
    rig.tick(1.02);
    require(rig.sink.events == ctrl_k, "the gap counts from delivery, not from the start");
    rig.tick(1.031);
    require(rig.sink.events == joined(ctrl_k, Events{{KEY_S, 1}}), "the next goes a gap after delivery");
    rig.acknowledge(1.04);
    require(rig.sink.events == joined(ctrl_k, s) && rig.runner.end() == ShortcutRunner::End::Finished,
            "and the run finishes when it is delivered");
}
void whole_or_refused() {
    {
        Rig rig;
        rig.output.set_enabled(false);
        const auto letter = shortcut({{{}, "KeyA"}});
        const bool started = rig.runner.start(letter, 0);
        require(!started && rig.sink.events.empty() && !rig.runner.running(),
                "no target: refused, nothing sent");
    }
    {
        // A full queue: thirty taps wait behind a key the target hasn't taken.
        Rig rig;
        rig.sink.slow = true;
        for (int i = 0; i < 30; ++i) {
            require(rig.output.tap({KEY_X}), "filler queued");
        }
        const auto before = rig.sink.events;
        const auto three = shortcut({{{}, "KeyA"}, {{}, "KeyB"}, {{}, "KeyC"}});
        const bool started = rig.runner.start(three, 0);
        require(!started && rig.sink.events == before && rig.output.pending_entries() == 30,
                "three combinations into room for two: refused whole, nothing queued");
        require(rig.runner.start(shortcut({{{}, "KeyA"}, {{}, "KeyB"}}), 0),
                "two combinations into room for two start");
    }
    {
        // A press is admitted as a key's: a failure with nothing to retry
        // is cleared, and the shortcut goes.
        Rig rig;
        rig.sink.slow = true;
        rig.sink.succeed = false;
        rig.output.tap({KEY_X});
        rig.acknowledge(0);
        require(rig.output.failures() == 1, "a failure");
        rig.sink.slow = false;
        rig.sink.events.clear();
        const auto letter = shortcut({{{}, "KeyA"}});
        const bool started = rig.runner.start(letter, 1);
        require(started && rig.sink.events == Events{{KEY_A, 1}, {KEY_A, 0}},
                "after a failure with nothing to retry, a button press sends");
    }
    {
        // Text still waiting for an explicit retry keeps it refused, with
        // the status a key would get.
        Rig rig;
        require(rig.output.commit_text("ab"), "text queued");
        rig.sink.text_done = true;
        rig.sink.text_ok = false;
        rig.tick(.1);
        require(rig.output.retry_available(), "the text waits for a retry");
        rig.output.take_status();
        rig.sink.events.clear();
        const auto letter = shortcut({{{}, "KeyA"}});
        const bool started = rig.runner.start(letter, 1);
        require(!started && rig.sink.events.empty() && rig.output.take_status().has_value(),
                "a failure with text to retry refuses a button, with a status");
    }
    {
        Rig rig;
        require(!rig.runner.start(shortcut({}), 0), "a shortcut without combinations is refused");
        require(!rig.runner.start(shortcut({{{}, "Nope"}}), 0), "an unknown key refuses the whole");
        require(!rig.runner.start(shortcut({{{}, "KeyA"}, {{"KeyB"}, "KeyC"}}), 0),
                "a bad modifier in a later combination refuses the whole");
        require(!rig.runner.start(shortcut({{{}, ""}}), 0), "an empty combination is refused");
        ShortcutAction macro;
        macro.kind = ShortcutAction::Kind::Macro;
        macro.steps.push_back({MacroStep::Kind::Wait, {}, 100});
        require(rig.runner.start(macro, 0), "a macro runs, a wait alone too");
        rig.runner.stop();
        ShortcutAction run;
        run.kind = ShortcutAction::Kind::Run;
        run.command = "true";
        require(!rig.runner.start(run, 0), "nor programs");
        require(rig.sink.events.empty(), "nothing refused sends anything");
    }
}
void letters() {
    Rig rig;
    // An AZERTY target: the letter A is on the QWERTY Q key, and Z on W.
    rig.sink.keymap = {{XKB_KEY_a, KEY_Q}, {XKB_KEY_z, KEY_W}};
    rig.runner.start(
        shortcut(
            {{{"ControlLeft"}, "KeyA"}, {{"ControlLeft"}, "KeyZ"}, {{}, "Digit1"}, {{}, "Semicolon"}}),
        0);
    run_out(rig, 0, .2);
    require(rig.sink.events == Events{{KEY_LEFTCTRL, 1},
                                      {KEY_Q, 1},
                                      {KEY_Q, 0},
                                      {KEY_LEFTCTRL, 0},
                                      {KEY_LEFTCTRL, 1},
                                      {KEY_W, 1},
                                      {KEY_W, 0},
                                      {KEY_LEFTCTRL, 0},
                                      {KEY_1, 1},
                                      {KEY_1, 0},
                                      {KEY_SEMICOLON, 1},
                                      {KEY_SEMICOLON, 0}},
            "letters go through the target's keymap; other keys by position");
    require(rig.sink.asked == std::vector<xkb_keysym_t>{XKB_KEY_a, XKB_KEY_z},
            "only letters are looked up");
    // Resolved when sent: a keymap that changes mid-run applies to what follows.
    Rig later;
    later.runner.start(shortcut({{{}, "KeyA"}, {{}, "KeyA"}}), 0);
    later.sink.keymap = {{XKB_KEY_a, KEY_Q}};
    later.tick(.031);
    require(later.sink.events == Events{{KEY_A, 1}, {KEY_A, 0}, {KEY_Q, 1}, {KEY_Q, 0}},
            "each combination is resolved as it is sent");
    // Outside Unicode mode App keeps letters by position, as Cut, Copy and
    // Paste do: the target's keymap is not asked.
    Recorder sink;
    sink.keymap = {{XKB_KEY_a, KEY_Q}};
    OutputController output(sink);
    output.set_enabled(true);
    bool unicode = false;
    QueuedShortcutOutput view(output, [&] { return unicode; });
    ShortcutRunner runner(view);
    runner.start(shortcut({{{}, "KeyA"}}), 0);
    unicode = true;
    runner.start(shortcut({{{}, "KeyA"}}), 1);
    require(sink.events == Events{{KEY_A, 1}, {KEY_A, 0}, {KEY_Q, 1}, {KEY_Q, 0}} &&
                sink.asked == std::vector<xkb_keysym_t>{XKB_KEY_a},
            "the keymap is asked only when App says letters go through it");
}
void stops() {
    const auto two = shortcut({{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}});
    {
        Rig rig;
        rig.runner.start(two, 0);
        rig.runner.stop();
        require(!rig.runner.running() && rig.runner.end() == ShortcutRunner::End::Stopped, "stopped");
        rig.tick(1);
        require(rig.sink.events == ctrl_k, "a stopped run sends nothing more");
    }
    {
        // A stop while a combination waits: it still finishes, balanced.
        Rig rig;
        rig.sink.slow = true;
        rig.runner.start(two, 0);
        rig.runner.stop();
        rig.acknowledge(.5);
        rig.tick(1);
        require(rig.sink.events == ctrl_k, "the combination under way finishes, nothing follows");
    }
    {
        // A new start ends the run under way.
        Rig rig;
        rig.runner.start(two, 0);
        require(rig.runner.start(shortcut({{{}, "KeyB"}}), .01), "another button starts");
        rig.tick(1);
        require(rig.sink.events == joined(ctrl_k, Events{{KEY_B, 1}, {KEY_B, 0}}),
                "the first run sends nothing more");
    }
    {
        // The output is canceled: hiding, a target change, sleep.
        Rig rig;
        rig.sink.slow = true;
        rig.runner.start(two, 0);
        rig.output.cancel();
        require(rig.tick(.1) && rig.runner.end() == ShortcutRunner::End::Canceled,
                "a canceled output ends the run");
        rig.acknowledge(.2);
        rig.tick(1);
        require(rig.sink.events == Events{{KEY_LEFTCTRL, 1}, {KEY_LEFTCTRL, 0}},
                "a canceled run sends nothing more, and its held key comes up");
    }
    {
        // Output turned off, which cancels it.
        Rig rig;
        rig.runner.start(two, 0);
        rig.output.set_enabled(false);
        require(rig.tick(.05) && rig.runner.end() == ShortcutRunner::End::Canceled,
                "output turned off ends the run");
        require(rig.sink.events == ctrl_k, "and nothing more is sent");
    }
    {
        // A step fails to deliver.
        Rig rig;
        rig.sink.slow = true;
        rig.sink.succeed = false;
        rig.runner.start(two, 0);
        rig.acknowledge(.1);
        require(rig.runner.end() == ShortcutRunner::End::Failed, "a failed delivery ends the run");
        rig.sink.succeed = true;
        rig.tick(1);
        rig.acknowledge(1);
        require(rig.sink.events == Events{{KEY_LEFTCTRL, 1}, {KEY_LEFTCTRL, 0}},
                "nothing is retried or sent after a failure");
    }
    {
        // Another window takes the keys mid-run: the output fails the run.
        Rig rig;
        rig.sink.slow = true;
        rig.sink.window = 5;
        rig.runner.start(two, 0);
        rig.sink.window = 6;
        rig.acknowledge(.1);
        rig.tick(1);
        require(rig.runner.end() == ShortcutRunner::End::Failed, "a focus change mid-run ends it");
        require(rig.sink.events == Events{{KEY_LEFTCTRL, 1}, {KEY_LEFTCTRL, 0}},
                "and nothing reaches the new window");
    }
    {
        // Between combinations the output queue is empty and checks no
        // window: the runner itself ends the run before the next one.
        Rig rig;
        rig.sink.window = 5;
        rig.runner.start(two, 0);
        rig.tick(.01);
        rig.sink.window = 6;
        rig.tick(.05);
        require(rig.runner.end() == ShortcutRunner::End::Canceled && rig.sink.events == ctrl_k,
                "another window between combinations ends the run, the second unsent");
    }
    {
        // Inside SteamOS's Desktop the window stays one: an activation
        // reported between combinations ends the run (R320).
        Rig rig;
        rig.sink.window = 5;
        rig.sink.serial = 3;
        rig.runner.start(two, 0);
        rig.tick(.01);
        rig.sink.serial = 4;
        rig.tick(.05);
        require(rig.runner.end() == ShortcutRunner::End::Canceled && rig.sink.events == ctrl_k,
                "an activation in the same window between combinations ends the run, the second unsent");
    }
    {
        // A window that became unknown counts as another.
        Rig rig;
        rig.sink.window = 5;
        rig.runner.start(two, 0);
        rig.sink.window = 0;
        rig.tick(.05);
        require(rig.runner.end() == ShortcutRunner::End::Canceled && rig.sink.events == ctrl_k,
                "a window that became unknown ends the run");
    }
    {
        // A route that names no window, as a Steam field, isn't checked so.
        Rig rig;
        rig.runner.start(two, 0);
        rig.tick(.05);
        require(rig.runner.end() == ShortcutRunner::End::Finished &&
                    rig.sink.events == joined(ctrl_k, s),
                "a route without windows runs to its end");
    }
}
} // namespace

ShortcutAction macro(std::vector<MacroStep> steps, MacroRepeat repeat = {}) {
    ShortcutAction action;
    action.kind = ShortcutAction::Kind::Macro;
    action.steps = std::move(steps);
    action.repeat = repeat;
    return action;
}
MacroStep keys_of(std::vector<ShortcutChord> chords) {
    MacroStep step;
    step.kind = MacroStep::Kind::Keys;
    step.keys = std::move(chords);
    return step;
}
MacroStep wait_of(int ms) {
    MacroStep step;
    step.kind = MacroStep::Kind::Wait;
    step.wait_ms = ms;
    return step;
}
const Events a{{KEY_A, 1}, {KEY_A, 0}}, b{{KEY_B, 1}, {KEY_B, 0}};
// The tick time at which `code` went down for the `nth` time (0 first).
double down_at(const Recorder& sink, int code, int nth = 0) {
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        if (sink.events[i] == std::pair{code, 1} && nth-- == 0) {
            return sink.times[i];
        }
    }
    throw std::runtime_error("no such key down");
}
std::size_t downs(const Recorder& sink, int code) {
    return static_cast<std::size_t>(
        std::count(sink.events.begin(), sink.events.end(), std::pair{code, 1}));
}
// Macros (design, Macro): steps in order, waits, repeats, while held and
// every stop rule, mid-step and mid-wait.
void macros() {
    {
        // A Keys step's combinations in turn, then a wait, then the next step.
        Rig rig;
        require(rig.runner.start(macro({keys_of({{{}, "KeyA"}, {{}, "KeyB"}}), wait_of(200),
                                        keys_of({{{}, "KeyS"}})}),
                                 0),
                "a macro starts");
        run_out(rig, 0, .2);
        require(rig.sink.events == joined(a, b), "a Keys step's combinations in order, then the wait");
        run_out(rig, .2, .5);
        require(rig.sink.events == joined(joined(a, b), s) &&
                    rig.runner.end() == ShortcutRunner::End::Finished,
                "the step after the wait, and the run finishes");
        const double waited = down_at(rig.sink, KEY_S) - down_at(rig.sink, KEY_B);
        require(waited >= .2 && waited < .2 + .02, "a wait lasts its time from the step before it");
    }
    {
        // Times: as many passes, each a gap after the one before.
        Rig rig;
        rig.runner.start(macro({keys_of({{{}, "KeyA"}})}, {MacroRepeat::Mode::Times, 3}), 0);
        run_out(rig, 0, .5);
        require(downs(rig.sink, KEY_A) == 3 && rig.runner.end() == ShortcutRunner::End::Finished,
                "Times runs its count of passes");
        require(down_at(rig.sink, KEY_A, 1) - down_at(rig.sink, KEY_A, 0) >=
                    ShortcutRunner::shortcut_key_gap,
                "passes are spaced as combinations are");
    }
    {
        // Once: one pass; an empty macro and Run do nothing.
        Rig rig;
        rig.runner.start(macro({keys_of({{{}, "KeyA"}})}), 0);
        run_out(rig, 0, .5);
        require(downs(rig.sink, KEY_A) == 1, "Once runs one pass");
        require(!rig.runner.start(macro({}), 1), "a macro without steps does nothing");
        ShortcutAction run;
        run.kind = ShortcutAction::Kind::Run;
        run.command = "true";
        require(!rig.runner.start(run, 1), "Run is not sent as keys");
        require(!rig.runner.start(macro({keys_of({{{}, "Nope"}})}), 1),
                "an unknown key refuses the macro");
    }
    {
        // While held: passes go on until the trigger lets go; the pass under
        // way finishes, and no other starts.
        Rig rig;
        rig.runner.start(macro({keys_of({{{}, "KeyA"}}), wait_of(100), keys_of({{{}, "KeyB"}})},
                               {MacroRepeat::Mode::WhileHeld, 0}),
                         0);
        run_out(rig, 0, .5);
        require(downs(rig.sink, KEY_A) >= 3 && rig.runner.running(), "passes go on while held");
        // Let go mid-pass: after an A, during its wait.
        const auto passes = downs(rig.sink, KEY_A);
        while (downs(rig.sink, KEY_A) == downs(rig.sink, KEY_B) + 0 && rig.runner.running()) {
            rig.tick(rig.sink.now + .005);
        }
        rig.runner.release();
        run_out(rig, rig.sink.now, rig.sink.now + .5);
        require(!rig.runner.running() && rig.runner.end() == ShortcutRunner::End::Finished &&
                    downs(rig.sink, KEY_A) == downs(rig.sink, KEY_B) &&
                    downs(rig.sink, KEY_A) <= passes + 1,
                "the pass under way finishes after the release, and no other starts");
    }
    const auto waiting = macro({keys_of({{{}, "KeyA"}}), wait_of(500), keys_of({{{}, "KeyB"}})});
    {
        // stop() mid-wait: nothing more.
        Rig rig;
        rig.runner.start(waiting, 0);
        run_out(rig, 0, .2);
        rig.runner.stop();
        run_out(rig, .2, 1);
        require(rig.sink.events == a && rig.runner.end() == ShortcutRunner::End::Stopped,
                "a stop mid-wait sends nothing more");
    }
    {
        // stop() mid-step: the combination under way finishes, balanced.
        Rig rig;
        rig.sink.slow = true;
        rig.runner.start(waiting, 0);
        rig.runner.stop();
        rig.acknowledge(.1);
        run_out(rig, .1, 1);
        require(rig.sink.events == a, "a stop mid-step finishes the combination, nothing follows");
    }
    {
        // A failed step ends the run as Failed; nothing is retried.
        Rig rig;
        rig.sink.slow = true;
        rig.sink.succeed = false;
        rig.runner.start(waiting, 0);
        rig.acknowledge(.1);
        rig.sink.succeed = true;
        run_out(rig, .1, 1);
        require(rig.runner.end() == ShortcutRunner::End::Failed && downs(rig.sink, KEY_B) == 0,
                "a step that fails ends the run");
    }
    {
        // The output drops what was queued (the target went away) mid-wait.
        Rig rig;
        rig.runner.start(waiting, 0);
        run_out(rig, 0, .2);
        rig.output.cancel();
        run_out(rig, .2, 1);
        require(rig.runner.end() == ShortcutRunner::End::Canceled && downs(rig.sink, KEY_B) == 0,
                "a cancel mid-wait ends the run");
    }
    {
        // Another window takes the keys during a wait: the run ends then.
        Rig rig;
        rig.sink.window = 5;
        rig.runner.start(waiting, 0);
        run_out(rig, 0, .2);
        rig.sink.window = 6;
        rig.tick(.21);
        require(!rig.runner.running() && rig.runner.end() == ShortcutRunner::End::Canceled,
                "a focus change during a wait ends the run at once");
        run_out(rig, .21, 1);
        require(downs(rig.sink, KEY_B) == 0, "and nothing reaches the new window");
    }
    {
        // SteamOS's Desktop: the window stays, its active app changes mid-wait.
        Rig rig;
        rig.sink.window = 5;
        rig.sink.serial = 1;
        rig.runner.start(waiting, 0);
        run_out(rig, 0, .2);
        rig.sink.serial = 2;
        run_out(rig, .2, 1);
        require(rig.runner.end() == ShortcutRunner::End::Canceled && downs(rig.sink, KEY_B) == 0,
                "an activation in the Desktop during a wait ends the run");
    }
}
// R372: a run planned past half a second shows how far it has come: over the whole run for Once and
// a count, over each pass for While held; never ahead of delivery, never back, full at the end.
void progress() {
    {
        // 0.4 s planned: no progress shown.
        Rig rig;
        rig.runner.start(macro({wait_of(400), keys_of({{{}, "KeyA"}})}), 0);
        rig.tick(.1);
        require(!rig.runner.progress(.1), "a run of 0.5 s or less shows none");
    }
    {
        // Once: a wait of 0.3 s, a key, a wait of 0.3 s: planned 0.6 s (a key after a wait goes at
        // once, and a wait counts from the key's delivery).
        Rig rig;
        rig.runner.start(macro({wait_of(300), keys_of({{{}, "KeyA"}}), wait_of(300)}), 0);
        double last = -1;
        bool rising = true;
        for (double now = .005; rig.runner.running() && now < 1; now += .005) {
            rig.tick(now);
            if (const auto p = rig.runner.progress(now)) {
                rising = rising && *p >= last;
                last = *p;
            }
        }
        // It ends as its last wait runs out, in the tick after the last one seen: within a tick of full.
        require(rising && last >= 1 - .005 / .6 - 1e-9, "it rises with the run and is full at its end");
        require(!rig.runner.progress(1), "and goes with it");
    }
    {
        // A slow delivery: the key waits for its acknowledgement, and the progress waits with it.
        Rig rig;
        rig.sink.slow = true;
        rig.runner.start(macro({keys_of({{{}, "KeyA"}}), wait_of(600)}), 0);
        rig.tick(.005);
        const auto before = rig.runner.progress(.5);
        require(before && *before == 0, "nothing done while the first key is not delivered");
        rig.acknowledge(.5);
        rig.tick(.8);
        const auto after = rig.runner.progress(.8);
        require(after && *after > 0 && *after < 1, "then it runs on from the delivery");
    }
    {
        // Times 3 of 0.2 s a pass: planned 0.6 s, one sweep for the whole run.
        Rig rig;
        rig.runner.start(macro({wait_of(200)}, {MacroRepeat::Mode::Times, 3}), 0);
        rig.tick(.25);
        const auto first = rig.runner.progress(.25);
        rig.tick(.45);
        const auto second = rig.runner.progress(.45);
        require(first && second && *first > .3 && *first < .5 && *second > *first && *second < .8,
                "a count's passes make one sweep");
    }
    {
        // R375: the total is what the run really waits. A gap of 30 ms only between two
        // combinations; none before a wait, after one, at the start or at the end.
        const auto planned = [](const ShortcutAction& action, double now = .001) {
            Rig rig;
            rig.runner.start(action, 0);
            rig.tick(now);
            return rig.runner.planned();
        };
        const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        std::vector<ShortcutChord> twenty;
        for (int i = 0; i < 20; ++i) {
            twenty.push_back({{}, i % 2 ? "KeyA" : "KeyB"});
        }
        require(near(planned(macro({keys_of(twenty)})), 19 * .03), "only keys: the gaps between them");
        require(near(planned(macro({wait_of(300), wait_of(400)})), .7), "only waits: their sum");
        require(near(planned(macro({keys_of({{{}, "KeyA"}}), wait_of(300),
                                    keys_of({{{}, "KeyB"}, {{}, "KeyC"}}), wait_of(200)})),
                     .3 + .03 + .2),
                "a mix: waits, and a gap only between two keys");
        // A, wait, B three times: the first pass 0.1 s, each later one 0.13 s (B, then A).
        require(near(planned(macro({keys_of({{{}, "KeyA"}}), wait_of(100), keys_of({{{}, "KeyB"}})},
                                   {MacroRepeat::Mode::Times, 3})),
                     .1 + 2 * .13),
                "a count: a gap between passes only after a key");
        Rig rig;
        rig.runner.start(macro({keys_of({{{}, "KeyA"}}), wait_of(600), keys_of({{{}, "KeyB"}})},
                               {MacroRepeat::Mode::WhileHeld, 0}),
                         0);
        rig.runner.hold();
        rig.tick(.001);
        require(near(rig.runner.planned(), .6), "While held: the first pass's own time");
        run_out(rig, .001, .7);
        require(near(rig.runner.planned(), .63), "and a later pass's, the gap after B included");
        rig.runner.release();
        run_out(rig, .7, 2);
        require(rig.runner.planned() == 0, "nothing when the run is over");
        // A count that ends on a key closes its stroke: full before the run ends.
        Rig closing;
        closing.runner.start(
            macro({wait_of(200), keys_of({{{}, "KeyA"}, {{}, "KeyB"}})}, {MacroRepeat::Mode::Times, 3}),
            0);
        double last = -1;
        for (double now = .005; closing.runner.running() && now < 2; now += .005) {
            closing.tick(now);
            if (const auto p = closing.runner.progress(now)) {
                require(*p >= last, "a count's stroke never goes back");
                last = *p;
            }
        }
        // 0.23 s a pass, 0.69 s in all: within the 5 ms tick before the end of full.
        require(last >= 1 - .005 / .69 - 1e-9, "a count's stroke closes as the run ends");
    }
    {
        // While held, 0.6 s a pass: a sweep each pass, back to the start for the next.
        Rig rig;
        rig.runner.start(macro({wait_of(600)}, {MacroRepeat::Mode::WhileHeld, 0}), 0);
        rig.runner.hold();
        rig.tick(.5);
        const auto late = rig.runner.progress(.5);
        rig.tick(.7);
        const auto next = rig.runner.progress(.7);
        require(late && next && *late > .7 && *next < .3, "While held sweeps once a pass");
        rig.runner.release();
        run_out(rig, .7, 2);
    }
}
int main() {
    try {
        order_and_spacing();
        macros();
        waits_for_delivery();
        whole_or_refused();
        letters();
        stops();
        progress();
        std::cout << "Shortcut runner order, spacing, refusal, letters and stops passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
