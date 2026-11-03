#include "framekeyboard/candidates.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
// A laser moving further than this from the press scrolls instead of picking.
constexpr double drag_distance = 20;
} // namespace
void CandidateStrip::set(Rect area, std::vector<std::string> texts, int selected, bool stale,
                         const Measure& measure, std::optional<Rect> list) {
    if (texts.empty()) {
        clear();
        return;
    }
    const bool same = texts == texts_;
    if (same && area == area_ && list == list_) {
        stale_ = stale;
        if (selected != selected_) {
            selected_ = selected;
            reveal(selected);
        }
        return;
    }
    if (!same) {
        texts_ = std::move(texts);
        starts_.clear();
        widths_.clear();
        double x = 0;
        for (const auto& text : texts_) {
            const double width = std::max(min_chip, std::ceil(measure(text)) + 2 * chip_padding);
            starts_.push_back(x);
            widths_.push_back(width);
            x += width + chip_spacing;
        }
        width_ = x - chip_spacing;
    }
    if (!same || list.has_value() != list_.has_value()) {
        // A chip pressed in the old list must not pick one of the new, and
        // opening or closing changes which way the strip scrolls.
        press_.reset();
        glide_.stop();
        scroll_ = 0;
        hovered_.clear();
    }
    area_ = area;
    list_ = list;
    place();
    stale_ = stale;
    selected_ = selected;
    scroll_ = std::clamp(scroll_, 0.0, scroll_limit());
    reveal(selected);
}
void CandidateStrip::place() {
    lines_.clear();
    line_starts_.clear();
    line_count_ = 0;
    if (!list_) {
        return;
    }
    // The row keeps the chips that fit from the start; the next one, and all
    // after it, wrap within the list's width.
    int line = 0;
    double x = 0;
    for (const double width : widths_) {
        const double room = line == 0 ? area_.width : list_->width;
        if (x > 0 && x + width > room) {
            ++line;
            x = 0;
        } else if (line == 0 && width > room) {
            line = 1; // Too wide for the row even alone.
        }
        lines_.push_back(line);
        line_starts_.push_back(x);
        x += width + chip_spacing;
    }
    line_count_ = line + 1;
}
void CandidateStrip::clear() {
    texts_.clear();
    starts_.clear();
    widths_.clear();
    width_ = 0;
    list_.reset();
    lines_.clear();
    line_starts_.clear();
    line_count_ = 0;
    selected_ = -1;
    stale_ = false;
    scroll_ = 0;
    press_.reset();
    glide_.stop();
    hovered_.clear();
}
double CandidateStrip::scroll_limit() const {
    if (list_) {
        // The last line and the row's own margin under it, inside the list.
        const double bottom =
            row_top() + (line_count_ - 1) * line_pitch + chip_height + (area_.height - chip_height) / 2;
        return std::max(0.0, bottom - (list_->y + list_->height));
    }
    return std::max(0.0, width_ - area_.width);
}
Rect CandidateStrip::chip_bounds(std::size_t index) const {
    // Whole pixels: the slow tail of a glide then leaves the view unchanged.
    if (list_) {
        const int line = lines_[index];
        const double origin = line == 0 ? area_.x : list_->x;
        const double scroll = line == 0 ? 0 : std::round(scroll_);
        return {origin + line_starts_[index], row_top() + line * line_pitch - scroll,
                std::min(widths_[index], line == 0 ? area_.width : list_->width), chip_height};
    }
    return {area_.x + starts_[index] - std::round(scroll_), row_top(), widths_[index], chip_height};
}
int CandidateStrip::chip_at(double x, double y) const {
    if (!contains(x, y)) {
        return -1;
    }
    for (std::size_t i = 0; i < texts_.size(); ++i) {
        // An open list's chips take presses only where they show: the row's
        // in the row, the others in the list below it.
        const auto& region = list_ && lines_[i] > 0 ? *list_ : area_;
        if (chip_bounds(i).contains(x, y) && region.contains(x, y)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
void CandidateStrip::reveal(int index) {
    if (index < 0 || index >= static_cast<int>(texts_.size()) || press_) {
        return;
    }
    const auto at = static_cast<std::size_t>(index);
    const double old = scroll_;
    if (list_) {
        if (lines_[at] > 0) {
            // Its line, with the row's margin above and below it.
            const double margin = (area_.height - chip_height) / 2;
            const double top = row_top() + lines_[at] * line_pitch - margin;
            const double bottom = top + chip_height + 2 * margin;
            if (top - scroll_ < list_->y) {
                scroll_ = top - list_->y;
            } else if (bottom - scroll_ > list_->y + list_->height) {
                scroll_ = bottom - (list_->y + list_->height);
            }
        }
    } else if (starts_[at] < scroll_) {
        scroll_ = starts_[at];
    } else if (starts_[at] + widths_[at] > scroll_ + area_.width) {
        scroll_ = starts_[at] + widths_[at] - area_.width;
    }
    scroll_ = std::clamp(scroll_, 0.0, scroll_limit());
    if (scroll_ != old) {
        glide_.stop();
    }
}
CandidateStrip::Event CandidateStrip::down(unsigned pointer, double x, double y, double now) {
    Event event;
    positions_[pointer] = {x, y};
    if (!contains(x, y) || press_) {
        return event;
    }
    // A press on a gliding row catches it, like a finger on a phone list.
    const bool caught = glide_.active();
    glide_.stop();
    scroll_ = std::clamp(scroll_, 0.0, scroll_limit());
    press_ = Press{pointer, list_ ? y : x, scroll_, caught ? -1 : chip_at(x, y), false, caught};
    glide_.track(now, scroll_);
    event.redraw = true;
    return event;
}
CandidateStrip::Event CandidateStrip::move(unsigned pointer, double x, double y, double now) {
    Event event;
    positions_[pointer] = {x, y};
    if (press_ && press_->pointer == pointer) {
        auto& press = *press_;
        const double at = list_ ? y : x;
        if (!press.dragging && std::abs(at - press.at) > drag_distance) {
            press.dragging = true;
            hovered_.clear();
            event.redraw = true;
        }
        if (press.dragging) {
            const double old = scroll_;
            scroll_ = std::clamp(press.scroll - (at - press.at), 0.0, scroll_limit());
            glide_.track(now, scroll_);
            event.redraw |= std::round(scroll_) != std::round(old);
            return event;
        }
    }
    const int chip = press_ && press_->dragging ? -1 : chip_at(x, y);
    auto& hovered = hovered_[pointer];
    if (hovered != chip) {
        hovered = chip;
        event.redraw = true;
    }
    return event;
}
CandidateStrip::Event CandidateStrip::up(unsigned pointer, double x, double y, double now) {
    if (!press_ || press_->pointer != pointer) {
        return move(pointer, x, y, now);
    }
    Event event;
    positions_[pointer] = {x, y};
    const auto press = *press_;
    press_.reset();
    event.redraw = true;
    if (press.dragging) {
        glide_.release(now, scroll_);
    } else if (!press.caught && !stale_ && press.chip >= 0 && chip_at(x, y) == press.chip) {
        event.chosen = press.chip;
    }
    hovered_[pointer] = chip_at(x, y);
    return event;
}
CandidateStrip::Event CandidateStrip::tick(double now) {
    Event event;
    const double old = std::round(scroll_);
    if (glide_.step(now, scroll_, scroll_limit())) {
        event.redraw = std::round(scroll_) != old;
        // The chips moved under the lasers.
        for (auto& [pointer, chip] : hovered_) {
            if (const auto at = positions_.find(pointer); at != positions_.end()) {
                chip = chip_at(at->second.first, at->second.second);
            }
        }
    }
    return event;
}
bool CandidateStrip::scroll(double x, double y, double steps) {
    if (!contains(x, y) || press_ || !std::isfinite(steps)) {
        return false;
    }
    glide_.stop(); // The stick takes over.
    const double old = scroll_;
    const double page = list_ ? list_->height : area_.width;
    scroll_ = std::clamp(scroll_ + steps * page / 3, 0.0, scroll_limit());
    if (scroll_ == old) {
        return false;
    }
    for (auto& [pointer, chip] : hovered_) {
        if (const auto at = positions_.find(pointer); at != positions_.end()) {
            chip = chip_at(at->second.first, at->second.second);
        }
    }
    return true;
}
std::optional<ScrollRoom> CandidateStrip::scroll_room(double x, double y) const {
    if (!contains(x, y) || scroll_limit() <= 0) {
        return {};
    }
    return ScrollRoom{scroll_ > 0, scroll_ < scroll_limit()};
}
void CandidateStrip::cancel_pointer(unsigned pointer) {
    if (press_ && press_->pointer == pointer) {
        press_.reset();
    }
    hovered_.erase(pointer);
    positions_.erase(pointer);
}
void CandidateStrip::cancel() {
    press_.reset();
    glide_.stop();
    scroll_ = std::clamp(scroll_, 0.0, scroll_limit());
    hovered_.clear();
    positions_.clear();
}
CandidateStripView CandidateStrip::view() const {
    CandidateStripView result;
    result.area = area_;
    result.list = list_;
    result.stale = stale_;
    for (std::size_t i = 0; i < texts_.size(); ++i) {
        const auto bounds = chip_bounds(i);
        const bool listed = list_ && lines_[i] > 0;
        if (listed ? bounds.y + bounds.height <= list_->y || bounds.y >= list_->y + list_->height
                   : bounds.x + bounds.width <= area_.x || bounds.x >= area_.x + area_.width) {
            continue;
        }
        // Stale chips take no press, so they show no hover either.
        const bool hovered =
            !stale_ && std::any_of(hovered_.begin(), hovered_.end(),
                                   [&](const auto& item) { return item.second == static_cast<int>(i); });
        result.chips.push_back({texts_[i], bounds, static_cast<int>(i) == selected_, hovered, listed});
    }
    return result;
}
} // namespace framekeyboard
