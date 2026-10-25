#pragma once
#include <array>
#include <optional>

namespace framekeyboard {
// Row-major rigid transform. Its columns are right, up, toward viewer, position.
using Transform = std::array<std::array<double, 4>, 3>;
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
// Where a pointer ray meets the panel's front face. The overlay is centered on
// its transform, width meters wide and width * height_over_width tall; u and v
// run from its top-left corner to 1 at the opposite edges.
struct PanelHit {
    double u{}, v{}, distance{};
    std::array<double, 3> point{};
};
std::optional<PanelHit> panel_ray_hit(const Transform& panel, double width, double height_over_width,
                                      const std::array<double, 3>& origin,
                                      const std::array<double, 3>& direction);
// Laser: the movement line. Resize: a corner resizer, also laser-held.
enum class MoveSource { Laser, Grip, Resize };
// Capture belongs to the initiating head/controller and cursor. Another pointer
// must not steal or release movement, even when it reuses the same cursor index.
class MoveCapture {
  public:
    bool begin(MoveSource source, unsigned device, unsigned pointer = 0) {
        if (active()) {
            return false;
        }
        source_ = source;
        device_ = device;
        pointer_ = pointer;
        return true;
    }
    bool active() const { return source_.has_value(); }
    std::optional<MoveSource> source() const { return source_; }
    unsigned device() const { return device_; }
    bool owns_pointer(unsigned device, unsigned pointer) const {
        return source_ && source_ != MoveSource::Grip && device_ == device && pointer_ == pointer;
    }
    void reset() { source_.reset(); }

  private:
    std::optional<MoveSource> source_;
    unsigned device_{}, pointer_{};
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
    bool ready() const { return ready_; }

  private:
    Transform world_{};
    double width_{.95};
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
