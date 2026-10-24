#pragma once

#include "input.hpp"
#include <array>
#include <cairo.h>
#include <optional>

namespace framekeyboard {
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
// Transparent space above/below the case lets dropdowns escape its border.
// Logical UI and placement coordinates still describe the 1600 x 600 case.
constexpr int popup_margin = 200;
constexpr int texture_height = panel_height + 2 * popup_margin;
enum class ControlStyle { Button, Dropdown, Checkbox, MenuItem, Scrollbar, MoveHandle };
enum class ControlKind {
    None,
    Settings,
    Apply,
    Reload,
    Updates,
    NativeFallback,
    Quit,
    Recenter,
    Smaller,
    Larger,
    Pin,
    Passthrough,
    LanguageCycle,
    Close,
    MoveHandle,
    ToggleDropdown,
    ChooseLayout,
    ChooseLanguage,
    ChooseTheme,
    Favorite,
    HideNumpad,
    UseDragToMove,
    AutoLeveling,
    ReplaceNativeKeyboard,
    StartWithSteamos,
    ScrollMenu,
    // Contiguous composition groups support policy checks without inspecting IDs.
    ImeToggle,
    ImeConvert,
    ImeCommit,
    ImeCancel,
    ImeHiragana,
    ImeKatakana,
    ImeNext,
    ImePrev,
    ImeSegmentNext,
    ImeSegmentPrev,
    ImeCandidate,
    CjkToggle,
    CjkCommit,
    CjkCancel,
    CjkNext,
    CjkPrev,
    CjkCandidate,
    SwipeTyping,
    SwipeCandidate
};
struct ControlAction {
    ControlKind kind{ControlKind::None};
    int index{};
    std::string value{};
    bool operator==(const ControlAction&) const = default;
};
struct Control {
    bool operator==(const Control&) const = default;
    std::string id, label;
    Rect bounds;
    bool selected{};
    Icon icon{Icon::None};
    ControlStyle style{ControlStyle::Button};
    std::optional<Rect> clip{};
    bool enabled{true};
    ControlAction action{};
    bool hit(double x, double y) const {
        return enabled && bounds.contains(x, y) && (!clip || clip->contains(x, y));
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
    bool enabled{true};
    double size{22};
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
    // The flick key being held: what a tap and each flick would type, and the
    // direction release would choose. Painted as a cross above the keys.
    struct FlickPreview {
        bool operator==(const FlickPreview&) const = default;
        std::string key;
        std::array<std::string, 5> text; // Tap, left, up, right, down.
        int selected{};
    };
    std::optional<FlickPreview> flick_preview;
    std::set<std::string> hovered_keys, hovered_controls;
    // Case coordinates of the swipe in progress, drawn above the keys.
    std::vector<std::pair<double, double>> swipe_trail;
};

bool hidden_key(const PanelView& view, const Key& key);
// Logical case bounds also define the VR interaction area. Hidden numpad space
// stays transparent in the fixed-size texture, preserving the dashboard anchor.
Rect panel_case_bounds(const PanelView& view);
// This is the complete hit region, including transparent padding around the line.
std::optional<Rect> panel_move_handle_bounds(const PanelView& view);
// Panel area of the flick preview, which may extend past the case.
std::optional<Rect> flick_preview_bounds(const PanelView& view);

class PanelRenderer {
  public:
    PanelRenderer();
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;
    // force_full is used by pixel-equivalence tests and rendering benchmarks.
    void paint(const PanelView& view, double now, bool force_full = false);
    const Key* hit_key(const PanelView& view, double x, double y) const;
    // Case coordinates to the layout's own key coordinates, as hit_key uses.
    std::pair<double, double> layout_point(const PanelView& view, double x, double y) const;
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
    Animation move_handle_;
    cairo_surface_t* surface_{};
    // The swipe trail as an opaque layer, composited at 60% by every pass.
    // Cairo strokes slightly differently under a clip, and re-stroking a
    // growing path shifts its older antialiasing, so new segments are added
    // to the layer without touching pixels already drawn.
    cairo_surface_t* trail_{};
    std::vector<std::pair<double, double>> trail_points_; // Already in the layer.
    std::optional<Rect> trail_extent_;
    bool animating_{};
};
} // namespace framekeyboard
