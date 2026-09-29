#pragma once

#include "input.hpp"
#include <cairo.h>

namespace framekeyboard {
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
struct Control {
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
};

class PanelRenderer {
  public:
    PanelRenderer();
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;
    void paint(const PanelView& view, double now);
    const Key* hit_key(const PanelView& view, double x, double y) const;
    bool animating() const { return animating_; }
    cairo_surface_t* surface() const { return surface_; }
    std::vector<unsigned char> rgba() const;
    void write_png(const fs::path& path) const;

  private:
    struct Animation {
        double amount{}, target{}, from{}, started{};
    };
    std::map<std::string, Animation> animations_;
    cairo_surface_t* surface_{};
    bool animating_{};
};
} // namespace framekeyboard
