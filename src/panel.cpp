#include "framekeyboard/panel.hpp"
#include "framekeyboard/emoji_glyphs.hpp"
#include "framekeyboard/japanese.hpp"
#include "framekeyboard/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <pango/pangocairo.h>
#include <stdexcept>
#include <sys/mman.h>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <utility>

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
    if (key.action_kind == ActionKind::Flick && key.action == flick_script_slot) {
        return !view.flick_script_key; // Only with Flick Katakana (JP).
    }
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
// Set by record_label_fits: every label drawn while it is set.
std::vector<LabelFit>* label_fits{};
// The renderer's shared key bodies (PanelRenderer::body_atlas_), set while it
// paints; keys drawn elsewhere draw their bodies themselves.
std::map<std::string, cairo_surface_t*>* body_atlas{};
bool list_open(const PanelView& view) {
    return view.candidates && view.candidates->list;
}
bool two_row_band(const PanelView& view);
// A full layout's open list opens under the reading line, as a raised menu
// over the keys; the flick layout's covers all of them.
std::optional<Rect> candidate_menu(const PanelView& view) {
    if (!list_open(view) || two_row_band(view)) {
        return {};
    }
    const auto& list = *view.candidates->list;
    return Rect{list.x - 8, list.y, list.width + 16, list.height + 8};
}
// Hidden keys, and every key while the flick layout's open candidate list
// takes their place, are neither drawn nor pressed but keep their place. A
// full layout's list is a menu over the keys, drawn whole under it; no key
// takes a press while either is open (PanelRenderer::hit_key()).
bool covered_key(const PanelView& view, const Key& key) {
    return (list_open(view) && !candidate_menu(view)) || hidden_key(view, key);
}
// How far the flick preview's caps and the skin tone menu raise their soft
// shadows; repaints cover them.
constexpr double flick_shadow = 8, tone_shadow = 12;
Color disabled_legend(const Theme& theme) {
    return {(theme.legend.r + theme.disabled.r) / 2, (theme.legend.g + theme.disabled.g) / 2,
            (theme.legend.b + theme.disabled.b) / 2};
}
// A key press takes this share of the theme's press duration and its release
// this share, so a press lands fast and returns softly. With Hover fade, a key
// the laser left takes this long to go dark.
constexpr double press_share = .75, release_share = 1.6, hover_fade_seconds = .18;
// The swipe trail: the whole path as a thin line, drawn faintly, and over it
// a comet, the pieces of the path newer than `trail_comet_seconds`, each
// thinner and fainter with age. When the swipe ends the path fades out over
// `trail_end_seconds`. SteamVR draws its own dot where the laser lands, so the
// trail has no head of its own.
constexpr double trail_path_width = 2.6, trail_path_alpha = .30;
// A neon theme's path is its latched colour, which is close to its keys'
// own: it needs more of its opacity to show.
constexpr double trail_neon_path_alpha = .62;
constexpr double trail_comet_width = 7, trail_glow_width = 18;
constexpr double trail_comet_seconds = .42, trail_end_seconds = .22;
// The laser trembles, so a trail point is the average of the points around
// it, this many on each side, nearer ones counting more. Toward either end
// the reach shrinks, and both ends stay where the laser was.
constexpr std::size_t trail_smoothing = 4;
std::pair<double, double> trail_point(const std::vector<std::pair<double, double>>& points,
                                      std::size_t index) {
    const std::size_t reach = std::min({trail_smoothing, index, points.size() - 1 - index});
    double x = 0, y = 0, total = 0;
    for (std::size_t i = index - reach; i <= index + reach; ++i) {
        const double weight = static_cast<double>(reach + 1) -
                              std::abs(static_cast<double>(i) - static_cast<double>(index));
        x += points[i].first * weight;
        y += points[i].second * weight;
        total += weight;
    }
    return {x / total, y / total};
}
Rect joined(const Rect& a, const Rect& b) {
    const double left = std::min(a.x, b.x), top = std::min(a.y, b.y);
    return {left, top, std::max(a.x + a.width, b.x + b.width) - left,
            std::max(a.y + a.height, b.y + b.height) - top};
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
// The band takes a compact toolbar's second row's place, under the first,
// the row's switches at the left of its reading line; the keys follow it.
// It stays in every mode, so switching never changes their size, and the
// case is measured with it. On full layouts it is the reading's line alone,
// the candidates in the toolbar row above; the flick layout, as short as a
// phone's, gives the candidates a row of the band.
constexpr double ime_band_height = 44, ime_band_gap = 6, ime_toolbar_height = 106,
                 ime_two_row_height = ime_toolbar_height + ime_band_height + ime_band_gap;
// The reading line's top, in the flick band and on full layouts: the second
// toolbar row's 42-pixel switches centred on its 44 pixels.
constexpr double reading_line_top = second_row_top - 1;
// The second toolbar row the reading line replaced: the case of a layout,
// in every view of it, is at most that much shorter.
constexpr double second_row_room = 50;
double case_height(const PanelView& view) {
    return view.settings ? settings_height : panel_height - view.reading_room;
}
bool two_row_band(const PanelView& view) {
    return view.ime_band && view.language && view.language->input_method == "japanese-flick";
}
// The flick layout's column icons (↶, 😊, ␣, ↵) share Backspace's size, so
// they match the ← and → labels between them.
bool flick_layout(const PanelView& view) {
    return view.language && view.language->input_method == "japanese-flick";
}
constexpr double flick_icon_size = 18;
// Cut, Copy and Paste: 32 units at a full-size layout's scale.
constexpr double shortcut_icon_pixels = 34;
// The reading line of a composing full layout, full width, centred.
bool reading_line(const PanelView& view) {
    return view.ime_band && !two_row_band(view) && !view.settings;
}
constexpr double candidate_font_size = 21, reading_font_size = 24;
struct Placement {
    double scale, x, y;
};
Placement placement(const PanelView& view) {
    const double margin = view.theme->padding;
    const double toolbar_height = two_row_band(view)               ? ime_two_row_height
                                  : view.ime_band || view.ime_room ? ime_toolbar_height
                                                                   : normal_toolbar_height;
    // A page keeping the reading line's room lays its keys out as the
    // line's page does, the second toolbar row in the line's place.
    const double height = case_height(view);
    const double scale =
        std::min((panel_width - 2 * margin) / view.layout->width,
                 (height - toolbar_height - 2 * margin - view.theme->depth) / view.layout->height);
    return {scale, (panel_width - visible_layout_width(view) * scale) / 2,
            toolbar_height + (height - toolbar_height - view.layout->height * scale) / 2};
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
Rect clip_bounds(cairo_t* cr) {
    double left, top, right, bottom;
    cairo_clip_extents(cr, &left, &top, &right, &bottom);
    return {left, top, right - left, bottom - top};
}
// The panel is a texture seen at any angle, with no fixed pixel layout, so
// text is antialiased in grey even where the system's fonts ask for subpixel
// (coloured) antialiasing, as SteamOS's do: that would fringe every legend
// and, over a clear case, leave colour above alpha in premultiplied pixels.
// Measuring contexts take the same options, so their metrics match drawing.
void grey_text(cairo_t* cr) {
    static cairo_font_options_t* const options = [] {
        auto* created = cairo_font_options_create();
        cairo_font_options_set_antialias(created, CAIRO_ANTIALIAS_GRAY);
        return created;
    }();
    cairo_set_font_options(cr, options);
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
// A theme with its own key edges (High Contrast) keeps its keys as it
// defines them: no light, tones, shading or quieter legends are added.
bool plain_keys(const Theme& t) {
    return t.edge.has_value();
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
        if (plain_keys(t)) {
            source(cr, t.latched);
            cairo_fill(cr);
        } else {
            // One colour from the theme: shaded as the keys are, lighter at
            // the top and toward the case at the foot.
            gradient(cr, r, mix(t.latched, Color{1, 1, 1}, .13), t.latched,
                     mix(t.latched, t.surface, .22));
        }
    } else {
        gradient(cr, r, t.latched_top, t.latched, t.latched_bottom);
    }
}
// The light a keycap or button face catches along its top edge and round its
// upper corners, and the shade it falls into at its foot. Both stay inside
// the face, so a key still repaints within its bounds. `strength` fades it:
// more under the laser, none on a key held down. One stroke along the edge
// carries both: its gradient fades the light out over the top 16 pixels (a
// third of a short face), stays clear, and fades the shade in over the
// bottom 14.
void cap_light(cairo_t* cr, const Theme& t, const Rect& face, double radius, double strength = 1) {
    if (plain_keys(t) || strength <= 0) {
        return;
    }
    const bool light = light_theme(t);
    const double light_end = std::min(16.0, face.height * .3),
                 shade_start = std::max(light_end, face.height - 14);
    cairo_save(cr);
    rounded(cr, face, radius);
    cairo_clip(cr);
    cairo_set_line_width(cr, 1.5);
    auto* edge = cairo_pattern_create_linear(0, face.y, 0, face.y + face.height);
    cairo_pattern_add_color_stop_rgba(edge, 0, 1, 1, 1, std::min(1.0, (light ? .9 : .15) * strength));
    cairo_pattern_add_color_stop_rgba(edge, .3 * light_end / face.height, 1, 1, 1,
                                      std::min(1.0, (light ? .4 : .05) * strength));
    cairo_pattern_add_color_stop_rgba(edge, light_end / face.height, 1, 1, 1, 0);
    cairo_pattern_add_color_stop_rgba(edge, shade_start / face.height, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(edge, 1, 0, 0, 0, std::min(1.0, (light ? .10 : .30) * strength));
    rounded(cr, {face.x + .75, face.y + .75, face.width - 1.5, face.height - 1.5},
            std::max(1.0, radius - .75));
    cairo_set_source(cr, edge);
    cairo_stroke(cr);
    cairo_pattern_destroy(edge);
    cairo_restore(cr);
}
// Keys that type a character keep the theme's face. The others (modifiers,
// editing, navigation and function keys, and the shortcut column) sit a tone
// back toward the case, so the letter block stands out on a full layout.
bool character_key(const Key& key) {
    if (key.action_kind == ActionKind::Flick) {
        return true;
    }
    if (key.action_kind != ActionKind::Key) {
        return false;
    }
    static const std::set<std::string> other{"Backquote",     "Minus",     "Equal",     "BracketLeft",
                                             "BracketRight",  "Backslash", "Semicolon", "Quote",
                                             "Comma",         "Period",    "Slash",     "Space",
                                             "IntlBackslash", "IntlRo",    "IntlYen",   "NumpadDecimal"};
    const auto& action = key.action;
    return action.starts_with("Key") || action.starts_with("Digit") || other.contains(action) ||
           (action.starts_with("Numpad") && action.size() == 7 && action[6] >= '0' && action[6] <= '9');
}
struct KeyTone {
    Color top, middle, bottom, hover;
};
KeyTone key_tone(const Theme& t, bool character) {
    if (plain_keys(t) || character) {
        return {t.top, t.middle, t.bottom, t.hover};
    }
    // A light theme's faces are close to white; they need a longer step.
    const double back = light_theme(t) ? .40 : .26;
    return {mix(t.top, t.surface, back), mix(t.middle, t.surface, back), mix(t.bottom, t.surface, back),
            mix(t.hover, t.surface, back * .6)};
}
KeyTone key_tone(const Theme& t, const Key& key) {
    return key_tone(t, character_key(key));
}
// The Shift character over a key's own legend is quieter than what the key
// types, on its face's colour.
Color quiet_legend(const Theme& t, const KeyTone& tone, bool latched) {
    return plain_keys(t) ? t.legend : mix(t.legend, latched ? t.latched : tone.middle, .42);
}
// A high-contrast theme's edge just inside a keycap or button face:
// `edge_hover` and wider while the laser is on it or it is pressed, dimmed
// on a disabled one. Other themes have none.
void draw_edge(cairo_t* cr, const Theme& t, const Rect& face, double radius, bool lit, bool enabled) {
    lit = lit && enabled && t.edge_hover;
    if (!t.edge && !lit) {
        return;
    }
    const double width = lit ? 3 : 2;
    const Color color = lit ? *t.edge_hover : enabled ? *t.edge : mix(*t.edge, t.surface, .55);
    cairo_save(cr);
    rounded(cr, {face.x + width / 2, face.y + width / 2, face.width - width, face.height - width},
            std::max(1.0, radius - width / 2));
    source(cr, color);
    cairo_set_line_width(cr, width);
    cairo_stroke(cr);
    cairo_restore(cr);
}
// The rim of a recessed well or a raised card on a theme with edges: the
// edge colour, dimmed by `dim` toward the case, so a black case still shows
// where they are. Other themes keep their own faint rims.
bool edge_rim(cairo_t* cr, const Theme& t, const Rect& r, double radius, double dim) {
    if (!t.edge) {
        return false;
    }
    cairo_save(cr);
    rounded(cr, {r.x + .75, r.y + .75, r.width - 1.5, r.height - 1.5}, std::max(1.0, radius - .75));
    source(cr, mix(*t.edge, t.surface, dim));
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_restore(cr);
    return true;
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
    } else if (alignment == PANGO_ALIGN_LEFT) {
        // Left means left, or Arabic and Hebrew names flip to the far edge.
        pango_layout_set_auto_dir(layout, FALSE);
    }
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    // ゛ and ゜ alone, the 小 key's flick hints and preview cells and the JIS
    // kana keys, are marks no bigger than a dot at a kana's size: drawn half
    // again larger, they read. They stay where the font puts them beside a
    // kana, so ゜ is never taken for 。 or ○.
    if (text == "゛" || text == "゜") {
        size *= 1.6;
        pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
    }
    if (fit) {
        int natural_width, natural_height;
        pango_layout_get_pixel_size(layout, &natural_width, &natural_height);
        if (natural_width > r.width - 8) {
            // A little under the width: scaled text rounds a pixel wider at
            // times, and ellipsizing would turn a lone ゛ into "…".
            const double fitted = std::max(9.0, size * (r.width - 8) / natural_width * .96);
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
    if (label_fits) {
        const double drawn = pango_font_description_get_size(font) / static_cast<double>(PANGO_SCALE);
        label_fits->push_back({text, family, drawn / size, pango_layout_get_line_count(layout),
                               pango_layout_is_ellipsized(layout) || (wrap && height > r.height + 1)});
    }
    source(cr, color);
    cairo_move_to(cr, r.x + 4, r.y + (r.height - height) / 2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}
// Interface text, which translations make longer: one line shrinks to 80%
// of its size at most, then the text breaks onto a second line in its room,
// and only then shrinks further (docs/localization-design.md). Text that
// wraps anyway shrinks until its lines fit the room's height. Cut with "…"
// only past the smallest size, which the interface tour reports.
void interface_label(cairo_t* cr, const std::string& text, Rect r, double size,
                     const std::string& family, Color color, bool = false,
                     PangoAlignment alignment = PANGO_ALIGN_CENTER, bool wrap = false,
                     bool right_to_left = false, double below = 0) {
    constexpr double smallest = 9, step = .94;
    auto* layout = pango_cairo_create_layout(cr);
    if (right_to_left) {
        pango_layout_set_auto_dir(layout, FALSE);
        pango_context_set_base_dir(pango_layout_get_context(layout), PANGO_DIRECTION_RTL);
        pango_layout_context_changed(layout);
    } else if (alignment == PANGO_ALIGN_LEFT) {
        pango_layout_set_auto_dir(layout, FALSE);
    }
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    const auto resize = [&](double points) {
        pango_font_description_set_absolute_size(font, points * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
    };
    resize(size);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    pango_layout_set_alignment(layout, alignment);
    const double room = std::max(1.0, r.width - 8);
    int width, height;
    double drawn = size;
    if (wrap) {
        pango_layout_set_width(layout, static_cast<int>(room * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        for (pango_layout_get_pixel_size(layout, &width, &height); height > r.height && drawn > smallest;
             pango_layout_get_pixel_size(layout, &width, &height)) {
            resize(drawn = std::max(smallest, drawn * step));
        }
    } else {
        pango_layout_get_pixel_size(layout, &width, &height);
        // A little under the room: scaled text rounds a pixel wider at times.
        const double one_line = width > room ? std::max(smallest, size * room / width * .96) : size;
        drawn = one_line;
        if (one_line < size * .8) {
            // Two lines from 80% down, while they are larger than one line
            // would be and fit the room's height.
            pango_layout_set_width(layout, static_cast<int>(room * PANGO_SCALE));
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
            bool two = false;
            for (double points = size * .8; points > one_line && !two; points *= step) {
                resize(points);
                pango_layout_get_pixel_size(layout, &width, &height);
                if (pango_layout_get_line_count(layout) <= 2 && height <= r.height + below) {
                    drawn = points;
                    two = true;
                }
            }
            if (!two) {
                pango_layout_set_width(layout, -1);
            }
        }
        if (pango_layout_get_width(layout) < 0) {
            resize(drawn);
            pango_layout_set_width(layout, static_cast<int>(room * PANGO_SCALE));
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
    }
    pango_layout_get_pixel_size(layout, &width, &height);
    const int lines = pango_layout_get_line_count(layout);
    if (label_fits) {
        // A single line a pixel taller than its room isn't cut.
        label_fits->push_back(
            {text, family, drawn / size, lines,
             pango_layout_is_ellipsized(layout) || (lines > 1 && height > r.height + below + 1)});
    }
    source(cr, color);
    // With room below, the first line stays where a single line would be.
    const double top = below > 0 && lines > 1
                           ? r.y + (r.height - height / static_cast<double>(lines)) / 2
                           : r.y + (r.height - height) / 2;
    cairo_move_to(cr, r.x + 4, top);
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
    case Icon::HapticSound:
    case Icon::HapticSoundOff:
        // A dot sounding out to both sides. Off, slashed like Pin.
        cairo_arc(cr, 0, 0, 3.8, 0, 2 * pi);
        cairo_fill(cr);
        cairo_set_line_width(cr, 2);
        for (const double radius : {7.2, 10.8}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, 0, 0, radius, -pi / 4.5, pi / 4.5);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 0, 0, radius, pi - pi / 4.5, pi + pi / 4.5);
        }
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.8);
        if (icon == Icon::HapticSoundOff) {
            line(-10, -10, 10, 10);
        }
        break;
    case Icon::KeyFlick:
    case Icon::KeyFlickOff:
        // A key, and under it the downward pull that flicks it. Off, slashed
        // like Pin.
        rounded(cr, {-8.5, -10, 17, 9.5}, 3);
        cairo_stroke(cr);
        line(-2.5, -5.3, 2.5, -5.3);
        cairo_set_line_width(cr, 2.4);
        cairo_move_to(cr, -4.5, 4.5);
        cairo_line_to(cr, 0, 9);
        cairo_line_to(cr, 4.5, 4.5);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.8);
        if (icon == Icon::KeyFlickOff) {
            line(-10, -10, 10, 10);
        }
        break;
    case Icon::Volume25:
    case Icon::Volume50:
    case Icon::Volume75:
    case Icon::Volume100: {
        // A solid speaker, then one more sound wave for each step up.
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, -9.9, -3.2);
        cairo_line_to(cr, -5, -3.2);
        cairo_line_to(cr, .8, -7.8);
        cairo_line_to(cr, .8, 7.8);
        cairo_line_to(cr, -5, 3.2);
        cairo_line_to(cr, -9.9, 3.2);
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.4);
        // A symmetric curve from (x, -y) to (x, y), passing through (far, 0).
        auto wave = [&](double x, double y, double far) {
            const double bulge = x + 4. / 3 * (far - x);
            cairo_move_to(cr, x, -y);
            cairo_curve_to(cr, bulge, -y / 3, bulge, y / 3, x, y);
            cairo_stroke(cr);
        };
        if (icon != Icon::Volume25) {
            wave(3.1, 3.4, 4.2);
        }
        if (icon == Icon::Volume75 || icon == Icon::Volume100) {
            wave(5.4, 5.5, 7.2);
        }
        if (icon == Icon::Volume100) {
            wave(7.4, 7.5, 10.1);
        }
        break;
    }
    case Icon::PopHigh:
    case Icon::PopHighClick:
    case Icon::PopMiddle:
    case Icon::PopMiddleClick:
    case Icon::PopLow:
    case Icon::PopLowClick: {
        // A note on three lines: the higher it sits, the higher the pitch. A
        // ring around it is the click.
        const double y = icon == Icon::PopHigh || icon == Icon::PopHighClick       ? -6.5
                         : icon == Icon::PopMiddle || icon == Icon::PopMiddleClick ? 0
                                                                                   : 6.5;
        cairo_set_source_rgba(cr, color.r, color.g, color.b, .38);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        for (const double line_y : {-6.5, 0., 6.5}) {
            line(-10, line_y, 10, line_y);
        }
        cairo_stroke(cr);
        source(cr, color);
        cairo_arc(cr, 0, y, 3, 0, 2 * pi);
        cairo_fill(cr);
        if (icon == Icon::PopHighClick || icon == Icon::PopMiddleClick || icon == Icon::PopLowClick) {
            cairo_set_line_width(cr, 1.6);
            cairo_arc(cr, 0, y, 5.6, 0, 2 * pi);
            cairo_stroke(cr);
        }
        break;
    }
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
    case Icon::Reset:
        cairo_scale(cr, -1, 1); // Reload's arrow turned back: undo, not refresh.
        [[fallthrough]];
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
        // Focus mode: the window being typed into, a monitor like Computer's.
        // Off, slashed like Pin.
        rounded(cr, {-10, -8, 20, 13}, 2);
        cairo_stroke(cr);
        line(0, 5, 0, 9);
        line(-5, 9, 5, 9);
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
    case Icon::DeleteLeft:
        // ⌫ as phones draw Backspace: a tag pointing left with × in it.
        cairo_set_line_width(cr, 2.3);
        cairo_move_to(cr, -13, 0);
        cairo_line_to(cr, -6, -8);
        cairo_arc(cr, 10, -6, 2, -pi / 2, 0);
        cairo_arc(cr, 10, 6, 2, 0, pi / 2);
        cairo_line_to(cr, -6, 8);
        cairo_close_path(cr);
        line(0, -3.5, 7, 3.5);
        line(7, -3.5, 0, 3.5);
        break;
    case Icon::Back:
        line(9, 0, -9, 0);
        line(-2, -7, -9, 0);
        cairo_line_to(cr, -2, 7);
        break;
    case Icon::TurnBack:
        // An arrow back to the left that turns down: back one step, not undo.
        line(-3.6, 2.4, -8.4, -2.4);
        cairo_line_to(cr, -3.6, -7.2);
        line(-8.4, -2.4, 4.8, -2.4);
        cairo_arc(cr, 4.8, 2.4, 4.8, -pi / 2, pi / 2);
        cairo_line_to(cr, 3.6, 7.2);
        break;
    case Icon::Space:
        // ␣: an open box's floor, as phones mark the space bar.
        line(-12, -1, -12, 5);
        cairo_line_to(cr, 12, 5);
        cairo_line_to(cr, 12, -1);
        break;
    case Icon::Enter:
        // ↵: down from the top right, then back to the left.
        line(10, -8, 10, 3);
        cairo_line_to(cr, -9, 3);
        line(-3, -3, -9, 3);
        cairo_line_to(cr, -3, 9);
        break;
    case Icon::ChevronDown:
    case Icon::ChevronUp: {
        const double tip = icon == Icon::ChevronUp ? -4.5 : 4.5;
        line(-9, -tip, 0, tip);
        cairo_line_to(cr, 9, -tip);
        break;
    }
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
    case Icon::Cut:
        // Scissors, open and pointing up: two crossed blades over their loops.
        cairo_new_sub_path(cr);
        cairo_arc(cr, -5.5, 7.5, 3.5, 0, 2 * pi);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 5.5, 7.5, 3.5, 0, 2 * pi);
        line(-3.5, 4.5, 6, -10);
        line(3.5, 4.5, -6, -10);
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
    case Icon::Swipe:
    case Icon::SwipeOff:
        // A dot, and beside it the S a swipe leaves behind. Off, slashed like Pin.
        cairo_new_sub_path(cr);
        cairo_arc(cr, -6.7, -5.4, 2.4, 0, 2 * pi);
        cairo_fill(cr);
        cairo_set_line_width(cr, 2.4);
        cairo_move_to(cr, -1.1, -5.8);
        cairo_curve_to(cr, 0.6, -8, 5.5, -8.6, 6, -5);
        cairo_curve_to(cr, 6.4, -2, 1, -0.5, -2, 1.6);
        cairo_curve_to(cr, -6, 4.5, -5.5, 8, -2, 7.6);
        cairo_curve_to(cr, 1, 7.3, 4, 6, 6.6, 4);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.8);
        if (icon == Icon::SwipeOff) {
            line(-10, -10, 10, 10);
        }
        break;
    case Icon::Lock:
        // A padlock: its shackle over a rounded body with a keyhole.
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, -3.5, 4.6, pi, 2 * pi);
        line(-4.6, -3.5, -4.6, -.5);
        line(4.6, -3.5, 4.6, -.5);
        cairo_stroke(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, -5.5, 1.5, 1.5, pi, 1.5 * pi);
        cairo_arc(cr, 5.5, 1.5, 1.5, 1.5 * pi, 2 * pi);
        cairo_arc(cr, 5.5, 8.5, 1.5, 0, .5 * pi);
        cairo_arc(cr, -5.5, 8.5, 1.5, .5 * pi, pi);
        cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_arc(cr, 0, 4.4, 1.3, 0, 2 * pi);
        cairo_fill(cr);
        line(0, 4.4, 0, 7);
        break;
    case Icon::Eraser:
        // A block tilted on the line it leaves behind.
        cairo_move_to(cr, -9, 2);
        cairo_line_to(cr, 1, -8);
        cairo_line_to(cr, 9, 0);
        cairo_line_to(cr, -1, 10);
        cairo_line_to(cr, -5, 10);
        cairo_close_path(cr);
        line(-4, -3, 4, 5);
        line(-1, 10, 10, 10);
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
// A toolbar icon's shake from `since`, in pixels: a few damped swings, sampled
// 30 times a second like the mic key's bars; 0 once it is over.
constexpr double icon_shake_duration = .45;
double icon_shake(double since, double now) {
    constexpr double pi = 3.141592653589793;
    const double t = std::floor(now * 30) / 30 - since;
    if (since < 0 || t < 0 || t >= icon_shake_duration) {
        return 0;
    }
    return std::round(6 * std::sin(t * 2 * pi * 6) * (1 - t / icon_shake_duration) * 2) / 2;
}
// How much of its alert colour the shaking icon has left: 1 at the start, then
// down to 0 as the shake ends.
double icon_shake_fade(double since, double now) {
    const double t = std::floor(now * 30) / 30 - since;
    return since < 0 || t >= icon_shake_duration ? 0 : 1 - std::max(t, 0.0) / icon_shake_duration;
}
// A hovered name too long for its row, `overflow` pixels in all, waits,
// scrolls to its end, rests there, then starts over. Its offset in whole
// pixels, and how long until that changes: never more often than 30 times a
// second, and not at all while it waits or rests.
struct MarqueeStep {
    double offset{}, wait{};
};
constexpr double marquee_wait = .8, marquee_speed = 45, marquee_rest = 1.2;
double marquee_cycle(double overflow) {
    return marquee_wait + overflow / marquee_speed + marquee_rest;
}
MarqueeStep marquee_step(double overflow, double elapsed) {
    constexpr double wait = marquee_wait, speed = marquee_speed;
    const double travel = overflow / speed, cycle = marquee_cycle(overflow);
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
    // A key pulled into a flick fills, as a latched one does: release types its Shift character.
    if (view.key_flick && view.key_flick->key == key.id && view.key_flick->pull >= 1) {
        return true;
    }
    return modifiers.contains(code) || (key.action == "CapsLock" && view.keyboard->caps()) ||
           (key.action == "NumLock" && view.keyboard->num()) ||
           (view.dictation_capturing && key.icon == Icon::Mic);
}
// How a key's face is filled: as it rests or the laser lights it, latched,
// in the theme's pressed colour, or disabled.
enum class KeyFace { Plain, Latched, Pressed, Disabled };
// A keycap: its side, and its face `amount` of the way down its travel and
// lit `lift` by the laser, each 0 to 1, with the shade of a press, the edge
// light, a neon tube and a high-contrast edge as the theme has them. `r` is
// where the face rests. Returns where the face is drawn, for its legend.
Rect draw_keycap(cairo_t* cr, const Theme& t, const KeyTone& tone, const Rect& r, double amount,
                 double lift, KeyFace fill, bool edge_lit) {
    const bool disabled = fill == KeyFace::Disabled, latched = fill == KeyFace::Latched,
               pressing = fill == KeyFace::Pressed;
    // The side begins below the resting face. A fixed-size face moves down
    // over it; changing face height would make a pressed key look stretched.
    Rect side{r.x, r.y + t.travel, r.width, r.height + t.depth - t.travel};
    rounded(cr, side, t.radius);
    // A disabled key sinks flat, as a disabled toolbar button does.
    if (disabled) {
        source(cr, mix(t.side_bottom, t.surface, .5));
        cairo_fill(cr);
    } else {
        gradient(cr, side, t.side_top, t.side_bottom, t.side_bottom);
    }
    Rect face{r.x, r.y + amount * t.travel, r.width, r.height};
    if (disabled) {
        rounded(cr, face, t.radius);
        source(cr, mix(t.bottom, t.surface, .4));
        cairo_fill(cr);
    } else if (latched) {
        fill_latched(cr, t, face, t.radius);
    } else if (pressing) {
        rounded(cr, face, t.radius);
        source(cr, *t.pressed);
        cairo_fill(cr);
    } else if (lift > 0) {
        rounded(cr, face, t.radius);
        if (light_theme(t)) {
            // A light key keeps its own shading, lightest at the top, and
            // takes on the hover colour evenly.
            gradient(cr, face, mix(tone.top, tone.hover, .7 * lift),
                     mix(tone.middle, tone.hover, .7 * lift), mix(tone.bottom, tone.hover, .7 * lift));
        } else {
            // The whole face lifts toward the hover colour, not only its top.
            gradient(cr, face, mix(tone.top, mix(tone.hover, t.legend, .1), lift),
                     mix(tone.middle, tone.hover, .85 * lift), mix(tone.bottom, tone.hover, .5 * lift));
        }
    } else {
        rounded(cr, face, t.radius);
        gradient(cr, face, tone.top, tone.middle, tone.bottom);
    }
    // Plain faces only: a latched or pressed-colour face shows its own state,
    // and a neon theme lights its tube on a held key instead.
    const bool shaded = !disabled && !plain_keys(t) && t.latched_finish != "neon";
    if (shaded && amount > 0 && !latched && !pressing) {
        // A key held down sinks into shade from its top edge, so a press
        // reads apart from the hover it started from, at any distance.
        cairo_save(cr);
        rounded(cr, face, t.radius);
        cairo_clip(cr);
        const double dark = (light_theme(t) ? .16 : .34) * amount;
        auto* shade = cairo_pattern_create_linear(0, face.y, 0, face.y + face.height);
        cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, dark);
        cairo_pattern_add_color_stop_rgba(shade, .35, 0, 0, 0, dark * .45);
        cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, dark * .3);
        cairo_set_source(cr, shade);
        cairo_paint(cr);
        cairo_pattern_destroy(shade);
        cairo_restore(cr);
    }
    if (!disabled) {
        // The edge light dims as the key goes down, and is brighter under the laser.
        cap_light(cr, t, face, t.radius, (1 - amount) * (1 + .5 * lift));
    }
    if (t.latched_finish == "neon" && !latched && amount > 0) {
        // A pressed key lights the same tube, as far as it has travelled.
        neon_tube(cr, t, face, t.radius, amount);
    }
    draw_edge(cr, t, face, t.radius, edge_lit, !disabled);
    return face;
}
// A key's body at rest, copied from the image every key of its shape, fill
// and tone shares (body_atlas), which is drawn once at whole pixels. The key
// must be at whole pixels too (draw_key). Returns the face.
Rect atlas_keycap(cairo_t* cr, const Theme& t, const KeyTone& tone, const Key& key, KeyFace fill) {
    const auto& r = key.bounds;
    cairo_matrix_t matrix;
    cairo_get_matrix(cr, &matrix);
    const double scale = matrix.xx;
    constexpr int margin = 4; // room for antialiasing around the body
    char name[96];
    std::snprintf(name, sizeof name, "%.3fx%.3f/%d/%d/%.6f", r.width, r.height, static_cast<int>(fill),
                  character_key(key) ? 1 : 0, scale);
    auto& image = (*body_atlas)[name];
    if (!image) {
        image = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(r.width * scale)) + 2 * margin,
            static_cast<int>(std::ceil((r.height + t.depth) * scale)) + 2 * margin);
        auto* body = cairo_create(image);
        cairo_translate(body, margin, margin);
        cairo_scale(body, scale, scale);
        draw_keycap(body, t, tone, {0, 0, r.width, r.height}, 0, 0, fill, false);
        cairo_destroy(body);
        cairo_surface_flush(image);
    }
    double x = r.x, y = r.y;
    cairo_user_to_device(cr, &x, &y);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_set_source_surface(cr, image, std::round(x) - margin, std::round(y) - margin);
    cairo_paint(cr);
    cairo_restore(cr);
    return r;
}
// `amount` is how far the key is pressed and `lift` how far it is lit by the
// laser, each 0 to 1.
void draw_key_at(cairo_t* cr, const PanelView& view, const Key& key, double amount, double lift,
                 double phase, double ring) {
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
    // A theme with a pressed colour fills a key held halfway down or more.
    const bool pressing = view.theme->pressed && amount >= .5 && !latched && !listening && !disabled;
    // A latched or pressed face carries its own legend colour; everything
    // below draws with this theme so every mark on the face follows it.
    std::optional<Theme> inked;
    if ((latched || listening) && view.theme->latched_legend != view.theme->legend) {
        inked = *view.theme;
        inked->legend = inked->latched_legend;
    } else if (pressing && view.theme->pressed_legend != view.theme->legend) {
        inked = *view.theme;
        inked->legend = inked->pressed_legend;
    }
    const auto& t = inked ? *inked : *view.theme;
    const auto tone = key_tone(t, key);
    const KeyFace fill = disabled               ? KeyFace::Disabled
                         : latched || listening ? KeyFace::Latched
                         : pressing             ? KeyFace::Pressed
                                                : KeyFace::Plain;
    // A key at rest copies its body; one moving, lit or in the pressed
    // colour draws it.
    const bool edge_lit = view.hovered_keys.contains(key.id) || amount > 0;
    const auto face = body_atlas && amount == 0 && lift == 0 && !edge_lit && fill != KeyFace::Pressed
                          ? atlas_keycap(cr, t, tone, key, fill)
                          : draw_keycap(cr, t, tone, key.bounds, amount, lift, fill, edge_lit);
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
    // A label set for the moment, such as 変換 on the flick layout's space
    // bar while composing, stands in for the key's icon.
    if (key.icon != Icon::None && !view.key_labels.contains(key.id)) {
        const bool super_icon = key.icon == Icon::SteamFrame || key.icon == Icon::SteamOS;
        // Dictation's Copy, Cut and Paste, and the stack on layouts that need
        // one, are ordinary key height or less.
        const bool short_key = key.action_kind == ActionKind::Dictation && key.bounds.height < 64;
        double size = super_icon || short_key                                       ? 22
                      : key.icon == Icon::Backspace || key.icon == Icon::DeleteLeft ? 18
                      : key.icon == Icon::Emoji || key.icon == Icon::Back           ? 26
                                                                                    : 32;
        if (key.action_kind == ActionKind::Shortcut) {
            // Keys are drawn at the layout's scale, which the flick layout's
            // large keys raise by a third: Cut, Copy and Paste keep the size
            // a full-size layout gives them, in panel pixels.
            cairo_matrix_t matrix;
            cairo_get_matrix(cr, &matrix);
            size = shortcut_icon_pixels / matrix.xx;
        } else if (flick_layout(view) &&
                   (key.icon == Icon::Emoji || key.icon == Icon::Space || key.icon == Icon::Enter)) {
            size = flick_icon_size; // As Backspace's, beside the arrows' labels.
        }
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
        // The kana page's legends follow the script key; its own labels stay.
        const auto shown = [&](const std::string& text) {
            return view.flick_katakana && !view.flick_digits && !view.flick_latin
                       ? kana_in_script(text, true)
                       : text;
        };
        const auto center = shown(custom != view.key_labels.end() ? custom->second : key.label);
        const auto& table = view.flick_digits ? view.language->flick_digits : view.language->flick;
        const auto slot = table.find(key.action);
        if (slot == table.end()) {
            label(cr, center, face, t.font_size * 1.3, view.language->font, t.legend, true);
            return;
        }
        const auto hint = disabled_legend(t);
        if (key.action == flick_undo_slot) {
            draw_icon(cr, Icon::TurnBack, face, t.legend, flick_icon_size); // without hints
            return;
        }
        if (key.action == flick_script_slot) {
            // あ and ア side by side, the script in use lit, as the mode key
            // does; disabled, both dim, the one in use a little less.
            const auto& script = slot->second;
            const double inset = face.width * .1, half = (face.width - 2 * inset) / 2;
            const auto lit = disabled ? mix(t.legend, hint, .55) : t.legend;
            for (int i = 0; i < 2; ++i) {
                label(cr, i == 0 ? script.left : script.right,
                      {face.x + inset + half * i, face.y, half, face.height},
                      std::min(t.font_size, half * .8), view.language->font,
                      (i == 1) == view.flick_katakana ? lit : hint, true);
            }
            return;
        }
        if (key.action == flick_mode_slot) {
            // The mode key: あ, A and 1 across it, the one in use lit. A tap
            // moves on to the next; a flick picks one, as its preview shows.
            const auto& mode = slot->second;
            const std::array<const std::string*, 3> modes{&mode.left, &mode.up, &mode.right};
            const int on = view.flick_digits ? 2 : view.flick_latin ? 1 : 0;
            const double inset = face.width * .08, third = (face.width - 2 * inset) / 3;
            for (int i = 0; i < 3; ++i) {
                // One size for all three, small enough for the kana's width.
                label(cr, *modes[static_cast<std::size_t>(i)],
                      {face.x + inset + third * i, face.y, third, face.height},
                      std::min(t.font_size, third * .8), view.language->font, i == on ? t.legend : hint,
                      true);
            }
            return;
        }
        // The tap kana in the middle, the four flick kana as hints on their sides.
        const auto& flick = slot->second;
        const double side = face.width * .22, band = face.height * .34;
        label(cr, shown(flick.left), {face.x, face.y, side, face.height}, t.small_font_size,
              view.language->font, hint, true);
        label(cr, shown(flick.right), {face.x + face.width - side, face.y, side, face.height},
              t.small_font_size, view.language->font, hint, true);
        label(cr, shown(flick.up), {face.x + side, face.y, face.width - 2 * side, band},
              t.small_font_size, view.language->font, hint, true);
        label(cr, shown(flick.down),
              {face.x + side, face.y + face.height - band, face.width - 2 * side, band},
              t.small_font_size, view.language->font, hint, true);
        const Rect middle{face.x + side, face.y + band * .6, face.width - 2 * side,
                          face.height - band * 1.2};
        label(cr, center, middle, t.font_size, view.language->font, t.legend, true);
        return;
    }
    auto text = view.keymap->legend(key, legend_modifiers, view.keyboard->caps(), view.keyboard->num());
    if (const auto custom = view.key_labels.find(key.id); custom != view.key_labels.end()) {
        text = custom->second;
    }
    std::string secondary;
    if (view.key_labels.contains(key.id)) {
        const auto small = view.language->kana_shift.find(key.action);
        const auto jamo = view.language->composition_shift.find(key.action);
        if (small != view.language->kana_shift.end() && small->second != text) {
            secondary = small->second;
        } else if (view.key_flick_hints && jamo != view.language->composition_shift.end() &&
                   jamo->second != text) {
            secondary = jamo->second; // ㅃ over ㅂ, for a flick.
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
    // The Shift character over a key is quieter than what the key types.
    const auto quiet = disabled ? legend : quiet_legend(t, tone, latched);
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
    if (view.key_flick && view.key_flick->key == key.id && view.key_flick->pull > 0) {
        // Pulled down, the Shift character slides from its corner into the
        // middle and grows as the key's own shrinks away below; a letter's
        // capital, which has no corner, fades in.
        const double pull = view.key_flick->pull;
        const auto mix_rect = [&](Rect a, Rect b) {
            return Rect{a.x + (b.x - a.x) * pull, a.y + (b.y - a.y) * pull,
                        a.width + (b.width - a.width) * pull, a.height + (b.height - a.height) * pull};
        };
        const Rect corner{face.x, face.y + 3, face.width, face.height * .40};
        const Rect middle{face.x, face.y + face.height * .4, face.width, face.height * .55};
        const Rect below{face.x, face.y + face.height * .62, face.width, face.height * .38};
        const double size = secondary.empty() ? t.font_size : t.font_size * .85;
        const auto faded = [&](double alpha, auto&& draw) {
            cairo_push_group(cr);
            draw();
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, alpha);
        };
        const auto& lit = legend; // A latched face's colour, once the pull fills it.
        faded(1 - pull, [&] {
            label(cr, text, mix_rect(secondary.empty() ? face : middle, below),
                  size + (t.small_font_size - size) * pull, view.language->font, lit, true,
                  PANGO_ALIGN_CENTER, false, right_to_left(text));
        });
        const auto& shifted = view.key_flick->legend;
        faded(secondary.empty() ? pull : 1, [&] {
            // It brightens from its quiet colour as it takes the key over.
            label(cr, shifted, mix_rect(corner, face),
                  t.small_font_size + (t.font_size - t.small_font_size) * pull, view.language->font,
                  mix(quiet, lit, pull), true, PANGO_ALIGN_CENTER, false, right_to_left(shifted));
        });
        return;
    }
    if (!secondary.empty()) {
        label(cr, secondary, {face.x, face.y + 3, face.width, face.height * .40}, t.small_font_size,
              view.language->font, quiet, true, PANGO_ALIGN_CENTER, false, right_to_left(secondary));
        label(cr, text, {face.x, face.y + face.height * .4, face.width, face.height * .55},
              t.font_size * .85, view.language->font, legend, true, PANGO_ALIGN_CENTER, false,
              right_to_left(text));
    } else {
        label(cr, text, face, utility ? t.small_font_size : t.font_size, view.language->font, legend,
              true, PANGO_ALIGN_CENTER, false, right_to_left(text));
    }
}
// Every key is drawn at whole pixels, moved by less than half a pixel, so
// the body it shares with keys of its shape (atlas_keycap) lines up with its
// legends and looks the same wherever the layout's scale puts it.
void draw_key(cairo_t* cr, const PanelView& view, const Key& key, double amount, double lift,
              double phase = 1, double ring = 0) {
    double x = key.bounds.x, y = key.bounds.y;
    cairo_user_to_device(cr, &x, &y);
    double dx = std::round(x) - x, dy = std::round(y) - y;
    cairo_device_to_user_distance(cr, &dx, &dy);
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    draw_key_at(cr, view, key, amount, lift, phase, ring);
    cairo_restore(cr);
}
} // namespace
void record_label_fits(std::vector<LabelFit>* into) {
    label_fits = into;
}
double semi_transparent_case_opacity(const Theme& theme) {
    return light_theme(theme) ? .15 : .5;
}
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
    const auto p = placement(view);
    // Custom geometry can leave only a few pixels after hiding its numpad.
    // Reserve the fixed main toolbar. This same case is used by painting,
    // controls and the VR mask.
    const double minimum = 630;
    // A layout narrower than the panel at this scale (flick) also narrows the
    // case, except in Settings, whose controls and cards span the full panel.
    const double slack =
        view.settings ? 0 : panel_width - 2 * view.theme->padding - view.layout->width * p.scale;
    const double width =
        std::max(minimum, panel_width - (view.layout->width - visible_layout_width(view)) * p.scale -
                              (slack > 1 ? slack : 0));
    return {(panel_width - width) / 2, 0, width, case_height(view)};
}
double unbanded_layout_height(const PanelView& view) {
    if (!view.ime_band && !view.ime_room) {
        return view.layout->height;
    }
    const double room =
        case_height(view) - normal_toolbar_height - 2 * view.theme->padding - view.theme->depth;
    return std::max(view.layout->height, room / placement(view).scale);
}
double reading_case_room(const Layout& layout, const Theme& theme, bool flick) {
    if (flick) {
        return second_row_room;
    }
    // The scale placement() gives the keys under the normal toolbar in the
    // full case; under the reading line's taller toolbar, at that scale, they
    // need `needed` of the case.
    const double margin = theme.padding;
    const double scale =
        std::min((panel_width - 2 * margin) / layout.width,
                 (panel_height - normal_toolbar_height - 2 * margin - theme.depth) / layout.height);
    const double needed = ime_toolbar_height + 2 * margin + theme.depth + layout.height * scale;
    return std::clamp(std::floor(panel_height - needed), 0.0, second_row_room);
}
ImeBand ime_band(const PanelView& view) {
    const auto body = panel_case_bounds(view);
    // The first row's buttons end where the full layouts' candidates start;
    // the second row's switches, where the reading line does.
    double first = body.x + 8, line = body.x + 20, end = body.x + body.width - 18;
    for (const auto& control : view.controls) {
        const auto& r = control.bounds;
        switch (control.action.kind) {
        case ControlKind::Settings:
        case ControlKind::Recenter:
        case ControlKind::Pin:
        case ControlKind::Passthrough:
        case ControlKind::KeepOpen:
        case ControlKind::SwipeToggle:
        case ControlKind::SoundsToggle:
        case ControlKind::KeyFlicksToggle:
        case ControlKind::LanguageCycle:
            if (r.y < 50) {
                first = std::max(first, r.x + r.width);
            } else {
                line = std::max(line, r.x + r.width + 10);
            }
            break;
        case ControlKind::Close:
        case ControlKind::SinkOff:
            end = std::min(end, r.x);
            break;
        default:
            break;
        }
    }
    const double top = reading_line_top, right = body.x + body.width - 20;
    const Rect reading{line, top, right - line, ime_band_height};
    if (two_row_band(view)) {
        const Rect candidates{body.x + 20, top + ime_band_height + ime_band_gap, body.width - 40,
                              ime_band_height};
        ImeBand result{{body.x + 20, top, body.width - 40, 2 * ime_band_height + ime_band_gap},
                       reading,
                       candidates,
                       {}};
        if (view.ime_more) {
            constexpr double width = 64, gap = 8;
            auto& row = result.candidates;
            row.width -= width + gap;
            result.more = Rect{row.x + row.width + gap, row.y + 3, width, row.height - 6};
        }
        return result;
    }
    // Full layouts: the reading line across the case, and the candidates'
    // room in the toolbar row, a clear gap after its buttons, wider than
    // theirs, and before the close button or the shell's. App starts them at
    // its left, as phones do, ∨ after them if they overflow.
    constexpr double candidate_gap = 28;
    return {reading,
            reading,
            {first + candidate_gap, 12, std::max(0.0, end - 10 - first - candidate_gap), 42},
            {}};
}
Rect candidate_list_area(const PanelView& view) {
    const auto band = ime_band(view);
    const auto p = placement(view);
    if (!two_row_band(view)) {
        // Under the reading line, which stays in view, in the toolbar's
        // candidate room's columns, over the keys: five lines, or as many
        // as fit above the keys' foot. App fits it to fewer.
        const auto& room = band.candidates;
        const double top = band.reading.y + band.reading.height + 4;
        const double lines = 5 * (CandidateStrip::chip_height + CandidateStrip::chip_spacing) + 8;
        const double bottom = std::min(top + lines, p.y + view.layout->height * p.scale);
        return {room.x, top, room.width, std::max(0.0, bottom - top)};
    }
    const double top = band.candidates.y + band.candidates.height;
    const double bottom = p.y + view.layout->height * p.scale;
    return {band.band.x, top, band.band.width, std::max(0.0, bottom - top)};
}
double candidate_text_width(const std::string& text, const std::string& family) {
    // Measuring needs a context but no pixels: one tiny surface per thread.
    thread_local struct Measuring {
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* context = [this] {
            auto* created = cairo_create(surface);
            grey_text(created);
            return created;
        }();
        ~Measuring() {
            cairo_destroy(context);
            cairo_surface_destroy(surface);
        }
        // Each list measures its chips again, and the same ones come back
        // (a kaomoji under many readings): each is laid out once.
        std::unordered_map<std::string, double> widths;
    } measuring;
    auto key = family + '\n' + text;
    if (const auto known = measuring.widths.find(key); known != measuring.widths.end()) {
        return known->second;
    }
    if (measuring.widths.size() >= 4096) {
        measuring.widths.clear();
    }
    const double width = text_width(measuring.context, text, candidate_font_size, family);
    measuring.widths.emplace(std::move(key), width);
    return width;
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
    // Measuring needs a context but no pixels: one tiny surface per thread.
    // Shell previews lay out the editor on a worker thread while the VR
    // thread does, and a Cairo context must not be shared between them.
    struct Measuring {
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* context = [this] {
            auto* created = cairo_create(surface);
            grey_text(created);
            return created;
        }();
        ~Measuring() {
            cairo_destroy(context);
            cairo_surface_destroy(surface);
        }
    };
    thread_local Measuring measuring;
    auto* layout = pango_cairo_create_layout(measuring.context);
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
std::size_t editor_selection_edge(const PanelView& view, std::size_t first, std::size_t last,
                                  int direction) {
    const auto start = editor_caret(view, first);
    const auto end = editor_caret(view, last);
    bool first_is_left = start.x <= end.x;
    // Across lines the columns say nothing; the line's direction tells which
    // way the text reads.
    if (start.y != end.y) {
        auto* layout = editor_layout(view);
        int line = 0;
        pango_layout_index_to_line_x(layout, static_cast<int>(first), false, &line, nullptr);
        first_is_left =
            pango_layout_get_line_readonly(layout, line)->resolved_dir != PANGO_DIRECTION_RTL;
        g_object_unref(layout);
    }
    return (direction < 0) == first_is_left ? first : last;
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
    // App asks for this every frame while a laser is on the editor, and laying
    // out the whole text is the costly part. Keep each thread's last answer
    // until the text, its width or its font changes.
    struct Measured {
        std::string font, text;
        double width{-1}, height{};
    };
    thread_local Measured last;
    const double area_width = editor_text_area(view).width;
    const auto& text = view.dictation->text;
    if (area_width == last.width && text == last.text && view.language->font == last.font) {
        return last.height;
    }
    auto* layout = editor_layout(view);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    last = {view.language->font, text, area_width, static_cast<double>(height)};
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
// Narrower than this, no word of a status would fit.
constexpr double min_status_width = 40;
// The editor covers the status line, so it moves up into the toolbar row,
// between the left buttons and the next control or the counter.
Rect dictation_status_line(const PanelView& view) {
    const auto body = panel_case_bounds(view);
    double left = body.x + 18, right = body.x + body.width - 18;
    for (const auto& control : view.controls) {
        if (control.bounds.y < 56 && control.bounds.x + control.bounds.width <= body.x + 640 &&
            !control.id.starts_with("swipe-candidate-")) {
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
    return {left + 12, 12, std::max(0.0, right - left - 24), 42};
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
// The tooltip above the case: Keep open's hidden window, or else the label
// of the toolbar icon button a laser is on. Only one shows, so the two never
// overlap; the hidden window's wins.
struct ShownTooltip {
    bool operator==(const ShownTooltip&) const = default;
    std::string text;
    Rect button; // the button it points at
};
std::optional<ShownTooltip> shown_tooltip(const PanelView& view) {
    auto button = [&](std::string_view id) -> const Control* {
        const auto found = std::find_if(view.controls.begin(), view.controls.end(),
                                        [&](const Control& c) { return c.id == id; });
        return found == view.controls.end() ? nullptr : &*found;
    };
    if (!view.focus_tooltip.empty() && !view.settings) {
        if (const auto* keep = button("keep-open")) {
            return ShownTooltip{view.focus_tooltip, keep->bounds};
        }
    }
    if (const auto* hovered = view.hover_tooltip.empty() ? nullptr : button(view.hover_tooltip);
        hovered && !hovered->label.empty()) {
        return ShownTooltip{hovered->label, hovered->bounds};
    }
    return {};
}
// Where the tooltip may lie: above the case, centred on its button and kept
// on the panel. Its card is as wide as its text, up to this.
constexpr double focus_tooltip_height = 46, focus_tooltip_pointer = 10;
std::optional<Rect> focus_tooltip_area(const PanelView& view, const std::optional<ShownTooltip>& tip) {
    if (!tip) {
        return {};
    }
    constexpr double width = 640;
    const double centre = tip->button.x + tip->button.width / 2;
    const double bottom = panel_case_bounds(view).y - 4; // the pointer's tip
    return Rect{std::clamp(centre - width / 2, 8.0, panel_width - 8 - width),
                bottom - focus_tooltip_pointer - focus_tooltip_height, width,
                focus_tooltip_height + focus_tooltip_pointer};
}
// A raised card like Settings' cards, with a pointer down to the button and
// the window's title in the legend colour. It floats over whatever is behind
// the panel, so its fill stays opaque with any Background.
constexpr double focus_tooltip_size = 21, focus_tooltip_pad = 18;
// A theme's radius may be large: the corners fit the card's height.
double card_radius(const Theme& t) {
    return std::min(t.radius + 4, focus_tooltip_height / 2);
}
// The tooltip's card without its pointer: as wide as its text, centred on
// the button, and wide enough for both corners and the pointer between them,
// however short the text.
std::optional<Rect> focus_tooltip_card(cairo_t* cr, const PanelView& view) {
    const auto tip = shown_tooltip(view);
    const auto area = focus_tooltip_area(view, tip);
    if (!area) {
        return {};
    }
    const double ink = text_width(cr, tip->text, focus_tooltip_size, view.interface_font);
    const double radius = card_radius(*view.theme);
    const double least = std::min(area->width, 2 * (radius + focus_tooltip_pointer) + 8);
    const double width = std::clamp(ink + 2 * focus_tooltip_pad, least, area->width);
    const double centre = tip->button.x + tip->button.width / 2;
    return Rect{std::clamp(centre - width / 2, area->x, area->x + area->width - width), area->y, width,
                focus_tooltip_height};
}
// A raised card floating over whatever is behind the panel, as Settings'
// cards are, so its fill stays opaque with any Background.
void draw_card(cairo_t* cr, const Theme& t, const Rect& card) {
    soft_shadow(cr, card, card_radius(t), 6);
    rounded(cr, card, card_radius(t));
    source(cr, mix(t.surface, t.bottom, .45));
    cairo_fill(cr);
    edge_rim(cr, t, card, card_radius(t), .6);
    cairo_move_to(cr, card.x + card_radius(t), card.y + 1.5);
    cairo_line_to(cr, card.x + card.width - card_radius(t), card.y + 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}
// The keyboard's messages: a card above the case, at the focus tooltip's
// height, centred on the case. Beside the tooltip when both show, or above
// it when the panel has no room beside it.
void draw_status_card(cairo_t* cr, const PanelView& view) {
    constexpr double size = 17, pad = 20, gap = 12;
    const auto body = panel_case_bounds(view);
    const double width =
        std::min(text_width(cr, view.status, size, view.interface_font) + 2 * pad, panel_width - 16.0);
    Rect card{std::clamp(body.x + (body.width - width) / 2, 8.0, panel_width - 8 - width),
              body.y - 4 - focus_tooltip_pointer - focus_tooltip_height, width, focus_tooltip_height};
    if (const auto tooltip = focus_tooltip_card(cr, view); tooltip &&
                                                           card.x < tooltip->x + tooltip->width + gap &&
                                                           tooltip->x < card.x + card.width + gap) {
        if (const double right = tooltip->x + tooltip->width + gap; right + width <= panel_width - 8) {
            card.x = std::max(card.x, right);
        } else if (const double left = tooltip->x - gap - width; left >= 8) {
            card.x = std::min(card.x, left);
        } else {
            card.y = tooltip->y - 8 - focus_tooltip_height;
        }
    }
    draw_card(cr, *view.theme, card);
    // label() sets text 4 px inside its area.
    interface_label(cr, view.status, {card.x + pad - 4, card.y, card.width - 2 * pad + 8, card.height},
                    size, view.interface_font, view.theme->legend);
}
void draw_focus_tooltip(cairo_t* cr, const PanelView& view) {
    const auto found = focus_tooltip_card(cr, view);
    if (!found) {
        return;
    }
    const auto& t = *view.theme;
    const auto tooltip = shown_tooltip(view);
    const double radius = card_radius(t);
    const Rect card = *found;
    const double centre = tooltip->button.x + tooltip->button.width / 2;
    // The pointer stays clear of the card's rounded corners.
    const double tip = std::clamp(centre, card.x + radius + focus_tooltip_pointer,
                                  card.x + card.width - radius - focus_tooltip_pointer);
    // One outline, clockwise, the pointer part of the bottom edge: two
    // separate shapes would leave an antialiased seam where they meet.
    constexpr double pi = 3.141592653589793;
    const double left = card.x, top = card.y, right = card.x + card.width, bottom = card.y + card.height;
    soft_shadow(cr, card, radius, 6);
    cairo_new_path(cr);
    cairo_arc(cr, left + radius, top + radius, radius, pi, 1.5 * pi);
    cairo_arc(cr, right - radius, top + radius, radius, 1.5 * pi, 2 * pi);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0, .5 * pi);
    cairo_line_to(cr, tip + focus_tooltip_pointer, bottom);
    cairo_line_to(cr, tip, bottom + focus_tooltip_pointer);
    cairo_line_to(cr, tip - focus_tooltip_pointer, bottom);
    cairo_arc(cr, left + radius, bottom - radius, radius, .5 * pi, pi);
    cairo_close_path(cr);
    source(cr, mix(t.surface, t.bottom, .45));
    cairo_fill(cr);
    cairo_move_to(cr, card.x + radius, card.y + 1.5);
    cairo_line_to(cr, card.x + card.width - radius, card.y + 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    // label() sets text 4 px inside its area.
    interface_label(
        cr, tooltip->text,
        {card.x + focus_tooltip_pad - 4, card.y, card.width - 2 * focus_tooltip_pad + 8, card.height},
        focus_tooltip_size, view.interface_font, t.legend, false, PANGO_ALIGN_LEFT);
}
void draw_settings_card(cairo_t* cr, const Theme& t, const Rect& r) {
    const double radius = t.radius + 4;
    soft_shadow(cr, r, radius, 8);
    rounded(cr, r, radius);
    source(cr, mix(t.surface, t.bottom, .45));
    cairo_fill(cr);
    edge_rim(cr, t, r, radius, .6);
    // A faint light along the top edge, as on the keycaps.
    cairo_move_to(cr, r.x + radius, r.y + 1.5);
    cairo_line_to(cr, r.x + r.width - radius, r.y + 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .07);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}
// A keycap face for buttons and dropdowns: the keys' own gradient and hover.
void draw_cap_face(cairo_t* cr, const Theme& t, const Rect& r, bool hovered, bool selected,
                   bool enabled) {
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
// The cap with its edge, on a theme that has edges.
void draw_cap(cairo_t* cr, const Theme& t, const Rect& r, bool hovered, bool selected, bool enabled) {
    draw_cap_face(cr, t, r, hovered, selected, enabled);
    if (enabled) {
        cap_light(cr, t, r, t.radius, hovered ? 1.5 : 1);
    }
    draw_edge(cr, t, r, t.radius, hovered, enabled);
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
        if (!edge_rim(cr, t, track, height / 2, .3)) {
            rounded(cr, {track.x + .5, track.y + .5, track.width - 1, track.height - 1}, height / 2);
            source(cr, mix(t.surface, t.bottom, .8));
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
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
    if (!edge_rim(cr, t, r, radius, .3)) {
        rounded(cr, {r.x + .5, r.y + .5, r.width - 1, r.height - 1}, radius);
        cairo_set_source_rgba(cr, t.surface.r + (t.bottom.r - t.surface.r) * .8,
                              t.surface.g + (t.bottom.g - t.surface.g) * .8,
                              t.surface.b + (t.bottom.b - t.surface.b) * .8, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    // The light catch: a hairline just inside the bottom edge.
    cairo_move_to(cr, r.x + radius, r.y + r.height - 1.5);
    cairo_line_to(cr, r.x + r.width - radius, r.y + r.height - 1.5);
    cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .06);
    cairo_stroke(cr);
    return well;
}
// A see-through case (Background other than Visible); Settings keeps it opaque.
bool clear_case(const PanelView& view) {
    return view.case_opacity < 1 && !view.settings;
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
    const double ink = text_width(cr, text, size, view.interface_font) + extra;
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
// Candidates as caps like the toolbar's buttons, clipped to their strip.
void draw_candidates(cairo_t* cr, const PanelView& view, const CandidateStripView& strip) {
    const auto& t = *view.theme;
    // The row's chips within the row, an open list's within the list.
    for (const bool listed : {false, true}) {
        if (listed && !strip.list) {
            break;
        }
        const auto clip = pixel_bounds(listed ? *strip.list : strip.area);
        if (!overlaps(clip, clip_bounds(cr))) {
            continue;
        }
        cairo_save(cr);
        cairo_rectangle(cr, clip.x, clip.y, clip.width, clip.height);
        cairo_clip(cr);
        const auto pass = clip_bounds(cr);
        for (const auto& chip : strip.chips) {
            if (chip.listed != listed || !overlaps(pixel_bounds(chip.bounds, 1), pass)) {
                continue;
            }
            draw_cap(cr, t, chip.bounds, chip.hovered, chip.selected, true);
            if (strip.fallback) {
                // Debug mode's mark for Anthy's conversions, there to be
                // seen rather than to match the theme.
                const Rect inner{chip.bounds.x + 1, chip.bounds.y + 1, chip.bounds.width - 2,
                                 chip.bounds.height - 2};
                rounded(cr, inner, std::max(0.0, t.radius - 1));
                cairo_set_source_rgb(cr, 0.96, 0.55, 0.13);
                cairo_set_line_width(cr, 2);
                cairo_stroke(cr);
            }
            const auto legend = chip.selected ? t.latched_legend : t.legend;
            // Within its cap: a kaomoji's stacked marks (tears of ̥) reach far below.
            cairo_save(cr);
            const auto face = pixel_bounds(chip.bounds);
            cairo_rectangle(cr, face.x, face.y, face.width, face.height);
            cairo_clip(cr);
            label(cr, chip.text, chip.bounds, candidate_font_size, view.language->font, legend, true);
            cairo_restore(cr);
        }
        cairo_restore(cr);
    }
    if (strip.thumb) {
        // As the languages popup's lists show how far they scroll.
        rounded(cr, *strip.thumb, strip.thumb->width / 2);
        source(cr, light_theme(t) ? mix(t.surface, t.legend, .22) : t.hover);
        cairo_fill(cr);
    }
}
// The reading in a small well, underlined as in a text field with the
// converting segment on the latched colour, and the candidates as caps. A
// hint stands in while nothing is typed. On full layouts the reading takes
// the status line's place, so a status shows beside it, or for the hint.
void draw_ime_band(cairo_t* cr, const PanelView& view) {
    const auto& t = *view.theme;
    const auto areas = ime_band(view);
    const bool line = reading_line(view);
    const auto clip = clip_bounds(cr);
    if (overlaps(pixel_bounds(line && view.preedit.empty() ? areas.band : areas.reading, 6), clip)) {
        if (view.preedit.empty()) {
            const auto& hint = view.composition_hint;
            const auto& r = line ? areas.band : areas.reading;
            case_plate(cr, view, r, hint, 19, PANGO_ALIGN_CENTER);
            interface_label(cr, hint, r, 19, view.interface_font, mix(t.legend, t.surface, .35));
        } else {
            const auto& r = areas.reading;
            draw_well(cr, t, r);
            auto* layout = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_new();
            pango_font_description_set_family(font, view.language->font.c_str());
            pango_font_description_set_absolute_size(font, reading_font_size * PANGO_SCALE);
            pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            pango_layout_set_text(layout, view.preedit.c_str(), static_cast<int>(view.preedit.size()));
            pango_layout_set_single_paragraph_mode(layout, TRUE);
            pango_layout_set_width(layout, static_cast<int>((r.width - 24) * PANGO_SCALE));
            // A long reading keeps its end, where typing goes on, in view; while
            // converting, the side the active segment is on.
            const bool early = (view.preedit_active_end > view.preedit_active_begin &&
                                view.preedit_active_end * 2 <= view.preedit.size()) ||
                               (view.preedit_cursor && *view.preedit_cursor * 2 < view.preedit.size());
            pango_layout_set_ellipsize(layout, early ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_START);
            if (line) {
                pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
            }
            auto* attributes = pango_attr_list_new();
            pango_attr_list_insert(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            if (view.preedit_active_end > view.preedit_active_begin &&
                view.preedit_active_end <= view.preedit.size()) {
                auto channel = [](double value) {
                    return static_cast<guint16>(std::clamp(value, 0.0, 1.0) * 65535);
                };
                for (auto* attribute :
                     {pango_attr_background_new(channel(t.latched.r), channel(t.latched.g),
                                                channel(t.latched.b)),
                      pango_attr_foreground_new(channel(t.latched_legend.r), channel(t.latched_legend.g),
                                                channel(t.latched_legend.b))}) {
                    attribute->start_index = static_cast<guint>(view.preedit_active_begin);
                    attribute->end_index = static_cast<guint>(view.preedit_active_end);
                    pango_attr_list_insert(attributes, attribute);
                }
            }
            pango_layout_set_attributes(layout, attributes);
            pango_attr_list_unref(attributes);
            int width = 0, height = 0;
            pango_layout_get_pixel_size(layout, &width, &height);
            const double top = r.y + (r.height - height) / 2;
            source(cr, t.legend);
            cairo_move_to(cr, r.x + 12, top);
            pango_cairo_show_layout(cr, layout);
            if (view.preedit_cursor && *view.preedit_cursor <= view.preedit.size()) {
                // The caret, as in a text field, where typing goes.
                PangoRectangle at;
                pango_layout_index_to_pos(layout, static_cast<int>(*view.preedit_cursor), &at);
                const double x = std::round(r.x + 12 + at.x / static_cast<double>(PANGO_SCALE));
                cairo_rectangle(cr, x - 1, top + 3, 2, height - 6);
                cairo_fill(cr);
            }
            g_object_unref(layout);
        }
    }
    if (const auto menu = candidate_menu(view);
        menu && overlaps(pixel_bounds(*menu, tone_shadow), clip)) {
        // A raised menu, like the dropdowns, over keys drawn whole under it:
        // a faint rim of the legend's colour keeps its edge apart from theirs.
        // The rim is the menu filled, then filled again a pixel inside: a
        // stroke would antialias differently under each repaint's clip.
        const double radius = t.radius + 2;
        soft_shadow(cr, *menu, radius, tone_shadow);
        const Color fill = mix(t.bottom, t.surface, .25);
        rounded(cr, *menu, radius);
        source(cr, mix(fill, t.legend, .14));
        cairo_fill(cr);
        rounded(cr, {menu->x + 1, menu->y + 1, menu->width - 2, menu->height - 2}, radius - 1);
        source(cr, fill);
        cairo_fill(cr);
    }
    if (view.candidates) {
        draw_candidates(cr, view, *view.candidates);
    }
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
    // Fills what `mark` adds for each line piece of a byte range of the text.
    auto mark_range = [&](std::size_t begin, std::size_t end, auto&& mark) {
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
            for (int i = 0; i < count; ++i) {
                const double x0 = area.x + ranges[2 * i] / static_cast<double>(PANGO_SCALE);
                const double x1 = area.x + ranges[2 * i + 1] / static_cast<double>(PANGO_SCALE);
                mark(iter, x0, x1 - x0);
            }
            g_free(ranges);
            cairo_fill(cr);
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);
    };
    auto underline = [&](std::size_t begin, std::size_t end, double thickness) {
        mark_range(begin, end, [&](PangoLayoutIter* iter, double x, double width) {
            const int baseline = pango_layout_iter_get_baseline(iter);
            cairo_rectangle(cr, x, area.y - editor.scroll + baseline / PANGO_SCALE + 3, width,
                            thickness);
        });
    };
    if (editor.selection_end > editor.selection_begin) {
        // Behind the text, and from its own color, so it reads in every theme.
        cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .25);
        mark_range(editor.selection_begin, editor.selection_end,
                   [&](PangoLayoutIter* iter, double x, double width) {
                       int top = 0, bottom = 0;
                       pango_layout_iter_get_line_yrange(iter, &top, &bottom);
                       cairo_rectangle(cr, x,
                                       area.y - editor.scroll + top / static_cast<double>(PANGO_SCALE),
                                       width, (bottom - top) / static_cast<double>(PANGO_SCALE));
                   });
    }
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
        // laid over the text rather than part of it. Candidates are drawn
        // on it.
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
        interface_label(cr, editor.notice,
                        {band.x + editor_padding_x, band.y, band.width - 280, band.height},
                        t.small_font_size * 1.3, view.interface_font,
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
        interface_label(cr, editor.counter, counter, size, view.interface_font,
                        editor.at_limit ? amber : disabled_legend(t), false, PANGO_ALIGN_RIGHT);
        if (editor.where != Icon::None) {
            // Where speech is recognized, just left of the count.
            const double width = text_width(cr, editor.counter, size, view.interface_font);
            draw_icon(cr, editor.where,
                      {counter.x + counter.width - 4 - width - 32, counter.y + (counter.height - 26) / 2,
                       26, 26},
                      disabled_legend(t), 16);
        }
    }
}
// Display key collision (PanelView::key_outlines): a green square around
// where each key takes the laser, in layout coordinates. A key whose area is
// an L, beside a split pocket, gets one outline around the whole L: an edge
// another of its own areas continues across is left out. A debug view, so
// its colour is fixed rather than the theme's.
void draw_key_outlines(cairo_t* cr, const PanelView& view, double scale) {
    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_width(cr, 2 / scale);
    cairo_set_source_rgb(cr, .15, .95, .35);
    if (!view.key_areas) {
        for (const auto& key : view.layout->keys) {
            if (!covered_key(view, key)) {
                const auto& r = key.bounds;
                cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            }
        }
        cairo_stroke(cr);
        cairo_restore(cr);
        return;
    }
    constexpr double slack = 1e-6;
    const auto& areas = *view.key_areas;
    for (std::size_t i = 0; i < areas.size(); ++i) {
        const auto& area = areas[i];
        if (area.key >= view.layout->keys.size() || covered_key(view, view.layout->keys[area.key])) {
            continue;
        }
        // The key's other areas: one or two at most.
        std::array<const Rect*, 3> others{};
        std::size_t count = 0;
        for (std::size_t j = 0; j < areas.size() && count < others.size(); ++j) {
            if (j != i && areas[j].key == area.key) {
                others[count++] = &areas[j].bounds;
            }
        }
        const auto& r = area.bounds;
        // Each edge: fixed coordinate, span, and whether it runs across.
        const std::array<std::tuple<double, double, double, bool>, 4> edges{
            std::tuple{r.y, r.x, r.x + r.width, true},
            std::tuple{r.y + r.height, r.x, r.x + r.width, true},
            std::tuple{r.x, r.y, r.y + r.height, false},
            std::tuple{r.x + r.width, r.y, r.y + r.height, false}};
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto [at, from, to, across] = edges[e];
            std::array<std::pair<double, double>, 4> pieces{std::pair{from, to}};
            std::size_t pieces_count = 1;
            for (std::size_t o = 0; o < count; ++o) {
                const auto& other = *others[o];
                // The other area's edge facing this one, on the far side.
                const double facing = across ? (e == 0 ? other.y + other.height : other.y)
                                             : (e == 2 ? other.x + other.width : other.x);
                if (std::abs(facing - at) > slack) {
                    continue;
                }
                const double cut_from = across ? other.x : other.y;
                const double cut_to = across ? other.x + other.width : other.y + other.height;
                std::array<std::pair<double, double>, 4> kept{};
                std::size_t kept_count = 0;
                for (std::size_t p = 0; p < pieces_count; ++p) {
                    const auto [a, b] = pieces[p];
                    if (cut_from > a + slack && kept_count < kept.size()) {
                        kept[kept_count++] = {a, std::min(b, cut_from)};
                    }
                    if (cut_to < b - slack && kept_count < kept.size()) {
                        kept[kept_count++] = {std::max(a, cut_to), b};
                    }
                }
                pieces = kept;
                pieces_count = kept_count;
            }
            for (std::size_t p = 0; p < pieces_count; ++p) {
                const auto [a, b] = pieces[p];
                if (b - a <= slack) {
                    continue;
                }
                if (across) {
                    cairo_move_to(cr, a, at);
                    cairo_line_to(cr, b, at);
                } else {
                    cairo_move_to(cr, at, a);
                    cairo_line_to(cr, at, b);
                }
            }
        }
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}
// The flick preview (PanelView::flick_preview), in panel coordinates.
void draw_flick_preview(cairo_t* cr, const PanelView& view, const std::array<Rect, 5>& cells) {
    const auto& t = *view.theme;
    // Raised keycaps floating over the keys: their shadows first, so
    // none falls on a neighbouring cap, then the caps with the keys'
    // hover gradient and a faint light along the top edge.
    const auto& preview = *view.flick_preview;
    const double size = cells[0].height;
    auto used = [&](std::size_t i) { return i == 0 || !preview.text[i].empty(); };
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (used(i)) {
            soft_shadow(cr, cells[i], t.radius, flick_shadow);
        }
    }
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (!used(i)) {
            continue; // A direction this key does not use.
        }
        const auto& r = cells[i];
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
} // namespace
Rect dictation_status_bounds(const PanelView& view) {
    return dictation_status_line(view);
}
PanelRenderer::PanelRenderer() {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, texture_height);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
        throw std::runtime_error("cannot allocate keyboard image");
    }
}
namespace {
// A layer on pages of its own: zeroed only where it is drawn and given back
// whole when it goes. From malloc's heap, as after its first large free,
// it would be zeroed in full and not returned (R215).
const cairo_user_data_key_t mapped_key{}; // a mapped layer's pages: {address, size}
cairo_surface_t* mapped_layer(int width, int height) {
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    const auto size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    }
    auto* surface = cairo_image_surface_create_for_data(static_cast<unsigned char*>(data),
                                                        CAIRO_FORMAT_ARGB32, width, height, stride);
    auto* mapping = new std::pair<void*, std::size_t>(data, size);
    const auto unmap = [](void* pages) {
        const auto* held = static_cast<std::pair<void*, std::size_t>*>(pages);
        munmap(held->first, held->second);
        delete held;
    };
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_set_user_data(surface, &mapped_key, mapping, unmap) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        unmap(mapping);
        return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    }
    return surface;
}
// Gives a mapped layer's pages back to the system while it keeps its
// mapping: it reads as clear afterwards. Used only on layers that are
// erased over everything drawn on them, or (the comet's passes) whose
// content is never read before it is cleared again. A layer from Cairo's
// own allocation is left as is.
void release_pages(cairo_surface_t* layer) {
    const auto* mapping = layer ? static_cast<std::pair<void*, std::size_t>*>(
                                      cairo_surface_get_user_data(layer, &mapped_key))
                                : nullptr;
    if (mapping) {
        cairo_surface_flush(layer);
        madvise(mapping->first, mapping->second, MADV_DONTNEED);
        cairo_surface_mark_dirty(layer);
    }
}
// How much of a mapped layer is in memory, for tests; a layer from Cairo's
// own allocation counts nothing.
std::size_t resident_bytes(cairo_surface_t* layer) {
    const auto* mapping = layer ? static_cast<std::pair<void*, std::size_t>*>(
                                      cairo_surface_get_user_data(layer, &mapped_key))
                                : nullptr;
    if (!mapping) {
        return 0;
    }
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> pages((mapping->second + page - 1) / page);
    if (mincore(mapping->first, mapping->second, pages.data()) != 0) {
        throw std::runtime_error("cannot see which of a layer's pages are in memory");
    }
    return page * static_cast<std::size_t>(std::count_if(pages.begin(), pages.end(),
                                                         [](unsigned char in) { return in & 1; }));
}
} // namespace
// After the trail has ended, the layers keep their pages this long for the
// next word before they give them back (R239).
constexpr double layer_idle_seconds = 3;
void PanelRenderer::release_idle_layers(double now) {
    if (layers_idle_since_ < 0 || now - layers_idle_since_ < layer_idle_seconds) {
        return;
    }
    layers_idle_since_ = -1;
    release_pages(trail_);
    release_pages(comet_);
    release_pages(comet_passes_);
}
std::size_t PanelRenderer::trail_resident_bytes() const {
    return resident_bytes(trail_) + resident_bytes(comet_) + resident_bytes(comet_passes_);
}
void PanelRenderer::forget_key_bodies() {
    for (auto& [name, image] : body_atlas_) {
        cairo_surface_destroy(image);
    }
    body_atlas_.clear();
}
std::size_t PanelRenderer::key_body_bytes() const {
    std::size_t bytes = 0;
    for (const auto& [name, image] : body_atlas_) {
        bytes += static_cast<std::size_t>(cairo_image_surface_get_stride(image)) *
                 static_cast<std::size_t>(cairo_image_surface_get_height(image));
    }
    return bytes;
}
void PanelRenderer::settle() {
    settle_ = true;
    layers_idle_since_ = -1;
    // Nothing paints while the panel is hidden; a summon repaints whole.
    forget_key_bodies();
    // The next paint forgets the trail, damaging where it was; its pixels
    // need nothing kept until then.
    for (auto* layer : {&trail_, &comet_, &comet_passes_}) {
        if (*layer) {
            cairo_surface_destroy(*layer);
            *layer = nullptr;
        }
    }
}
PanelRenderer::~PanelRenderer() {
    forget_key_bodies();
    for (auto* layer : {trail_, comet_, comet_passes_}) {
        if (layer) {
            cairo_surface_destroy(layer);
        }
    }
    cairo_surface_destroy(surface_);
}
std::pair<double, double> PanelRenderer::layout_point(const PanelView& view, double x, double y) const {
    const auto p = placement(view);
    return {(x - p.x) / p.scale, (y - p.y) / p.scale};
}
const Key* PanelRenderer::hit_key(const PanelView& view, double x, double y) const {
    // No key takes a press while the candidate list is open; one outside it
    // still shows, and a press there closes the list (App::down).
    if (view.settings || list_open(view)) {
        return nullptr;
    }
    const auto p = placement(view);
    x = (x - p.x) / p.scale;
    y = (y - p.y) / p.scale;
    for (const auto& key : view.layout->keys) {
        if (key.bounds.contains(x, y) && !covered_key(view, key)) {
            return &key;
        }
    }
    if (!view.key_areas) {
        return nullptr;
    }
    // Between keys, the key whose area holds the point. Areas never
    // overlap; should a custom layout's ever do, the nearer key wins.
    const Key* nearest = nullptr;
    double nearest_distance = std::numeric_limits<double>::max();
    for (const auto& area : *view.key_areas) {
        if (area.key >= view.layout->keys.size() || !area.bounds.contains(x, y)) {
            continue;
        }
        const auto& key = view.layout->keys[area.key];
        if (covered_key(view, key)) {
            continue;
        }
        const auto& b = key.bounds;
        const double dx = std::max({b.x - x, 0.0, x - (b.x + b.width)});
        const double dy = std::max({b.y - y, 0.0, y - (b.y + b.height)});
        if (const double distance = std::hypot(dx, dy); distance < nearest_distance) {
            nearest = &key;
            nearest_distance = distance;
        }
    }
    return nearest;
}
const Key* PanelRenderer::swipe_key(const PanelView& view, double x, double y) const {
    if (!view.swipe_reach || view.settings || list_open(view)) {
        return nullptr;
    }
    const auto [lx, ly] = layout_point(view, x, y);
    const Key* nearest = nullptr;
    double nearest_distance = std::numeric_limits<double>::max();
    for (const auto& key : view.layout->keys) {
        const auto reach = view.swipe_reach->find(key.id);
        if (reach == view.swipe_reach->end() || !reach->second.contains(lx, ly) ||
            covered_key(view, key)) {
            continue;
        }
        const auto& b = key.bounds;
        const double dx = std::max({b.x - lx, 0.0, lx - (b.x + b.width)});
        const double dy = std::max({b.y - ly, 0.0, ly - (b.y + b.height)});
        if (const double distance = std::hypot(dx, dy); distance < nearest_distance) {
            nearest = &key;
            nearest_distance = distance;
        }
    }
    return nearest;
}
bool PanelRenderer::paint(const PanelView& view, double now, bool force_full) {
    const auto& t = *view.theme;
    // Key bodies drawn for another theme or scale go; keys draw theirs from
    // here while this paint lasts.
    if (!view.settings) {
        const double scale = placement(view).scale;
        if (atlas_theme_ != t || scale != atlas_scale_) {
            forget_key_bodies();
            atlas_theme_ = t;
            atlas_scale_ = scale;
        }
    }
    struct AtlasScope {
        ~AtlasScope() { body_atlas = nullptr; }
    } atlas_scope;
    body_atlas = &body_atlas_;
    const auto modifiers = view.keyboard->modifiers();
    const bool shift_locked =
        view.keyboard->locked(KEY_LEFTSHIFT) || view.keyboard->locked(KEY_RIGHTSHIFT);
    // A hovered dropdown choice whose name does not fit scrolls. A new one
    // starts from the beginning.
    std::optional<Rect> marquee_text;
    bool marquee_status = false;
    double marquee_shift = 0;
    std::optional<double> marquee_due;
    const bool in_editor = view.dictation && !view.settings;
    const bool status_shown = in_editor && !view.status_card && !view.status.empty() &&
                              dictation_status_line(view).width >= min_status_width;
    {
        const Control* choice = nullptr;
        for (const auto& control : view.controls) {
            if (control.style == ControlStyle::MenuItem && view.hovered_controls.contains(control.id)) {
                choice = &control;
            }
        }
        // An input button can narrow the toolbar's gap: a dictation status too
        // long for two lines of it sits on one, and scrolls under the laser.
        // Moving by itself it would distract. One paint a frame goes on while
        // the microphone's bars move, so the text is measured once.
        std::optional<Rect> status_line;
        double status_overflow = 0;
        if (status_shown && (status_measured_ != view.status || status_font_ != view.interface_font)) {
            auto* measure = cairo_create(surface_);
            grey_text(measure);
            status_width_ = text_width(measure, view.status, 15, view.interface_font);
            cairo_destroy(measure);
            status_measured_ = view.status;
            status_font_ = view.interface_font;
        }
        if (!choice && status_shown) {
            const auto line = dictation_status_line(view);
            if (status_width_ > 1.8 * line.width) {
                status_line = line;
                status_overflow = status_width_ - (line.width - 8);
            }
        }
        const std::string hovered = choice ? choice->id : status_line ? "status: " + view.status : "";
        if (status_line) {
            marquee_status = true;
        }
        if (hovered != marquee_) {
            marquee_ = hovered;
            marquee_since_ = now;
            marquee_shown_ = -1;
            status_scrolling_ = false;
            status_scroll_end_.reset();
        }
        if (choice && view.settings) {
            const auto& r = choice->bounds;
            const Rect text{r.x + 20, r.y, r.width - 40, r.height};
            auto* measure = cairo_create(surface_);
            grey_text(measure);
            const double overflow =
                text_width(measure, choice->label, 21, view.interface_font) - (text.width - 8);
            cairo_destroy(measure);
            if (overflow > 0) {
                const auto step = marquee_step(overflow, now - marquee_since_);
                marquee_text = text;
                marquee_shift = step.offset;
                marquee_due = now + step.wait;
            }
        }
        if (status_line) {
            marquee_text = status_line;
            if (view.status_hovered) {
                status_scroll_end_.reset();
                if (!status_scrolling_) {
                    status_scrolling_ = true;
                    marquee_since_ = now;
                }
            } else if (status_scrolling_ && !status_scroll_end_) {
                const double cycle = marquee_cycle(status_overflow);
                status_scroll_end_ =
                    marquee_since_ + (std::floor((now - marquee_since_) / cycle) + 1) * cycle;
            }
            if (status_scroll_end_ && now >= *status_scroll_end_) {
                status_scrolling_ = false;
                status_scroll_end_.reset();
            }
            if (status_scrolling_) {
                const auto step = marquee_step(status_overflow, now - marquee_since_);
                marquee_shift = step.offset;
                marquee_due = now + step.wait;
                if (status_scroll_end_) {
                    marquee_due = std::min(*marquee_due, *status_scroll_end_);
                }
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
        previous_->view.reading_room != view.reading_room ||
        // The message card moves to clear the tooltip.
        (!view.status.empty() && shown_tooltip(previous_->view) != shown_tooltip(view)) ||
        previous_->view.composing != view.composing || previous_->view.ime_band != view.ime_band ||
        previous_->view.ime_room != view.ime_room || previous_->view.flick_latin != view.flick_latin ||
        previous_->view.flick_digits != view.flick_digits ||
        previous_->view.flick_script_key != view.flick_script_key ||
        previous_->view.flick_katakana != view.flick_katakana ||
        previous_->view.hide_numpad != view.hide_numpad ||
        previous_->view.key_outlines != view.key_outlines ||
        previous_->view.case_opacity != view.case_opacity ||
        previous_->view.key_labels != view.key_labels ||
        previous_->view.disabled_keys != view.disabled_keys ||
        previous_->view.emoji.has_value() != view.emoji.has_value() ||
        previous_->view.dictation.has_value() != view.dictation.has_value() ||
        previous_->view.dictation_available != view.dictation_available ||
        list_open(previous_->view) != list_open(view) ||
        // A resized menu uncovers keys it was drawn over.
        candidate_menu(previous_->view) != candidate_menu(view);
    // A row carried over the popup's lists moves alone, and repaints alone.
    const bool only_lifted_moved = [&] {
        const auto* before = previous_ ? &previous_->view.languages : nullptr;
        if (!before || !*before || !view.languages || !(*before)->lifted || !view.languages->lifted ||
            (*before)->lifted == view.languages->lifted) {
            return false;
        }
        const auto over_lists = [](const Rect& r) {
            const auto area = pixel_bounds(r, tone_shadow + 1);
            return area.y >= languages_list_bounds.y &&
                   area.y + area.height <= languages_list_bounds.y + languages_list_bounds.height;
        };
        if (!over_lists((*before)->lifted->bounds) || !over_lists(view.languages->lifted->bounds)) {
            return false;
        }
        auto carried = **before;
        carried.lifted = view.languages->lifted;
        return carried == *view.languages;
    }();
    // Settings repaints whole, except when nothing in it changed but a
    // scrolling choice name, then only that name's box once it moved a pixel,
    // or that carried row.
    const bool settings_still =
        previous_ && view.settings && previous_->view.settings &&
        previous_->view.controls == view.controls && previous_->view.cards == view.cards &&
        previous_->view.field_labels == view.field_labels && previous_->view.popup == view.popup &&
        previous_->view.status == view.status &&
        previous_->view.hovered_controls == view.hovered_controls &&
        previous_->view.address == view.address && previous_->view.confirm == view.confirm &&
        (only_lifted_moved || previous_->view.languages == view.languages);
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
    // Hover fade, the swipe comet and the trail's fade step at 30 fps, as the
    // mic key's bars do: they cover more keys than a press does, and each
    // repaint uploads the whole texture. Starts are aligned to a step, so the
    // first step after one always shows.
    const double stepped = std::floor(now * 30) / 30;
    const bool settling = std::exchange(settle_, false);
    for (const auto& key : view.layout->keys) {
        if (covered_key(view, key)) {
            continue;
        }
        auto& animation = animations_[key.id];
        const auto pull_of = [&](const PanelView& v) {
            return v.key_flick && v.key_flick->key == key.id ? v.key_flick->pull : 0.0;
        };
        const double old_amount = animation.amount;
        const double target = view.keyboard->pressed(key.id) ? 1 : 0;
        const bool changed_target = target != animation.target;
        if (changed_target) {
            animation.from = animation.amount;
            animation.target = target;
            animation.started = now;
        }
        // A press lands faster than the theme's duration and the release
        // takes longer, each slowing into its end.
        const double duration_ms = t.duration_ms * (animation.target > 0 ? press_share : release_share);
        // Settling, a key let go of while the panel was hidden shows at rest.
        const double progress =
            t.duration_ms == 0 || settling
                ? 1
                : std::clamp((now - animation.started) * 1000 / duration_ms, 0.0, 1.0);
        animation.amount =
            animation.from + (animation.target - animation.from) * handle_ease_out(progress);
        animating_ |= !view.settings && progress < 1;
        continuous |= !view.settings && progress < 1;
        // The laser lights a key at once; with Hover fade the key dims
        // slowly after it leaves, a short trail behind the laser. A theme
        // with key edges keeps its keys as they were: its edge under the
        // laser is a line that can only be on or off (R181).
        auto& hover = hovers_[key.id];
        const double old_lift = hover.amount;
        const double lit =
            view.hovered_keys.contains(key.id) && !view.disabled_keys.contains(key.id) ? 1 : 0;
        if (lit != hover.target) {
            hover.from = hover.amount;
            hover.target = lit;
            hover.started = stepped;
        }
        const double fade = lit > 0 || !view.hover_fade || settling || plain_keys(t)
                                ? 1
                                : std::clamp((stepped - hover.started) / hover_fade_seconds, 0.0, 1.0);
        hover.amount = hover.from + (hover.target - hover.from) * handle_ease_out(fade);
        animating_ |= !view.settings && fade < 1; // Not continuous: see `stepped`.
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
             hover.amount != old_lift ||
             previous_->view.hovered_keys.contains(key.id) != view.hovered_keys.contains(key.id) ||
             pull_of(previous_->view) != pull_of(view))) {
            damage.push_back(key_bounds_.at(key.id));
        }
    }
    // A shaking icon stays inside its button, which alone repaints.
    const double shake = view.settings ? 0 : icon_shake(view.focus_shake_since, now);
    const double swipe_shake = view.settings ? 0 : icon_shake(view.swipe_shake_since, now);
    const double swipe_fade = view.settings ? 0 : icon_shake_fade(view.swipe_shake_since, now);
    for (const double since : {view.focus_shake_since, view.swipe_shake_since}) {
        animating_ |= !view.settings && since >= 0 && now - since < icon_shake_duration + .05;
    }
    auto repaint = [&](const char* id) {
        for (const auto& c : view.controls) {
            if (c.id == id) {
                damage.push_back({std::floor(c.bounds.x), std::floor(c.bounds.y),
                                  std::ceil(c.bounds.width) + 1, std::ceil(c.bounds.height) + 1});
            }
        }
    };
    if (shake != focus_shake_ && !full) {
        repaint("keep-open");
    }
    if ((swipe_shake != swipe_shake_ || swipe_fade != swipe_fade_) && !full) {
        repaint("swipe-toggle");
    }
    swipe_shake_ = swipe_shake;
    swipe_fade_ = swipe_fade;
    focus_shake_ = shake;
    // A busy button's ring steps at 30 fps, as the mic's does, also in Settings.
    const double busy_ring = std::floor(now * 30) / 30;
    for (const auto& c : view.controls) {
        if (c.busy) {
            animating_ = true;
            if (!full && busy_ring != busy_ring_) {
                repaint(c.id.c_str());
            }
        }
    }
    busy_ring_ = busy_ring;
    // The mic key's bars and ring step at 30 fps; between steps a 60 Hz
    // frame would rebuild and compare the whole view for no new pixel.
    next_change_ = continuous ? now : (std::floor(now * 30) + 1) / 30;
    if (marquee_due) {
        next_change_ = animating_ ? std::min(next_change_, *marquee_due) : *marquee_due;
        animating_ = true;
    }
    // Outside the check above: a status returning to its start is due no more.
    if (!full && marquee_text && marquee_shift != marquee_shown_) {
        damage.push_back(pixel_bounds(*marquee_text));
    }
    // A clip edge through a key can drop parts of its legend glyphs. Damage
    // a changed area together with every key it touches, each whole, as
    // separate pixel-aligned rectangles: the clip's edges then run only
    // through the gaps between keys or along key bounds, and nothing chains
    // across the keyboard the way one bounding rectangle would.
    std::vector<Rect> areas;
    const auto contains = [](const Rect& outer, const Rect& inner) {
        return outer.x <= inner.x && outer.y <= inner.y &&
               outer.x + outer.width >= inner.x + inner.width &&
               outer.y + outer.height >= inner.y + inner.height;
    };
    auto damage_area = [&](Rect r) {
        if (std::none_of(areas.begin(), areas.end(),
                         [&](const Rect& area) { return contains(area, r); })) {
            std::erase_if(areas, [&](const Rect& area) { return contains(r, area); });
            areas.push_back(r);
        }
        for (const auto& [id, bounds] : key_bounds_) {
            if (overlaps(bounds, r)) {
                damage.push_back(bounds);
            }
        }
    };
    // The swipe trail lives in two layers that every pass copies. The path
    // layer only gains segments, so pixels already drawn never change; the
    // comet layer is redrawn while any of its pieces is alive. `trail_changed`
    // is where either layer differs from the last paint, in whole pixels.
    std::optional<Rect> trail_changed;
    {
        const auto touched = [&](const Rect& r) {
            trail_changed = trail_changed ? joined(*trail_changed, r) : r;
        };
        // Laser points are fractional. Like key bounds, extents round outwards
        // to whole pixels: an antialiased clip edge would partly clear pixels
        // and leave dark, translucent seams that build up frame by frame.
        const auto around = [](std::pair<double, double> a, std::pair<double, double> b, double reach) {
            const double left = std::floor(std::min(a.first, b.first) - reach);
            const double top = std::floor(std::min(a.second, b.second) - reach);
            return Rect{left, top, std::ceil(std::max(a.first, b.first) + reach) - left,
                        std::ceil(std::max(a.second, b.second) + reach) - top};
        };
        const auto erase = [&](cairo_surface_t* layer, double offset, std::optional<Rect>& extent) {
            if (!extent) {
                return;
            }
            if (layer) { // settle() may have freed it already
                auto* eraser = cairo_create(layer);
                cairo_translate(eraser, 0, offset);
                cairo_rectangle(eraser, extent->x, extent->y, extent->width, extent->height);
                cairo_set_operator(eraser, CAIRO_OPERATOR_CLEAR);
                cairo_fill(eraser);
                cairo_destroy(eraser);
                cairo_surface_flush(layer);
            }
            touched(*extent);
            extent.reset();
        };
        const auto forget = [&] {
            erase(trail_, popup_margin, trail_extent_);
            erase(comet_, 0, comet_extent_);
            comet_pieces_.clear();
            comet_first_ = 0;
            comet_step_ = -1;
            trail_points_.clear();
            trail_stamps_.clear();
            trail_drawn_ = 0;
            trail_released_ = -1;
            trail_fade_ = 1;
        };
        // The layers stay from swipe to swipe while the panel is in use: freed
        // and made again for every word, they would be zeroed each time and
        // might not return to the system (R198). During a swipe the three
        // hold up to about 14 MB. Once a trail has ended they keep their pages
        // for the next word, and give them back after a few idle seconds
        // (release_idle_layers, from App's tick), so words in a row fault
        // nothing in again (R234, R239). They go under Settings and when the
        // panel hides (settle()).
        const auto release_layers = [&] {
            for (auto* layer : {&trail_, &comet_, &comet_passes_}) {
                if (*layer) {
                    cairo_surface_destroy(*layer);
                    *layer = nullptr;
                }
            }
        };
        if (settling) {
            forget(); // A trail from before the panel hid ends without a fade.
            release_layers();
        }
        const bool fading = trail_released_ >= 0;
        if (view.settings) {
            forget(); // Settings covers the keys: nothing fades over its cards.
            release_layers();
        } else if (view.swipe_trail.size() > 1) {
            layers_idle_since_ = -1; // a word in use
            const auto& points = view.swipe_trail;
            const bool grows = !fading && points.size() >= trail_points_.size() &&
                               std::equal(trail_points_.begin(), trail_points_.end(), points.begin());
            if (!grows) {
                forget(); // Another swipe: what was left of the last one goes at once.
            }
            trail_points_ = points;
            trail_stamps_.resize(points.size(), now); // New points are as old as this paint.
        } else if (!trail_points_.empty() && !fading) {
            trail_released_ = stepped; // The swipe ended; its points are final.
        }
        if (const std::size_t count = trail_points_.size(); count > 1) {
            // A tint of the latched colour. A theme with key edges keeps the
            // plain legend colour, its strongest contrast. A neon theme draws
            // the path in its latched colour and the comet as a tube in the
            // colour its tubes start from.
            const bool neon = t.latched_finish == "neon";
            const Color path_ink = neon            ? t.latched
                                   : plain_keys(t) ? t.legend
                                                   : mix(t.legend, t.latched, .22);
            const Color ink = neon ? t.latched_top : path_ink;
            const Color glow = neon            ? t.latched_top
                               : plain_keys(t) ? t.legend
                                               : mix(t.latched, t.legend, .35);
            // A smoothed point is settled once its later neighbours have all
            // arrived, or the swipe has ended. Only settled segments join the path.
            const std::size_t settled = trail_released_ >= 0      ? count
                                        : count > trail_smoothing ? count - trail_smoothing
                                                                  : 0;
            if (settled > std::max<std::size_t>(trail_drawn_, 1)) {
                if (!trail_) {
                    trail_ = mapped_layer(panel_width, texture_height);
                }
                auto* path = cairo_create(trail_);
                cairo_translate(path, 0, popup_margin);
                cairo_set_line_width(path, trail_path_width);
                cairo_set_line_cap(path, CAIRO_LINE_CAP_ROUND);
                source(path, path_ink);
                // Each segment is its own stroke with round caps, so the joins
                // match a joined path while earlier pixels stay as they were.
                for (std::size_t i = std::max<std::size_t>(trail_drawn_, 1); i < settled; ++i) {
                    const auto a = trail_point(trail_points_, i - 1), b = trail_point(trail_points_, i);
                    cairo_move_to(path, a.first, a.second);
                    cairo_line_to(path, b.first, b.second);
                    cairo_stroke(path);
                    const auto piece = around(a, b, trail_path_width / 2 + 2);
                    touched(piece);
                    trail_extent_ = trail_extent_ ? joined(*trail_extent_, piece) : piece;
                }
                cairo_destroy(path);
                cairo_surface_flush(trail_);
                trail_drawn_ = settled;
            }
            // The comet: the pieces newer than its lifetime, each thinner and
            // fainter with age. Stamps never decrease, so they are the last ones.
            // Ages count in 30 fps steps, as the mic key's bars do, so between
            // steps only pieces whose points moved change.
            std::size_t first = count; // Piece i joins points i - 1 and i.
            while (first > 1 && stepped - trail_stamps_[first - 1] < trail_comet_seconds) {
                --first;
            }
            const bool alive = first < count;
            // Pieces drawn at the last paint: comet_first_ up to `drawn`.
            const std::size_t drawn = comet_first_ + comet_pieces_.size();
            const bool same_step = comet_extent_ && stepped == comet_step_ && first == comet_first_;
            if (!alive || !same_step) {
                erase(comet_, 0, comet_extent_);
                comet_pieces_.clear();
            }
            comet_step_ = stepped;
            comet_first_ = first;
            if (alive && !(same_step && count == drawn)) {
                if (!comet_) {
                    comet_ = mapped_layer(panel_width, panel_height);
                }
                if (!comet_passes_) {
                    comet_passes_ = mapped_layer(panel_width, panel_height);
                }
                std::vector<std::pair<double, double>> at; // Smoothed points first - 1 to the head.
                for (std::size_t i = first - 1; i < count; ++i) {
                    at.push_back(trail_point(trail_points_, i));
                }
                std::vector<Rect> pieces; // Each piece's reach, for piece first + k.
                for (std::size_t k = 1; k < at.size(); ++k) {
                    pieces.push_back(around(at[k - 1], at[k], trail_glow_width / 2 + 2));
                }
                Rect extent = pieces.front();
                for (const auto& piece : pieces) {
                    extent = joined(extent, piece);
                }
                // On a new step every piece ages, so the whole comet is redrawn.
                // Within a step new points only move the smoothed points that
                // reach them, the last `trail_smoothing` ones: only the pieces
                // from there change, where they were and where they are now.
                Rect area = extent;
                if (same_step) {
                    const std::size_t from = std::max(first, drawn - std::min(drawn, trail_smoothing));
                    std::optional<Rect> moved;
                    const auto add = [&](const Rect& r) { moved = moved ? joined(*moved, r) : r; };
                    for (std::size_t i = from; i < drawn; ++i) {
                        add(comet_pieces_[i - first]);
                    }
                    for (std::size_t i = from; i < count; ++i) {
                        add(pieces[i - first]);
                    }
                    area = *moved; // count > drawn, so at least one new piece.
                    auto* eraser = cairo_create(comet_);
                    cairo_rectangle(eraser, area.x, area.y, area.width, area.height);
                    cairo_set_operator(eraser, CAIRO_OPERATOR_CLEAR);
                    cairo_fill(eraser);
                    cairo_destroy(eraser);
                }
                // Only pieces whose reach meets `area` can change a pixel in it,
                // so a partial redraw strokes just those (R216). The groups
                // take the reach of the pieces drawn, which no stroke gets to
                // the edge of, so each comes out exactly as in a whole redraw;
                // only their compositing is limited to `area`, outside which
                // the layer keeps its pixels. A clip at `area` itself would
                // cut through pieces and shade their edges slightly
                // differently (R199). Without a clip each group would cover
                // the whole layer.
                std::vector<char> drawn_piece(pieces.size(), 1);
                Rect groups = extent;
                if (same_step) {
                    std::optional<Rect> reach;
                    for (std::size_t k = 0; k < pieces.size(); ++k) {
                        drawn_piece[k] = overlaps(pieces[k], area);
                        if (drawn_piece[k]) {
                            reach = reach ? joined(*reach, pieces[k]) : pieces[k];
                        }
                    }
                    groups = reach.value_or(area); // the new pieces lie in `area`
                }
                auto* comet = cairo_create(comet_);
                cairo_rectangle(comet, groups.x, groups.y, groups.width, groups.height);
                cairo_clip(comet);
                // Each piece replaces what lies under it instead of adding to
                // it, so overlapping round caps don't darken the joins, and a
                // newer, stronger piece wins where two meet. A pass is drawn on
                // `comet_passes_`, cleared over `groups` first, as a group
                // would be, then laid onto the comet within `area`.
                const auto strokes = [&](double width, double strength, Color colour) {
                    auto* pass = cairo_create(comet_passes_);
                    cairo_rectangle(pass, groups.x, groups.y, groups.width, groups.height);
                    cairo_clip(pass);
                    cairo_set_operator(pass, CAIRO_OPERATOR_CLEAR);
                    cairo_paint(pass);
                    cairo_set_operator(pass, CAIRO_OPERATOR_SOURCE);
                    cairo_set_line_cap(pass, CAIRO_LINE_CAP_ROUND);
                    for (std::size_t i = first; i < count; ++i) {
                        if (!drawn_piece[i - first]) {
                            continue;
                        }
                        const double left =
                            std::clamp(1 - (stepped - trail_stamps_[i]) / trail_comet_seconds, 0.0, 1.0);
                        const double eased = left * left * (3 - 2 * left);
                        const auto& a = at[i - first];
                        const auto& b = at[i - first + 1];
                        cairo_move_to(pass, a.first, a.second);
                        cairo_line_to(pass, b.first, b.second);
                        cairo_set_source_rgba(pass, colour.r, colour.g, colour.b, eased);
                        cairo_set_line_width(pass, width * (.25 + .75 * eased));
                        cairo_stroke(pass);
                    }
                    cairo_destroy(pass);
                    cairo_surface_flush(comet_passes_);
                    cairo_save(comet);
                    cairo_rectangle(comet, area.x, area.y, area.width, area.height);
                    cairo_clip(comet);
                    cairo_set_source_surface(comet, comet_passes_, 0, 0);
                    cairo_paint_with_alpha(comet, strength);
                    cairo_restore(comet);
                };
                strokes(trail_glow_width, neon ? .26 : .16, glow);
                strokes(trail_comet_width, neon ? .95 : .9, ink);
                if (neon) {
                    strokes(2.4, .7, mix(ink, Color{1, 1, 1}, .75)); // The tube's hot core.
                }
                cairo_destroy(comet);
                cairo_surface_flush(comet_);
                comet_extent_ = extent;
                comet_pieces_ = std::move(pieces);
                touched(area);
            }
            // After the swipe the path fades out instead of vanishing, in the
            // same 30 fps steps.
            const double fade =
                trail_released_ < 0
                    ? 1
                    : std::clamp(1 - (stepped - trail_released_) / trail_end_seconds, 0.0, 1.0);
            if (fade != trail_fade_ && trail_extent_) {
                touched(*trail_extent_);
            }
            trail_fade_ = fade;
            if (trail_released_ >= 0 && fade <= 0 && !alive) {
                forget();
                layers_idle_since_ = now; // their pages go after a few idle seconds
            } else if (alive || trail_released_ >= 0) {
                // The comet and the fade step at 30 fps, as the mic key's bars
                // do: they cover more keys than a pressed key does. A resting
                // laser's comet burns out, and then nothing repaints.
                animating_ = true;
                next_change_ = std::min(next_change_, (std::floor(now * 30) + 1) / 30);
            }
        }
    }
    if (!full && trail_changed) {
        damage_area(*trail_changed);
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
    // The reading, hint and candidates repaint only their own area, as does
    // the flick preview below. The preview's old and new extents both count;
    // it may lie beyond the case.
    if (!full && view.ime_band &&
        (previous_->view.preedit != view.preedit ||
         previous_->view.preedit_active_begin != view.preedit_active_begin ||
         previous_->view.preedit_active_end != view.preedit_active_end ||
         previous_->view.preedit_cursor != view.preedit_cursor ||
         previous_->view.candidates != view.candidates)) {
        const auto band = ime_band(view);
        for (const auto& area : {band.band, band.reading, band.candidates}) {
            damage_area(pixel_bounds(area, 6));
        }
        if (const auto menu = candidate_menu(view)) {
            damage_area(pixel_bounds(*menu, tone_shadow));
        } else if (list_open(view)) {
            damage_area(pixel_bounds(*view.candidates->list, 6));
        }
    }
    // A tooltip that comes, goes, or changes text or button repaints its old
    // and new places above the case.
    if (!full) {
        const auto before = shown_tooltip(previous_->view), after = shown_tooltip(view);
        for (const auto& bounds :
             {focus_tooltip_area(previous_->view, before), focus_tooltip_area(view, after)}) {
            if (bounds && before != after) {
                // The card's shadow spreads a few pixels past it.
                const double left = std::floor(bounds->x - 8), top = std::floor(bounds->y - 4);
                damage_area({left, top, std::ceil(bounds->x + bounds->width + 8) - left,
                             std::ceil(bounds->y + bounds->height + 8) - top});
            }
        }
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
    if (!full && only_lifted_moved) {
        // One area, grown to every row it touches: its edge must not cut
        // through a row's text, which a clip draws differently.
        const auto old_row = pixel_bounds(previous_->view.languages->lifted->bounds, tone_shadow + 1);
        const auto new_row = pixel_bounds(view.languages->lifted->bounds, tone_shadow + 1);
        double left = std::min(old_row.x, new_row.x), top = std::min(old_row.y, new_row.y);
        double right = std::max(old_row.x + old_row.width, new_row.x + new_row.width);
        double bottom = std::max(old_row.y + old_row.height, new_row.y + new_row.height);
        const auto take = [&](const Rect& bounds) {
            const auto row = pixel_bounds(bounds, 1);
            if (overlaps(row, {left, top, right - left, bottom - top}) &&
                (row.x < left || row.x + row.width > right || row.y < top ||
                 row.y + row.height > bottom)) {
                left = std::min(left, row.x);
                top = std::min(top, row.y);
                right = std::max(right, row.x + row.width);
                bottom = std::max(bottom, row.y + row.height);
                return true;
            }
            return false;
        };
        for (bool grew = true; grew;) {
            grew = take(view.languages->main.bounds);
            for (const auto& control : view.controls) {
                grew |= take(control.bounds);
            }
        }
        damage_area({left, top, right - left, bottom - top});
    }
    // The editor and its counter repaint only their own areas.
    if (!full && view.dictation && previous_->view.candidates != view.candidates) {
        damage_area(pixel_bounds(view.dictation->bounds, 2));
    }
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
        // These surfaces are only used to measure. A key's shared body is an
        // image with transparent room around it; measuring the body drawn
        // keeps that room out of the bounds, so neighbours' repaints don't
        // overlap and clip each other's lit keys.
        if (!view.settings) {
            const auto p = placement(view);
            body_atlas = nullptr;
            for (const auto& key : view.layout->keys) {
                if (covered_key(view, key)) {
                    continue;
                }
                auto* recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
                auto* bounds_cr = cairo_create(recording);
                grey_text(bounds_cr);
                cairo_translate(bounds_cr, p.x, p.y);
                cairo_scale(bounds_cr, p.scale, p.scale);
                draw_key(bounds_cr, view, key, 0, 0);
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
            body_atlas = &body_atlas_;
        }
        previous_ = Snapshot{
            *view.layout,         t,           *view.language, view, modifiers, view.keyboard->caps(),
            view.keyboard->num(), shift_locked};
        // No animation state from a removed key should survive a profile reload.
        const auto gone = [&](const auto& item) {
            return std::none_of(view.layout->keys.begin(), view.layout->keys.end(), [&](const Key& key) {
                return key.id == item.first && !hidden_key(view, key);
            });
        };
        std::erase_if(animations_, gone);
        std::erase_if(hovers_, gone);
    } else {
        previous_->view = view;
    }
    if (!full && damage.empty() && areas.empty()) {
        return false;
    }
    marquee_shown_ = marquee_text ? marquee_shift : -1;
    auto* cr = cairo_create(surface_);
    grey_text(cr);
    cairo_translate(cr, 0, popup_margin);
    // Cairo composites antialiased strokes and glyphs slightly differently
    // under a clip of several rectangles, or a clip edge through a glyph. So
    // repaint each damaged rectangle in its own pass with a single box clip,
    // each pass clearing and redrawing everything in its box: first the
    // changed areas, then every touched key whole, so an area edge through a
    // key is painted over by that key's exact pass.
    std::vector<Rect> passes;
    if (!full) {
        // A key whole inside a changed area, as under the open candidate
        // menu, repaints with that area.
        passes = areas;
        for (const auto& r : damage) {
            if (std::none_of(passes.begin(), passes.end(),
                             [&](const Rect& pass) { return contains(pass, r); })) {
                std::erase_if(passes, [&](const Rect& pass) { return contains(r, pass); });
                passes.push_back(r);
            }
        }
    }
    // The flick preview's soft shadows rasterize slightly differently under
    // each pass's clip, so the preview is drawn once, into an image at whole
    // pixels, which every pass copies exactly.
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> preview_image(
        nullptr, cairo_surface_destroy);
    Rect preview_at{};
    if (const auto cells = flick_cells(view); cells && !view.settings) {
        preview_at = *flick_preview_bounds(view);
        preview_image.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       static_cast<int>(preview_at.width),
                                                       static_cast<int>(preview_at.height)));
        auto* preview = cairo_create(preview_image.get());
        grey_text(preview);
        cairo_translate(preview, -preview_at.x, -preview_at.y);
        draw_flick_preview(preview, view, *cells);
        cairo_destroy(preview);
        cairo_surface_flush(preview_image.get());
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
        // see-through background shows the dashboard or app behind the keys.
        if (const double opacity = clear_case(view) ? view.case_opacity : 1; opacity > 0) {
            rounded(cr, body, t.surface_radius);
            cairo_set_source_rgba(cr, t.surface.r, t.surface.g, t.surface.b, opacity);
            cairo_fill(cr);
        }
        if (!view.settings) {
            const auto p = placement(view);
            cairo_save(cr);
            cairo_translate(cr, p.x, p.y);
            cairo_scale(cr, p.scale, p.scale);
            for (const auto& key : view.layout->keys) {
                if (covered_key(view, key)) {
                    continue;
                }
                const auto& bounds = key_bounds_.at(key.id);
                const bool intersects =
                    full || std::any_of(pass.begin(), pass.end(), [&](const Rect& r) {
                        return bounds.x < r.x + r.width && bounds.x + bounds.width > r.x &&
                               bounds.y < r.y + r.height && bounds.y + bounds.height > r.y;
                    });
                if (intersects) {
                    draw_key(cr, view, key, animations_.at(key.id).amount, hovers_.at(key.id).amount,
                             mic_phase_, ring_time_);
                }
            }
            if (view.key_outlines) {
                draw_key_outlines(cr, view, p.scale);
            }
            cairo_restore(cr);
        }
        if (trail_extent_) {
            const auto& r = *trail_extent_;
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_clip(cr);
            cairo_set_source_surface(cr, trail_, 0, -popup_margin);
            // The whole path, quiet enough to leave the legends under it readable.
            cairo_paint_with_alpha(
                cr,
                (t.latched_finish == "neon" ? trail_neon_path_alpha : trail_path_alpha) * trail_fade_);
            cairo_restore(cr);
        }
        if (comet_extent_) {
            const auto& r = *comet_extent_;
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_clip(cr);
            cairo_set_source_surface(cr, comet_, 0, 0);
            cairo_paint(cr);
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
                draw_edge(cr, t, tab.bounds, t.radius, tab.pressed || tab.hovered, true);
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
                    draw_edge(cr, t, face, t.radius, true, true); // Only a theme with edges.
                }
                emoji_image(cell.text, cell.bounds);
            }
            if (!picker.hint.empty()) {
                interface_label(cr, picker.hint, picker.hint_bounds, t.small_font_size * 1.3,
                                view.interface_font, disabled_legend(t), false, PANGO_ALIGN_CENTER,
                                true);
            }
            if (!picker.favorites_hint.empty()) {
                const auto& r = picker.favorites_hint_bounds;
                interface_label(cr, picker.favorites_hint, {r.x + 8, r.y, r.width - 16, r.height},
                                t.small_font_size * 1.1, view.interface_font, disabled_legend(t), false,
                                PANGO_ALIGN_CENTER, true);
            }
            cairo_restore(cr);
        }
        if (view.dictation && !view.settings) {
            draw_editor(cr, view);
            if (view.candidates && !view.ime_band) {
                draw_candidates(cr, view, *view.candidates); // on the band over its foot
            }
        }
        draw_focus_tooltip(cr, view);
        if (!view.status.empty() && (!in_editor || view.status_card)) {
            draw_status_card(cr, view);
        }
        if (view.ime_band && !view.settings) {
            draw_ime_band(cr, view);
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
                interface_label(
                    cr, card.title, {card.bounds.x + 30, card.bounds.y + 12, card.bounds.width - 60, 32},
                    19, view.interface_font, mix(t.legend, t.surface, .35), false, PANGO_ALIGN_LEFT);
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
                interface_label(cr, field.text, field.bounds, field.size, view.interface_font,
                                !field.enabled  ? disabled_legend(t)
                                : field.warning ? warning
                                                : t.legend,
                                field.left && !field.wrap,
                                field.left ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT, field.wrap, false,
                                field.below);
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
                        scrolled_label(cr, control.label, text, 21, view.interface_font, legend,
                                       marquee_shift);
                    } else {
                        interface_label(cr, control.label, text, 21, view.interface_font, legend, false,
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
                    interface_label(cr, control.label, {r.x + 18, r.y, r.width - 36, r.height}, 21,
                                    view.interface_font, legend, false, PANGO_ALIGN_LEFT);
                    break;
                }
                case ControlStyle::Segment: {
                    // Each segment paints its slice of the shared well, so it
                    // can repaint alone within its own bounds.
                    // A hard clip: soft edges would show as seams between them.
                    cairo_save(cr);
                    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
                    cairo_rectangle(cr, r.x, r.y, r.width, r.height);
                    cairo_clip(cr);
                    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
                    draw_well(cr, t,
                              {r.x - control.action.index * r.width, r.y, r.width * control.segments,
                               r.height});
                    cairo_restore(cr);
                    const Rect thumb{r.x + 4, r.y + 4, r.width - 8, r.height - 8};
                    if (control.selected && control.enabled) {
                        fill_latched(cr, t, thumb, t.radius);
                    } else if (hovered) {
                        rounded(cr, thumb, t.radius);
                        source(cr, t.hover);
                        cairo_fill(cr);
                    }
                    if (control.icon != Icon::None) {
                        draw_icon(cr, control.icon, r, legend, 30);
                    } else {
                        interface_label(cr, control.label, r, 21, view.interface_font, legend);
                    }
                    break;
                }
                case ControlStyle::Dropdown: {
                    draw_cap(cr, t, r, hovered, control.selected, control.enabled);
                    interface_label(cr, control.label, {r.x + 18, r.y, r.width - 62, r.height}, 21,
                                    view.interface_font, legend, false, PANGO_ALIGN_LEFT);
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
                        interface_label(cr, control.label, r, 21, view.interface_font, legend);
                    }
                    if (control.busy) {
                        draw_ring(cr, t, r, busy_ring_);
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
                if (control.style != ControlStyle::MenuItem) {
                    draw_edge(cr, t, r, radius, hovered, control.enabled); // A toolbar button's.
                }
            }
            if (control.style == ControlStyle::Scrollbar) {
            } else if (control.style == ControlStyle::Dropdown) {
                interface_label(cr, control.label, {r.x + 6, r.y, r.width - 58, r.height}, 22,
                                view.interface_font, legend);
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
                auto at = r;
                auto tint = control.warning ? t.alert : control.dimmed ? disabled_legend(t) : legend;
                if (control.id == "keep-open") {
                    at.x += focus_shake_; // Its button stays; only the icon shakes.
                } else if (control.id == "swipe-toggle") {
                    at.x += swipe_shake_;
                    tint = mix(tint, t.alert, swipe_fade_);
                }
                draw_icon(cr, control.icon, at, tint);
            } else {
                // Toolbar names such as a language's short name shrink to fit,
                // in that language's own fonts.
                interface_label(cr, control.label, r, view.settings ? 22 : 19,
                                control.action.kind == ControlKind::LanguageCycle ? view.language->font
                                                                                  : view.interface_font,
                                legend, !view.settings);
            }
            if (control.badge) {
                // Stays inside the button, whose bounds are its repaint damage.
                // The ring of the button's own colour parts it from the icon.
                constexpr double pi = 3.141592653589793;
                const double x = r.x + r.width - 11, y = r.y + 11;
                cairo_new_path(cr);
                cairo_arc(cr, x, y, 7, 0, 2 * pi);
                source(cr, control.enabled && view.hovered_controls.contains(control.id) ? t.hover
                                                                                         : t.bottom);
                cairo_fill(cr);
                cairo_arc(cr, x, y, 5, 0, 2 * pi);
                source(cr, t.alert);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        };
        for (const auto& control : view.controls) {
            if (control.style != ControlStyle::MenuItem && control.id != "scroll-menu" &&
                !control.id.starts_with("address-") && !control.id.starts_with("confirm-") &&
                !control.id.starts_with("languages-")) {
                draw_control(control);
            }
        }
        if (view.settings && !view.version.empty()) {
            // Beside the button that acts on it, quiet enough to overlook.
            for (const auto& control : view.controls) {
                if (control.action.kind == ControlKind::Updates) {
                    const auto& r = control.bounds;
                    interface_label(cr, view.version, {r.x + r.width + 6, r.y, 120, r.height}, 16,
                                    view.interface_font, disabled_legend(t), false, PANGO_ALIGN_LEFT);
                }
            }
        }
        if (status_shown) {
            const auto line = dictation_status_line(view);
            case_plate(cr, view, line, view.status, 15, PANGO_ALIGN_LEFT);
            if (marquee_status) {
                scrolled_label(cr, view.status, line, 15, view.interface_font, t.legend, marquee_shift);
            } else {
                interface_label(cr, view.status, line, 15, view.interface_font, t.legend, false,
                                PANGO_ALIGN_LEFT, true);
            }
        }
        if (preview_image) {
            // Above the keys, the reading band and the toolbar, which a
            // top-row key's caps can reach (draw_flick_preview()).
            cairo_set_source_surface(cr, preview_image.get(), preview_at.x, preview_at.y);
            cairo_paint(cr);
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
        if (view.languages) {
            // One well split by faint lines, as the emoji picker's sections
            // are: layouts act as tabs, languages list, the chosen ones raised.
            const auto& popup = *view.languages;
            modal_panel(popup.panel);
            interface_label(cr, popup.title, {popup.panel.x + 92, popup.panel.y + 10, 500, 42}, 24,
                            view.interface_font, t.legend, false, PANGO_ALIGN_LEFT);
            draw_well(cr, t, popup.well);
            for (const double x : {popup.languages_list.x - 15, popup.additional_list.x - 16}) {
                cairo_move_to(cr, std::round(x) + .5, popup.well.y + 12);
                cairo_line_to(cr, std::round(x) + .5, popup.well.y + popup.well.height - 12);
                cairo_set_source_rgba(cr, t.legend.r, t.legend.g, t.legend.b, .07);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            const auto caption = [&](const std::string& text, double x, double y) {
                interface_label(cr, text, {x, y, 340, 30}, 17, view.interface_font, disabled_legend(t),
                                false, PANGO_ALIGN_LEFT);
            };
            caption(popup.layouts_caption, popup.layouts_list.x, popup.layouts_list.y - 36);
            caption(popup.languages_caption, popup.languages_list.x, popup.languages_list.y - 36);
            caption(popup.main_caption, popup.additional_list.x, popup.layouts_list.y - 36);
            caption(popup.additional_caption, popup.additional_list.x, popup.additional_list.y - 40);
            const auto hovered = [&](const auto& row) { return view.hovered_controls.contains(row.id); };
            // The title, and under it a quieter note when there is one. A
            // swipe icon, when shown, sits before them.
            const auto text = [&](const auto& row, double right, Color title, Color note) {
                const auto& r = row.bounds;
                const double left = row.swipe ? 46 : 12;
                const double width = r.width - left - right + 12;
                if (row.note.empty()) {
                    interface_label(cr, row.title, {r.x + left, r.y, width, r.height}, 21,
                                    view.interface_font, title, false, PANGO_ALIGN_LEFT);
                    return;
                }
                interface_label(cr, row.title, {r.x + left, r.y + 3, width, 28}, 21, view.interface_font,
                                title, false, PANGO_ALIGN_LEFT);
                interface_label(cr, row.note, {r.x + left, r.y + 29, width, 21}, 15, view.interface_font,
                                note, false, PANGO_ALIGN_LEFT);
            };
            const Color quiet = disabled_legend(t);
            const Color off = mix(t.legend, t.surface, .72);
            // At the row's start; the slashed form stays quieter.
            const auto swipe_icon = [&](const auto& row) {
                if (row.swipe) {
                    draw_icon(cr, *row.swipe ? Icon::Swipe : Icon::SwipeOff,
                              {row.bounds.x + 6, row.bounds.y, 40, row.bounds.height},
                              *row.swipe ? quiet : off, 24);
                }
            };
            const auto clipped = [&](const Rect& list, const auto& draw) {
                cairo_save(cr);
                cairo_rectangle(cr, list.x, list.y, list.width, list.height);
                cairo_clip(cr);
                draw();
                cairo_restore(cr);
            };
            clipped(popup.layouts_list, [&] {
                for (const auto& row : popup.layouts) {
                    if (row.selected) {
                        fill_latched(cr, t, row.bounds, t.radius);
                    } else if (hovered(row)) {
                        rounded(cr, row.bounds, t.radius);
                        source(cr, t.bottom);
                        cairo_fill(cr);
                    }
                    text(row, 24, t.legend, quiet);
                }
            });
            clipped(popup.languages_list, [&] {
                for (const auto& row : popup.languages) {
                    if (row.enabled && hovered(row)) {
                        rounded(cr, row.bounds, t.radius);
                        source(cr, t.bottom);
                        cairo_fill(cr);
                    }
                    text(row, 56, row.enabled ? t.legend : off, row.enabled ? quiet : off);
                    swipe_icon(row);
                    if (row.checked) {
                        draw_icon(
                            cr, Icon::Apply,
                            {row.bounds.x + row.bounds.width - 44, row.bounds.y, 40, row.bounds.height},
                            quiet, 20);
                    }
                }
            });
            draw_cap(cr, t, popup.main.bounds, hovered(popup.main), popup.main_target, true);
            text(popup.main, 92, t.legend, quiet);
            swipe_icon(popup.main);
            clipped(popup.additional_list, [&] {
                for (const auto& row : popup.additional) {
                    draw_cap(cr, t, row.bounds, hovered(row), false, true);
                    text(row, 24, t.legend, quiet);
                    swipe_icon(row);
                }
            });
            if (!popup.hint.empty()) {
                interface_label(cr, popup.hint,
                                {popup.additional_list.x + 34, popup.additional_list.y + 60,
                                 popup.additional_list.width - 68, 120},
                                18, view.interface_font, quiet, false, PANGO_ALIGN_CENTER, true);
            }
            for (const auto& thumb :
                 {popup.layouts_thumb, popup.languages_thumb, popup.additional_thumb}) {
                if (thumb) {
                    rounded(cr, *thumb, 3);
                    source(cr, light_theme(t) ? mix(t.surface, t.legend, .22) : t.hover);
                    cairo_fill(cr);
                }
            }
            for (const auto& control : view.controls) {
                if (control.id.starts_with("languages-") && !control.id.starts_with("languages-row-")) {
                    draw_control(control);
                    if (control.id == "languages-clear" && control.enabled &&
                        view.hovered_controls.contains(control.id)) {
                        // Its name, to the button's left, over the caption.
                        const Rect tip{control.bounds.x - 310, control.bounds.y, 300,
                                       control.bounds.height};
                        rounded(cr, tip, t.radius);
                        source(cr, mix(t.bottom, t.surface, .25));
                        cairo_fill(cr);
                        interface_label(cr, control.label, tip, 17, view.interface_font, t.legend);
                    }
                }
            }
            if (popup.lifted) {
                const auto& row = *popup.lifted;
                soft_shadow(cr, row.bounds, t.radius + 2, tone_shadow);
                draw_cap(cr, t, row.bounds, true, false, !row.removing);
                auto shown = row;
                if (row.removing) {
                    shown.note = popup.drop_note;
                }
                text(shown, 24, row.removing ? quiet : t.legend, quiet);
                swipe_icon(row);
            }
        }
        if (view.confirm) {
            const auto& popup = *view.confirm;
            modal_panel(popup.panel);
            interface_label(cr, popup.title,
                            {popup.panel.x + 40, popup.panel.y + 30, popup.panel.width - 80, 36}, 25,
                            view.interface_font, t.legend, false, PANGO_ALIGN_LEFT);
            interface_label(
                cr, popup.body,
                {popup.panel.x + 40, popup.panel.y + 78, popup.panel.width - 80, popup.body_height}, 18,
                view.interface_font, mix(t.legend, t.surface, .3), false, PANGO_ALIGN_LEFT, true);
            for (const auto& row : popup.rows) {
                // The name and its note; the row's switch is its control.
                const Rect& r = row.bounds;
                interface_label(cr, row.title, {r.x, r.y, r.width - 80, 30}, 22, view.interface_font,
                                t.legend, false, PANGO_ALIGN_LEFT);
                interface_label(cr, row.note, {r.x, r.y + 30, r.width - 80, 22}, 16, view.interface_font,
                                mix(t.legend, t.surface, .3), false, PANGO_ALIGN_LEFT);
            }
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
            interface_label(cr, popup.title,
                            {popup.panel.x + 30, popup.panel.y + 18, popup.panel.width - 140, 32}, 19,
                            view.interface_font, mix(t.legend, t.surface, .35), false, PANGO_ALIGN_LEFT);
            draw_well(cr, t, popup.field);
            auto* text = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_new();
            pango_font_description_set_family(font, view.interface_font.c_str());
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
            interface_label(cr, popup.message, popup.hint, 17, view.interface_font,
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
// Qualified calls reach the file's own helpers in the unnamed namespace.
namespace paint {
void rounded(cairo_t* cr, Rect r, double radius) {
    framekeyboard::rounded(cr, r, radius);
}
void source(cairo_t* cr, Color color) {
    framekeyboard::source(cr, color);
}
Color mix(Color a, Color b, double amount) {
    return framekeyboard::mix(a, b, amount);
}
void settings_card(cairo_t* cr, const Theme& theme, const Rect& r) {
    draw_settings_card(cr, theme, r);
}
void cap(cairo_t* cr, const Theme& theme, const Rect& r, bool hovered, bool selected, bool enabled) {
    draw_cap(cr, theme, r, hovered, selected, enabled);
}
Color well(cairo_t* cr, const Theme& theme, const Rect& r) {
    return draw_well(cr, theme, r);
}
void latched(cairo_t* cr, const Theme& theme, const Rect& r, double radius) {
    fill_latched(cr, theme, r, radius);
}
Rect key(cairo_t* cr, const Theme& theme, const Rect& face, double amount, double lift, bool latched,
         bool character) {
    return draw_keycap(cr, theme, key_tone(theme, character), face, amount, lift,
                       latched ? KeyFace::Latched : KeyFace::Plain, lift > 0 || amount > 0);
}
Color shift_legend(const Theme& theme, bool latched, bool character) {
    return quiet_legend(theme, key_tone(theme, character), latched);
}
void label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
           Color color, PangoAlignment alignment, bool wrap) {
    interface_label(cr, text, r, size, family, color, false, alignment, wrap);
}
} // namespace paint
} // namespace framekeyboard
