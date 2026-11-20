// The shortcut editor (docs/shortcut-bar-design.md, Settings and the editor):
// a screen of its own over the case, on its own copy of the profiles until
// ✓, with a popup over it where labels and profile names are typed and keys
// recorded on a mini keyboard of its own (Typing and recording). The label
// field typed on the real keyboard waits for renaming in place.
#include "framekeyboard/app.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
namespace {
using PopupKind = ShortcutPopupView::Kind;
constexpr std::string_view key_prefix = "shortcuts-popup-key-";
// The keyboard's own switches, never sent to an app (R330): Num Lock does
// nothing in the popup, where the numpad always types or records itself,
// and Caps Lock switches capitals for text but is never recorded.
bool popup_disabled(const Key& key, PopupKind kind) {
    return key.action == "NumLock" || (key.action == "CapsLock" && kind == PopupKind::Recording);
}
// A drag past this many pixels lifts a row or a button.
constexpr double dead_zone = 12;
constexpr double row_height = 52;
// The full and the no-numpad editor (design: 286 or 236, 300 or 250).
constexpr double narrow_width = 1326;
// A chip: its text, measured at the size it is drawn, and 12 px on each
// side (R331).
double chip_width(const std::string& text, double size, const std::string& family) {
    return interface_text_width(text, size, family) + 24;
}
// A Used for chip: the tie's stored name, else what its identity gives, as a
// hand-edited file may leave the name empty.
// A stream from a PC shows the editor's words in any language (R359).
std::string tie_chip(const AppTie& tie, const std::string& stream_name) {
    if (tie.app.kind == AppIdentity::Kind::Stream) {
        return stream_name;
    }
    return tie.name.empty() ? fallback_app_name(tie.app) : tie.name;
}
// The gap between chips, where a chevron goes, and one stick step's scroll.
constexpr double chip_gap = 28, chip_step = 80;
// A macro step's row in its well: 48 tall, 54 apart.
constexpr double step_height = 48, step_pitch = 54;
// A step's row, and where its parts go: a Keys step's chips run from
// `content` to `remove`; a Wait step has its − and + around its time.
struct StepGeometry {
    Rect row, content, remove, minus, time, plus;
};
StepGeometry step_geometry(const Rect& well, double top, std::size_t index, double scroll) {
    StepGeometry g;
    g.row = {well.x + 8, top + static_cast<double>(index) * step_pitch - scroll, well.width - 26,
             step_height};
    const auto& r = g.row;
    g.remove = {r.x + r.width - 44, r.y, 40, r.height};
    g.content = {r.x + 88, r.y, g.remove.x - 8 - (r.x + 88), r.height};
    g.minus = {r.x + 160, r.y + 6, 40, 36};
    g.time = {r.x + 204, r.y, 76, r.height};
    g.plus = {r.x + 284, r.y + 6, 40, 36};
    return g;
}
// Where the editor's parts go on a case of `body`.
struct EditorLayout {
    bool wide{};
    double column_width{};
    Rect column, profiles_list, new_profile, used;
    double list_top{}, divider{}, right{}, left{};
    std::size_t visible_rows{};
    Rect strip_well, strip, label, explanation;
    // The Keys row: the chips' strip of fixed width, then Record and clear,
    // which stay in place whatever is recorded (R331).
    Rect chips, record, clear;
    // The Macro view: the steps' well, + Keys and + Wait beside it, the
    // Repeat row with its count, and the line saying what a press does.
    Rect steps_well, add_keys, add_wait, repeat, count_minus, count_value, count_plus, sends;
    double steps_top{};
    // The Run view in the Macro view's places: the script list in the steps'
    // well, Refresh where + Keys is, the command field over the well's top,
    // Script | Command where Repeat is, and the line saying what it starts
    // beside the switch.
    Rect refresh, command, run_switch, run_line;
    double keys_y{}, segments_x{};
    double button_width{}, button_height{}, button_top{}, pitch{}, gap{};
};
EditorLayout editor_layout(const Rect& body, const ShortcutBarGeometry& bar, double depth,
                           std::size_t bars) {
    EditorLayout l;
    l.wide = body.width >= 1500;
    l.column_width = l.wide ? 286 : 236;
    const double x = body.x + 18;
    l.column = {x, 70, l.column_width, body.height - 70 - 16};
    l.list_top = l.column.y + 34;
    l.used = {x + 12, l.column.y + l.column.height - 150, l.column_width - 12, 150};
    const double room = l.used.y - 12 - row_height - l.list_top;
    l.visible_rows = static_cast<std::size_t>(std::max(1.0, std::floor(room / row_height)));
    const double shown = static_cast<double>(std::min(bars, l.visible_rows));
    l.profiles_list = {x, l.list_top, l.column_width, shown * row_height};
    l.new_profile = {x, l.list_top + shown * row_height, l.column_width, row_height};
    l.divider = x + l.column_width + 16;
    l.left = l.divider + 16;
    l.right = body.x + body.width - 18;
    l.strip_well = {l.left, 70, l.right - l.left, 102};
    l.strip = {l.left + 14, 70, l.right - l.left - 28, 102};
    l.button_width = bar.button_width;
    l.button_height = bar.button_height;
    l.pitch = bar.pitch;
    l.gap = bar.pitch - bar.button_width;
    const double scale = bar.button_width / 69.5;
    l.button_top = l.strip_well.y + (l.strip_well.height - l.button_height - depth * scale) / 2;
    l.label = {l.left + 100, 196, l.wide ? 300.0 : 250.0, 56};
    l.segments_x = l.label.x + l.label.width + 40 + 120;
    l.keys_y = 280;
    l.clear = {l.right - 60, l.keys_y, 60, 52};
    l.record = {l.clear.x - 12 - 160, l.keys_y, 160, 52};
    l.chips = {l.left + 100, l.keys_y, l.record.x - 16 - (l.left + 100), 52};
    l.add_keys = {l.right - 170, 267, 170, 52};
    l.add_wait = {l.right - 170, 329, 170, 52};
    l.steps_well = {l.label.x, 267, l.add_keys.x - 18 - l.label.x, 210};
    l.steps_top = l.steps_well.y + 7;
    l.repeat = {l.label.x, 494, 3 * 117, 54};
    l.count_minus = {l.repeat.x + l.repeat.width + 20, 495, 52, 52};
    l.count_value = {l.count_minus.x + 60, 495, 70, 52};
    l.count_plus = {l.count_value.x + 78, 495, 52, 52};
    // One 26-pixel line, centered on the Repeat row.
    l.sends = {l.count_plus.x + 72, 508, l.right - (l.count_plus.x + 72), 26};
    l.explanation = {l.left, 372, l.right - l.left, 60};
    l.refresh = l.add_keys;
    l.command = {l.label.x, l.steps_well.y, l.right - l.label.x, 56};
    l.run_switch = {l.label.x, l.repeat.y, 2 * 130, l.repeat.height};
    const double line_x = l.run_switch.x + l.run_switch.width + 24;
    l.run_line = {line_x, l.sends.y, l.right - line_x, 26};
    return l;
}
// A profile's row in the editor's column, scrolled.
Rect profile_row(const EditorLayout& l, std::size_t index, double scroll) {
    return {l.column.x, l.list_top + static_cast<double>(index) * row_height - scroll, l.column_width,
            row_height};
}
// The popover beside a profile's row that a button was dropped on: Move
// and Copy, and where the carried button waits at the row's end.
struct CarryGeometry {
    Rect panel, move, copy, button;
};
CarryGeometry carry_geometry(const EditorLayout& l, std::size_t row, double scroll) {
    const auto r = profile_row(l, row, scroll);
    CarryGeometry g;
    const double middle = r.y + r.height / 2;
    g.panel = {r.x + r.width + 14, middle - 31, 248, 62};
    g.move = {g.panel.x + 9, g.panel.y + 9, 110, 44};
    g.copy = {g.panel.x + 129, g.panel.y + 9, 110, 44};
    g.button = {r.x + r.width - l.button_width + 12, middle - l.button_height / 2, l.button_width,
                l.button_height};
    return g;
}
// One stick step at a time, as the bar scrolls; fractions add up.
template <class Scroll> bool step_scroll(Scroll& scroll, double delta, double size, double limit) {
    if (limit <= 0 || !std::isfinite(delta)) {
        return false;
    }
    scroll.remainder += delta;
    const double old = scroll.target;
    while (std::abs(scroll.remainder) >= 1) {
        const double sign = scroll.remainder > 0 ? 1 : -1;
        scroll.target = std::clamp(scroll.target + sign * size, 0.0, limit);
        scroll.remainder -= sign;
    }
    if ((scroll.target <= 0 && scroll.remainder < 0) ||
        (scroll.target >= limit && scroll.remainder > 0)) {
        scroll.remainder = 0;
    }
    return scroll.target != old;
}
// Where the popup's parts go (design, Typing and recording): under the
// strip, centered on the case, 380 tall whatever the width. Row 1 holds what
// is edited and the buttons, row 2 the recessed strip, and the keys take the
// rest at a scale that fits the mini layout's height into it.
struct PopupLayout {
    Rect panel, row, well, keys;
    double scale{};
};
PopupLayout popup_layout(const Rect& body, const Rect& strip_well, const Layout& mini) {
    // 6 above and below and 4 between its rows keep the mini keys at 42 x 48
    // in a 572-pixel case.
    constexpr double height = 380, pad = 6, margin = 12, row = 40, well = 44, between = 4;
    constexpr double keys_top = pad + row + between + well + between; // 98
    PopupLayout l;
    l.scale = mini.height > 0 ? (height - keys_top - pad) / mini.height : 1;
    const double width = mini.width * l.scale + 2 * margin;
    l.panel = {body.x + (body.width - width) / 2, strip_well.y + strip_well.height + 8, width, height};
    l.row = {l.panel.x + margin, l.panel.y + pad, width - 2 * margin, row};
    l.well = {l.panel.x + margin, l.panel.y + pad + row + between, width - 2 * margin, well};
    l.keys = {l.panel.x + margin, l.panel.y + keys_top, mini.width * l.scale, mini.height * l.scale};
    return l;
}
// The chips of the combinations in `strip`, `height` tall, scrolled by
// `scroll`, and where the pending modifiers' chip goes after them.
struct ChipRow {
    std::vector<ShortcutChip> chips;
    Rect pending;
    double end{};   // where the last chip ends, scrolled
    double limit{}; // how far the row can scroll
};
ChipRow chip_row(const Rect& strip, double height, const std::vector<ShortcutChord>& chords,
                 const std::string& pending, double scroll, const std::string& family) {
    ChipRow row;
    double x = strip.x;
    const double y = strip.y + (strip.height - height) / 2;
    for (const auto& chord : chords) {
        const auto text = chord_text(chord);
        const double width = chip_width(text, 20, family);
        row.chips.push_back({text, {x, y, width, height}});
        x += width + chip_gap;
    }
    if (!pending.empty()) {
        row.pending = {x, y, chip_width(pending, 20, family), height};
        x += row.pending.width + chip_gap;
    }
    const double content = std::max(0.0, x - chip_gap - strip.x);
    row.limit = std::max(0.0, content - strip.width);
    scroll = std::clamp(scroll, 0.0, row.limit);
    for (auto& chip : row.chips) {
        chip.bounds.x -= scroll;
    }
    row.pending.x -= scroll;
    row.end = strip.x + content - scroll;
    return row;
}
// The popup's row 2 inside its well: where its chips go.
// Remove last, an icon button at the right end of the recording's well,
// beside the combinations it takes back.
constexpr double remove_last_width = 52, remove_last_height = 36;
Rect popup_remove_last(const Rect& well) {
    return {well.x + well.width - 4 - remove_last_width, well.y + (well.height - remove_last_height) / 2,
            remove_last_width, remove_last_height};
}
// The combinations' strip in the well, short of Remove last.
Rect popup_chips_strip(const Rect& well) {
    return {well.x + 10, well.y, well.width - 20 - remove_last_width - 4, well.height};
}
// The pending modifiers' chip, while recording has room for it.
std::string pending_text(const std::vector<std::string>& latched, bool room) {
    return latched.empty() || !room ? std::string() : chord_text({latched, {}}) + " + …";
}
} // namespace

