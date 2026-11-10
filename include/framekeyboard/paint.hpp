#pragma once
// The panel's own drawing of cards, buttons, wells and text, for windows
// outside the keyboard (the installer) that must look like its Settings.
// Every colour comes from the theme, as in panel.cpp.
#include "config.hpp"
#include "layout.hpp"
#include <cairo.h>
#include <pango/pango.h>
#include <string>

namespace framekeyboard::paint {
void rounded(cairo_t* cr, Rect r, double radius);
void source(cairo_t* cr, Color color);
Color mix(Color a, Color b, double amount);
// A Settings card: the raised plate the rows sit on.
void settings_card(cairo_t* cr, const Theme& theme, const Rect& r);
// A button or dropdown face: the keys' gradient, hover and latched fill.
void cap(cairo_t* cr, const Theme& theme, const Rect& r, bool hovered, bool selected, bool enabled);
// A well recessed into the case; returns its colour.
Color well(cairo_t* cr, const Theme& theme, const Rect& r);
// The latched fill, as on a selected segment or a switched-on track.
void latched(cairo_t* cr, const Theme& theme, const Rect& r, double radius);
// Interface text that shrinks, or wraps to two lines, to fit `r`; with
// `wrap` it wraps freely and shrinks only when the lines overflow.
void label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
           Color color, PangoAlignment alignment = PANGO_ALIGN_CENTER, bool wrap = false);
} // namespace framekeyboard::paint
