#include "framekeyboard/placement.hpp"
#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
constexpr double radians = 3.14159265358979323846 / 180.0;
using Rotation = std::array<std::array<double, 3>, 3>;
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
// Unit quaternions (w, x, y, z) let corrections about different axes blend
// along the shortest rotation.
Quaternion slerp(Quaternion from, const Quaternion& to, double t) {
    double dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    if (dot < 0) {
        for (auto& value : from) {
            value = -value;
        }
        dot = -dot;
    }
    Quaternion result{};
    // Linear blending only where the angle is negligible; slerp elsewhere keeps
    // the eased angle exactly proportional, including for small corrections.
    if (dot > 1 - 1e-12) {
        for (std::size_t i = 0; i < 4; ++i) {
            result[i] = from[i] + (to[i] - from[i]) * t;
        }
    } else {
        const double theta = std::acos(dot), sine = std::sin(theta);
        const double a = std::sin((1 - t) * theta) / sine, b = std::sin(t * theta) / sine;
        for (std::size_t i = 0; i < 4; ++i) {
            result[i] = from[i] * a + to[i] * b;
        }
    }
    const double length = std::sqrt(result[0] * result[0] + result[1] * result[1] +
                                    result[2] * result[2] + result[3] * result[3]);
    for (auto& value : result) {
        value /= length;
    }
    return result;
}
double quaternion_angle(const Quaternion& a, const Quaternion& b) {
    const double dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::min(1.0, dot));
}
Rotation rotation(const Quaternion& q) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
             {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
             {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}
} // namespace
void HorizonAlignment::retarget(double now) {
    from_ = correction_;
    started_ = now;
}
Transform HorizonAlignment::update(const Transform& raw, double now, bool grabbed, bool enabled,
                                   const std::optional<Transform>& reference) {
    if (grabbed || !enabled) {
        // Preserve the exact pose while moving or with assistance disabled.
        // Reset the easing clock so release or enabling starts smoothly.
        reset();
        return raw;
    }
    // Relative to the reference frame B split the panel's rotation as yaw about
    // up, then pitch about the level axis square to the long edge, then roll
    // about the long edge itself: R = B Ry(yaw) Rz(pitch) Rx(roll). Roll is the
    // desk tilt and is always kept; yaw and pitch are removed, so the long edge
    // lands on B's right edge. Without a reference, B is the room and the yaw
    // the user chose is kept. Turning about the panel normal instead would level
    // the edge but leave the keyboard twisted after a hand tilt.
    Rotation basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (reference) {
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                basis[r][c] = (*reference)[r][c];
            }
        }
    }
    Rotation relative{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                relative[r][c] += basis[k][r] * raw[k][c];
            }
        }
    }
    const double horizontal = std::hypot(relative[0][0], relative[2][0]);
    if (!std::isfinite(now) || !std::isfinite(horizontal) || horizontal < .001) {
        // A long edge along the reference up has no well-defined pitch.
        reset();
        return raw;
    }
    const double pitch = std::atan2(relative[1][0], horizontal);
    const double yaw = std::atan2(-relative[2][0], relative[0][0]);
    // The correction in the reference frame, then in the room: B Q B^T.
    auto turn_y = [](double a) {
        return Rotation{{{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}}};
    };
    auto turn_z = [](double a) {
        return Rotation{{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}};
    };
    auto product = [](const Rotation& a, const Rotation& b) {
        Rotation result{};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                for (std::size_t k = 0; k < 3; ++k) {
                    result[r][c] += a[r][k] * b[k][c];
                }
            }
        }
        return result;
    };
    auto correction = product(turn_z(-pitch), turn_y(-yaw));
    if (!reference) {
        correction = product(turn_y(yaw), correction); // No yaw reference: keep the heading.
    }
    Rotation transposed{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            transposed[r][c] = basis[c][r];
        }
    }
    const auto world = product(product(basis, correction), transposed);
    // As one rotation about a fixed axis, so easing follows the shortest path.
    const std::array<double, 3> twice_sine_axis{world[2][1] - world[1][2], world[0][2] - world[2][0],
                                                world[1][0] - world[0][1]};
    const double twice_sine = std::hypot(twice_sine_axis[0], twice_sine_axis[1], twice_sine_axis[2]);
    const double angle = std::atan2(twice_sine / 2, (world[0][0] + world[1][1] + world[2][2] - 1) / 2);
    std::array<double, 3> axis{};
    if (twice_sine > 1e-12) {
        for (std::size_t r = 0; r < 3; ++r) {
            axis[r] = twice_sine_axis[r] / twice_sine;
        }
    }
    const double roll = angle; // Lean and twist together, for the capture range.
    const bool within = std::abs(roll) <= 5 * radians + 1e-9;
    if (within != within_threshold_) {
        within_threshold_ = within;
        retarget(now);
    }
    // The full correction, or none outside the capture range, as a quaternion.
    const Quaternion target =
        within ? Quaternion{std::cos(angle / 2), axis[0] * std::sin(angle / 2),
                            axis[1] * std::sin(angle / 2), axis[2] * std::sin(angle / 2)}
               : Quaternion{1, 0, 0, 0};
    const double t = std::clamp((now - started_) / .5, 0.0, 1.0);
    const double ease = t * t * (3 - 2 * t);
    // Track the live target while easing in. Leaving the range or changing the
    // reference eases from the correction already shown, never jumping.
    correction_ = slerp(from_, target, ease);
    animating_ = t < 1 && quaternion_angle(from_, target) > 1e-6;
    const auto turn = rotation(correction_);
    auto result = raw;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            result[r][c] = turn[r][0] * raw[0][c] + turn[r][1] * raw[1][c] + turn[r][2] * raw[2][c];
        }
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
std::optional<Transform> dashboard_anchor(const Transform& scaled_bottom) {
    for (const auto& row : scaled_bottom) {
        for (double value : row) {
            if (!std::isfinite(value)) {
                return {};
            }
        }
        if (std::abs(row[3]) >= 1000) {
            return {};
        }
    }
    // Remove overlay scale without flattening pitch. The normal determines
    // facing direction; rebuild an orthogonal right/up basis around it.
    std::array<double, 3> normal{}, right{}, up{};
    const double normal_length =
        std::hypot(scaled_bottom[0][2], scaled_bottom[1][2], scaled_bottom[2][2]);
    if (normal_length < .001) {
        return {};
    }
    double projection = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        normal[r] = scaled_bottom[r][2] / normal_length;
        projection += scaled_bottom[r][0] * normal[r];
    }
    for (std::size_t r = 0; r < 3; ++r) {
        right[r] = scaled_bottom[r][0] - projection * normal[r];
    }
    const double right_length = std::hypot(right[0], right[1], right[2]);
    if (right_length < .001) {
        return {};
    }
    for (auto& value : right) {
        value /= right_length;
    }
    up = {normal[1] * right[2] - normal[2] * right[1], normal[2] * right[0] - normal[0] * right[2],
          normal[0] * right[1] - normal[1] * right[0]};
    double up_projection = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        up_projection += up[r] * scaled_bottom[r][1];
    }
    if (up_projection < .001) {
        return {}; // Reject degenerate or mirrored bases.
    }
    Transform anchor = scaled_bottom;
    for (std::size_t r = 0; r < 3; ++r) {
        anchor[r][0] = right[r];
        anchor[r][1] = up[r];
        anchor[r][2] = normal[r];
    }
    return anchor;
}
void DashboardAnchor::restore(std::optional<Transform> anchor, std::optional<Transform> bar,
                              bool full_rotation) {
    anchor_ = anchor;
    bar_ = bar;
    full_rotation_ = full_rotation;
}
std::optional<Transform> DashboardAnchor::update(std::optional<Transform> main,
                                                 std::optional<Transform> bar) {
    if (!main && !bar) {
        return {};
    }
    if (anchor_ && bar_ && bar) {
        auto current = *bar;
        if (!full_rotation_) {
            // Old saves contain yaw-only bar poses. Apply their last translation
            // and yaw delta once, then adopt full rotation without a tilt jump.
            const auto heading = level_heading(current);
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 3; ++c) {
                    current[r][c] = heading[r][c];
                }
            }
        }
        if (current != *bar_) {
            anchor_ = move_with_dashboard(*anchor_, *bar_, current);
        }
    } else if (main) {
        anchor_ = main;
    } else {
        // A first launch directly into an app has no calibrated main-tab mount.
        // Use the live bar instead of an invisible tab's stale room position.
        anchor_ = bar;
    }
    // Keep one reference across tab switches: the bar has its own fixed tilt,
    // so replacing its carried anchor with a different tab's basis could snap.
    bar_ = bar;
    full_rotation_ = true;
    return anchor_;
}
namespace {
// Below this the panel counts as flat: the radius would be kilometers.
constexpr double flat_curvature = 1e-6;
constexpr double pi = 3.14159265358979323846;
// A curved panel's arc in panel space: radius r about an axis parallel to up
// through (0, *, r), so the middle stays at the origin and the ends come
// toward the viewer. `angle` is the turn from the middle, right positive.
struct Arc {
    double radius{};
    double angle(double right) const { return right / radius; }
};
std::optional<Arc> arc(double width, double curvature) {
    if (!(curvature > flat_curvature) || !(width > 0)) {
        return {};
    }
    return Arc{width / (2 * pi * curvature)};
}
} // namespace
Transform curve_frame(const Transform& panel, double width, double curvature, double right, double up) {
    Transform local{{{1, 0, 0, right}, {0, 1, 0, up}, {0, 0, 1, 0}}};
    if (const auto bend = arc(width, std::min(curvature, maximum_curvature))) {
        const double a = bend->angle(right), c = std::cos(a), s = std::sin(a);
        // Columns: along the arc, up, toward the axis (the viewer's side).
        local = {{{c, 0, -s, bend->radius * s}, {0, 1, 0, up}, {s, 0, c, bend->radius * (1 - c)}}};
    }
    return compose(panel, local);
}
double curve_depth(double width, double curvature) {
    const auto bend = arc(width, std::min(curvature, maximum_curvature));
    return bend ? bend->radius * (1 - std::cos(bend->angle(width / 2))) : 0;
}
double curvature_for_depth(double width, double depth) {
    if (!(depth > 0) || !(width > 0)) {
        return 0;
    }
    if (depth >= curve_depth(width, maximum_curvature)) {
        return maximum_curvature;
    }
    // The depth grows steadily with the curvature up to the maximum.
    double low = 0, high = maximum_curvature;
    for (int i = 0; i < 48; ++i) {
        const double middle = (low + high) / 2;
        (curve_depth(width, middle) < depth ? low : high) = middle;
    }
    return (low + high) / 2;
}
std::optional<PanelHit> panel_ray_hit(const Transform& panel, double width, double height_over_width,
                                      const std::array<double, 3>& origin,
                                      const std::array<double, 3>& direction, double curvature) {
    auto column = [&](std::size_t c) {
        return std::array<double, 3>{panel[0][c], panel[1][c], panel[2][c]};
    };
    auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto right = column(0), up = column(1), normal = column(2), center = column(3);
    if (const auto bend = arc(width, std::min(curvature, maximum_curvature))) {
        // Into panel space, where the face is x² + (z − r)² = r².
        const std::array<double, 3> offset{origin[0] - center[0], origin[1] - center[1],
                                           origin[2] - center[2]};
        const std::array<double, 3> o{dot(offset, right), dot(offset, up), dot(offset, normal)};
        const std::array<double, 3> d{dot(direction, right), dot(direction, up), dot(direction, normal)};
        const double r = bend->radius;
        const double a = d[0] * d[0] + d[2] * d[2];
        const double b = 2 * (o[0] * d[0] + (o[2] - r) * d[2]);
        const double c = o[0] * o[0] + (o[2] - r) * (o[2] - r) - r * r;
        const double discriminant = b * b - 4 * a * c;
        if (!(a > 1e-12) || !(discriminant >= 0)) {
            return {}; // Along the axis, or past the cylinder.
        }
        const double root = std::sqrt(discriminant);
        for (const double t : {(-b - root) / (2 * a), (-b + root) / (2 * a)}) {
            if (!(t >= 0)) {
                continue;
            }
            const double x = o[0] + t * d[0], y = o[1] + t * d[1], z = o[2] + t * d[2];
            const double angle = std::atan2(x, r - z);
            // Only the panel's own arc, and only its face toward the viewer,
            // whose normal points at the axis: (−sin, 0, cos).
            if (std::abs(angle) * r > width / 2 ||
                !(-std::sin(angle) * d[0] + std::cos(angle) * d[2] < -1e-6)) {
                continue;
            }
            PanelHit hit;
            hit.distance = t;
            hit.u = angle * r / width + .5;
            hit.v = .5 - y / (width * height_over_width);
            if (hit.v < 0 || hit.v > 1) {
                continue;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                hit.point[i] = origin[i] + direction[i] * t;
            }
            return hit;
        }
        return {};
    }
    // Only the face toward the viewer (+z) is visible; a ray from behind misses.
    const double approach = dot(direction, normal);
    if (!(approach < -1e-6) || width <= 0) {
        return {};
    }
    const std::array<double, 3> to_center{center[0] - origin[0], center[1] - origin[1],
                                          center[2] - origin[2]};
    const double distance = dot(to_center, normal) / approach;
    if (!std::isfinite(distance) || distance < 0) {
        return {};
    }
    PanelHit hit;
    hit.distance = distance;
    for (std::size_t i = 0; i < 3; ++i) {
        hit.point[i] = origin[i] + direction[i] * distance;
    }
    const std::array<double, 3> offset{hit.point[0] - center[0], hit.point[1] - center[1],
                                       hit.point[2] - center[2]};
    hit.u = dot(offset, right) / width + .5;
    hit.v = .5 - dot(offset, up) / (width * height_over_width);
    if (hit.u < 0 || hit.u > 1 || hit.v < 0 || hit.v > 1) {
        return {};
    }
    return hit;
}
Transform move_with_dashboard(const Transform& panel, const Transform& previous,
                              const Transform& current) {
    // Invert only the rigid anchor, never Steam's scaled overlay matrix.
    Transform inverse{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            inverse[r][c] = previous[c][r];
            inverse[r][3] -= previous[c][r] * previous[c][3];
        }
    }
    return compose(current, compose(inverse, panel));
}
Transform turn_with_dashboard(const Transform& facing, const Transform& previous,
                              const Transform& current, const Transform& panel) {
    // Headings of the toward columns; a dashboard facing straight up or down has none.
    auto heading = [](const Transform& pose) -> std::optional<double> {
        if (std::hypot(pose[0][2], pose[2][2]) < .001) {
            return {};
        }
        return std::atan2(pose[0][2], pose[2][2]);
    };
    const auto from = heading(previous), to = heading(current);
    const double turn = from && to ? *to - *from : 0;
    const double c = std::cos(turn), s = std::sin(turn);
    // Turn the toward column about room up; right stays up x toward.
    const double x = c * facing[0][2] + s * facing[2][2], z = -s * facing[0][2] + c * facing[2][2];
    return Transform{{{z, 0, x, panel[0][3]}, {0, 1, 0, panel[1][3]}, {-x, 0, z, panel[2][3]}}};
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
    world_ = world;
    ready_ = true;
}
void PanelPlacement::restore(const Transform& world, double width) {
    set_transform(world);
    set_width(width);
}
void PanelPlacement::set_width(double width) {
    width_ = std::clamp(width, minimum_width, maximum_width);
}
void PanelPlacement::set_curvature(double curvature) {
    curvature_ = std::isfinite(curvature) ? std::clamp(curvature, 0.0, maximum_curvature) : 0;
}
namespace {
double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
} // namespace
void PanelBend::begin(const Transform& panel, double width, double curvature,
                      const std::array<double, 3>& handle, const std::array<double, 3>& hand,
                      const std::array<double, 3>& origin, const std::array<double, 3>& direction) {
    for (std::size_t r = 0; r < 3; ++r) {
        normal_[r] = panel[r][2];
    }
    start_ = hand;
    handle_ = handle;
    origin_ = origin;
    width_ = width;
    depth_ = curve_depth(width, curvature);
    // The plane's own normal: the laser's direction without its part along
    // the face's normal. Shorter than 0.2 (within about 12 degrees of the
    // normal), the plane would be met only at a grazing angle.
    const double length = std::sqrt(dot3(direction, direction));
    std::array<double, 3> across{};
    for (std::size_t r = 0; r < 3; ++r) {
        across[r] = length > 0 ? direction[r] / length : 0;
    }
    const double along = dot3(across, normal_);
    for (std::size_t r = 0; r < 3; ++r) {
        across[r] -= along * normal_[r];
    }
    const double off_normal = std::sqrt(dot3(across, across));
    twist_ = off_normal >= .2;
    if (twist_) {
        for (std::size_t r = 0; r < 3; ++r) {
            across_[r] = across[r] / off_normal;
        }
        const auto start = swept(direction);
        twist_ = start.has_value();
        swept_ = start.value_or(0);
    }
}
std::optional<double> PanelBend::swept(const std::array<double, 3>& direction) const {
    const double length = std::sqrt(dot3(direction, direction));
    const double approach = dot3(direction, across_);
    // Grazing: under about 3 degrees, a tremble would sweep meters.
    if (!(length > 0) || !(std::abs(approach) >= .05 * length)) {
        return {};
    }
    std::array<double, 3> to_handle{};
    for (std::size_t r = 0; r < 3; ++r) {
        to_handle[r] = handle_[r] - origin_[r];
    }
    const double t = dot3(to_handle, across_) / approach;
    if (!(t >= 0)) {
        return {};
    }
    double result = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        result += (origin_[r] + direction[r] * t - handle_[r]) * normal_[r];
    }
    return result;
}
std::optional<double> PanelBend::update(const std::array<double, 3>& hand,
                                        const std::array<double, 3>& direction) const {
    double pulled = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        pulled += (hand[r] - start_[r]) * normal_[r];
    }
    if (twist_) {
        const auto now = swept(direction);
        if (!now) {
            return {};
        }
        pulled += *now - swept_;
    }
    return curvature_for_depth(width_, depth_ + pulled);
}
void PanelPlacement::recenter(const Transform& head, const Transform* dashboard_bottom,
                              double height_over_width) {
    const auto rotation = level_heading(dashboard_bottom ? *dashboard_bottom : head);
    // Tilt the top edge away from the viewer: 50 degrees from upright,
    // leaving the typing surface 40 degrees above a horizontal desk.
    const double pitch = -50 * radians;
    const double c = std::cos(pitch), s = std::sin(pitch);
    for (std::size_t row = 0; row < 3; ++row) {
        world_[row][0] = rotation[row][0];
        world_[row][1] = rotation[row][1] * c + rotation[row][2] * s;
        world_[row][2] = -rotation[row][1] * s + rotation[row][2] * c;
        // Leave 6 cm between the dashboard bottom and the keyboard's top edge,
        // with the keyboard center 24 cm toward the viewer.
        world_[row][3] = dashboard_bottom
                             ? (*dashboard_bottom)[row][3] + rotation[row][2] * .24 -
                                   rotation[row][1] * (.06 + width_ * height_over_width * c / 2)
                             : head[row][3] - rotation[row][2] * .85 - rotation[row][1] * .65;
    }
    ready_ = true;
}
} // namespace framekeyboard
