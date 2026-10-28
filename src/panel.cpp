#include "framekeyboard/panel.hpp"
#include "framekeyboard/emoji_glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdexcept>

namespace framekeyboard {
std::optional<Rect> panel_move_handle_bounds(const PanelView& view) {
    for (const auto& control : view.controls) {
        if (control.style == ControlStyle::MoveHandle) {
            return control.bounds;
        }
    }
    return {};
}
bool hidden_key(const PanelView& view, const Key& key) {
    if (key.action_kind == ActionKind::Dictation && key.action == "mic") {
        // Its slot stays empty, unless a model download is on its way.
        return !view.dictation_available && !view.dictation && !view.dictation_download;
    }
    return view.hide_numpad && !view.settings &&
           (key.action == "NumLock" || key.action.starts_with("Numpad"));
}
double visible_layout_width(const PanelView& view) {
    if (view.emoji) {
        return view.emoji->width; // The picker keeps the keyboard's width.
    }
    if (!view.hide_numpad || view.settings) {
        return view.layout->width;
    }
    double right = 0;
    bool removed = false;
    for (const auto& key : view.layout->keys) {
        if (hidden_key(view, key)) {
            removed = true;
        } else {
            right = std::max(right, key.bounds.x + key.bounds.width);
        }
    }
    // Do not crop intentional whitespace on a layout without a numpad, or an
    // all-numpad custom layout to zero width.
    return removed && right > 0 ? right : view.layout->width;
}
namespace {
// How far the flick preview's caps and the skin tone menu raise their soft
// shadows; repaints cover them.
constexpr double flick_shadow = 8, tone_shadow = 12;
Color disabled_legend(const Theme& theme) {
    return {(theme.legend.r + theme.disabled.r) / 2, (theme.legend.g + theme.disabled.g) / 2,
            (theme.legend.b + theme.disabled.b) / 2};
}
// CSS ease-out is cubic-bezier(0, 0, .58, 1), as used by Steam's handle.
double handle_ease_out(double progress) {
    if (progress <= 0 || progress >= 1) {
        return progress;
    }
    double low = 0, high = 1;
    for (int i = 0; i < 16; ++i) {
        const double t = (low + high) / 2;
        const double x = 3 * .58 * (1 - t) * t * t + t * t * t;
        if (x < progress) {
            low = t;
        } else {
            high = t;
        }
    }
    const double t = (low + high) / 2;
    return t * t * (3 - 2 * t);
}
constexpr double normal_toolbar_height = 96;
struct Placement {
    double scale, x, y;
};
Placement placement(const PanelView& view, bool normal = false) {
    const double margin = view.theme->padding;
    const double toolbar_height = view.composing && !normal ? 208 : normal_toolbar_height;
    const double scale =
        std::min((panel_width - 2 * margin) / view.layout->width,
                 (panel_height - toolbar_height - 2 * margin - view.theme->depth) / view.layout->height);
    return {scale, (panel_width - visible_layout_width(view) * scale) / 2,
            toolbar_height + (panel_height - toolbar_height - view.layout->height * scale) / 2};
}
// Whole pixels around a rectangle, for clips that never cut antialiasing.
Rect pixel_bounds(Rect r, double margin = 0) {
    const double left = std::floor(r.x - margin), top = std::floor(r.y - margin);
    return {left, top, std::ceil(r.x + r.width + margin) - left,
            std::ceil(r.y + r.height + margin) - top};
}
// Cells of the flick preview in panel coordinates: tap, left, up, right, down.
// Square cells a little taller than the key, centred on it.
std::optional<std::array<Rect, 5>> flick_cells(const PanelView& view) {
    if (!view.flick_preview || view.settings) {
        return {};
    }
    const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                  [&](const Key& k) { return k.id == view.flick_preview->key; });
    if (key == view.layout->keys.end()) {
        return {};
    }
    const auto p = placement(view);
    const double size = key->bounds.height * p.scale * 1.15, step = size + 4;
    const double x = p.x + (key->bounds.x + key->bounds.width / 2) * p.scale - size / 2;
    const double y = p.y + (key->bounds.y + key->bounds.height / 2) * p.scale - size / 2;
    return std::array<Rect, 5>{Rect{x, y, size, size}, Rect{x - step, y, size, size},
                               Rect{x, y - step, size, size}, Rect{x + step, y, size, size},
                               Rect{x, y + step, size, size}};
}
bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
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
Color mix(Color a, Color b, double amount) {
    return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount};
}
// A light theme has a dark legend on a light case.
bool light_theme(const Theme& t) {
    return .2126 * t.legend.r + .7152 * t.legend.g + .0722 * t.legend.b <= .5;
}
// Moves a colour away from the legend: darker under a light legend, lighter
// under a dark one, so recessed surfaces keep their text readable in light
// themes too. With a white legend this is a plain darkening.
Color recede(const Theme& t, Color c, double amount) {
    return mix(c, light_theme(t) ? Color{1, 1, 1} : Color{}, amount);
}
// A neon tube along the inside edge of a rounded shape, at `alpha` strength:
// a soft glow falling inward, the tube in the latched gradient, and its hot
// core. All of it stays inside the shape, so a key repaints within its bounds.
void neon_tube(cairo_t* cr, const Theme& t, const Rect& r, double radius, double alpha = 1) {
    const Rect tube{r.x + 2, r.y + 2, r.width - 4, r.height - 4};
    const double tube_radius = std::max(1.0, radius - 2);
    auto* light = cairo_pattern_create_linear(0, tube.y, 0, tube.y + tube.height);
    cairo_pattern_add_color_stop_rgb(light, 0, t.latched_top.r, t.latched_top.g, t.latched_top.b);
    cairo_pattern_add_color_stop_rgb(light, .5, t.latched.r, t.latched.g, t.latched.b);
    cairo_pattern_add_color_stop_rgb(light, 1, t.latched_bottom.r, t.latched_bottom.g,
                                     t.latched_bottom.b);
    cairo_save(cr);
    rounded(cr, r, radius);
    cairo_clip(cr);
    cairo_push_group(cr); // The layers blend among themselves, then fade together.
    for (const auto& [width, strength] :
         std::initializer_list<std::pair<double, double>>{{14, .1}, {8, .18}, {4.5, .4}, {2.2, 1}}) {
        rounded(cr, tube, tube_radius);
        cairo_set_source(cr, light);
        cairo_set_line_width(cr, width);
        cairo_push_group(cr);
        cairo_stroke(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, strength);
    }
    rounded(cr, tube, tube_radius);
    cairo_set_source_rgba(cr, 1, 1, 1, .45);
    cairo_set_line_width(cr, .8);
    cairo_stroke(cr);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_pattern_destroy(light);
}
// Fills a rounded shape with the latched colour in the theme's finish.
void fill_latched(cairo_t* cr, const Theme& t, const Rect& r, double radius) {
    rounded(cr, r, radius);
    if (t.latched_finish == "neon") {
        // A dark face with a faint tint of the light, inside the tube.
        gradient(cr, r, mix(t.top, t.latched_top, .1), mix(t.middle, t.latched, .1),
                 mix(t.bottom, t.latched_bottom, .12));
        neon_tube(cr, t, r, radius);
    } else if (t.latched_top == t.latched && t.latched_bottom == t.latched) {
        source(cr, t.latched);
        cairo_fill(cr);
    } else {
        gradient(cr, r, t.latched_top, t.latched, t.latched_bottom);
    }
}
void label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
           Color color, bool fit = false, PangoAlignment alignment = PANGO_ALIGN_CENTER,
           bool wrap = false, bool right_to_left = false) {
    auto* layout = pango_cairo_create_layout(cr);
    if (right_to_left) {
        // A lone bracket then mirrors as it does in that script's text.
        pango_layout_set_auto_dir(layout, FALSE);
        pango_context_set_base_dir(pango_layout_get_context(layout), PANGO_DIRECTION_RTL);
        pango_layout_context_changed(layout);
    }
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
    if (wrap) {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
    } else {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    }
    pango_layout_set_alignment(layout, alignment);
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
        // Trace the reference's 194px mark: 47.5px bars, a 128px
        // quarter-circle and small rounded exterior corners. Inner corners
        // stay square; stroking the outline would change the measured gaps.
        cairo_scale(cr, 24.0 / 194, 24.0 / 194);
        cairo_translate(cr, -225.5, -225.5);
        cairo_move_to(cr, 131.5, 128.5);
        cairo_line_to(cr, 319.5, 128.5);
        cairo_arc(cr, 319.5, 131.5, 3, -.5 * pi, 0);
        cairo_line_to(cr, 322.5, 176);
        cairo_line_to(cr, 176, 176);
        cairo_line_to(cr, 176, 322.5);
        cairo_line_to(cr, 131.5, 322.5);
        cairo_arc(cr, 131.5, 319.5, 3, .5 * pi, pi);
        cairo_line_to(cr, 128.5, 131.5);
        cairo_arc(cr, 131.5, 131.5, 3, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_move_to(cr, 322.5, 322.5);
        cairo_arc(cr, 322.5, 322.5, 128, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::SteamOS:
        // The reference uses concentric radii 48, 62.4 and 96. Its ink
        // bounds are 144x192, centered here without stretching to a square.
        cairo_arc(cr, -3, 0, 6, 0, 2 * pi);
        cairo_fill(cr);
        cairo_arc(cr, -3, 0, 12, -.5 * pi, .5 * pi);
        cairo_arc_negative(cr, -3, 0, 7.8, .5 * pi, -.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::Apply:
        line(-8, 0, -2, 6);
        line(-2, 6, 9, -7);
        break;
    case Icon::Reload:
        cairo_arc(cr, 0, 0, 8, -.5, 4.8);
        line(1, -8, -3, -10);
        line(1, -8, -2, -4);
        break;
    case Icon::PinOn:
    case Icon::PinOff:
        if (icon == Icon::PinOff) {
            cairo_rotate(cr, .65);
        }
        line(-5, -9, 5, -9);
        line(-4, -9, -4, -1);
        line(4, -9, 4, -1);
        cairo_move_to(cr, -4, -1);
        cairo_line_to(cr, -8, 3);
        cairo_line_to(cr, 8, 3);
        cairo_line_to(cr, 4, -1);
        line(0, 3, 0, 10);
        if (icon == Icon::PinOff) {
            cairo_stroke(cr);
            cairo_rotate(cr, -.65);
            line(-10, -10, 10, 10);
        }
        break;
    case Icon::PassthroughOn:
    case Icon::PassthroughOff:
        // A ray and a window: on, the ray passes through to the app behind it;
        // off, the window stops it.
        cairo_rectangle(cr, -3, -9, 6, 18);
        if (icon == Icon::PassthroughOn) {
            line(-11, 0, -3, 0);
            line(3, 0, 11, 0);
            line(11, 0, 7, -4);
            line(11, 0, 7, 4);
        } else {
            line(-11, 0, -4, 0);
            line(-4, 0, -8, -4);
            line(-4, 0, -8, 4);
        }
        break;
    case Icon::KeepOpenOn:
    case Icon::KeepOpenOff:
        // An infinity loop: the panel stays. Off, slashed like Pin.
        for (int i = 0; i <= 48; ++i) {
            const double t = i * pi / 24, d = 1 + std::sin(t) * std::sin(t);
            const double x = 10.5 * std::cos(t) / d, y = 10.5 * std::sin(t) * std::cos(t) / d;
            if (i == 0) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        if (icon == Icon::KeepOpenOff) {
            line(-10, -10, 10, 10);
        }
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
    case Icon::Backspace:
        // |<-- : the bar sets it apart from the plain left arrow. Keys draw it
        // at the arrow glyphs' size; a thicker line keeps their stroke weight.
        cairo_set_line_width(cr, 2.3);
        line(-12, -7, -12, 7);
        line(11, 0, -6, 0);
        line(0, -6, -6, 0);
        cairo_line_to(cr, 0, 6);
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
    case Icon::ClearText:
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 10, 0, 2 * pi);
        line(-4, -4, 4, 4);
        line(4, -4, -4, 4);
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
    case Icon::Mic:
        rounded(cr, {-4, -11, 8, 14}, 4);
        cairo_stroke(cr);
        cairo_arc(cr, 0, 0, 7.5, 0, pi);
        cairo_stroke(cr);
        line(0, 7.5, 0, 11);
        line(-4, 11, 4, 11);
        break;
    case Icon::Headset: {
        // A VR headset seen from the front: a rounded visor with a notch
        // for the nose and short strap stubs.
        const double l = -10, r = 10, t = -5, b = 5, k = 3.5;
        cairo_new_path(cr);
        cairo_move_to(cr, l + k, t);
        cairo_line_to(cr, r - k, t);
        cairo_arc(cr, r - k, t + k, k, -.5 * pi, 0);
        cairo_line_to(cr, r, b - k);
        cairo_arc(cr, r - k, b - k, k, 0, .5 * pi);
        cairo_line_to(cr, 3, b);
        cairo_arc_negative(cr, 0, b, 3, 0, pi);
        cairo_line_to(cr, l + k, b);
        cairo_arc(cr, l + k, b - k, k, .5 * pi, pi);
        cairo_line_to(cr, l, t + k);
        cairo_arc(cr, l + k, t + k, k, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_stroke(cr);
        line(-12.5, -1, -10, -1);
        line(10, -1, 12.5, -1);
        break;
    }
    case Icon::Computer:
        // A monitor on a stand.
        rounded(cr, {-10, -8, 20, 13}, 2);
        cairo_stroke(cr);
        line(0, 5, 0, 9);
        line(-5, 9, 5, 9);
        break;
    case Icon::Cloud:
        cairo_new_path(cr);
        cairo_arc(cr, -6.5, 2.5, 3.5, .5 * pi, 1.5 * pi);
        cairo_arc(cr, 0, -1, 5.5, pi, 2 * pi);
        cairo_arc(cr, 6.5, 2.5, 3.5, 1.5 * pi, 2.5 * pi);
        cairo_close_path(cr);
        cairo_stroke(cr);
        break;
    case Icon::Undo:
        cairo_arc_negative(cr, 1, 1, 8, -.15 * pi, -1.0 * pi);
        cairo_stroke(cr);
        line(-7, 1, -10, -4);
        line(-7, 1, -2, -2);
        break;
    case Icon::Emoji:
    case Icon::Smileys:
        cairo_arc(cr, 0, 0, 10, 0, 2 * pi);
        for (const double x : {-3.6, 3.6}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, -3, .5, 0, 2 * pi);
        }
        cairo_new_sub_path(cr);
        if (icon == Icon::Emoji) {
            cairo_arc(cr, 0, 1, 5.5, .15 * pi, .85 * pi);
        } else {
            // Grinning: an open mouth.
            cairo_move_to(cr, -5.5, 2);
            cairo_line_to(cr, 5.5, 2);
            cairo_arc(cr, 0, 2, 5.5, 0, pi);
        }
        break;
    case Icon::Favorites:
        // A five-pointed star, its points rounded by the round join.
        for (int i = 0; i < 10; ++i) {
            const double angle = -pi / 2 + i * pi / 5, radius = i % 2 ? 4.4 : 10.5;
            const double x = radius * std::cos(angle), y = radius * std::sin(angle) + .8;
            i ? cairo_line_to(cr, x, y) : cairo_move_to(cr, x, y);
        }
        cairo_close_path(cr);
        break;
    case Icon::Recent:
        cairo_arc(cr, 0, 0, 10, 0, 2 * pi);
        line(0, 0, 0, -6);
        line(0, 0, 4.5, 2.5);
        break;
    case Icon::People:
        cairo_arc(cr, 0, -5, 4, 0, 2 * pi);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 11, 8.5, pi, 2 * pi);
        break;
    case Icon::Animals:
        // A paw: one pad and four toes.
        cairo_arc(cr, 0, 5, 4.5, 0, 2 * pi);
        for (const auto [x, y] : {std::pair{-7.5, -1.5}, {-3, -7}, {3, -7}, {7.5, -1.5}}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, y, 2, 0, 2 * pi);
        }
        break;
    case Icon::Food:
        // A burger: bun, filling, base.
        cairo_arc(cr, 0, -1, 9, pi, 2 * pi);
        cairo_close_path(cr);
        line(-9.5, 3, 9.5, 3);
        cairo_new_sub_path(cr);
        rounded(cr, {-9, 6, 18, 4}, 2);
        break;
    case Icon::Travel:
        // A car.
        rounded(cr, {-10.5, -1, 21, 7}, 2);
        cairo_move_to(cr, -6.5, -1);
        cairo_line_to(cr, -4, -6.5);
        cairo_line_to(cr, 4, -6.5);
        cairo_line_to(cr, 6.5, -1);
        for (const double x : {-5.5, 5.5}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, 8, 2.2, 0, 2 * pi);
        }
        break;
    case Icon::Activities:
        // A ball with seams.
        cairo_arc(cr, 0, 0, 10, 0, 2 * pi);
        line(0, -10, 0, 10);
        line(-10, 0, 10, 0);
        cairo_new_sub_path(cr);
        cairo_arc(cr, -14, 0, 10, -.8, .8);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 14, 0, 10, pi - .8, pi + .8);
        break;
    case Icon::Objects:
        // A light bulb.
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, -3, 7.5, .5 * pi + .7, 2.5 * pi - .7);
        cairo_line_to(cr, 3, 6);
        cairo_line_to(cr, -3, 6);
        cairo_close_path(cr);
        line(-2.5, 9.5, 2.5, 9.5);
        break;
    case Icon::Symbols:
        // A heart.
        cairo_move_to(cr, 0, 9);
        cairo_curve_to(cr, -13, 0, -7, -11, 0, -4.5);
        cairo_curve_to(cr, 7, -11, 13, 0, 0, 9);
        cairo_close_path(cr);
        break;
    case Icon::Flags:
        line(-7, -10, -7, 11);
        cairo_move_to(cr, -7, -9.5);
        cairo_line_to(cr, 8.5, -9.5);
        cairo_line_to(cr, 5, -5);
        cairo_line_to(cr, 8.5, -.5);
        cairo_line_to(cr, -7, -.5);
        break;
    case Icon::Clear:
        // A bin.
        line(-8.5, -6, 8.5, -6);
        cairo_move_to(cr, -3, -6);
        cairo_line_to(cr, -3, -9);
        cairo_line_to(cr, 3, -9);
        cairo_line_to(cr, 3, -6);
        cairo_move_to(cr, -6, -6);
        cairo_line_to(cr, -5, 10);
        cairo_line_to(cr, 5, 10);
        cairo_line_to(cr, 6, -6);
        line(-2, -2, -2, 6);
        line(2, -2, 2, 6);
        break;
    case Icon::None:
        break;
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}
// How far the mic key's bars have eased toward their new heights, 0–1.
// Flat dots do not change, so silence repaints nothing. The easing is
// sampled 30 times a second, so it repaints and uploads every other display
// frame: half the cost of 60 (owner choice).
double mic_phase(const PanelView& view, double now) {
    const auto& levels = view.dictation_levels;
    if (std::all_of(levels.begin(), levels.end(), [](int level) { return level == 0; })) {
        return 1;
    }
    constexpr double frame = 1.0 / 30;
    const double sampled = std::floor(now / frame) * frame;
    return std::clamp((sampled - view.dictation_level_since) / dictation_level_period, 0.0, 1.0);
}
// A hovered name too long for its row, `overflow` pixels in all, waits,
// scrolls to its end, rests there, then starts over. Its offset in whole
// pixels, and how long until that changes: never more often than 30 times a
// second, and not at all while it waits or rests.
struct MarqueeStep {
    double offset{}, wait{};
};
MarqueeStep marquee_step(double overflow, double elapsed) {
    constexpr double wait = .8, speed = 45, rest = 1.2;
    const double travel = overflow / speed, cycle = wait + travel + rest;
    const double t = std::fmod(elapsed, cycle);
    if (t < wait) {
        return {0, wait - t};
    }
    if (t >= wait + travel) {
        return {std::floor(overflow), cycle - t};
    }
    const double raw = (t - wait) * speed, shown = std::floor(raw);
    return {shown, std::max((shown + 1 - raw) / speed, 1.0 / 30)};
}
// A label's text without ellipsis, moved left by `offset` inside its box.
void scrolled_label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
                    Color color, double offset) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    cairo_save(cr);
    cairo_rectangle(cr, r.x + 4, r.y, r.width - 8, r.height);
    cairo_clip(cr);
    source(cr, color);
    cairo_move_to(cr, std::round(r.x + 4 - offset), r.y + (r.height - height) / 2);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
}
// The width of a label's text, measured as label() draws it.
double text_width(cairo_t* cr, const std::string& text, double size, const std::string& family) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    pango_font_description_free(font);
    g_object_unref(layout);
    return width;
}
// Where speech is recognized, small in the mic key's top-right corner; the
// editor shows it by its counter instead.
void draw_where(cairo_t* cr, const PanelView& view, const Theme& t, const Rect& face) {
    if (view.dictation_where == Icon::None || view.dictation) {
        return;
    }
    draw_icon(cr, view.dictation_where, {face.x + face.width - 30, face.y + 4, 26, 24},
              mix(t.legend, t.top, .35), 14);
}
// A bright segment circling just inside the mic key's face while phrases
// are being recognized; one lap takes 1.2 s.
void draw_ring(cairo_t* cr, const Theme& t, const Rect& face, double time) {
    const Rect r{face.x + 1.5, face.y + 1.5, face.width - 3, face.height - 3};
    const double radius = std::max(1.0, t.radius - 1.5);
    const double perimeter = 2 * (r.width + r.height - 4 * radius) + 2 * 3.141592653589793 * radius;
    const double segment = perimeter * .22;
    const double dash[]{segment, perimeter - segment};
    cairo_save(cr);
    rounded(cr, r, radius);
    cairo_set_dash(cr, dash, 2, -std::fmod(time / 1.2, 1.0) * perimeter);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .85);
    cairo_stroke(cr);
    cairo_restore(cr);
}
// The arrow before a Shift key's legend, `width` wide with its foot at
// `bottom`: an outline, filled while Shift is locked.
void shift_arrow(cairo_t* cr, double x, double bottom, double width, Color color, bool filled) {
    const double height = width * 1.15, shoulder = bottom - height * .5, stem = width * .26;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, x + width / 2, bottom - height);
    cairo_line_to(cr, x + width, shoulder);
    cairo_line_to(cr, x + width / 2 + stem, shoulder);
    cairo_line_to(cr, x + width / 2 + stem, bottom);
    cairo_line_to(cr, x + width / 2 - stem, bottom);
    cairo_line_to(cr, x + width / 2 - stem, shoulder);
    cairo_line_to(cr, x, shoulder);
    cairo_close_path(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 1.8);
    source(cr, color);
    if (filled) {
        cairo_fill_preserve(cr);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}
