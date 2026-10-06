#pragma once

#include "input.hpp"
#include <cairo.h>
#include <optional>

namespace framekeyboard {
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
// Transparent space above/below the case lets dropdowns escape its border.
// Logical UI and placement coordinates still describe the 1600 x 600 case.
constexpr int popup_margin = 200;
constexpr int texture_height = panel_height + 2 * popup_margin;
enum class ControlStyle { Button, Dropdown, Checkbox, MenuItem, Scrollbar };
struct Control {
    bool operator==(const Control&) const = default;
    std::string id, label;
    Rect bounds;
    bool selected{};
    Icon icon{Icon::None};
    ControlStyle style{ControlStyle::Button};
    std::optional<Rect> clip{};
    bool hit(double x, double y) const {
        return bounds.contains(x, y) && (!clip || clip->contains(x, y));
    }
};
struct CardView {
    std::string title;
    Rect bounds;
};
struct FieldLabel {
    std::string text;
    Rect bounds, clip;
    bool left{};
};
struct PanelView {
    const Layout* layout{};
    const Theme* theme{};
    const Language* language{};
    const LanguageMap* keymap{};
    const KeyboardState* keyboard{};
    std::vector<Control> controls;
    std::vector<CardView> cards;
    std::vector<FieldLabel> field_labels;
    std::optional<Rect> popup;
    std::string status;
    bool settings{};
    bool composing{};
    bool hide_numpad{};
    std::string preedit;
    std::map<std::string, std::string> key_labels;
    std::set<std::string> hovered;
};

bool hidden_key(const PanelView& view, const Key& key);
// Logical case bounds also define the VR interaction area. Hidden numpad space
// stays transparent in the fixed-size texture, preserving the dashboard anchor.
Rect panel_case_bounds(const PanelView& view);

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
