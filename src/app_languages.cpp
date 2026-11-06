// The Additional languages popup (docs/configuration.md, Additional languages):
// the main language and the ordered languages the toolbar cycles through.
// Everything here is pending until Apply, like the rest of Settings.
#include "framekeyboard/app.hpp"
#include "framekeyboard/swipe.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
constexpr double dead_zone = 12;
const Selection default_main{};
bool same_pair(const Selection& a, const Selection& b) {
    return a.layout == b.layout && a.language == b.language;
}
double column_width() {
    return (languages_list_bounds.width - languages_list_gap) / 2;
}
double scroll_limit(double content, const Rect& list) {
    return std::max(0.0, content - list.height);
}
std::optional<Rect> scroll_thumb(const Rect& list, double scroll, double limit) {
    if (limit <= 0) {
        return {};
    }
    const Rect track{list.x + list.width + 2, list.y + 4, 6, list.height - 8};
    const double length = track.height * track.height / (track.height + limit);
    return Rect{track.x, track.y + scroll / limit * (track.height - length), track.width, length};
}
} // namespace

std::vector<Selection> App::additional_pairs() const {
    std::vector<Selection> result;
    for (const auto& entry : pending_favorites_) {
        // Legacy favorites may repeat a pair or hold the main language.
        if (!same_pair(entry.selection, pending_) &&
            std::none_of(result.begin(), result.end(),
                         [&](const Selection& pair) { return same_pair(pair, entry.selection); })) {
            result.push_back(entry.selection);
        }
    }
    return result;
}
void App::set_additional_pairs(const std::vector<Selection>& pairs) {
    std::vector<Favorite> result;
    for (const auto& pair : pairs) {
        const auto kept = std::find_if(pending_favorites_.begin(), pending_favorites_.end(),
                                       [&](const Favorite& f) { return same_pair(f.selection, pair); });
        if (kept != pending_favorites_.end()) {
            result.push_back(*kept);
            continue;
        }
        // IDs are metadata only; choose one that cannot collide with legacy favorites.
        int index = 1;
        std::string id;
        const auto taken = [&](const Favorite& f) { return f.id == id; };
        do {
            id = "favorite-" + std::to_string(index++);
        } while (std::any_of(pending_favorites_.begin(), pending_favorites_.end(), taken) ||
                 std::any_of(result.begin(), result.end(), taken));
        const auto language = profiles_.languages.find(pair.language);
        result.push_back(
            {id, language == profiles_.languages.end() ? pair.language : language->second.name, pair});
    }
    pending_favorites_ = std::move(result);
    languages_edited_ = true;
    update_settings_ui();
}
void App::remember_languages() {
    languages_edited_ = false;
    saved_main_ = pending_;
    saved_additional_ = additional_pairs();
}
bool App::languages_changed() const {
    const auto pairs = additional_pairs();
    return languages_edited_ && (!same_pair(pending_, saved_main_) ||
                                 !std::equal(pairs.begin(), pairs.end(), saved_additional_.begin(),
                                             saved_additional_.end(), same_pair));
}
void App::open_languages() {
    settings_ui_.close_popup();
    languages_ = LanguagesPopup{};
    languages_->layout = pending_.layout;
    languages_->layout_order = layout_order();
    order_languages();
    if (pending_swipe_typing_ && (options_.swipe_encoder || swipe_engine_installed(options_.data_dir))) {
        for (const auto& [id, language] : profiles_.languages) {
            languages_->swipe[id] = swipe_known(id);
        }
    }
    pressed_controls_.clear();
    hovered_.clear();
}
void App::close_languages() {
    languages_.reset();
    pressed_controls_.clear();
    hovered_.clear();
}
std::vector<std::string> App::layout_order() const {
    std::vector<std::string> result;
    for (const auto& [id, layout] : profiles_.layouts) {
        if (!layout.companion) { // another layout's English page
            result.push_back(id);
        }
    }
    // The common layouts first, then the rest by name.
    const auto rank = [](const std::string& id) {
        return id == "en-us-full" ? 0 : id == "en-gb-full" ? 1 : id == "international-full" ? 2 : 3;
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        const int first = rank(a), second = rank(b);
        return first != second ? first < second
                               : profiles_.layouts.at(a).name < profiles_.layouts.at(b).name;
    });
    return result;
}
void App::order_languages() {
    auto& state = *languages_;
    state.language_order.clear();
    state.usable.clear();
    for (const auto& [id, language] : profiles_.languages) {
        state.language_order.push_back(id);
        state.usable[id] = fits(profiles_, {state.layout, id, pending_.theme});
    }
    // All are listed, with those this layout can type first.
    std::stable_sort(state.language_order.begin(), state.language_order.end(),
                     [&](const auto& a, const auto& b) {
                         return state.usable[a] != state.usable[b]
                                    ? state.usable[a]
                                    : profiles_.languages.at(a).name < profiles_.languages.at(b).name;
                     });
}
double App::languages_limit(LanguagesPopup::Which which) const {
    using Which = LanguagesPopup::Which;
    if (which == Which::Layouts) {
        return scroll_limit(static_cast<double>(profiles_.layouts.size()) * languages_row_height,
                            languages_layouts_bounds);
    }
    if (which == Which::Languages) {
        const auto rows = (profiles_.languages.size() + 1) / 2;
        return scroll_limit(static_cast<double>(rows) * languages_row_height, languages_list_bounds);
    }
    return scroll_limit(static_cast<double>(additional_pairs().size()) * additional_row_height,
                        additional_list_bounds);
}
std::optional<bool> App::language_swipe(const std::string& language) const {
    const auto found = languages_->swipe.find(language);
    return found == languages_->swipe.end() ? std::nullopt : std::optional{found->second};
}
PanelView::LanguagesView::Row App::language_row(const Selection& pair) const {
    PanelView::LanguagesView::Row row;
    const auto language = profiles_.languages.find(pair.language);
    const auto layout = profiles_.layouts.find(pair.layout);
    // A pair whose profiles were removed stays listed, so it can go too.
    row.title = language != profiles_.languages.end() ? language->second.name : pair.language;
    row.note = layout != profiles_.layouts.end() ? layout->second.name : pair.layout + " (missing)";
    if (language == profiles_.languages.end()) {
        row.note += " (language missing)";
    }
    row.swipe = language_swipe(pair.language);
    return row;
}
void App::append_languages(PanelView& v) const {
    if (!settings_open_ || !languages_) {
        return;
    }
    using Which = LanguagesPopup::Which;
    const auto& state = *languages_;
    PanelView::LanguagesView popup;
    popup.panel = languages_popup_bounds;
    popup.well = languages_well_bounds;
    popup.layouts_list = languages_layouts_bounds;
    popup.languages_list = languages_list_bounds;
    popup.additional_list = additional_list_bounds;
    auto add = [&](ControlAction action, Control control, std::optional<Rect> clip = {}) {
        control.action = std::move(action);
        control.clip = clip;
        v.controls.push_back(std::move(control));
    };
    add({ControlKind::CloseLanguages}, {"languages-close",
                                        "Back",
                                        {popup.panel.x + 16, popup.panel.y + 10, 60, 42},
                                        false,
                                        Icon::Back});

    const auto& layouts_list = languages_layouts_bounds;
    const auto& layouts = state.layout_order;
    for (std::size_t i = 0; i < layouts.size(); ++i) {
        const Rect bounds{layouts_list.x,
                          layouts_list.y + static_cast<double>(i) * languages_row_height -
                              state.layouts.scroll,
                          layouts_list.width, languages_row_height - 4};
        if (bounds.y + bounds.height <= layouts_list.y ||
            bounds.y >= layouts_list.y + layouts_list.height) {
            continue;
        }
        PanelView::LanguagesView::Row row;
        row.id = "languages-row-layout-" + std::to_string(i);
        row.title = profiles_.layouts.at(layouts[i]).name;
        row.bounds = bounds;
        row.selected = layouts[i] == state.layout;
        popup.layouts.push_back(row);
        add({ControlKind::LanguagesLayout, 0, layouts[i]}, {row.id, row.title, bounds}, layouts_list);
    }

    const auto pairs = additional_pairs();
    const auto added = [&](const Selection& pair) {
        return same_pair(pair, pending_) ||
               std::any_of(pairs.begin(), pairs.end(),
                           [&](const Selection& entry) { return same_pair(entry, pair); });
    };
    const auto& list = languages_list_bounds;
    const auto& languages = state.language_order;
    for (std::size_t i = 0; i < languages.size(); ++i) {
        const double x = list.x + static_cast<double>(i % 2) * (column_width() + languages_list_gap);
        const double y =
            list.y + static_cast<double>(i / 2) * languages_row_height - state.languages.scroll;
        const Rect bounds{x, y, column_width(), languages_row_height - 4};
        if (y + bounds.height <= list.y || y >= list.y + list.height) {
            continue;
        }
        const Selection pair{state.layout, languages[i], pending_.theme};
        const auto& language = profiles_.languages.at(languages[i]);
        PanelView::LanguagesView::Row row;
        row.id = "languages-row-language-" + std::to_string(i);
        row.title = language.name;
        row.note = language.native_name;
        row.bounds = bounds;
        row.enabled = state.usable.at(languages[i]);
        row.checked = added(pair);
        row.swipe = language_swipe(languages[i]);
        popup.languages.push_back(row);
        Control control{row.id, row.title, bounds};
        control.enabled = row.enabled;
        add({ControlKind::AddLanguage, 0, languages[i]}, control, list);
    }

    popup.main = language_row(pending_);
    popup.main.id = "languages-main";
    popup.main.bounds = languages_main_bounds;
    popup.main_target = state.drag && state.drag->lifted && state.drag->over_main;
    Control reset{"languages-reset",
                  "Reset to English (US)",
                  {languages_main_bounds.x + languages_main_bounds.width - 62,
                   languages_main_bounds.y + 7, 54, 42},
                  false,
                  Icon::Reset};
    reset.enabled = !same_pair(pending_, default_main) && fits(profiles_, default_main);
    add({ControlKind::ResetMainLanguage}, reset);
    Control clear{"languages-clear",
                  "Clear all additional languages",
                  {additional_list_bounds.x + additional_list_bounds.width - 54,
                   additional_list_bounds.y - 44, 54, 38},
                  false,
                  Icon::Eraser};
    clear.enabled = !pairs.empty();
    add({ControlKind::ClearLanguages}, clear);

    // A lifted row leaves its place, and the rest open where it would land.
    const bool lifted = state.drag && state.drag->lifted;
    const auto gap = lifted ? state.drag->insert : std::optional<std::size_t>{};
    const bool leaves = lifted && (state.drag->source == Which::Additional || gap);
    std::size_t slot = 0;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (leaves && same_pair(pairs[i], state.drag->pair)) {
            continue;
        }
        if (gap == slot) {
            ++slot;
        }
        const Rect bounds{additional_list_bounds.x,
                          additional_list_bounds.y + static_cast<double>(slot) * additional_row_height -
                              state.additional.scroll,
                          additional_list_bounds.width, additional_row_height - 6};
        ++slot;
        if (bounds.y + bounds.height <= additional_list_bounds.y ||
            bounds.y >= additional_list_bounds.y + additional_list_bounds.height) {
            continue;
        }
        auto row = language_row(pairs[i]);
        row.id = "languages-row-additional-" + std::to_string(i);
        row.bounds = bounds;
        popup.additional.push_back(row);
        add({ControlKind::None}, {row.id, row.title, bounds}, additional_list_bounds);
    }
    if (pairs.empty() && !(state.drag && state.drag->lifted)) {
        popup.hint = "Drag and drop languages to add and order them, drop outside to remove.";
    }
    popup.layouts_thumb =
        scroll_thumb(layouts_list, state.layouts.scroll, languages_limit(Which::Layouts));
    popup.languages_thumb =
        scroll_thumb(list, state.languages.scroll, languages_limit(Which::Languages));
    popup.additional_thumb = scroll_thumb(additional_list_bounds, state.additional.scroll,
                                          languages_limit(Which::Additional));
    if (state.drag && state.drag->lifted) {
        auto row = language_row(state.drag->pair);
        const Rect size = additional_list_bounds;
        row.bounds = {state.drag->x - size.width / 2, state.drag->y - (additional_row_height - 6) / 2,
                      size.width, additional_row_height - 6};
        row.removing =
            state.drag->source == Which::Additional && !state.drag->insert && !state.drag->over_main;
        popup.lifted = row;
    }
    v.languages = std::move(popup);
}
void App::languages_press(unsigned pointer, double x, double y) {
    using Which = LanguagesPopup::Which;
    auto& state = *languages_;
    if (state.drag) {
        return;
    }
    LanguagesPopup::Drag drag;
    drag.pointer = pointer;
    drag.press_x = drag.x = x;
    drag.press_y = drag.y = y;
    const auto& list = languages_list_bounds;
    if (list.contains(x, y)) {
        const auto row = std::floor((y - list.y + state.languages.scroll) / languages_row_height);
        const bool second = x >= list.x + column_width() + languages_list_gap;
        const auto& languages = state.language_order;
        const auto index = static_cast<std::size_t>(row) * 2 + (second ? 1 : 0);
        drag.source = Which::Languages;
        drag.scroll = state.languages.scroll;
        if (index < languages.size()) {
            drag.pair = {state.layout, languages[index], pending_.theme};
            drag.liftable = state.usable.at(languages[index]);
        }
        state.drag = drag;
    } else if (additional_list_bounds.contains(x, y)) {
        const auto pairs = additional_pairs();
        const auto index = static_cast<std::size_t>(std::floor(
            (y - additional_list_bounds.y + state.additional.scroll) / additional_row_height));
        if (index < pairs.size()) {
            drag.source = Which::Additional;
            drag.pair = pairs[index];
            drag.liftable = true;
            state.drag = drag;
        }
    }
}
bool App::languages_move(unsigned pointer, double x, double y) {
    using Which = LanguagesPopup::Which;
    if (!languages_ || !languages_->drag || languages_->drag->pointer != pointer) {
        return false;
    }
    auto& state = *languages_;
    auto& drag = *state.drag;
    drag.x = x;
    drag.y = y;
    const double dx = std::abs(x - drag.press_x), dy = std::abs(y - drag.press_y);
    if (!drag.lifted && !drag.scrolling) {
        if (drag.source == Which::Additional) {
            drag.lifted = std::max(dx, dy) > dead_zone;
        } else if (drag.liftable && dx > dead_zone && dx >= dy) {
            // Sideways lifts a language; up and down scrolls the list.
            drag.lifted = true;
        } else if (dy > dead_zone) {
            drag.scrolling = true;
        }
    }
    if (drag.scrolling) {
        state.languages.scroll = state.languages.target =
            std::clamp(drag.scroll - (y - drag.press_y), 0.0, languages_limit(Which::Languages));
    } else if (drag.lifted) {
        update_languages_drop();
    } else {
        return false;
    }
    pressed_controls_.erase(pointer);
    hovered_.clear();
    dirty = true;
    return true;
}
void App::update_languages_drop() {
    auto& state = *languages_;
    auto& drag = *state.drag;
    drag.over_main = languages_main_bounds.contains(drag.x, drag.y);
    drag.insert.reset();
    // The whole right column takes a drop, so it need not land on a row.
    const auto& list = additional_list_bounds;
    if (Rect{list.x, list.y - 44, list.width, list.height + 44}.contains(drag.x, drag.y)) {
        auto pairs = additional_pairs();
        std::erase_if(pairs, [&](const Selection& pair) { return same_pair(pair, drag.pair); });
        const auto slot = std::max(
            0.0, std::round((drag.y - list.y + state.additional.scroll) / additional_row_height - .5));
        drag.insert = std::min(static_cast<std::size_t>(slot), pairs.size());
    }
}
bool App::languages_release(unsigned pointer) {
    using Which = LanguagesPopup::Which;
    if (!languages_ || !languages_->drag || languages_->drag->pointer != pointer) {
        return false;
    }
    // The other hand or the list's scrolling may have moved things since the
    // last pointer move, so the drop is worked out on the list as it is now.
    if (languages_->drag->lifted) {
        update_languages_drop();
    }
    const auto drag = *languages_->drag;
    languages_->drag.reset();
    if (!drag.lifted) {
        if (drag.scrolling) {
            pressed_controls_.erase(pointer); // a drag's release is not a click
            dirty = true;
        }
        return drag.scrolling;
    }
    pressed_controls_.erase(pointer);
    auto pairs = additional_pairs();
    const auto was = std::find_if(pairs.begin(), pairs.end(),
                                  [&](const Selection& pair) { return same_pair(pair, drag.pair); });
    if (drag.over_main) {
        if (!same_pair(drag.pair, pending_) && fits(profiles_, drag.pair)) {
            // From the additional list the two trade places. From the list of
            // all, the old main language becomes the first additional one, so
            // making Korean main keeps English a tap away.
            if (was != pairs.end()) {
                *was = pending_;
            } else if (pairs.size() < 32) {
                pairs.insert(pairs.begin(), pending_);
            }
            set_main_language(drag.pair, pairs);
        }
    } else if (drag.insert) {
        const bool usable = !same_pair(drag.pair, pending_);
        if (was != pairs.end()) {
            pairs.erase(was);
        }
        if (usable && (drag.source == Which::Additional || pairs.size() < 32)) {
            pairs.insert(pairs.begin() + static_cast<std::ptrdiff_t>(*drag.insert), drag.pair);
            set_additional_pairs(pairs);
        }
    } else if (drag.source == Which::Additional && was != pairs.end()) {
        pairs.erase(was); // Dropped outside.
        set_additional_pairs(pairs);
    }
    hovered_.clear();
    dirty = true;
    return true;
}
void App::set_main_language(const Selection& pair, std::vector<Selection> additional) {
    pending_.layout = pair.layout;
    pending_.language = pair.language;
    // The main language is never also an additional one.
    std::erase_if(additional, [&](const Selection& entry) { return same_pair(entry, pair); });
    set_additional_pairs(additional);
}
void App::languages_action(const ControlAction& command) {
    const auto kind = command.kind;
    if (kind == ControlKind::OpenLanguages) {
        open_languages();
    } else if (kind == ControlKind::CloseLanguages) {
        close_languages();
    } else if (kind == ControlKind::LanguagesLayout && languages_) {
        languages_->layout = command.value;
        languages_->languages = {};
        order_languages();
    } else if (kind == ControlKind::AddLanguage && languages_) {
        const Selection pair{languages_->layout, command.value, pending_.theme};
        auto pairs = additional_pairs();
        const bool listed = std::any_of(pairs.begin(), pairs.end(),
                                        [&](const Selection& entry) { return same_pair(entry, pair); });
        if (!same_pair(pair, pending_) && !listed && pairs.size() < 32 && fits(profiles_, pair)) {
            pairs.push_back(pair);
            set_additional_pairs(pairs);
        }
    } else if (kind == ControlKind::ResetMainLanguage && fits(profiles_, default_main)) {
        set_main_language(default_main, additional_pairs());
    } else if (kind == ControlKind::ClearLanguages && !additional_pairs().empty()) {
        pressed_controls_.clear();
        hovered_.clear();
        confirm_ = Confirm::ClearLanguages;
    }
}
bool App::languages_scroll(unsigned pointer, double dy) {
    using Which = LanguagesPopup::Which;
    if (!interaction_active_ || dragging_ || !pointer_positions_.contains(pointer) ||
        !std::isfinite(dy)) {
        return false;
    }
    const auto [x, y] = pointer_positions_.at(pointer);
    auto* list = languages_list_under(x, y);
    if (!list) {
        return false;
    }
    const auto which = list == &languages_->layouts     ? Which::Layouts
                       : list == &languages_->languages ? Which::Languages
                                                        : Which::Additional;
    const double old = list->target;
    list->target = std::clamp(old + dy * 52, 0.0, languages_limit(which));
    if (list->target == old) {
        return false;
    }
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
    return true;
}
App::LanguagesPopup::List* App::languages_list_under(double x, double y) {
    auto& state = *languages_;
    return languages_layouts_bounds.contains(x, y) ? &state.layouts
           : languages_list_bounds.contains(x, y)  ? &state.languages
           : additional_list_bounds.contains(x, y) ? &state.additional
                                                   : nullptr;
}
std::optional<ScrollRoom> App::languages_scroll_room(double x, double y) const {
    using Which = LanguagesPopup::Which;
    const auto& state = *languages_;
    const LanguagesPopup::List* list = nullptr;
    auto which = Which::Layouts;
    if (languages_layouts_bounds.contains(x, y)) {
        list = &state.layouts;
    } else if (languages_list_bounds.contains(x, y)) {
        list = &state.languages;
        which = Which::Languages;
    } else if (additional_list_bounds.contains(x, y)) {
        list = &state.additional;
        which = Which::Additional;
    }
    if (!list) {
        return {};
    }
    const double limit = languages_limit(which);
    return limit > 0 ? std::optional{ScrollRoom{list->target > 0, list->target < limit}} : std::nullopt;
}
void App::tick_languages(double now) {
    using Which = LanguagesPopup::Which;
    auto& state = *languages_;
    const double elapsed = state.last_tick < 0 ? 0 : std::clamp(now - state.last_tick, 0.0, .1);
    state.last_tick = now;
    if (state.drag && state.drag->lifted) {
        // A lifted row near the list's end moves the list under it.
        const auto& list = additional_list_bounds;
        const double step = elapsed * 300;
        const auto limit = languages_limit(Which::Additional);
        if (state.drag->x >= list.x && state.drag->x <= list.x + list.width && limit > 0) {
            if (state.drag->y < list.y + 30) {
                state.additional.target = std::max(0.0, state.additional.target - step);
            } else if (state.drag->y > list.y + list.height - 30) {
                state.additional.target = std::min(limit, state.additional.target + step);
            }
        }
    }
    // Stick steps glide, about 70 ms to cover most of one, as the cards do.
    const double additional_was = state.additional.scroll;
    for (auto* list : {&state.layouts, &state.languages, &state.additional}) {
        if (list->scroll != list->target) {
            list->scroll =
                std::abs(list->target - list->scroll) < .5
                    ? list->target
                    : list->scroll + (list->target - list->scroll) * (1 - std::exp(-elapsed / .07));
            dirty = true;
        }
    }
    if (state.additional.scroll != additional_was && state.drag && state.drag->lifted) {
        update_languages_drop(); // The rows moved under a held row.
    }
}
} // namespace framekeyboard
