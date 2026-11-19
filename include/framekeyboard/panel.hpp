#pragma once

#include "candidates.hpp"
#include "emoji.hpp"
#include "input.hpp"
#include "key_areas.hpp"
#include <array>
#include <cairo.h>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace framekeyboard {
class EmojiGlyphs;
constexpr int panel_width = 1600;
constexpr int panel_height = 600;
// Settings' case: as tall as a full-size layout's (reading_case_room()), its
// cards' scrollbar as far above its foot as the cards are from its sides.
constexpr int settings_height = 572;
// Transparent space above/below the case lets dropdowns escape its border.
// Logical UI and placement coordinates still describe the 1600 x 600 case.
constexpr int popup_margin = 200;
constexpr int texture_height = panel_height + 2 * popup_margin;
// With a transparent background, text drawn on the case (status line, reading
// line, dictation counter) sits on a plate of the case color this opaque.
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
    HoverFade,      // Settings: a key the laser leaves dims slowly
    CurvedKeyboard, // Settings: curve bars beside the keyboard bend it
    ChooseHapticSounds,
    ChooseHapticEffect,
    ChooseHapticVolume,
    UseDragToMove,
    AutoLeveling,
    Vibration,
    ReplaceNativeKeyboard,
    StartWithSteamos,
    StreamingSupport, // Settings: type into Steam Link streams (stream_support.hpp)
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
    OpenThemes,     // the theme picker
    CloseThemes,
    ConfirmAccept,
    ConfirmCancel,
    ScrollMenu,
    // あ / A, on a flick layout without 「あA1」.
    ImeToggle,
    // The candidate row's ∨, which opens the candidate list over the keys (∧ closes it).
    CandidateList,
    SwipeTyping,
    TypingSuggestions,
    TypingSuggestion,
    KeyFlicks, // Settings: flick a key down for its Shift character
    SwipeCandidate,
    FlickOnly,            // Settings: Japanese flick taps never cycle
    FlickKatakana,        // Settings: show the flick script key
    ClearLearning,        // asks first
    ClearLearnedRow,      // a language's switch in the Clear learned words popup
    ExtendedKeyCollision, // Settings: presses between keys reach the nearest one
    // The Debug card, only in debug builds (FRAMEKEYBOARD_DEBUG_MODE).
    DebugMarkAnthy,
    DebugKeyCollision,
    DebugInterfaceLanguage,
    // The shortcut bar (docs/shortcut-bar-design.md): the toolbar's switch
    // for the bar, Settings' Shortcuts and macros switch, and the Debug
    // card's sample bars.
    ShortcutsToggle,
    Shortcuts,
    DebugSampleShortcuts,
    // The shortcut editor's controls (shortcuts-…; value says which), its
    // popup's included, and the keyboard's strip while a label is renamed
    // in place: its buttons at the right, where they end before them.
    ShortcutEditor,
    FieldAction
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
    bool warning{}; // a toolbar icon drawn in the theme's alert color
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
    bool wrap{}, warning{}; // a Note row: wrapped, in the warning color
    // Room under the bounds for a second line, the first staying in place.
    double below{};
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
// Turning Streaming support on explains itself at more length.
constexpr Rect streaming_popup_bounds{390, 60, 820, 460};
// The Additional languages popup covers Settings. One well holds three
// columns, as the emoji picker's sections share one.
constexpr Rect languages_popup_bounds{14, 8, 1572, 556};
constexpr Rect languages_well_bounds{30, 72, 1540, 476};
constexpr Rect languages_layouts_bounds{38, 116, 270, 424};
constexpr Rect languages_list_bounds{338, 116, 760, 424};
constexpr double languages_list_gap = 8;
constexpr Rect languages_main_bounds{1130, 116, 424, 56};
constexpr Rect additional_list_bounds{1130, 226, 424, 314};
constexpr double languages_row_height = 56, additional_row_height = 62;
// The theme picker covers Settings as Additional languages does. Its well
// holds a grid of cards, four across: a preview of the keyboard in that
// theme over its name. Two rows show and the third peeks in; it scrolls.
constexpr Rect themes_popup_bounds = languages_popup_bounds;
// Each card's control is this followed by its theme's ID.
inline constexpr std::string_view theme_card_prefix = "themes-card-";
constexpr Rect themes_well_bounds = languages_well_bounds;
constexpr int theme_columns = 4;
constexpr double theme_card_pad = 16, theme_card_gap = 16;
constexpr double theme_card_width =
    (themes_well_bounds.width - 2 * theme_card_pad - (theme_columns - 1) * theme_card_gap) /
    theme_columns;
