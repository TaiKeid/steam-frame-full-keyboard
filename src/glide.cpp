#include "framekeyboard/glide.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
// The release speed over the last 0.1 s, if at least min_glide pixels a
// second, decays with this time constant until it is slower than
// settled_speed. Its first second covers about 0.3 s of speed.
constexpr double glide_window = .1, min_glide = 300, settled_speed = 30, glide_decay = .35,
                 max_glide = 6000;
// A glide that runs past an end overshoots on a critically damped spring and
// settles back on the end, like iOS lists. Entering at most bounce_speed, it
// overshoots at most bounce_speed / (bounce_rate * e), about 65 pixels.
constexpr double bounce_rate = 11, bounce_speed = 2000;
} // namespace
void Glide::track(double now, double scroll) {
    std::erase_if(samples_, [&](const auto& sample) { return sample.first < now - glide_window; });
    samples_.emplace_back(now, scroll);
}
void Glide::release(double now, double scroll) {
    // A pause before releasing leaves nothing to glide.
    track(now, scroll);
    const auto [first_at, first] = samples_.front();
    samples_.clear();
    const double velocity = now - first_at > .01 ? (scroll - first) / (now - first_at) : 0;
    if (std::abs(velocity) >= min_glide) {
        motion_ = Motion{std::clamp(velocity, -max_glide, max_glide), now};
    }
}
bool Glide::step(double now, double& scroll, double limit) {
    // A release is timed when its event arrives, after the VR loop read the
    // frame's time, so a step can come no later than the release: skip it.
    if (!motion_ || now <= motion_->at) {
        return false;
    }
    const double seconds = std::min(now - motion_->at, .1);
    const double old = scroll;
    motion_->at = now;
    // Exact steps of x'' = -w²x - 2wx' from the end the list ran past.
    auto spring = [&](double t) {
        const double w = bounce_rate, e = std::exp(-w * t);
        const double x = scroll - motion_->end, v = motion_->velocity;
        scroll = motion_->end + (x + (v + w * x) * t) * e;
        motion_->velocity = (v - w * (v + w * x) * t) * e;
        if (std::abs(scroll - motion_->end) < .5 && std::abs(motion_->velocity) < settled_speed) {
            scroll = motion_->end;
            motion_.reset(); // Settled on the end.
        }
    };
    if (motion_->bounce) {
        spring(seconds);
    } else {
        // The exact distance under exponential decay, independent of frame rate.
        const double v = motion_->velocity, decay = std::exp(-seconds / glide_decay);
        const double next = scroll + v * glide_decay * (1 - decay);
        if (next < 0 || next > limit) {
            // Spring from the moment the glide reaches the end, at the capped
            // speed, so a fast fling or a late frame overshoots no more than a
            // slow one: solve x + v·τ(1 − e^(−t/τ)) = end.
            const double end = next < 0 ? 0 : limit;
            const double reached = std::clamp((end - scroll) / (v * glide_decay), 0.0, 1 - 1e-9);
            const double at = -glide_decay * std::log(1 - reached);
            scroll = end;
            motion_->bounce = true;
            motion_->end = end;
            motion_->velocity = std::clamp(v * (1 - reached), -bounce_speed, bounce_speed);
            spring(std::max(0.0, seconds - at));
        } else {
            scroll = next;
            motion_->velocity *= decay;
            if (std::abs(motion_->velocity) < settled_speed) {
                motion_.reset(); // Slowed to a stop.
            }
        }
    }
    return scroll != old;
}
void Glide::stop() {
    motion_.reset();
    samples_.clear();
}
} // namespace framekeyboard
