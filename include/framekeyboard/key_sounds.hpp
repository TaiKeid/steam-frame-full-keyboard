#pragma once
#include "feedback.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace framekeyboard {
// A key's pop as mono samples at `rate`, for an effect "a" to "f" of Haptic
// effect. Release is a little higher than press. Hover has none.
std::vector<float> key_sound(KeyFeedback kind, int rate, const std::string& effect);
// The sample gain for `volume`, a percentage of full loudness. Sound seems about
// half as loud 10 dB down, so 50 is .32 here, not .5.
float sound_gain(int volume);

// VR-thread policy, separate from playback: a pointer prewarms audio before
// the first key, and a short departure never restarts it between sounds.
class KeySoundActivity {
  public:
    static constexpr double leave_delay = 2;
    void pointer(std::uint64_t id, bool inside, double now) {
        if (inside) {
            pointers_.insert(id);
            left_since_.reset();
        } else if (pointers_.erase(id) && pointers_.empty() && awake_) {
            left_since_ = now;
        }
    }
    void clear_pointers(double now) {
        if (!pointers_.empty()) {
            pointers_.clear();
            if (awake_) {
                left_since_ = now;
            }
        }
    }
    bool contains(std::uint64_t id) const { return pointers_.contains(id); }
    bool has_pointers() const { return !pointers_.empty(); }
    void reset() {
        pointers_.clear();
        left_since_.reset();
        awake_ = false;
    }
    bool update(double now, bool visible, bool enabled, bool worn = true) {
        if (!visible || !worn) {
            reset();
        } else if (!enabled) {
            // Unmuting while a pointer is already inside should prewarm too.
            awake_ = false;
            left_since_.reset();
        } else if (!pointers_.empty()) {
            awake_ = true;
            left_since_.reset();
        } else if (awake_) {
            if (!left_since_) {
                left_since_ = now;
            }
            if (now >= *left_since_ + leave_delay) {
                awake_ = false;
                left_since_.reset();
            }
        }
        return awake_;
    }

  private:
    std::set<std::uint64_t> pointers_;
    std::optional<double> left_since_;
    bool awake_{};
};

// Plays pops through PipeWire, loaded at run time. Nothing is loaded and no
// thread starts until the first set_awake(true), and a system without
// PipeWire stays silent. Both calls are cheap enough for the VR frame.
class KeySounds {
  public:
    KeySounds();
    ~KeySounds();
    // While awake the stream stays warm between sounds. The caller applies
    // pointer activity, mute and visibility policy; false closes the connection.
    void set_awake(bool awake);
    void play(KeyFeedback kind, const std::string& effect, int volume);

  private:
    struct Player;
    std::unique_ptr<Player> player_;
    bool awake_{};
};
} // namespace framekeyboard
