#include "framekeyboard/curve_bars.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
PanelOffset case_corner(const PanelView& view, double width, bool right, bool bottom) {
    const auto body = panel_case_bounds(view);
    const double meters = width / panel_width;
    return {((right ? body.x + body.width : body.x) - panel_width / 2.0) * meters,
            (texture_height / 2.0 - popup_margin - (bottom ? body.y + body.height : body.y)) * meters};
}
PanelOffset curve_bar_offset(const PanelView& view, double width, std::size_t side) {
    const auto top = case_corner(view, width, side == 1, false);
    const auto bottom = case_corner(view, width, side == 1, true);
    const double outward = (side == 1 ? 1 : -1) * curve_bar_gap * width / panel_width;
    return {top.right + outward, (top.up + bottom.up) / 2};
}
Transform curve_bar_frame(const PanelView& view, const PanelPlacement& panel, std::size_t side) {
    const auto middle = curve_bar_offset(view, panel.width(), side);
    return curve_frame(panel.transform(), panel.width(), panel.curvature(), middle.right, middle.up);
}
std::optional<CurveBarReach> curve_bar_reach(const Transform& bar, double width, const Ray& ray) {
    Vector3 axis[3]{}, middle{};
    for (std::size_t r = 0; r < 3; ++r) {
        middle[r] = bar[r][3];
        for (std::size_t c = 0; c < 3; ++c) {
            axis[c][r] = bar[r][c];
        }
    }
    auto dot = [](const Vector3& a, const Vector3& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const double approach = dot(ray.direction, axis[2]);
    if (!(approach < -1e-6)) {
        return {};
    }
    const Vector3 to_middle{middle[0] - ray.origin[0], middle[1] - ray.origin[1],
                            middle[2] - ray.origin[2]};
    const double along = dot(to_middle, axis[2]) / approach;
    if (!std::isfinite(along) || along < 0) {
        return {};
    }
    CurveBarReach reach;
    for (std::size_t r = 0; r < 3; ++r) {
        reach.point[r] = ray.origin[r] + ray.direction[r] * along;
    }
    const Vector3 offset{reach.point[0] - middle[0], reach.point[1] - middle[1],
                         reach.point[2] - middle[2]};
    const double meters = width / panel_width;
    reach.distance =
        std::hypot(std::max(0.0, std::abs(dot(offset, axis[0])) - curve_bar_reach_x * meters),
                   std::max(0.0, std::abs(dot(offset, axis[1])) - curve_bar_reach_y * meters));
    return reach;
}
bool curve_bar_covers_popup(const PanelView& view, std::size_t side) {
    const auto body = panel_case_bounds(view);
    const double middle = side == 1 ? body.x + body.width + curve_bar_gap : body.x - curve_bar_gap;
    const Rect area{middle - curve_bar_reach_x, body.y + body.height / 2 - curve_bar_reach_y,
                    2 * curve_bar_reach_x, 2 * curve_bar_reach_y};
    auto overlaps = [&](const std::optional<Rect>& r) {
        return r && r->x < area.x + area.width && area.x < r->x + r->width &&
               r->y < area.y + area.height && area.y < r->y + r->height;
    };
    return overlaps(view.popup) || overlaps(flick_preview_bounds(view));
}
std::vector<std::uint8_t> curve_bar_pixels() {
    constexpr int width = curve_bar_image_width, height = curve_bar_image_height;
    constexpr double radius = curve_bar_width / 2 * curve_bar_image_scale;
    constexpr double reach = (curve_bar_height / 2 - curve_bar_width / 2) * curve_bar_image_scale;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dx = x + .5 - width / 2.0;
            const double dy = std::max(0.0, std::abs(y + .5 - height / 2.0) - reach);
            const double alpha = std::clamp(radius + .5 - std::hypot(dx, dy), 0.0, 1.0);
            auto* p = &pixels[static_cast<std::size_t>((y * width + x) * 4)];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255));
        }
    }
    return pixels;
}
} // namespace framekeyboard
