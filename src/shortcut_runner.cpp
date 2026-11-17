#include "framekeyboard/shortcut_runner.hpp"
#include "framekeyboard/input.hpp"
#include "framekeyboard/output.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
bool QueuedShortcutOutput::admit(double now) {
    return output_.admit(true, now);
}
bool QueuedShortcutOutput::can_queue(std::size_t entries) const {
    return output_.can_queue(entries);
}
bool QueuedShortcutOutput::tap(const std::vector<int>& codes) {
    return output_.tap(codes);
}
int QueuedShortcutOutput::shortcut_code(xkb_keysym_t symbol, int fallback) {
    if (keymap_letters_ && !keymap_letters_()) {
        return fallback;
    }
    return output_.shortcut_code(symbol, fallback);
}
std::uint32_t QueuedShortcutOutput::focused_window() const {
    return output_.focused_window();
}
std::uint64_t QueuedShortcutOutput::focus_serial() const {
    return output_.focus_serial();
}
std::uint64_t QueuedShortcutOutput::completed() const {
    return output_.completed();
}
std::size_t QueuedShortcutOutput::pending_entries() const {
    return output_.pending_entries();
}
std::uint64_t QueuedShortcutOutput::failures() const {
    return output_.failures();
}
std::uint64_t QueuedShortcutOutput::cancels() const {
    return output_.cancels();
}

std::vector<int> ShortcutRunner::codes(const ShortcutChord& chord) {
    std::vector<int> result;
    for (const auto& modifier : chord.modifiers) {
        const int code = key_code(modifier);
        if (code == 0) {
            return {};
        }
        result.push_back(code);
    }
    if (!chord.key.empty()) {
        int code = key_code(chord.key);
        if (code == 0) {
            return {};
        }
        // A letter is kept as the letter the key showed when recorded:
        // Ctrl+A recorded on AZERTY is Ctrl+A on any target.
        if (chord.key.size() == 4 && chord.key.starts_with("Key") && chord.key[3] >= 'A' &&
            chord.key[3] <= 'Z') {
            code =
                output_.shortcut_code(static_cast<xkb_keysym_t>(XKB_KEY_a + (chord.key[3] - 'A')), code);
        }
        result.push_back(code);
    }
    return result;
}
bool ShortcutRunner::start(const ShortcutAction& action, double now) {
    stop();
    // A chord whose names can't be sent refuses the whole action.
    const auto sendable = [](const ShortcutChord& chord) {
        if (chord.key.empty() && chord.modifiers.empty()) {
            return false;
        }
        for (const auto& name : chord.modifiers) {
            if (!shortcut_modifier(name)) {
                return false;
            }
        }
        return chord.key.empty() || key_code(chord.key) != 0;
    };
    std::vector<Step> steps;
    std::size_t passes = 1;
    bool held = false;
    if (action.kind == ShortcutAction::Kind::Keys) {
        for (const auto& chord : action.chords) {
            if (!sendable(chord)) {
                return false;
            }
            steps.push_back({Step::Kind::Chord, chord, 0});
        }
    } else if (action.kind == ShortcutAction::Kind::Macro) {
        for (const auto& step : action.steps) {
            if (step.kind == MacroStep::Kind::Wait) {
                steps.push_back({Step::Kind::Wait, {}, step.wait_ms / 1000.0});
                continue;
            }
            for (const auto& chord : step.keys) {
                if (!sendable(chord)) {
                    return false;
                }
                steps.push_back({Step::Kind::Chord, chord, 0});
            }
        }
        switch (action.repeat.mode) {
        case MacroRepeat::Mode::Once:
            break;
        case MacroRepeat::Mode::Times:
            passes = static_cast<std::size_t>(std::max(1, action.repeat.count));
            break;
        case MacroRepeat::Mode::WhileHeld:
            held = true;
            break;
        }
    } else {
        return false; // Run starts programs elsewhere.
    }
    if (steps.empty()) {
        return false; // nothing recorded: a press does nothing
    }
    // Admitted as a key press is. A shortcut must fit whole now, or none of
    // it goes; a macro's combinations go one at a time, so one must fit.
    const bool shortcut = action.kind == ShortcutAction::Kind::Keys;
    if (!output_.admit(now) || !output_.can_queue(shortcut ? steps.size() : 1)) {
        return false;
    }
    steps_ = std::move(steps);
    next_ = 0;
    pass_ = 0;
    passes_ = passes;
    held_ = held;
    in_flight_ = false;
    sent_ = false;
    waiting_ = false;
    active_ = true;
    end_ = End::None;
    failures_ = output_.failures();
    cancels_ = output_.cancels();
    window_ = output_.focused_window();
    focus_serial_ = output_.focus_serial();
    ready_at_ = done_at_ = now;
    advance(now);
    return true;
}
bool ShortcutRunner::finish(End end) {
    active_ = false;
    in_flight_ = false;
    waiting_ = false;
    held_ = false;
    steps_.clear();
    next_ = 0;
    end_ = end;
    return true;
}
void ShortcutRunner::stop() {
    if (active_) {
        finish(End::Stopped);
    }
}
void ShortcutRunner::release() {
    held_ = false;
}
bool ShortcutRunner::delivered() const {
    return output_.completed() >= delivered_at_ || output_.pending_entries() == 0;
}
bool ShortcutRunner::focus_moved() const {
    return output_.focused_window() != window_ || output_.focus_serial() != focus_serial_;
}
bool ShortcutRunner::advance(double now) {
    while (active_) {
        // Checked before delivery: a failure or cancel empties the queue too.
        if (output_.failures() != failures_) {
            return finish(End::Failed);
        }
        if (output_.cancels() != cancels_) {
            return finish(End::Canceled);
        }
        if (in_flight_) {
            if (!delivered()) {
                return false;
            }
            in_flight_ = false;
            done_at_ = now;
            ready_at_ = now + shortcut_key_gap;
        }
        if (next_ == steps_.size()) {
            // A pass ended: another while passes remain or the trigger holds.
            ++pass_;
            if (pass_ < passes_ || held_) {
                next_ = 0;
                continue;
            }
            return finish(End::Finished);
        }
        // Ctrl+O, then S: the first may open a dialog, which the second must
        // not reach. Inside SteamOS's Desktop only the serial shows it. During
        // a gap or a wait too, so a run waits for nothing in another window.
        if (sent_ && focus_moved()) {
            return finish(End::Canceled);
        }
        const auto& step = steps_[next_];
        if (step.kind == Step::Kind::Wait) {
            // A wait counts from the step before it: the last combination's
            // delivery, or the start; the run holds here until it is over.
            if (!waiting_) {
                ready_at_ = done_at_ + step.wait;
                waiting_ = true;
            }
            if (now < ready_at_) {
                return false;
            }
            waiting_ = false;
            done_at_ = ready_at_;
            ++next_;
            continue;
        }
        if (now < ready_at_) {
            return false;
        }
        const auto chord = codes(step.chord);
        // Counted before the tap, which may deliver at once.
        delivered_at_ = output_.completed() + output_.pending_entries() + 1;
        if (chord.empty() || !output_.tap(chord)) {
            return finish(End::Failed);
        }
        in_flight_ = true;
        sent_ = true;
        ++next_;
    }
    return false;
}
bool ShortcutRunner::tick(double now) {
    return active_ && advance(now);
}
} // namespace framekeyboard
