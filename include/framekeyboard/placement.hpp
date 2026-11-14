#pragma once
#include <array>
#include <optional>

namespace framekeyboard {
// Row-major rigid transform. Its columns are right, up, toward viewer, position.
using Transform = std::array<std::array<double, 4>, 3>;
// `a` then `b`: a pose `b` given relative to the pose `a`.
Transform compose(const Transform& a, const Transform& b);
// Normalize Steam's scaled overlay pose into a rigid anchor, retaining its full rotation.
std::optional<Transform> dashboard_anchor(const Transform& scaled_bottom);
Transform move_with_dashboard(const Transform& panel, const Transform& previous,
                              const Transform& current);
// A level room frame facing the viewer (columns right, up, toward), turned by the
// dashboard's change of heading and placed at `panel`'s center, so a keyboard
// carried along with the dashboard keeps its room leveling reference.
Transform turn_with_dashboard(const Transform& facing, const Transform& previous,
                              const Transform& current, const Transform& panel);
// Steam's main tab freezes when an app tab is selected. Carry its last live
// bottom-center anchor with the persistent dashboard bar during those tabs.
class DashboardAnchor {
  public:
    void restore(std::optional<Transform> anchor, std::optional<Transform> bar,
                 bool full_rotation = true);
    std::optional<Transform> update(std::optional<Transform> main, std::optional<Transform> bar);
    const std::optional<Transform>& bar() const { return bar_; }

  private:
    std::optional<Transform> anchor_, bar_;
    bool full_rotation_{true};
};
// Leveling assistance for the panel's long (right) edge. Feed the uncorrected
// pose each frame so the animation never feeds its own correction back into the
// controller grab. Relative to `reference`, a frame whose +z faces the viewer,
// it removes the edge's pitch (one end higher than the other) and yaw (turned
// away), so the edge lands on the reference's right edge. The reference is the
// dashboard near it, or the room facing the user. Without one it removes only
// the pitch. The desk tilt about the edge (roll) is always kept.
using Quaternion = std::array<double, 4>;
class HorizonAlignment {
  public:
    Transform update(const Transform& raw, double now, bool grabbed = false, bool enabled = true,
                     const std::optional<Transform>& reference = {});
    // Ease from the correction shown now, as after a change of reference.
    void retarget(double now);
    void reset() { *this = {}; }
    bool animating() const { return animating_; }

