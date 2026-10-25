#include "framekeyboard/emoji.hpp"
#include "bundled_profiles.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <glib.h>
#include <json-c/json.h>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unistd.h>

namespace framekeyboard {
namespace {
// A press that moves this far, in panel pixels, scrolls instead of typing.
constexpr double drag_distance = 10;
// Holding a toned emoji this long opens its skin tones.
constexpr double hold_seconds = .5;
constexpr double tab_spacing = 6;
constexpr int tone_choices = 6; // Default, then light to dark.

bool valid_text(std::string_view text) {
    return !text.empty() && text.size() <= 64 && text.find('\0') == std::string_view::npos &&
           g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}
std::vector<std::string_view> split(std::string_view line, char separator) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto end = line.find(separator, start);
        fields.push_back(line.substr(start, end - start));
        if (end == std::string_view::npos) {
            return fields;
        }
        start = end + 1;
    }
}
} // namespace

Icon emoji_group_icon(EmojiGroup group) {
    static constexpr std::array<Icon, emoji_group_count> icons{
        Icon::Recent, Icon::Smileys,    Icon::People,  Icon::Animals, Icon::Food,
        Icon::Travel, Icon::Activities, Icon::Objects, Icon::Symbols, Icon::Flags};
    return icons[static_cast<std::size_t>(group)];
}
const std::string& EmojiEntry::text(int tone) const {
    return tone > 0 && tone <= 5 && toned() ? tones[static_cast<std::size_t>(tone - 1)] : base;
}
const EmojiEntry* EmojiCatalog::find(const std::string& base) const {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const EmojiEntry& entry) { return entry.base == base; });
    return found == entries.end() ? nullptr : &*found;
}
EmojiCatalog parse_emoji_table(std::string_view table) {
    EmojiCatalog catalog;
    for (const auto line : split(table, '\n')) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = split(line, '\t');
        if ((fields.size() != 2 && fields.size() != 7) || fields[0].size() != 1 || fields[0][0] < '1' ||
            fields[0][0] > '9') {
            throw std::runtime_error("invalid emoji table line");
        }
        EmojiEntry entry;
        entry.group = static_cast<EmojiGroup>(fields[0][0] - '0');
        for (std::size_t i = 1; i < fields.size(); ++i) {
            if (!valid_text(fields[i])) {
                throw std::runtime_error("invalid emoji text");
            }
        }
        entry.base = fields[1];
        for (std::size_t i = 2; i < fields.size(); ++i) {
            entry.tones[i - 2] = fields[i];
        }
        catalog.entries.push_back(std::move(entry));
    }
    return catalog;
}
const EmojiCatalog& bundled_emoji() {
    static const EmojiCatalog catalog = parse_emoji_table(bundled::emoji_table);
    return catalog;
}

