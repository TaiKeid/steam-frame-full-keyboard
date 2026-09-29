#include "framekeyboard/grip.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace framekeyboard {
double component_rotation_degrees(const Transform& released, const Transform& current) {
    // trace(R_rest^T R_current) = 1 + 2*cos(angle). Clamp float rounding at rest.
    double trace = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            trace += released[r][c] * current[r][c];
        }
    }
    return std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0)) * 180 / std::numbers::pi;
}
GripTransition GripLatch::update(std::optional<double> angle_degrees) {
    if (!angle_degrees || !std::isfinite(*angle_degrees)) {
        reset();
        return {};
    }
    if (!armed_) {
        armed_ = *angle_degrees < .5;
        return {};
    }
    const bool held = *angle_degrees >= (held_ ? .5 : 1.0);
    const GripTransition state{held, held && !held_};
    held_ = held;
    return state;
}
void GripLatch::reset() {
    held_ = false;
    armed_ = false;
}
} // namespace framekeyboard
