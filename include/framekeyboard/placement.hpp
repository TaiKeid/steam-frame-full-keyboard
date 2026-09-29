#pragma once
#include <array>

namespace framekeyboard {
// Row-major rigid transform. Its columns are right, up, toward viewer, position.
using Transform = std::array<std::array<double, 4>, 3>;
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
    double width() const { return width_; }
    bool ready() const { return ready_; }

  private:
    Transform anchor_{};
    double x_{}, y_{}, z_{}, pitch_{}, yaw_{}, roll_{};
    double width_{1.15};
    bool ready_{};
};
} // namespace framekeyboard
