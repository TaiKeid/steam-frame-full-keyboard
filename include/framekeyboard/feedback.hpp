#pragma once
#include <array>
#include <map>
#include <utility>
#include <vector>

namespace framekeyboard {
enum class KeyFeedback { Hover, Press, Release };
struct HapticPulse {
    float duration, frequency, amplitude;
    float delay{}; // Seconds from now.
};
constexpr HapticPulse key_pulse(KeyFeedback feedback) {
    // Equal API values felt stronger on release on Frame. Keep its pulse
    // shorter so the perceived click can match the press.
    switch (feedback) {
    case KeyFeedback::Hover:
        return {.004f, 240.f, .1f};
    case KeyFeedback::Press:
        return {.025f, 150.f, 1.f};
    case KeyFeedback::Release:
        return {.008f, 150.f, .35f};
    }
    return {};
}
// Dictation listening starts with two light rising ticks and ends with one
// soft, low pulse, unlike any key click. Silence or a full editor can end it
// while the eyes are on the text, so the hand feels the change.
constexpr std::array<HapticPulse, 2> listening_pulses(bool started) {
    if (started) {
        return {{{.012f, 180.f, .5f, 0}, {.012f, 230.f, .6f, .09f}}};
    }
    return {{{.035f, 110.f, .45f, 0}, {}}};
}
class KeyHaptics {
  public:
    void hover(unsigned device) { pending_.try_emplace(device, KeyFeedback::Hover); }
    void press(unsigned pointer, unsigned device) {
        owners_[pointer] = device;
        pending_[device] = KeyFeedback::Press;
    }
    void release(unsigned pointer) {
        const auto owner = owners_.find(pointer);
        if (owner != owners_.end()) {
            pending_[owner->second] = KeyFeedback::Release;
            owners_.erase(owner);
        }
    }
    void cancel_pointer(unsigned pointer) {
        if (const auto owner = owners_.find(pointer); owner != owners_.end()) {
            pending_.erase(owner->second);
            owners_.erase(owner);
        }
    }
    void cancel() {
        owners_.clear();
        pending_.clear();
    }
    std::vector<std::pair<unsigned, KeyFeedback>> take(double now) {
        std::vector<std::pair<unsigned, KeyFeedback>> result;
        for (const auto& [device, kind] : pending_) {
            // A hover arriving in the next frame must not extend a click.
            if (kind == KeyFeedback::Hover && now < hover_after_[device]) {
                continue;
            }
            result.emplace_back(device, kind);
            if (kind != KeyFeedback::Hover) {
                hover_after_[device] = now + key_pulse(kind).duration;
            }
        }
        pending_.clear();
        return result;
    }

  private:
    std::map<unsigned, unsigned> owners_;
    std::map<unsigned, KeyFeedback> pending_;
    std::map<unsigned, double> hover_after_;
};
} // namespace framekeyboard