double App::editor_width(const PanelView& keyboard) const {
    // A full-width case keeps its width, and so does one with its numpad
    // hidden, to the fraction of a pixel (R329); any other, the flick
    // layout's (1509) included, takes the width without the numpad.
    const double width = panel_case_bounds(keyboard).width;
    if (width >= panel_width - .5 || (keyboard.hide_numpad && std::abs(width - narrow_width) < 2)) {
        return width;
    }
    return narrow_width;
}
void App::open_shortcut_editor() {
    if (settings_open_ || dictation_open_ || shortcut_editor_) {
        return;
    }
    stop_shortcut_run();
    bar_menu_ = false;
    bar_press_.reset();
    close_emoji();
    release_input(false); // a reading stays, out of sight, as Settings keeps it
    shortcut_editor_.emplace(shortcuts_, profile_choice_.shown());
    // Its candidates go out of sight and take no press until the editor closes (R333).
    candidate_list_ = false;
    sync_candidates();
    editor_question_ = EditorQuestion::None;
    editor_rows_scroll_ = editor_strip_scroll_ = editor_scripts_scroll_ = {};
    scripts_.reset(); // read again when the list shows
    scripts_beyond_.clear();
    shortcut_drag_.reset();
    carry_.reset();
    hovered_.clear();
    pressed_controls_.clear();
    dirty = true;
}
void App::close_shortcut_editor(bool save) {
    if (!shortcut_editor_) {
        return;
    }
    if (field_) {
        end_field(false);
    }
    end_popup(false);
    if (save) {
        // Each button keeps only its chosen action's content.
        const auto model = shortcut_editor_->saved();
        // The editor never builds a model past a limit; checked all the same.
        if (const auto problem = validate_shortcuts(model)) {
            status_ = tr("status.shortcuts-not-saved", {{"error", *problem}});
            dirty = true;
            return;
        }
        // Each item's limits allow a model larger than the file the next
        // start reads: refused, and the editor stays open with its draft (R399).
        if (serialize_shortcuts(model).size() > shortcut_limits::file_bytes) {
            status_ = tr("status.shortcuts-too-large");
            dirty = true;
            return;
        }
        // The profile selected in the editor shows, as if chosen in the
        // picker, and a free one becomes the free profile: a first profile shows as soon as
        // it is saved (R332).
        const auto* selected = shortcut_editor_->selected_profile();
        const std::string shown = selected ? selected->id : std::string();
        shortcuts_ = model;
        auto changes = profile_choice_.model_changed(shortcuts_);
        if (!shown.empty()) {
            const auto picked = profile_choice_.pick(shortcuts_, shown);
            changes.shortcuts_switch |= picked.shortcuts_switch;
        }
        // The worker keeps an unreadable file as shortcuts.json.bad; the
        // note goes once its result says the save worked. The free profile goes
        // in the same save.
        save_shortcuts();
        changes.free_profile = false;
        apply_bar_changes(changes);
    }
    shortcut_editor_.reset();
    editor_question_ = EditorQuestion::None;
    shortcut_drag_.reset();
    carry_.reset();
    hovered_.clear();
    pressed_controls_.clear();
    sync_candidates();    // a waiting reading's candidates come back (R333)
    update_settings_ui(); // the Debug card's sample button follows the profiles
    dirty = true;
}
void App::append_editor_controls(std::vector<Control>& result, const Rect& body) const {
    const auto add = [&](std::string id, std::string value, Control control, int index = 0) {
        control.id = std::move(id);
        control.action = {ControlKind::ShortcutEditor, index, std::move(value)};
        result.push_back(std::move(control));
    };
    const auto& model = shortcut_editor_->model();
    const auto bar = bar_geometry(0);
    const auto l = editor_layout(body, bar, shown_theme().depth, model.profiles.size());
    add("shortcuts-back", "back", {{}, {}, {body.x + 18, 12, 60, 42}, false, Icon::Back});
    add("shortcuts-save", "save", {{}, {}, {body.x + 88, 12, 60, 42}, false, Icon::Apply});
    const bool lifted = shortcut_drag_ && shortcut_drag_->lifted;
    // The profiles, a row each; the selected one has rename and delete.
    for (std::size_t i = 0; i < model.profiles.size(); ++i) {
        const Rect r{l.column.x,
                     l.list_top + static_cast<double>(i) * row_height - editor_rows_scroll_.shown,
                     l.column_width, row_height};
        if (r.y + r.height <= l.profiles_list.y || r.y >= l.profiles_list.y + l.profiles_list.height ||
            (lifted && shortcut_drag_->row && shortcut_drag_->index == i)) {
            continue;
        }
        Control row{{},
                    model.profiles[i].name,
                    {r.x, r.y + 2, r.width, r.height - 4},
                    shortcut_editor_->profile() == i};
        row.clip = l.profiles_list;
        add("shortcuts-row-" + std::to_string(i), "row", row, static_cast<int>(i));
        if (shortcut_editor_->profile() == i) {
            Control rename{{}, {}, {r.x + r.width - 92, r.y, 44, r.height}, false, Icon::Pencil};
            rename.clip = l.profiles_list;
            add("shortcuts-rename", "rename", rename);
            Control remove{{}, {}, {r.x + r.width - 48, r.y, 44, r.height}, false, Icon::Clear};
            remove.clip = l.profiles_list;
            add("shortcuts-delete", "delete", remove);
        }
    }
    Control new_profile{{}, tr("shortcuts.editor.new-profile"), l.new_profile};
    new_profile.enabled = shortcut_editor_->can_add_profile();
    add("shortcuts-new-profile", "new-profile", new_profile);
    // Used for: each tie's ✕.
    double x = l.used.x, y = l.used.y + 34;
    for (const auto index : shortcut_editor_->ties()) {
        const double width =
            std::min(l.used.width, chip_width(tie_chip(model.apps[index], tr("shortcuts.app.stream")),
                                              19, localization_.fonts()) +
                                       32);
        if (x + width > l.used.x + l.used.width && x > l.used.x) {
            x = l.used.x;
            y += 48;
        }
        add("shortcuts-tie-" + std::to_string(index), "tie", {{}, {}, {x + width - 38, y, 34, 40}},
            static_cast<int>(index));
        x += width + 8;
    }
    // The strip: the selected profile's buttons, then +.
    if (const auto* selected = shortcut_editor_->selected_profile()) {
        const std::size_t count = selected->buttons.size();
        for (std::size_t i = 0; i <= count; ++i) {
            const Rect r{l.strip.x + static_cast<double>(i) * l.pitch - editor_strip_scroll_.shown,
                         l.button_top, l.button_width, l.button_height};
            if (r.x + r.width <= l.strip.x || r.x >= l.strip.x + l.strip.width ||
                (i < count && lifted && !shortcut_drag_->row && !shortcut_drag_->step &&
                 shortcut_drag_->index == i) ||
                (i < count && carry_ && carry_->button == i)) {
                continue; // carried: drawn where the laser or the drop holds it
            }
            if (i == count) {
                Control plus{{}, {}, r};
                plus.clip = l.strip;
                plus.enabled = shortcut_editor_->can_add_button();
                add("shortcuts-add", "add", plus);
            } else {
                // A label being typed shows on its button as it grows.
                const bool typing =
                    popup_ && popup_->kind == PopupKind::Label && shortcut_editor_->button() == i;
                Control button{{},
                               typing ? popup_->text : selected->buttons[i].label,
                               r,
                               shortcut_editor_->button() == i};
                button.clip = l.strip;
                // A Run button's mark, as on the bar; pending, as all here is.
                if (selected->buttons[i].action.kind == ShortcutAction::Kind::Run) {
                    button.icon = Icon::RunMark;
                }
                add("shortcuts-button-" + std::to_string(i), "button", button, static_cast<int>(i));
            }
        }
    }
    if (const auto* button = shortcut_editor_->selected_button()) {
        const bool typing = popup_ && popup_->kind == PopupKind::Label;
        Control label{{}, typing ? popup_->text : button->label, l.label};
        label.style = ControlStyle::TextField;
        add("shortcuts-label", "label", label);
        const auto kind = button->action.kind;
        const std::array<std::pair<const char*, ShortcutAction::Kind>, 3> kinds{
            {{"shortcuts.editor.shortcut", ShortcutAction::Kind::Keys},
             {"shortcuts.editor.macro", ShortcutAction::Kind::Macro},
             {"shortcuts.editor.run", ShortcutAction::Kind::Run}}};
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            // Shortcut, Macro and Run switch, each keeping its content.
            Control segment{{},
                            tr(kinds[i].first),
                            {l.segments_x + 110 * static_cast<double>(i), l.label.y, 110, 56},
                            kind == kinds[i].second};
            segment.style = ControlStyle::Segment;
            segment.segments = 3;
            add("shortcuts-kind-" + std::to_string(i), "kind", segment, static_cast<int>(i));
        }
        if (kind == ShortcutAction::Kind::Keys) {
            add("shortcuts-record", "record", {{}, tr("shortcuts.editor.record"), l.record});
            Control clear{{}, {}, l.clear, false, Icon::Clear};
            clear.enabled = !button->action.chords.empty();
            add("shortcuts-clear", "clear", clear);
        }
        if (kind == ShortcutAction::Kind::Macro) {
            append_macro_controls(result, l.steps_well, *button);
        }
        if (kind == ShortcutAction::Kind::Run) {
            append_run_controls(result, *button);
        }
    }
    if (carry_) {
        // Move and Copy beside the row the button was dropped on.
        const auto g = carry_geometry(l, carry_->profile, editor_rows_scroll_.shown);
        add("shortcuts-carry-move", "carry-move", {{}, tr("shortcuts.editor.move"), g.move});
        add("shortcuts-carry-copy", "carry-copy", {{}, tr("shortcuts.editor.copy"), g.copy});
    }
    if (popup_) {
        append_popup_controls(result, body);
    }
    if (editor_question_ != EditorQuestion::None) {
        const Rect panel = confirm_popup_bounds;
        const bool leave = editor_question_ == EditorQuestion::Leave;
        result.push_back({"confirm-cancel",
                          tr(leave ? "shortcuts.question.discard" : "button.cancel"),
                          {panel.x + 40, panel.y + panel.height - 80, 210, 52}});
        result.back().action = {ControlKind::ShortcutEditor, 0, "question-cancel"};
        result.push_back({"confirm-accept",
                          tr(leave ? "shortcuts.question.save" : "shortcuts.question.delete"),
                          {panel.x + panel.width - 250, panel.y + panel.height - 80, 210, 52},
                          true});
        result.back().action = {ControlKind::ShortcutEditor, 0, "question-accept"};
    }
}
std::vector<App::ScriptRow> App::script_rows() const {
    std::vector<ScriptRow> rows;
    if (scripts_) {
        for (const auto& name : *scripts_) {
            rows.push_back({name, false});
        }
    }
    // A chosen script past the list's end shows in its place; one that is
    // gone stays there too, marked missing.
    if (const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
        button && !button->action.script.empty() && scripts_ &&
        std::none_of(rows.begin(), rows.end(),
                     [&](const ScriptRow& row) { return row.name == button->action.script; })) {
        const auto& name = button->action.script;
        const auto at = std::find_if(rows.begin(), rows.end(),
                                     [&](const ScriptRow& row) { return row.name > name; });
        const bool there =
            std::find(scripts_beyond_.begin(), scripts_beyond_.end(), name) != scripts_beyond_.end();
        rows.insert(at, {name, !there});
    }
    return rows;
}
bool App::editing_scripts() const {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    return button && button->action.kind == ShortcutAction::Kind::Run && !button->action.command_chosen;
}
double App::editor_scripts_limit() const {
    if (!editing_scripts()) {
        return 0;
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto count = script_rows().size();
    if (count == 0) {
        return 0;
    }
    const double content = static_cast<double>(count) * step_pitch + 14 - step_pitch + step_height;
    return std::max(0.0, content - l.steps_well.height);
}
std::string App::scripts_folder_text() const {
    const auto folder = scripts_folder(fs::absolute(config_dir())).lexically_normal().string();
    const char* home = std::getenv("HOME");
    const std::string prefix = home && *home ? fs::path(home).lexically_normal().string() : "";
    if (!prefix.empty() && folder.starts_with(prefix) &&
        (folder.size() == prefix.size() || folder[prefix.size()] == '/')) {
        return "~" + folder.substr(prefix.size());
    }
    return folder;
}
void App::append_run_controls(std::vector<Control>& result, const ShortcutButton& button) const {
    const auto add = [&](std::string id, std::string value, Control control, int index = 0) {
        control.id = std::move(id);
        control.action = {ControlKind::ShortcutEditor, index, std::move(value)};
        result.push_back(std::move(control));
    };
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto& action = button.action;
    if (!action.command_chosen) {
        // The folder's scripts, a row each, clipped to the well; a press
        // chooses one, and on the chosen one clears it.
        const auto rows = script_rows();
        const auto& well = l.steps_well;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto g = step_geometry(well, l.steps_top, i, editor_scripts_scroll_.shown);
            if (g.row.y + g.row.height <= well.y || g.row.y >= well.y + well.height) {
                continue;
            }
            Control row{{}, rows[i].name, g.row, rows[i].name == action.script};
            row.clip = well;
            add("shortcuts-script-" + std::to_string(i), "script:" + rows[i].name, row);
        }
        add("shortcuts-refresh", "refresh", {{}, tr("shortcuts.editor.refresh"), l.refresh});
    } else {
        // The caption as large as Label's and the others; the field starts
        // as far right as it needs (R348).
        auto bounds = l.command;
        const double caption =
            interface_text_width(tr("shortcuts.editor.command"), 22, localization_.fonts());
        const double start = l.divider + 16 + caption + 16;
        if (start > bounds.x) {
            bounds.width -= start - bounds.x;
            bounds.x = start;
        }
        Control field{{}, action.command, bounds};
        field.style = ControlStyle::TextField;
        add("shortcuts-command", "command", field);
    }
    // Script | Command, as Repeat's switch.
    for (std::size_t i = 0; i < 2; ++i) {
        Control segment{
            {},
            tr(i == 0 ? "shortcuts.editor.script" : "shortcuts.editor.command"),
            {l.run_switch.x + 130 * static_cast<double>(i), l.run_switch.y, 130, l.run_switch.height},
            action.command_chosen == (i == 1)};
        segment.style = ControlStyle::Segment;
        segment.segments = 2;
        add("shortcuts-run-" + std::to_string(i), "run-mode", segment, static_cast<int>(i));
    }
}
void App::append_macro_controls(std::vector<Control>& result, const Rect& well,
                                const ShortcutButton& button) const {
    const auto add = [&](std::string id, std::string value, Control control, int index = 0) {
        control.id = std::move(id);
        control.action = {ControlKind::ShortcutEditor, index, std::move(value)};
        result.push_back(std::move(control));
    };
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto& steps = button.action.steps;
    const bool lifted = shortcut_drag_ && shortcut_drag_->lifted && shortcut_drag_->step;
    // The steps' rows, in the well, clipped to it; a lifted one follows the laser.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto g = step_geometry(well, l.steps_top, i, editor_steps_scroll_.shown);
        if (g.row.y + g.row.height <= well.y || g.row.y >= well.y + well.height ||
            (lifted && shortcut_drag_->index == i)) {
            continue;
        }
        const auto index = static_cast<int>(i);
        Control row{{}, {}, g.row};
        row.clip = well;
        add("shortcuts-step-" + std::to_string(i), "step", row, index);
        if (steps[i].kind == MacroStep::Kind::Wait) {
            Control minus{{}, "−", g.minus};
            minus.clip = well;
            minus.enabled = steps[i].wait_ms > shortcut_limits::wait_min_ms;
            add("shortcuts-step-minus-" + std::to_string(i), "step-minus", minus, index);
            Control plus{{}, "+", g.plus};
            plus.clip = well;
            plus.enabled = steps[i].wait_ms < shortcut_limits::wait_max_ms;
            add("shortcuts-step-plus-" + std::to_string(i), "step-plus", plus, index);
        }
        Control remove{{}, {}, g.remove, false, Icon::Close};
        remove.clip = well;
        add("shortcuts-step-delete-" + std::to_string(i), "step-delete", remove, index);
    }
    const bool room = shortcut_editor_->can_add_step();
    Control keys{{}, tr("shortcuts.editor.add-keys"), l.add_keys};
    keys.enabled = room;
    add("shortcuts-add-keys", "add-keys", keys);
    Control wait{{}, tr("shortcuts.editor.add-wait"), l.add_wait};
    wait.enabled = room;
    add("shortcuts-add-wait", "add-wait", wait);
    // Repeat: Once, Times with its count, While held.
    const std::array<const char*, 3> modes{"shortcuts.editor.once", "shortcuts.editor.times",
                                           "shortcuts.editor.while-held"};
    const auto mode = static_cast<std::size_t>(button.action.repeat.mode);
    for (std::size_t i = 0; i < modes.size(); ++i) {
        Control segment{{},
                        tr(modes[i]),
                        {l.repeat.x + 117 * static_cast<double>(i), l.repeat.y, 117, l.repeat.height},
                        mode == i};
        segment.style = ControlStyle::Segment;
        segment.segments = 3;
        add("shortcuts-repeat-" + std::to_string(i), "repeat", segment, static_cast<int>(i));
    }
    if (button.action.repeat.mode == MacroRepeat::Mode::Times) {
        Control minus{{}, "−", l.count_minus};
        minus.enabled = button.action.repeat.count > shortcut_limits::repeat_min;
        add("shortcuts-count-minus", "count-minus", minus);
        Control plus{{}, "+", l.count_plus};
        plus.enabled = button.action.repeat.count < shortcut_limits::repeat_max;
        add("shortcuts-count-plus", "count-plus", plus);
    }
}
namespace {
// The macro's steps as rows in their well, scrolled; or the one row at
// `only` (a lifted one), wherever the list would have it.
std::vector<ShortcutStepView> steps_view(const EditorLayout& l, const ShortcutAction& action,
                                         double scroll, const std::string& family,
                                         const std::function<std::string(int)>& wait_text,
                                         std::optional<std::size_t> only = {}) {
    std::vector<ShortcutStepView> rows;
    for (std::size_t i = 0; i < action.steps.size(); ++i) {
        if (only && *only != i) {
            continue;
        }
        const auto& step = action.steps[i];
        const auto g = step_geometry(l.steps_well, l.steps_top, i, scroll);
        ShortcutStepView row;
        row.bounds = g.row;
        row.remove_bounds = g.remove;
        row.wait = step.kind == MacroStep::Kind::Wait;
        if (row.wait) {
            row.time = wait_text(step.wait_ms);
            row.time_bounds = g.time;
            row.minus_bounds = g.minus;
            row.plus_bounds = g.plus;
        } else {
            // As many chips as fit, the rest an ellipsis.
            const auto chips =
                chip_row({g.content.x, g.row.y, 1e6, g.row.height}, 36, step.keys, {}, 0, family);
            for (const auto& chip : chips.chips) {
                if (chip.bounds.x + chip.bounds.width > g.content.x + g.content.width - 36) {
                    row.more = true;
                    break;
                }
                row.chips.push_back(chip);
            }
            if (row.more) {
                const double end = row.chips.empty()
                                       ? g.content.x
                                       : row.chips.back().bounds.x + row.chips.back().bounds.width;
                row.more_bounds = {end + 4, g.row.y, 32, g.row.height};
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace
double App::editor_steps_limit() const {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    if (!button || button->action.kind != ShortcutAction::Kind::Macro) {
        return 0;
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const double content =
        static_cast<double>(button->action.steps.size()) * step_pitch + 14 - step_pitch + step_height;
    return std::max(0.0, content - l.steps_well.height);
}
// What a press on a macro does, for the line beside its Repeat row.
// The time a wait shows, with the language's decimal mark and unit: 0.5 s,
// 0,5 s, 0.5 秒; the same in the steps list and in what a press sends (R377).
std::string App::wait_text(int ms) const {
    return tr("shortcuts.editor.seconds", {{"number", localization_.decimal(ms / 1000.0, 1)}});
}
std::string App::macro_sends(const ShortcutAction& action) const {
    if (action.steps.empty()) {
        return tr("shortcuts.editor.macro-empty");
    }
    std::string keys;
    for (const auto& step : action.steps) {
        if (!keys.empty()) {
            keys += tr("shortcuts.editor.then");
        }
        if (step.kind == MacroStep::Kind::Wait) {
            keys += tr("shortcuts.editor.wait-for", {{"time", wait_text(step.wait_ms)}});
            continue;
        }
        for (std::size_t i = 0; i < step.keys.size(); ++i) {
            keys += (i ? " " : "") + chord_text(step.keys[i]);
        }
    }
    switch (action.repeat.mode) {
    case MacroRepeat::Mode::Times:
        return tr("shortcuts.editor.macro-times",
                  {{"keys", keys}, {"count", static_cast<long long>(action.repeat.count)}});
    case MacroRepeat::Mode::WhileHeld:
        return tr("shortcuts.editor.macro-held", {{"keys", keys}});
    case MacroRepeat::Mode::Once:
        break;
    }
    return tr("shortcuts.editor.macro-once", {{"keys", keys}});
}
void App::append_shortcut_editor(PanelView& v) const {
    const auto body = panel_case_bounds(v);
    const auto& model = shortcut_editor_->model();
    const auto bar = bar_geometry(0);
    const auto l = editor_layout(body, bar, shown_theme().depth, model.profiles.size());
    auto& e = *v.shortcut_editor;
    e.wide = l.wide;
    e.title = tr("shortcuts.editor.title");
    if (!shortcuts_invalid_.empty()) {
        e.notice = tr("shortcuts.editor.invalid", {{"reason", shortcuts_invalid_}});
    }
    e.profiles_caption = tr("shortcuts.editor.profiles");
    e.column = l.column;
    e.profiles_list = l.profiles_list;
    e.divider = l.divider;
    e.scale = bar.button_width / 69.5;
    const double rows_limit =
        std::max(0.0, static_cast<double>(model.profiles.size()) * row_height - l.profiles_list.height);
    if (rows_limit > 0) {
        const double length =
            l.profiles_list.height * l.profiles_list.height / (l.profiles_list.height + rows_limit);
        e.profiles_thumb = Rect{l.column.x + l.column.width + 4,
                                l.profiles_list.y + editor_rows_scroll_.shown / rows_limit *
                                                        (l.profiles_list.height - length),
                                3, length};
    }
    if (shortcut_editor_->selected_profile()) {
        e.used = l.used;
        e.used_caption = tr("shortcuts.editor.used");
        double x = l.used.x, y = l.used.y + 34;
        for (const auto index : shortcut_editor_->ties()) {
            const auto name = tie_chip(model.apps[index], tr("shortcuts.app.stream"));
            const double width =
                std::min(l.used.width, chip_width(name, 19, localization_.fonts()) + 32);
            if (x + width > l.used.x + l.used.width && x > l.used.x) {
                x = l.used.x;
                y += 48;
            }
            e.ties.push_back({name, {x, y, width, 40}});
            x += width + 8;
        }
        e.used_hint = tr(e.ties.empty() ? "shortcuts.editor.used-none" : "shortcuts.editor.used-hint");
        e.strip_well = l.strip_well;
        e.strip = l.strip;
        const auto count = shortcut_editor_->selected_profile()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        e.more_before = editor_strip_scroll_.shown > .5;
        e.more_after = editor_strip_scroll_.shown < limit - .5;
        // The hint only where the buttons leave it room.
        if (l.wide && static_cast<double>(count + 1) * l.pitch < l.strip.width - 440) {
            e.strip_hint = tr("shortcuts.editor.strip-hint");
        }
        if (count == 0) {
            e.empty_hint = tr("shortcuts.editor.no-buttons");
        }
    } else {
        e.empty_hint = tr("shortcuts.editor.no-profiles");
    }
    if (const auto* button = shortcut_editor_->selected_button()) {
        e.label_caption = tr("shortcuts.editor.label");
        e.action_caption = tr("shortcuts.editor.action");
        e.explanation_bounds = l.explanation;
        switch (button->action.kind) {
        case ShortcutAction::Kind::Keys: {
            e.keys_caption = tr("shortcuts.editor.keys");
            const auto row = chip_row(l.chips, 52, button->action.chords, {}, editor_chips_scroll_.shown,
                                      localization_.fonts());
            e.chords = row.chips;
            e.chords_strip = l.chips;
            e.chords_before = editor_chips_scroll_.shown > .5;
            e.chords_after = editor_chips_scroll_.shown < row.limit - .5;
            std::string sent;
            for (const auto& chord : button->action.chords) {
                sent += (sent.empty() ? "" : tr("shortcuts.editor.then")) + chord_text(chord);
            }
            e.explanation.push_back(sent.empty() ? tr("shortcuts.editor.no-keys")
                                                 : tr("shortcuts.editor.sends", {{"keys", sent}}));
            e.explanation.push_back(tr("shortcuts.editor.record-hint"));
            break;
        }
        case ShortcutAction::Kind::Macro: {
            const auto& action = button->action;
            e.steps_caption = tr("shortcuts.editor.steps");
            e.repeat_caption = tr("shortcuts.editor.repeat");
            e.wait_caption = tr("shortcuts.editor.wait");
            e.steps_well = l.steps_well;
            e.steps = steps_view(l, action, editor_steps_scroll_.shown, localization_.fonts(),
                                 [this](int ms) { return wait_text(ms); });
            const double limit = editor_steps_limit();
            if (limit > 0) {
                const auto& well = l.steps_well;
                const double length = well.height * well.height / (well.height + limit);
                e.steps_thumb =
                    Rect{well.x + well.width - 10,
                         well.y + 6 + editor_steps_scroll_.shown / limit * (well.height - 12 - length),
                         3, length};
            }
            if (action.repeat.mode == MacroRepeat::Mode::Times) {
                e.count = std::to_string(action.repeat.count);
                e.count_bounds = l.count_value;
            }
            // What a press does, beside Repeat, ending in an ellipsis.
            e.explanation_bounds = l.sends;
            e.explanation.push_back(macro_sends(action));
            break;
        }
        case ShortcutAction::Kind::Run: {
            const auto& action = button->action;
            const auto folder = scripts_folder_text();
            e.start_caption = tr("shortcuts.editor.start");
            e.explanation_bounds = l.run_line;
            if (action.command_chosen) {
                e.run_caption = tr("shortcuts.editor.command");
                e.explanation.push_back(action.command.empty() ? tr("shortcuts.editor.type-command")
                                                               : tr("shortcuts.editor.starts-command",
                                                                    {{"command", action.command}}));
                break;
            }
            e.run_caption = tr("shortcuts.editor.script");
            e.scripts_well = l.steps_well;
            e.missing_mark = tr("shortcuts.editor.missing");
            const auto rows = script_rows();
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto g = step_geometry(l.steps_well, l.steps_top, i, editor_scripts_scroll_.shown);
                e.scripts.push_back(
                    {rows[i].name, g.row, rows[i].name == action.script, rows[i].missing});
            }
            if (scripts_ && rows.empty()) {
                e.scripts_note = tr("shortcuts.editor.no-scripts", {{"folder", folder}});
            }
            const double limit = editor_scripts_limit();
            if (limit > 0) {
                const auto& well = l.steps_well;
                const double length = well.height * well.height / (well.height + limit);
                e.scripts_thumb =
                    Rect{well.x + well.width - 10,
                         well.y + 6 + editor_scripts_scroll_.shown / limit * (well.height - 12 - length),
                         3, length};
            }
            const bool missing = std::any_of(rows.begin(), rows.end(), [&](const ScriptRow& row) {
                return row.missing && row.name == action.script;
            });
            e.explanation.push_back(
                action.script.empty() ? tr("shortcuts.editor.choose-script", {{"folder", folder}})
                : missing ? tr("shortcuts.editor.script-gone", {{"script", action.script}})
                          : tr("shortcuts.editor.starts-script", {{"script", action.script}}));
            break;
        }
        }
    }
    // What a drag carries, where the laser holds it.
    if (shortcut_drag_ && shortcut_drag_->lifted) {
        const auto& d = *shortcut_drag_;
        if (d.row && d.index < model.profiles.size()) {
            e.lifted_row =
                ShortcutChip{model.profiles[d.index].name,
                             {l.column.x, d.py - row_height / 2, l.column_width, row_height - 4}};
        } else if (const auto* button = shortcut_editor_->selected_button();
                   d.step && button && d.index < button->action.steps.size()) {
            // The row the laser carries, drawn where it holds it.
            auto rows = steps_view(
                l, button->action, editor_steps_scroll_.shown, localization_.fonts(),
                [this](int ms) { return wait_text(ms); }, d.index);
            if (!rows.empty()) {
                // Everything of the row moves with it: its time, − and +,
                // chips and ✕ (R337).
                auto lifted = rows.front();
                const double dy = d.py - step_height / 2 - lifted.bounds.y;
                for (auto* r : {&lifted.bounds, &lifted.more_bounds, &lifted.time_bounds,
                                &lifted.minus_bounds, &lifted.plus_bounds, &lifted.remove_bounds}) {
                    r->y += dy;
                }
                for (auto& chip : lifted.chips) {
                    chip.bounds.y += dy;
                }
                e.lifted_step = lifted;
            }
        } else if (const auto* selected = shortcut_editor_->selected_profile();
                   d.button() && selected && d.index < selected->buttons.size()) {
            e.lifted_button = ShortcutChip{selected->buttons[d.index].label,
                                           {d.px - l.button_width / 2, d.py - l.button_height / 2,
                                            l.button_width, l.button_height}};
            e.lifted_run = selected->buttons[d.index].action.kind == ShortcutAction::Kind::Run;
            // Over a profile that takes it, its row lights up; over a row
            // that doesn't, nothing is deleted either.
            e.lifted_removing = d.outside && !d.over_row;
            if (d.over_row && d.takes) {
                e.drop_row = profile_row(l, *d.over_row, editor_rows_scroll_.shown);
            }
        }
    }
    if (const auto* selected = shortcut_editor_->selected_profile();
        carry_ && selected && carry_->button < selected->buttons.size()) {
        // Dropped on another profile: its row lit, the button at its end,
        // and Move and Copy beside it.
        const auto g = carry_geometry(l, carry_->profile, editor_rows_scroll_.shown);
        e.drop_row = profile_row(l, carry_->profile, editor_rows_scroll_.shown);
        e.lifted_button = ShortcutChip{selected->buttons[carry_->button].label, g.button};
        e.lifted_run = selected->buttons[carry_->button].action.kind == ShortcutAction::Kind::Run;
        e.carry_panel = g.panel;
    }
    if (popup_) {
        append_popup_view(e, body);
    }
    if (editor_question_ == EditorQuestion::Leave) {
        v.confirm = PanelView::ConfirmView{confirm_popup_bounds, tr("shortcuts.question.leave-title"),
                                           tr("shortcuts.question.leave-body")};
    } else if (editor_question_ == EditorQuestion::DeleteProfile &&
               shortcut_editor_->selected_profile()) {
        const auto count = static_cast<long long>(shortcut_editor_->selected_profile()->buttons.size());
        v.confirm = PanelView::ConfirmView{confirm_popup_bounds, tr("shortcuts.question.delete-title"),
                                           tr("shortcuts.question.delete-body", {{"count", count}})};
    }
}
void App::editor_action(const ControlAction& command) {
    if (!shortcut_editor_) {
        return;
    }
    const auto& value = command.value;
    const auto index = static_cast<std::size_t>(std::max(0, command.index));
    if (value.starts_with("popup-")) {
        popup_action(value);
        return;
    }
    if (value == "carry-move" || value == "carry-copy") {
        // Either puts the button at the end of that profile; the selected
        // profile stays selected, so more buttons can follow.
        if (const auto carry = std::exchange(carry_, std::nullopt)) {
            shortcut_editor_->carry_button(carry->button, carry->profile, value == "carry-copy");
        }
        dirty = true;
        return;
    }
    if (value == "question-cancel" || value == "question-accept") {
        const auto question = std::exchange(editor_question_, EditorQuestion::None);
        const bool accept = value == "question-accept";
        if (question == EditorQuestion::Leave) {
            close_shortcut_editor(accept); // Discard or Save
            return;
        }
        if (question == EditorQuestion::DeleteProfile && accept) {
            shortcut_editor_->delete_profile();
            editor_strip_scroll_ = {};
        }
    } else if (value == "back") {
        if (shortcut_editor_->changed()) {
            editor_question_ = EditorQuestion::Leave;
        } else {
            close_shortcut_editor(false);
            return;
        }
    } else if (value == "save") {
        close_shortcut_editor(true);
        return;
    } else if (value == "row") {
        shortcut_editor_->select_profile(index);
        editor_strip_scroll_ = {};
        editor_chips_scroll_ = editor_steps_scroll_ = {};
    } else if (value == "rename") {
        open_popup(PopupKind::Name);
    } else if (value == "delete") {
        if (const auto* bar = shortcut_editor_->selected_profile(); bar && !bar->buttons.empty()) {
            editor_question_ = EditorQuestion::DeleteProfile; // its buttons would go too
        } else {
            shortcut_editor_->delete_profile();
            editor_strip_scroll_ = {};
        }
    } else if (value == "new-profile") {
        const auto number = static_cast<long long>(shortcut_editor_->model().profiles.size() + 1);
        shortcut_editor_->add_profile(tr("shortcuts.editor.profile-name", {{"number", number}}));
        editor_strip_scroll_ = {};
        // The new row in view, then its name to type.
        const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                     shortcut_editor_->model().profiles.size());
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().profiles.size()) * row_height -
                              l.profiles_list.height);
        editor_rows_scroll_.shown = editor_rows_scroll_.target = limit;
        open_popup(PopupKind::Name);
    } else if (value == "tie") {
        shortcut_editor_->remove_tie(index);
    } else if (value == "button") {
        shortcut_editor_->select_button(index);
        editor_chips_scroll_ = editor_steps_scroll_ = editor_scripts_scroll_ = {};
    } else if (value == "add") {
        shortcut_editor_->add_button();
        editor_chips_scroll_ = {};
        // The new button in view, at the strip's end.
        const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                     shortcut_editor_->model().profiles.size());
        const auto count = shortcut_editor_->selected_profile()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        editor_strip_scroll_.target = limit;
    } else if (value == "label") {
        open_popup(PopupKind::Label);
    } else if (value == "record") {
        open_popup(PopupKind::Recording);
    } else if (value == "clear") {
        shortcut_editor_->set_chords({});
        editor_chips_scroll_ = {};
    } else if (value == "kind") {
        shortcut_editor_->set_kind(command.index == 2   ? ShortcutAction::Kind::Run
                                   : command.index == 1 ? ShortcutAction::Kind::Macro
                                                        : ShortcutAction::Kind::Keys);
        editor_chips_scroll_ = editor_steps_scroll_ = editor_scripts_scroll_ = {};
    } else if (value == "run-mode") {
        shortcut_editor_->set_run_command(command.index == 1);
        editor_scripts_scroll_ = {};
    } else if (value.starts_with("script:")) {
        // A press chooses a script; one on the chosen one clears it.
        const auto name = value.substr(7);
        const auto* button = shortcut_editor_->selected_button();
        shortcut_editor_->set_script(button && button->action.script == name ? std::string() : name);
    } else if (value == "refresh") {
        read_scripts();
    } else if (value == "command") {
        open_popup(PopupKind::Command);
    } else if (value == "step") {
        // A Keys step's row opens the recorder for it.
        const auto* button = shortcut_editor_->selected_button();
        if (button && index < button->action.steps.size() &&
            button->action.steps[index].kind == MacroStep::Kind::Keys) {
            open_popup(PopupKind::Recording, index);
        }
    } else if (value == "step-delete") {
        shortcut_editor_->delete_step(index);
    } else if (value == "step-minus" || value == "step-plus") {
        const auto* button = shortcut_editor_->selected_button();
        if (button && index < button->action.steps.size()) {
            constexpr int step_ms = 100;
            const int ms = button->action.steps[index].wait_ms;
            shortcut_editor_->set_wait(index, value == "step-plus" ? ms + step_ms : ms - step_ms);
        }
    } else if (value == "add-keys") {
        open_popup(PopupKind::Recording, std::nullopt, true);
    } else if (value == "add-wait") {
        MacroStep wait;
        wait.kind = MacroStep::Kind::Wait;
        wait.wait_ms = 500;
        shortcut_editor_->add_step(wait);
        editor_steps_scroll_.target = editor_steps_limit(); // the new step in view
    } else if (value == "repeat" || value == "count-minus" || value == "count-plus") {
        const auto* button = shortcut_editor_->selected_button();
        if (button && button->action.kind == ShortcutAction::Kind::Macro) {
            auto repeat = button->action.repeat;
            if (value == "repeat") {
                repeat.mode = static_cast<MacroRepeat::Mode>(std::clamp(command.index, 0, 2));
                if (repeat.mode == MacroRepeat::Mode::Times &&
                    repeat.count < shortcut_limits::repeat_min) {
                    repeat.count = shortcut_limits::repeat_min;
                }
            } else {
                repeat.count += value == "count-plus" ? 1 : -1;
            }
            shortcut_editor_->set_repeat(repeat);
        }
    }
    dirty = true;
}
void App::editor_press(unsigned pointer, const std::string& id, double x, double y) {
    if (shortcut_drag_ || editor_question_ != EditorQuestion::None || popup_) {
        return;
    }
    if (id.starts_with("shortcuts-script-")) {
        // A drag from any row scrolls the list; a press without one chooses.
        ShortcutDrag drag;
        drag.pointer = pointer;
        drag.x = drag.px = x;
        drag.y = drag.py = y;
        drag.scripts = true;
        drag.scroll = editor_scripts_scroll_.shown;
        shortcut_drag_ = drag;
        return;
    }
    const bool row = id.starts_with("shortcuts-row-");
    // A step's own row, not its − + or ✕: shortcuts-step-<digits>.
    constexpr std::string_view step_prefix = "shortcuts-step-";
    const bool step = id.starts_with(step_prefix) &&
                      id.find_first_not_of("0123456789", step_prefix.size()) == std::string::npos;
    if (!row && !step && !id.starts_with("shortcuts-button-")) {
        return;
    }
    const auto index = static_cast<std::size_t>(std::stoul(id.substr(row ? 14 : step ? 15 : 17)));
    shortcut_drag_ = ShortcutDrag{pointer, row, index, x, y, x, y, false, false, step, {}, false};
}
bool App::editor_drag_move(unsigned pointer, double x, double y) {
    if (!editor_screen() || !shortcut_drag_ || shortcut_drag_->pointer != pointer) {
        return false;
    }
    auto& d = *shortcut_drag_;
    d.px = x;
    d.py = y;
    if (!d.lifted) {
        // A row or step lifts going up or down, a button going sideways.
        if (std::abs(d.row || d.step || d.scripts ? y - d.y : x - d.x) <= dead_zone) {
            return false;
        }
        d.lifted = true;
        pressed_controls_.erase(pointer); // its release clicks nothing
        if (d.row) {
            shortcut_editor_->select_profile(d.index);
            editor_strip_scroll_ = {};
        } else if (d.button()) {
            shortcut_editor_->select_button(d.index);
        }
    }
    if (d.scripts) {
        // The list follows the laser, as the bar does a drag.
        auto& scroll = editor_scripts_scroll_;
        scroll.shown = scroll.target = std::clamp(d.scroll - (y - d.y), 0.0, editor_scripts_limit());
        scroll.remainder = 0;
        dirty = true;
        return true;
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    if (d.step) {
        const auto* button = shortcut_editor_->selected_button();
        const auto count = button ? button->action.steps.size() : 0;
        const double at = std::floor((y - l.steps_top + editor_steps_scroll_.shown) / step_pitch);
        const auto to = static_cast<std::size_t>(std::clamp(at, 0.0, static_cast<double>(count) - 1));
        if (count > 0 && to != d.index) {
            shortcut_editor_->move_step(d.index, to);
            d.index = to;
        }
    } else if (d.row) {
        const auto count = shortcut_editor_->model().profiles.size();
        const double at = std::floor((y - l.list_top + editor_rows_scroll_.shown) / row_height);
        const auto to = static_cast<std::size_t>(std::clamp(at, 0.0, static_cast<double>(count - 1)));
        if (to != d.index) {
            shortcut_editor_->move_profile(d.index, to);
            d.index = to;
        }
    } else if (const auto* bar = shortcut_editor_->selected_profile()) {
        // Off the strip, a button would be deleted when let go, unless it is
        // over a profile's row: another one that has room lights up and
        // takes it; its own, or a full one, takes nothing.
        const auto& well = l.strip_well;
        d.outside = x < well.x - 20 || x > well.x + well.width + 20 || y < well.y - 20 ||
                    y > well.y + well.height + 20;
        d.over_row.reset();
        d.takes = false;
        if (l.profiles_list.contains(x, y)) {
            const double at = std::floor((y - l.list_top + editor_rows_scroll_.shown) / row_height);
            if (at >= 0 && at < static_cast<double>(shortcut_editor_->model().profiles.size())) {
                const auto row = static_cast<std::size_t>(at);
                d.over_row = row;
                d.takes = row != shortcut_editor_->profile() && shortcut_editor_->can_take(row);
            }
        }
        if (!d.outside) {
            const double at = std::floor((x - l.strip.x + editor_strip_scroll_.shown) / l.pitch);
            const auto last = static_cast<double>(bar->buttons.size() - 1);
            const auto to = static_cast<std::size_t>(std::clamp(at, 0.0, last));
            if (to != d.index) {
                shortcut_editor_->move_button(d.index, to);
                d.index = to;
            }
        }
    }
    dirty = true;
    return true;
}
bool App::editor_drag_up(unsigned pointer) {
    if (!shortcut_drag_ || shortcut_drag_->pointer != pointer) {
        return false;
    }
    const auto d = *std::exchange(shortcut_drag_, std::nullopt);
    if (!d.lifted) {
        return false; // a click: the control acts
    }
    if (shortcut_editor_ && d.button()) {
        if (d.over_row) {
            // On another profile that takes it: Move or Copy. On its own
            // profile's row, or a full one's, it goes back to its place.
            if (d.takes) {
                carry_ = Carry{d.index, *d.over_row};
            }
        } else if (d.outside) {
            shortcut_editor_->delete_button(d.index);
        }
    }
    dirty = true;
    return true;
}
bool App::editing_macro() const {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    return button && button->action.kind == ShortcutAction::Kind::Macro;
}
bool App::editing_keys() const {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    return button && button->action.kind == ShortcutAction::Kind::Keys;
}
bool App::editor_scroll(unsigned pointer, double dx, double dy) {
    const auto position = pointer_positions_.find(pointer);
    if (!editor_screen() || position == pointer_positions_.end() ||
        editor_question_ != EditorQuestion::None) {
        return false;
    }
    const auto [x, y] = position->second;
    const auto body = panel_case_bounds(geometry());
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const double delta = std::isfinite(dx) && std::abs(dx) > std::abs(dy) ? dx : dy;
    bool moved = false;
    if (popup_) {
        // Under the popup only its own recorded combinations scroll.
        if (popup_->kind == PopupKind::Recording &&
            popup_layout(body, l.strip_well, mini_layout_).well.contains(x, y)) {
            moved = step_scroll(popup_chips_scroll_, delta, chip_step, popup_chips_limit());
        }
    } else if (editing_keys() && l.chips.contains(x, y)) {
        moved = step_scroll(editor_chips_scroll_, delta, chip_step, editor_chips_limit());
    } else if (editing_macro() && l.steps_well.contains(x, y)) {
        moved = step_scroll(editor_steps_scroll_, delta, step_pitch, editor_steps_limit());
    } else if (editing_scripts() && l.steps_well.contains(x, y)) {
        moved = step_scroll(editor_scripts_scroll_, delta, step_pitch, editor_scripts_limit());
    } else if (l.profiles_list.contains(x, y)) {
        // The Move/Copy popover hangs off a row: the column scrolling under
        // it closes it, and the button goes back, as a press elsewhere does (R341).
        if (carry_) {
            carry_.reset();
            dirty = true;
        }
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().profiles.size()) * row_height -
                              l.profiles_list.height);
        moved = step_scroll(editor_rows_scroll_, delta, row_height, limit);
    } else if (l.strip_well.contains(x, y) && shortcut_editor_->selected_profile()) {
        const auto count = shortcut_editor_->selected_profile()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        moved = step_scroll(editor_strip_scroll_, delta, l.pitch, limit);
    }
    dirty |= moved;
    return moved;
}
std::optional<ScrollRoom> App::editor_scroll_room(double x, double y) const {
    if (!editor_screen() || editor_question_ != EditorQuestion::None) {
        return {};
    }
    const auto body = panel_case_bounds(geometry());
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto room = [](const BarScroll& scroll, double limit) {
        return limit > 0 ? std::optional{ScrollRoom{scroll.target > 0, scroll.target < limit}}
                         : std::nullopt;
    };
    if (popup_) {
        if (popup_->kind == PopupKind::Recording &&
            popup_layout(body, l.strip_well, mini_layout_).well.contains(x, y)) {
            return room(popup_chips_scroll_, popup_chips_limit());
        }
        return {};
    }
    // The Shortcut view's chip strip lies where a macro's first step row is:
    // each is asked only for its own kind of button (R339).
    if (editing_keys() && l.chips.contains(x, y)) {
        return room(editor_chips_scroll_, editor_chips_limit());
    }
    if (editing_macro() && l.steps_well.contains(x, y)) {
        return room(editor_steps_scroll_, editor_steps_limit());
    }
    if (editing_scripts() && l.steps_well.contains(x, y)) {
        return room(editor_scripts_scroll_, editor_scripts_limit());
    }
    if (l.profiles_list.contains(x, y)) {
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().profiles.size()) * row_height -
                              l.profiles_list.height);
        return limit > 0 ? std::optional{ScrollRoom{editor_rows_scroll_.target > 0,
                                                    editor_rows_scroll_.target < limit}}
                         : std::nullopt;
    }
    if (l.strip_well.contains(x, y) && shortcut_editor_->selected_profile()) {
        const auto count = shortcut_editor_->selected_profile()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        return limit > 0 ? std::optional{ScrollRoom{editor_strip_scroll_.target > 0,
                                                    editor_strip_scroll_.target < limit}}
                         : std::nullopt;
    }
    return {};
}
void App::tick_shortcut_editor(double now) {
    const double elapsed = editor_tick_ < 0 ? 0 : now - editor_tick_;
    editor_tick_ = now;
    if (!shortcut_editor_) {
        return;
    }
    tick_popup(now);
    // A held − or + steps on, as a held key repeats.
    if (value_repeat_) {
        const auto held = pressed_controls_.find(value_repeat_->pointer);
        if (held == pressed_controls_.end() || held->second.id != value_repeat_->id || popup_) {
            value_repeat_.reset();
        } else if (now >= value_repeat_->next) {
            value_repeat_->next = now + .1;
            editor_action(value_repeat_->action);
        }
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const double rows =
        std::max(0.0, static_cast<double>(shortcut_editor_->model().profiles.size()) * row_height -
                          l.profiles_list.height);
    const auto count =
        shortcut_editor_->selected_profile() ? shortcut_editor_->selected_profile()->buttons.size() : 0;
    const double strip = std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
    // Rows, buttons or chips gone shorten the lists: their ends stay in place.
    for (auto [scroll, limit] :
         {std::pair{&editor_rows_scroll_, rows}, std::pair{&editor_strip_scroll_, strip},
          std::pair{&editor_chips_scroll_, editor_chips_limit()},
          std::pair{&editor_steps_scroll_, editor_steps_limit()},
          std::pair{&editor_scripts_scroll_, editor_scripts_limit()},
          std::pair{&popup_chips_scroll_, popup_chips_limit()}}) {
        scroll->target = std::min(scroll->target, limit);
        dirty |= ease_scroll(scroll->shown, scroll->target, elapsed);
    }
    // The scripts folder, read when the list first shows; a read's names
    // arrive here.
    if (editing_scripts() && !scripts_ && !scripts_reading_) {
        read_scripts();
    }
    if (auto list = script_lister_.take()) {
        scripts_ = std::move(list->names);
        scripts_beyond_ = std::move(list->also_present);
        scripts_reading_ = false;
        dirty = true;
    }
}
void App::read_scripts() {
    std::vector<std::string> chosen;
    for (const auto& profile : shortcut_editor_->model().profiles) {
        for (const auto& button : profile.buttons) {
            if (button.action.kind == ShortcutAction::Kind::Run && !button.action.script.empty()) {
                chosen.push_back(button.action.script);
            }
        }
    }
    script_lister_.read(scripts_folder(config_dir()), std::move(chosen));
    scripts_reading_ = true;
}

void App::open_popup(PopupKind kind, std::optional<std::size_t> step, bool new_step) {
    if (!shortcut_editor_ || popup_) {
        return;
    }
    EditorPopup popup;
    popup.kind = kind;
    popup.step = step;
    popup.new_step = new_step;
    if (kind == PopupKind::Name) {
        if (!shortcut_editor_->selected_profile()) {
            return;
        }
        popup.text = shortcut_editor_->selected_profile()->name;
    } else {
        const auto* button = shortcut_editor_->selected_button();
        // A shortcut records its combinations; a macro one of its Keys steps.
        const auto wanted = step || new_step ? ShortcutAction::Kind::Macro : ShortcutAction::Kind::Keys;
        if (!button || (kind == PopupKind::Recording && button->action.kind != wanted) ||
            (kind == PopupKind::Command && button->action.kind != ShortcutAction::Kind::Run)) {
            return;
        }
        if (kind == PopupKind::Label) {
            popup.text = button->label;
        } else if (kind == PopupKind::Command) {
            popup.text = button->action.command;
        }
    }
    popup.cursor = popup.text.size();
    // The US keyboard, whatever is in use: its keys and their characters.
    const auto us = profiles_.layouts.find("en-us-full");
    const auto english = profiles_.languages.find("en-us");
    if (us == profiles_.layouts.end() || english == profiles_.languages.end()) {
        return;
    }
    mini_layout_ = mini_keyboard_layout(us->second, shown_theme().depth);
    if (!mini_keymap_) {
        mini_keymap_ = std::make_unique<LanguageMap>(english->second);
    }
    popup_ = std::move(popup);
    popup_repeat_.reset();
    popup_chips_scroll_ = {};
    chips_drag_.reset();
    shortcut_drag_.reset();
    hovered_.clear();
    pressed_controls_.clear();
    dirty = true;
}
void App::end_popup(bool save) {
    if (!popup_) {
        return;
    }
    const auto popup = *std::exchange(popup_, std::nullopt);
    if (save && shortcut_editor_) {
        switch (popup.kind) {
        case PopupKind::Label:
            shortcut_editor_->set_label(popup.text);
            break;
        case PopupKind::Name:
            shortcut_editor_->rename_profile(popup.text);
            break;
        case PopupKind::Command:
            shortcut_editor_->set_command(popup.text);
            break;
        case PopupKind::Recording:
            // Done replaces: a shortcut's combinations, or a step's. A new
            // step is added only with something in it; an existing one keeps
            // its keys when nothing was recorded, as a step holds at least one.
            if (popup.new_step) {
                MacroStep step;
                step.kind = MacroStep::Kind::Keys;
                step.keys = popup.chords;
                shortcut_editor_->add_step(std::move(step));
                editor_steps_scroll_.target = editor_steps_limit();
            } else if (popup.step) {
                shortcut_editor_->set_step_keys(*popup.step, popup.chords);
            } else {
                shortcut_editor_->set_chords(popup.chords);
                editor_chips_scroll_ = {};
            }
            break;
        }
    }
    popup_repeat_.reset();
    chips_drag_.reset();
    hovered_.clear();
    pressed_controls_.clear();
    dirty = true;
}
void App::popup_key(const std::string& id, unsigned pointer, double now) {
    if (!popup_) {
        return;
    }
    const auto found = std::find_if(mini_layout_.keys.begin(), mini_layout_.keys.end(),
                                    [&](const Key& key) { return key.id == id; });
    if (found == mini_layout_.keys.end()) {
        return;
    }
    const Key& key = *found;
    auto& p = *popup_;
    if (popup_disabled(key, p.kind)) {
        return;
    }
    dirty = true;
    // A modifier latches on a tap and shows lit; a second tap lets it go.
    if (is_modifier(key_code(key.action))) {
        const auto latched = std::find(p.latched.begin(), p.latched.end(), key.action);
        if (latched != p.latched.end()) {
            p.latched.erase(latched);
        } else {
            p.latched.push_back(key.action);
        }
        if (p.kind == PopupKind::Recording) {
            follow_popup_chips(); // the pending chip in view
        }
        return;
    }
    if (p.kind == PopupKind::Recording) {
        popup_record(key.action);
        return;
    }
    popup_text_key(key);
    if (popup_ &&
        (key.action == "Backspace" || key.action == "Delete" || key.action.starts_with("Arrow"))) {
        popup_repeat_ = PopupRepeat{pointer, key.id, now + .5};
    }
}
void App::popup_text_key(const Key& key) {
    auto& p = *popup_;
    const auto& action = key.action;
    const auto latched = [&](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* name) {
            return std::find(p.latched.begin(), p.latched.end(), name) != p.latched.end();
        });
    };
    const bool shift = latched({"ShiftLeft", "ShiftRight"});
    // Ctrl, Alt or Super latched would make a key a shortcut, which types nothing.
    const bool command =
        latched({"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"});
    // Any key but a modifier takes the latches with it.
    const auto modifiers = std::exchange(p.latched, {});
    if (action == "CapsLock") {
        p.caps = !p.caps;
        p.latched = modifiers; // a lock, not a key: the latches wait
        return;
    }
    if (action == "Escape") {
        end_popup(false);
        return;
    }
    const bool label = p.kind == PopupKind::Label;
    // A command stops taking characters at 512 bytes, one line.
    const auto fits = [&](const std::string& text) {
        return label                          ? fits_label(text)
               : p.kind == PopupKind::Command ? fits_command(text)
                                              : fits_profile_name(text);
    };
    auto& text = p.text;
    const auto at = std::min(p.cursor, text.size());
    const auto previous = [&](std::size_t from) {
        return from == 0 ? std::size_t{0}
                         : static_cast<std::size_t>(
                               g_utf8_find_prev_char(text.c_str(), text.c_str() + from) - text.c_str());
    };
    const auto next = [&](std::size_t from) {
        return from >= text.size()
                   ? text.size()
                   : static_cast<std::size_t>(g_utf8_next_char(text.c_str() + from) - text.c_str());
    };
    // The line the caret is on: [start, end), and where it is along it.
    const auto line_start = [&](std::size_t from) {
        const auto found = from == 0 ? std::string::npos : text.rfind('\n', from - 1);
        return found == std::string::npos ? std::size_t{0} : found + 1;
    };
    const auto line_end = [&](std::size_t from) {
        const auto found = text.find('\n', from);
        return found == std::string::npos ? text.size() : found;
    };
    const auto column = [&](std::size_t from) {
        return static_cast<std::size_t>(g_utf8_strlen(text.c_str() + line_start(from),
                                                      static_cast<gssize>(from - line_start(from))));
    };
    const auto along = [&](std::size_t start, std::size_t end, std::size_t characters) {
        auto place = start;
        for (std::size_t i = 0; i < characters && place < end; ++i) {
            place = next(place);
        }
        return place;
    };
    if (action == "Enter" || action == "NumpadEnter") {
        // In a label Shift+Enter starts a second line, once; otherwise saves.
        if (label && shift) {
            auto broken = text;
            broken.insert(at, "\n");
            if (fits(broken)) {
                text = std::move(broken);
                p.cursor = at + 1;
            }
            return;
        }
        end_popup(true);
        return;
    }
    if (action == "Backspace") {
        const auto from = previous(at);
        text.erase(from, at - from);
        p.cursor = from;
    } else if (action == "Delete") {
        text.erase(at, next(at) - at);
    } else if (action == "ArrowLeft") {
        p.cursor = previous(at);
    } else if (action == "ArrowRight") {
        p.cursor = next(at);
    } else if (action == "ArrowUp") {
        // The other line at the same place along it, or the start.
        const auto start = line_start(at);
        p.cursor = start == 0 ? 0 : along(line_start(start - 1), start - 1, column(at));
    } else if (action == "ArrowDown") {
        const auto end = line_end(at);
        p.cursor = end == text.size() ? text.size() : along(end + 1, line_end(end + 1), column(at));
    } else if (action == "Home") {
        p.cursor = line_start(at);
    } else if (action == "End") {
        p.cursor = line_end(at);
    } else if (!command) {
        // The key's US character, with Shift or Caps Lock.
        std::set<int> held;
        if (shift) {
            held.insert(key_code("ShiftLeft"));
        }
        const auto symbol = mini_keymap_->symbol(key, held, p.caps, true);
        const auto codepoint = xkb_keysym_to_utf32(symbol);
        if (!LanguageMap::printable(symbol) || codepoint == 0 || g_unichar_iscntrl(codepoint)) {
            return;
        }
        char bytes[8]{};
        const auto length = g_unichar_to_utf8(codepoint, bytes);
        auto typed = text;
        typed.insert(at, bytes, static_cast<std::size_t>(length));
        if (fits(typed)) {
            text = std::move(typed);
            p.cursor = at + static_cast<std::size_t>(length);
        }
    }
}
void App::popup_record(const std::string& key) {
    auto& p = *popup_;
    const auto modifiers = std::exchange(p.latched, {});
    // A shortcut stops at four combinations (a macro's Keys step at 32).
    if (p.chords.size() >= popup_limit() || (key.empty() && modifiers.empty())) {
        return;
    }
    p.chords.push_back({modifiers, key});
    follow_popup_chips();
    dirty = true;
}
void App::popup_action(const std::string& value) {
    if (!popup_) {
        return;
    }
    auto& p = *popup_;
    if (value == "popup-done") {
        if (p.kind == PopupKind::Recording && !p.latched.empty()) {
            popup_record({}); // pending modifiers record alone
        }
        end_popup(true);
        return;
    }
    if (value == "popup-cancel") {
        end_popup(false);
        return;
    }
    if (value == "popup-remove-last") {
        if (!p.chords.empty()) {
            p.chords.pop_back();
        }
        follow_popup_chips();
    } else if (value == "popup-pending") {
        popup_record({});
    }
    dirty = true;
}
void App::tick_popup(double now) {
    if (!popup_repeat_) {
        return;
    }
    const auto held = pressed_controls_.find(popup_repeat_->pointer);
    if (!popup_ || held == pressed_controls_.end() ||
        held->second.id != std::string(key_prefix) + popup_repeat_->key) {
        popup_repeat_.reset();
        return;
    }
    if (now >= popup_repeat_->next) {
        const auto key = popup_repeat_->key;
        popup_repeat_->next = now + .04; // no catching up, as the address popup's keys
        const auto found = std::find_if(mini_layout_.keys.begin(), mini_layout_.keys.end(),
                                        [&](const Key& k) { return k.id == key; });
        if (found != mini_layout_.keys.end()) {
            popup_text_key(*found);
        }
        dirty = true;
    }
}
void App::append_popup_controls(std::vector<Control>& result, const Rect& body) const {
    const auto& p = *popup_;
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto pl = popup_layout(body, l.strip_well, mini_layout_);
    const auto add = [&](std::string id, std::string value, Control control, int index = 0) {
        control.id = "shortcuts-popup-" + std::move(id);
        control.action = {ControlKind::ShortcutEditor, index, std::move(value)};
        result.push_back(std::move(control));
    };
    // Row 1's buttons, from the right: Done and Cancel. While recording,
    // Remove last is an icon at the well's right end. F13–F24 aren't
    // offered: many targets map them to their own functions (R313); a
    // file's still load and send.
    double right = pl.row.x + pl.row.width;
    const auto take = [&](double width) {
        right -= width;
        const Rect r{right, pl.row.y, width, pl.row.height};
        right -= 8;
        return r;
    };
    add("done", "popup-done", {{}, tr("shortcuts.popup.done"), take(120), true, Icon::Apply});
    add("cancel", "popup-cancel", {{}, tr("button.cancel"), take(104)});
    const bool room = p.chords.size() < popup_limit();
    if (p.kind == PopupKind::Recording) {
        Control remove{{}, {}, popup_remove_last(pl.well), false, Icon::DeleteLeft};
        remove.enabled = !p.chords.empty();
        add("remove-last", "popup-remove-last", remove);
        if (const auto pending = pending_text(p.latched, room); !pending.empty()) {
            const auto strip = popup_chips_strip(pl.well);
            const auto row =
                chip_row(strip, 34, p.chords, pending, popup_chips_scroll_.shown, localization_.fonts());
            Control chip{{}, pending, row.pending};
            chip.clip = strip; // scrolled out of the well, it takes no press
            add("pending", "popup-pending", chip);
        }
    }
    // The keys: each takes its row's whole height, side included, so a
    // press between rows still lands on one.
    const double depth = shown_theme().depth;
    for (const auto& key : mini_layout_.keys) {
        const auto& r = key.bounds;
        Control control{{},
                        key.label,
                        {pl.keys.x + r.x * pl.scale, pl.keys.y + r.y * pl.scale, r.width * pl.scale,
                         (r.height + depth + 1) * pl.scale}};
        control.enabled = !popup_disabled(key, p.kind);
        add("key-" + key.id, "key", control);
    }
}
void App::append_popup_view(ShortcutEditorView& e, const Rect& body) const {
    const auto& p = *popup_;
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto pl = popup_layout(body, l.strip_well, mini_layout_);
    ShortcutPopupView view;
    view.kind = p.kind;
    view.panel = pl.panel;
    view.well = pl.well;
    switch (p.kind) {
    case PopupKind::Label:
        view.title = tr("shortcuts.popup.label");
        view.hint = tr("shortcuts.popup.label-hint");
        break;
    case PopupKind::Name:
        view.title = tr("shortcuts.popup.name");
        view.hint = tr("shortcuts.popup.name-hint");
        break;
    case PopupKind::Command:
        view.title = tr("shortcuts.popup.command");
        view.hint = tr("shortcuts.popup.command-hint");
        break;
    case PopupKind::Recording:
        view.title = tr("shortcuts.popup.recording");
        break;
    }
    view.title_bounds = {pl.row.x + 6, pl.row.y, 300, pl.row.height};
    // The hint runs up to the first of row 1's buttons.
    const double buttons = 120 + 104 + 2 * 8;
    view.hint_bounds = {pl.row.x + 6, pl.row.y, pl.row.width - 6 - buttons - 8, pl.row.height};
    view.text = p.text;
    view.cursor = p.cursor;
    if (p.kind == PopupKind::Recording) {
        const bool room = p.chords.size() < popup_limit();
        const auto strip = popup_chips_strip(pl.well);
        const auto row = chip_row(strip, 34, p.chords, pending_text(p.latched, room),
                                  popup_chips_scroll_.shown, localization_.fonts());
        view.chords = row.chips;
        view.chords_before = popup_chips_scroll_.shown > .5;
        view.chords_after = popup_chips_scroll_.shown < row.limit - .5;
        const double after = row.end + 20, room_left = strip.x + strip.width - after;
        if (!room && room_left >= 160) {
            // Where the chips leave room for it.
            view.full = tr(p.step || p.new_step ? "shortcuts.popup.full-step" : "shortcuts.popup.full");
            view.full_bounds = {after, pl.well.y, room_left, pl.well.height};
        } else if (p.chords.empty() && p.latched.empty()) {
            view.full = tr("shortcuts.popup.recording-hint");
            const auto strip = popup_chips_strip(pl.well);
            view.full_bounds = {strip.x + 6, pl.well.y, strip.width - 6, pl.well.height};
        }
    }
    view.layout = &mini_layout_;
    view.language = &profiles_.languages.at("en-us");
    view.keymap = mini_keymap_.get();
    view.keys = pl.keys;
    view.scale = pl.scale;
    for (const auto& name : p.latched) {
        view.marks.modifiers.insert(key_code(name));
    }
    view.marks.caps = p.caps;
    view.marks.num = true; // the numpad types its digits and signs; Num is drawn disabled
    for (const auto& key : mini_layout_.keys) {
        if (popup_disabled(key, p.kind)) {
            view.disabled.insert(key.id);
        }
    }
    for (const auto& [pointer, pressed] : pressed_controls_) {
        (void)pointer;
        if (pressed.id.starts_with(key_prefix)) {
            view.pressed.insert(pressed.id.substr(key_prefix.size()));
        }
    }
    e.popup = std::move(view);
}