  private:
    double started_{};
    Quaternion from_{1, 0, 0, 0}, correction_{1, 0, 0, 0};
    bool within_threshold_{}, animating_{};
};
// A curved keyboard: SteamVR's overlay curvature bends the panel into part of
// a cylinder about its up axis. Curvature is the share of a full circle the
// panel's width spans, 0 when flat; the arc keeps that width, its middle
// stays on the transform and the ends come toward the viewer. Panel space
// below is meters from the center along the flat panel's right, up and
// toward-the-viewer axes.
constexpr double maximum_curvature = .25; // A quarter circle: each end turns 45 degrees in.
// The face's frame at `right` meters along the arc from the middle and `up`
// meters up, in the world: columns right (along the arc), up, the face's
// normal toward the viewer, and the point itself. Flat, it is the panel's
// own rotation at that offset.
Transform curve_frame(const Transform& panel, double width, double curvature, double right, double up);
// How far the ends come toward the viewer, in meters.
double curve_depth(double width, double curvature);
// The curvature whose ends come `depth` meters toward the viewer, kept
// between flat and maximum_curvature.
double curvature_for_depth(double width, double depth);
// A bend let go with its ends less than this far toward the viewer becomes
// exactly flat: hand tremor would otherwise leave an unmoved grab, or a push
// back to flat, a few millimetres bent (owner's ask, 2026-11-14).
constexpr double curve_snap_depth = .005;
// `curvature` as kept when a bend ends: flat under curve_snap_depth.
double settled_curvature(double width, double curvature);
// Where a pointer ray meets the panel's front face. The overlay is centered on
// its transform, width meters wide and width * height_over_width tall; u and v
// run from its top-left corner to 1 at the opposite edges. On a curved panel
// u follows the arc.
struct PanelHit {
    double u{}, v{}, distance{};
    std::array<double, 3> point{};
};
std::optional<PanelHit> panel_ray_hit(const Transform& panel, double width, double height_over_width,
                                      const std::array<double, 3>& origin,
                                      const std::array<double, 3>& direction, double curvature = 0);
// Laser: the movement line. Resize: a corner resizer, also laser-held.
// Curve: a curve bar, held with the trigger or the grip.
enum class MoveSource { Laser, Grip, Resize, Curve };
// Capture belongs to the initiating head/controller and cursor. Another pointer
// must not steal or release movement, even when it reuses the same cursor index.
class MoveCapture {
  public:
    // `by_grip`: held by the controller's grip rather than a pointer's
    // trigger, as Grip always is and a curve bar may be.
    bool begin(MoveSource source, unsigned device, unsigned pointer = 0, bool by_grip = false) {
        if (active()) {
            return false;
        }
        source_ = source;
        device_ = device;
        pointer_ = pointer;
        by_grip_ = by_grip || source == MoveSource::Grip;
        return true;
    }
    bool active() const { return source_.has_value(); }
    std::optional<MoveSource> source() const { return source_; }
    unsigned device() const { return device_; }
    bool by_grip() const { return active() && by_grip_; }
    bool owns_pointer(unsigned device, unsigned pointer) const {
        // Grip-held captures have no pointer to own.
        return source_ && !by_grip_ && device_ == device && pointer_ == pointer;
    }
    void reset() { source_.reset(); }

  private:
    std::optional<MoveSource> source_;
    unsigned device_{}, pointer_{};
    bool by_grip_{};
};
class PanelPlacement {
  public:
    static constexpr double minimum_width = .45, maximum_width = 2.0;
    void recenter(const Transform& head, const Transform* dashboard_bottom = nullptr,
                  double height_over_width = 600.0 / 1600.0);
    Transform transform() const { return world_; }
    void set_transform(const Transform& world);
    void set_width(double width);
    void restore(const Transform& world, double width);
    double width() const { return width_; }
    // The panel's bend (see maximum_curvature); restore() leaves it as it is.
    double curvature() const { return curvature_; }
    void set_curvature(double curvature);
    bool ready() const { return ready_; }

  private:
    Transform world_{};
    double width_{.95};
    double curvature_{};
    bool ready_{};
};
// A curve handle held by a controller, with its trigger or grip. Two
// gestures add up, both along the face's normal as it was at the grab (not
// bent). Pulling: the ends follow the hand, so a pull of 10 cm toward the
// viewer brings them 10 cm closer and pushing back flattens the panel again.
// Twisting: they follow how far the laser, from where it started, sweeps
// along that normal across a plane through the handle. The plane contains
// the normal and faces the laser as squarely as it can, so with the hand off
// to one side it stands upright. A laser within about 12 degrees of the
// normal crosses no such plane well; then only pulling counts. Only the
// curvature changes; the panel keeps its pose and width.
class PanelBend {
  public:
    void begin(const Transform& panel, double width, double curvature,
               const std::array<double, 3>& handle, const std::array<double, 3>& hand,
               const std::array<double, 3>& origin, const std::array<double, 3>& direction);
    // The curvature for the hand and its laser's direction now; none while
    // the laser grazes the plane or points away from it, which keeps the last.
    std::optional<double> update(const std::array<double, 3>& hand,
                                 const std::array<double, 3>& direction) const;

  private:
    // Where a laser from the grab's origin meets the plane, along the normal
    // from the handle.
    std::optional<double> swept(const std::array<double, 3>& direction) const;
    std::array<double, 3> normal_{}, start_{}, handle_{}, origin_{}, across_{};
    double width_{}, depth_{}, swept_{};
    bool twist_{};
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