constexpr double theme_preview_inset = 8, theme_name_height = 44;
constexpr int theme_preview_width = static_cast<int>(theme_card_width - 2 * theme_preview_inset);
constexpr int theme_preview_height = 160;
constexpr double theme_card_height = theme_preview_inset + theme_preview_height + theme_name_height;
// The helper's level interval; the mic key's bars ease to new heights in it.
constexpr double dictation_level_period = .08;
// The notice band over the editor's foot, in panel units.
constexpr double dictation_notice_height = 46;
// How a label fitted its room, for finding interface text that doesn't fit
// (docs/localization-design.md): its size over the size asked for after
// shrinking, its lines, and whether it was cut short or ran out of room.
struct LabelFit {
    std::string text, family;
    double scale{1};
    int lines{1};
    bool cut{};
};
// While set, the renderer adds every label it draws; nullptr stops it.
// The theme picker's preview of `theme`: the US keyboard's lower left
// corner (Shift, Z, X, C over Ctrl held down, Super under the laser, Alt and
// Space) on the theme's case, drawn by the keys' own code into a new width x height image
// the caller owns. `layout` is en-us-full, `keymap` its language's.
cairo_surface_t* render_theme_preview(const Theme& theme, const Layout& layout, const Language& language,
                                      const LanguageMap& keymap, int width, int height);