std::size_t App::popup_limit() const {
    // A shortcut stops at four combinations, a macro's Keys step at 32.
    return popup_ && (popup_->step || popup_->new_step) ? shortcut_limits::step_chords
                                                        : shortcut_limits::chords;
}
double App::editor_chips_limit() const {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    if (!button || button->action.kind != ShortcutAction::Kind::Keys) {
        return 0;
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    return chip_row(l.chips, 52, button->action.chords, {}, 0, localization_.fonts()).limit;
}
double App::popup_chips_limit() const {
    if (!popup_ || popup_->kind != PopupKind::Recording || !shortcut_editor_) {
        return 0;
    }
    const auto body = panel_case_bounds(geometry());
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    const auto pl = popup_layout(body, l.strip_well, mini_layout_);
    const auto& p = *popup_;
    const bool room = p.chords.size() < popup_limit();
    return chip_row(popup_chips_strip(pl.well), 34, p.chords, pending_text(p.latched, room), 0,
                    localization_.fonts())
        .limit;
}
void App::follow_popup_chips() {
    popup_chips_scroll_.shown = popup_chips_scroll_.target = popup_chips_limit();
    popup_chips_scroll_.remainder = 0;
}
bool App::chips_press(unsigned pointer, double x, double y) {
    if (!editor_screen() || chips_drag_ || editor_question_ != EditorQuestion::None) {
        return false;
    }
    const auto body = panel_case_bounds(geometry());
    const auto l = editor_layout(body, bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().profiles.size());
    if (popup_) {
        const auto well = popup_layout(body, l.strip_well, mini_layout_).well;
        if (popup_->kind != PopupKind::Recording || !well.contains(x, y) ||
            popup_remove_last(well).contains(x, y)) {
            return false;
        }
        chips_drag_ = ChipsDrag{pointer, true, x, popup_chips_scroll_.shown};
        return true;
    }
    if (!l.chips.contains(x, y) || editor_chips_limit() <= 0) {
        return false;
    }
    chips_drag_ = ChipsDrag{pointer, false, x, editor_chips_scroll_.shown};
    return true;
}
bool App::chips_drag_move(unsigned pointer, double x) {
    if (!chips_drag_ || chips_drag_->pointer != pointer) {
        return false;
    }
    auto& scroll = chips_drag_->popup ? popup_chips_scroll_ : editor_chips_scroll_;
    const double limit = chips_drag_->popup ? popup_chips_limit() : editor_chips_limit();
    const double to = std::clamp(chips_drag_->scroll - (x - chips_drag_->x), 0.0, limit);
    if (to != scroll.shown || to != scroll.target) {
        scroll.shown = scroll.target = to;
        scroll.remainder = 0;
        dirty = true;
    }
    return true;
}

void App::open_field() {
    const auto* button = shortcut_editor_ ? shortcut_editor_->selected_button() : nullptr;
    if (!button) {
        return;
    }
    ShortcutField field;
    field.text = button->label;
    field.cursor = field.text.size();
    // Nothing held, latched or composing carries into the field.
    release_input(true);
    keyboard_.cancel_all();
    field_ = std::move(field);
    // Typed text, composed, swiped or flicked, goes into the field.
    text_output_ = &field_output_;
    hovered_.clear();
    pressed_controls_.clear();
    dirty = true;
}
void App::end_field(bool save) {
    if (!field_) {
        return;
    }
    // A held tap or flick types as if let go, a reading or a swiped word
    // still pending goes in, all into the field; a cancel drops them.
    release_input(!save);
    if (save) {
        commit_composition();
        finish_swipe(true);
    }
    const auto field = *field_;
    if (save && shortcut_editor_) {
        shortcut_editor_->set_label(field.text);
    }
    discard_swipe();
    composition_.cancel();
    if (cjk_) {
        cjk_->cancel();
    }
    keymap_->cancel_compose();
    // Latches and locks made for the field stay with it.
    keyboard_.cancel_all();
    field_.reset();
    text_output_ = options_.text_output ? options_.text_output : &output_;
    lose_word();
    hovered_.clear();
    pressed_controls_.clear();
    dirty = true;
}
bool App::field_insert(const std::string& text) {
    if (!field_typing()) {
        return false;
    }
    auto& f = *field_;
    auto next = f.text;
    next.insert(std::min(f.cursor, next.size()), text);
    if (!fits_label(next)) {
        return false;
    }
    f.text = std::move(next);
    f.cursor = std::min(f.cursor, f.text.size()) + text.size();
    dirty = true;
    return true;
}
bool App::field_replace(std::size_t delete_before, const std::string& text) {
    if (!field_typing()) {
        return false;
    }
    auto& f = *field_;
    std::size_t start = std::min(f.cursor, f.text.size());
    for (; delete_before > 0 && start > 0; --delete_before) {
        start = static_cast<std::size_t>(g_utf8_find_prev_char(f.text.c_str(), f.text.c_str() + start) -
                                         f.text.c_str());
    }
    auto next = f.text;
    next.replace(start, std::min(f.cursor, f.text.size()) - start, text);
    if (!fits_label(next)) {
        return false;
    }
    f.text = std::move(next);
    f.cursor = start + text.size();
    dirty = true;
    return true;
}
bool App::field_down(unsigned pointer, const Key& key, double x, double y, double now, bool gap) {
    if (keyboard_.pointer_pressed(pointer)) {
        return false;
    }
    // As in dictation's editor: another hand's flick or tap types first.
    if (flick_track_ && flick_track_->pointer != pointer) {
        finish_flick();
    }
    if (swipe_track_ && swipe_track_->pointer != pointer && !swipe_track_->swiping) {
        const auto track = std::move(*swipe_track_);
        swipe_track_.reset();
        type_swipe_tap(track);
    }
    if (key_flick_ && key_flick_->pointer != pointer) {
        finish_key_flick();
    }
    if (swipe_track_) {
        return false; // one pointer swipes at a time
    }
    if (const auto held = keyboard_.modifiers(); swipes(key, held)) {
        return swipe_down(pointer, key, x, y, now, held, gap);
    }
    if (gap) {
        return false;
    }
    finish_swipe(true);
    if (key.action_kind != ActionKind::Key ||
        !((key.sticky && is_modifier(key_code(key.action))) || key.action == "CapsLock")) {
        discard_swipe();
        swipe_space_ = false;
    }
    // The mic, the emoji key and Cut, Copy and Paste wait while a field is typed.
    if (key.action_kind == ActionKind::Dictation || key.action_kind == ActionKind::Emoji ||
        key.action_kind == ActionKind::Shortcut) {
        return false;
    }
    if (key.action_kind == ActionKind::Flick) {
        return flick_down(pointer, key, x, y, now);
    }
    if (const auto held = keyboard_.modifiers(); key_flick_key(key, held)) {
        return key_flick_down(pointer, key, x, y, now, held);
    }
    const auto modifiers = keyboard_.modifiers();
    const auto symbol = key.action_kind == ActionKind::Key
                            ? keymap_->symbol(key, modifiers, keyboard_.caps(), keyboard_.num())
                            : XKB_KEY_NoSymbol;
    const bool ime = japanese_key(key, false, modifiers) || cjk_key(key, false, modifiers);
    // Every key is local here: it shows as pressed and latches, but sends nothing.
    if (!keyboard_.down(pointer, key, now, true, ime ? ime_press_code(key) : 0, !latin_toggle(key))) {
        return false;
    }
    dirty = true;
    if (ime) {
        try {
            japanese_key(key, true, modifiers);
            cjk_key(key, true, modifiers);
            start_reading_repeat(pointer, key, modifiers, now);
        } catch (const std::exception& error) {
            status_ = tr(error);
        }
        return true;
    }
    if (key.action_kind != ActionKind::Key || is_modifier(key_code(key.action)) ||
        key.action == "CapsLock" || key.action == "NumLock") {
        return true;
    }
    if (key.action == "Escape") {
        end_field(false);
        return true;
    }
    // Any other key ends a reading first, so the text keeps its order.
    if (!commit_composition()) {
        return true;
    }
    auto& f = *field_;
    const auto& text = f.text;
    const auto at = std::min(f.cursor, text.size());
    const auto previous = [&] {
        return at == 0 ? std::size_t{0}
                       : static_cast<std::size_t>(
                             g_utf8_find_prev_char(text.c_str(), text.c_str() + at) - text.c_str());
    };
    const auto next = [&] {
        return at >= text.size()
                   ? text.size()
                   : static_cast<std::size_t>(g_utf8_next_char(text.c_str() + at) - text.c_str());
    };
    const bool shift = modifiers.contains(KEY_LEFTSHIFT) || modifiers.contains(KEY_RIGHTSHIFT);
    if (key.action == "Enter" || key.action == "NumpadEnter") {
        if (shift) {
            field_insert("\n"); // a second line, once
        } else {
            end_field(true);
        }
        return true;
    }
    if (key.action == "Backspace") {
        const auto from = previous();
        f.text.erase(from, at - from);
        f.cursor = from;
    } else if (key.action == "Delete") {
        f.text.erase(at, next() - at);
    } else if (key.action == "ArrowLeft") {
        f.cursor = previous();
    } else if (key.action == "ArrowRight") {
        f.cursor = next();
    } else if (key.action == "Home") {
        f.cursor = 0;
    } else if (key.action == "End") {
        f.cursor = text.size();
    } else {
        const auto codepoint = xkb_keysym_to_utf32(symbol);
        if (!LanguageMap::printable(symbol) || (codepoint != 0 && g_unichar_iscntrl(codepoint))) {
            return true;
        }
        std::string typed;
        try {
            typed = keymap_->compose(symbol);
        } catch (const std::exception& error) {
            status_ = tr(error);
            return true;
        }
        if (!typed.empty()) {
            text_output_->text_press(pointer, typed, now);
        }
    }
    return true;
}
void App::field_action(const ControlAction& command) {
    if (!field_) {
        return;
    }
    if (command.value == "done") {
        end_field(true);
    } else if (command.value == "cancel") {
        end_field(false);
    }
    dirty = true;
}
void App::append_field_controls(std::vector<Control>& result, const Rect& body) const {
    double right = body.x + body.width - 18;
    const auto take = [&](double width) {
        right -= width;
        const Rect r{right, 12, width, 42};
        right -= 10;
        return r;
    };
    Control done{"field-done", tr("shortcuts.field.done"), take(150), true};
    done.action = {ControlKind::FieldAction, 0, "done"};
    result.push_back(std::move(done));
    Control cancel{"field-cancel", tr("button.cancel"), take(130)};
    cancel.action = {ControlKind::FieldAction, 0, "cancel"};
    result.push_back(std::move(cancel));
}
void App::append_field_view(PanelView& v) const {
    const auto body = panel_case_bounds(v);
    ShortcutFieldView view;
    view.strip = {body.x + 18, 12, body.width - 36, 42};
    double right = body.x + body.width - 18;
    for (const auto& c : v.controls) {
        if (c.action.kind == ControlKind::FieldAction && c.bounds.y < 50) {
            right = std::min(right, c.bounds.x);
        }
    }
    const double x = body.x + 18 + 150;
    view.title = tr("shortcuts.field.label");
    view.hint = tr("shortcuts.field.label-hint");
    // A narrow keyboard drops the hint.
    if (right - 16 - x >= 220) {
        view.hint_bounds = {x, 12, right - 16 - x, 42};
    }
    v.shortcut_field = std::move(view);
    // The mic, the emoji key, and Cut, Copy and Paste wait in a text field.
    for (const auto& key : v.layout->keys) {
        if (key.action_kind == ActionKind::Dictation || key.action_kind == ActionKind::Emoji ||
            key.action_kind == ActionKind::Shortcut) {
            v.disabled_keys.insert(key.id);
        }
    }
}
void App::append_field_bar(PanelView& v) const {
    if (!shortcut_editor_ || !shortcut_editor_->selected_profile()) {
        return;
    }
    const auto& bar = *shortcut_editor_->selected_profile();
    const auto count = bar.buttons.size();
    const auto g = bar_geometry(count);
    const auto selected = shortcut_editor_->button().value_or(0);
    // The button being edited in view, near the middle.
    const double scroll =
        std::clamp(static_cast<double>(selected) * g.pitch - (g.strip.width - g.button_width) / 2, 0.0,
                   g.scroll_limit);
    ShortcutBarView view;
    view.plate = g.plate;
    view.strip = g.strip;
    view.scale = g.button_width / 69.5;
    view.slots = count == 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto r = bar_button_bounds(g, count, i, scroll);
        if (r.x + r.width <= g.strip.x || r.x >= g.strip.x + g.strip.width) {
            continue;
        }
        ShortcutBarView::Button shown;
        shown.label = bar.buttons[i].label;
        shown.bounds = r;
        shown.running = i == selected && shortcut_editor_->button(); // lit, as in the editor's strip
        shown.run = bar.buttons[i].action.kind == ShortcutAction::Kind::Run;
        if (i == selected) {
            view.field = view.buttons.size();
            view.field_text = field_->text;
            view.field_cursor = field_->cursor;
        }
        view.buttons.push_back(std::move(shown));
    }
    view.more_before = scroll > .5;
    view.more_after = scroll < g.scroll_limit - .5;
    view.picker = g.picker;
    if (view.field) {
        v.bar_tooltip = tr("shortcuts.field.label-tooltip");
        v.bar_tooltip_at = view.buttons[*view.field].bounds;
    }
    v.shortcut_bar = std::move(view);
}
} // namespace framekeyboard
