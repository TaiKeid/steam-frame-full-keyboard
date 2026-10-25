#include "framekeyboard/resizer.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
// Bracket shape in corner coordinates (meters): `a` runs outward from the
// case's side edge, `b` downward from its bottom edge, both 0 at the sharp
// corner. Like Steam's window corners, the arc is concentric with the case's
// rounded corner a few millimeters out, and short legs continue along the
// side and bottom edges. Only the radius follows the keyboard's size.
constexpr double gap = .0055, leg_length = .014, stroke = .002; // Gap to the line's middle; half width.
constexpr double pixels_per_meter = ResizerShape::pixels_per_meter;

Vector3 column(const Transform& t, std::size_t c) {
    return {t[0][c], t[1][c], t[2][c]};
}
double dot(const Vector3& a, const Vector3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vector3 minus(const Vector3& a, const Vector3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
// Points in corner coordinates, mirrored for the left corner so both share one shape.
Vector3 corner_point(const Transform& panel, PanelOffset corner, ResizeCorner side, double a, double b) {
    const double outward = side == ResizeCorner::Right ? a : -a;
    return panel_point(panel, {corner.right + outward, corner.up - b});
}
std::optional<Vector3> plane_hit(const Transform& panel, const Ray& ray) {
    const auto normal = column(panel, 2);
    // Only the face toward the viewer counts, as for the keyboard itself.
    const double approach = dot(ray.direction, normal);
    if (!(approach < -1e-6)) {
        return {};
    }
    const double distance = dot(minus(column(panel, 3), ray.origin), normal) / approach;
    if (!std::isfinite(distance) || distance < 0) {
        return {};
    }
    return Vector3{ray.origin[0] + ray.direction[0] * distance,
                   ray.origin[1] + ray.direction[1] * distance,
                   ray.origin[2] + ray.direction[2] * distance};
}
double segment_distance(double a, double b, double a0, double b0, double a1, double b1) {
    const double da = a1 - a0, db = b1 - b0;
    const double t = std::clamp(((a - a0) * da + (b - b0) * db) / (da * da + db * db), 0.0, 1.0);
    return std::hypot(a - a0 - da * t, b - b0 - db * t);
}
double bracket_distance(double a, double b, const ResizerShape& shape) {
    const double c = -shape.case_radius, edge = gap, end = c - leg_length;
    double distance =
        std::min(segment_distance(a, b, c, edge, end, edge), segment_distance(a, b, edge, c, edge, end));
    if (a >= c && b >= c) {
        distance = std::min(distance, std::abs(std::hypot(a - c, b - c) - (shape.case_radius + gap)));
    }
    return distance;
}
} // namespace
double ResizerShape::half_size() const {
    // Room for the legs, with a comfortable laser area for small radii.
    return std::max(.032, std::max(case_radius, 0.0) + leg_length + stroke + .003);
}
int ResizerShape::pixels() const {
    return 2 * static_cast<int>(std::ceil(half_size() * pixels_per_meter));
}
Vector3 panel_point(const Transform& panel, PanelOffset offset) {
    Vector3 point{};
    for (std::size_t r = 0; r < 3; ++r) {
        point[r] = panel[r][3] + panel[r][0] * offset.right + panel[r][1] * offset.up;
    }
    return point;
}
Transform resizer_transform(const Transform& panel, PanelOffset corner) {
    Transform result = panel;
    const auto center = panel_point(panel, corner);
    for (std::size_t r = 0; r < 3; ++r) {
        result[r][3] = center[r] + panel[r][2] * .003;
    }
    return result;
}
Vector3 resizer_target(const Transform& panel, PanelOffset corner, ResizeCorner side,
                       const ResizerShape& shape) {
    const double bend = -shape.case_radius + (shape.case_radius + gap) / std::sqrt(2.0);
    return corner_point(panel, corner, side, bend, bend);
}
double ray_distance(const Ray& ray, const Vector3& point) {
    const double length = std::sqrt(dot(ray.direction, ray.direction));
    const auto offset = minus(point, ray.origin);
    if (!(length > 1e-9)) {
        return std::sqrt(dot(offset, offset));
    }
    const double along = std::max(0.0, dot(offset, ray.direction) / length);
    return std::sqrt(std::max(0.0, dot(offset, offset) - along * along));
}
double resizer_proximity(double distance) {
    constexpr double shown = .035, hidden = .085;
    const double t = std::clamp((hidden - distance) / (hidden - shown), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
std::optional<Vector3> resizer_hit(const Transform& panel, PanelOffset corner, ResizeCorner side,
                                   const ResizerShape& shape, const Ray& ray) {
    const auto hit = plane_hit(panel, ray);
    if (!hit) {
        return {};
    }
    const auto offset = minus(*hit, panel_point(panel, corner));
    const double right = dot(offset, column(panel, 0));
    const double a = side == ResizeCorner::Right ? right : -right;
    const double b = -dot(offset, column(panel, 1));
    const double half = shape.half_size();
    if (std::abs(a) > half || std::abs(b) > half || (a <= 0 && b <= 0)) {
        return {};
    }
    return hit;
}
std::vector<std::uint8_t> resizer_pixels(ResizeCorner side, const ResizerShape& shape) {
    const int size = shape.pixels();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size * size * 4));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double right = (x + .5 - size / 2.0) / pixels_per_meter;
            const double a = side == ResizeCorner::Right ? right : -right;
            const double b = (y + .5 - size / 2.0) / pixels_per_meter;
            const double distance = bracket_distance(a, b, shape);
            // Steam's muted steel blue, slightly translucent, with no rim.
            const double coverage = std::clamp((stroke - distance) * pixels_per_meter + .5, 0.0, 1.0);
            auto* p = &pixels[static_cast<std::size_t>((y * size + x) * 4)];
            p[0] = 150;
            p[1] = 172;
            p[2] = 200;
            p[3] = static_cast<std::uint8_t>(std::lround(coverage * .92 * 255));
        }
    }
    return pixels;
}
std::array<std::array<float, 4>, 2> resizer_mask(ResizeCorner side, const ResizerShape& shape) {
    const float size = static_cast<float>(shape.pixels()), half = size / 2;
    // The outward half at full height, plus the quadrant below the case's bottom edge.
    if (side == ResizeCorner::Right) {
        return {{{half, 0, half, size}, {0, half, half, half}}};
    }
    return {{{0, 0, half, size}, {half, half, half, half}}};
}
double ResizerFade::update(double target, double now) {
    constexpr double fade_in = .12, fade_out = .2; // Seconds for a complete fade.
    const double elapsed = updated_ ? std::clamp(now - *updated_, 0.0, .1) : 0.0;
    updated_ = now;
    const double step = elapsed / (target > value_ ? fade_in : fade_out);
    value_ = std::clamp(target, value_ - step, value_ + step);
    return value_;
}
double ResizerFade::hold(double now) {
    value_ = 1;
    updated_ = now;
    return value_;
}
bool PanelResize::begin(const Transform& panel, double width, const Ray& ray) {
    const auto hit = plane_hit(panel, ray);
    if (!hit || !(width > 0)) {
        return false;
    }
    const auto span = minus(*hit, column(panel, 3));
    if (dot(span, span) < .01 * .01) {
        return false;
    }
    panel_ = panel;
    width_ = width;
    grab_ = *hit;
    return true;
}
std::optional<double> PanelResize::update(const Ray& ray) const {
    // The plane captured at the press, so the scale never feeds back into the hit.
    const auto hit = plane_hit(panel_, ray);
    if (!hit) {
        return {};
    }
    const auto center = column(panel_, 3);
    const auto span = minus(grab_, center);
    const double factor = dot(minus(*hit, center), span) / dot(span, span);
    return std::clamp(width_ * factor, PanelPlacement::minimum_width, PanelPlacement::maximum_width);
}
} // namespace framekeyboard
