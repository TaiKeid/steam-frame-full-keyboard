#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
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
        // That title, and when (monotonic seconds) it was sampled and the
        // overlay names were last read. Kept in memory only, never logged.
        std::string title{};
        double now{}, names_read{};
    };
    // Gamescope renames a window's overlay soon after its title changes:
    // names read sooner may still list the old one.
    static constexpr double rename_wait = .3;
    void update(const Sample& sample) {
        if (sample.window != focused_) {
            // Only a move between two known windows is a click. Focus first
            // read, or read again after a reconnect or a hidden spell, is a
            // baseline: the user may be looking elsewhere.
            const bool pointed =
                sample.pointed && std::find(sample.visible.begin(), sample.visible.end(),
                                            *sample.pointed) != sample.visible.end();
            if (focused_ && sample.window && (sample.visible.size() == 1 || pointed)) {
                learn(sample.window,
                      sample.visible.size() == 1 ? sample.visible.front() : *sample.pointed);
            }
            focused_ = sample.window;
        }
        if (sample.window) {
            note_title(sample);
        }
        if (sample.window && sample.named && names_tell(sample)) {
            learn(sample.window, *sample.named);
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
    // and learned links and titles stay.
    void pause() {
        focused_ = 0;
        hidden_ = false;
    }
    bool hidden() const { return hidden_; }
    std::size_t links() const { return learned_.size(); } // for tests

  private:
    struct Title {
        std::string text;
        double changed{-1}; // when it last changed while known, -1 never
    };
    // Whether the overlay name matched tells which overlay is the focused
    // window's. Overlays are named by title alone, so a title another window
    // also has, such as two empty documents of one editor, names either. And
    // just after the window's own title changed, its overlay may still carry
    // the old one, so a name read then can belong to another window that had
    // the new title already. Empty titles are not compared: they name nothing.
    bool names_tell(const Sample& sample) const {
        if (sample.title.empty()) {
            return true;
        }
        const auto& seen = titles_.at(sample.window);
        if (seen.changed >= 0 && sample.names_read < seen.changed + rename_wait) {
            return false;
        }
        return std::none_of(titles_.begin(), titles_.end(), [&](const auto& entry) {
            return entry.first != sample.window && entry.second.text == sample.title;
        });
    }
    void note_title(const Sample& sample) {
        auto& seen = titles_[sample.window];
        if (seen.text != sample.title) {
            if (!seen.text.empty()) {
                seen.changed = sample.now;
            }
            seen.text = sample.title;
        }
        bound(titles_, sample.window);
    }
    // At most 16 entries, as windows come and go: the lowest X window id goes,
    // an older window, but never `keep`, the one just updated.
    template <class Map> static void bound(Map& map, std::uint32_t keep) {
        if (map.size() > 16) {
            const auto oldest = map.begin();
            map.erase(oldest->first != keep ? oldest : std::next(oldest));
        }
    }
    void learn(std::uint32_t window, std::uint32_t overlay) {
        learned_[window] = overlay;
        bound(learned_, window);
    }
    std::map<std::uint32_t, std::uint32_t> learned_;
    // Each window's last title by X window id: Gamescope's overlay names
    // carry no window id, so this tells when a name could be another's.
    std::map<std::uint32_t, Title> titles_;
    std::uint32_t focused_{};
    bool hidden_{};
};
// Whether Steam's dashboard has the keys, so nothing should be typed
// (docs/configuration.md). Gamescope's keyboard focus follows SteamVR's
// overlay input focus: a click on the dashboard gives it a Steam window, but
// using our keyboard soon moves it on to the focused app, as if that app had
// been chosen. So Steam's page sets this, and only something the user did
// elsewhere clears it: a trigger pull off the keyboard that leaves another
// window focused, or another app taking focus. Pure logic; the VR loop samples.
//
// A pull is seen as the trigger starts to rise, but SteamVR clicks only deep
// in the pull, so Steam's page can keep the keys for a moment after it. What
// the pull began on (SteamVR's hover target) says whether that is left over.
class DashboardFocus {
  public:
    static constexpr double settle = .3; // for a click to move Gamescope's focus
    // A pull on an app window: Steam's page seen this long after it means
    // the click moved nothing.
    static constexpr double window_click = 1;
    // A pull on the dashboard's bar waits this long for the app it chose to show.
    static constexpr double bar_click = 2;
    enum class Target {
        other,  // nothing known, as before targets were read
        page,   // Steam's page
        window, // an app window's overlay: the click chooses that window
        bar,    // the dashboard's bar or its popups: an app's icon, Steam's, or another button
    };
    // Who has Gamescope's keys: Steam's page; another Steam window, such as
    // the bar while it is pointed at or clicked, which is no choice made; or an app.
    enum class Keys { page, steam, app };
    // `app`: Gamescope's focused app window (GAMESCOPE_FOCUSED_WINDOW), 0
    // when unknown. `app_shown`: whether that app's window is in sight, in
    // the dashboard or floating in the world, read only while wants_visibility().
    void update(bool dashboard_visible, Keys keys, std::uint32_t app, double now, double sampled_at,
                std::optional<bool> app_shown = {}) {
        if (!dashboard_visible) {
            *this = {};
            return;
        }
        if (sampled_at <= 0) {
            return; // Unknown focus is not evidence that another app was chosen.
        }
        if (clicked_ && target_ == Target::bar && sampled_at > *clicked_ + bar_click) {
            clicked_.reset(); // The bar's click showed no app.
        }
        const bool after = clicked_ && sampled_at > *clicked_;
        if (keys == Keys::page) {
            focused_ = true;
            if (after && !left_over(sampled_at)) {
                clicked_.reset(); // The click stayed in the dashboard.
            }
        } else if (keys == Keys::steam) {
            left_page_ |= after;
        } else if (app && app != app_ && app_) {
            focused_ = false; // Another app took focus, such as one just launched.
        } else if (app && after && chose(now, app_shown)) {
            focused_ = false; // The click chose the app with the keys.
            clicked_.reset();
        } else if (app && after) {
            left_page_ = true;
        }
        // A Steam window's sample leaves the app for the next app sample to
        // compare, as the bar may have the keys while another app takes focus.
        if (app && keys != Keys::steam) {
            app_ = app;
        }
    }
    // A trigger pull while the laser was off our keyboard, and what it began on.
    void click(double now, Target target = Target::other) {
        clicked_ = now;
        target_ = target;
        left_page_ = false;
    }
    void reset() { *this = {}; }
    // Keep the dashboard's ownership decision, but not a pre-sleep click's
    // permission to clear it after wake.
    void suspend() { clicked_.reset(); }
    bool focused() const { return focused_; }
    bool click_pending() const { return clicked_.has_value(); } // for tests and tracing
    // A bar click chooses an app only while that app's window is in sight.
    bool wants_visibility() const { return focused_ && clicked_ && target_ == Target::bar; }

  private:
    // Steam's page after the pull, still there from before SteamVR clicked.
    bool left_over(double sampled_at) const {
        switch (target_) {
        case Target::window:
            return sampled_at <= *clicked_ + window_click;
        case Target::bar:
            return !left_page_; // The bar takes the keys first when SteamVR clicks it.
        default:
            return false;
        }
    }
    bool chose(double now, std::optional<bool> app_shown) const {
        if (target_ == Target::bar) {
            // Steam's icon or the bar's keyboard button leave the app hidden
            // behind Steam's page, where the keys fall back to it unseen.
            // Another app's icon moves Gamescope's focus too (above).
            return app_shown == true;
        }
        return now - *clicked_ >= settle;
    }
    std::optional<double> clicked_;
    Target target_{};
    std::uint32_t app_{};
    bool focused_{}, left_page_{};
};
} // namespace framekeyboard
