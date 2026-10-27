#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace framekeyboard {
// The panel's entrance and exit. It fades in while rising into place and
// fades out while settling slightly, and leaves quicker than it arrives. Only
// the overlay's opacity and position change, so no frame is repainted. A
// show during an exit, or a hide during an entrance, turns around from the
// opacity already shown instead of jumping.
class PanelMotion {
  public:
    static constexpr double enter_seconds = .18, exit_seconds = .12;
    static constexpr double enter_rise = .02, exit_sink = .01; // Metres.

    void show(double now) { start(true, now); }
    void hide(double now) { start(false, now); }
    // Drops any exit in progress: the panel is gone at once.
    void cut() { *this = {}; }

    double alpha(double now) const {
        return entering_ ? ease_out(progress(now)) : 1 - ease_in(progress(now));
    }
    // Metres along the panel's up axis; below its place while moving.
    double offset(double now) const {
        return entering_ ? -enter_rise * (1 - ease_out(progress(now)))
                         : -exit_sink * ease_in(progress(now));
    }
    bool animating(double now) const { return started_ && progress(now) < 1; }
    // An exit has finished; the overlay can be hidden.
    bool gone(double now) const { return started_ && !entering_ && progress(now) >= 1; }
    bool leaving() const { return started_ && !entering_; }

  private:
    void start(bool entering, double now) {
        if (started_ && entering == entering_) {
            return;
        }
        // Turning around: begin where the current opacity puts the new curve.
        const double shown = started_ ? alpha(now) : entering ? 0 : 1;
        entering_ = entering;
        const double from = entering ? inverse_ease_out(shown) : inverse_ease_in(1 - shown);
        started_ = now - from * duration();
    }
    double duration() const { return entering_ ? enter_seconds : exit_seconds; }
    double progress(double now) const {
        return started_ ? std::clamp((now - *started_) / duration(), 0.0, 1.0) : 1.0;
    }
    // Cubic ease-out to arrive, ease-in to leave.
    static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
    static double ease_in(double t) { return t * t * t; }
    static double inverse_ease_out(double v) { return 1 - std::cbrt(1 - std::clamp(v, 0.0, 1.0)); }
    static double inverse_ease_in(double v) { return std::cbrt(std::clamp(v, 0.0, 1.0)); }

    std::optional<double> started_;
    bool entering_{true};
};
} // namespace framekeyboard
