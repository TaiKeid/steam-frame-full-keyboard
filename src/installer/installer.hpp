#pragma once
// The installer's screens on one canvas, without the window (main.cpp shows
// it and turns window events into these calls):
//
// Home: the keyboard and Install (Reinstall when one is installed) with a
// version list, and Uninstall when installed.
// Setup: the keyboard slides up and fades; progress shows above a card with
// the first-install wizard and then Finish. Reinstalls go straight to Finish.
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
        Close,
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
    void pointer_move(double x, double y);
    void pointer_down(double x, double y);
    void pointer_up(double x, double y);
    void pointer_leave();
    // Polls the backend and repaints; true when the canvas changed.
    bool tick(double now);
    cairo_surface_t* canvas() const { return canvas_.get(); }
    bool quitting() const { return quit_; }
    // While something runs, closing the window must wait.
    bool busy() const;
    // Where an action's control was last painted, for the tour.
    std::optional<Rect> control(Action action, int index = 0) const;

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
    const Hit* hit_at(double x, double y) const;
    bool hovered(Action action, int index = 0) const;
    void poll();
    void paint(double now);
    void paint_home();
    void paint_setup(double now);
    void paint_progress();
    void paint_wizard_page();
    void paint_finish_page(double now);
    void paint_versions();
    void paint_confirm();
    void paint_keyboard(double slide);
    // A button: a keycap face, or the latched fill for the main action.
    void button(const Rect& r, Action action, int index, const std::string& text, bool primary,
                bool enabled = true, double size = 22);
    // Yes and No side by side in a well; `chosen` is 0, 1 or -1 for neither.
    void segments(const Rect& r, Action action, int chosen);
    void spinner(double x, double y, double radius, double now);
    void text(const std::string& value, const Rect& r, double size, Color color,
              PangoAlignment alignment = PANGO_ALIGN_CENTER, bool wrap = false);
    // Wrapped text from the top of `r`, for descriptions.
    void paragraph(const std::string& value, const Rect& r, double size, Color color);
    std::string tr(std::string_view key, const UiArgs& args = {}) const;
    Color dim() const;
    bool first_install() const { return first_install_; }

    Backend& backend_;
    Theme theme_;
    const Localization& localization_;
    std::string font_;
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> canvas_;
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> keyboard_;
    cairo_t* cr_{};
    double now_{}; // the time being painted, for spinners
    std::vector<Hit> hits_;
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

    Screen screen_{Screen::Home};
    double setup_started_{};
    bool first_install_{};
    std::string installing_;
    int page_{};
    Choices choices_;
    bool install_done_{}, finish_pressed_{};
    bool quit_{};
};

} // namespace framekeyboard::installer
