#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <pango/pangocairo.h>
#include <stdexcept>

namespace framekeyboard {
namespace {
constexpr double normal_toolbar_height = 96;
struct Placement {
    double scale, x, y;
};
Placement placement(const PanelView& view) {
    const double margin = view.theme->padding;
    const double toolbar_height = view.composing ? 208 : normal_toolbar_height;
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
// Shared vector icons keep controls independent of installed symbol fonts.
// Key icons are centered on the moving face, so they follow press animation.
void draw_icon(cairo_t* cr, Icon icon, Rect bounds, Color color, double size = 24) {
    constexpr double pi = 3.141592653589793;
    cairo_save(cr);
    cairo_translate(cr, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
    const double scale = std::min({size, bounds.width - 8, bounds.height - 8}) / 24;
    cairo_scale(cr, scale, scale);
    source(cr, color);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    auto line = [&](double x1, double y1, double x2, double y2) {
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
    };
    switch (icon) {
    case Icon::SteamFrame:
        cairo_rectangle(cr, -10, -10, 20, 4.8);
        cairo_rectangle(cr, -10, -5.2, 4.8, 15.2);
        cairo_move_to(cr, 10, 10);
        cairo_arc(cr, 10, 10, 13.2, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::SteamOS:
        cairo_arc(cr, -5, 0, 7, 0, 2 * pi);
        cairo_fill(cr);
        cairo_arc(cr, 0, 0, 12, -.5 * pi, .5 * pi);
        cairo_arc_negative(cr, 0, 0, 8.5, .5 * pi, -.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::Settings:
        // Each tooth has a flat tip and a recessed gap.
        for (int i = 0; i < 32; ++i) {
            const double angle = i * pi / 16;
            const double radius = i % 4 < 2 ? 11 : 8.5;
            const double x = radius * std::cos(angle), y = radius * std::sin(angle);
            if (i == 0) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_close_path(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 3.5, 0, 2 * pi);
        break;
    case Icon::Back:
        line(9, 0, -9, 0);
        line(-2, -7, -9, 0);
        cairo_line_to(cr, -2, 7);
        break;
    case Icon::Recenter:
        cairo_arc(cr, 0, 0, 7, 0, 2 * pi);
        line(-11, 0, -5, 0);
        line(5, 0, 11, 0);
        line(0, -11, 0, -5);
        line(0, 5, 0, 11);
        break;
    case Icon::Close:
        line(-7, -7, 7, 7);
        line(7, -7, -7, 7);
        break;
    case Icon::Copy:
        rounded(cr, {-3, -3, 13, 14}, 2);
        cairo_move_to(cr, 4, -6);
        cairo_line_to(cr, 4, -9);
        cairo_line_to(cr, -10, -9);
        cairo_line_to(cr, -10, 5);
        cairo_line_to(cr, -6, 5);
        break;
    case Icon::Paste:
        cairo_move_to(cr, -5, -8);
        cairo_line_to(cr, -9, -8);
        cairo_line_to(cr, -9, 11);
        cairo_line_to(cr, 9, 11);
        cairo_line_to(cr, 9, -8);
        cairo_line_to(cr, 5, -8);
        rounded(cr, {-5, -11, 10, 6}, 2);
        line(-4, 0, 4, 0);
        line(-4, 5, 2, 5);
        break;
    case Icon::ScaleDown:
    case Icon::ScaleUp:
        cairo_arc(cr, -3, -3, 8, 0, 2 * pi);
        line(3, 3, 10, 10);
        line(-7, -3, 1, -3);
        if (icon == Icon::ScaleUp) {
            line(-3, -7, -3, 1);
        }
        break;
    case Icon::None:
        break;
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}
void draw_key(cairo_t* cr, const PanelView& view, const Key& key, double amount) {
    const auto& t = *view.theme;
    const auto modifiers = view.keyboard->modifiers();
    std::set<int> legend_modifiers;
    for (const char* name : {"ShiftLeft", "ShiftRight", "AltRight"}) {
        const int code = key_code(name);
        if (modifiers.contains(code)) {
            legend_modifiers.insert(code);
        }
    }
    const auto r = key.bounds;
    // The side begins below the resting face. A fixed-size face moves down
    // over it; changing face height would make a pressed key look stretched.
    Rect side{r.x, r.y + t.travel, r.width, r.height + t.depth - t.travel};
    rounded(cr, side, t.radius);
    gradient(cr, side, t.side_top, t.side_bottom, t.side_bottom);
    Rect face{r.x, r.y + amount * t.travel, r.width, r.height};
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
    if (key.icon != Icon::None) {
        draw_icon(cr, key.icon, face, t.legend, 32);
        return;
    }
    auto text = view.keymap->legend(key, legend_modifiers, view.keyboard->caps(), view.keyboard->num());
    if (const auto custom = view.key_labels.find(key.id); custom != view.key_labels.end()) {
        text = custom->second;
    }
    std::string secondary;
    if (view.key_labels.contains(key.id)) {
        const auto small = view.language->kana_shift.find(key.action);
        if (small != view.language->kana_shift.end() && small->second != text) {
            secondary = small->second;
        }
    } else if (key.action_kind == ActionKind::Key && key.action.starts_with("Numpad")) {
        secondary = key.secondary_label;
    } else if (key.action_kind == ActionKind::Key && !text.empty() && text != key.label &&
               text.size() < 5) {
        auto shifted = legend_modifiers;
        shifted.insert(key_code("ShiftLeft"));
        secondary = view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
        if (secondary == text) {
            secondary.clear();
        }
    } else if (!key.secondary_label.empty()) {
        auto shifted = legend_modifiers;
        shifted.insert(key_code("ShiftLeft"));
        secondary = view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
        if (secondary == text) {
            secondary.clear();
        }
    }
    if (!view.key_labels.contains(key.id) && g_utf8_strlen(text.c_str(), -1) == 1 &&
        g_utf8_strlen(secondary.c_str(), -1) == 1 && g_unichar_isalpha(g_utf8_get_char(text.c_str())) &&
        g_unichar_tolower(g_utf8_get_char(text.c_str())) ==
            g_unichar_tolower(g_utf8_get_char(secondary.c_str()))) {
        // Case belongs in the main legend, including letters on punctuation
        // positions in Cyrillic/AZERTY layouts. Distinct shifted symbols stay visible.
        secondary.clear();
    }
    // F10-F12 must not shrink simply because their names have three characters.
    const bool function_key = key.action_kind == ActionKind::Key && key.action.starts_with("F") &&
                              key.action.size() > 1 &&
                              key.action.find_first_not_of("0123456789", 1) == std::string::npos;
    const bool utility =
        g_utf8_strlen(text.c_str(), -1) > 2 && !key.action.starts_with("Key") && !function_key;
    if (!secondary.empty()) {
        label(cr, secondary, {face.x, face.y + 3, face.width, face.height * .40}, t.small_font_size,
              view.language->font, t.legend, true);
        label(cr, text, {face.x, face.y + face.height * .4, face.width, face.height * .55},
              t.font_size * .85, view.language->font, t.legend, true);
    } else {
        label(cr, text, face, utility ? t.small_font_size : t.font_size, view.language->font, t.legend,
              true);
    }
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
void PanelRenderer::paint(const PanelView& view, double now, bool force_full) {
    const auto& t = *view.theme;
    const auto modifiers = view.keyboard->modifiers();
    const bool scene_changed =
        !previous_ || previous_->layout != *view.layout || previous_->theme != t ||
        previous_->language != *view.language || previous_->modifiers != modifiers ||
        previous_->caps != view.keyboard->caps() || previous_->num != view.keyboard->num() ||
        previous_->view.controls != view.controls || previous_->view.status != view.status ||
        previous_->view.settings != view.settings || previous_->view.composing != view.composing ||
        previous_->view.preedit != view.preedit || previous_->view.key_labels != view.key_labels;
    bool full = force_full || scene_changed || view.settings;
    std::vector<Rect> damage;
    animating_ = false;
    for (const auto& key : view.layout->keys) {
        auto& animation = animations_[key.id];
        const double old_amount = animation.amount;
        const double target = view.keyboard->pressed(key.id) ? 1 : 0;
        const bool changed_target = target != animation.target;
        if (changed_target) {
            animation.from = animation.amount;
            animation.target = target;
            animation.started = now;
        }
        const double progress =
            t.duration_ms == 0 ? 1
                               : std::clamp((now - animation.started) * 1000 / t.duration_ms, 0.0, 1.0);
        animation.amount = animation.from + (animation.target - animation.from) * progress;
        animating_ |= !view.settings && progress < 1;
        if (!full && (changed_target || animation.amount != old_amount ||
                      previous_->view.hovered.contains(key.id) != view.hovered.contains(key.id))) {
            damage.push_back(key_bounds_.at(key.id));
        }
    }
    // Toolbar changes are infrequent and may overlap custom key geometry.
    if (!full) {
        for (const auto& control : view.controls) {
            if (previous_->view.hovered.contains(control.id) != view.hovered.contains(control.id)) {
                full = true;
            }
        }
    }
    if (scene_changed) {
        key_bounds_.clear();
        // Capture the exact ink extents, including oversized/custom-font labels.
        // These surfaces are only used to measure; pixels are drawn directly onto
        // the image below, preserving Cairo's original blending and font rendering.
        if (!view.settings) {
            const auto p = placement(view);
            for (const auto& key : view.layout->keys) {
                auto* recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
                auto* bounds_cr = cairo_create(recording);
                cairo_translate(bounds_cr, p.x, p.y);
                cairo_scale(bounds_cr, p.scale, p.scale);
                draw_key(bounds_cr, view, key, 0);
                double x, y, width, height;
                cairo_recording_surface_ink_extents(recording, &x, &y, &width, &height);
                // Union the resting and fully depressed positions, then round
                // outwards to pixel boundaries to avoid clipped antialiasing.
                const double left = std::floor(x) - 2, top = std::floor(y) - 2;
                key_bounds_[key.id] = {left, top, std::ceil(x + width) + 2 - left,
                                       std::ceil(y + height + t.travel * p.scale) + 2 - top};
                cairo_destroy(bounds_cr);
                cairo_surface_destroy(recording);
            }
        }
        previous_ =
            Snapshot{*view.layout,        t, *view.language, view, modifiers, view.keyboard->caps(),
                     view.keyboard->num()};
        // No animation state from a removed key should survive a profile reload.
        std::erase_if(animations_, [&](const auto& item) {
            return std::none_of(view.layout->keys.begin(), view.layout->keys.end(),
                                [&](const Key& key) { return key.id == item.first; });
        });
    } else {
        previous_->view = view;
    }
    if (!full && damage.empty()) {
        return;
    }
    auto* cr = cairo_create(surface_);
    if (!full) {
        for (const auto& r : damage) {
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        }
        cairo_clip(cr);
    }
    // Clear and restore the case only inside the damaged pixels. Any neighboring
    // key intersecting that region is repainted in the original stacking order.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    rounded(cr, {0, 0, panel_width, panel_height}, t.surface_radius);
    source(cr, t.surface);
    cairo_fill(cr);
    if (!view.settings) {
        const auto p = placement(view);
        cairo_save(cr);
        cairo_translate(cr, p.x, p.y);
        cairo_scale(cr, p.scale, p.scale);
        for (const auto& key : view.layout->keys) {
            const auto& bounds = key_bounds_.at(key.id);
            const bool intersects =
                full || std::any_of(damage.begin(), damage.end(), [&](const Rect& r) {
                    return bounds.x < r.x + r.width && bounds.x + bounds.width > r.x &&
                           bounds.y < r.y + r.height && bounds.y + bounds.height > r.y;
                });
            if (intersects) {
                draw_key(cr, view, key, animations_.at(key.id).amount);
            }
        }
        cairo_restore(cr);
    }
    if (view.composing && !view.settings) {
        const auto& method = view.language->input_method;
        const char* hint = method == "korean-2set" ? "Type → Space/Enter: commit · Esc: cancel"
                           : method.starts_with("chinese-")
                               ? "Pinyin → Space/1–5: select · Enter: commit · Esc: cancel"
                               : "Type → Space: convert · Enter: commit · Esc: cancel";
        label(cr, view.preedit.empty() ? hint : view.preedit, {20, 94, 1560, 40}, 27,
              view.language->font, t.legend);
    }
    for (const auto& control : view.controls) {
        rounded(cr, control.bounds, t.radius);
        source(cr,
               control.selected ? t.latched : (view.hovered.contains(control.id) ? t.hover : t.bottom));
        cairo_fill(cr);
        if (control.icon == Icon::None) {
            label(cr, control.label, control.bounds, 19, view.language->font, t.legend);
        } else {
            draw_icon(cr, control.icon, control.bounds, t.legend);
        }
    }
    if (!view.status.empty()) {
        label(cr, view.status, view.settings ? Rect{30, 526, 1540, 54} : Rect{18, 60, 1564, 28}, 17,
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
            // Compatibility uploads use straight RGBA; the native path skips this conversion.
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
