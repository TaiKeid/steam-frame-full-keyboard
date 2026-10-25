#pragma once
#include "framekeyboard/layout.hpp"
#include "framekeyboard/placement.hpp"
#include <array>
#include <optional>

namespace framekeyboard {
// Angular travel between a controller component's released and current poses.
// Translation is deliberately ignored: the render model defines the button pivot.
double component_rotation_degrees(const Transform& released, const Transform& current);

// Recover one signed stick axis from a render component's tilt. Calibration
// uses synthetic neutral/full-up poses, so model orientation and handedness do
// not need hardcoded Euler angles.
class ComponentAxis {
  public:
    bool calibrate(const Transform& neutral, const Transform& positive);
    double read(const Transform& current) const;

  private:
    Transform neutral_{};
    std::array<double, 3> axis_{};
    double full_angle_{};
};

// Which way a thumbstick tilt scrolls in the dashboard, where SteamVR turns
// the stick into scroll steps by its own binding. It is learned from steps
// that arrive while the stick is clearly tilted; until then, right scrolls
// forward and up scrolls back, like a wheel. Forward is the positive step
// App::scroll takes.
class StickScrollDirection {
  public:
    void learn(double x, double y, double step);
    // +1 forward, -1 back, or 0 while the stick is too near center to step.
    int predict(double x, double y) const;

  private:
    std::array<int, 2> forward_{1, -1}; // Sign of a forward tilt on x, then y.
};
// Whether SteamVR's ticking scroll steps are wanted for an area with this
// room: toward a direction that can still move, or while the stick is
// centered and either direction can.
bool wants_scroll_steps(ScrollRoom room, int direction);

struct GripTransition {
    bool held{}, pressed{};
};
class GripLatch {
  public:
    // Frame's grip component has 9.5 degrees of travel. A small dead zone and
    // separate release threshold keep a resting finger from chattering capture.
    // Missing samples disarm it; startup/reconnect requires a release first.
    GripTransition update(std::optional<double> angle_degrees);
    void reset();

  private:
    bool armed_{}, held_{};
};
// Recover a line capture when overlay mouse-up is lost. An initial rest pose
// can be stale, so allow a short grace period before trusting repeated rest
// samples without a preceding held sample. Missing samples break that streak.
class TriggerReleaseGuard {
  public:
    void begin(double now);
    bool update(std::optional<double> angle_degrees, double now);

  private:
    double started_{};
    bool seen_held_{};
    std::optional<double> released_since_, sampled_at_;
};
} // namespace framekeyboard
