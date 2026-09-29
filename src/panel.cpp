#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <pango/pangocairo.h>
#include <stdexcept>

namespace framekeyboard {
namespace {
constexpr double toolbar_height = 68;
struct Placement {
    double scale, x, y;
};
Placement placement(const PanelView& view) {
    const double margin = view.theme->padding;
    const double scale =
        std::min((panel_width - 2 * margin) / view.layout->width,
                 (panel_height - toolbar_height - 2 * margin - view.theme->depth) / view.layout->height);
    return {scale, (panel_width - view.layout->width * scale) / 2,
            toolbar_height + (panel_height - toolbar_height - view.layout->height * scale) / 2};
}
void source(cairo_t* cr, Color color) {
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
}
void rounded(cairo_t* cr, Rect r, double radius) {
    radius = std::min({radius, r.width / 2, r.height / 2});
    constexpr double pi = 3.141592653589793;
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -pi / 2, 0);
    cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, pi / 2);
    cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, pi / 2, pi);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, pi, 1.5 * pi);
    cairo_close_path(cr);
}
void gradient(cairo_t* cr, Rect r, Color top, Color middle, Color bottom) {
    auto* pattern = cairo_pattern_create_linear(0, r.y, 0, r.y + r.height);
    cairo_pattern_add_color_stop_rgb(pattern, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(pattern, .5, middle.r, middle.g, middle.b);
    cairo_pattern_add_color_stop_rgb(pattern, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cr, pattern);
    cairo_fill(cr);
    cairo_pattern_destroy(pattern);
}
void label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
           Color color, bool fit = false) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    if (fit) {
        int natural_width, natural_height;
        pango_layout_get_pixel_size(layout, &natural_width, &natural_height);
        if (natural_width > r.width - 8) {
            const double fitted = std::max(9.0, size * (r.width - 8) / natural_width);
            pango_font_description_set_absolute_size(font, fitted * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
        }
    }
    pango_layout_set_width(layout, static_cast<int>(std::max(1.0, r.width - 8) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    source(cr, color);
    cairo_move_to(cr, r.x + 4, r.y + (r.height - height) / 2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}
} // namespace
PanelRenderer::PanelRenderer() {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, panel_height);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
        throw std::runtime_error("cannot allocate keyboard image");
    }
}
PanelRenderer::~PanelRenderer() {
    cairo_surface_destroy(surface_);
}
const Key* PanelRenderer::hit_key(const PanelView& view, double x, double y) const {
    if (view.settings) {
        return nullptr;
    }
    const auto p = placement(view);
    x = (x - p.x) / p.scale;
    y = (y - p.y) / p.scale;
    for (const auto& key : view.layout->keys) {
        if (key.bounds.contains(x, y)) {
            return &key;
        }
    }
    return nullptr;
}
void PanelRenderer::paint(const PanelView& view, double now) {
    auto* cr = cairo_create(surface_);
    const auto& t = *view.theme;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    rounded(cr, {0, 0, panel_width, panel_height}, t.surface_radius);
    source(cr, t.surface);
    cairo_fill(cr);
    animating_ = false;
    if (!view.settings) {
        const auto p = placement(view);
        cairo_save(cr);
        cairo_translate(cr, p.x, p.y);
        cairo_scale(cr, p.scale, p.scale);
        const auto modifiers = view.keyboard->modifiers();
        std::set<int> legend_modifiers;
        // Ctrl/Meta affect commands, not the printed character legends.
        for (const char* name : {"ShiftLeft", "ShiftRight", "AltRight"}) {
            const int code = key_code(name);
            if (modifiers.contains(code)) {
                legend_modifiers.insert(code);
            }
        }
        for (const auto& key : view.layout->keys) {
            auto& animation = animations_[key.id];
            const double target = view.keyboard->pressed(key.id) ? 1 : 0;
            if (target != animation.target) {
                animation.from = animation.amount;
                animation.target = target;
                animation.started = now;
            }
            const double progress =
                t.duration_ms == 0
                    ? 1
                    : std::clamp((now - animation.started) * 1000 / t.duration_ms, 0.0, 1.0);
            animation.amount = animation.from + (animation.target - animation.from) * progress;
            animating_ |= progress < 1;
            const auto r = key.bounds;
            // The side begins below the resting face. A fixed-size face moves down
            // over it; changing face height would make a pressed key look stretched.
            Rect side{r.x, r.y + t.travel, r.width, r.height + t.depth - t.travel};
            rounded(cr, side, t.radius);
            gradient(cr, side, t.side_top, t.side_bottom, t.side_bottom);
            Rect face{r.x, r.y + animation.amount * t.travel, r.width, r.height};
            const int code = key.action_kind == ActionKind::Key ? key_code(key.action) : 0;
            const bool latched = modifiers.contains(code) ||
                                 (key.action == "CapsLock" && view.keyboard->caps()) ||
                                 (key.action == "NumLock" && view.keyboard->num());
            rounded(cr, face, t.radius);
            if (latched) {
                source(cr, t.latched);
                cairo_fill(cr);
            } else if (view.hovered.contains(key.id)) {
                gradient(cr, face, t.hover, t.middle, t.bottom);
            } else {
                gradient(cr, face, t.top, t.middle, t.bottom);
            }
            auto text =
                view.keymap->legend(key, legend_modifiers, view.keyboard->caps(), view.keyboard->num());
            std::string secondary;
            if (key.action_kind == ActionKind::Key && key.action.starts_with("Numpad")) {
                secondary = key.secondary_label;
            } else if (key.action_kind == ActionKind::Key && !text.empty() && text != key.label &&
                       text.size() < 5) {
                auto shifted = legend_modifiers;
                shifted.insert(key_code("ShiftLeft"));
                secondary =
                    view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
                if (secondary == text) {
                    secondary.clear();
                }
            } else if (!key.secondary_label.empty()) {
                auto shifted = legend_modifiers;
                shifted.insert(key_code("ShiftLeft"));
                secondary =
                    view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
                if (secondary == text) {
                    secondary.clear();
                }
            }
            if (key.action.starts_with("Key")) {
                auto* upper = g_utf8_strup(text.c_str(), -1);
                text = upper;
                g_free(upper);
                secondary.clear();
            }
            const bool utility = text.size() > 2 && !key.action.starts_with("Key");
            if (!secondary.empty()) {
                label(cr, secondary, {face.x, face.y + 3, face.width, face.height * .40},
                      t.small_font_size, view.language->font, t.legend, true);
                label(cr, text, {face.x, face.y + face.height * .4, face.width, face.height * .55},
                      t.font_size * .85, view.language->font, t.legend, true);
            } else {
                label(cr, text, face, utility ? t.small_font_size : t.font_size, view.language->font,
                      t.legend, true);
            }
        }
        cairo_restore(cr);
    }
    for (const auto& control : view.controls) {
        rounded(cr, control.bounds, t.radius);
        source(cr,
               control.selected ? t.latched : (view.hovered.contains(control.id) ? t.hover : t.bottom));
        cairo_fill(cr);
        label(cr, control.label, control.bounds, 19, view.language->font, t.legend);
    }
    if (!view.status.empty()) {
        label(cr, view.status, view.settings ? Rect{30, 526, 1540, 54} : Rect{620, 12, 700, 40}, 17,
              view.language->font, t.legend);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface_);
}
std::vector<unsigned char> PanelRenderer::rgba() const {
    const auto* bytes = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    std::vector<unsigned char> result(static_cast<std::size_t>(panel_width) * panel_height * 4);
    for (int y = 0; y < panel_height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(bytes + y * stride);
        for (int x = 0; x < panel_width; ++x) {
            const auto pixel = row[x];
            const unsigned alpha = pixel >> 24;
            const auto at = static_cast<std::size_t>(y * panel_width + x) * 4;
            // Cairo stores premultiplied ARGB. OpenVR expects straight RGBA.
            result[at] = static_cast<unsigned char>(
                alpha ? std::min(255u, ((pixel >> 16) & 255u) * 255u / alpha) : 0);
            result[at + 1] = static_cast<unsigned char>(
                alpha ? std::min(255u, ((pixel >> 8) & 255u) * 255u / alpha) : 0);
            result[at + 2] =
                static_cast<unsigned char>(alpha ? std::min(255u, (pixel & 255u) * 255u / alpha) : 0);
            result[at + 3] = static_cast<unsigned char>(alpha);
        }
    }
    return result;
}
void PanelRenderer::write_png(const fs::path& path) const {
    if (cairo_surface_write_to_png(surface_, path.c_str()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("cannot write preview PNG");
    }
}
} // namespace framekeyboard
