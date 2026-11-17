// The shortcut editor (docs/shortcut-bar-design.md, Settings and the editor):
// a screen of its own over the case, on its own copy of the bars until ✓,
// with a popup over it where labels and bar names are typed and keys
// recorded on a mini keyboard of its own (Typing and recording). The label
// field typed on the real keyboard waits for renaming in place.
#include "framekeyboard/app.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
// A chip's width for its text, at the size it is drawn; text that doesn't
// quite fit shrinks (interface_label).
double chip_width(const std::string& text, double size) {
    return 28 + size * .56 * static_cast<double>(g_utf8_strlen(text.c_str(), -1));
}
// Where the editor's parts go on a case of `body`.
struct EditorLayout {
    bool wide{};
    double column_width{};
    Rect column, bars_list, new_bar, used;
    double list_top{}, divider{}, right{}, left{};
    std::size_t visible_rows{};
    Rect strip_well, strip, label, explanation;
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
    l.bars_list = {x, l.list_top, l.column_width, shown * row_height};
    l.new_bar = {x, l.list_top + shown * row_height, l.column_width, row_height};
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
    l.explanation = {l.left, 372, l.right - l.left, 60};
    return l;
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
// strip, centered on the case, 396 tall whatever the width. Row 1 holds what
// is edited and the buttons, row 2 the recessed strip, and the keys take the
// rest at a scale that fits the mini layout's height into it.
struct PopupLayout {
    Rect panel, row, well, keys;
    double scale{};
};
PopupLayout popup_layout(const Rect& body, const Rect& strip_well, const Layout& mini) {
    constexpr double height = 396, pad = 10, margin = 12, row = 40, well = 44;
    constexpr double keys_top = pad + row + 8 + well + 8; // 110
    PopupLayout l;
    l.scale = mini.height > 0 ? (height - keys_top - pad) / mini.height : 1;
    const double width = mini.width * l.scale + 2 * margin;
    l.panel = {body.x + (body.width - width) / 2, strip_well.y + strip_well.height + 8, width, height};
    l.row = {l.panel.x + margin, l.panel.y + pad, width - 2 * margin, row};
    l.well = {l.panel.x + margin, l.panel.y + pad + row + 8, width - 2 * margin, well};
    l.keys = {l.panel.x + margin, l.panel.y + keys_top, mini.width * l.scale, mini.height * l.scale};
    return l;
}
// The chips of the combinations recorded so far, and where the pending
// modifiers' chip goes after them; scrolled so the newest shows.
struct ChipRow {
    std::vector<ShortcutChip> chips;
    Rect pending;
    double end{};
};
ChipRow chip_row(const Rect& well, const std::vector<ShortcutChord>& chords,
                 const std::string& pending) {
    constexpr double gap = 28, height = 34;
    ChipRow row;
    double x = well.x + 10;
    for (const auto& chord : chords) {
        const auto text = chord_text(chord);
        row.chips.push_back(
            {text, {x, well.y + (well.height - height) / 2, chip_width(text, 20), height}});
        x += chip_width(text, 20) + gap;
    }
    if (!pending.empty()) {
        row.pending = {x, well.y + (well.height - height) / 2, chip_width(pending, 20), height};
        x += row.pending.width + gap;
    }
    row.end = x - gap;
    // The newest in view: everything moves left by what overflows.
    const double over = row.end - (well.x + well.width - 10);
    if (over > 0) {
        for (auto& chip : row.chips) {
            chip.bounds.x -= over;
        }
        row.pending.x -= over;
        row.end -= over;
    }
    return row;
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
    shortcut_editor_.emplace(shortcuts_, bar_choice_.shown());
    editor_question_ = EditorQuestion::None;
    editor_rows_scroll_ = editor_strip_scroll_ = {};
    shortcut_drag_.reset();
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
        const auto& model = shortcut_editor_->model();
        // The editor never builds a model past a limit; checked all the same.
        if (const auto problem = validate_shortcuts(model)) {
            status_ = tr("status.shortcuts-not-saved", {{"error", *problem}});
            dirty = true;
            return;
        }
        shortcuts_ = model;
        // The worker keeps an unreadable file as shortcuts.json.bad; the
        // note goes once its result says the save worked.
        save_shortcuts();
        apply_bar_changes(bar_choice_.model_changed(shortcuts_));
    }
    shortcut_editor_.reset();
    editor_question_ = EditorQuestion::None;
    shortcut_drag_.reset();
    hovered_.clear();
    pressed_controls_.clear();
    update_settings_ui(); // the Debug card's sample button follows the bars
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
    const auto l = editor_layout(body, bar, shown_theme().depth, model.bars.size());
    add("shortcuts-back", "back", {{}, {}, {body.x + 18, 12, 60, 42}, false, Icon::Back});
    add("shortcuts-save", "save", {{}, {}, {body.x + 88, 12, 60, 42}, false, Icon::Apply});
    const bool lifted = shortcut_drag_ && shortcut_drag_->lifted;
    // The bars, a row each; the selected one has rename and delete.
    for (std::size_t i = 0; i < model.bars.size(); ++i) {
        const Rect r{l.column.x,
                     l.list_top + static_cast<double>(i) * row_height - editor_rows_scroll_.shown,
                     l.column_width, row_height};
        if (r.y + r.height <= l.bars_list.y || r.y >= l.bars_list.y + l.bars_list.height ||
            (lifted && shortcut_drag_->row && shortcut_drag_->index == i)) {
            continue;
        }
        Control row{
            {}, model.bars[i].name, {r.x, r.y + 2, r.width, r.height - 4}, shortcut_editor_->bar() == i};
        row.clip = l.bars_list;
        add("shortcuts-row-" + std::to_string(i), "row", row, static_cast<int>(i));
        if (shortcut_editor_->bar() == i) {
            Control rename{{}, {}, {r.x + r.width - 92, r.y, 44, r.height}, false, Icon::Pencil};
            rename.clip = l.bars_list;
            add("shortcuts-rename", "rename", rename);
            Control remove{{}, {}, {r.x + r.width - 48, r.y, 44, r.height}, false, Icon::Clear};
            remove.clip = l.bars_list;
            add("shortcuts-delete", "delete", remove);
        }
    }
    Control new_bar{{}, tr("shortcuts.editor.new-bar"), l.new_bar};
    new_bar.enabled = shortcut_editor_->can_add_bar();
    add("shortcuts-new-bar", "new-bar", new_bar);
    // Used for: each tie's ✕.
    double x = l.used.x, y = l.used.y + 34;
    for (const auto index : shortcut_editor_->ties()) {
        const double width = std::min(l.used.width, chip_width(model.apps[index].name, 19) + 32);
        if (x + width > l.used.x + l.used.width && x > l.used.x) {
            x = l.used.x;
            y += 48;
        }
        add("shortcuts-tie-" + std::to_string(index), "tie", {{}, {}, {x + width - 38, y, 34, 40}},
            static_cast<int>(index));
        x += width + 8;
    }
    // The strip: the selected bar's buttons, then +.
    if (const auto* selected = shortcut_editor_->selected_bar()) {
        const std::size_t count = selected->buttons.size();
        for (std::size_t i = 0; i <= count; ++i) {
            const Rect r{l.strip.x + static_cast<double>(i) * l.pitch - editor_strip_scroll_.shown,
                         l.button_top, l.button_width, l.button_height};
            if (r.x + r.width <= l.strip.x || r.x >= l.strip.x + l.strip.width ||
                (i < count && lifted && !shortcut_drag_->row && shortcut_drag_->index == i)) {
                continue;
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
            // Macros and Run come later: only a shortcut's own segment shows
            // as usable, and a button keeps the action it has.
            Control segment{{},
                            tr(kinds[i].first),
                            {l.segments_x + 110 * static_cast<double>(i), l.label.y, 110, 56},
                            kind == kinds[i].second};
            segment.style = ControlStyle::Segment;
            segment.segments = 3;
            segment.enabled = i == 0 && kind == ShortcutAction::Kind::Keys;
            add("shortcuts-kind-" + std::to_string(i), "kind", segment, static_cast<int>(i));
        }
        if (kind == ShortcutAction::Kind::Keys) {
            double after = l.left + 100;
            for (const auto& chord : button->action.chords) {
                after += chip_width(chord_text(chord), 20) + 28;
            }
            if (!button->action.chords.empty()) {
                after -= 12; // the last chip's gap holds no chevron
            }
            add("shortcuts-record", "record",
                {{}, tr("shortcuts.editor.record"), {after, l.keys_y, 160, 52}});
            Control clear{{}, {}, {after + 172, l.keys_y, 60, 52}, false, Icon::Clear};
            clear.enabled = !button->action.chords.empty();
            add("shortcuts-clear", "clear", clear);
        }
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
void App::append_shortcut_editor(PanelView& v) const {
    const auto body = panel_case_bounds(v);
    const auto& model = shortcut_editor_->model();
    const auto bar = bar_geometry(0);
    const auto l = editor_layout(body, bar, shown_theme().depth, model.bars.size());
    auto& e = *v.shortcut_editor;
    e.wide = l.wide;
    e.title = tr("shortcuts.editor.title");
    if (!shortcuts_invalid_.empty()) {
        e.notice = tr("shortcuts.editor.invalid", {{"reason", shortcuts_invalid_}});
    }
    e.bars_caption = tr("shortcuts.editor.bars");
    e.column = l.column;
    e.bars_list = l.bars_list;
    e.divider = l.divider;
    e.scale = bar.button_width / 69.5;
    const double rows_limit =
        std::max(0.0, static_cast<double>(model.bars.size()) * row_height - l.bars_list.height);
    if (rows_limit > 0) {
        const double length =
            l.bars_list.height * l.bars_list.height / (l.bars_list.height + rows_limit);
        e.bars_thumb =
            Rect{l.column.x + l.column.width + 4,
                 l.bars_list.y + editor_rows_scroll_.shown / rows_limit * (l.bars_list.height - length),
                 3, length};
    }
    if (shortcut_editor_->selected_bar()) {
        e.used = l.used;
        e.used_caption = tr("shortcuts.editor.used");
        double x = l.used.x, y = l.used.y + 34;
        for (const auto index : shortcut_editor_->ties()) {
            const auto& name = model.apps[index].name;
            const double width = std::min(l.used.width, chip_width(name, 19) + 32);
            if (x + width > l.used.x + l.used.width && x > l.used.x) {
                x = l.used.x;
                y += 48;
            }
            e.ties.push_back({name.empty() ? model.apps[index].app.id : name, {x, y, width, 40}});
            x += width + 8;
        }
        e.used_hint = tr(e.ties.empty() ? "shortcuts.editor.used-none" : "shortcuts.editor.used-hint");
        e.strip_well = l.strip_well;
        e.strip = l.strip;
        const auto count = shortcut_editor_->selected_bar()->buttons.size();
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
        e.empty_hint = tr("shortcuts.editor.no-bars");
    }
    if (const auto* button = shortcut_editor_->selected_button()) {
        e.label_caption = tr("shortcuts.editor.label");
        e.action_caption = tr("shortcuts.editor.action");
        e.explanation_bounds = l.explanation;
        switch (button->action.kind) {
        case ShortcutAction::Kind::Keys: {
            e.keys_caption = tr("shortcuts.editor.keys");
            double x = l.left + 100;
            std::string sent;
            for (const auto& chord : button->action.chords) {
                const auto text = chord_text(chord);
                e.chords.push_back({text, {x, l.keys_y, chip_width(text, 20), 52}});
                x += chip_width(text, 20) + 28;
                sent += (sent.empty() ? "" : tr("shortcuts.editor.then")) + text;
            }
            e.explanation.push_back(sent.empty() ? tr("shortcuts.editor.no-keys")
                                                 : tr("shortcuts.editor.sends", {{"keys", sent}}));
            e.explanation.push_back(tr("shortcuts.editor.record-hint"));
            break;
        }
        case ShortcutAction::Kind::Macro:
            e.explanation.push_back(tr("shortcuts.editor.macro-later"));
            break;
        case ShortcutAction::Kind::Run:
            e.explanation.push_back(tr("shortcuts.editor.run-later"));
            break;
        }
    }
    // What a drag carries, where the laser holds it.
    if (shortcut_drag_ && shortcut_drag_->lifted) {
        const auto& d = *shortcut_drag_;
        if (d.row && d.index < model.bars.size()) {
            e.lifted_row =
                ShortcutChip{model.bars[d.index].name,
                             {l.column.x, d.py - row_height / 2, l.column_width, row_height - 4}};
        } else if (const auto* selected = shortcut_editor_->selected_bar();
                   !d.row && selected && d.index < selected->buttons.size()) {
            e.lifted_button = ShortcutChip{selected->buttons[d.index].label,
                                           {d.px - l.button_width / 2, d.py - l.button_height / 2,
                                            l.button_width, l.button_height}};
            e.lifted_removing = d.outside;
        }
    }
    if (popup_) {
        append_popup_view(e, body);
    }
    if (editor_question_ == EditorQuestion::Leave) {
        v.confirm = PanelView::ConfirmView{confirm_popup_bounds, tr("shortcuts.question.leave-title"),
                                           tr("shortcuts.question.leave-body")};
    } else if (editor_question_ == EditorQuestion::DeleteBar && shortcut_editor_->selected_bar()) {
        const auto count = static_cast<long long>(shortcut_editor_->selected_bar()->buttons.size());
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
        popup_action(value, command.index);
        return;
    }
    if (value == "question-cancel" || value == "question-accept") {
        const auto question = std::exchange(editor_question_, EditorQuestion::None);
        const bool accept = value == "question-accept";
        if (question == EditorQuestion::Leave) {
            close_shortcut_editor(accept); // Discard or Save
            return;
        }
        if (question == EditorQuestion::DeleteBar && accept) {
            shortcut_editor_->delete_bar();
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
        shortcut_editor_->select_bar(index);
        editor_strip_scroll_ = {};
    } else if (value == "rename") {
        open_popup(PopupKind::Name);
    } else if (value == "delete") {
        if (const auto* bar = shortcut_editor_->selected_bar(); bar && !bar->buttons.empty()) {
            editor_question_ = EditorQuestion::DeleteBar; // its buttons would go too
        } else {
            shortcut_editor_->delete_bar();
            editor_strip_scroll_ = {};
        }
    } else if (value == "new-bar") {
        const auto number = static_cast<long long>(shortcut_editor_->model().bars.size() + 1);
        shortcut_editor_->add_bar(tr("shortcuts.editor.bar-name", {{"number", number}}));
        editor_strip_scroll_ = {};
        // The new row in view, then its name to type.
        const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                     shortcut_editor_->model().bars.size());
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().bars.size()) * row_height -
                              l.bars_list.height);
        editor_rows_scroll_.shown = editor_rows_scroll_.target = limit;
        open_popup(PopupKind::Name);
    } else if (value == "tie") {
        shortcut_editor_->remove_tie(index);
    } else if (value == "button") {
        shortcut_editor_->select_button(index);
    } else if (value == "add") {
        shortcut_editor_->add_button();
        // The new button in view, at the strip's end.
        const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                     shortcut_editor_->model().bars.size());
        const auto count = shortcut_editor_->selected_bar()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        editor_strip_scroll_.target = limit;
    } else if (value == "label") {
        open_popup(PopupKind::Label);
    } else if (value == "record") {
        open_popup(PopupKind::Recording);
    } else if (value == "clear") {
        shortcut_editor_->set_chords({});
    }
    dirty = true;
}
void App::editor_press(unsigned pointer, const std::string& id, double x, double y) {
    if (shortcut_drag_ || editor_question_ != EditorQuestion::None || popup_) {
        return;
    }
    const bool row = id.starts_with("shortcuts-row-");
    if (!row && !id.starts_with("shortcuts-button-")) {
        return;
    }
    const auto index = static_cast<std::size_t>(std::stoul(id.substr(row ? 14 : 17)));
    shortcut_drag_ = ShortcutDrag{pointer, row, index, x, y, x, y, false, false};
}
bool App::editor_drag_move(unsigned pointer, double x, double y) {
    if (!editor_screen() || !shortcut_drag_ || shortcut_drag_->pointer != pointer) {
        return false;
    }
    auto& d = *shortcut_drag_;
    d.px = x;
    d.py = y;
    if (!d.lifted) {
        // A row lifts going up or down, a button going sideways.
        if (std::abs(d.row ? y - d.y : x - d.x) <= dead_zone) {
            return false;
        }
        d.lifted = true;
        pressed_controls_.erase(pointer); // its release clicks nothing
        if (d.row) {
            shortcut_editor_->select_bar(d.index);
            editor_strip_scroll_ = {};
        } else {
            shortcut_editor_->select_button(d.index);
        }
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().bars.size());
    if (d.row) {
        const auto count = shortcut_editor_->model().bars.size();
        const double at = std::floor((y - l.list_top + editor_rows_scroll_.shown) / row_height);
        const auto to = static_cast<std::size_t>(std::clamp(at, 0.0, static_cast<double>(count - 1)));
        if (to != d.index) {
            shortcut_editor_->move_bar(d.index, to);
            d.index = to;
        }
    } else if (const auto* bar = shortcut_editor_->selected_bar()) {
        // Off the strip, a button would be deleted when let go.
        const auto& well = l.strip_well;
        d.outside = x < well.x - 20 || x > well.x + well.width + 20 || y < well.y - 20 ||
                    y > well.y + well.height + 20;
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
    if (shortcut_editor_ && !d.row && d.outside) {
        shortcut_editor_->delete_button(d.index);
    }
    dirty = true;
    return true;
}
bool App::editor_scroll(unsigned pointer, double dx, double dy) {
    const auto position = pointer_positions_.find(pointer);
    if (!editor_screen() || position == pointer_positions_.end() ||
        editor_question_ != EditorQuestion::None || popup_) {
        return false;
    }
    const auto [x, y] = position->second;
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().bars.size());
    const double delta = std::isfinite(dx) && std::abs(dx) > std::abs(dy) ? dx : dy;
    bool moved = false;
    if (l.bars_list.contains(x, y)) {
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().bars.size()) * row_height -
                              l.bars_list.height);
        moved = step_scroll(editor_rows_scroll_, delta, row_height, limit);
    } else if (l.strip_well.contains(x, y) && shortcut_editor_->selected_bar()) {
        const auto count = shortcut_editor_->selected_bar()->buttons.size();
        const double limit =
            std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
        moved = step_scroll(editor_strip_scroll_, delta, l.pitch, limit);
    }
    dirty |= moved;
    return moved;
}
std::optional<ScrollRoom> App::editor_scroll_room(double x, double y) const {
    if (!editor_screen() || editor_question_ != EditorQuestion::None || popup_) {
        return {};
    }
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().bars.size());
    if (l.bars_list.contains(x, y)) {
        const double limit =
            std::max(0.0, static_cast<double>(shortcut_editor_->model().bars.size()) * row_height -
                              l.bars_list.height);
        return limit > 0 ? std::optional{ScrollRoom{editor_rows_scroll_.target > 0,
                                                    editor_rows_scroll_.target < limit}}
                         : std::nullopt;
    }
    if (l.strip_well.contains(x, y) && shortcut_editor_->selected_bar()) {
        const auto count = shortcut_editor_->selected_bar()->buttons.size();
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
    const auto l = editor_layout(panel_case_bounds(geometry()), bar_geometry(0), shown_theme().depth,
                                 shortcut_editor_->model().bars.size());
    const double rows =
        std::max(0.0, static_cast<double>(shortcut_editor_->model().bars.size()) * row_height -
                          l.bars_list.height);
    const auto count =
        shortcut_editor_->selected_bar() ? shortcut_editor_->selected_bar()->buttons.size() : 0;
    const double strip = std::max(0.0, static_cast<double>(count + 1) * l.pitch - l.gap - l.strip.width);
    // Rows or buttons gone shorten the lists: their ends stay in place.
    for (auto [scroll, limit] :
         {std::pair{&editor_rows_scroll_, rows}, std::pair{&editor_strip_scroll_, strip}}) {
        scroll->target = std::min(scroll->target, limit);
        dirty |= ease_scroll(scroll->shown, scroll->target, elapsed);
    }
}