EmojiPreferences load_emoji_preferences(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) ||
        std::filesystem::file_size(path, error) > 16384) {
        return {};
    }
    std::unique_ptr<json_object, decltype(&json_object_put)> document(
        json_object_from_file(path.c_str()), json_object_put);
    json_object *version{}, *tone{}, *recent{};
    if (!document || !json_object_object_get_ex(document.get(), "schema_version", &version) ||
        json_object_get_int(version) != 1 || !json_object_object_get_ex(document.get(), "tone", &tone) ||
        !json_object_is_type(tone, json_type_int) ||
        !json_object_object_get_ex(document.get(), "recent", &recent) ||
        !json_object_is_type(recent, json_type_array)) {
        return {};
    }
    EmojiPreferences preferences;
    preferences.tone = json_object_get_int(tone);
    if (preferences.tone < 0 || preferences.tone > 5) {
        return {};
    }
    for (std::size_t i = 0; i < json_object_array_length(recent); ++i) {
        auto* item = json_object_array_get_idx(recent, i);
        if (!json_object_is_type(item, json_type_string) ||
            !valid_text({json_object_get_string(item),
                         static_cast<std::size_t>(json_object_get_string_len(item))}) ||
            preferences.recent.size() >= EmojiPreferences::max_recent) {
            return {};
        }
        preferences.recent.emplace_back(json_object_get_string(item));
    }
    return preferences;
}
void save_emoji_preferences(const std::filesystem::path& path, const EmojiPreferences& preferences) {
    std::unique_ptr<json_object, decltype(&json_object_put)> document(json_object_new_object(),
                                                                      json_object_put);
    json_object_object_add(document.get(), "schema_version", json_object_new_int(1));
    json_object_object_add(document.get(), "tone", json_object_new_int(preferences.tone));
    auto* recent = json_object_new_array();
    for (const auto& text : preferences.recent) {
        json_object_array_add(recent, json_object_new_string(text.c_str()));
    }
    json_object_object_add(document.get(), "recent", recent);
    const std::string data =
        std::string(json_object_to_json_string_ext(document.get(), JSON_C_TO_STRING_PRETTY)) + '\n';
    std::filesystem::create_directories(path.parent_path());
    // Written on the VR thread when the picker closes, so no fsync: losing the
    // last few recent emoji in a crash is harmless, a stalled frame is not.
    std::string temporary = (path.parent_path() / ".emoji-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw std::runtime_error("Cannot save recent emoji");
    }
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = write(fd, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error("Cannot save recent emoji");
            }
            offset += static_cast<std::size_t>(written);
        }
        std::filesystem::rename(temporary, path);
        close(fd);
    } catch (...) {
        close(fd);
        unlink(temporary.c_str());
        throw;
    }
}
void remember_emoji(EmojiPreferences& preferences, const std::string& base) {
    std::erase(preferences.recent, base);
    preferences.recent.insert(preferences.recent.begin(), base);
    if (preferences.recent.size() > EmojiPreferences::max_recent) {
        preferences.recent.resize(EmojiPreferences::max_recent);
    }
}

EmojiLayout emoji_layout(const Layout& main, double visible_width) {
    // Designed on the bundled 416-unit-high layouts: the tab row on the
    // function-key row, the grid over the four middle rows, and the space bar
    // on the bottom row. Taller or shorter layouts scale vertically.
    const double k = main.height / 416;
    const double right = std::clamp(visible_width, 0.0, main.width);
    EmojiLayout result;
    result.layout.id = main.id + "-emoji";
    result.layout.name = main.name;
    result.layout.language = main.language;
    result.layout.width = main.width;
    result.layout.height = main.height;
    double side = 0;
    for (const auto& key : main.keys) {
        if (key.action_kind == ActionKind::Emoji || key.action_kind == ActionKind::Shortcut) {
            auto copy = key;
            if (key.action_kind == ActionKind::Emoji) {
                copy.icon = Icon::Back;
                copy.label = "Back";
            }
            side = std::max(side, key.bounds.x + key.bounds.width);
            result.layout.keys.push_back(std::move(copy));
        }
    }
    const double left = side > 0 ? side + 41 : 0;
    const double grid_right = std::max(left + 120, right - 174 - 20);
    result.tabs = {left, 0, grid_right - left, 44 * k};
    result.grid = {left, 66 * k, grid_right - left, (344 - 66) * k};
    auto add = [&](const std::string& action, Rect bounds, const std::string& label,
                   Icon icon = Icon::None) {
        Key key;
        key.id = action;
        key.label = label;
        key.icon = icon;
        key.action_kind = ActionKind::Key;
        key.action = action;
        key.bounds = bounds;
        // Keep the main layout's own legend and key ID where it has the key.
        const auto found = std::find_if(main.keys.begin(), main.keys.end(), [&](const Key& other) {
            return other.action_kind == ActionKind::Key && other.action == action;
        });
        if (found != main.keys.end()) {
            key.id = found->id;
            key.label = found->label;
            key.secondary_label = found->secondary_label;
            key.icon = found->icon;
        }
        result.layout.keys.push_back(std::move(key));
    };
    const auto space = std::find_if(main.keys.begin(), main.keys.end(), [](const Key& key) {
        return key.action_kind == ActionKind::Key && key.action == "Space";
    });
    const double grid_width = grid_right - left;
    if (space != main.keys.end() && space->bounds.x >= left &&
        space->bounds.x + space->bounds.width <= grid_right && space->bounds.y >= 344 * k) {
        add("Space", space->bounds, "");
    } else {
        const double width = std::min(371.5, grid_width);
        add("Space", {left + (grid_width - width) / 2, 354 * k, width, 62 * k}, "");
    }
    add("Backspace", {right - 114, 66 * k, 114, 62 * k}, "", Icon::Backspace);
    add("Enter", {right - 114, 138 * k, 114, 134 * k}, "Enter");
    add("ArrowUp", {right - 114, 282 * k, 54, 62 * k}, "↑");
    add("ArrowLeft", {right - 174, 354 * k, 54, 62 * k}, "←");
    add("ArrowDown", {right - 114, 354 * k, 54, 62 * k}, "↓");
    add("ArrowRight", {right - 54, 354 * k, 54, 62 * k}, "→");
    return result;
}

