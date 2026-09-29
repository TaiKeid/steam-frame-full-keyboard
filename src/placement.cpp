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
Transform HorizonAlignment::update(const Transform& raw, double now) {
    // A local Z rotation changes roll without changing the panel normal, hence
    // preserves its heading and tilt. World Y projected onto right/up gives roll.
    const double vertical = std::hypot(raw[1][0], raw[1][1]);
    if (!std::isfinite(now) || !std::isfinite(vertical) || vertical < .001) {
        // A panel facing straight up/down has no well-defined horizon roll.
        reset();
        return raw;
    }
    const double roll = std::atan2(raw[1][0], raw[1][1]);
    const bool within = std::abs(roll) <= 5 * radians + 1e-9;
    if (within != within_threshold_) {
        within_threshold_ = within;
        from_correction_ = correction_;
        started_ = now;
    }
    const double t = std::clamp((now - started_) / .5, 0.0, 1.0);
    const double ease = t * t * (3 - 2 * t);
    // Track the live roll while easing in. On leaving the capture range, ease
    // the existing correction away instead of jumping back to the raw pose.
    correction_ = std::lerp(from_correction_, within ? -roll : 0.0, ease);
    const double c = std::cos(correction_), s = std::sin(correction_);
    auto result = raw;
    for (std::size_t row = 0; row < 3; ++row) {
        result[row][0] = raw[row][0] * c + raw[row][1] * s;
        result[row][1] = -raw[row][0] * s + raw[row][1] * c;
    }
    return result;
}
Transform compose(const Transform& a, const Transform& b) {
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                result[r][c] += a[r][k] * b[k][c];
            }
        }
        result[r][3] += a[r][3];
    }
    return result;
}
void PanelDrag::begin(const Transform& controller, const Transform& panel,
                      std::array<double, 3> ray_direction) {
    Transform inverse{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            inverse[r][c] = controller[c][r];
            inverse[r][3] -= controller[c][r] * controller[c][3];
        }
    }
    relative_ = compose(inverse, panel);
    const double length = std::hypot(ray_direction[0], ray_direction[1], ray_direction[2]);
    ray_direction_ = {0, 0, -1};
    if (std::isfinite(length) && length > .001) {
        for (std::size_t i = 0; i < 3; ++i) {
            ray_direction_[i] = ray_direction[i] / length;
        }
    }
    depth_ = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        depth_ += relative_[i][3] * ray_direction_[i];
    }
    // Do not snap an existing out-of-range panel when grabbed. Allow the stick
    // to bring it back toward the normal 20 cm to 3 m range, but not farther out.
    minimum_depth_ = std::min(.2, depth_);
    maximum_depth_ = std::max(3.0, depth_);
}
void PanelDrag::move_depth(double stick_y, double elapsed_seconds) {
    constexpr double dead_zone = .2, speed = .65;
    if (!std::isfinite(stick_y) || !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0 ||
        std::abs(stick_y) <= dead_zone) {
        return;
    }
    const double magnitude = (std::min(std::abs(stick_y), 1.0) - dead_zone) / (1 - dead_zone);
    // Cap elapsed time so a suspended/stalled frame cannot teleport the panel.
    const double delta = std::copysign(magnitude, stick_y) * speed * std::min(elapsed_seconds, .05);
    const double next = std::clamp(depth_ + delta, minimum_depth_, maximum_depth_);
    for (std::size_t i = 0; i < 3; ++i) {
        relative_[i][3] += ray_direction_[i] * (next - depth_);
    }
    depth_ = next;
}
Transform PanelDrag::update(const Transform& controller) const {
    return compose(controller, relative_);
}
void PanelPlacement::set_transform(const Transform& world) {
    anchor_ = world;
    x_ = y_ = z_ = pitch_ = yaw_ = roll_ = 0;
    ready_ = true;
}
void PanelPlacement::restore(const Transform& world, double width) {
    set_transform(world);
    width_ = width;
}
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
