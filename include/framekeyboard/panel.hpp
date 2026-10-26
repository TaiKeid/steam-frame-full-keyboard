#pragma once

#include "emoji.hpp"
#include "input.hpp"
#include <array>
#include <cairo.h>
#include <optional>

namespace framekeyboard {
class EmojiGlyphs;
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
// Transparent space above/below the case lets dropdowns escape its border.
// Logical UI and placement coordinates still describe the 1600 x 600 case.
constexpr int popup_margin = 200;
constexpr int texture_height = panel_height + 2 * popup_margin;
// TextField shows a value that opens its own editor, such as a server address.
enum class ControlStyle { Button, Dropdown, Checkbox, MenuItem, Scrollbar, MoveHandle, TextField };
enum class ControlKind {
    None,
    Settings,
    Apply,
    Reload,
    Updates,
    NativeFallback,
    Quit,
    Recenter,
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
    EmojiCloseOnSelect,
    DictationDiscard,
    DictationKeep,
    ChooseSendGap,
    ChooseDictation,
    DictationServer,
    DictationTest,
    // The server address popup: a key (its value is the character, or
    // backspace, left, right, enter or esc), a recent server, or close.
    AddressKey,
    AddressRecent,
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
// The dictation editor as the panel draws it.
struct DictationView {
    bool operator==(const DictationView&) const = default;
    Rect bounds;             // Panel coordinates of the editor's frame.
    std::string text;        // Never logged.
    std::size_t cursor{};    // Byte offset into `text`.
    double scroll{};         // Content pixels scrolled out above the frame.
    std::string counter;     // For example "412/900".
    bool at_limit{};         // Dictation cannot continue; the counter turns amber.
    bool sending{};          // Locked: dimmed text and no caret.
    std::size_t uncertain{}; // Leading bytes that may already have reached the target.
    bool capturing{};        // Read-only: dimmed text, caret kept.
    bool revealing{};        // A phrase is still appearing word by word; read-only.
    std::string notice;      // A band over the editor's foot: waiting, an error or a question.
    bool notice_error{};
};
// The server address popup over Settings, in panel coordinates.
constexpr Rect address_popup_bounds{300, 36, 1000, 508};
// The helper's level interval; the mic key's bars ease to new heights in it.
constexpr double dictation_level_period = .08;
// The notice band over the editor's foot, in panel units.
constexpr double dictation_notice_height = 46;
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
    // Keys drawn dimmed, which ignore the laser.
    std::set<std::string> disabled_keys;
    // The emoji picker's grid and tabs; its keys are in `layout`.
    std::optional<EmojiPickerView> emoji;
    const EmojiGlyphs* emoji_glyphs{};
    // Dictation is usable, so the layout's mic key shows.
    bool dictation_available{};
    // The mic key shows level bars instead of its icon while capturing.
    bool dictation_capturing{};
    // Six levels, 0–8, oldest first. Each of the five bars eases from one
    // level to the next one, so the newest enters on the right; the easing
    // started at `dictation_level_since` and takes dictation_level_period.
    std::vector<int> dictation_levels;
    double dictation_level_since{};
    // Dictation mode's editor; its keys are in `layout`.
    std::optional<DictationView> dictation;
    // The server address popup over Settings; its keys and chips are
    // controls whose ids start with "address-".
    struct AddressView {
        bool operator==(const AddressView&) const = default;
        Rect panel, field, hint;
        std::string text; // a server address, never anything typed elsewhere
        std::size_t cursor{};
        std::string message;
        enum class Tone { Quiet, Warning, Error } tone{Tone::Quiet};
    };
    std::optional<AddressView> address;
};

// Dictation editor text geometry, shared by painting and App's cursor moves.
// Content coordinates start at the text's top-left corner, before scrolling.
struct EditorCaret {
    double x{}, y{}, height{};
};
Rect editor_text_area(const PanelView& view); // Panel coordinates inside the frame.
EditorCaret editor_caret(const PanelView& view, std::size_t index);
std::size_t editor_index_at(const PanelView& view, double x, double y);
// The visual line holding `index`, as byte offsets [start, end) without its break.
std::pair<std::size_t, std::size_t> editor_line(const PanelView& view, std::size_t index);
double editor_content_height(const PanelView& view);

bool hidden_key(const PanelView& view, const Key& key);
// The layout's width without its hidden numpad.
double visible_layout_width(const PanelView& view);
// A rectangle in layout coordinates, in panel coordinates.
Rect panel_rect(const PanelView& view, Rect layout_rect);
// Logical case bounds also define the VR interaction area. Hidden numpad space
// stays transparent in the fixed-size texture, preserving the dashboard anchor.
Rect panel_case_bounds(const PanelView& view);
// A case narrower than the panel (numpad hidden, flick layout) cannot fit the
// main toolbar and swipe suggestions in one row; the zoom and passthrough
// buttons then form a second row under Settings.
bool compact_toolbar(const PanelView& view);
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
    // Whether any pixel was redrawn; nothing to upload otherwise.
    bool paint(const PanelView& view, double now, bool force_full = false);
    const Key* hit_key(const PanelView& view, double x, double y) const;
    // Case coordinates to the layout's own key coordinates, as hit_key uses.
    std::pair<double, double> layout_point(const PanelView& view, double x, double y) const;
    bool animating() const { return animating_; }
    cairo_surface_t* surface() const { return surface_; }
    // Surface pixels the last paint that returned true changed, as whole-pixel
    // rectangles inside the surface; empty means everything.
    const std::vector<Rect>& damage() const { return damage_; }
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
    std::vector<Rect> damage_;
    double mic_phase_{1}; // How far the bars had slid when last drawn.
    bool animating_{};
};
} // namespace framekeyboard
