#include "framekeyboard/placement.hpp"
#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
constexpr double radians = 3.14159265358979323846 / 180.0;
using Rotation = std::array<std::array<double, 3>, 3>;
Rotation multiply(const Rotation& a, const Rotation& b) {
    Rotation result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            for (std::size_t k = 0; k < 3; ++k) {
                result[row][col] += a[row][k] * b[k][col];
            }
        }
    }
    return result;
}
// Flatten the heading so looking down or tilting one's head does not summon a
// slanted panel. When looking straight up/down, derive heading from head-right.
Rotation level_heading(const Transform& head) {
    double zx = head[0][2], zz = head[2][2];
    if (std::hypot(zx, zz) < .001) {
        zx = -head[2][0];
        zz = head[0][0];
    }
    const double length = std::hypot(zx, zz);
    if (length < .001) {
        return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    }
    zx /= length;
    zz /= length;
    return {{{zz, 0, zx}, {0, 1, 0}, {-zx, 0, zz}}};
}
} // namespace
void PanelPlacement::recenter(const Transform& head) {
    const auto rotation = level_heading(head);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            anchor_[row][col] = rotation[row][col];
        }
        anchor_[row][3] = head[row][3] - rotation[row][2] * .85 - rotation[row][1] * .25;
    }
    x_ = y_ = z_ = pitch_ = yaw_ = roll_ = 0;
    ready_ = true;
}
void PanelPlacement::adjust(PlacementAction action) {
    constexpr double step = .025, angle = 5 * radians;
    switch (action) {
    case PlacementAction::Left:
        x_ -= step;
        break;
    case PlacementAction::Right:
        x_ += step;
        break;
    case PlacementAction::Up:
        y_ += step;
        break;
    case PlacementAction::Down:
        y_ -= step;
        break;
    case PlacementAction::Nearer:
        z_ += step;
        break;
    case PlacementAction::Farther:
        z_ -= step;
        break;
    case PlacementAction::TiltUp:
        pitch_ -= angle;
        break;
    case PlacementAction::TiltDown:
        pitch_ += angle;
        break;
    case PlacementAction::TurnLeft:
        yaw_ -= angle;
        break;
    case PlacementAction::TurnRight:
        yaw_ += angle;
        break;
    case PlacementAction::RollLeft:
        roll_ += angle;
        break;
    case PlacementAction::RollRight:
        roll_ -= angle;
        break;
    case PlacementAction::Smaller:
        width_ -= .05;
        break;
    case PlacementAction::Larger:
        width_ += .05;
        break;
    case PlacementAction::FaceMe:
        break; // Needs a current headset pose; handled by face().
    }
    x_ = std::clamp(x_, -2.0, 2.0);
    y_ = std::clamp(y_, -1.5, 1.5);
    z_ = std::clamp(z_, -2.0, .5);
    pitch_ = std::clamp(pitch_, -85 * radians, 85 * radians);
    yaw_ = std::remainder(yaw_, 2 * 3.14159265358979323846);
    roll_ = std::remainder(roll_, 2 * 3.14159265358979323846);
    width_ = std::clamp(width_, .45, 2.0);
}
Transform PanelPlacement::transform() const {
    const Rotation pitch{
        {{1, 0, 0}, {0, std::cos(pitch_), -std::sin(pitch_)}, {0, std::sin(pitch_), std::cos(pitch_)}}};
    const Rotation yaw{
        {{std::cos(yaw_), 0, std::sin(yaw_)}, {0, 1, 0}, {-std::sin(yaw_), 0, std::cos(yaw_)}}};
    const Rotation roll{
        {{std::cos(roll_), -std::sin(roll_), 0}, {std::sin(roll_), std::cos(roll_), 0}, {0, 0, 1}}};
    const auto rotation = multiply(multiply(yaw, pitch), roll);
    Transform result = anchor_;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            result[row][col] = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                result[row][col] += anchor_[row][k] * rotation[k][col];
            }
        }
        result[row][3] += anchor_[row][0] * x_ + anchor_[row][1] * y_ + anchor_[row][2] * z_;
    }
    return result;
}
void PanelPlacement::face(const Transform& head) {
    if (!ready_) {
        return;
    }
    const auto current = transform();
    auto direction = head;
    direction[0][2] = head[0][3] - current[0][3];
    direction[2][2] = head[2][3] - current[2][3];
    const auto rotation = level_heading(direction);
    // Retain position and size. Only yaw changes; pitch/roll are leveled.
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            anchor_[row][col] = rotation[row][col];
        }
        anchor_[row][3] = current[row][3];
    }
    x_ = y_ = z_ = pitch_ = yaw_ = roll_ = 0;
}
} // namespace framekeyboard
