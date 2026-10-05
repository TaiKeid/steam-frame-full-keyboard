#pragma once

#include "input.hpp"
#include <cairo.h>
#include <optional>

namespace framekeyboard {
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
struct Control {
    bool operator==(const Control&) const = default;
    std::string id, label;
    Rect bounds;
    bool selected{};
    Icon icon{Icon::None};
};
struct PanelView {
    const Layout* layout{};
    const Theme* theme{};
    const Language* language{};
    const LanguageMap* keymap{};
    const KeyboardState* keyboard{};
    std::vector<Control> controls;
    std::string status;
    bool settings{};
    bool composing{};
    std::string preedit;
    std::map<std::string, std::string> key_labels;
    std::set<std::string> hovered;
    // Keys drawn in the latched color because an app action is running.
    std::set<std::string> active_keys;
    // Visible panel width in pixels. The image stays panel_width wide; VR shows
    // only the left part, so a compact layout makes the overlay narrower.
    double width{panel_width};
};
// Width that shows `visible` at the same key scale as `full`, plus margins.
double compact_panel_width(const Layout& full, const Layout& visible, const Theme& theme);

class PanelRenderer {
  public:
    PanelRenderer();
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;
    // force_full is used by pixel-equivalence tests and rendering benchmarks.
    void paint(const PanelView& view, double now, bool force_full = false);
    const Key* hit_key(const PanelView& view, double x, double y) const;
    bool animating() const { return animating_; }
    cairo_surface_t* surface() const { return surface_; }
    std::vector<unsigned char> rgba() const;
    void write_png(const fs::path& path) const;

  private:
    struct Animation {
        double amount{}, target{}, from{}, started{};
    };
    struct Snapshot {
        Layout layout;
        Theme theme;
        Language language;
        // Own all compared values; never dereference pointers from an old view.
        PanelView view;
        std::set<int> modifiers;
        bool caps{}, num{};
    };
    std::optional<Snapshot> previous_;
    std::map<std::string, Rect> key_bounds_;
    std::map<std::string, Animation> animations_;
    cairo_surface_t* surface_{};
    bool animating_{};
};
} // namespace framekeyboard
