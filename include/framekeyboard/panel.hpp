#pragma once

#include "candidates.hpp"
#include "emoji.hpp"
#include "input.hpp"
#include <array>
#include <cairo.h>
#include <map>
#include <optional>

namespace framekeyboard {
class EmojiGlyphs;
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
// Transparent space above/below the case lets dropdowns escape its border.
// Logical UI and placement coordinates still describe the 1600 x 600 case.
constexpr int popup_margin = 200;
constexpr int texture_height = panel_height + 2 * popup_margin;
// With a transparent background, text drawn on the case (status line, reading
// line, dictation counter) sits on a plate of the case colour this opaque.
constexpr double case_plate_alpha = .88;

// TextField shows a value that opens its own editor, such as a server address.
// Note: a Settings row without a control, its text across the card.
// Segment: one choice of a row that shows all its choices side by side.
enum class ControlStyle {
    Button,
    Dropdown,
    Checkbox,
    MenuItem,
    Scrollbar,
    MoveHandle,
    TextField,
    Note,
    Segment
};
enum class ControlKind {
    None,
    SinkOff, // ends the shell's text sink (docs/integration.md, Privacy)
    Settings,
    Apply,
    Reload,
    Updates,
    NativeFallback,
    Quit,
    Recenter,
    Pin,
    Passthrough,
    KeepOpen,
    LanguageCycle,
    Close,
    MoveHandle,
    ToggleDropdown,
    ChooseLayout,
    ChooseLanguage,
    ChooseTheme,
    HideNumpad,
    ChooseBackgroundSurface,
    ChooseHapticSounds,
    ChooseHapticEffect,
    ChooseHapticVolume,
    UseDragToMove,
    AutoLeveling,
    ReplaceNativeKeyboard,
    StartWithSteamos,
    EmojiCloseOnSelect,
    FocusTooltip,
    ChooseDesktopUnicode,
    DictationDiscard,
    DictationKeep,
    ChooseSendGap,
    DictationDirect,
    // On device: the model row's button (Download, Cancel or Delete), the
    // server's fallback switch and the development model format.
    ModelAction,
    DictationFallback,
    ChooseDeviceModel,
    ChooseDictation,
    DictationServer,
    DictationTest,
    // The server address popup: a key (its value is the character, or
    // backspace, left, right, enter or esc), a recent server, or close.
    AddressKey,
    AddressRecent,
    SwipeToggle,     // the toolbar's own switch for swiping
    SoundsToggle,    // the toolbar's own switch for haptic sounds
    KeyFlicksToggle, // the toolbar's own switch for key flicks
    OpenLanguages,   // the Additional languages popup
    CloseLanguages,
    LanguagesLayout, // value: the layout whose languages show
    AddLanguage,     // value: the language, on the layout showing
    ResetMainLanguage,
    ClearLanguages, // asks first
    ConfirmAccept,
    ConfirmCancel,
    ScrollMenu,
    // あ / A, on a flick layout without 「あA1」.
    ImeToggle,
    // The candidate row's ∨, which opens the candidate list over the keys (∧ closes it).
    CandidateList,
    SwipeTyping,
    KeyFlicks, // Settings: flick a key down for its Shift character
    SwipeCandidate,
    FlickOnly,     // Settings: Japanese flick taps never cycle
    FlickKatakana, // Settings: show the flick script key
    ClearLearning  // asks first
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
    bool warning{}; // a toolbar icon drawn in the theme's alert colour
    bool badge{};   // a notification dot on the button's top right corner
    bool dimmed{};  // a toolbar icon drawn quietly, though it still responds
    int segments{}; // a Segment: how many share its track; action.index is its place
    bool busy{};    // a ring circles its border while its work is under way
    bool hit(double x, double y) const {
        return enabled && bounds.contains(x, y) && (!clip || clip->contains(x, y));
    }
};
struct CardView {
    bool operator==(const CardView&) const = default;
    std::string title;
    Rect bounds;
};
struct FieldLabel {
    bool operator==(const FieldLabel&) const = default;
    std::string text;
    Rect bounds, clip;
    bool left{};
    bool enabled{true};
    double size{22};
    bool wrap{}, warning{}; // a Note row: wrapped, in the warning colour
};
// The dictation editor as the panel draws it.
struct DictationView {
    bool operator==(const DictationView&) const = default;
    Rect bounds;             // Panel coordinates of the editor's frame.
    std::string text;        // Never logged.
    std::size_t cursor{};    // Byte offset into `text`.
    double scroll{};         // Content pixels scrolled out above the frame.
    std::string counter;     // For example "412/900".
    Icon where{Icon::None};  // Where speech is recognized, shown left of the counter.
    bool at_limit{};         // Dictation cannot continue; the counter turns amber.
    bool sending{};          // Locked: dimmed text and no caret.
    std::size_t uncertain{}; // Leading bytes that may already have reached the target.
    bool capturing{};        // Read-only: dimmed text, caret kept.
    bool revealing{};        // A phrase is still appearing word by word; read-only.
    std::string notice;      // A band over the editor's foot: waiting, an error or a question.
    bool notice_error{};
    // A composer's reading, already in `text` at the cursor: underlined, its
    // converting segment more strongly. Byte ranges into `text`.
    std::size_t preedit_begin{}, preedit_end{}, active_begin{}, active_end{};
    std::size_t selection_begin{}, selection_end{}; // Selected bytes of `text`.
    // The composer's candidates fill the band over the editor's foot; the
    // text area then ends above it. Convert sits beside the editor instead.
    bool candidates{};
};
// The server address popup over Settings, in panel coordinates.
constexpr Rect address_popup_bounds{300, 36, 1000, 508};
constexpr Rect confirm_popup_bounds{530, 170, 540, 240};
// The Additional languages popup covers Settings. One well holds three
// columns, as the emoji picker's sections share one.
constexpr Rect languages_popup_bounds{14, 8, 1572, 584};
constexpr Rect languages_well_bounds{30, 72, 1540, 504};
constexpr Rect languages_layouts_bounds{38, 116, 270, 452};
constexpr Rect languages_list_bounds{338, 116, 760, 452};
constexpr double languages_list_gap = 8;
constexpr Rect languages_main_bounds{1130, 116, 424, 56};
constexpr Rect additional_list_bounds{1130, 226, 424, 342};
constexpr double languages_row_height = 56, additional_row_height = 62;
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
    std::string version; // Shown in Settings; empty without an installed updater.
    bool settings{};
    bool composing{}; // The composing layout and its preedit are showing.
    bool hide_numpad{};
    // Background (Settings): how opaque the case is behind the keys, 1 when
    // filled; below 1 it is drawn see-through, and still takes the laser.
    double case_opacity{1};
    std::string preedit;
    // A composing language (Japanese, Chinese, Korean) shows its reading, a
    // hint while there is none, and its candidates scrolling sideways
    // (ime_band() says where). The converting segment is a byte range of
    // `preedit`.
    bool ime_band{};
    // A language among the user's composes on this layout while the shown
    // one doesn't: the keys keep the scale and place the reading line gives
    // them, a compact toolbar's second row where the line would be, so
    // switching between them moves neither keys nor toolbar.
    bool ime_room{};
    // The layout composes, or shares its layout with a language that does:
    // in every view of it but Settings (the emoji picker and dictation mode
    // too) the case lacks the second toolbar row the reading line replaces,
    // and ends that much sooner.
    bool reading_case{};
    // The candidate row ends with ∨, which opens the whole list over the
    // keys, as a phone's does.
    bool ime_more{};
    // The Japanese flick mode: English shows the QWERTY page, digits turn the
    // grid into a number pad; the 「あA1」 key marks which mode is on.
    bool flick_latin{}, flick_digits{};
    // Flick Katakana (JP) shows the script key; while katakana is on, the
    // kana keys show and type katakana. The key shows the script on every
    // page, and waits, disabled, on the digits page.
    bool flick_script_key{}, flick_katakana{};
    std::size_t preedit_active_begin{}, preedit_active_end{};
    // Where typing goes in `preedit`, before conversion: a byte offset.
    std::optional<std::size_t> preedit_cursor;
    std::optional<CandidateStripView> candidates;
    std::map<std::string, std::string> key_labels;
    // Key flicks: the key pulled toward its Shift character, that character,
    // and how far, 0 to 1; at 1 a release types it.
    struct KeyFlick {
        bool operator==(const KeyFlick&) const = default;
        std::string key, legend;
        double pull{};
    };
    std::optional<KeyFlick> key_flick;
    // Key flicks are on: Korean keys show their Shift jamo, as kana keys do.
    bool key_flick_hints{};
    // The flick key being held: what a tap and each flick would type, and the
    // direction release would choose. Painted as a cross above the keys.
    struct FlickPreview {
        bool operator==(const FlickPreview&) const = default;
        std::string key;
        std::array<std::string, 5> text; // Tap, left, up, right, down.
        int selected{};
    };
    std::optional<FlickPreview> flick_preview;
    // Keep open types into a window out of sight (its amber cue): a tooltip
    // above that button names it. Empty: none.
    std::string focus_tooltip;
    // Typing was refused for want of a target: from this time the Focus mode
    // button's icon shakes, like an error, instead of a status message.
    double focus_shake_since{-1};
    // The swipe button was pressed with no language that can swipe: from this
    // time its icon shakes, fading from the alert colour.
    double swipe_shake_since{-1};
    std::set<std::string> hovered_keys, hovered_controls;
    bool status_hovered{}; // A laser is on the dictation status line.
    // Case coordinates of the swipe in progress, drawn above the keys.
    std::vector<std::pair<double, double>> swipe_trail;
    // While a press on a letter would start a swipe: each letter's reach in
    // layout coordinates (SwipeKeyboard::reach), where a press between keys
    // can start one too (PanelRenderer::swipe_key).
    const std::map<std::string, Rect>* swipe_reach{};
    // Keys drawn dimmed, which ignore the laser.
    std::set<std::string> disabled_keys;
    // The emoji picker's grid and tabs; its keys are in `layout`.
    std::optional<EmojiPickerView> emoji;
    const EmojiGlyphs* emoji_glyphs{};
    // Dictation is usable, so the layout's mic key shows.
    bool dictation_available{};
    // The mic key shows level bars instead of its icon while capturing.
    bool dictation_capturing{};
    // Phrases are being recognized, in the editor or Type as I speak: a
    // segment circles the mic key's border, at 30 fps, until their text is back.
    bool dictation_recognizing{};
    // Where speech is recognized, a small icon in the mic key's corner on
    // the keyboard: the headset, the user's computer or a hosted provider.
    Icon dictation_where{Icon::None};
    // A speech model download, 0–1, or below 0 while it is prepared: the mic
    // key shows, disabled, with the progress along its foot.
    std::optional<double> dictation_download;
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
    // A question over Settings; its buttons are controls whose ids start
    // with "confirm-".
    struct ConfirmView {
        bool operator==(const ConfirmView&) const = default;
        Rect panel;
        std::string title, body;
    };
    std::optional<ConfirmView> confirm;
    // The Additional languages popup; rows are drawn from here and hit-tested
    // as controls whose ids start with "languages-row-".
    struct LanguagesView {
        bool operator==(const LanguagesView&) const = default;
        struct Row {
            bool operator==(const Row&) const = default;
            std::string id, title, note;
            Rect bounds;
            bool selected{}, enabled{true}, checked{}, removing{};
            // Whether swipe typing works here; empty while swiping is off.
            std::optional<bool> swipe;
        };
        Rect panel, well, layouts_list, languages_list, additional_list;
        std::vector<Row> layouts, languages, additional; // only those in view
        Row main;
        bool main_target{}; // A lifted row would land on it.
        std::string hint;
        std::optional<Row> lifted;
        std::optional<Rect> layouts_thumb, languages_thumb, additional_thumb;
    };
    std::optional<LanguagesView> languages;
    // The prefix of the open modal's controls; nothing else responds. A
    // question asked from the languages popup sits over it.
    std::string modal_prefix() const {
        return address ? "address-" : confirm ? "confirm-" : languages ? "languages-" : "";
    }
};

