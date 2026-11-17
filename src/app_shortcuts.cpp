// The shortcut bar (docs/shortcut-bar-design.md): a bar of the user's own
// buttons above the case, the picker and its menu, and which bar shows.
// App builds the bar's view and takes its presses here; ShortcutRunner alone
// sends a button's keys, and drawing code acts on nothing.
#include "framekeyboard/app.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
// A pressed button dragged sideways this far scrolls the bar instead.
constexpr double drag_threshold = 12;
// The index in a target named prefix + N, or none.
std::optional<std::size_t> target_index(const std::string& target, std::string_view prefix) {
    if (!target.starts_with(prefix)) {
        return {};
    }
    try {
        return static_cast<std::size_t>(std::stoul(target.substr(prefix.size())));
    } catch (const std::exception&) {
        return {};
    }
}
ShortcutChord chord(std::vector<std::string> modifiers, std::string key) {
    return {std::move(modifiers), std::move(key)};
}
ShortcutButton keys(std::string label, std::vector<ShortcutChord> chords) {
    ShortcutButton button;
    button.label = std::move(label);
    button.action.kind = ShortcutAction::Kind::Keys;
    button.action.chords = std::move(chords);
    return button;
}
} // namespace

bool App::bar_shown() const {
    // The editor's screen takes the keys' place, and its fields show the
    // editor's own bar (append_field_bar).
    return shortcuts_enabled() && !settings_open_ && !dictation_open_ && !shortcut_editor_ &&
           (bar_choice_.shown() || bar_choice_.empty_bar());
}
const ShortcutBar* App::shown_bar() const {
    const auto* id = bar_choice_.shown();
    return id ? shortcuts_.bar(*id) : nullptr;
}
void App::load_shortcuts(bool reloading) {
    auto loaded = settings_writer_.load_shortcuts();
    // The load waited for any save under way: its result is taken first, so
    // it never clears the note this load sets.
    take_shortcuts_result();
    shortcuts_invalid_ = loaded.state == ShortcutsLoad::State::Invalid ? loaded.reason : "";
    if (reloading && loaded.state == ShortcutsLoad::State::Invalid) {
        // A hand edit with a fault: the bars already loaded stay, and the
        // next save keeps the file as shortcuts.json.bad.
        // Reload's status shows its errors.
        profiles_.errors.push_back(tr("status.shortcuts-invalid", {{"reason", shortcuts_invalid_}}));
        dirty = true;
        return;
    }
    shortcuts_ = std::move(loaded.model);
    // A file loaded anew may lack the bar a run or a press was on.
    stop_shortcut_run();
    bar_press_.reset();
    bar_menu_ = false;
}
void App::configure_bar() {
    bar_choice_.configure(shortcuts_, shortcuts_enabled(), settings_.shortcuts_switch);
    if (!bar_shown()) {
        bar_menu_ = false;
    }
    dirty = true;
}
void App::save_shortcuts() {
    settings_writer_.save_shortcuts(shortcuts_);
}
void App::apply_bar_changes(const ShortcutBarChoice::Changes& changes) {
    if (changes.shortcuts_switch) {
        // Saved at once, as the toolbar's other switches are.
        settings_.shortcuts_switch = bar_choice_.switch_on();
        save();
    }
    if (changes.free_bar) {
        save_shortcuts();
    }
    if (!bar_shown()) {
        bar_menu_ = false;
        bar_press_.reset();
    }
    dirty = true;
}
bool App::shortcut_button_enabled(const ShortcutButton& button) const {
    // Macros and Run come in later phases; a Steam field takes only a few
    // named keys, so no combination can go there (design, Where actions can go).
    return button.action.kind == ShortcutAction::Kind::Keys && !shortcut_action_empty(button.action) &&
           !output_.steam_field_target();
}
ShortcutBarGeometry App::bar_geometry(std::size_t buttons) const {
    // From the keyboard's own keys: the emoji picker keeps its case and
    // scale, and the bar stays as it was over it.
    auto view = geometry();
    if (emoji_open_) {
        view.layout = &shown_layout();
        view.emoji.reset();
    }
    return shortcut_bar_geometry(view, buttons);
}
Rect App::bar_button_bounds(const ShortcutBarGeometry& bar, std::size_t count, std::size_t index,
                            double scroll) const {
    // One button sits centred in the two slots.
    const double x = count == 1 ? bar.strip.x + (bar.strip.width - bar.button_width) / 2
                                : bar.strip.x + static_cast<double>(index) * bar.pitch - scroll;
    return {x, bar.button_top, bar.button_width, bar.button_height};
}
void App::append_shortcut_bar(PanelView& v) const {
    if (field_) {
        append_field_bar(v); // the editor's bar, over a field or a recording
        return;
    }
    if (!bar_shown()) {
        return;
    }
    const auto* bar = shown_bar();
    const std::size_t count = bar ? bar->buttons.size() : 0;
    const auto g = bar_geometry(count);
    const auto scrolled = bar ? bar_scroll_.find(bar->id) : bar_scroll_.end();
    const double scroll =
        scrolled == bar_scroll_.end() ? 0 : std::clamp(scrolled->second.shown, 0.0, g.scroll_limit);
    const auto hovered = [&](const std::string& target) {
        return std::any_of(hovered_.begin(), hovered_.end(), [&](const auto& entry) {
            return entry.second == HoverTarget{HoverTarget::Kind::Bar, target};
        });
    };
    const auto pressed = [&](const std::string& target) {
        return bar_press_ && bar_press_->target == target && !bar_press_->dragging &&
               !bar_press_->stopped;
    };
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
        const auto& button = bar->buttons[i];
        const auto target = "button-" + std::to_string(i);
        ShortcutBarView::Button shown;
        shown.label = button.label;
        shown.bounds = r;
        shown.enabled = shortcut_button_enabled(button);
        shown.hovered = hovered(target);
        shown.pressed = pressed(target);
        shown.running = shortcut_run_ && shortcut_runner_.running() && shortcut_run_->bar == bar->id &&
                        shortcut_run_->button == i;
        view.buttons.push_back(std::move(shown));
    }
    view.more_before = scroll > .5;
    view.more_after = scroll < g.scroll_limit - .5;
    view.picker = g.picker;
    view.picker_button = g.picker_button;
    view.picker_hovered = hovered("picker");
    view.picker_pressed = pressed("picker");
    if (bar_menu_) {
        const auto m = shortcut_menu_geometry(g, shortcuts_.bars.size());
        ShortcutBarView::Menu menu;
        menu.bounds = m.bounds;
        menu.header = m.header;
        menu.list = m.list;
        menu.edit = m.edit;
        menu.title = tr("shortcuts.menu.title");
        // The focused app's name comes with app identities (design, Apps).
        const auto* showing = bar_choice_.shown();
        for (std::size_t i = 0; i < shortcuts_.bars.size(); ++i) {
            const auto r = m.row(i, menu_scroll_.shown);
            if (r.y + r.height <= m.list.y || r.y >= m.list.y + m.list.height) {
                continue;
            }
            const auto& listed = shortcuts_.bars[i];
            ShortcutBarView::Menu::Row row;
            row.name = listed.name;
            row.bounds = r;
            row.assign = m.assign(r);
            row.shown = showing && *showing == listed.id;
            row.hovered = hovered("row-" + std::to_string(i));
            row.pressed = pressed("row-" + std::to_string(i));
            // Disabled until the focused app can be identified.
            row.assign_enabled = false;
            menu.rows.push_back(std::move(row));
        }
        if (m.scroll_limit > 0) {
            const double length = m.list.height * m.list.height / (m.list.height + m.scroll_limit);
            menu.thumb = Rect{m.bounds.x + m.bounds.width - 6,
                              m.list.y + menu_scroll_.shown / m.scroll_limit * (m.list.height - length),
                              3, length};
        }
        menu.edit_label = tr("shortcuts.menu.edit");
        menu.edit_enabled = true;
        menu.edit_hovered = hovered("edit");
        menu.edit_pressed = pressed("edit");
        view.menu = std::move(menu);
    }
    if (view.picker_hovered && !bar_menu_) {
        v.bar_tooltip = tr("tooltip.shortcuts-picker");
        v.bar_tooltip_at = g.picker_button;
    }
    v.shortcut_bar = std::move(view);
}
std::optional<std::string> App::bar_hit(const PanelView& view, double x, double y) const {
    // Over a field or a recording the bar only shows what is being edited.
    if (!view.shortcut_bar || view.settings || field_) {
        return {};
    }
    const auto& bar = *view.shortcut_bar;
    if (bar.menu) {
        const auto& menu = *bar.menu;
        if (menu.list.contains(x, y)) {
            const auto g = bar_geometry(shown_bar() ? shown_bar()->buttons.size() : 0);
            const auto m = shortcut_menu_geometry(g, shortcuts_.bars.size());
            for (std::size_t i = 0; i < shortcuts_.bars.size(); ++i) {
                const auto row = m.row(i, menu_scroll_.shown);
                if (row.contains(x, y)) {
                    return (m.assign(row).contains(x, y) ? "assign-" : "row-") + std::to_string(i);
                }
            }
        }
        if (menu.edit.contains(x, y)) {
            return "edit";
        }
        if (menu.bounds.contains(x, y)) {
            return "menu"; // Its margins take the laser too, choosing nothing.
        }
    }
    if (bar.picker_button.contains(x, y)) {
        return "picker";
    }
    if (bar.strip.contains(x, y)) {
        const auto* shown = shown_bar();
        const std::size_t count = shown ? shown->buttons.size() : 0;
        const auto g = bar_geometry(count);
        const auto scrolled = shown ? bar_scroll_.find(shown->id) : bar_scroll_.end();
        const double scroll =
            scrolled == bar_scroll_.end() ? 0 : std::clamp(scrolled->second.shown, 0.0, g.scroll_limit);
        for (std::size_t i = 0; i < count; ++i) {
            if (bar_button_bounds(g, count, i, scroll).contains(x, y)) {
                return "button-" + std::to_string(i);
            }
        }
    }
    if (bar.plate.contains(x, y) || bar.picker.contains(x, y)) {
        return "plate";
    }
    return {};
}
bool App::bar_down(unsigned pointer, const std::string& target, double x, double now) {
    if (target == "plate" || target == "menu") {
        return false; // Nothing there acts; the press goes nowhere else.
    }
    if (const auto index = target_index(target, "button-")) {
        const auto* bar = shown_bar();
        if (!bar || *index >= bar->buttons.size()) {
            return false;
        }
        // Another button stops the run under way; its own button pressed
        // again only stops it (design, Macro).
        bool stopped = false;
        if (shortcut_run_ && shortcut_runner_.running()) {
            stopped = shortcut_run_->bar == bar->id && shortcut_run_->button == *index;
            stop_shortcut_run();
        }
        const auto scrolled = bar_scroll_.find(bar->id);
        const double scroll = scrolled == bar_scroll_.end() ? 0 : scrolled->second.target;
        bar_press_ = BarPress{pointer, target, x, scroll, now, false, stopped, bar->id};
        dirty = true;
        // A key's press haptic and pop, for a button that can act.
        return shortcut_button_enabled(bar->buttons[*index]);
    }
    if (target.starts_with("assign-")) {
        // Disabled until apps can be identified. The press goes nowhere else.
        return false;
    }
    bar_press_ = BarPress{pointer, target, x, 0, now, false, false, {}};
    dirty = true;
    return false;
}
bool App::bar_move(unsigned pointer, double x) {
    if (!bar_press_ || bar_press_->pointer != pointer || !bar_press_->target.starts_with("button-")) {
        return false;
    }
    const auto* bar = shown_bar();
    if (!bar || bar->id != bar_press_->bar) {
        return false;
    }
    auto& press = *bar_press_;
    const auto g = bar_geometry(bar->buttons.size());
    if (!press.dragging) {
        // Only a bar that overflows scrolls; a button released elsewhere
        // simply does nothing.
        if (g.scroll_limit <= 0 || std::abs(x - press.x) <= drag_threshold) {
            return false;
        }
        press.dragging = true;
    }
    auto& scroll = bar_scroll_[bar->id];
    const double to = std::clamp(press.scroll - (x - press.x), 0.0, g.scroll_limit);
    if (to != scroll.shown || to != scroll.target) {
        scroll.shown = scroll.target = to;
        scroll.remainder = 0;
        dirty = true;
    }
    return true;
}
bool App::bar_up(unsigned pointer) {
    const auto press = *std::exchange(bar_press_, std::nullopt);
    dirty = true;
    const auto index = target_index(press.target, "button-");
    const auto* bar = shown_bar();
    // Only on the bar it began on: another that showed meanwhile has other
    // buttons in the same places (R321).
    const bool button = index && bar && bar->id == press.bar && *index < bar->buttons.size();
    // A key's release haptic and pop, as for any button pressed.
    const bool felt = button && shortcut_button_enabled(bar->buttons[*index]);
    if (press.dragging || press.stopped) {
        return felt;
    }
    const auto hovered = hovered_.find(pointer);
    if (hovered == hovered_.end() ||
        hovered->second != HoverTarget{HoverTarget::Kind::Bar, press.target}) {
        return felt; // Let go elsewhere: nothing happens.
    }
    if (button) {
        press_shortcut_button(*index, std::max(press.at, mozc_clock_));
    } else if (!index) {
        bar_action(press.target);
    }
    return felt;
}
void App::bar_action(const std::string& target) {
    if (target == "picker") {
        bar_menu_ = !bar_menu_;
        menu_scroll_ = {};
        // The chosen bar's row in view.
        if (const auto* id = bar_choice_.shown(); bar_menu_ && id) {
            const auto* bar = shown_bar();
            const auto m = shortcut_menu_geometry(bar_geometry(bar ? bar->buttons.size() : 0),
                                                  shortcuts_.bars.size());
            for (std::size_t i = 0; i < shortcuts_.bars.size(); ++i) {
                if (shortcuts_.bars[i].id == *id) {
                    const double below = static_cast<double>(i + 1) * m.row_height - m.list.height;
                    menu_scroll_.shown = menu_scroll_.target = std::clamp(below, 0.0, m.scroll_limit);
                }
            }
        }
    } else if (const auto row = target_index(target, "row-"); row && *row < shortcuts_.bars.size()) {
        // A choice closes the menu; a free bar becomes the one returned to.
        bar_menu_ = false;
        const auto changes = bar_choice_.pick(shortcuts_, shortcuts_.bars[*row].id);
        apply_bar_changes(changes);
    } else if (target == "edit") {
        bar_menu_ = false;
        open_shortcut_editor(); // on the bar showing
    }
    dirty = true;
}
std::optional<bool> App::bar_scroll(unsigned pointer, double dx, double dy) {
    if (!bar_shown() || !interaction_active_ || dragging_) {
        return {};
    }
    const auto position = pointer_positions_.find(pointer);
    if (position == pointer_positions_.end()) {
        return {};
    }
    const auto [x, y] = position->second;
    const auto* bar = shown_bar();
    const auto g = bar_geometry(bar ? bar->buttons.size() : 0);
    // A step moves a button, or a row, at a time: whole steps from the
    // dashboard, and the passthrough pointer's stick in fractions that add up.
    const double delta = std::isfinite(dx) && std::abs(dx) > std::abs(dy) ? dx : dy;
    const bool on_menu =
        bar_menu_ && shortcut_menu_geometry(g, shortcuts_.bars.size()).bounds.contains(x, y);
    // The menu and the plates take every scroll over them, at an end too:
    // the candidates, the dictation editor or the emoji grid under the menu
    // never move (R325).
    if (!on_menu && !g.plate.contains(x, y) && !g.picker.contains(x, y)) {
        return {};
    }
    if (!std::isfinite(delta)) {
        return false;
    }
    auto step = [&](BarScroll& scroll, double size, double limit) {
        if (limit <= 0) {
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
            scroll.remainder = 0; // Nothing builds up past an end.
        }
        dirty |= scroll.target != old;
        return scroll.target != old;
    };
    if (on_menu) {
        const auto m = shortcut_menu_geometry(g, shortcuts_.bars.size());
        return step(menu_scroll_, m.row_height, m.scroll_limit);
    }
    if (bar && g.plate.contains(x, y)) {
        return step(bar_scroll_[bar->id], g.pitch, g.scroll_limit);
    }
    return false;
}
std::optional<ScrollRoom> App::bar_scroll_room(double x, double y) const {
    // Asked every frame: only over the bar, above the case, or an open menu.
    if (!bar_shown() || (y >= 0 && !bar_menu_)) {
        return {};
    }
    const auto* bar = shown_bar();
    const auto g = bar_geometry(bar ? bar->buttons.size() : 0);
    if (bar_menu_) {
        const auto m = shortcut_menu_geometry(g, shortcuts_.bars.size());
        if (m.bounds.contains(x, y)) {
            return m.scroll_limit > 0 ? std::optional{ScrollRoom{menu_scroll_.target > 0,
                                                                 menu_scroll_.target < m.scroll_limit}}
                                      : std::nullopt;
        }
    }
    if (!bar || !g.plate.contains(x, y) || g.scroll_limit <= 0) {
        return {};
    }
    const auto scrolled = bar_scroll_.find(bar->id);
    const double target = scrolled == bar_scroll_.end() ? 0 : scrolled->second.target;
    return ScrollRoom{target > 0, target < g.scroll_limit};
}
void App::tick_shortcut_bar(double now) {
    const double elapsed = bar_tick_ < 0 ? 0 : now - bar_tick_;
    bar_tick_ = now;
    if (shortcut_runner_.tick(now)) {
        dirty = true; // Its button goes dark.
    }
    if (shortcut_run_ && !shortcut_runner_.running()) {
        shortcut_run_.reset();
        dirty = true;
    }
    // Another window has the keys: a run stops, as nothing between its
    // combinations would catch it, and which bar shows is decided again.
    // Until apps can be told apart, every window counts as one without a bar.
    if (const auto window = output_.focused_window(); window != bar_window_) {
        bar_window_ = window;
        bar_focus_serial_ = output_.focus_serial();
        stop_shortcut_run();
        bar_choice_.focus(shortcuts_, std::nullopt);
        dirty = true;
    }
    // Inside SteamOS's Desktop another app or a dialog took the keys in the
    // same window: a run stops too. The bar stays, as the Desktop's apps
    // aren't told apart yet (R320).
    if (const auto serial = output_.focus_serial(); serial != bar_focus_serial_) {
        bar_focus_serial_ = serial;
        stop_shortcut_run();
        dirty = true;
    }
    if (!bar_shown()) {
        bar_menu_ = false;
        bar_press_.reset();
    }
    // A button's press belongs to the bar it began on: another bar showing
    // drops it, so its release sends nothing (R321).
    if (bar_press_ && bar_press_->target.starts_with("button-") &&
        (!shown_bar() || shown_bar()->id != bar_press_->bar)) {
        bar_press_.reset();
        dirty = true;
    }
    if (const auto* bar = shown_bar()) {
        auto& scroll = bar_scroll_[bar->id];
        if (!bar_press_ || !bar_press_->dragging) {
            dirty |= ease_scroll(scroll.shown, scroll.target, elapsed);
        }
    }
    dirty |= ease_scroll(menu_scroll_.shown, menu_scroll_.target, elapsed);
    take_shortcuts_result();
}
void App::take_shortcuts_result() {
    const auto result = settings_writer_.take_shortcuts_result();
    if (!result) {
        return;
    }
    if (*result) {
        status_ = tr("status.shortcuts-not-saved", {{"error", **result}});
        dirty = true;
    } else if (!shortcuts_invalid_.empty()) {
        // The save kept the unreadable file as shortcuts.json.bad and wrote
        // a valid one: the note goes, in Settings and the editor (R323).
        shortcuts_invalid_.clear();
        update_settings_ui();
        dirty = true;
    }
}
void App::press_shortcut_button(std::size_t index, double now) {
    const auto* bar = shown_bar();
    if (!bar || index >= bar->buttons.size() || !shortcut_button_enabled(bar->buttons[index])) {
        return;
    }
    // Copied: what follows may change the model's storage.
    const auto bar_id = bar->id;
    const auto action = bar->buttons[index].action;
    // Without a target, as a key: Focus mode's icon shakes.
    if (refuse_without_target(now) || !backend_ready_) {
        return;
    }
    // A pending swiped word and an unfinished reading go out first, as
    // before any raw shortcut.
    finish_swipe(true);
    discard_swipe();
    swipe_space_ = false;
    if (!commit_composition()) {
        dirty = true;
        return;
    }
    if (!output_.admit(true, now)) {
        update_output_status();
        dirty = true;
        return;
    }
    // Only the button's own modifiers: latched ones clear, held or locked
    // ones come up first (a lock stays locked).
    if (!keyboard_.clear_for_shortcut()) {
        return;
    }
    // A shortcut may move the cursor: the word being typed is forgotten.
    lose_word();
    if (shortcut_runner_.start(action, now)) {
        shortcut_run_ = ShortcutRun{bar_id, index};
    } else {
        // Refused as a key would be: its status, or a queue too full for
        // the whole sequence. Without typing the status already says why.
        update_output_status();
        if (output_.enabled() && output_.can_queue(1)) {
            status_ = tr("output.queue-full");
        }
    }
    dirty = true;
}
void App::stop_shortcut_run() {
    if (shortcut_runner_.running() || shortcut_run_) {
        shortcut_runner_.stop();
        shortcut_run_.reset();
        dirty = true;
    }
}
void App::add_sample_shortcuts() {
    if (!shortcuts_.bars.empty()) {
        return;
    }
    // Two bars to try the bar with before the editor exists: everyday
    // editing, and enough keys to scroll.
    ShortcutBar editing{
        "sample-editing",
        "Sample editing",
        {keys("Undo", {chord({"ControlLeft"}, "KeyZ")}), keys("Redo", {chord({"ControlLeft"}, "KeyY")}),
         keys("Save", {chord({"ControlLeft"}, "KeyS")}), keys("Find", {chord({"ControlLeft"}, "KeyF")}),
         keys("Select\nall", {chord({"ControlLeft"}, "KeyA")}),
         keys("Close\ntab", {chord({"ControlLeft"}, "KeyW")})}};
    ShortcutBar many{
        "sample-keys",
        "Sample keys",
        {keys("Home", {chord({}, "Home")}), keys("End", {chord({}, "End")}),
         keys("Page\nup", {chord({}, "PageUp")}), keys("Page\ndown", {chord({}, "PageDown")}),
         keys("←", {chord({}, "ArrowLeft")}), keys("→", {chord({}, "ArrowRight")}),
         keys("↑", {chord({}, "ArrowUp")}), keys("↓", {chord({}, "ArrowDown")}),
         keys("Tab", {chord({}, "Tab")}), keys("Shift\nTab", {chord({"ShiftLeft"}, "Tab")}),
         keys("Esc", {chord({}, "Escape")}), keys("Ctrl\nHome", {chord({"ControlLeft"}, "Home")}),
         keys("Ctrl\nEnd", {chord({"ControlLeft"}, "End")}),
         keys("Word\nleft", {chord({"ControlLeft"}, "ArrowLeft")}),
         keys("Word\nright", {chord({"ControlLeft"}, "ArrowRight")}),
         keys("Copy", {chord({"ControlLeft"}, "KeyC")}), keys("Paste", {chord({"ControlLeft"}, "KeyV")}),
         keys("Save,\nthen close", {chord({"ControlLeft"}, "KeyS"), chord({"ControlLeft"}, "KeyW")})}};
    shortcuts_.bars = {std::move(editing), std::move(many)};
    shortcuts_.free_bar = shortcuts_.bars.front().id; // shows wherever no app has a bar
    save_shortcuts();
    // Shown at once, in place of the empty bar the toolbar may have opened.
    configure_bar();
}
} // namespace framekeyboard
