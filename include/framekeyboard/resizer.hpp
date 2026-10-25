#pragma once
#include "placement.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace framekeyboard {
using Vector3 = std::array<double, 3>;
struct Ray {
    Vector3 origin{}, direction{0, 0, -1};
};
// Corner resizers sit outside the case, beside its bottom corners. Any laser
// shows and drives either one.
enum class ResizeCorner { Left, Right };
// A resizer is a square overlay centered on the case's sharp bottom corner.
// Its bracket follows the case's rounded corner at a fixed gap, like Steam's
// window corners, so the image depends on that radius in meters. Three
// quadrants lie outside the case and take the laser; the one over the case
// never does, so keys stay reachable.
struct ResizerShape {
    static constexpr double pixels_per_meter = 2000;
    double case_radius{};
    // Half the overlay's width in meters, and its width in pixels.
    double half_size() const;
    int pixels() const;
    bool operator==(const ResizerShape&) const = default;
};
// The case's sharp corner as an offset from the panel's center along its own
// right and up axes, in meters.
struct PanelOffset {
    double right{}, up{};
};
Vector3 panel_point(const Transform& panel, PanelOffset offset);
// The resizer overlay pose: the panel's rotation, centered on `corner`, just
// in front of the face so the keyboard never covers it.
Transform resizer_transform(const Transform& panel, PanelOffset corner);
// Where the drawn bracket bends; a laser's distance to it drives the fade.
Vector3 resizer_target(const Transform& panel, PanelOffset corner, ResizeCorner side,
                       const ResizerShape& shape);
// Shortest distance from a ray (only ahead of its origin) to a point.
double ray_distance(const Ray& ray, const Vector3& point);
// Whether a laser is near enough to show a resizer: within 5 cm of the
// bracket, and until it is beyond 7 cm. The gap keeps hand tremor at the
// edge from toggling it; ResizerFade times the change.
class ResizerReach {
  public:
    bool update(double distance);

  private:
    bool near_{};
};
// Where the ray meets the panel plane inside the resizer's laser area, the
// same area as resizer_mask.
std::optional<Vector3> resizer_hit(const Transform& panel, PanelOffset corner, ResizeCorner side,
                                   const ResizerShape& shape, const Ray& ray);
// The bracket image: `_|` for the right corner, mirrored for the left. RGBA,
// not premultiplied, shape.pixels() square, rows from the top.
std::vector<std::uint8_t> resizer_pixels(ResizeCorner side, const ResizerShape& shape);
// The laser area in the overlay's top-left pixel coordinates: rectangles
// {x, y, width, height} for the three quadrants outside the sharp corner, and
// a circle {x, y, radius} around the bracket's bend. On a large keyboard the
// bend lies inside the sharp corner; the circle reaches it without entering
// the rounded case, since it is the bracket's gap wide.
struct ResizerMask {
    std::array<std::array<float, 4>, 2> rectangles{};
    std::array<float, 3> circle{};
};
ResizerMask resizer_mask(ResizeCorner side, const ResizerShape& shape);
// Eases displayed opacity toward a target so the resizer fades in and out.
class ResizerFade {
  public:
    double update(double target, double now);
    // Full opacity with no easing, as while the resizer is held.
    double hold(double now);
    double value() const { return value_; }
    void reset() { *this = {}; }

  private:
    double value_{};
    std::optional<double> updated_;
};
// Scales the panel about its center, so only its width changes. The scale
// follows the ray's hit on the panel plane projected onto the line from the
// center through the initial grab, so a press never jumps, the grabbed corner
// follows the laser and sideways motion changes nothing.
class PanelResize {
  public:
    bool begin(const Transform& panel, double width, const Ray& ray);
    // The new width, or none while the ray misses the panel's plane.
    std::optional<double> update(const Ray& ray) const;

  private:
    Transform panel_{};
    double width_{};
    Vector3 grab_{};
};
} // namespace framekeyboard