void EmojiPicker::set_geometry(Rect tabs, Rect grid) {
    if (tabs == tabs_ && grid == grid_) {
        return;
    }
    tabs_ = tabs;
    grid_ = grid;
    // Five rows on the bundled layouts; square cells fill the grid's height.
    rows_ = std::max(1, static_cast<int>(grid.height / 54));
    cell_ = grid.height / rows_;
    layout_items();
}
void EmojiPicker::set_content(std::array<std::vector<const EmojiEntry*>, emoji_group_count> sections,
                              int tone, bool loading) {
    sections_ = std::move(sections);
    tone_ = tone;
    loading_ = loading;
    // Items move; a press or hover would now point at another emoji.
    cancel();
    layout_items();
}
void EmojiPicker::set_tone(int tone) {
    tone_ = std::clamp(tone, 0, 5);
}
void EmojiPicker::layout_items() {
    items_.clear();
    const double gap = cell_ / 2; // Between categories.
    double x = 0;
    int column = 0;
    for (std::size_t g = 0; g < emoji_group_count; ++g) {
        auto& section = layout_[g];
        const auto& list = sections_[g];
        const bool recent = g == 0;
        section = {};
        section.shown = loading_ || recent || !list.empty();
        if (!section.shown || loading_) {
            continue;
        }
        section.start = x;
        section.first_column = column;
        section.first_item = items_.size();
        const auto count = recent ? std::min(list.size(), recent_capacity()) : list.size();
        for (std::size_t i = 0; i < count; ++i) {
            items_.push_back(
                {list[i], g, column + static_cast<int>(i) / rows_, static_cast<int>(i) % rows_});
        }
        if (recent && count) {
            // Clear takes the bottom-right cell, after the last one Recent can fill.
            items_.push_back({nullptr, g, column + recent_columns - 1, rows_ - 1});
        }
        section.items = items_.size() - section.first_item;
        // Recent keeps its full width, so nothing after it moves as it fills.
        section.columns = recent ? recent_columns : items_.back().column - column + 1;
        x += section.columns * cell_ + gap;
        column += section.columns;
    }
    width_ = std::max(0.0, x - gap);
    scroll_ = std::clamp(scroll_, 0.0, scroll_limit());
    tab_scroll_ = std::clamp(tab_scroll_, 0.0, tab_limit());
}
double EmojiPicker::scroll_limit() const {
    return std::max(0.0, width_ - grid_.width);
}
void EmojiPicker::reset() {
    scroll_ = tab_scroll_ = 0;
    chosen_group_.reset();
    cancel();
}
std::vector<std::size_t> EmojiPicker::shown_groups() const {
    std::vector<std::size_t> groups;
    for (std::size_t g = 0; g < emoji_group_count; ++g) {
        if (layout_[g].shown) {
            groups.push_back(g);
        }
    }
    return groups;
}
double EmojiPicker::tab_width() const {
    const auto count = static_cast<double>(std::max<std::size_t>(1, shown_groups().size()));
    return std::clamp((tabs_.width + tab_spacing) / count - tab_spacing, 48.0, 80.0);
}
Rect EmojiPicker::scrolling_tabs() const {
    const double fixed = tab_width() + tab_spacing; // Recent stays at the left.
    return {tabs_.x + fixed, tabs_.y, std::max(0.0, tabs_.width - fixed), tabs_.height};
}
double EmojiPicker::tab_limit() const {
    const auto count = shown_groups().size();
    if (count < 2) {
        return 0;
    }
    const double strip = static_cast<double>(count - 1) * (tab_width() + tab_spacing) - tab_spacing;
    return std::max(0.0, strip - scrolling_tabs().width);
}
Rect EmojiPicker::tab_bounds(std::size_t group) const {
    const auto groups = shown_groups();
    const auto index =
        static_cast<double>(std::find(groups.begin(), groups.end(), group) - groups.begin());
    const double width = tab_width();
    const double x = tabs_.x + index * (width + tab_spacing) - (group == 0 ? 0 : tab_scroll_);
    return {x, tabs_.y, width, tabs_.height};
}
void EmojiPicker::reveal_tab(std::size_t group) {
    if (group == 0) {
        return;
    }
    const auto clip = scrolling_tabs();
    const auto bounds = tab_bounds(group);
    if (bounds.x < clip.x) {
        tab_scroll_ -= clip.x - bounds.x;
    } else if (bounds.x + bounds.width > clip.x + clip.width) {
        tab_scroll_ += bounds.x + bounds.width - clip.x - clip.width;
    }
    tab_scroll_ = std::clamp(tab_scroll_, 0.0, tab_limit());
}
std::size_t EmojiPicker::active_group() const {
    if (chosen_group_) {
        return *chosen_group_;
    }
    // The category under the grid's left edge, counting a half-visible column.
    std::size_t active = 0;
    for (std::size_t g = 0; g < emoji_group_count; ++g) {
        if (layout_[g].shown && !loading_ && layout_[g].start <= scroll_ + cell_ / 2) {
            active = g;
        }
    }
    return active;
}
Rect EmojiPicker::cell_bounds(const Item& item) const {
    const auto& section = layout_[item.group];
    return {grid_.x + section.start + (item.column - section.first_column) * cell_ - scroll_,
            grid_.y + item.row * cell_, cell_, cell_};
}
std::optional<Rect> EmojiPicker::popup_bounds() const {
    if (!press_ || !press_->popup) {
        return {};
    }
    constexpr double padding = 6, spacing = 4;
    const auto cell = cell_bounds(items_[press_->hit.index]);
    const double width = tone_choices * cell_ + (tone_choices - 1) * spacing + 2 * padding;
    const double height = cell_ + 2 * padding;
    const double x = std::clamp(cell.x + cell.width / 2 - width / 2, grid_.x,
                                std::max(grid_.x, grid_.x + grid_.width - width));
    return Rect{x, cell.y - height - 4, width, height};
}
Rect EmojiPicker::choice_bounds(std::size_t choice) const {
    constexpr double padding = 6, spacing = 4;
    const auto popup = *popup_bounds();
    return {popup.x + padding + static_cast<double>(choice) * (cell_ + spacing), popup.y + padding,
            cell_, cell_};
}
std::string EmojiPicker::text(const Item& item) const {
    return item.entry ? item.entry->text(tone_) : std::string{};
}
EmojiPicker::Hit EmojiPicker::hit(double x, double y) const {
    if (press_ && press_->popup) {
        for (std::size_t i = 0; i < tone_choices; ++i) {
            if (choice_bounds(i).contains(x, y)) {
                return {Target::Choice, i};
            }
        }
        return {};
    }
    if (grid_.contains(x, y)) {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (cell_bounds(items_[i]).contains(x, y)) {
                return {Target::Cell, i};
            }
        }
        return {};
    }
    if (tabs_.contains(x, y)) {
        const auto clip = scrolling_tabs();
        for (const auto g : shown_groups()) {
            if (tab_bounds(g).contains(x, y) && (g == 0 || clip.contains(x, y))) {
                return {Target::Tab, g};
            }
        }
    }
    return {};
}
bool EmojiPicker::contains(double x, double y) const {
    if (const auto popup = popup_bounds(); popup && popup->contains(x, y)) {
        return true;
    }
    return grid_.contains(x, y) || tabs_.contains(x, y);
}
EmojiPicker::Event EmojiPicker::down(unsigned pointer, double x, double y, double now) {
    Event event;
    if (press_) {
        return event; // One press at a time; another hand's press waits.
    }
    const auto target = hit(x, y);
    hovered_[pointer] = target;
    if (target.target == Target::None) {
        return event;
    }
    press_ = Press{pointer, x, y, scroll_, tab_scroll_, now, target, false, false, std::nullopt};
    event.redraw = event.haptic = true;
    return event;
}
EmojiPicker::Event EmojiPicker::move(unsigned pointer, double x, double y) {
    Event event;
    if (press_ && press_->pointer == pointer) {
        auto& press = *press_;
        if (press.popup) {
            const auto target = hit(x, y);
            const auto choice = target.target == Target::Choice
                                    ? std::optional<std::size_t>(target.index)
                                    : std::nullopt;
            if (choice != press.choice) {
                press.choice = choice;
                event.redraw = true;
                event.haptic = choice.has_value();
            }
            return event;
        }
        if (!press.dragging && std::max(std::abs(x - press.x), std::abs(y - press.y)) > drag_distance) {
            press.dragging = true;
            hovered_.clear();
            event.redraw = true;
        }
        if (press.dragging) {
            if (press.hit.target == Target::Cell) {
                const double old = scroll_;
                scroll_ = std::clamp(press.scroll - (x - press.x), 0.0, scroll_limit());
                if (scroll_ != old) {
                    chosen_group_.reset();
                    reveal_tab(active_group());
                    event.redraw = true;
                }
            } else {
                const double old = tab_scroll_;
                tab_scroll_ = std::clamp(press.tab_scroll - (x - press.x), 0.0, tab_limit());
                event.redraw |= tab_scroll_ != old;
            }
            return event;
        }
    }
    const auto target = hit(x, y);
    auto& hovered = hovered_[pointer];
    if (hovered != target) {
        hovered = target;
        event.redraw = true;
        event.haptic = target.target != Target::None && !press_;
    }
    return event;
}
EmojiPicker::Event EmojiPicker::up(unsigned pointer, double x, double y) {
    Event event;
    if (!press_ || press_->pointer != pointer) {
        return move(pointer, x, y);
    }
    const auto press = *press_;
    const auto target = hit(x, y);
    event.redraw = true;
    if (press.popup) {
        const auto& item = items_[press.hit.index];
        event.recent = item.group == 0;
        if (press.choice) {
            const auto tone = static_cast<int>(*press.choice);
            event.type = item.entry->text(tone);
            event.typed = item.entry;
            event.tone = tone;
            tone_ = tone;
        }
    } else if (!press.dragging && target == press.hit) {
        if (target.target == Target::Cell) {
            const auto& item = items_[target.index];
            event.recent = item.group == 0;
            if (item.entry) {
                event.type = text(item);
                event.typed = item.entry;
            } else {
                event.clear = true;
            }
        } else if (target.target == Target::Tab) {
            scroll_ = std::clamp(layout_[target.index].start, 0.0, scroll_limit());
            chosen_group_ = target.index;
            reveal_tab(target.index);
        }
    }
    press_.reset();
    hovered_[pointer] = hit(x, y);
    return event;
}
EmojiPicker::Event EmojiPicker::tick(double now) {
    Event event;
    if (press_ && !press_->popup && !press_->dragging && press_->hit.target == Target::Cell &&
        now - press_->at >= hold_seconds) {
        const auto* entry = items_[press_->hit.index].entry;
        if (entry && entry->toned()) {
            press_->popup = true;
            event.redraw = event.haptic = true;
        }
    }
    return event;
}
bool EmojiPicker::scroll(double x, double y, double steps) {
    if (press_ || !std::isfinite(steps)) {
        return false;
    }
    if (grid_.contains(x, y)) {
        const double old = scroll_;
        scroll_ = std::clamp(scroll_ + steps * 2 * cell_, 0.0, scroll_limit());
        if (scroll_ == old) {
            return false;
        }
        chosen_group_.reset();
        reveal_tab(active_group());
    } else if (tabs_.contains(x, y)) {
        const double old = tab_scroll_;
        tab_scroll_ = std::clamp(tab_scroll_ + steps * (tab_width() + tab_spacing), 0.0, tab_limit());
        if (tab_scroll_ == old) {
            return false;
        }
    } else {
        return false;
    }
    hovered_.clear(); // Whatever was under the laser has moved.
    return true;
}
void EmojiPicker::cancel_pointer(unsigned pointer) {
    if (press_ && press_->pointer == pointer) {
        press_.reset();
    }
    hovered_.erase(pointer);
}
void EmojiPicker::cancel() {
    press_.reset();
    hovered_.clear();
}
EmojiPickerView EmojiPicker::view() const {
    EmojiPickerView v;
    v.grid = grid_;
    v.tabs = tabs_;
    v.scrolling_tabs = scrolling_tabs();
    v.glyphs = glyphs_;
    const bool dragging = press_ && press_->dragging;
    auto hovered = [&](Hit target) {
        return !dragging && std::any_of(hovered_.begin(), hovered_.end(),
                                        [&](const auto& item) { return item.second == target; });
    };
    auto pressed = [&](Hit target) {
        if (!press_ || dragging || press_->popup || press_->hit != target) {
            return false;
        }
        const auto over = hovered_.find(press_->pointer);
        return over != hovered_.end() && over->second == target;
    };
    const auto active = active_group();
    for (const auto g : shown_groups()) {
        const Hit target{Target::Tab, g};
        v.tab_list.push_back({tab_bounds(g), emoji_group_icon(static_cast<EmojiGroup>(g)), g == active,
                              hovered(target), pressed(target)});
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const auto bounds = cell_bounds(items_[i]);
        if (bounds.x + bounds.width <= grid_.x || bounds.x >= grid_.x + grid_.width) {
            continue;
        }
        const Hit target{Target::Cell, i};
        v.cells.push_back(
            {bounds, text(items_[i]), items_[i].entry == nullptr, hovered(target), pressed(target)});
    }
    if (loading_) {
        v.hint = "Loading emoji…";
        v.hint_bounds = grid_;
    } else if (sections_[0].empty()) {
        const auto& recent = layout_[0];
        v.hint = "Recently used emoji\nappear here";
        v.hint_bounds = {grid_.x + recent.start - scroll_, grid_.y, recent.columns * cell_,
                         grid_.height};
    }
    if (const auto popup = popup_bounds()) {
        EmojiPickerView::Popup p{*popup, {}};
        const auto* entry = items_[press_->hit.index].entry;
        for (std::size_t i = 0; i < tone_choices; ++i) {
            p.choices.push_back(
                {choice_bounds(i), entry->text(static_cast<int>(i)), false, press_->choice == i, false});
        }
        v.popup = std::move(p);
    }
    return v;
}
int EmojiPicker::first_column() const {
    return cell_ > 0 ? static_cast<int>(scroll_ / cell_) : 0;
}
int EmojiPicker::glyph_size() const {
    return std::max(8, static_cast<int>(std::lround(cell_ * .72)));
}
std::vector<std::string> EmojiPicker::glyph_order() const {
    std::vector<std::string> order;
    if (press_ && press_->popup) {
        const auto* entry = items_[press_->hit.index].entry;
        for (int tone = 0; tone < tone_choices; ++tone) {
            order.push_back(entry->text(tone));
        }
    }
    // Nearest the middle of the view first, so visible cells come before the rest.
    const double middle = grid_.x + grid_.width / 2;
    std::vector<std::size_t> indices(items_.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::vector<double> distance(items_.size());
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const auto bounds = cell_bounds(items_[i]);
        distance[i] = std::abs(bounds.x + bounds.width / 2 - middle);
    }
    std::stable_sort(indices.begin(), indices.end(),
                     [&](std::size_t a, std::size_t b) { return distance[a] < distance[b]; });
    for (const auto i : indices) {
        if (items_[i].entry) {
            order.push_back(text(items_[i]));
        }
    }
    return order;
}
} // namespace framekeyboard