void record_label_fits(std::vector<LabelFit>* into);
struct PanelView;
// The length of a bar button's border in pixels, which a long run's stroke measures (R372).
inline double bar_border_pixels(const Rect& bounds) {
    return 2 * (bounds.width + bounds.height);
}
// The shortcut bar above the case (docs/shortcut-bar-design.md), in panel
// coordinates, as App built it: the renderer only draws it, and nothing it
// draws acts. Its parts take the laser through shortcut_bar_bounds().
struct ShortcutBarView {
    // A button, drawn as a key in Ctrl's tone at the layout's scale.
    struct Button {
        bool operator==(const Button&) const = default;
        std::string label; // up to two lines; the user's own, never logged
        Rect bounds;       // where it is after scrolling; `strip` clips it
        bool hovered{}, pressed{}, enabled{true};
        bool running{}; // its run is under way: lit, as a latched key
        // A long run's progress (R372): how many pixels of the border's length (bar_border_pixels)
        // its stroke has reached, clockwise from the top center; -1 for none. Whole pixels, so the
        // bar repaints only when the stroke's end moves by one.
        int progress_px{-1};
        bool run{};     // a Run button: the >_ mark in its top right corner
    };
    bool operator==(const ShortcutBarView&) const = default;
    Rect plate, strip; // the bar's plate, and where its buttons show
    double scale{1};   // the layout's, which sizes the buttons as keys
    // Two empty slots: a bar without buttons, or the empty bar.
    bool slots{};
    std::vector<Button> buttons; // only those at least partly in the strip
    // Clipped sides fade out under a chevron.
    bool more_before{}, more_after{};
    // The picker: a tab on the plate's right side, part of its shape; the
    // whole tab is the button, lit while its menu is open.
    Rect picker;
    bool picker_hovered{}, picker_pressed{};
    // The picker's menu, dropped over the case from under the bar.
    struct Menu {
        struct Row {
            bool operator==(const Row&) const = default;
            std::string name; // the profile's, never logged
            Rect bounds, assign;
            bool shown{}, hovered{}, pressed{};
            bool assign_hovered{}, assign_enabled{}, assigned{};
        };
        bool operator==(const Menu&) const = default;
        Rect bounds, header, list; // `list` clips the rows
        std::string title;         // "Profiles", at the header's left
        std::vector<Row> rows;     // only those in view
        std::optional<Rect> thumb; // while more rows than six
        // Manage, at the header's right, which opens the editor on the
        // profile showing.
        Rect manage;
        std::string manage_label;
        bool manage_enabled{}, manage_hovered{}, manage_pressed{};
    };
    std::optional<Menu> menu;
    // A button whose label is being typed (the shortcut editor's Label
    // field): its index in `buttons`, drawn as a field with this text, the
    // caret at a byte offset of it.
    std::optional<std::size_t> field;
    std::string field_text; // never logged
    std::size_t field_cursor{};
};
// A chip of text: a recorded key combination, an assigned app.
struct ShortcutChip {
    bool operator==(const ShortcutChip&) const = default;
    std::string text;
    Rect bounds;
};
// Every control of the shortcut editor's screen carries this prefix, and
// no other control may: while the editor shows, the normal drawing pass
// leaves them to draw_shortcut_editor (R328).
inline constexpr std::string_view shortcut_editor_prefix = "shortcuts-";
// The modifiers and locks a key's face shows (KeyboardState's own, unless a
// view gives its own, as the shortcut editor's mini keyboard does).
struct KeyMarks {
    bool operator==(const KeyMarks&) const = default;
    std::set<int> modifiers, locked;
    bool caps{}, num{};
};
// The shortcut editor's mini keyboard (design, Typing and recording): the
// keys of the full US layout `us` that press a key, without its side column,
// the gaps between its blocks closed to 12 layout units, 4 under the
// function row and none between the other rows. In its own units from
// (0, 0). Rows touch, so each key's face leaves its side (`depth`) and a
// unit under it.
Layout mini_keyboard_layout(const Layout& us, double depth);
// The editor's popup for typing a label or a bar name, or recording keys,
// under the strip. Its buttons and its keys are controls named
// shortcuts-popup-… (the keys shortcuts-popup-key-<key id>); this holds the
// rest. Panel coordinates, but `layout`, whose keys are drawn at `keys`
// scaled by `scale`.
struct ShortcutPopupView {
    bool operator==(const ShortcutPopupView&) const = default;
    enum class Kind { Label, Name, Recording, Command };
    Kind kind{Kind::Label};
    Rect panel, well;        // the popup, and row 2's recessed strip
    std::string title, hint; // row 1; the hint after the title, when it fits
    Rect title_bounds, hint_bounds;
    std::string text; // Label, Name: never logged
    std::size_t cursor{};
    std::vector<ShortcutChip> chords;     // Recording, scrolled; the newest followed
    bool chords_before{}, chords_after{}; // more beyond a side: it fades (R331)
    // Recording: a quiet note in the strip, what to press while it is
    // empty, or after the chips why no more go.
    std::string full;
    Rect full_bounds;
    const Layout* layout{}; // the mini keyboard
    const Language* language{};
    const LanguageMap* keymap{}; // US legends, whatever language types
    Rect keys;                   // where its (0, 0) and its size land
    double scale{1};
    KeyMarks marks;                 // the popup's own latches and Caps Lock
    std::set<std::string> pressed;  // key IDs held down
    std::set<std::string> disabled; // key IDs that do nothing here: the locks
};
// A macro step's row in the editor's Macro view: a Keys step's chips (as
// many as fit, then an ellipsis), or a Wait step's time between its − and
// +, which are controls with its ✕ and the row itself.
struct ShortcutStepView {
    bool operator==(const ShortcutStepView&) const = default;
    Rect bounds;
    bool wait{};
    std::vector<ShortcutChip> chips;
    bool more{};
    Rect more_bounds;
    std::string time;
    Rect time_bounds;
    // Where its − + (a wait's) and ✕ are: controls on a row in its place,
    // drawn with the row while a drag carries it (R337).
    Rect minus_bounds, plus_bounds, remove_bounds;
};
// The shortcut editor's screen, over the case in place of the keys. Its
// pressable parts are controls named shortcuts-…, which it draws; this holds
// the rest. Panel coordinates.
struct ShortcutEditorView {
    bool operator==(const ShortcutEditorView&) const = default;
    double width{}; // the case's: the keyboard's, or without its numpad
    bool wide{};    // full width: the strip shows its hint
    std::string title;
    std::string notice; // shortcuts.json can't be read: why, and what saving does
    std::string profiles_caption, used_caption, used_hint, strip_hint, empty_hint;
    Rect column, profiles_list, strip_well, strip;
    Rect used;        // Used for: its caption at the top, its hint at the foot
    double divider{}; // x of the line right of the column
    double scale{1};  // the strip's buttons are keys at the layout's scale
    bool more_before{}, more_after{};
    std::optional<Rect> profiles_thumb;
    std::vector<ShortcutChip> ties; // Used for; their ✕ are controls
    // A row or button a drag carries, where the laser holds it; a button
    // carried off the strip would be deleted.
    std::optional<ShortcutChip> lifted_row, lifted_button;
    bool lifted_run{}; // the lifted or carried button is a Run button: its mark
    bool lifted_removing{};
    // A profile's row a carried button would go to, lit; and, once dropped
    // there, the popover with Move and Copy (controls shortcuts-carry-…).
    std::optional<Rect> drop_row, carry_panel;
    std::string label_caption, action_caption, keys_caption;
    // The combinations, scrolled, in a strip of fixed width between the
    // Keys caption and Record; a side with more beyond it fades (R331).
    std::vector<ShortcutChip> chords;
    Rect chords_strip;
    bool chords_before{}, chords_after{};
    std::vector<std::string> explanation;
    Rect explanation_bounds;
    // The Macro view: its steps in a well that scrolls, a lifted one where
    // the laser holds it, and Repeat's count for Times.
    std::string steps_caption, repeat_caption, wait_caption;
    Rect steps_well;
    std::vector<ShortcutStepView> steps;
    std::optional<Rect> steps_thumb;
    std::optional<ShortcutStepView> lifted_step;
    std::string count;
    Rect count_bounds;
    // The Run view in the Macro view's places: Script's list of the scripts
    // folder in a well that scrolls (its rows are controls
    // shortcuts-script-N), or Command's field; Script | Command under it.
    std::string run_caption, start_caption; // over the list or field; beside the switch
    Rect scripts_well;
    struct ScriptRow {
        bool operator==(const ScriptRow&) const = default;
        std::string name; // the user's file name, never logged
        Rect bounds;
        bool chosen{}, missing{};
    };
    std::vector<ScriptRow> scripts;
    std::optional<Rect> scripts_thumb;
    std::string scripts_note, missing_mark; // the empty list's note; "missing"
    // Typing or recording; under it the editor is dimmed but the strip.
    std::optional<ShortcutPopupView> popup;
};
// The keyboard's toolbar row while a label is renamed in place on the bar
// (design, Renaming in place): nothing typed there reaches the app. Its
// buttons are controls (field-…); the bar above shows the field
// (ShortcutBarView).
struct ShortcutFieldView {
    bool operator==(const ShortcutFieldView&) const = default;
    Rect strip;
    std::string title, hint;
    Rect hint_bounds; // where the hint fits; empty: left out
};
// Where the bar's parts go for `buttons` buttons on the case `view` shows.
// Sizes follow the layout's scale (design, The bar on the keyboard).
struct ShortcutBarGeometry {
    Rect plate, strip, picker; // the picker: the tab on the plate's right side
    double button_width{}, button_height{}, pitch{};
    double button_top{};   // a button's face at rest, its side below it
    std::size_t slots{};   // as many as show at once
    double scroll_limit{}; // how far the buttons can scroll
};
ShortcutBarGeometry shortcut_bar_geometry(const PanelView& view, std::size_t buttons);
// The picker's menu for `rows` bars: six rows show, more scroll.
struct ShortcutMenuGeometry {
    Rect bounds, header, list, manage; // Manage: in the header, at its right
    double row_height{};
    std::size_t visible{};
    double scroll_limit{};
    Rect row(std::size_t index, double scroll) const;
    Rect assign(const Rect& row) const;
};
inline constexpr std::size_t shortcut_menu_rows = 6;
ShortcutMenuGeometry shortcut_menu_geometry(const ShortcutBarGeometry& bar, std::size_t rows);
struct PanelView {
    const Layout* layout{};
    const Theme* theme{};
    const Language* language{};
    // Interface text's fonts follow the interface language; key legends,
    // candidates and typed text keep the typing language's (language->font).
    std::string interface_font{"Noto Sans,sans-serif"};
    // What the composing line says before anything is typed (App::composition_hint).
    std::string composition_hint;
    const LanguageMap* keymap{};
    const KeyboardState* keyboard{};
    // In place of `keyboard`'s modifiers and locks, for keys no key state
    // stands behind (key_marks()).
    std::optional<KeyMarks> key_marks;
    std::vector<Control> controls;
    std::vector<CardView> cards;
    std::vector<FieldLabel> field_labels;
    std::optional<Rect> popup;
    std::string status;
    // The status shows on the card above the case in dictation mode too,
    // not on its toolbar line: Send stopped, often because another window
    // or field took over, so the user may be looking away from the keys.
    bool status_card{};
    std::string version; // Shown in Settings; empty without an installed updater.
    bool settings{};
    bool composing{}; // The composing layout and its preedit are showing.
    bool hide_numpad{};
    // Background (Settings): how opaque the case is behind the keys, 1 when
    // filled; below 1 it is drawn see-through, and still takes the laser.
    double case_opacity{1};
    // Hover fade (Settings): a key the laser leaves dims over a moment, a
    // short trail behind the laser, instead of going dark at once.
    bool hover_fade{true};
    std::string preedit;
    // A composing language (Japanese, Chinese, Korean) shows its reading, a
    // hint while there is none, and its candidates scrolling sideways
    // (ime_band() says where). The converting segment is a byte range of
    // `preedit`.
    bool ime_band{};
    // The shown language doesn't compose (outside the emoji picker and
    // dictation mode): the keys keep the scale and place the reading line
    // gives them, so every layout's keys sit where a composing language's
    // do, and switching between them moves neither keys nor toolbar.
    bool ime_room{};
    // In every view but Settings (the emoji picker and dictation mode too)
    // the case ends `reading_room` sooner: reading_case_room(), from the main
    // layout, so the views' own layouts don't change it. Every layout's case
    // ends so, the same distance under its keys.
    double reading_room{};
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
    // The toolbar icon button a laser is on: its label shows in the same
    // place, above the case, while focus_tooltip is empty. Empty: none.
    std::string hover_tooltip;
    // A tooltip for a part of the shortcut bar the laser is on, such as the
    // picker, pointing at `bar_tooltip_at`, while neither above shows.
    std::string bar_tooltip;
    Rect bar_tooltip_at;
    // The shortcut bar above the case, while it shows; tooltips and the
    // status card then sit above it.
    std::optional<ShortcutBarView> shortcut_bar;
    // The shortcut editor's screen, in place of the keys; or the keyboard's
    // strip while one of its fields is typed or keys are recorded.
    std::optional<ShortcutEditorView> shortcut_editor;
    std::optional<ShortcutFieldView> shortcut_field;
    // Typing was refused for want of a target: from this time the Focus mode
    // button's icon shakes, like an error, instead of a status message.
    double focus_shake_since{-1};
    // The swipe button was pressed with no language that can swipe: from this
    // time its icon shakes, fading from the alert color.
    double swipe_shake_since{-1};
    std::set<std::string> hovered_keys, hovered_controls;
    bool status_hovered{}; // A laser is on the dictation status line.
    // Case coordinates of the swipe in progress, drawn above the keys.
    std::vector<std::pair<double, double>> swipe_trail;
    // While a press on a letter would start a swipe: each letter's reach in
    // layout coordinates (SwipeKeyboard::reach), where a press between keys
    // can start one too (PanelRenderer::swipe_key).
    const std::map<std::string, Rect>* swipe_reach{};
    // With Extended key collision, where each key of `layout` takes the
    // laser (key_areas.hpp); null leaves each key its own bounds.
    const std::vector<KeyArea>* key_areas{};
    // Debug card's Display key collision: a green square outline around
    // where each key takes the laser.
    bool key_outlines{};
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
        std::string title;
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
        // Room for the body below the title: two lines unless a question
        // needs more.
        double body_height{56};
        // Rows with a switch each, as Clear learned words has; the switches
        // are controls named confirm-row-N over the same bounds.
        struct Row {
            bool operator==(const Row&) const = default;
            std::string title, note;
            Rect bounds;
        };
        std::vector<Row> rows{};
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
        // The popup's own words, in the interface language.
        std::string title, layouts_caption, languages_caption, main_caption, additional_caption,
            drop_note;
        std::vector<Row> layouts, languages, additional; // only those in view
        Row main;
        bool main_target{}; // A lifted row would land on it.
        std::string hint;
        std::optional<Row> lifted;
        std::optional<Rect> layouts_thumb, languages_thumb, additional_thumb;
    };
    std::optional<LanguagesView> languages;
    // The theme picker: a card per theme, its preview over its name.
    struct ThemesView {
        bool operator==(const ThemesView&) const = default;
        struct Card {
            bool operator==(const Card&) const = default;
            std::string id, name;
            Rect bounds; // in view or partly so; the well clips it
            bool selected{};
            // Null until loaded or drawn (ThemePreviews); the card shows its
            // name over the theme's case color meanwhile.
            std::shared_ptr<cairo_surface_t> preview;
            Color surface;
        };
        Rect panel, well;
        std::string title;
        std::vector<Card> cards; // only those in view
        std::optional<Rect> thumb;
    };
    std::optional<ThemesView> themes;
    // The prefix of the open modal's controls; nothing else responds. A
    // question asked from the languages popup sits over it.
    std::string modal_prefix() const {
        return address                                     ? "address-"
               : confirm                                   ? "confirm-"
               : languages                                 ? "languages-"
               : themes                                    ? "themes-"
               : shortcut_editor && shortcut_editor->popup ? "shortcuts-popup-"
                                                           : "";
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
// What the theme picker repaints when only its cards changed between two
// views: the cards whose pictures arrived (R287), or the well when they
// scrolled, entered or left. Empty when the picker did not change or
// anything else did, which a full repaint covers.
std::vector<Rect> theme_cards_damage(const PanelView& before, const PanelView& now);
// The cards the laser left and entered, when nothing else in the hover
// changed (R292); empty otherwise.
std::vector<Rect> theme_hover_damage(const PanelView& before, const PanelView& now);
Rect panel_rect(const PanelView& view, Rect layout_rect);
// The rectangles of the shortcut bar the laser may hit: both plates and the
// open menu. The VR mask and the passthrough pointer add them to the case's.
std::vector<Rect> shortcut_bar_bounds(const PanelView& view);
// Everything that takes a pointer: the case, an open menu, the flick
// preview past the case, and the shortcut bar's plates and open menu. The
// VR mask, the passthrough pointer and the laser's hover, which the click
// watch and grabbing read, all use these, so a pull on any of them counts
// as one on the keyboard (R319). The move handle is the mask's own.
std::vector<Rect> pointer_bounds(const PanelView& view);
bool takes_pointer(const PanelView& view, double x, double y);
// What the bar repaints when only its buttons' or the picker's states
// changed (hover, press, enabled, running) or its buttons scrolled; empty
// when the bar did not change, or when anything else in it did, the open
// menu included (it lies over keys), which a full repaint covers. Bar
// buttons never force a full repaint by their hover alone.
std::vector<Rect> shortcut_bar_damage(const PanelView& before, const PanelView& now);
// Logical case bounds also define the VR interaction area. Hidden numpad space
// stays transparent in the fixed-size texture, preserving the dashboard anchor.
Rect panel_case_bounds(const PanelView& view);
// A case narrower than the panel (numpad hidden, flick layout) cannot fit the
// main toolbar and swipe suggestions in one row; the zoom and passthrough
// buttons then form a second row under Settings.
bool compact_toolbar(const PanelView& view);
// A compact toolbar's second row, 8 px under the first, in every layout: on
// a composing layout it sits on the reading line, its switches at the line's
// left, in every view of the layout.
constexpr double second_row_top = 62;
// How much sooner a layout's case ends (PanelView::reading_room). A full
// layout's keys keep the size they have under the normal toolbar, so its
// case loses only the height they leave spare under the reading line's
// taller one; the flick layout, as short as a phone's, is as wide as a full
// layout's case without its numpad and as tall as its keys need at that
// width.
double reading_case_room(const Layout& layout, const Theme& theme, bool flick);
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
// How much taller, in layout units, a layout may grow under the normal
// toolbar so that its keys keep the pixels `view` gives them under the
// band's taller toolbar (or its room kept), at the same scale. Dictation
// mode grows by it and no more: unbanded_layout_height would also take in
// the case's sub-pixel slack and move the keys off the normal layout's
// whole pixels.
double band_toolbar_room(const PanelView& view);
// A candidate's label width, as the band draws it.
double candidate_text_width(const std::string& text, const std::string& family);
// A label's text width at `size`, as interface labels draw it; for laying
// out chips that fit their text (R331).
double interface_text_width(const std::string& text, double size, const std::string& family);
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
    // The panel was hidden or shown: the next paint ends what was fading or
    // trailing at once, so nothing from before shows up after a summon. The
    // trail's layers go now, as nothing paints while the panel is hidden.
    void settle();
    // Gives the swipe trail's layers' pages back once no trail has been
    // drawn for a few seconds (R239). App calls it every tick.
    void release_idle_layers(double now);
    // Whether the swipe trail's layers are allocated; tests check they go.
    bool trail_layers() const { return trail_ || comet_ || comet_passes_; }
    // Drops the shared key bodies (body_atlas_) and the kept key
    // measurements (measured_); tests compare a renderer that keeps them with
    // one that draws and measures afresh.
    void forget_key_bodies();
    // How many bytes the shared key bodies take, for benchmarks.
    std::size_t key_body_bytes() const;
    // How many bytes of them are in memory, for tests that check the pages
    // are kept for the next word and then given back.
    std::size_t trail_resident_bytes() const;
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
        bool shift_locked{};  // a locked Shift fills its arrow, with the same modifiers
        std::set<int> locked; // locked modifiers show a border, with the same modifiers
    };
    std::optional<Snapshot> previous_;
    std::map<std::string, Rect> key_bounds_;
    // Each key's body at rest (side, face, edge light) drawn once per shape,
    // fill and tone at whole pixels and shared by every key like it; legends
    // are drawn over it. Dropped when the theme or the keys' scale changes,
    // and when the panel hides.
    std::map<std::string, cairo_surface_t*> body_atlas_;
    std::optional<Theme> atlas_theme_; // what the bodies were drawn for
    double atlas_scale_{};
    // Each shown key's measured bounds, by legend state (modifiers, Caps,
    // Num, a locked Shift), for the last few states (R261): a scene change
    // that changes no key, such as a status or a tooltip, measures nothing,
    // and tapping Shift back and forth measures each state once. Dropped when
    // something keys are drawn from changes (keys_changed in paint). A few
    // kilobytes.
    std::vector<std::pair<std::string, std::map<std::string, Rect>>> measured_; // most recent first
    std::array<double, 3> measured_placement_{}; // scale, x, y the measurements are for
    std::map<std::string, Animation> animations_;
    // How far each key is lit by the laser: 1 at once under it, then fading.
    std::map<std::string, Animation> hovers_;
    // Each key's lock ring as last drawn: how long it had run, at a 30 fps
    // step, or negative while none shows.
    std::map<std::string, double> lock_rings_;
    // Rows are 6 units apart, so a key's repaint area reaches a little into
    // the keys of the next row, and a pass's clip edge can cut a key there.
    // A key drawn by itself (moving, lit, fading, with a ring or a lock's
    // border) rasterizes slightly differently under such an edge, so it is
    // drawn once into an image at whole pixels, which every pass copies
    // exactly, as the flick preview is. Keys at rest copy their body from
    // the shared image already.
    // `marks`: key_marks(view), computed once per paint.
    bool drawn_by_itself(const PanelView& view, const Key& key, const KeyMarks& marks) const;
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
    // The swipe trail, in two layers that every pass copies. `trail_` holds the
    // whole path as an opaque thin line, composited faintly: Cairo strokes
    // slightly differently under a clip, and re-stroking a growing path
    // shifts its older antialiasing, so new segments are added without
    // touching pixels already drawn. `comet_` holds the path's last moments,
    // redrawn while any piece of it is alive.
    cairo_surface_t* trail_{};
    cairo_surface_t* comet_{};
    // Where each of the comet's stroke passes is drawn before it is laid
    // onto `comet_`: one layer kept with the others, instead of a group
    // Cairo would allocate, and likely map and fault in, every frame (R225).
    cairo_surface_t* comet_passes_{};
    std::vector<std::pair<double, double>> trail_points_; // The laser's points, as the view gave them.
    std::vector<double> trail_stamps_;                    // When each was first painted.
    std::size_t trail_drawn_{}; // Smoothed points whose segments are in the path layer.
    double trail_released_{-1}; // When the swipe ended; negative while it lasts.
    double trail_fade_{1};      // How much of the path's opacity is left after the swipe.
    std::optional<Rect> trail_extent_, comet_extent_;
    // The comet as last drawn: its 30 fps step, its oldest piece, and each
    // piece's reach from there, so a frame within a step redraws only the
    // pieces new points moved.
    double comet_step_{-1};
    std::size_t comet_first_{};
    std::vector<Rect> comet_pieces_;
    std::vector<Rect> damage_;
    double mic_phase_{1}; // How far the bars had slid when last drawn.
    double ring_time_{};
    double busy_ring_{};                  // A busy button's ring, stepping like the mic's.
    double swipe_shake_{}, swipe_fade_{}; // The swipe icon's, and its alert color left.
    double focus_shake_{}; // The Focus mode icon's shake offset, when last drawn.  // The recognition
                           // ring's time, sampled at 30 fps, when last drawn.
    double next_change_{};
    bool animating_{};
    bool settle_{};
    double layers_idle_since_{-1}; // when the last trail ended; negative while one is in use
};
} // namespace framekeyboard