void App::open_popup(PopupKind kind) {
    if (!shortcut_editor_ || popup_) {
        return;
    }
    EditorPopup popup;
    popup.kind = kind;
    if (kind == PopupKind::Name) {
        if (!shortcut_editor_->selected_bar()) {
            return;
        }
        popup.text = shortcut_editor_->selected_bar()->name;
    } else {
        const auto* button = shortcut_editor_->selected_button();
        if (!button ||
            (kind == PopupKind::Recording && button->action.kind != ShortcutAction::Kind::Keys)) {
            return;
        }
        if (kind == PopupKind::Label) {
            popup.text = button->label;
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
            shortcut_editor_->rename_bar(popup.text);
            break;
        case PopupKind::Recording:
            shortcut_editor_->set_chords(popup.chords); // Done replaces
            break;
        }
    }
    popup_repeat_.reset();
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
    p.fkeys = false;
    dirty = true;
    // A modifier latches on a tap and shows lit; a second tap lets it go.
    if (is_modifier(key_code(key.action))) {
        const auto latched = std::find(p.latched.begin(), p.latched.end(), key.action);
        if (latched != p.latched.end()) {
            p.latched.erase(latched);
        } else {
            p.latched.push_back(key.action);
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
    const auto fits = [&](const std::string& text) {
        return label ? fits_label(text) : fits_bar_name(text);
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
    if (p.chords.size() >= shortcut_limits::chords || (key.empty() && modifiers.empty())) {
        return;
    }
    p.chords.push_back({modifiers, key});
    dirty = true;
}
void App::popup_action(const std::string& value, int index) {
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
        p.fkeys = false;
    } else if (value == "popup-fkeys") {
        p.fkeys = !p.fkeys;
    } else if (value == "popup-fkey") {
        p.fkeys = false;
        popup_record("F" + std::to_string(index));
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
    const auto l =
        editor_layout(body, bar_geometry(0), shown_theme().depth, shortcut_editor_->model().bars.size());
    const auto pl = popup_layout(body, l.strip_well, mini_layout_);
    const auto add = [&](std::string id, std::string value, Control control, int index = 0) {
        control.id = "shortcuts-popup-" + std::move(id);
        control.action = {ControlKind::ShortcutEditor, index, std::move(value)};
        result.push_back(std::move(control));
    };
    // Row 1's buttons, from the right: Done, Cancel, and while recording
    // Remove last and F13–F24.
    double right = pl.row.x + pl.row.width;
    const auto take = [&](double width) {
        right -= width;
        const Rect r{right, pl.row.y, width, pl.row.height};
        right -= 8;
        return r;
    };
    add("done", "popup-done", {{}, tr("shortcuts.popup.done"), take(120), true, Icon::Apply});
    add("cancel", "popup-cancel", {{}, tr("button.cancel"), take(104)});
    const bool room = p.chords.size() < shortcut_limits::chords;
    if (p.kind == PopupKind::Recording) {
        Control remove{{}, tr("shortcuts.popup.remove-last"), take(146)};
        remove.enabled = !p.chords.empty();
        add("remove-last", "popup-remove-last", remove);
        Control fkeys{{}, tr("shortcuts.popup.fkeys"), take(130), p.fkeys};
        fkeys.enabled = room;
        const auto menu_at = fkeys.bounds;
        add("fkeys", "popup-fkeys", fkeys);
        if (const auto row = chip_row(pl.well, p.chords,
                                      p.latched.empty() || !room ? std::string()
                                                                 : chord_text({p.latched, {}}) + " + …");
            !p.latched.empty() && room) {
            add("pending", "popup-pending", {{}, chord_text({p.latched, {}}) + " + …", row.pending});
        }
        if (p.fkeys && room) {
            // Six across, two down, under their button, right-aligned with it.
            constexpr double size = 70, height = 46, gap = 8, pad = 8;
            const double left = menu_at.x + menu_at.width - (6 * size + 5 * gap + 2 * pad);
            for (int n = 13; n <= 24; ++n) {
                const int i = n - 13;
                add("fkey-" + std::to_string(n), "popup-fkey",
                    {{},
                     "F" + std::to_string(n),
                     {left + pad + (i % 6) * (size + gap),
                      menu_at.y + menu_at.height + 6 + pad + (i / 6) * (height + gap), size, height}},
                    n);
            }
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
    const auto l =
        editor_layout(body, bar_geometry(0), shown_theme().depth, shortcut_editor_->model().bars.size());
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
    case PopupKind::Recording:
        view.title = tr("shortcuts.popup.recording");
        break;
    }
    view.title_bounds = {pl.row.x + 6, pl.row.y, 300, pl.row.height};
    // The hint runs up to the first of row 1's buttons.
    const double buttons =
        p.kind == PopupKind::Recording ? 120 + 104 + 146 + 130 + 4 * 8 : 120 + 104 + 2 * 8;
    view.hint_bounds = {pl.row.x + 6, pl.row.y, pl.row.width - 6 - buttons - 8, pl.row.height};
    view.text = p.text;
    view.cursor = p.cursor;
    if (p.kind == PopupKind::Recording) {
        const bool room = p.chords.size() < shortcut_limits::chords;
        const auto row =
            chip_row(pl.well, p.chords,
                     p.latched.empty() || !room ? std::string() : chord_text({p.latched, {}}) + " + …");
        view.chords = row.chips;
        if (!room) {
            view.full = tr("shortcuts.popup.full");
            view.full_bounds = {row.end + 20, pl.well.y, pl.well.x + pl.well.width - row.end - 30,
                                pl.well.height};
        } else if (p.chords.empty() && p.latched.empty()) {
            view.full = tr("shortcuts.popup.recording-hint");
            view.full_bounds = {pl.well.x + 16, pl.well.y, pl.well.width - 32, pl.well.height};
        }
    }
    if (p.fkeys && p.kind == PopupKind::Recording && p.chords.size() < shortcut_limits::chords) {
        // Its button is row 1's fourth from the right.
        const double fkeys_right = pl.row.x + pl.row.width - (120 + 104 + 146 + 3 * 8);
        constexpr double size = 70, height = 46, gap = 8, pad = 8;
        const double width = 6 * size + 5 * gap + 2 * pad;
        view.fkeys =
            Rect{fkeys_right - width, pl.row.y + pl.row.height + 6, width, 2 * height + gap + 2 * pad};
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
    if (!shortcut_editor_ || !shortcut_editor_->selected_bar()) {
        return;
    }
    const auto& bar = *shortcut_editor_->selected_bar();
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
    view.picker_button = g.picker_button;
    if (view.field) {
        v.bar_tooltip = tr("shortcuts.field.label-tooltip");
        v.bar_tooltip_at = view.buttons[*view.field].bounds;
    }
    v.shortcut_bar = std::move(view);
}
} // namespace framekeyboard
