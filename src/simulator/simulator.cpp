#include "simulator.hpp"
#include "framekeyboard/mozc.hpp"
#include <algorithm>
#include <pango/pangocairo.h>
#include <stdexcept>
#include <utility>

namespace framekeyboard::simulator {
namespace {
// Neutral colors of a desktop text box, apart from any keyboard theme.
struct Rgb {
    double r, g, b;
};
constexpr Rgb background{.094, .098, .106};
constexpr Rgb field_fill{.137, .141, .157};
constexpr Rgb field_edge{.231, .239, .263};
constexpr Rgb ink{.925, .925, .945};
constexpr Rgb faint{.486, .498, .529};
constexpr Rgb accent{.353, .663, 1};
constexpr Rgb selected{.176, .357, .561};
constexpr Rgb button_fill{.169, .176, .192};
constexpr Rgb button_on{.184, .310, .478};
constexpr double field_padding = 14, field_font = 26, bar_font = 17;
// The simulator's controls sit below the keyboard's case.
constexpr double bar_y = popup_margin + panel_height + 22, bar_height = 44;
constexpr const char* placeholder = "Type here with the keyboard below. The text stays in this window.";
constexpr const char* hints = "Left button: point and press   ·   Right click: hold a second pointer, "
                              "again to let go   ·   Touch: one pointer per finger";

void set(cairo_t* cr, Rgb color, double alpha = 1) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
}
void rounded(cairo_t* cr, Rect r, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;
Layout text_layout(cairo_t* cr, const std::string& text, double size, const std::string& locale) {
    Layout layout(pango_cairo_create_layout(cr), g_object_unref);
    // The keyboard's language picks the font, so Japanese, Chinese and Korean
    // characters take their own shapes.
    if (!locale.empty()) {
        pango_context_set_language(pango_layout_get_context(layout.get()),
                                   pango_language_from_string(locale.c_str()));
        pango_layout_context_changed(layout.get());
    }
    auto* font = pango_font_description_from_string("Noto Sans, sans-serif");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font);
    pango_font_description_free(font);
    pango_layout_set_text(layout.get(), text.c_str(), static_cast<int>(text.size()));
    return layout;
}
Rect field_inner() {
    const auto& r = Simulator::field_bounds;
    return {r.x + field_padding, r.y + field_padding, r.width - 2 * field_padding,
            r.height - 2 * field_padding};
}
Layout field_layout(cairo_t* cr, const TextField& field, const std::string& locale) {
    auto layout = text_layout(cr, field.text(), field_font, locale);
    pango_layout_set_width(layout.get(), static_cast<int>(field_inner().width * PANGO_SCALE));
    pango_layout_set_wrap(layout.get(), PANGO_WRAP_WORD_CHAR);
    return layout;
}
void button(cairo_t* cr, Rect r, const char* text, bool on) {
    rounded(cr, r, 8);
    set(cr, on ? button_on : button_fill);
    cairo_fill_preserve(cr);
    set(cr, on ? accent : field_edge);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    auto layout = text_layout(cr, text, bar_font, {});
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout.get(), &w, &h);
    set(cr, ink);
    cairo_move_to(cr, r.x + (r.width - w) / 2, r.y + (r.height - h) / 2);
    pango_cairo_show_layout(cr, layout.get());
}
} // namespace

Rect Simulator::slow_button() {
    return {panel_width - 16 - 150 - 12 - 200, bar_y, 200, bar_height};
}
Rect Simulator::clear_button() {
    return {panel_width - 16 - 150, bar_y, 150, bar_height};
}

