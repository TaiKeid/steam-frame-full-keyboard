#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace framekeyboard {
// Kinetic scrolling along one axis after a laser drag, shared by the emoji
// grid and the Japanese candidates: a quick drag keeps gliding after the
// release, slowing down like a phone list, and bounces back from an end it
// runs past. The owner keeps the scroll offset, from 0 to its limit.
class Glide {
  public:
    // A dragged offset; the release keeps the speed of its last tenth of a second.
    void track(double now, double scroll);
    // The drag ended at `scroll`: glides on if it was quick enough.
    void release(double now, double scroll);
    // Moves a gliding offset on to `now`; true when it changed.
    bool step(double now, double& scroll, double limit);
    // Ends any glide and forgets the drag. A caller caught past an end
    // clamps its offset back to that end.
    void stop();
    bool active() const { return motion_.has_value(); }

  private:
    struct Motion {
        double velocity{}, at{}; // Scroll pixels per second, and when last moved.
        bool bounce{};           // Past an end, springing back to it.
        double end{};
    };
    std::vector<std::pair<double, double>> samples_; // Recent (time, scroll) while dragging.
    std::optional<Motion> motion_;
};
} // namespace framekeyboard
