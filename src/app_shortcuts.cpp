// The shortcut bar (docs/shortcut-bar-design.md): a bar of the user's own
// buttons above the case, the picker and its menu, and which profile shows.
// App builds the bar's view and takes its presses here; ShortcutRunner alone
// sends a button's keys, and drawing code acts on nothing.
#include "framekeyboard/app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

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
MacroStep keys_step(std::vector<ShortcutChord> chords) {
    MacroStep step;
    step.kind = MacroStep::Kind::Keys;
    step.keys = std::move(chords);
    return step;
}
MacroStep wait_step(int ms) {
    MacroStep step;
    step.kind = MacroStep::Kind::Wait;
    step.wait_ms = ms;
    return step;
}
ShortcutButton macro(std::string label, std::vector<MacroStep> steps, MacroRepeat repeat) {
    ShortcutButton button;
    button.label = std::move(label);
    button.action.kind = ShortcutAction::Kind::Macro;
    button.action.steps = std::move(steps);
    button.action.repeat = repeat;
    return button;
}
} // namespace

bool App::bar_shown() const {
    // The editor's screen takes the keys' place, and its fields show the
    // editor's own bar (append_field_bar).
    return shortcuts_enabled() && !settings_open_ && !dictation_open_ && !shortcut_editor_ &&
           (profile_choice_.shown() || profile_choice_.empty_bar());
}
const ShortcutProfile* App::shown_profile() const {
    const auto* id = profile_choice_.shown();
    return id ? shortcuts_.profile(*id) : nullptr;
}
void App::load_shortcuts(bool reloading) {
    auto loaded = settings_writer_.load_shortcuts();
    // The load waited for any save under way: its result is taken first, so
    // it never clears the note this load sets.
    take_shortcuts_result();
    shortcuts_invalid_ = loaded.state == ShortcutsLoad::State::Invalid ? loaded.reason : "";
    if (reloading && loaded.state == ShortcutsLoad::State::Invalid) {
        // A hand edit with a fault: the profiles already loaded stay, and the
        // next save keeps the file as shortcuts.json.bad.
        // Reload's status shows its errors.
        profiles_.errors.push_back(tr("status.shortcuts-invalid", {{"reason", shortcuts_invalid_}}));
        dirty = true;
        return;
    }
    shortcuts_ = std::move(loaded.model);
    // A file loaded anew may lack the profile a run or a press was on.
    stop_shortcut_run();
    bar_press_.reset();
    bar_menu_ = false;
}
void App::configure_bar() {
    profile_choice_.configure(shortcuts_, shortcuts_enabled(), settings_.shortcuts_switch);
    if (!bar_shown()) {
        bar_menu_ = false;
    }
    dirty = true;
}
void App::save_shortcuts() {
    settings_writer_.save_shortcuts(shortcuts_);
}
void App::apply_bar_changes(const ShortcutProfileChoice::Changes& changes) {
    if (changes.shortcuts_switch) {
        // Saved at once, as the toolbar's other switches are.
        settings_.shortcuts_switch = profile_choice_.switch_on();
        save();
    }
    if (changes.free_profile) {
        save_shortcuts();
    }
    if (!bar_shown()) {
        bar_menu_ = false;
        bar_press_.reset();
    }
    dirty = true;
}
bool App::shortcut_button_enabled(const ShortcutButton& button) const {
    if (shortcut_action_empty(button.action)) {
        return false;
    }
    // Run types nothing, so it needs no typing target: never disabled for
    // one (design, Where actions can go).
    if (button.action.kind == ShortcutAction::Kind::Run) {
        return true;
    }
    // A Steam field takes only a few named keys, so no combination can go there.
    return !output_.steam_field_target();
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
    const auto* bar = shown_profile();
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
        shown.running = shortcut_run_ && shortcut_runner_.running() &&
                        shortcut_run_->profile == bar->id && shortcut_run_->button == i;
        view.buttons.push_back(std::move(shown));
    }
    view.more_before = scroll > .5;
    view.more_after = scroll < g.scroll_limit - .5;
    view.picker = g.picker;
    view.picker_hovered = hovered("picker");
    view.picker_pressed = pressed("picker");
    if (bar_menu_) {
        const auto m = shortcut_menu_geometry(g, shortcuts_.profiles.size());
        ShortcutBarView::Menu menu;
        menu.bounds = m.bounds;
        menu.header = m.header;
        menu.list = m.list;
        menu.manage = m.manage;
        menu.title = tr("shortcuts.menu.title");
        // The menu doesn't name the focused app; the assign buttons' tooltips
        // do (design, Picker menu).
        const auto app_name = focused_app_name();
        const auto* tie = bar_focus_.app ? shortcuts_.tie(*bar_focus_.app) : nullptr;
        const bool assignable = assign_unavailable().empty();
        const auto* showing = profile_choice_.shown();
        for (std::size_t i = 0; i < shortcuts_.profiles.size(); ++i) {
            const auto r = m.row(i, menu_scroll_.shown);
            if (r.y + r.height <= m.list.y || r.y >= m.list.y + m.list.height) {
                continue;
            }
            const auto& listed = shortcuts_.profiles[i];
            ShortcutBarView::Menu::Row row;
            row.name = listed.name;
            row.bounds = r;
            row.assign = m.assign(r);
            row.shown = showing && *showing == listed.id;
            row.hovered = hovered("row-" + std::to_string(i));
            row.pressed = pressed("row-" + std::to_string(i));
            row.assign_enabled = assignable;
            row.assigned = assignable && tie && tie->profile == listed.id;
            row.assign_hovered = hovered("assign-" + std::to_string(i));
            if (row.assign_hovered) {
                // What a press will do, naming the profile and the app; on a
                // disabled button, why it is.
                v.bar_tooltip = assignable ? tr(row.assigned ? "tooltip.shortcuts-unassign"
                                                             : "tooltip.shortcuts-assign",
                                                {{"profile", listed.name}, {"app", app_name}})
                                           : assign_unavailable();
                v.bar_tooltip_at = row.assign;
            }
            menu.rows.push_back(std::move(row));
        }
        if (m.scroll_limit > 0) {
            const double length = m.list.height * m.list.height / (m.list.height + m.scroll_limit);
            menu.thumb = Rect{m.bounds.x + m.bounds.width - 6,
                              m.list.y + menu_scroll_.shown / m.scroll_limit * (m.list.height - length),
                              3, length};
        }
        menu.manage_label = tr("shortcuts.menu.manage");
        menu.manage_enabled = true;
        menu.manage_hovered = hovered("manage");
        menu.manage_pressed = pressed("manage");
        if (menu.manage_hovered) {
            v.bar_tooltip = tr("tooltip.shortcuts-manage");
            v.bar_tooltip_at = menu.manage;
        }
        view.menu = std::move(menu);
    }
    if (view.picker_hovered && !bar_menu_) {
        v.bar_tooltip = tr("tooltip.shortcuts-picker");
        v.bar_tooltip_at = g.picker;
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
            const auto g = bar_geometry(shown_profile() ? shown_profile()->buttons.size() : 0);
            const auto m = shortcut_menu_geometry(g, shortcuts_.profiles.size());
            for (std::size_t i = 0; i < shortcuts_.profiles.size(); ++i) {
                const auto row = m.row(i, menu_scroll_.shown);
                if (row.contains(x, y)) {
                    return (m.assign(row).contains(x, y) ? "assign-" : "row-") + std::to_string(i);
                }
            }
        }
        if (menu.manage.contains(x, y)) {
            return "manage";
        }
        if (menu.bounds.contains(x, y)) {
            return "menu"; // Its margins take the laser too, choosing nothing.
        }
    }
    if (bar.picker.contains(x, y)) {
        return "picker";
    }
    if (bar.strip.contains(x, y)) {
        const auto* shown = shown_profile();
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
        const auto* bar = shown_profile();
        if (!bar || *index >= bar->buttons.size()) {
            return false;
        }
        const auto scrolled = bar_scroll_.find(bar->id);
        const double scroll = scrolled == bar_scroll_.end() ? 0 : scrolled->second.target;
        const auto& button = bar->buttons[*index];
        const bool enabled = shortcut_button_enabled(button);
        const bool while_held = enabled && button.action.kind == ShortcutAction::Kind::Macro &&
                                button.action.repeat.mode == MacroRepeat::Mode::WhileHeld;
        // Another button stops the run under way; its own button pressed
        // again only stops it (design, Macro). A While held button's run,
        // only finishing its pass once let go, takes this press as its hold
        // again, so quick taps each act and nothing is sent twice (R342).
        bool stopped = false, rehold = false;
        if (shortcut_run_ && shortcut_runner_.running()) {
            const bool own = shortcut_run_->profile == bar->id && shortcut_run_->button == *index;
            if (own && while_held && shortcut_runner_.hold()) {
                rehold = true;
            } else {
                stopped = own;
                stop_shortcut_run();
            }
        }
        // A While held macro starts on press and runs while this press holds;
        // its release only lets it go.
        const bool held = while_held && !stopped;
        bar_press_ = BarPress{pointer, target, x, scroll, now, false, stopped || held, bar->id, held};
        if (held && !rehold) {
            press_shortcut_button(*index, std::max(now, mozc_clock_));
        }
        dirty = true;
        // A key's press haptic and pop, for a button that can act.
        return enabled;
    }
    if (const auto row = target_index(target, "assign-")) {
        // A disabled button takes the press, which goes nowhere else.
        if (*row >= shortcuts_.profiles.size() || !assign_unavailable().empty()) {
            return false;
        }
    }
    // The picker, a menu row, an assign button or Manage: any press
    // on the bar that acts stops a run, as a key does (R340).
    stop_shortcut_run();
    bar_press_ = BarPress{pointer, target, x, 0, now, false, false, {}, false};
    dirty = true;
    return false;
}
bool App::bar_move(unsigned pointer, double x) {
    if (!bar_press_ || bar_press_->pointer != pointer || !bar_press_->target.starts_with("button-")) {
        return false;
    }
    const auto* bar = shown_profile();
    if (!bar || bar->id != bar_press_->profile) {
        return false;
    }
    auto& press = *bar_press_;
    if (press.held) {
        return false; // A held macro's press never drags the bar.
    }
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
    if (press.held) {
        shortcut_runner_.release(); // the pass under way finishes
    }
    const auto index = target_index(press.target, "button-");
    const auto* bar = shown_profile();
    // Only on the profile it began on: another that showed meanwhile has other
    // buttons in the same places (R321).
    const bool button = index && bar && bar->id == press.profile && *index < bar->buttons.size();
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
        if (bar_menu_) {
            look_up_focused_name();
        }
        // The chosen profile's row in view.
        if (const auto* id = profile_choice_.shown(); bar_menu_ && id) {
            const auto* bar = shown_profile();
            const auto m = shortcut_menu_geometry(bar_geometry(bar ? bar->buttons.size() : 0),
                                                  shortcuts_.profiles.size());
            for (std::size_t i = 0; i < shortcuts_.profiles.size(); ++i) {
                if (shortcuts_.profiles[i].id == *id) {
                    const double below = static_cast<double>(i + 1) * m.row_height - m.list.height;
                    menu_scroll_.shown = menu_scroll_.target = std::clamp(below, 0.0, m.scroll_limit);
                }
            }
        }
    } else if (const auto row = target_index(target, "assign-")) {
        assign_profile(*row); // The menu stays open.
    } else if (const auto row = target_index(target, "row-"); row && *row < shortcuts_.profiles.size()) {
        // A choice closes the menu; a free profile becomes the one returned to.
        bar_menu_ = false;
        const auto changes = profile_choice_.pick(shortcuts_, shortcuts_.profiles[*row].id);
        apply_bar_changes(changes);
    } else if (target == "manage") {
        bar_menu_ = false;
        open_shortcut_editor(); // on the profile showing
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
    const auto* bar = shown_profile();
    const auto g = bar_geometry(bar ? bar->buttons.size() : 0);
    // A step moves a button, or a row, at a time: whole steps from the
    // dashboard, and the passthrough pointer's stick in fractions that add up.
    const double delta = std::isfinite(dx) && std::abs(dx) > std::abs(dy) ? dx : dy;
    const bool on_menu =
        bar_menu_ && shortcut_menu_geometry(g, shortcuts_.profiles.size()).bounds.contains(x, y);
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
        const auto m = shortcut_menu_geometry(g, shortcuts_.profiles.size());
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
    const auto* bar = shown_profile();
    const auto g = bar_geometry(bar ? bar->buttons.size() : 0);
    if (bar_menu_) {
        const auto m = shortcut_menu_geometry(g, shortcuts_.profiles.size());
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
    // A While held macro runs only while a press holds it: whatever ended
    // the press (a release, a cancel, the hold's limit, another profile), the
    // pass under way finishes and no other starts.
    if (!bar_press_ || !bar_press_->held) {
        shortcut_runner_.release();
    }
    if (shortcut_runner_.tick(now)) {
        dirty = true; // Its button goes dark.
    }
    if (shortcut_run_ && !shortcut_runner_.running()) {
        shortcut_run_.reset();
        dirty = true;
    }
    // Another window or another app has the keys: a run stops, as nothing
    // between its combinations would catch it, and which profile shows is
    // decided again. Inside SteamOS's Desktop another app is a change of
    // identity in the same window. Both come in one snapshot, so a new
    // window never shows the last app's profile.
    if (auto focus = focused_app(); focus.window != bar_focus_.window || focus.app != bar_focus_.app) {
        bar_focus_ = std::move(focus);
        bar_focus_serial_ = output_.focus_serial();
        stop_shortcut_run();
        profile_choice_.focus(shortcuts_, bar_focus_.app);
        if (bar_menu_) {
            look_up_focused_name(); // for the header of the menu still open
        }
        dirty = true;
    }
    // Another window of the same app, such as a dialog inside the Desktop,
    // took the keys: only a run stops. The bar and a profile picked for this
    // app stay (R320).
    if (const auto serial = output_.focus_serial(); serial != bar_focus_serial_) {
        bar_focus_serial_ = serial;
        stop_shortcut_run();
        dirty = true;
    }
    if (!bar_shown()) {
        bar_menu_ = false;
        bar_press_.reset();
    }
    // A button's press belongs to the profile it began on: another profile showing
    // drops it, so its release sends nothing (R321).
    if (bar_press_ && bar_press_->target.starts_with("button-") &&
        (!shown_profile() || shown_profile()->id != bar_press_->profile)) {
        bar_press_.reset();
        dirty = true;
    }
    if (const auto* bar = shown_profile()) {
        auto& scroll = bar_scroll_[bar->id];
        if (!bar_press_ || !bar_press_->dragging) {
            dirty |= ease_scroll(scroll.shown, scroll.target, elapsed);
        }
    }
    dirty |= ease_scroll(menu_scroll_.shown, menu_scroll_.target, elapsed);
    take_shortcuts_result();
    take_app_names();
    take_started();
}
FocusedApp App::focused_app() const {
    auto focus = output_.focused_app();
    if (dashboard_focus_) {
        // Steam's dashboard has the keys: it is no app, whatever window
        // Gamescope would fall back to.
        focus.app.reset();
        focus.program.clear();
    }
    return focus;
}
std::string App::focused_app_name() const {
    if (!bar_focus_.app) {
        return {};
    }
    const auto& app = *bar_focus_.app;
    if (const auto* tie = shortcuts_.tie(app); tie && !tie->name.empty()) {
        return tie->name; // stored with the tie, not looked up again
    }
    if (app_name_ && app_name_->app == app && !app_name_->name.empty()) {
        return app_name_->name;
    }
    return fallback_app_name(app, bar_focus_.program);
}
void App::look_up_focused_name() {
    if (!bar_focus_.app) {
        return;
    }
    const auto& app = *bar_focus_.app;
    if (const auto* tie = shortcuts_.tie(app); tie && !tie->name.empty()) {
        return;
    }
    if (!app_name_ || app_name_->app != app) {
        app_names_.request(app);
    }
}
void App::take_app_names() {
    // A tie's name changes only in the model the editor doesn't hold: its ✓
    // would put the old one back. Lookups wait until it closes.
    if (shortcut_editor_) {
        return;
    }
    bool renamed = false;
    while (auto result = app_names_.take()) {
        if (bar_focus_.app == result->app) {
            app_name_ = AppName{result->app, result->name};
            dirty = true;
        }
        // A tie saved with the identity's own name takes the one found, once,
        // unless it changed since.
        for (auto fix = name_fixes_.begin(); fix != name_fixes_.end();) {
            if (fix->app != result->app) {
                ++fix;
                continue;
            }
            for (auto& tie : shortcuts_.apps) {
                if (tie.app == fix->app && tie.name == fix->name && !result->name.empty() &&
                    tie.name != result->name) {
                    tie.name = result->name;
                    renamed = true;
                }
            }
            fix = name_fixes_.erase(fix);
        }
    }
    if (renamed) {
        save_shortcuts();
        dirty = true;
    }
}
std::string App::assign_unavailable() const {
    if (dashboard_focus_) {
        return tr("tooltip.shortcuts-assign-dashboard");
    }
    if (output_.steam_field_target()) {
        return tr("tooltip.shortcuts-assign-steam-field");
    }
    if (!bar_focus_.app) {
        return tr("tooltip.shortcuts-assign-unknown");
    }
    if (!shortcuts_.can_assign(*bar_focus_.app)) {
        return tr("tooltip.shortcuts-assign-full");
    }
    return {};
}
void App::assign_profile(std::size_t row) {
    if (row >= shortcuts_.profiles.size() || !assign_unavailable().empty()) {
        return;
    }
    const auto app = *bar_focus_.app;
    const auto id = shortcuts_.profiles[row].id;
    const auto* tie = shortcuts_.tie(app);
    ShortcutProfileChoice::Changes changes;
    if (tie && tie->profile == id) {
        // Untied: the bar stays as it is until the focus changes.
        shortcuts_.unassign(app);
        std::erase_if(name_fixes_, [&](const AppName& fix) { return fix.app == app; });
        changes = profile_choice_.model_changed(shortcuts_);
    } else {
        // Saved with the name known now; until a lookup lands, the one the
        // identity gives, updated once it does (take_app_names).
        const bool known = (tie && !tie->name.empty()) || (app_name_ && app_name_->app == app);
        const auto name = focused_app_name();
        if (!shortcuts_.assign(app, name, id)) {
            return;
        }
        if (!known) {
            std::erase_if(name_fixes_, [&](const AppName& fix) { return fix.app == app; });
            name_fixes_.push_back({app, name});
            app_names_.request(app);
        }
        // Its other tie is gone; the profile just tied shows at once.
        changes = profile_choice_.model_changed(shortcuts_);
        const auto picked = profile_choice_.pick(shortcuts_, id);
        changes.shortcuts_switch |= picked.shortcuts_switch;
    }
    // Saved at once, the free profile with it.
    save_shortcuts();
    changes.free_profile = false;
    apply_bar_changes(changes);
}
void App::take_shortcuts_result() {
    const auto result = settings_writer_.take_shortcuts_result();
    if (!result) {
        return;
    }
    // The file is in place whether or not its folder's sync worked: only a
    // power loss soon after could still lose it (R336).
    const bool saved = !*result || **result == ShortcutStore::unsynced;
    if (*result) {
        status_ = tr(saved ? "status.shortcuts-unsynced" : "status.shortcuts-not-saved",
                     {{"error", **result}});
        dirty = true;
    }
    if (saved && !shortcuts_invalid_.empty()) {
        // The save kept the unreadable file as shortcuts.json.bad and wrote
        // a valid one: the note goes, in Settings and the editor (R323).
        shortcuts_invalid_.clear();
        update_settings_ui();
        dirty = true;
    }
}
void App::press_shortcut_button(std::size_t index, double now) {
    const auto* bar = shown_profile();
    if (!bar || index >= bar->buttons.size() || !shortcut_button_enabled(bar->buttons[index])) {
        return;
    }
    // Copied: what follows may change the model's storage.
    const auto profile_id = bar->id;
    const auto action = bar->buttons[index].action;
    if (action.kind == ShortcutAction::Kind::Run) {
        start_run_button(profile_id, index, bar->buttons[index]);
        return;
    }
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
        shortcut_run_ = ShortcutRun{profile_id, index};
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
void App::start_run_button(const std::string& profile, std::size_t index, const ShortcutButton& button) {
    // One start at a time for a button: its next press waits for the report.
    if (std::any_of(pending_starts_.begin(), pending_starts_.end(), [&](const PendingStart& start) {
            return start.profile == profile && start.button == index;
        })) {
        return;
    }
    LaunchRequest request;
    request.script = button.action.script;
    request.command = button.action.command;
    request.scripts = scripts_folder(config_dir());
    const char* home = std::getenv("HOME");
    request.home = home && *home ? home : "/";
    request.environment = child_environment(current_environment());
    const auto id = ++next_start_;
    auto label = button.label;
    std::replace(label.begin(), label.end(), '\n', ' ');
    pending_starts_.push_back({id, profile, index, std::move(label)});
    launcher_.start(id, std::move(request));
}
void App::take_started() {
    while (const auto result = launcher_.take()) {
        const auto start = std::find_if(pending_starts_.begin(), pending_starts_.end(),
                                        [&](const PendingStart& item) { return item.id == result->id; });
        if (start == pending_starts_.end()) {
            continue;
        }
        if (result->failure != LaunchFailure::None) {
            // Only the kind, as a number: never the label, script or command.
            std::cerr << "Shortcut bar: a Run button did not start (failure "
                      << static_cast<int>(result->failure) << ")\n";
            status_ = tr("status.shortcuts-run-failed", {{"button", start->label}});
            dirty = true;
        }
        pending_starts_.erase(start);
    }
}
void App::stop_shortcut_run() {
    if (shortcut_runner_.running() || shortcut_run_) {
        shortcut_runner_.stop();
        shortcut_run_.reset();
        dirty = true;
    }
}
void App::add_sample_shortcuts() {
    if (!shortcuts_.profiles.empty()) {
        return;
    }
    // Profiles to try the bar with: everyday editing with two macros, enough
    // keys to scroll, and a terminal's keys assigned to an app.
    ShortcutProfile editing{
        "sample-editing",
        "Sample editing",
        {keys("Undo", {chord({"ControlLeft"}, "KeyZ")}), keys("Redo", {chord({"ControlLeft"}, "KeyY")}),
         keys("Save", {chord({"ControlLeft"}, "KeyS")}), keys("Find", {chord({"ControlLeft"}, "KeyF")}),
         keys("Select\nall", {chord({"ControlLeft"}, "KeyA")}),
         keys("Close\ntab", {chord({"ControlLeft"}, "KeyW")}),
         // Two macros: one repeated three times, one while held.
         macro("Line end,\ndown ×3",
               {keys_step({chord({}, "End")}), wait_step(200), keys_step({chord({}, "ArrowDown")})},
               {MacroRepeat::Mode::Times, 3}),
         macro("Page down\nheld", {keys_step({chord({}, "PageDown")}), wait_step(300)},
               {MacroRepeat::Mode::WhileHeld, 0})}};
    ShortcutProfile many{
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
    // A terminal's own keys, for the simulator's made-up Terminal (its App
    // button), assigned to it.
    ShortcutProfile terminal{"sample-terminal",
                             "Sample terminal",
                             {keys("Copy", {chord({"ControlLeft", "ShiftLeft"}, "KeyC")}),
                              keys("Paste", {chord({"ControlLeft", "ShiftLeft"}, "KeyV")}),
                              keys("Stop", {chord({"ControlLeft"}, "KeyC")}),
                              keys("Clear", {chord({"ControlLeft"}, "KeyL")}),
                              keys("Tab", {chord({}, "Tab")}), keys("Esc", {chord({}, "Escape")})}};
    shortcuts_.profiles = {std::move(editing), std::move(many), std::move(terminal)};
    shortcuts_.free_profile = shortcuts_.profiles.front().id; // shows wherever no app has a profile
    shortcuts_.apps = {
        {{AppIdentity::Kind::Desktop, "org.example.Terminal"}, "Terminal", "sample-terminal"}};
    save_shortcuts();
    // Shown at once, in place of the empty bar the toolbar may have opened.
    configure_bar();
}
} // namespace framekeyboard