// Dictation editor text geometry, shared by painting and App's cursor moves.
// Content coordinates start at the text's top-left corner, before scrolling.
struct EditorCaret {
    double x{}, y{}, height{};
};
Rect editor_text_area(const PanelView& view); // Panel coordinates inside the frame.
EditorCaret editor_caret(const PanelView& view, std::size_t index);
std::size_t editor_index_at(const PanelView& view, double x, double y);
// The cursor one character to the left (direction -1) or right (+1) on screen:
// backwards in right-to-left text, and over a whole cluster with its marks.
std::size_t editor_move(const PanelView& view, std::size_t index, int direction);
// The end of the selection [first, last) whose caret shows at its left
// (direction -1) or right (+1) edge: byte order reverses in right-to-left text.
std::size_t editor_selection_edge(const PanelView& view, std::size_t first, std::size_t last,
                                  int direction);
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
// Where a compact toolbar's second row goes: on a composing layout's reading
// line, its switches at the line's left, in every view of the layout.
// `banded` is the layout's view with the band (or its room) shown.
double second_row_top(const PanelView& banded);
// This is the complete hit region, including transparent padding around the line.
std::optional<Rect> panel_move_handle_bounds(const PanelView& view);
// Where a composing language shows its reading and candidates. The flick
// layout's band gives each a row between the toolbar and the keys. Full
// layouts keep the toolbar's height: the candidates take the first toolbar
// row after its buttons, where swipe suggestions go, and the reading the line
// under it, where the status shows; `band` is that line. Full layouts need the
// view's controls, which place the toolbar's buttons.
struct ImeBand {
    Rect band, reading, candidates;
    std::optional<Rect> more; // With ime_more.
};
ImeBand ime_band(const PanelView& view);
// Where the open candidate list goes on. Full layouts: under the reading
// line, in the candidate row's columns, five lines at most. The flick band:
// under its candidate row, across the band, down to the foot of the keys.
Rect candidate_list_area(const PanelView& view);
// The layout height at which a view without the Japanese band would give
// its keys the scale `view` gives them with it (or with its room kept); the
// layout's own height for a view without either. The emoji picker opens at
// it, in the same case.
double unbanded_layout_height(const PanelView& view);
// A candidate's label width, as the band draws it.
double candidate_text_width(const std::string& text, const std::string& family);
// Panel area of the flick preview, which may extend past the case.
std::optional<Rect> flick_preview_bounds(const PanelView& view);
// Where dictation mode's status line sits in the toolbar row.
Rect dictation_status_bounds(const PanelView& view);
// Background Semi-transparent: the case's opacity, half on a dark theme;
// a light theme's pale case reads far brighter, so it keeps only 15%.
double semi_transparent_case_opacity(const Theme& theme);

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
    // Between keys, the letter whose swipe reach holds the point, the
    // nearest if two do; a press there can only start a swipe.
    const Key* swipe_key(const PanelView& view, double x, double y) const;
    // Case coordinates to the layout's own key coordinates, as hit_key uses.
    std::pair<double, double> layout_point(const PanelView& view, double x, double y) const;
    bool animating() const { return animating_; }
    // When an animation can next change a pixel: now for key presses, the
    // next 30 fps step for the mic key's bars and ring.
    double next_change() const { return next_change_; }
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
        bool shift_locked{}; // a locked Shift fills its arrow, with the same modifiers
    };
    std::optional<Snapshot> previous_;
    std::map<std::string, Rect> key_bounds_;
    std::map<std::string, Animation> animations_;
    Animation move_handle_;
    // The hovered dropdown choice, since when, and the whole-pixel offset its
    // name was last painted at, for scrolling a long name.
    std::string marquee_;
    double marquee_since_{}, marquee_shown_{-1};
    // A status too long for its gap scrolls under the laser. Once the laser
    // leaves, the pass under way ends here and the text returns to its start.
    std::string status_measured_, status_font_;
    double status_width_{};
    bool status_scrolling_{};
    std::optional<double> status_scroll_end_;
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
    double ring_time_{};
    double busy_ring_{};                  // A busy button's ring, stepping like the mic's.
    double swipe_shake_{}, swipe_fade_{}; // The swipe icon's, and its alert colour left.
    double focus_shake_{}; // The Focus mode icon's shake offset, when last drawn.  // The recognition
                           // ring's time, sampled at 30 fps, when last drawn.
    double next_change_{};
    bool animating_{};
};
} // namespace framekeyboard
