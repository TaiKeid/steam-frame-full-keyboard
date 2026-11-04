#pragma once

#include "field.hpp"
#include "framekeyboard/app.hpp"
#include <cairo.h>
#include <map>
#include <memory>
#include <optional>

namespace framekeyboard::simulator {
// What a run starts with. Without a layout, language or theme the saved
// selection stays; with them it is applied and saved, as Settings would.
struct Setup {
    fs::path config_dir, data_dir, models_dir;
    std::optional<std::string> layout, language, theme;
    bool slow{};
    // Tests replace Japanese conversion; empty loads Anthy, as the keyboard does.
    JapaneseComposer::Factory japanese_converter;
};

// The simulator's window contents, without the window: the keyboard's own
// App typing into a TextField, drawn on one canvas with the field above the
// keyboard and the simulator's controls below it.
//
// Canvas coordinates are the keyboard texture's (panel_width x
// texture_height): the keyboard's case starts at y = popup_margin, and the
// field fills the margin above it, where only an open dropdown draws. A
// pointer counts as on the keyboard over the case or an open dropdown, as
// the VR panel's laser mask does; leaving it is the laser leaving the panel.
//
// The window system adapter (main.cpp) only converts its events to these
// calls and shows the canvas, so another platform needs only a new adapter.
class Simulator {
  public:
    static constexpr int width = panel_width, height = texture_height;
    // Pointer IDs: the mouse, the held second pointer (toggle_hold), and
    // touch screen fingers from first_finger up.
    static constexpr unsigned mouse = 0, held = 1, first_finger = 2;
    // Above the keyboard's messages, which show over the case as on the
    // headset (a card as tall as the focus tooltip, 60 px over the case).
    static constexpr Rect field_bounds{16, 12, panel_width - 32, popup_margin - 80};
    // Throws when the setup's layout, language or theme cannot be used.
    explicit Simulator(const Setup& setup);
    void pointer_down(unsigned pointer, double x, double y, double now);
    void pointer_move(unsigned pointer, double x, double y);
    void pointer_up(unsigned pointer, double x, double y);
    // The pointer is gone without a release, as when the window loses focus.
    void pointer_cancel(unsigned pointer);
    // The mouse left the window with its button up. Nothing outside the
    // window reaches the simulator's field, so only the hover ends: ↶ can
    // still bring a phrase back once the mouse is on the keys again, and
    // coming back over the field ends that chance, as leaving the keys does.
    void pointer_exit(unsigned pointer);
    // Presses the second pointer at (x, y) and keeps it there until the next
    // call releases it: a second hand holding Shift or a key, for a mouse.
    void toggle_hold(double x, double y, double now);
    void scroll(double x, double y, double dx, double dy);
    // Advances the keyboard and the field; true when the canvas changed.
    bool tick(double now);
    // Redraw everything on the next tick, as after the window was exposed.
    void invalidate() { redraw_ = true; }
    cairo_surface_t* canvas() const { return canvas_.get(); }
    bool quitting() const { return app_->quitting(); }
    // Ends input and saves pending settings, before the process exits.
    void shutdown();
    App& app() { return *app_; }
    TextField& field() { return field_; }
    FieldSink& sink() { return sink_; }
    std::optional<std::pair<double, double>> hold() const { return hold_; }
    static Rect slow_button();
    static Rect clear_button();

  private:
    struct Pointer {
        bool inside{};   // Last seen on the keyboard.
        bool on_close{}; // Pressed on the ×, which does nothing here.
    };
    bool on_close(double x, double y) const;
    bool on_keyboard(double x, double y) const;
    // The laser left the panel (vr.cpp, VREvent_FocusLeave).
    void leave(unsigned pointer);
    void press_outside(unsigned pointer, double x, double y);
    void compose();
    std::string locale() const;
    Profiles profiles_;
    TextField field_;
    FieldSink sink_{field_};
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> canvas_;
    std::map<unsigned, Pointer> pointers_;
    std::optional<std::pair<double, double>> hold_;
    double field_scroll_{}; // Pixels of field text above its top edge.
    std::uint64_t drawn_revision_{~std::uint64_t{}};
    bool redraw_{true};
    // Last, so it is destroyed first: App's cleanup still sends to the sink.
    std::unique_ptr<App> app_;
};
} // namespace framekeyboard::simulator
