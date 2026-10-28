#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace framekeyboard {
// Whether the window with Gamescope keyboard focus is out of sight, so a
// panel Keep open holds would type into a window the user cannot see
// (docs/configuration.md). Gamescope names each window's SteamVR overlay
// gamescope.<display>.window.<N> without linking N to the X window, so the
// link is learned: a window that just took focus was clicked, so it is the
// one window overlay visible at that moment. Pure logic; the VR loop samples.
class FocusVisibility {
  public:
    struct Sample {
        std::uint32_t window{};             // focused X window, 0 when unknown
        std::optional<bool> app_visible;    // a Steam game's own overlay, when it has one
        std::vector<std::uint32_t> visible; // N of each visible window overlay
    };
    void update(const Sample& sample) {
        if (sample.window != focused_) {
            // The first sample is not a focus change: the user may be elsewhere.
            if (started_ && sample.visible.size() == 1) {
                learned_[sample.window] = sample.visible.front();
                if (learned_.size() > 16) {
                    learned_.erase(learned_.begin());
                }
            }
            focused_ = sample.window;
        }
        started_ = true;
        if (!sample.window) {
            hidden_ = false;
        } else if (sample.app_visible) {
            hidden_ = !*sample.app_visible;
        } else if (sample.visible.empty()) {
            hidden_ = true; // No app window is in sight at all.
        } else {
            const auto it = learned_.find(sample.window);
            // Unknown with several windows in sight: say nothing rather than guess.
            hidden_ = it != learned_.end() && std::find(sample.visible.begin(), sample.visible.end(),
                                                        it->second) == sample.visible.end();
        }
    }
    void reset() { *this = {}; }
    bool hidden() const { return hidden_; }

  private:
    std::map<std::uint32_t, std::uint32_t> learned_;
    std::uint32_t focused_{};
    bool started_{}, hidden_{};
};
} // namespace framekeyboard
