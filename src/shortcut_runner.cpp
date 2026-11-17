#include "framekeyboard/shortcut_runner.hpp"
#include "framekeyboard/input.hpp"
#include "framekeyboard/output.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
bool QueuedShortcutOutput::can_queue(std::size_t entries) const {
    return output_.can_queue(entries);
}
bool QueuedShortcutOutput::tap(const std::vector<int>& codes) {
    return output_.tap(codes);
}
int QueuedShortcutOutput::shortcut_code(xkb_keysym_t symbol, int fallback) {
    return output_.shortcut_code(symbol, fallback);
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
    if (action.kind != ShortcutAction::Kind::Keys || action.chords.empty()) {
        return false; // Macros and Run come with their own phases.
    }
    std::vector<Step> steps;
    for (const auto& chord : action.chords) {
        // Names are checked now, so an unsendable chord refuses the whole.
        if (chord.key.empty() && chord.modifiers.empty()) {
            return false;
        }
        for (const auto& name : chord.modifiers) {
            if (!shortcut_modifier(name)) {
                return false;
            }
        }
        if (!chord.key.empty() && key_code(chord.key) == 0) {
            return false;
        }
        steps.push_back({Step::Kind::Chord, chord});
    }
    // The whole sequence must fit now, or none of it goes.
    if (!output_.can_queue(steps.size())) {
        return false;
    }
    steps_ = std::move(steps);
    next_ = 0;
    in_flight_ = false;
    active_ = true;
    end_ = End::None;
    failures_ = output_.failures();
    cancels_ = output_.cancels();
    ready_at_ = now;
    advance(now);
    return true;
}
bool ShortcutRunner::finish(End end) {
    active_ = false;
    in_flight_ = false;
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
bool ShortcutRunner::delivered() const {
    return output_.completed() >= delivered_at_ || output_.pending_entries() == 0;
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
            ready_at_ = now + shortcut_key_gap;
        }
        if (next_ == steps_.size()) {
            return finish(End::Finished);
        }
        if (now < ready_at_) {
            return false;
        }
        const auto chord = codes(steps_[next_].chord);
        // Counted before the tap, which may deliver at once.
        delivered_at_ = output_.completed() + output_.pending_entries() + 1;
        if (chord.empty() || !output_.tap(chord)) {
            return finish(End::Failed);
        }
        in_flight_ = true;
        ++next_;
    }
    return false;
}
bool ShortcutRunner::tick(double now) {
    return active_ && advance(now);
}
} // namespace framekeyboard
