#include "framekeyboard/output.hpp"
#include "framekeyboard/shortcut_runner.hpp"

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
    std::map<xkb_keysym_t, int> keymap; // the target's letters, when not US
    std::vector<xkb_keysym_t> asked;
    void send(int code, int value) override { events.emplace_back(code, value); }
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
    void cancel_delivery() override { waiting = false; }
    std::uint32_t focused_window() const override { return window; }
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
        // After a failed delivery nothing is queued until the next deliberate press.
        Rig rig;
        rig.sink.slow = true;
        rig.sink.succeed = false;
        rig.output.tap({KEY_X});
        rig.acknowledge(0);
        require(rig.output.failures() == 1, "a failure");
        require(!rig.runner.start(shortcut({{{}, "KeyA"}}), 1), "failed output refuses a shortcut");
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
        macro.steps.push_back({MacroStep::Kind::Wait, {}, {}, 100});
        require(!rig.runner.start(macro, 0), "macros are not run yet");
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
        // Between combinations the output queue is empty, so a focus change
        // fails nothing there: App stops the run when the focus moves.
        Rig rig;
        rig.sink.window = 5;
        rig.runner.start(two, 0);
        rig.runner.stop(); // as App does when the focus moves
        rig.sink.window = 6;
        rig.tick(1);
        require(rig.sink.events == ctrl_k, "a run stopped on a focus change sends nothing more");
    }
}
} // namespace

int main() {
    try {
        order_and_spacing();
        waits_for_delivery();
        whole_or_refused();
        letters();
        stops();
        std::cout << "Shortcut runner order, spacing, refusal, letters and stops passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
