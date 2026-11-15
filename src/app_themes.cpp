// The theme picker (docs/configuration.md, Theme): every theme as a card, a
// preview of the keyboard in it over its name. A card chooses its theme at
// once, as the dropdown before it did; the choice is pending until Apply,
// like the rest of Settings, and Back from Settings undoes it.
#include "framekeyboard/app.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
constexpr double dead_zone = 12;
constexpr double theme_row_step = theme_card_height + theme_card_gap;
// Where card `index` sits with the grid scrolled to its top.
Rect card_bounds(std::size_t index) {
    const auto column = static_cast<double>(index % static_cast<std::size_t>(theme_columns));
    const auto row = static_cast<double>(index / static_cast<std::size_t>(theme_columns));
    return {themes_well_bounds.x + theme_card_pad + column * (theme_card_width + theme_card_gap),
            themes_well_bounds.y + theme_card_pad + row * theme_row_step, theme_card_width,
            theme_card_height};
}
bool in_view(const Rect& card, double scroll) {
    const auto& well = themes_well_bounds;
    return card.y - scroll < well.y + well.height && card.y - scroll + card.height > well.y;
}
} // namespace

std::vector<const Theme*> App::theme_order() const {
    // By ID, as the dropdown listed them.
    std::vector<const Theme*> result;
    for (const auto& [id, theme] : profiles_.themes) {
        (void)id;
        result.push_back(&theme);
    }
    return result;
}
double App::themes_limit() const {
    const auto columns = static_cast<std::size_t>(theme_columns);
    const auto rows = static_cast<double>((profiles_.themes.size() + columns - 1) / columns);
    const double content =
        2 * theme_card_pad + rows * theme_card_height + std::max(0.0, rows - 1) * theme_card_gap;
    return std::max(0.0, content - themes_well_bounds.height);
}
void App::open_themes() {
    settings_ui_.close_popup();
    themes_ = ThemesPopup{};
    themes_->previews.set_directory(theme_previews_dir_);
    // The chosen theme's card in view: its row at the top if it would be cut off.
    const auto order = theme_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i]->id != pending_.theme) {
            continue;
        }
        const auto card = card_bounds(i);
        if (card.y + card.height > themes_well_bounds.y + themes_well_bounds.height) {
            themes_->scroll = themes_->target =
                std::min(themes_limit(), card.y - themes_well_bounds.y - theme_card_pad);
        }
    }
    pressed_controls_.clear();
    hovered_.clear();
}
void App::close_themes() {
    themes_.reset(); // and the pictures with it
    pressed_controls_.clear();
    hovered_.clear();
}
void App::append_themes(PanelView& v) const {
    if (!settings_open_ || !themes_) {
        return;
    }
    PanelView::ThemesView popup;
    popup.panel = themes_popup_bounds;
    popup.well = themes_well_bounds;
    popup.title = tr("setting.theme");
    auto add = [&](ControlAction action, Control control, std::optional<Rect> clip = {}) {
        control.action = std::move(action);
        control.clip = clip;
        v.controls.push_back(std::move(control));
    };
    add({ControlKind::CloseThemes}, {"themes-close",
                                     tr("languages.back"),
                                     {popup.panel.x + 16, popup.panel.y + 10, 60, 42},
                                     false,
                                     Icon::Back});
    // The cards rest on whole pixels, so their pictures are copied, not
    // resampled, and a glide's last fractions of a pixel change no view.
    const double scroll = std::round(themes_->scroll);
    const auto order = theme_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& theme = *order[i];
        auto bounds = card_bounds(i);
        if (!in_view(bounds, scroll)) {
            continue;
        }
        bounds.y -= scroll;
        PanelView::ThemesView::Card card;
        card.id = theme.id;
        // A shell's own theme has no name of its own.
        card.name = theme.id == shell_theme_id ? tr("setting.theme.shell") : theme.name;
        card.bounds = bounds;
        card.selected = theme.id == pending_.theme;
        card.preview = themes_->previews.find(theme);
        card.surface = theme.surface;
        // Its button: the card, as far as the well shows it.
        add({ControlKind::ChooseTheme, 0, theme.id},
            {"themes-card-" + theme.id, card.name, bounds, card.selected}, popup.well);
        popup.cards.push_back(std::move(card));
    }
    if (const double limit = themes_limit(); limit > 0) {
        const auto& well = popup.well;
        const Rect track{well.x + well.width - 11, well.y + 8, 6, well.height - 16};
        const double length = track.height * track.height / (track.height + limit);
        popup.thumb = Rect{track.x, std::round(track.y + scroll / limit * (track.height - length)),
                           track.width, length};
    }
    v.themes = std::move(popup);
}
void App::themes_press(unsigned pointer, double x, double y) {
    if (themes_->drag || !themes_well_bounds.contains(x, y)) {
        return;
    }
    themes_->drag = ThemesPopup::Drag{pointer, y, themes_->scroll, false};
}
bool App::themes_move(unsigned pointer, double x, double y) {
    (void)x;
    if (!themes_ || !themes_->drag || themes_->drag->pointer != pointer) {
        return false;
    }
    auto& drag = *themes_->drag;
    if (!drag.scrolling && std::abs(y - drag.press_y) > dead_zone) {
        drag.scrolling = true; // Up or down scrolls; the card pressed is not clicked.
    }
    if (!drag.scrolling) {
        return false;
    }
    themes_->scroll = themes_->target =
        std::clamp(drag.scroll - (y - drag.press_y), 0.0, themes_limit());
    pressed_controls_.erase(pointer);
    hovered_.clear();
    dirty = true;
    return true;
}
bool App::themes_release(unsigned pointer) {
    if (!themes_ || !themes_->drag || themes_->drag->pointer != pointer) {
        return false;
    }
    const bool scrolled = themes_->drag->scrolling;
    themes_->drag.reset();
    if (scrolled) {
        pressed_controls_.erase(pointer); // a drag's release is not a click
        dirty = true;
    }
    return scrolled;
}
bool App::themes_scroll(unsigned pointer, double dy) {
    if (!interaction_active_ || dragging_ || !pointer_positions_.contains(pointer) ||
        !std::isfinite(dy)) {
        return false;
    }
    const auto [x, y] = pointer_positions_.at(pointer);
    if (!themes_well_bounds.contains(x, y)) {
        return false;
    }
    const double old = themes_->target;
    themes_->target = std::clamp(old + dy * theme_row_step, 0.0, themes_limit()); // a row a step
    if (themes_->target == old) {
        return false;
    }
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
    return true;
}
std::optional<ScrollRoom> App::themes_scroll_room(double x, double y) const {
    const double limit = themes_limit();
    if (!themes_well_bounds.contains(x, y) || limit <= 0) {
        return {};
    }
    return ScrollRoom{themes_->target > 0, themes_->target < limit};
}
void App::tick_themes(double now) {
    auto& state = *themes_;
    const double elapsed = state.last_tick < 0 ? 0 : std::clamp(now - state.last_tick, 0.0, .1);
    state.last_tick = now;
    // Stick steps glide, as the cards do.
    if (glide(state.scroll, state.target, elapsed)) {
        dirty = true;
    }
    // Hidden, the pictures are freed (set_interaction_active) and none load.
    if (!interaction_active_) {
        return;
    }
    // One preview a frame, loaded from disk or drawn: the cards in view first.
    const auto order = theme_order();
    std::vector<const Theme*> wanted;
    for (const bool visible : {true, false}) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (in_view(card_bounds(i), state.scroll) == visible) {
                wanted.push_back(order[i]);
            }
        }
    }
    if (state.previews.fill_one(wanted, profiles_)) {
        dirty = true;
    }
}
} // namespace framekeyboard
