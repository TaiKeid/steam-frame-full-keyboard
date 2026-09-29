#pragma once
#include <array>

namespace framekeyboard {
// Row-major rigid transform. Its columns are right, up, toward viewer, position.
using Transform = std::array<std::array<double, 4>, 3>;
// Roll-only horizon assistance. Feed the uncorrected pose each frame so the
// animation never feeds its own correction back into the controller grab.
class HorizonAlignment {
  public:
    Transform update(const Transform& raw, double now);
    void reset() { *this = {}; }
    bool animating() const { return animating_; }

  private:
    double started_{}, from_correction_{}, correction_{};
    bool within_threshold_{}, animating_{};
};
enum class PlacementAction {
    Left,
    Right,
    Up,
    Down,
    Nearer,
    Farther,
    TiltUp,
    TiltDown,
    TurnLeft,
    TurnRight,
    RollLeft,
    RollRight,
    Smaller,
    Larger,
    FaceMe
};
class PanelPlacement {
  public:
    void recenter(const Transform& head);
    void adjust(PlacementAction action);
    void face(const Transform& head);
    Transform transform() const;
    void set_transform(const Transform& world);
    void restore(const Transform& world, double width);
    double width() const { return width_; }
    bool ready() const { return ready_; }

  private:
    Transform anchor_{};
    double x_{}, y_{}, z_{}, pitch_{}, yaw_{}, roll_{};
    double width_{1.15};
    bool ready_{};
};
// Store the initial controller-to-panel transform, so grabbing never snaps the
// panel center to the ray. Later updates preserve the exact grabbed point.
class PanelDrag {
  public:
    void begin(const Transform& controller, const Transform& panel,
               std::array<double, 3> ray_direction = {0, 0, -1});
    void move_depth(double stick_y, double elapsed_seconds);
    Transform update(const Transform& controller) const;

  private:
    Transform relative_{};
    std::array<double, 3> ray_direction_{};
    double depth_{}, minimum_depth_{}, maximum_depth_{};
};
} // namespace framekeyboard
