#pragma once
#include "feedback.hpp"

#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// A key's pop as mono samples at `rate`, for an effect "a" to "f" of Haptic
// effect. Release is a little higher than press. Hover has none.
std::vector<float> key_sound(KeyFeedback kind, int rate, const std::string& effect);
// The sample gain for `volume`, a percentage of full loudness. Sound seems about
// half as loud 10 dB down, so 50 is .32 here, not .5.
float sound_gain(int volume);

// Plays pops through PipeWire, loaded at run time. Nothing is loaded and no
// thread starts until the first set_awake(true), and a system without
// PipeWire stays silent. Both calls are cheap enough for the VR frame.
class KeySounds {
  public:
    KeySounds();
    ~KeySounds();
    // The output stream runs only while awake. A sleeping speaker would swallow
    // the first pop, so it stays open while the keyboard can be typed on.
    void set_awake(bool awake);
    void play(KeyFeedback kind, const std::string& effect, int volume);

  private:
    struct Player;
    std::unique_ptr<Player> player_;
    bool awake_{};
};
} // namespace framekeyboard