// A latched modifier or lock, or the mic key while it listens: drawn in the latched colour.
bool latched_key(const PanelView& view, const Key& key, const std::set<int>& modifiers) {
    const int code = key.action_kind == ActionKind::Key ? key_code(key.action) : 0;
    return modifiers.contains(code) || (key.action == "CapsLock" && view.keyboard->caps()) ||
           (key.action == "NumLock" && view.keyboard->num()) ||
           (view.dictation_capturing && key.icon == Icon::Mic);
}
void draw_key(cairo_t* cr, const PanelView& view, const Key& key, double amount, double phase = 1,
              double ring = 0) {
    const auto modifiers = view.keyboard->modifiers();
    std::set<int> legend_modifiers;
    for (const char* name : {"ShiftLeft", "ShiftRight", "AltRight"}) {
        const int code = key_code(name);
        if (modifiers.contains(code)) {
            legend_modifiers.insert(code);
        }
    }
    const bool listening = view.dictation_capturing && key.icon == Icon::Mic;
    const bool latched = latched_key(view, key, modifiers) && !listening;
    const bool disabled = view.disabled_keys.contains(key.id);
    // A latched face carries its own legend colour; everything below draws
    // with this theme so every mark on the face follows it.
    std::optional<Theme> inked;
    if ((latched || listening) && view.theme->latched_legend != view.theme->legend) {
        inked = *view.theme;
        inked->legend = inked->latched_legend;
    }
    const auto& t = inked ? *inked : *view.theme;
    const auto r = key.bounds;
    // The side begins below the resting face. A fixed-size face moves down
    // over it; changing face height would make a pressed key look stretched.
    Rect side{r.x, r.y + t.travel, r.width, r.height + t.depth - t.travel};
    rounded(cr, side, t.radius);
    gradient(cr, side, t.side_top, t.side_bottom, t.side_bottom);
    Rect face{r.x, r.y + amount * t.travel, r.width, r.height};
    if (latched || listening) {
        fill_latched(cr, t, face, t.radius);
    } else if (view.hovered_keys.contains(key.id) && !disabled) {
        rounded(cr, face, t.radius);
        if (light_theme(t)) {
            // A light key keeps its own shading, lightest at the top, and
            // takes on the hover colour evenly.
            gradient(cr, face, mix(t.top, t.hover, .7), mix(t.middle, t.hover, .7),
                     mix(t.bottom, t.hover, .7));
        } else {
            // The whole face lifts toward the hover colour, not only its top.
            gradient(cr, face, mix(t.hover, t.legend, .1), mix(t.middle, t.hover, .85),
                     mix(t.bottom, t.hover, .5));
        }
    } else {
        rounded(cr, face, t.radius);
        gradient(cr, face, t.top, t.middle, t.bottom);
    }
    if (t.latched_finish == "neon" && !latched && !listening && amount > 0) {
        // A pressed key lights the same tube, as far as it has travelled.
        neon_tube(cr, t, face, t.radius, amount);
    }
    if (listening) {
        // Five bars that stay in place; each eases from its old height to its
        // new one as a level passes from right to left, fading toward the
        // left; flat dots in silence.
        const auto& levels = view.dictation_levels;
        const double cx = face.x + face.width / 2, cy = face.y + face.height / 2;
        for (std::size_t i = 0; i + 1 < levels.size(); ++i) {
            const double level = levels[i] + (levels[i + 1] - levels[i]) * phase;
            const double height = 4 + 30 * level / 8.0;
            const double alpha =
                .35 + .65 * (static_cast<double>(i) + 1) / static_cast<double>(levels.size() - 1);
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, alpha);
            rounded(cr, {cx - 18 + static_cast<double>(i) * 8, cy - height / 2, 4, height}, 2);
            cairo_fill(cr);
        }
        if (view.dictation_recognizing) {
            draw_ring(cr, t, face, ring);
        }
        draw_where(cr, view, t, face);
        return;
    }
    if (key.icon != Icon::None) {
        const bool super_icon = key.icon == Icon::SteamFrame || key.icon == Icon::SteamOS;
        const double size = super_icon                                          ? 22
                            : key.icon == Icon::Backspace                       ? 18
                            : key.icon == Icon::Emoji || key.icon == Icon::Back ? 26
                                                                                : 32;
        draw_icon(cr, key.icon, face, disabled ? disabled_legend(t) : t.legend, size);
        if (key.icon == Icon::Mic && view.dictation_recognizing) {
            draw_ring(cr, t, face, ring);
        }
        if (key.icon == Icon::Mic) {
            draw_where(cr, view, t, face);
        }
        if (key.icon == Icon::Mic && view.dictation_download) {
            // A slim track along the face's foot fills as the model arrives,
            // and stays full while it is prepared.
            const Rect track{face.x + 14, face.y + face.height - 16, face.width - 28, 4};
            rounded(cr, track, 2);
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .15);
            cairo_fill(cr);
            const double share =
                *view.dictation_download < 0 ? 1 : std::clamp(*view.dictation_download, 0.0, 1.0);
            if (share > 0) {
                rounded(cr, {track.x, track.y, std::max(4.0, track.width * share), track.height}, 2);
                source(cr, mix(t.latched, t.legend, .35));
                cairo_fill(cr);
            }
        }
        return;
    }
    if (key.action_kind == ActionKind::Flick) {
        const auto custom = view.key_labels.find(key.id);
        const auto& center = custom != view.key_labels.end() ? custom->second : key.label;
        const auto slot = view.language->flick.find(key.action);
        if (slot == view.language->flick.end()) {
            label(cr, center, face, t.font_size * 1.3, view.language->font, t.legend, true);
            return;
        }
        // The tap kana in the middle, the four flick kana as hints on their sides.
        const auto& flick = slot->second;
        const double side = face.width * .22, band = face.height * .34;
        const auto hint = disabled_legend(t);
        label(cr, flick.left, {face.x, face.y, side, face.height}, t.small_font_size,
              view.language->font, hint, true);
        label(cr, flick.right, {face.x + face.width - side, face.y, side, face.height},
              t.small_font_size, view.language->font, hint, true);
        label(cr, flick.up, {face.x + side, face.y, face.width - 2 * side, band}, t.small_font_size,
              view.language->font, hint, true);
        label(cr, flick.down, {face.x + side, face.y + face.height - band, face.width - 2 * side, band},
              t.small_font_size, view.language->font, hint, true);
        label(cr, center,
              {face.x + side, face.y + band * .6, face.width - 2 * side, face.height - band * 1.2},
              t.font_size, view.language->font, t.legend, true);
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
    const auto legend = disabled ? disabled_legend(t) : t.legend;
    // Character legends of a right-to-left keymap; names such as "Enter ↵" stay as written.
    auto right_to_left = [&](const std::string& value) {
        return view.keymap->right_to_left() && g_utf8_strlen(value.c_str(), -1) <= 2;
    };
    const int shift = key.action_kind == ActionKind::Key && !view.key_labels.contains(key.id)
                          ? key_code(key.action)
                          : 0;
    if (secondary.empty() && (shift == KEY_LEFTSHIFT || shift == KEY_RIGHTSHIFT)) {
        // The arrow and the legend, centred together on the face.
        const double size = utility ? t.small_font_size : t.font_size;
        const double arrow = std::round(size * .75), gap = std::round(size * .35);
        const double width = text_width(cr, text, size, view.language->font);
        const double x = std::round(face.x + (face.width - arrow - gap - width) / 2);
        shift_arrow(cr, x, std::round(face.y + face.height / 2 + arrow * .55), arrow, legend,
                    view.keyboard->locked(shift));
        label(cr, text, {x + arrow + gap - 4, face.y, width + 8, face.height}, size, view.language->font,
              legend, true, PANGO_ALIGN_CENTER, false, right_to_left(text));
        return;
    }
    if (!secondary.empty()) {
        label(cr, secondary, {face.x, face.y + 3, face.width, face.height * .40}, t.small_font_size,
              view.language->font, legend, true, PANGO_ALIGN_CENTER, false, right_to_left(secondary));
        label(cr, text, {face.x, face.y + face.height * .4, face.width, face.height * .55},
              t.font_size * .85, view.language->font, legend, true, PANGO_ALIGN_CENTER, false,
              right_to_left(text));
    } else {
        label(cr, text, face, utility ? t.small_font_size : t.font_size, view.language->font, legend,
              true, PANGO_ALIGN_CENTER, false, right_to_left(text));
    }
}
} // namespace
std::optional<Rect> flick_preview_bounds(const PanelView& view) {
    const auto cells = flick_cells(view);
    if (!cells) {
        return {};
    }
    double left = (*cells)[0].x, top = (*cells)[0].y, right = left + (*cells)[0].width,
           bottom = top + (*cells)[0].height;
    for (std::size_t i = 1; i < cells->size(); ++i) {
        if (!view.flick_preview->text[i].empty()) {
            const auto& r = (*cells)[i];
            left = std::min(left, r.x);
            top = std::min(top, r.y);
            right = std::max(right, r.x + r.width);
            bottom = std::max(bottom, r.y + r.height);
        }
    }
    // Whole pixels, with room for the shadow, which spreads sideways by half
    // its depth and down by all of it.
    left = std::floor(left - flick_shadow / 2 - 1);
    top = std::floor(top - 3); // the gutter around each cap
    return Rect{left, top, std::ceil(right + flick_shadow / 2 + 1) - left,
                std::ceil(bottom + flick_shadow + 1) - top};
}
Rect panel_case_bounds(const PanelView& view) {
    const auto p = placement(view, true);
    // Custom geometry can leave only a few pixels after hiding its numpad.
    // Reserve the fixed main toolbar, and room for the scaled IME toolbar when
    // present. This same case is used by painting, controls and the VR mask.
    const bool ime = view.language && view.language->input_method != "xkb";
    const double minimum = ime ? 1300 : 630;
    // A layout narrower than the panel at this scale (flick) also narrows the
    // case, except in Settings, whose controls and cards span the full panel.
    const double slack =
        view.settings ? 0 : panel_width - 2 * view.theme->padding - view.layout->width * p.scale;
    const double width =
        std::max(minimum, panel_width - (view.layout->width - visible_layout_width(view)) * p.scale -
                              (slack > 1 ? slack : 0));
    return {(panel_width - width) / 2, 0, width, panel_height};
}
Rect panel_rect(const PanelView& view, Rect r) {
    const auto p = placement(view);
    return {p.x + r.x * p.scale, p.y + r.y * p.scale, r.width * p.scale, r.height * p.scale};
}
namespace {
constexpr double editor_font_size = 19;
constexpr double editor_padding_x = 14, editor_padding_y = 7;
// One Pango layout of the editor text, built the same way for painting and
// for App's cursor movement, so both agree on every line break.
PangoLayout* editor_layout(const PanelView& view) {
    // Measuring needs a context but no pixels; one tiny surface serves every call
    // on the VR thread.
    static cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    static cairo_t* context = cairo_create(surface);
    auto* layout = pango_cairo_create_layout(context);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, view.language->font.c_str());
    pango_font_description_set_absolute_size(font, editor_font_size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    const auto area = editor_text_area(view);
    pango_layout_set_width(layout, static_cast<int>(std::max(1.0, area.width) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    const auto& text = view.dictation->text;
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    return layout;
}
} // namespace
Rect editor_text_area(const PanelView& view) {
    const auto& r = view.dictation->bounds;
    const double band = view.dictation->candidates ? dictation_notice_height : 0;
    return {r.x + editor_padding_x, r.y + editor_padding_y, r.width - 2 * editor_padding_x,
            r.height - 2 * editor_padding_y - band};
}
EditorCaret editor_caret(const PanelView& view, std::size_t index) {
    auto* layout = editor_layout(view);
    PangoRectangle strong;
    pango_layout_get_cursor_pos(layout, static_cast<int>(index), &strong, nullptr);
    g_object_unref(layout);
    return {static_cast<double>(strong.x) / PANGO_SCALE, static_cast<double>(strong.y) / PANGO_SCALE,
            std::max(1.0, static_cast<double>(strong.height) / PANGO_SCALE)};
}
std::size_t editor_index_at(const PanelView& view, double x, double y) {
    auto* layout = editor_layout(view);
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(layout, static_cast<int>(x * PANGO_SCALE),
                             static_cast<int>(y * PANGO_SCALE), &index, &trailing);
    // A click past a character's middle puts the cursor after it.
    const auto& text = view.dictation->text;
    const char* p = text.c_str() + index;
    for (int i = 0; i < trailing && *p; ++i) {
        p = g_utf8_next_char(p);
    }
    g_object_unref(layout);
    return static_cast<std::size_t>(p - text.c_str());
}
std::size_t editor_move(const PanelView& view, std::size_t index, int direction) {
    auto* layout = editor_layout(view);
    int moved = 0, trailing = 0;
    pango_layout_move_cursor_visually(layout, TRUE, static_cast<int>(index), 0, direction, &moved,
                                      &trailing);
    g_object_unref(layout);
    const auto& text = view.dictation->text;
    // Pango marks the ends of the text with -1 and G_MAXINT.
    if (moved < 0) {
        return 0;
    }
    if (moved == G_MAXINT || static_cast<std::size_t>(moved) >= text.size()) {
        return text.size();
    }
    // A trailing position is after that many characters of the cluster at `moved`.
    const char* p = text.c_str() + moved;
    for (int i = 0; i < trailing && *p; ++i) {
        p = g_utf8_next_char(p);
    }
    return static_cast<std::size_t>(p - text.c_str());
}
std::pair<std::size_t, std::size_t> editor_line(const PanelView& view, std::size_t index) {
    auto* layout = editor_layout(view);
    int line_number = 0, x = 0;
    pango_layout_index_to_line_x(layout, static_cast<int>(index), false, &line_number, &x);
    auto* line = pango_layout_get_line_readonly(layout, line_number);
    std::size_t start = static_cast<std::size_t>(line->start_index);
    std::size_t end = start + static_cast<std::size_t>(line->length);
    g_object_unref(layout);
    const auto& text = view.dictation->text;
    // A wrapped line's trailing space and a hard break stay outside it, so End
    // lands before them as in other editors.
    while (end > start && (text[end - 1] == '\n' || (end < text.size() && text[end - 1] == ' '))) {
        --end;
    }
    return {start, end};
}
double editor_content_height(const PanelView& view) {
    auto* layout = editor_layout(view);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    return height;
}
bool compact_toolbar(const PanelView& view) {
    return !view.settings && panel_case_bounds(view).width < panel_width - 1;
}
namespace {
// The character counter sits in the toolbar row, left of the close button.
std::optional<Rect> dictation_counter_bounds(const PanelView& view) {
    for (const auto& control : view.controls) {
        if (control.action.kind == ControlKind::Close) {
            return Rect{control.bounds.x - 156, control.bounds.y, 144, control.bounds.height};
        }
    }
    return {};
}
// Settings surfaces, in the keyboard's language: raised panels and keycaps
// instead of outlines. Every colour derives from the theme.
// A soft shadow under a raised shape: a few widening, fading rings.
void soft_shadow(cairo_t* cr, const Rect& r, double radius, double depth) {
    for (int i = 1; i <= 4; ++i) {
        const double spread = i * depth / 4;
        rounded(cr, {r.x - spread / 2, r.y + spread / 2, r.width + spread, r.height + spread / 2},
                radius + spread);
        cairo_set_source_rgba(cr, 0, 0, 0, .09);
        cairo_fill(cr);
    }
}
void draw_settings_card(cairo_t* cr, const Theme& t, const Rect& r) {
    const double radius = t.radius + 4;
    soft_shadow(cr, r, radius, 8);
    rounded(cr, r, radius);
    source(cr, mix(t.surface, t.bottom, .45));
    cairo_fill(cr);
    // A faint light along the top edge, as on the keycaps.
    cairo_move_to(cr, r.x + radius, r.y + 1.5);
    cairo_line_to(cr, r.x + r.width - radius, r.y + 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .07);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}
// A keycap face for buttons and dropdowns: the keys' own gradient and hover.
void draw_cap(cairo_t* cr, const Theme& t, const Rect& r, bool hovered, bool selected, bool enabled) {
    if (selected && enabled) {
        fill_latched(cr, t, r, t.radius);
        return;
    }
    rounded(cr, r, t.radius);
    if (!enabled) {
        source(cr, mix(t.bottom, t.surface, .4));
        cairo_fill(cr);
    } else {
        if (hovered && light_theme(t)) {
            // As on the keys: a light cap keeps its shading under the hover colour.
            gradient(cr, r, mix(t.top, t.hover, .7), mix(t.middle, t.hover, .7),
                     mix(t.bottom, t.hover, .7));
        } else {
            gradient(cr, r, hovered ? t.hover : t.top, t.middle, t.bottom);
        }
    }
}
// A switch in place of a checkbox: a recessed track and a keycap knob that
// moves right, over the latched colour, when on.
void draw_switch(cairo_t* cr, const Theme& t, const Rect& r, bool on, bool hovered, bool enabled) {
    constexpr double width = 56, height = 30;
    const Rect track{r.x + r.width - width, r.y + (r.height - height) / 2, width, height};
    cairo_push_group(cr);
    if (on) {
        fill_latched(cr, t, track, height / 2);
    } else {
        rounded(cr, track, height / 2);
        source(cr, recede(t, t.surface, .45));
        cairo_fill_preserve(cr);
        cairo_save(cr);
        cairo_clip(cr);
        auto* shade = cairo_pattern_create_linear(0, track.y, 0, track.y + 8);
        cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, light_theme(t) ? .12 : .5);
        cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
        cairo_set_source(cr, shade);
        cairo_paint(cr);
        cairo_pattern_destroy(shade);
        cairo_restore(cr);
        // The well's rim, so the track reads on a dark card.
        rounded(cr, {track.x + .5, track.y + .5, track.width - 1, track.height - 1}, height / 2);
        source(cr, mix(t.surface, t.bottom, .8));
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    const double knob = height - 6;
    const Rect cap{on ? track.x + track.width - knob - 3 : track.x + 3, track.y + 3, knob, knob};
    rounded(cr, {cap.x, cap.y + 1, cap.width, cap.height}, knob / 2);
    cairo_set_source_rgba(cr, 0, 0, 0, .3);
    cairo_fill(cr);
    rounded(cr, cap, knob / 2);
    // Off, the knob is a light keycap; on, it takes the latched legend's colour.
    const Color ink = on ? t.latched_legend : t.legend;
    const Color top = on ? ink : mix(t.top, t.legend, .35);
    gradient(cr, cap, hovered && enabled ? mix(top, ink, .3) : top,
             on ? ink : mix(t.middle, t.legend, .25),
             on ? mix(ink, t.bottom, .25) : mix(t.bottom, t.legend, .1));
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, enabled ? 1 : .4);
}
// A well recessed into the case, the opposite of a keycap: darker than the
// case, shaded under its top edge, with a faint rim and a light catch on its
// lower lip. Every colour derives from the theme. Returns the well's colour.
Color draw_well(cairo_t* cr, const Theme& t, const Rect& r) {
    const double radius = t.radius + 2;
    const Color well = recede(t, t.surface, .45);
    rounded(cr, r, radius);
    source(cr, well);
    cairo_fill_preserve(cr);
    cairo_save(cr);
    cairo_clip(cr);
    auto* shade = cairo_pattern_create_linear(0, r.y, 0, r.y + 10);
    // A light field needs only a hint of shade to read as inset.
    cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, light_theme(t) ? .12 : .55);
    cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
    cairo_set_source(cr, shade);
    cairo_paint(cr);
    cairo_pattern_destroy(shade);
    cairo_restore(cr);
    // The rim takes most of its colour from the keys, so it reads as an edge
    // even on a case too dark for the well itself to show.
    rounded(cr, {r.x + .5, r.y + .5, r.width - 1, r.height - 1}, radius);
    cairo_set_source_rgba(cr, t.surface.r + (t.bottom.r - t.surface.r) * .8,
                          t.surface.g + (t.bottom.g - t.surface.g) * .8,
                          t.surface.b + (t.bottom.b - t.surface.b) * .8, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    // The light catch: a hairline just inside the bottom edge.
    cairo_move_to(cr, r.x + radius, r.y + r.height - 1.5);
    cairo_line_to(cr, r.x + r.width - radius, r.y + r.height - 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .06);
    cairo_stroke(cr);
    return well;
}
// A transparent case is left unfilled behind the keyboard; Settings keeps it.
bool clear_case(const PanelView& view) {
    return view.transparent_background && !view.settings;
}
// Over a clear case, text drawn on the case itself gets a piece of it back:
// a plate in the case colour, fitted to the text and kept inside `area`, so
// partial repaints of that area cover it. Text wider than the area (wrapped
// or ellipsized) takes the whole area. Nothing over an opaque case, where the
// text is not even measured: this runs in every repaint pass. `extra` widens
// the plate for an icon beside the text.
void case_plate(cairo_t* cr, const PanelView& view, const Rect& area, const std::string& text,
                double size, PangoAlignment alignment, double extra = 0) {
    if (!clear_case(view) || text.empty()) {
        return;
    }
    const double ink = text_width(cr, text, size, view.language->font) + extra;
    const auto& t = *view.theme;
    constexpr double pad = 14;
    const bool fills = ink + 8 > area.width;
    const double width = std::min(area.width, ink + 2 * pad);
    const double height = fills ? area.height : std::min(area.height, size + 16);
    // label() sets text 4 px inside the area's left or right edge.
    const double x = alignment == PANGO_ALIGN_CENTER ? area.x + (area.width - width) / 2
                     : alignment == PANGO_ALIGN_LEFT ? area.x + 4 - pad
                                                     : area.x + area.width - 4 + pad - width;
    const double left = std::clamp(x, area.x, area.x + area.width - width);
    rounded(cr, {left, area.y + (area.height - height) / 2, width, height}, t.radius + 2);
    cairo_set_source_rgba(cr, t.surface.r, t.surface.g, t.surface.b, case_plate_alpha);
    cairo_fill(cr);
}
// The emoji grid's well reaches a little past the cells, like the editor's.
Rect emoji_well(const Rect& grid) {
    return {grid.x - 6, grid.y - 6, grid.width + 12, grid.height + 12};
}
void draw_editor(cairo_t* cr, const PanelView& view) {
    const auto& t = *view.theme;
    const auto& editor = *view.dictation;
    const auto& r = editor.bounds;
    const double radius = t.radius + 2;
    const auto well = draw_well(cr, t, r);

    const auto area = editor_text_area(view);
    cairo_save(cr);
    cairo_rectangle(cr, area.x, area.y, area.width, area.height);
    cairo_clip(cr);
    // The measuring layout itself, so the text lands exactly where App's
    // cursor moves expect it.
    auto* layout = editor_layout(view);
    const Color amber = t.warning;
    // Underlines a byte range of the text on each of its line pieces.
    auto underline = [&](std::size_t begin, std::size_t end, double thickness) {
        auto* iter = pango_layout_get_iter(layout);
        do {
            auto* line = pango_layout_iter_get_line_readonly(iter);
            const auto start = static_cast<std::size_t>(line->start_index);
            if (start >= end) {
                break;
            }
            if (start + static_cast<std::size_t>(line->length) <= begin) {
                continue;
            }
            int *ranges = nullptr, count = 0;
            pango_layout_line_get_x_ranges(line, static_cast<int>(begin), static_cast<int>(end), &ranges,
                                           &count);
            const int baseline = pango_layout_iter_get_baseline(iter);
            for (int i = 0; i < count; ++i) {
                const double x0 = area.x + ranges[2 * i] / static_cast<double>(PANGO_SCALE);
                const double x1 = area.x + ranges[2 * i + 1] / static_cast<double>(PANGO_SCALE);
                cairo_rectangle(cr, x0, area.y - editor.scroll + baseline / PANGO_SCALE + 3, x1 - x0,
                                thickness);
            }
            g_free(ranges);
            cairo_fill(cr);
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);
    };
    if (editor.uncertain) {
        // Text that may already have arrived: an amber underline, quiet
        // enough to read through.
        cairo_set_source_rgba(cr, amber.r, amber.g, amber.b, .75);
        underline(0, editor.uncertain, 2);
    }
    if (editor.preedit_end > editor.preedit_begin) {
        // A reading not yet committed, underlined as in a text field; the
        // segment being converted more strongly.
        cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .55);
        underline(editor.preedit_begin, editor.preedit_end, 1.5);
        if (editor.active_end > editor.active_begin) {
            source(cr, t.legend);
            underline(editor.active_begin, editor.active_end, 3);
        }
    }
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b,
                          editor.sending || editor.capturing ? .5 : .92);
    cairo_move_to(cr, area.x, area.y - editor.scroll);
    pango_cairo_show_layout(cr, layout);
    PangoRectangle caret;
    pango_layout_get_cursor_pos(layout, static_cast<int>(editor.cursor), &caret, nullptr);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    if (!editor.sending) {
        rounded(cr,
                {area.x + caret.x / PANGO_SCALE - 1, area.y - editor.scroll + caret.y / PANGO_SCALE + 2,
                 2, std::max(1.0, caret.height / static_cast<double>(PANGO_SCALE) - 4)},
                1);
        source(cr, t.legend);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    if (height > area.height) {
        // A slim indicator in the well's right margin, as quiet as a hint.
        const double track = r.height - 2 * radius;
        const double thumb = std::max(18.0, track * area.height / height);
        const double y = r.y + radius +
                         (track - thumb) * std::clamp(editor.scroll / (height - area.height), 0.0, 1.0);
        rounded(cr, {r.x + r.width - 7, y, 3, thumb}, 1.5);
        cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .22);
        cairo_fill(cr);
    }
    if (!editor.notice.empty() || editor.candidates) {
        // A band across the well's foot, darker than the well, so it reads as
        // laid over the text rather than part of it. Candidates are controls
        // drawn on it.
        const Rect band{r.x + 1, r.y + r.height - dictation_notice_height, r.width - 2,
                        dictation_notice_height - 1};
        cairo_save(cr);
        rounded(cr, r, radius);
        cairo_clip(cr);
        cairo_rectangle(cr, band.x, band.y, band.width, band.height);
        source(cr, recede(t, well, .4));
        cairo_fill(cr);
        cairo_move_to(cr, band.x, band.y + .5);
        cairo_line_to(cr, band.x + band.width, band.y + .5);
        cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .08);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (!editor.notice.empty()) {
        const Rect band{r.x + 1, r.y + r.height - dictation_notice_height, r.width - 2,
                        dictation_notice_height - 1};
        label(cr, editor.notice, {band.x + editor_padding_x, band.y, band.width - 280, band.height},
              t.small_font_size * 1.3, view.language->font,
              editor.notice_error ? amber : Color{t.legend.r, t.legend.g, t.legend.b}, false,
              PANGO_ALIGN_LEFT);
    }
    if (const auto bounds = dictation_counter_bounds(view)) {
        // On a clear case the count and its icon share one plate; the count
        // moves in from the plate's right edge as it sits in from the left.
        auto counter = *bounds;
        const double size = t.small_font_size * 1.3;
        if (clear_case(view)) {
            counter.width -= 10;
            case_plate(cr, view, *bounds, editor.counter, size, PANGO_ALIGN_RIGHT,
                       editor.where == Icon::None ? 0 : 32);
        }
        // Quiet while there is room; amber once dictation has reached its limit.
        label(cr, editor.counter, counter, size, view.language->font,
              editor.at_limit ? amber : disabled_legend(t), false, PANGO_ALIGN_RIGHT);
        if (editor.where != Icon::None) {
            // Where speech is recognized, just left of the count.
            const double width = text_width(cr, editor.counter, size, view.language->font);
            draw_icon(cr, editor.where,
                      {counter.x + counter.width - 4 - width - 32, counter.y + (counter.height - 26) / 2,
                       26, 26},
                      disabled_legend(t), 16);
        }
    }
}
} // namespace
std::vector<Rect> panel_hit_areas(const PanelView& view, bool held, std::size_t limit) {
    const auto body = panel_case_bounds(view);
    if (!clear_case(view) || !view.laser_through_gaps || held || limit < 2) {
        return {body};
    }
    // Everything drawn on the case that takes a press, with a little slack
    // for a laser wobbling at an edge. Keys in a row overlap through it.
    std::vector<Rect> areas;
    auto add = [&](const Rect& r) {
        const double left = std::max(body.x, r.x - 4), top = std::max(body.y, r.y - 4);
        const double right = std::min(body.x + body.width, r.x + r.width + 4);
        const double bottom = std::min(body.y + body.height, r.y + r.height + 4);
        if (right > left && bottom > top) {
            areas.push_back({left, top, right - left, bottom - top});
        }
    };
    for (const auto& key : view.layout->keys) {
        if (!hidden_key(view, key)) {
            add(panel_rect(view, key.bounds));
        }
    }
    for (const auto& control : view.controls) {
        if (control.style != ControlStyle::MoveHandle) {
            add(control.bounds);
        }
    }
    if (view.emoji) {
        add(emoji_well(view.emoji->grid));
        add(view.emoji->tabs);
        // A top-row emoji's tones rise above the grid, over the gap below the tabs.
        if (view.emoji->popup) {
            add(view.emoji->popup->bounds);
        }
    }
    if (view.dictation) {
        add(view.dictation->bounds);
    }
    auto unite = [](const Rect& a, const Rect& b) {
        const double left = std::min(a.x, b.x), top = std::min(a.y, b.y);
        return Rect{left, top, std::max(a.x + a.width, b.x + b.width) - left,
                    std::max(a.y + a.height, b.y + b.height) - top};
    };
    // First the cheap pass: neighbours in one row that touch become one strip,
    // leaving a few dozen strips rather than a hundred keys.
    std::sort(areas.begin(), areas.end(),
              [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<Rect> strips;
    for (const auto& r : areas) {
        if (!strips.empty()) {
            auto& last = strips.back();
            if (std::abs(last.y - r.y) < .5 && std::abs(last.height - r.height) < .5 &&
                r.x <= last.x + last.width) {
                last = unite(last, r);
                continue;
            }
        }
        strips.push_back(r);
    }
    // Then, while OpenVR's primitive limit is exceeded, merge the two strips
    // whose bounding rectangle adds the least uncovered space: a row's gap
    // to the next row, not a gap the size of a key cluster.
    auto overlap = [](const Rect& a, const Rect& b) {
        const double width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
        const double height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
        return width > 0 && height > 0 ? width * height : 0;
    };
    while (strips.size() > limit) {
        std::size_t best_i = 0, best_j = 1;
        double best = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < strips.size(); ++i) {
            for (std::size_t j = i + 1; j < strips.size(); ++j) {
                const auto u = unite(strips[i], strips[j]);
                const double waste = u.width * u.height - strips[i].width * strips[i].height -
                                     strips[j].width * strips[j].height + overlap(strips[i], strips[j]);
                if (waste < best) {
                    best = waste;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        strips[best_i] = unite(strips[best_i], strips[best_j]);
        strips.erase(strips.begin() + static_cast<std::ptrdiff_t>(best_j));
    }
    return strips.empty() ? std::vector<Rect>{body} : strips;
}
PanelRenderer::PanelRenderer() {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, texture_height);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
        throw std::runtime_error("cannot allocate keyboard image");
    }
}
PanelRenderer::~PanelRenderer() {
    if (trail_) {
        cairo_surface_destroy(trail_);
    }
    cairo_surface_destroy(surface_);
}
std::pair<double, double> PanelRenderer::layout_point(const PanelView& view, double x, double y) const {
    const auto p = placement(view);
    return {(x - p.x) / p.scale, (y - p.y) / p.scale};
}
const Key* PanelRenderer::hit_key(const PanelView& view, double x, double y) const {
    if (view.settings) {
        return nullptr;
    }
    const auto p = placement(view);
    x = (x - p.x) / p.scale;
    y = (y - p.y) / p.scale;
    for (const auto& key : view.layout->keys) {
        if (!hidden_key(view, key) && key.bounds.contains(x, y)) {
            return &key;
        }
    }
    return nullptr;
}
bool PanelRenderer::paint(const PanelView& view, double now, bool force_full) {
    const auto& t = *view.theme;
    const auto modifiers = view.keyboard->modifiers();
    const bool shift_locked =
        view.keyboard->locked(KEY_LEFTSHIFT) || view.keyboard->locked(KEY_RIGHTSHIFT);
    // A hovered dropdown choice whose name does not fit scrolls. A new one
    // starts from the beginning.
    std::optional<Rect> marquee_text;
    double marquee_shift = 0;
    std::optional<double> marquee_due;
    {
        const Control* choice = nullptr;
        for (const auto& control : view.controls) {
            if (control.style == ControlStyle::MenuItem && view.hovered_controls.contains(control.id)) {
                choice = &control;
            }
        }
        const std::string hovered = choice ? choice->id : "";
        if (hovered != marquee_) {
            marquee_ = hovered;
            marquee_since_ = now;
            marquee_shown_ = -1;
        }
        if (choice && view.settings) {
            const auto& r = choice->bounds;
            const Rect text{r.x + 20, r.y, r.width - 40, r.height};
            auto* measure = cairo_create(surface_);
            const double overflow =
                text_width(measure, choice->label, 21, view.language->font) - (text.width - 8);
            cairo_destroy(measure);
            if (overflow > 0) {
                const auto step = marquee_step(overflow, now - marquee_since_);
                marquee_text = text;
                marquee_shift = step.offset;
                marquee_due = now + step.wait;
            }
        }
    }
    // Swipe alternatives come and go with every swiped word; they sit in the
    // toolbar, so they repaint their own area instead of the whole scene.
    auto without_candidates = [](const std::vector<Control>& controls) {
        std::vector<Control> result;
        std::copy_if(controls.begin(), controls.end(), std::back_inserter(result),
                     [](const Control& c) { return c.action.kind != ControlKind::SwipeCandidate; });
        return result;
    };
    const bool controls_changed =
        previous_ && without_candidates(previous_->view.controls) != without_candidates(view.controls);
    const bool scene_changed =
        !previous_ || previous_->layout != *view.layout || previous_->theme != t ||
        previous_->language != *view.language || previous_->modifiers != modifiers ||
        previous_->caps != view.keyboard->caps() || previous_->num != view.keyboard->num() ||
        previous_->shift_locked != shift_locked || controls_changed ||
        previous_->view.status != view.status || previous_->view.settings != view.settings ||
        previous_->view.composing != view.composing || previous_->view.hide_numpad != view.hide_numpad ||
        previous_->view.transparent_background != view.transparent_background ||
        // No pixel changes, but the VR loop sets the laser mask with each paint.
        previous_->view.laser_through_gaps != view.laser_through_gaps ||
        previous_->view.key_labels != view.key_labels ||
        previous_->view.disabled_keys != view.disabled_keys ||
        previous_->view.emoji.has_value() != view.emoji.has_value() ||
        previous_->view.dictation.has_value() != view.dictation.has_value() ||
        previous_->view.dictation_available != view.dictation_available;
    // Settings repaints whole, except when nothing in it changed but a
    // scrolling choice name: then only that name's box, once it moved a pixel.
    const bool settings_still =
        previous_ && view.settings && previous_->view.settings &&
        previous_->view.controls == view.controls && previous_->view.cards == view.cards &&
        previous_->view.field_labels == view.field_labels && previous_->view.popup == view.popup &&
        previous_->view.status == view.status &&
        previous_->view.hovered_controls == view.hovered_controls &&
        previous_->view.address == view.address && previous_->view.confirm == view.confirm &&
        previous_->view.favorites == view.favorites;
    bool full = force_full || scene_changed || (view.settings && !settings_still);
    std::vector<Rect> damage;
    animating_ = false;
    next_change_ = now;
    bool continuous = false; // an animation that changes every frame
    if (const auto handle = panel_move_handle_bounds(view)) {
        const bool held = std::any_of(view.controls.begin(), view.controls.end(), [](const Control& c) {
            return c.style == ControlStyle::MoveHandle && c.selected;
        });
        const double target = held || view.hovered_controls.contains("move-handle") ? 1 : 0;
        const double old_amount = move_handle_.amount;
        const bool changed = target != move_handle_.target;
        if (changed) {
            move_handle_.from = move_handle_.amount;
            move_handle_.target = target;
            move_handle_.started = now;
        }
        const double progress = std::clamp((now - move_handle_.started) / .08, 0.0, 1.0);
        move_handle_.amount =
            move_handle_.from + (target - move_handle_.from) * handle_ease_out(progress);
        animating_ |= progress < 1;
        continuous |= progress < 1;
        full |= changed || move_handle_.amount != old_amount;
    } else {
        move_handle_ = {};
    }
    for (const auto& key : view.layout->keys) {
        if (hidden_key(view, key)) {
            continue;
        }
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
        continuous |= !view.settings && progress < 1;
        bool mic_changed = false;
        if (key.icon == Icon::Mic) {
            // The bars ease while a level is passing through.
            const double phase = view.dictation_capturing ? mic_phase(view, now) : 1;
            // Settings covers the mic key, so neither animation repaints under it.
            animating_ |= !view.settings && phase < 1;
            // The recognition ring steps at 30 fps while it shows.
            const double ring = view.dictation_recognizing ? std::floor(now * 30) / 30 : 0;
            animating_ |= !view.settings && view.dictation_recognizing;
            mic_changed =
                !full && (previous_->view.dictation_capturing != view.dictation_capturing ||
                          previous_->view.dictation_levels != view.dictation_levels ||
                          previous_->view.dictation_download != view.dictation_download ||
                          previous_->view.dictation_recognizing != view.dictation_recognizing ||
                          previous_->view.dictation_where != view.dictation_where ||
                          ring != ring_time_ || phase != mic_phase_);
            mic_phase_ = phase;
            ring_time_ = ring;
        }
        // Settings covers the keys, and key_bounds_ is empty while it shows.
        if (!view.settings && !full &&
            (changed_target || animation.amount != old_amount || mic_changed ||
             previous_->view.hovered_keys.contains(key.id) != view.hovered_keys.contains(key.id))) {
            damage.push_back(key_bounds_.at(key.id));
        }
    }
    // The mic key's bars and ring step at 30 fps; between steps a 60 Hz
    // frame would rebuild and compare the whole view for no new pixel.
    next_change_ = continuous ? now : (std::floor(now * 30) + 1) / 30;
    if (marquee_due) {
        next_change_ = animating_ ? std::min(next_change_, *marquee_due) : *marquee_due;
        animating_ = true;
        if (!full && marquee_shift != marquee_shown_) {
            damage.push_back(pixel_bounds(*marquee_text));
        }
    }
    // A clip edge through a key can drop parts of its legend glyphs. Damage
    // a changed area together with every key it touches, each whole, as
    // separate pixel-aligned rectangles: the clip's edges then run only
    // through the gaps between keys or along key bounds, and nothing chains
    // across the keyboard the way one bounding rectangle would.
    std::vector<Rect> areas;
    auto damage_area = [&](Rect r) {
        areas.push_back(r);
        for (const auto& [id, bounds] : key_bounds_) {
            if (overlaps(bounds, r)) {
                damage.push_back(bounds);
            }
        }
    };
    // A trail can cross many keys. While it grows, only its newest segments
    // change, including their join at the old end; otherwise (a new or ended
    // swipe) repaint everything either trail covered.
    if (!full && previous_->view.swipe_trail != view.swipe_trail) {
        double left = panel_width, top = panel_height, right = 0, bottom = 0;
        const auto& old_trail = previous_->view.swipe_trail;
        const bool grows = !old_trail.empty() && view.swipe_trail.size() > old_trail.size() &&
                           std::equal(old_trail.begin(), old_trail.end(), view.swipe_trail.begin());
        auto extend = [&](auto begin, auto end) {
            for (auto point = begin; point != end; ++point) {
                left = std::min(left, point->first);
                top = std::min(top, point->second);
                right = std::max(right, point->first);
                bottom = std::max(bottom, point->second);
            }
        };
        if (grows) {
            extend(view.swipe_trail.begin() + static_cast<std::ptrdiff_t>(old_trail.size() - 1),
                   view.swipe_trail.end());
        } else {
            extend(old_trail.begin(), old_trail.end());
            extend(view.swipe_trail.begin(), view.swipe_trail.end());
        }
        if (right >= left) {
            // Laser points are fractional. Like key bounds, round outwards to
            // whole pixels: an antialiased clip edge would partly clear pixels
            // and leave dark, translucent seams that build up frame by frame.
            left = std::floor(left - 8);
            top = std::floor(top - 8);
            damage_area({left, top, std::ceil(right + 8) - left, std::ceil(bottom + 8) - top});
        }
    }
    if (!full && previous_->view.controls != view.controls) {
        const std::array<const std::vector<Control>*, 2> both{&previous_->view.controls, &view.controls};
        for (const auto* controls : both) {
            for (const auto& c : *controls) {
                if (c.action.kind == ControlKind::SwipeCandidate) {
                    const double left = std::floor(c.bounds.x - 2), top = std::floor(c.bounds.y - 2);
                    damage_area({left, top, std::ceil(c.bounds.x + c.bounds.width + 2) - left,
                                 std::ceil(c.bounds.y + c.bounds.height + 2) - top});
                }
            }
        }
    }
    // The reading line and the flick preview repaint only their own area. The
    // preview's old and new extents both count; it may lie beyond the case.
    if (!full && previous_->view.preedit != view.preedit) {
        const auto body = panel_case_bounds(view);
        damage_area({std::floor(body.x + 20), 94, std::ceil(body.width - 40), 40});
    }
    if (!full && previous_->view.flick_preview != view.flick_preview) {
        auto old = view;
        old.flick_preview = previous_->view.flick_preview;
        for (const auto& bounds : {flick_preview_bounds(old), flick_preview_bounds(view)}) {
            if (bounds) {
                damage_area(*bounds);
            }
        }
    }
    // The picker's grid, tabs and tone popup repaint only their own areas.
    if (!full && previous_->view.emoji != view.emoji) {
        const auto& old = *previous_->view.emoji;
        const auto& now = *view.emoji;
        // A hover or press changes only its own cell or tab: with nothing moved
        // or redrawn, repaint just those, not the whole grid or tab row.
        auto same_place = [](const auto& a, const auto& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](const auto& x, const auto& y) { return x.bounds == y.bounds; });
        };
        const bool grid_still = old.grid == now.grid && old.hint == now.hint &&
                                old.hint_bounds == now.hint_bounds &&
                                old.favorites_hint == now.favorites_hint &&
                                old.favorites_hint_bounds == now.favorites_hint_bounds &&
                                old.dividers == now.dividers && same_place(old.cells, now.cells);
        if (grid_still) {
            for (std::size_t i = 0; i < now.cells.size(); ++i) {
                if (old.cells[i] != now.cells[i]) {
                    damage_area(pixel_bounds(now.cells[i].bounds, 1));
                }
            }
        } else {
            damage_area(pixel_bounds(now.grid, 1));
        }
        if (old.tabs == now.tabs && old.scrolling_tabs == now.scrolling_tabs &&
            same_place(old.tab_list, now.tab_list)) {
            for (std::size_t i = 0; i < now.tab_list.size(); ++i) {
                if (old.tab_list[i] != now.tab_list[i]) {
                    damage_area(pixel_bounds(now.tab_list[i].bounds, 1));
                }
            }
        } else {
            damage_area(pixel_bounds(now.tabs, 1));
        }
        // Tone images can arrive after the popup opens, marking its choices
        // ready; above the grid's top row it lies outside the grid's rectangle.
        if (old.popup != now.popup) {
            for (const auto* popup : {&old.popup, &now.popup}) {
                if (*popup) {
                    damage_area(pixel_bounds((*popup)->bounds, tone_shadow + 1));
                }
            }
        }
        // A lifted emoji follows the laser anywhere over the panel.
        if (old.lifted != now.lifted) {
            for (const auto* lifted : {&old.lifted, &now.lifted}) {
                if (*lifted) {
                    damage_area(pixel_bounds((*lifted)->bounds, tone_shadow + 1));
                }
            }
        }
    }
    // The editor and its counter repaint only their own areas.
    if (!full && previous_->view.dictation != view.dictation) {
        damage_area(pixel_bounds(view.dictation->bounds, 2));
        if (previous_->view.dictation->counter != view.dictation->counter) {
            if (const auto counter = dictation_counter_bounds(view)) {
                damage_area(pixel_bounds(*counter, 1));
            }
        }
    }
    // Toolbar changes are infrequent and may overlap custom key geometry.
    if (!full) {
        for (const auto& control : view.controls) {
            if (previous_->view.hovered_controls.contains(control.id) !=
                view.hovered_controls.contains(control.id)) {
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
                if (hidden_key(view, key)) {
                    continue;
                }
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
        previous_ = Snapshot{
            *view.layout,         t,           *view.language, view, modifiers, view.keyboard->caps(),
            view.keyboard->num(), shift_locked};
        // No animation state from a removed key should survive a profile reload.
        std::erase_if(animations_, [&](const auto& item) {
            return std::none_of(view.layout->keys.begin(), view.layout->keys.end(), [&](const Key& key) {
                return key.id == item.first && !hidden_key(view, key);
            });
        });
    } else {
        previous_->view = view;
    }
    if (!full && damage.empty() && areas.empty()) {
        return false;
    }
    marquee_shown_ = marquee_text ? marquee_shift : -1;
    auto* cr = cairo_create(surface_);
    cairo_translate(cr, 0, popup_margin);
    // Cairo composites antialiased strokes and glyphs slightly differently
    // under a clip of several rectangles, or a clip edge through a glyph. So
    // repaint each damaged rectangle in its own pass with a single box clip,
    // each pass clearing and redrawing everything in its box: first the
    // changed areas, then every touched key whole, so an area edge through a
    // key is painted over by that key's exact pass.
    std::vector<Rect> passes;
    if (!full) {
        passes = areas;
        for (const auto& r : damage) {
            if (std::find(passes.begin(), passes.end(), r) == passes.end()) {
                passes.push_back(r);
            }
        }
    }
    if (view.swipe_trail.size() > 1 && !view.settings && !trail_) {
        trail_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, texture_height);
    }
    if (trail_) {
        const auto& points = view.settings ? decltype(view.swipe_trail){} : view.swipe_trail;
        const bool grows = !trail_points_.empty() && points.size() >= trail_points_.size() &&
                           std::equal(trail_points_.begin(), trail_points_.end(), points.begin());
        auto* trail = cairo_create(trail_);
        cairo_translate(trail, 0, popup_margin);
        if (!grows) {
            if (trail_extent_) {
                const auto& r = *trail_extent_;
                cairo_rectangle(trail, r.x, r.y, r.width, r.height);
                cairo_set_operator(trail, CAIRO_OPERATOR_CLEAR);
                cairo_fill(trail);
                cairo_set_operator(trail, CAIRO_OPERATOR_OVER);
            }
            trail_extent_.reset();
            trail_points_.clear();
        }
        if (points.size() > 1) {
            cairo_set_line_width(trail, 6);
            cairo_set_line_cap(trail, CAIRO_LINE_CAP_ROUND);
            source(trail, t.legend);
            auto extent =
                trail_extent_.value_or(Rect{points.front().first, points.front().second, 0, 0});
            // Each segment is its own stroke with round caps, so the joins
            // match a joined path while earlier pixels stay as they were.
            for (std::size_t i = std::max<std::size_t>(trail_points_.size(), 1); i < points.size();
                 ++i) {
                const auto [ax, ay] = points[i - 1];
                const auto [bx, by] = points[i];
                cairo_move_to(trail, ax, ay);
                cairo_line_to(trail, bx, by);
                cairo_stroke(trail);
                const double left = std::floor(std::min({extent.x, ax - 8, bx - 8}));
                const double top = std::floor(std::min({extent.y, ay - 8, by - 8}));
                const double right = std::ceil(std::max({extent.x + extent.width, ax + 8, bx + 8}));
                const double bottom = std::ceil(std::max({extent.y + extent.height, ay + 8, by + 8}));
                extent = {left, top, right - left, bottom - top};
            }
            trail_extent_ = extent;
            trail_points_ = points;
        }
        cairo_destroy(trail);
        cairo_surface_flush(trail_);
    }
    auto draw = [&](const std::vector<Rect>& pass) {
        // Clear and restore the case only inside the damaged pixels. Any neighboring
        // key intersecting that region is repainted in the original stacking order.
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        const auto body = panel_case_bounds(view);
        // Settings keeps the case so its cards stay legible; the keyboard's
        // transparent background shows the dashboard or app behind the keys.
        if (!clear_case(view)) {
            rounded(cr, body, t.surface_radius);
            source(cr, t.surface);
            cairo_fill(cr);
        }
        if (!view.settings) {
            const auto p = placement(view);
            cairo_save(cr);
            cairo_translate(cr, p.x, p.y);
            cairo_scale(cr, p.scale, p.scale);
            for (const auto& key : view.layout->keys) {
                if (hidden_key(view, key)) {
                    continue;
                }
                const auto& bounds = key_bounds_.at(key.id);
                const bool intersects =
                    full || std::any_of(pass.begin(), pass.end(), [&](const Rect& r) {
                        return bounds.x < r.x + r.width && bounds.x + bounds.width > r.x &&
                               bounds.y < r.y + r.height && bounds.y + bounds.height > r.y;
                    });
                if (intersects) {
                    draw_key(cr, view, key, animations_.at(key.id).amount, mic_phase_, ring_time_);
                }
            }
            cairo_restore(cr);
        }
        if (trail_extent_) {
            const auto& r = *trail_extent_;
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_clip(cr);
            cairo_set_source_surface(cr, trail_, 0, -popup_margin);
            // Legend color stays readable over both key faces and the case.
            cairo_paint_with_alpha(cr, .6);
            cairo_restore(cr);
        }
        auto emoji_image = [&](const std::string& text, Rect cell) {
            auto* image = view.emoji_glyphs ? view.emoji_glyphs->image(text) : nullptr;
            if (!image) {
                return; // Still being drawn; the cell stays empty until then.
            }
            const int size = cairo_image_surface_get_width(image);
            cairo_set_source_surface(cr, image, std::round(cell.x + (cell.width - size) / 2),
                                     std::round(cell.y + (cell.height - size) / 2));
            cairo_paint(cr);
        };
        if (view.emoji && !view.settings) {
            const auto& picker = *view.emoji;
            for (const auto& tab : picker.tab_list) {
                cairo_save(cr);
                if (tab.icon != Icon::Recent) {
                    const auto& r = picker.scrolling_tabs;
                    cairo_rectangle(cr, r.x, r.y, r.width, r.height);
                    cairo_clip(cr);
                }
                if (tab.active) {
                    fill_latched(cr, t, tab.bounds, t.radius);
                } else {
                    rounded(cr, tab.bounds, t.radius);
                    source(cr, tab.pressed || tab.hovered ? t.hover : t.bottom);
                    cairo_fill(cr);
                }
                draw_icon(cr, tab.icon, tab.bounds, tab.active ? t.latched_legend : t.legend, 24);
                cairo_restore(cr);
            }
            draw_well(cr, t, emoji_well(picker.grid));
            cairo_save(cr);
            cairo_rectangle(cr, picker.grid.x, picker.grid.y, picker.grid.width, picker.grid.height);
            cairo_clip(cr);
            for (const double x : picker.dividers) {
                // A hairline in the legend colour, fading out toward the
                // grid's top and bottom, like the line under a Settings title.
                const auto& g = picker.grid;
                const double top = g.y + 10, bottom = g.y + g.height - 10, line = std::round(x);
                auto* fade = cairo_pattern_create_linear(0, top, 0, bottom);
                const auto& c = t.legend;
                cairo_pattern_add_color_stop_rgba(fade, 0, c.r, c.g, c.b, 0);
                cairo_pattern_add_color_stop_rgba(fade, .5, c.r, c.g, c.b, light_theme(t) ? .26 : .2);
                cairo_pattern_add_color_stop_rgba(fade, 1, c.r, c.g, c.b, 0);
                cairo_set_source(cr, fade);
                cairo_move_to(cr, line, top);
                cairo_line_to(cr, line, bottom);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
                cairo_pattern_destroy(fade);
            }
            for (const auto& cell : picker.cells) {
                const Rect face{cell.bounds.x + 3, cell.bounds.y + 3, cell.bounds.width - 6,
                                cell.bounds.height - 6};
                if (cell.clear) {
                    if (cell.pressed) {
                        fill_latched(cr, t, face, t.radius);
                    } else {
                        rounded(cr, face, t.radius);
                        source(cr, cell.hovered ? t.hover : t.bottom);
                        cairo_fill(cr);
                    }
                    draw_icon(cr, Icon::Clear, face, cell.pressed ? t.latched_legend : t.legend, 24);
                    continue;
                }
                if (cell.slot) {
                    // Where a lifted emoji would land: an empty key's face.
                    rounded(cr, face, t.radius);
                    source(cr, t.hover);
                    cairo_fill(cr);
                    continue;
                }
                if (cell.pressed) {
                    fill_latched(cr, t, face, t.radius); // Like the tabs.
                } else if (cell.hovered) {
                    rounded(cr, face, t.radius);
                    source(cr, t.hover);
                    cairo_fill(cr);
                }
                emoji_image(cell.text, cell.bounds);
            }
            if (!picker.hint.empty()) {
                label(cr, picker.hint, picker.hint_bounds, t.small_font_size * 1.3, view.language->font,
                      disabled_legend(t), false, PANGO_ALIGN_CENTER, true);
            }
            if (!picker.favorites_hint.empty()) {
                const auto& r = picker.favorites_hint_bounds;
                label(cr, picker.favorites_hint, {r.x + 8, r.y, r.width - 16, r.height},
                      t.small_font_size * 1.1, view.language->font, disabled_legend(t), false,
                      PANGO_ALIGN_CENTER, true);
            }
            cairo_restore(cr);
        }
        if (view.dictation && !view.settings) {
            draw_editor(cr, view);
        }
        if (const auto cells = flick_cells(view)) {
            // Above the keys, like the trail; partial repaints redraw it inside their clip.
            // Raised keycaps floating over the keys: their shadows first, so
            // none falls on a neighbouring cap, then the caps with the keys'
            // hover gradient and a faint light along the top edge.
            const auto& preview = *view.flick_preview;
            const double size = (*cells)[0].height;
            auto used = [&](std::size_t i) { return i == 0 || !preview.text[i].empty(); };
            for (std::size_t i = 0; i < cells->size(); ++i) {
                if (used(i)) {
                    soft_shadow(cr, (*cells)[i], t.radius, flick_shadow);
                }
            }
            for (std::size_t i = 0; i < cells->size(); ++i) {
                if (!used(i)) {
                    continue; // A direction this key does not use.
                }
                const auto& r = (*cells)[i];
                // A gutter in the case's colour sets each cap off from the
                // keys beneath, as the gaps between keys do. It stays over a
                // transparent case too: without it, caps over keys of the
                // same colour run together.
                rounded(cr, {r.x - 2, r.y - 2, r.width + 4, r.height + 4}, t.radius + 2);
                source(cr, t.surface);
                cairo_fill(cr);
                draw_cap(cr, t, r, true, static_cast<int>(i) == preview.selected, true);
                cairo_move_to(cr, r.x + t.radius, r.y + 1.5);
                cairo_line_to(cr, r.x + r.width - t.radius, r.y + 1.5);
                cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .12);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
                label(cr, preview.text[i], r, size * .5, view.language->font,
                      static_cast<int>(i) == preview.selected ? t.latched_legend : t.legend, true);
            }
        }
        if (view.composing && !view.settings) {
            const auto& method = view.language->input_method;
            const char* hint = method == "korean-2set" ? "Type → Space/Enter: commit · Esc: cancel"
                               : method == "korean-cheonjiin"
                                   ? "천지인 → ㅣㆍㅡ: vowels · Space/Enter: commit · Esc: cancel"
                               : method.starts_with("chinese-")
                                   ? "Pinyin → Space/1–5: select · Enter: commit · Esc: cancel"
                               : method == "japanese-kana"
                                   ? "かな入力 → スペース: 変換 · Enter: 確定 · Esc: キャンセル"
                               : method == "japanese-flick"
                                   ? "フリック入力 → 空白: 変換 · 改行: 確定 · Esc: キャンセル"
                                   : "ローマ字入力 → スペース: 変換 · Enter: 確定 · Esc: キャンセル";
            const std::string reading = view.preedit.empty() ? hint : view.preedit;
            const Rect line{body.x + 20, 94, body.width - 40, 40};
            case_plate(cr, view, line, reading, 27, PANGO_ALIGN_CENTER);
            label(cr, reading, line, 27, view.language->font, t.legend);
        }
        if (view.settings) {
            cairo_save(cr);
            cairo_rectangle(cr, 18, 78, 1564, 476);
            cairo_clip(cr);
            for (const auto& card : view.cards) {
                draw_settings_card(cr, t, card.bounds);
            }
            for (const auto& card : view.cards) {
                // Titles stay fixed while the card's fields scroll beneath them;
                // quieter than the rows, like a section header.
                label(cr, card.title,
                      {card.bounds.x + 30, card.bounds.y + 12, card.bounds.width - 60, 32}, 19,
                      view.language->font, mix(t.legend, t.surface, .35), false, PANGO_ALIGN_LEFT);
                // One quiet line under the title; rows are set apart by space
                // alone, as most of them configure similar things.
                const double y = std::round(card.bounds.y + 50) + .5;
                cairo_move_to(cr, card.bounds.x + 16, y);
                cairo_line_to(cr, card.bounds.x + card.bounds.width - 16, y);
                cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .07);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
            for (const auto& field : view.field_labels) {
                cairo_save(cr);
                cairo_rectangle(cr, field.clip.x, field.clip.y, field.clip.width, field.clip.height);
                cairo_clip(cr);
                const Color warning = t.warning; // the editor's and address popup's amber
                label(cr, field.text, field.bounds, field.size, view.language->font,
                      !field.enabled  ? disabled_legend(t)
                      : field.warning ? warning
                                      : t.legend,
                      field.left && !field.wrap, field.left ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT,
                      field.wrap);
                cairo_restore(cr);
            }
        }
        auto draw_control = [&](const Control& control) {
            cairo_save(cr);
            if (control.style == ControlStyle::MoveHandle) {
                const auto r = control.bounds;
                const double width = (r.width - 80) * (1 + .1 * move_handle_.amount);
                rounded(cr, {r.x + (r.width - width) / 2, r.y + r.height / 2 - 4, width, 8}, 4);
                auto channel = [&](double idle) {
                    return idle / 255 + (1 - idle / 255) * move_handle_.amount;
                };
                cairo_set_source_rgb(cr, channel(61), channel(68), channel(80));
                cairo_fill(cr);
                cairo_restore(cr);
                return;
            }
            if (control.clip) {
                const auto& r = *control.clip;
                cairo_rectangle(cr, r.x, r.y, r.width, r.height);
                cairo_clip(cr);
            }
            const auto r = control.bounds;
            // Text on a selected control sits on the latched colour.
            const auto legend = !control.enabled ? disabled_legend(t)
                                : control.selected && control.style != ControlStyle::TextField
                                    ? t.latched_legend
                                    : t.legend;
            const bool hovered = control.enabled && view.hovered_controls.contains(control.id);
            if (control.style == ControlStyle::Scrollbar) {
                // A quiet pill, lit white under the laser and while dragged.
                rounded(cr, r, 3);
                // A light theme's hover is a pale tint that would read as a
                // stripe on its case; a shade of the legend stays quiet there.
                source(cr, control.selected || hovered ? t.legend
                           : light_theme(t)            ? mix(t.surface, t.legend, .22)
                                                       : t.hover);
                cairo_fill(cr);
                cairo_restore(cr);
                return;
            }
            if (view.settings && control.style != ControlStyle::Scrollbar) {
                switch (control.style) {
                case ControlStyle::Checkbox:
                    draw_switch(cr, t, r, control.selected, hovered, control.enabled);
                    break;
                case ControlStyle::MenuItem:
                    // Only the item under the laser and the current choice show.
                    if (control.selected && control.enabled) {
                        fill_latched(cr, t, {r.x + 5, r.y + 3, r.width - 10, r.height - 6}, t.radius);
                    } else if (hovered) {
                        rounded(cr, {r.x + 5, r.y + 3, r.width - 10, r.height - 6}, t.radius);
                        source(cr, t.hover);
                        cairo_fill(cr);
                    }
                    if (const Rect text{r.x + 20, r.y, r.width - 40, r.height};
                        marquee_text && control.id == marquee_) {
                        // A name too long for its row scrolls while the laser is on it.
                        scrolled_label(cr, control.label, text, 21, view.language->font, legend,
                                       marquee_shift);
                    } else {
                        label(cr, control.label, text, 21, view.language->font, legend, false,
                              PANGO_ALIGN_LEFT);
                    }
                    break;
                case ControlStyle::TextField: {
                    // A small well, as the dictation editor is, since it holds
                    // typed text; hover lifts its rim.
                    draw_well(cr, t, r);
                    if (hovered) {
                        rounded(cr, {r.x + .5, r.y + .5, r.width - 1, r.height - 1}, t.radius + 2);
                        cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .18);
                        cairo_set_line_width(cr, 1);
                        cairo_stroke(cr);
                    }
                    label(cr, control.label, {r.x + 18, r.y, r.width - 36, r.height}, 21,
                          view.language->font, legend, false, PANGO_ALIGN_LEFT);
                    break;
                }
                case ControlStyle::Dropdown: {
                    draw_cap(cr, t, r, hovered, control.selected, control.enabled);
                    label(cr, control.label, {r.x + 18, r.y, r.width - 62, r.height}, 21,
                          view.language->font, legend, false, PANGO_ALIGN_LEFT);
                    const double x = r.x + r.width - 26, y = r.y + r.height / 2;
                    cairo_move_to(cr, x - 6, y - 3);
                    cairo_line_to(cr, x, y + 3);
                    cairo_line_to(cr, x + 6, y - 3);
                    cairo_set_source_rgba(cr, legend.r, legend.g, legend.b, .75);
                    cairo_set_line_width(cr, 2);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                    cairo_stroke(cr);
                    break;
                }
                default:
                    draw_cap(cr, t, r, hovered, control.selected, control.enabled);
                    if (control.icon != Icon::None) {
                        draw_icon(cr, control.icon, r, legend);
                    } else {
                        label(cr, control.label, r, 21, view.language->font, legend);
                    }
                    break;
                }
                cairo_restore(cr);
                return;
            }
            const double radius = control.style == ControlStyle::Checkbox ? 3 : t.radius;
            if (control.enabled && control.selected) {
                fill_latched(cr, t, r, radius);
                rounded(cr, r, radius);
            } else {
                rounded(cr, r, radius);
                source(cr, control.enabled && view.hovered_controls.contains(control.id) ? t.hover
                                                                                         : t.bottom);
                cairo_fill_preserve(cr);
            }
            if (view.settings && control.style != ControlStyle::Scrollbar &&
                control.style != ControlStyle::MenuItem) {
                source(cr, legend);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            } else {
                cairo_new_path(cr);
            }
            if (control.style == ControlStyle::Scrollbar) {
            } else if (control.style == ControlStyle::Dropdown) {
                label(cr, control.label, {r.x + 6, r.y, r.width - 58, r.height}, 22, view.language->font,
                      legend);
                source(cr, legend);
                cairo_move_to(cr, r.x + r.width - 50, r.y);
                cairo_line_to(cr, r.x + r.width - 50, r.y + r.height);
                const double x = r.x + r.width - 25, y = r.y + r.height / 2;
                cairo_move_to(cr, x - 7, y - 4);
                cairo_line_to(cr, x, y + 4);
                cairo_line_to(cr, x + 7, y - 4);
                cairo_stroke(cr);
            } else if (control.style == ControlStyle::Checkbox) {
                if (control.selected) {
                    draw_icon(cr, Icon::Apply, r, legend, 28);
                }
            } else if (control.icon != Icon::None) {
                draw_icon(cr, control.icon, r, control.warning ? t.alert : legend);
            } else {
                label(cr, control.label, r, view.settings ? 22 : 19, view.language->font, legend);
            }
            cairo_restore(cr);
        };
        for (const auto& control : view.controls) {
            if (control.style != ControlStyle::MenuItem && control.id != "scroll-menu" &&
                !control.id.starts_with("address-") && !control.id.starts_with("confirm-") &&
                !control.id.starts_with("favorites-")) {
                draw_control(control);
            }
        }
        if (!view.status.empty() && view.dictation && !view.settings) {
            // The editor covers the status line, so it moves up into the
            // toolbar row, between the left buttons and the next control or
            // the counter, wrapping onto a second line when long.
            double left = body.x + 18, right = body.x + body.width - 18;
            for (const auto& control : view.controls) {
                if (control.bounds.y < 56 && control.bounds.x + control.bounds.width <= body.x + 400) {
                    left = std::max(left, control.bounds.x + control.bounds.width);
                }
            }
            for (const auto& control : view.controls) {
                if (control.bounds.y < 56 && control.bounds.x >= left) {
                    right = std::min(right, control.bounds.x);
                }
            }
            if (const auto counter = dictation_counter_bounds(view)) {
                right = std::min(right, counter->x);
            }
            const Rect line{left + 12, 12, right - left - 24, 42};
            case_plate(cr, view, line, view.status, 15, PANGO_ALIGN_LEFT);
            label(cr, view.status, line, 15, view.language->font, t.legend, false, PANGO_ALIGN_LEFT,
                  true);
        } else if (!view.status.empty()) {
            // A compact toolbar's second row takes the left of the status line.
            const double inset = compact_toolbar(view) ? 88 : 18;
            const Rect line = view.settings ? Rect{30, 574, 1540, 24}
                                            : Rect{body.x + inset, 60, body.width - inset - 18, 28};
            case_plate(cr, view, line, view.status, 17, PANGO_ALIGN_CENTER);
            label(cr, view.status, line, 17, view.language->font, t.legend);
        }
        // The skin tones of a held emoji, above everything in the picker.
        if (view.emoji && view.emoji->popup && !view.settings) {
            // A raised menu, like the dropdowns: its edge drawn by light and
            // shadow rather than an outline.
            const auto& popup = *view.emoji->popup;
            const auto r = popup.bounds;
            const double radius = t.radius + 2;
            soft_shadow(cr, r, radius, tone_shadow);
            rounded(cr, r, radius);
            source(cr, mix(t.bottom, t.surface, .25));
            cairo_fill(cr);
            cairo_move_to(cr, r.x + radius, r.y + 1.5);
            cairo_line_to(cr, r.x + r.width - radius, r.y + 1.5);
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            for (const auto& choice : popup.choices) {
                if (choice.hovered) {
                    fill_latched(cr, t, choice.bounds, t.radius);
                }
                emoji_image(choice.text, choice.bounds);
            }
        }
        // A lifted emoji on a raised keycap, above the grid and its tones;
        // over a place where dropping it would remove it, it fades.
        if (view.emoji && view.emoji->lifted && !view.settings) {
            const auto& lifted = *view.emoji->lifted;
            const auto r = lifted.bounds;
            soft_shadow(cr, r, t.radius + 2, tone_shadow);
            rounded(cr, r, t.radius + 2);
            source(cr, mix(t.bottom, t.surface, .25));
            cairo_fill(cr);
            cairo_push_group(cr);
            emoji_image(lifted.text, r);
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, lifted.removing ? .4 : 1);
        }
        // Popup is the last layer and deliberately bypasses both card and case clips.
        if (view.popup) {
            // A raised menu over the cards, its edge drawn by light and shadow.
            const auto r = *view.popup;
            const Rect panel{r.x - 4, r.y - 4, r.width + 8, r.height + 8};
            soft_shadow(cr, panel, t.radius + 4, 14);
            rounded(cr, panel, t.radius + 4);
            source(cr, mix(t.bottom, t.surface, .25));
            cairo_fill(cr);
            cairo_move_to(cr, panel.x + t.radius + 4, panel.y + 1.5);
            cairo_line_to(cr, panel.x + panel.width - t.radius - 4, panel.y + 1.5);
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            for (const auto& control : view.controls) {
                if (control.style == ControlStyle::MenuItem || control.id == "scroll-menu") {
                    draw_control(control);
                }
            }
        }
        // A popup over Settings: the case dims, and a raised panel holds it.
        auto modal_panel = [&](const Rect& panel) {
            rounded(cr, body, t.surface_radius);
            cairo_set_source_rgba(cr, 0, 0, 0, .45);
            cairo_fill(cr);
            const double radius = t.radius + 6;
            soft_shadow(cr, panel, radius, 18);
            rounded(cr, panel, radius);
            source(cr, mix(t.surface, t.bottom, .5));
            cairo_fill(cr);
            cairo_move_to(cr, panel.x + radius, panel.y + 1.5);
            cairo_line_to(cr, panel.x + panel.width - radius, panel.y + 1.5);
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        };
        if (view.favorites) {
            // The favorites popup over the Languages card: a header like the
            // card's, the list of pairs with their bins, and Clear all.
            const auto& popup = *view.favorites;
            modal_panel(popup.panel);
            label(cr, "Favorites", {popup.panel.x + 88, popup.panel.y + 16, popup.panel.width - 112, 36},
                  21, view.language->font, mix(t.legend, t.surface, .35), false, PANGO_ALIGN_LEFT);
            const double line = std::round(popup.list.y - 6) + .5;
            for (const double y : {line, std::round(popup.list.y + popup.list.height + 6) + .5}) {
                cairo_move_to(cr, popup.panel.x + 16, y);
                cairo_line_to(cr, popup.panel.x + popup.panel.width - 16, y);
                cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .07);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            cairo_save(cr);
            cairo_rectangle(cr, popup.list.x, popup.list.y, popup.list.width, popup.list.height);
            cairo_clip(cr);
            for (const auto& row : popup.rows) {
                // The language over its layout, as Settings rows with a note.
                const double width = row.bounds.width - 104;
                label(cr, row.language, {row.bounds.x + 12, row.bounds.y + 10, width, 30}, 22,
                      view.language->font, t.legend, true, PANGO_ALIGN_LEFT);
                label(cr, row.layout, {row.bounds.x + 12, row.bounds.y + 40, width, 22}, 16,
                      view.language->font, t.legend, true, PANGO_ALIGN_LEFT);
            }
            if (popup.rows.empty()) {
                label(cr, "No favorites saved", popup.list, 19, view.language->font, disabled_legend(t));
            }
            cairo_restore(cr);
            if (popup.thumb) {
                rounded(cr, *popup.thumb, 3);
                source(cr, light_theme(t) ? mix(t.surface, t.legend, .22) : t.hover);
                cairo_fill(cr);
            }
            label(cr, popup.count,
                  {popup.panel.x + 24, popup.panel.y + popup.panel.height - 70, 220, 52}, 16,
                  view.language->font, t.legend, false, PANGO_ALIGN_LEFT);
            for (const auto& control : view.controls) {
                if (control.id.starts_with("favorites-")) {
                    draw_control(control);
                }
            }
        }
        if (view.confirm) {
            const auto& popup = *view.confirm;
            modal_panel(popup.panel);
            label(cr, popup.title, {popup.panel.x + 40, popup.panel.y + 30, popup.panel.width - 80, 36},
                  25, view.language->font, t.legend, false, PANGO_ALIGN_LEFT);
            label(cr, popup.body, {popup.panel.x + 40, popup.panel.y + 78, popup.panel.width - 80, 56},
                  18, view.language->font, mix(t.legend, t.surface, .3), false, PANGO_ALIGN_LEFT, true);
            for (const auto& control : view.controls) {
                if (control.id.starts_with("confirm-")) {
                    draw_control(control);
                }
            }
        }
        if (view.address) {
            // The server address popup: the field, its hint, recent servers
            // and the keys.
            const auto& popup = *view.address;
            modal_panel(popup.panel);
            label(cr, "Server address",
                  {popup.panel.x + 30, popup.panel.y + 18, popup.panel.width - 140, 32}, 19,
                  view.language->font, mix(t.legend, t.surface, .35), false, PANGO_ALIGN_LEFT);
            draw_well(cr, t, popup.field);
            auto* text = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_new();
            pango_font_description_set_family(font, view.language->font.c_str());
            pango_font_description_set_absolute_size(font, 26 * PANGO_SCALE);
            pango_layout_set_font_description(text, font);
            pango_font_description_free(font);
            pango_layout_set_text(text, popup.text.c_str(), static_cast<int>(popup.text.size()));
            int width = 0, height = 0;
            pango_layout_get_pixel_size(text, &width, &height);
            const double x = popup.field.x + 20, y = popup.field.y + (popup.field.height - height) / 2;
            cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .95);
            cairo_move_to(cr, x, y);
            pango_cairo_show_layout(cr, text);
            PangoRectangle caret;
            pango_layout_index_to_pos(text, static_cast<int>(popup.cursor), &caret);
            g_object_unref(text);
            rounded(cr, {x + caret.x / PANGO_SCALE - 1, y + 4, 2, height - 8.0}, 1);
            source(cr, t.legend);
            cairo_fill(cr);
            using Tone = PanelView::AddressView::Tone;
            label(cr, popup.message, popup.hint, 17, view.language->font,
                  popup.tone == Tone::Error     ? t.error
                  : popup.tone == Tone::Warning ? t.warning
                                                : disabled_legend(t),
                  false, PANGO_ALIGN_LEFT);
            for (const auto& control : view.controls) {
                if (control.id.starts_with("address-")) {
                    draw_control(control);
                }
            }
        }
    };
    if (full) {
        draw({});
    }
    // Record the changed pixels for a partial upload: the passes, moved by
    // the popup margin into surface rows and widened to whole pixels.
    damage_.clear();
    for (const auto& box : full ? std::vector<Rect>{} : passes) {
        const double left = std::max(0.0, std::floor(box.x));
        const double top = std::max(0.0, std::floor(box.y + popup_margin));
        const double right = std::min<double>(panel_width, std::ceil(box.x + box.width));
        const double bottom =
            std::min<double>(texture_height, std::ceil(box.y + box.height + popup_margin));
        if (right > left && bottom > top) {
            damage_.push_back({left, top, right - left, bottom - top});
        }
    }
    if (!full && damage_.empty()) {
        damage_.push_back({0, 0, 1, 1}); // passes all off the surface; a token upload
    }
    for (const auto& box : passes) {
        cairo_save(cr);
        cairo_rectangle(cr, box.x, box.y, box.width, box.height);
        cairo_clip(cr);
        draw({box});
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface_);
    return true;
}
std::vector<unsigned char> PanelRenderer::rgba() const {
    const auto* bytes = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    std::vector<unsigned char> result(static_cast<std::size_t>(panel_width) * texture_height * 4);
    for (int y = 0; y < texture_height; ++y) {
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
    // Include overflow controls in previews instead of cropping the movement line.
    const bool popup = previous_ && previous_->view.popup.has_value();
    const auto handle = previous_ ? panel_move_handle_bounds(previous_->view) : std::nullopt;
    const int height = handle ? static_cast<int>(handle->y + handle->height + 12) : panel_height;
    auto* output = cairo_surface_create_for_rectangle(surface_, 0, popup ? 0 : popup_margin, panel_width,
                                                      popup ? texture_height : height);
    const auto status = cairo_surface_write_to_png(output, path.c_str());
    cairo_surface_destroy(output);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("cannot write preview PNG");
    }
}
} // namespace framekeyboard
