#pragma once
// The installer's screens on one canvas, without the window (main.cpp shows
// it and turns window events into these calls):
//
// Home: the keyboard, its name and Install (Reinstall when one is installed)
// with a version list, and Uninstall when installed.
// Setup: the keyboard slides up and fades; progress shows above a card with
// the first-install wizard and then Finish. Each wizard page plays a picture
// of its feature beside its question, and pages turn like a carousel.
// Reinstalls go straight to Finish.
#include "backend.hpp"
#include "framekeyboard/config.hpp"
#include "framekeyboard/layout.hpp"
#include "framekeyboard/localization.hpp"
#include <cairo.h>
#include <memory>
#include <pango/pango.h>

namespace framekeyboard::installer {

class Installer {
  public:
    static constexpr int width = 1280, height = 800;
    // Wizard pages; the page after the last is Finish.
    static constexpr int pages = 5;
    enum class Action {
        None,
        Install,
        Versions, // opens the version list
        Version,  // one row of it, by index
        Uninstall,
        KeepSettings, // the confirmation's Yes/No, by index (0 = keep)
        CancelUninstall,
        ConfirmUninstall,
        Choice, // a wizard page's Yes/No, by index (0 = Yes)
        Back,
        Next,
        Finish,
        Retry, // a failed list or install
    };

    Installer(Backend& backend, Theme theme, const Localization& localization);
    // The canvas takes the window's shape, at least width x height: the
    // screens stay width x height in its middle and the backdrop fills the
    // rest, so the window shows no bars. Pointer calls and control() use
    // canvas coordinates.
    void resize(int canvas_width, int canvas_height);
    int canvas_width() const { return canvas_width_; }
    int canvas_height() const { return canvas_height_; }
    void pointer_move(double x, double y);
    void pointer_down(double x, double y);
    void pointer_up(double x, double y);
    void pointer_leave();
    // Polls the backend and repaints when the picture has to change: every
    // call through a transition and while a wizard page's picture plays,
    // thirty times a second for a spinner, and not at all while nothing
    // moves. True when the canvas changed.
    bool tick(double now);
    cairo_surface_t* canvas() const { return canvas_.get(); }
    bool quitting() const { return quit_; }
    // While something runs, closing the window must wait.
    bool busy() const;
    // Where an action's control was last painted, for the tour.
    std::optional<Rect> control(Action action, int index = 0) const;
    // Whether that control is shown and takes presses.
    bool enabled(Action action, int index = 0) const;

  private:
    struct Hit {
        Rect bounds;
        Action action{};
        int index{};
        bool enabled{true};
    };
    enum class Screen { Home, Setup };

