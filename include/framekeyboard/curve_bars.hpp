#pragma once
#include "framekeyboard/panel.hpp"
#include "framekeyboard/placement.hpp"
#include "framekeyboard/resizer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace framekeyboard {
// A case corner as an offset from the overlay center, in meters along the
// flat panel's right and up axes. The texture adds popup_margin above and
// below the case, so its center is the case's middle row.
PanelOffset case_corner(const PanelView& view, double width, bool right, bool bottom);

// Curve bars (Curved keyboard) sit beside the case's left and right edges,
// index 0 left and 1 right, at its middle row. In texture pixels, like the
// movement handle below the case: a pill 8 wide and 200 tall whose middle
// lies 42 from the case, as the handle's does below it. A grip or trigger
// takes a bar anywhere within 30 of its middle line and 20 beyond its ends.
constexpr double curve_bar_width = 8, curve_bar_height = 200, curve_bar_gap = 42;
constexpr double curve_bar_reach_x = 30, curve_bar_reach_y = curve_bar_height / 2 + 20;
// A bar's middle as an offset from the overlay center, along the arc.
PanelOffset curve_bar_offset(const PanelView& view, double width, std::size_t side);
// The bar's frame on the panel's face (curve_frame): it follows the bend.
Transform curve_bar_frame(const PanelView& view, const PanelPlacement& panel, std::size_t side);
// Where a ray meets a bar's plane, and how far that lies from its grab area
// (0 inside), in meters; none when the ray misses the plane or meets its back.
struct CurveBarReach {
    double distance{};
    Vector3 point{};
};
std::optional<CurveBarReach> curve_bar_reach(const Transform& bar, double width, const Ray& ray);
// Whether `area`, in panel pixels, overlaps an open dropdown or a flick
// preview: both may extend past the case, where a side overlay in front of
// the keyboard (a corner resizer or a curve bar) would take their clicks.
bool covers_popup(const PanelView& view, const Rect& area);
// Whether a bar's grab area covers a popup (covers_popup).
bool curve_bar_covers_popup(const PanelView& view, std::size_t side);
// A bar overlay's image: its grab area at 2 pixels per texture pixel, clear
// but for a white pill with rounded ends in its middle. RGBA, not
// premultiplied, rows from the top; tinted and faded by the overlay.
constexpr int curve_bar_image_scale = 2;
constexpr int curve_bar_image_width = static_cast<int>(2 * curve_bar_reach_x * curve_bar_image_scale);
constexpr int curve_bar_image_height = static_cast<int>(2 * curve_bar_reach_y * curve_bar_image_scale);
std::vector<std::uint8_t> curve_bar_pixels();
} // namespace framekeyboard
