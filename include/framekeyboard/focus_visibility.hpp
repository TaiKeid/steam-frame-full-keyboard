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
// one window overlay visible at that moment, or, with several in sight, the
// one the laser points at. Pure logic; the VR loop samples.
class FocusVisibility {
  public:
    struct Sample {
        std::uint32_t window{};             // focused X window, 0 when unknown
        std::optional<bool> app_visible;    // a Steam game's own overlay, when it has one
        std::vector<std::uint32_t> visible; // N of each visible window overlay
        // N of the visible window overlay the dashboard laser points at, read
        // only when focus just moved and several are in sight.
        std::optional<std::uint32_t> pointed{};
        // N of the one window overlay named after the focused window's title:
        // Gamescope's own link, which takes precedence over a learned one.
        std::optional<std::uint32_t> named{};
    };
    void update(const Sample& sample) {
        if (sample.window != focused_) {
            // Only a move between two known windows is a click. Focus first
            // read, or read again after a reconnect or a hidden spell, is a
            // baseline: the user may be looking elsewhere.
            const bool pointed =
                sample.pointed && std::find(sample.visible.begin(), sample.visible.end(),
                                            *sample.pointed) != sample.visible.end();
            if (focused_ && sample.window && (sample.visible.size() == 1 || pointed)) {
                learned_[sample.window] =
                    sample.visible.size() == 1 ? sample.visible.front() : *sample.pointed;
                if (learned_.size() > 16) {
                    learned_.erase(learned_.begin());
                }
            }
            focused_ = sample.window;
        }
        if (sample.window && sample.named) {
            learned_[sample.window] = *sample.named;
        }
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
    // Not sampled while the panel is hidden: the next sample is a baseline,
    // and learned links stay.
    void pause() {
        focused_ = 0;
        hidden_ = false;
    }
    bool hidden() const { return hidden_; }

  private:
    std::map<std::uint32_t, std::uint32_t> learned_;
    std::uint32_t focused_{};
    bool hidden_{};
};
// Whether Steam's dashboard has the keys, so nothing should be typed
// (docs/configuration.md). Gamescope's keyboard focus follows SteamVR's
// overlay input focus: a click on the dashboard gives it a Steam window, but
// using our keyboard soon moves it on to the focused app, as if that app had
// been chosen. So a Steam window sets this, and only something the user did
// elsewhere clears it: a trigger pull off the keyboard that leaves another
// window focused, or another app taking focus. Pure logic; the VR loop samples.
class DashboardFocus {
  public:
    static constexpr double settle = .3; // for a click to move Gamescope's focus
    // `steam`: the keyboard focus is a Steam window; `app`: Gamescope's
    // focused app window (GAMESCOPE_FOCUSED_WINDOW), 0 when unknown.
    void update(bool dashboard_visible, bool steam, std::uint32_t app, double now) {
        if (!dashboard_visible) {
            *this = {};
            return;
        }
        if (steam) {
            focused_ = true;
            clicked_.reset();
        } else if (app != app_ && app_) {
            focused_ = false; // Another app took focus, such as one just launched.
        } else if (clicked_ && now - *clicked_ >= settle) {
            focused_ = false; // The click landed on an app window.
            clicked_.reset();
        }
        app_ = app;
    }
    // A trigger pull while the laser was off our keyboard.
    void click(double now) {
        if (focused_) {
            clicked_ = now;
        }
    }
    void reset() { *this = {}; }
    bool focused() const { return focused_; }

  private:
    std::optional<double> clicked_;
    std::uint32_t app_{};
    bool focused_{};
};
} // namespace framekeyboard