    void activate(const Hit& hit);
    void begin_install();
    void turn_page(int page);
    const Hit* hit_at(double x, double y) const;
    bool hovered(Action action, int index = 0) const;
    void poll();
    // The longest the picture may stay as it is, in seconds.
    double frame_interval() const;
    void paint(double now);
    void paint_backdrop();
    void paint_home();
    void paint_setup();
    void paint_progress();
    // One page of the card, `shift` pixels to the side while pages slide.
    void paint_page(int page, double shift);
    void paint_wizard_page(int page);
    void paint_finish_page();
    void paint_footer(double turned);
    void paint_versions();
    void paint_confirm();
    void paint_keyboard(double slide);
    // Each wizard page's picture, drawn in the stage to the left of its text.
    void paint_replace_demo(const Rect& stage, double t);
    void paint_swipe_demo(const Rect& stage, double t);
    void paint_suggestions_demo(const Rect& stage, double t);
    void paint_pull_demo(const Rect& stage, double t);
    void paint_numpad_demo(const Rect& stage, double t);
    // The keyboard's picture, scaled beforehand, with its top left at
    // (x, y); the case narrows over the numpad, which fades, as `numpad`
    // goes from 1 to 0.
    void small_picture(cairo_surface_t* picture, double scale, double x, double y, double numpad);
    // What a page's picture plays on: a piece of Home's backdrop, set into the card.
    void stage_plate(const Rect& r);
    // A piece of the keyboard's case for a picture's keys to sit on.
    void case_plate(const Rect& r);
    // A laser from beyond the stage's lower right corner to its dot at (x, y).
    void laser(const Rect& stage, double x, double y, double alpha = 1);
    void paint_finish_badge(double x, double y, double radius);
    // A button: a keycap face, or the latched fill for the main action.
    void button(const Rect& r, Action action, int index, const std::string& text, bool primary,
                bool enabled = true, double size = 22);
    // Install and the version list's chevron as one latched control.
    void split_button(const Rect& main, const Rect& more, bool ready);
    // Yes and No side by side in a well; `chosen` is 0, 1 or -1 for neither.
    void segments(const Rect& r, Action action, int chosen);
    void spinner(double x, double y, double radius);
    // A small keycap with its side below, as on the keyboard, for the pictures.
    void key(const Rect& r, bool lit, double radius = 5);
    // A text field in a well, with `value` and a blinking caret.
    void field(const Rect& r, const std::string& value, double size, double alpha, bool caret);
    void text(const std::string& value, const Rect& r, double size, Color color,
              PangoAlignment alignment = PANGO_ALIGN_CENTER, bool wrap = false);
    // Wrapped text from the top of `r` that shrinks until it fits; returns
    // the height it takes. Without `draw` it only measures. `spacing` is the
    // lines' pitch in line heights: 0 sets headings tight and text open.
    double block(const std::string& value, const Rect& r, double size, Color color,
                 PangoWeight weight = PANGO_WEIGHT_NORMAL, bool draw = true,
                 PangoAlignment alignment = PANGO_ALIGN_LEFT, double spacing = 0);
    // Whether the laser holds that control down: it sits lower, as a key does.
    bool held(Action action, int index = 0) const;
    double text_width(const std::string& value, double size) const;
    std::string tr(std::string_view key, const UiArgs& args = {}) const;
    Color dim() const;
    bool choice(int page) const;
    // The same as 1 or 0, easing for a moment after the choice changes.
    double choice_shown(int page) const;
    // How far the install has come, 0..1, for the bar and the Finish badge.
    double progress() const;
    bool first_install() const { return first_install_; }

    Backend& backend_;
    Theme theme_;
    const Localization& localization_;
    std::string font_;
    using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
    Surface canvas_;
    int canvas_width_{width}, canvas_height_{height};
    double offset_x_{}, offset_y_{}; // where the screens start on the canvas
    // The keyboard's picture at Home's size and at the wizard pictures' two,
    // scaled once so no frame scales it again.
    Surface hero_, mini_, half_;
    // The backdrop, painted once: every frame starts from it.
    Surface backdrop_;
    // Drawn once and painted from then on: the keyboard on Home with its
    // shadow and reflection, Setup's card and a page's stage. Their soft
    // shadows and glows cost too much to draw every frame.
    Surface grounded_, card_, plate_;
    // Paints what `draw` draws within `bounds`, drawing it only the first time.
    template <typename Draw> void cached(Surface& cache, const Rect& bounds, Draw&& draw);
    cairo_t* cr_{};
    double now_{}; // the time being painted, for spinners
    // pointer_move in screen coordinates, for the public calls that convert.
    void move(double x, double y);
    // Set by anything that changes the picture outside the clock: input and
    // the backend's news. `moving_` is whether the last frame was mid-motion,
    // so one more follows when the motion ends.
    bool dirty_{true}, moving_{};
    double painted_at_{};
    std::vector<Hit> hits_;
    // Off while painting the page that slides away: it takes no presses.
    bool live_{true};
    double pointer_x_{-1}, pointer_y_{-1};
    std::optional<Hit> pressed_;

    Status status_;
    Task seen_task_{Task::None};
    State seen_state_{State::Idle};
    std::vector<Release> releases_;
    std::size_t selected_{};
    std::string installed_;
    UiText home_message_;
    bool versions_open_{}, confirm_open_{}, keep_settings_{true};
    double confirm_opened_{};

    Screen screen_{Screen::Home};
    double setup_started_{};
    bool first_install_{};
    std::string installing_;
    int page_{};
    // The page slid away from, and when; and when a choice last changed,
    // for its picture's change.
    int page_from_{};
    double page_turned_{}, choice_changed_{-10};
    // When the page slid away from was turned to: its picture plays on as it leaves.
    double page_from_turned_{};
    // The bar eases toward progress(); `done_at_` starts the badge's tick.
    double shown_progress_{}, last_tick_{}, done_at_{};
    Choices choices_;
    bool install_done_{}, finish_pressed_{};
    bool quit_{};
};

} // namespace framekeyboard::installer