Simulator::Simulator(const Setup& setup)
    : profiles_(load_profiles(setup.data_dir, setup.config_dir)),
      canvas_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy) {
    sink_.set_slow(setup.slow);
    // App types only in a VR launch with an input backend and typing enabled
    // (App::refresh_typing); the field's sink stands in for that backend.
    // Nothing else of a VR launch runs here: the VR loop, instance lock,
    // startup unit and native replacement all belong to main.cpp's VR path.
    Options options;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    options.target_language = "en-us";
    options.config_dir = setup.config_dir;
    // What its Anthy learns stays in its own folder, apart from the user's.
    options.anthy_home = setup.config_dir;
    options.data_dir = setup.data_dir;
    options.models_dir = setup.models_dir;
    if (!setup.japanese_converter) {
        options.mozc_server = mozc_engine(); // As the keyboard: Mozc where it is bundled.
    }
    // For testing conversion: Mozc is waited for rather than left to Anthy.
    options.mozc_patience = std::chrono::seconds(3);
    // Debug mode is always on here, in builds that have it: what Anthy
    // converts anyway has an orange border (Settings → Debug).
    options.debug = true;
    // Dictation's paste Send writes the field's own clipboard.
    options.clipboard = &sink_;
    options.japanese_converter = setup.japanese_converter;
    options.wait_for_japanese = setup.wait_for_japanese;
    app_ = std::make_unique<App>(options, sink_);
    // The window is the keyboard's place: nothing moves or resizes it. The ×
    // shows, so the toolbar lays out as on the headset, but closes nothing
    // (on_close).
    App::HostChrome chrome;
    chrome.move = chrome.resize = false;
    app_->set_chrome(chrome);
    if (setup.layout || setup.language || setup.theme) {
        auto selection = app_->selection();
        auto known = [](bool found, const std::string& kind, const std::string& id) {
            if (!found) {
                throw std::runtime_error("unknown " + kind + " " + id + "; --list shows them");
            }
        };
        if (setup.language) {
            known(profiles_.languages.contains(*setup.language), "language", *setup.language);
            selection.language = *setup.language;
        }
        if (setup.theme) {
            known(profiles_.themes.contains(*setup.theme), "theme", *setup.theme);
            selection.theme = *setup.theme;
        }
        if (setup.layout) {
            known(profiles_.layouts.contains(*setup.layout), "layout", *setup.layout);
            selection.layout = *setup.layout;
        } else {
            selection.layout = compatible_layout(profiles_, selection);
        }
        if (!fits(profiles_, selection)) {
            throw std::runtime_error("layout " + selection.layout + " cannot type " +
                                     selection.language);
        }
        app_->apply(selection);
    }
}
std::string Simulator::locale() const {
    const auto found = profiles_.languages.find(app_->selection().language);
    return found == profiles_.languages.end() ? std::string{} : found->second.locale;
}
bool Simulator::on_keyboard(double x, double y) const {
    const double py = y - popup_margin;
    if (panel_case_bounds(app_->geometry()).contains(x, py)) {
        return true;
    }
    const auto popup = app_->input_view().popup;
    return popup && popup->contains(x, py);
}
void Simulator::leave(unsigned pointer) {
    // A repeating key, such as Backspace, stays held until its button lets go.
    if (app_->holds_repeat(pointer)) {
        return;
    }
    // Off the keys with the button up, swipe suggestions and the next word's
    // space stay until the next press elsewhere (press_outside).
    if (!app_->pointer_pressed(pointer) && app_->holds_swipe_state()) {
        app_->move(pointer, -1e4, -1e4);
        return;
    }
    app_->cancel_pointer(pointer);
}
void Simulator::press_outside(unsigned pointer, double x, double y) {
    // A press off the keyboard may click anywhere in the target, as a trigger
    // pulled off the panel can: ↶'s reopen and swipe replacements end here.
    app_->cancel_pointer(pointer);
    if (slow_button().contains(x, y)) {
        sink_.set_slow(!sink_.slow());
        redraw_ = true;
    } else if (clear_button().contains(x, y)) {
        // A new start: the keyboard's reading and its output go too.
        app_->cancel();
        field_.clear();
    } else if (field_bounds.contains(x, y)) {
        auto* cr = cairo_create(canvas_.get());
        auto layout = field_layout(cr, field_, locale());
        const auto inner = field_inner();
        int index = 0, trailing = 0;
        pango_layout_xy_to_index(layout.get(), static_cast<int>((x - inner.x) * PANGO_SCALE),
                                 static_cast<int>((y - inner.y + field_scroll_) * PANGO_SCALE), &index,
                                 &trailing);
        const char* text = field_.text().c_str();
        const char* at = g_utf8_offset_to_pointer(text + index, trailing);
        cairo_destroy(cr);
        field_.place_caret(static_cast<std::size_t>(at - text));
    }
}
void Simulator::pointer_down(unsigned pointer, double x, double y, double now) {
    sink_.set_time(now);
    auto& state = pointers_[pointer];
    if (!on_keyboard(x, y)) {
        state.inside = false;
        press_outside(pointer, x, y);
        return;
    }
    state.inside = true;
    const double py = y - popup_margin;
    app_->move(pointer, x, py);
    if (on_close(x, py)) {
        state.on_close = true;
        return;
    }
    app_->down(pointer, x, py, now);
}
bool Simulator::on_close(double x, double y) const {
    const auto controls = app_->view().controls;
    return std::any_of(controls.begin(), controls.end(), [&](const Control& control) {
        return control.action.kind == ControlKind::Close && control.bounds.contains(x, y);
    });
}
void Simulator::pointer_move(unsigned pointer, double x, double y) {
    auto& state = pointers_[pointer];
    const bool in_window = x >= 0 && y >= 0 && x < width && y < height;
    if (on_keyboard(x, y)) {
        state.inside = true;
        app_->move(pointer, x, y - popup_margin);
    } else if (!in_window && state.inside && !app_->pointer_pressed(pointer)) {
        // Outside the window with the button up, as a pointer-driving tool
        // parks the mouse after a click: the mouse left, as pointer_exit().
        pointer_exit(pointer);
    } else if (std::exchange(state.inside, false)) {
        leave(pointer);
    }
}
void Simulator::pointer_exit(unsigned pointer) {
    if (pointers_[pointer].inside && !app_->pointer_pressed(pointer)) {
        app_->move(pointer, -1e4, -1e4); // Still inside, for the next move.
    } else {
        pointer_move(pointer, -1, -1);
    }
}
void Simulator::pointer_up(unsigned pointer, double x, double y) {
    auto& state = pointers_[pointer];
    if (std::exchange(state.on_close, false)) {
        return;
    }
    if (state.inside) {
        app_->up(pointer, x, y - popup_margin);
    } else if (app_->holds_repeat(pointer)) {
        app_->cancel_pointer(pointer); // A key kept held off the keyboard ends with its button.
    }
}
void Simulator::pointer_cancel(unsigned pointer) {
    app_->cancel_pointer(pointer);
    pointers_.erase(pointer);
    if (pointer == held && hold_) {
        hold_.reset();
        redraw_ = true;
    }
}
void Simulator::toggle_hold(double x, double y, double now) {
    if (hold_) {
        const auto [hx, hy] = *hold_;
        hold_.reset();
        pointer_up(held, hx, hy);
        redraw_ = true;
    } else if (on_keyboard(x, y)) {
        pointer_move(held, x, y);
        pointer_down(held, x, y, now);
        hold_ = {x, y};
        redraw_ = true;
    }
}
void Simulator::scroll(double x, double y, double dx, double dy) {
    if (on_keyboard(x, y)) {
        app_->move(mouse, x, y - popup_margin);
        app_->scroll(mouse, dx, dy);
    }
}
bool Simulator::tick(double now) {
    sink_.set_time(now);
    bool changed = std::exchange(redraw_, false);
    if (app_->tick(now)) {
        changed |= app_->paint(now);
    }
    changed |= field_.revision() != drawn_revision_;
    if (changed) {
        compose();
        drawn_revision_ = field_.revision();
    }
    return changed;
}
void Simulator::compose() {
    auto* cr = cairo_create(canvas_.get());
    set(cr, background);
    cairo_paint(cr);

    // The field, scrolled to keep its caret in view.
    rounded(cr, field_bounds, 10);
    set(cr, field_fill);
    cairo_fill_preserve(cr);
    set(cr, field_edge);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    const auto inner = field_inner();
    const auto language = locale();
    auto layout = field_layout(cr, field_, language);
    if (const auto range = field_.selection()) {
        auto* attributes = pango_attr_list_new();
        auto* highlight = pango_attr_background_new(static_cast<guint16>(selected.r * 65535),
                                                    static_cast<guint16>(selected.g * 65535),
                                                    static_cast<guint16>(selected.b * 65535));
        highlight->start_index = static_cast<guint>(range->first);
        highlight->end_index = static_cast<guint>(range->second);
        pango_attr_list_insert(attributes, highlight);
        pango_layout_set_attributes(layout.get(), attributes);
        pango_attr_list_unref(attributes);
    }
    PangoRectangle caret{};
    pango_layout_get_cursor_pos(layout.get(), static_cast<int>(field_.caret()), &caret, nullptr);
    int text_width = 0, text_height = 0;
    pango_layout_get_pixel_size(layout.get(), &text_width, &text_height);
    const double caret_top = caret.y / double(PANGO_SCALE);
    const double caret_height = std::max(caret.height / double(PANGO_SCALE), field_font * 1.2);
    field_scroll_ = std::clamp(field_scroll_, caret_top + caret_height - inner.height, caret_top);
    field_scroll_ = std::clamp(field_scroll_, 0.0, std::max(0.0, text_height - inner.height));
    cairo_save(cr);
    cairo_rectangle(cr, inner.x - 4, inner.y, inner.width + 8, inner.height);
    cairo_clip(cr);
    if (field_.text().empty()) {
        auto hint = text_layout(cr, placeholder, field_font * .8, {});
        set(cr, faint);
        cairo_move_to(cr, inner.x + 4, inner.y + 3);
        pango_cairo_show_layout(cr, hint.get());
    } else {
        set(cr, ink);
        cairo_move_to(cr, inner.x, inner.y - field_scroll_);
        pango_cairo_show_layout(cr, layout.get());
    }
    set(cr, accent);
    cairo_rectangle(cr, inner.x + caret.x / double(PANGO_SCALE) - 1, inner.y + caret_top - field_scroll_,
                    2.5, caret_height);
    cairo_fill(cr);
    cairo_restore(cr);

    // The simulator's controls, with the pointer hints beside them.
    auto hint = text_layout(cr, hints, bar_font, {});
    int hint_width = 0, hint_height = 0;
    pango_layout_get_pixel_size(hint.get(), &hint_width, &hint_height);
    set(cr, faint);
    cairo_move_to(cr, 24, bar_y + (bar_height - hint_height) / 2);
    pango_cairo_show_layout(cr, hint.get());
    button(cr, slow_button(), sink_.slow() ? "Slow field: on" : "Slow field: off", sink_.slow());
    button(cr, clear_button(), "Clear field", false);

    // The keyboard over it all: only its case and open dropdowns are opaque.
    cairo_set_source_surface(cr, app_->renderer.surface(), 0, 0);
    cairo_paint(cr);
    if (hold_) {
        // Where the second pointer holds, as VR shows a laser's dot. A new
        // path, or the arc would join the last label's current point.
        cairo_new_path(cr);
        cairo_arc(cr, hold_->first, hold_->second, 9, 0, 2 * G_PI);
        set(cr, accent, .9);
        cairo_fill_preserve(cr);
        set(cr, background);
        cairo_set_line_width(cr, 2.5);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(canvas_.get());
}
void Simulator::shutdown() {
    app_->cancel();
    app_->flush_settings();
}
} // namespace framekeyboard::simulator
