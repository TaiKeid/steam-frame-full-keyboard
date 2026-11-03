#include "field.hpp"
#include <algorithm>
#include <glib.h>
#include <pango/pango.h>

namespace framekeyboard::simulator {
namespace {
bool valid_text(const std::string& text) {
    return text.find('\0') == std::string::npos &&
           g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr);
}
bool space_at(const std::string& text, std::size_t offset) {
    return offset < text.size() && g_unichar_isspace(g_utf8_get_char(text.c_str() + offset));
}
} // namespace

std::optional<std::pair<std::size_t, std::size_t>> TextField::selection() const {
    if (!anchor_ || *anchor_ == caret_) {
        return {};
    }
    return std::pair{std::min(*anchor_, caret_), std::max(*anchor_, caret_)};
}
std::vector<std::size_t> TextField::stops() const {
    std::vector<std::size_t> result{0};
    const auto characters = g_utf8_strlen(text_.c_str(), static_cast<gssize>(text_.size()));
    if (characters <= 0) {
        return result;
    }
    // Pango's cursor positions are the grapheme cluster boundaries.
    std::vector<PangoLogAttr> attributes(static_cast<std::size_t>(characters) + 1);
    pango_get_log_attrs(text_.c_str(), static_cast<int>(text_.size()), -1, pango_language_get_default(),
                        attributes.data(), static_cast<int>(attributes.size()));
    const char* at = text_.c_str();
    for (std::size_t i = 1; i < attributes.size(); ++i) {
        at = g_utf8_next_char(at);
        if (attributes[i].is_cursor_position) {
            result.push_back(static_cast<std::size_t>(at - text_.c_str()));
        }
    }
    if (result.back() != text_.size()) {
        result.push_back(text_.size());
    }
    return result;
}
std::size_t TextField::line_start(std::size_t offset) const {
    const auto found = offset == 0 ? std::string::npos : text_.rfind('\n', offset - 1);
    return found == std::string::npos ? 0 : found + 1;
}
std::size_t TextField::line_end(std::size_t offset) const {
    return std::min(text_.find('\n', offset), text_.size());
}
void TextField::move_to(std::size_t offset, bool extend) {
    if (extend && !anchor_) {
        anchor_ = caret_;
    } else if (!extend) {
        anchor_.reset();
    }
    caret_ = offset;
    changed();
}
void TextField::erase_selection() {
    if (const auto range = selection()) {
        text_.erase(range->first, range->second - range->first);
        caret_ = range->first;
    }
    anchor_.reset();
}
void TextField::insert(const std::string& text) {
    if (text.empty() || !valid_text(text)) {
        return;
    }
    erase_selection();
    text_.insert(caret_, text);
    caret_ += text.size();
    changed();
}
void TextField::erase_backward() {
    if (selection()) {
        erase_selection();
    } else if (caret_ > 0) {
        const auto s = stops();
        const auto previous = *(std::lower_bound(s.begin(), s.end(), caret_) - 1);
        text_.erase(previous, caret_ - previous);
        caret_ = previous;
        anchor_.reset();
    } else {
        return;
    }
    changed();
}
void TextField::erase_forward() {
    if (selection()) {
        erase_selection();
    } else if (caret_ < text_.size()) {
        const auto s = stops();
        const auto next = *std::upper_bound(s.begin(), s.end(), caret_);
        text_.erase(caret_, next - caret_);
        anchor_.reset();
    } else {
        return;
    }
    changed();
}
void TextField::move_horizontal(int direction, bool extend, bool word) {
    if (const auto range = selection(); range && !extend && !word) {
        // An arrow without Shift leaves the selection at its own end.
        move_to(direction < 0 ? range->first : range->second, false);
        return;
    }
    const auto s = stops();
    auto step = [&](std::size_t from) -> std::size_t {
        if (direction < 0) {
            return from == 0 ? 0 : *(std::lower_bound(s.begin(), s.end(), from) - 1);
        }
        return from >= text_.size() ? text_.size() : *std::upper_bound(s.begin(), s.end(), from);
    };
    auto target = step(caret_);
    if (word) {
        // Over the spaces next to the caret, then to the far end of the word.
        auto next_space = [&](std::size_t at) { return space_at(text_, direction < 0 ? step(at) : at); };
        target = caret_;
        while (target != (direction < 0 ? 0 : text_.size()) && next_space(target)) {
            target = step(target);
        }
        while (target != (direction < 0 ? 0 : text_.size()) && !next_space(target)) {
            target = step(target);
        }
    }
    move_to(target, extend);
}
void TextField::move_vertical(int direction, bool extend) {
    const auto s = stops();
    const auto start = line_start(caret_);
    const auto column = column_.value_or(
        std::count_if(s.begin(), s.end(), [&](auto at) { return at > start && at <= caret_; }));
    std::size_t from, limit;
    if (direction < 0) {
        if (start == 0) {
            move_to(0, extend);
            column_ = column;
            return;
        }
        from = line_start(start - 1);
        limit = start - 1;
    } else {
        const auto end = line_end(caret_);
        if (end == text_.size()) {
            move_to(text_.size(), extend);
            column_ = column;
            return;
        }
        from = end + 1;
        limit = line_end(from);
    }
    // The same number of characters into the other line, or its end.
    auto target = from;
    auto it = std::upper_bound(s.begin(), s.end(), from);
    for (std::ptrdiff_t counted = 0; it != s.end() && *it <= limit && counted < column;
         ++it, ++counted) {
        target = *it;
    }
    move_to(target, extend);
    column_ = column;
}
void TextField::move_line_edge(bool end, bool extend, bool whole) {
    const auto target = whole ? (end ? text_.size() : 0) : (end ? line_end(caret_) : line_start(caret_));
    move_to(target, extend);
}
void TextField::select_all() {
    anchor_ = 0;
    caret_ = text_.size();
    changed();
}
void TextField::copy() {
    if (const auto range = selection()) {
        clipboard_ = text_.substr(range->first, range->second - range->first);
    }
}
void TextField::cut() {
    if (selection()) {
        copy();
        erase_selection();
        changed();
    }
}
void TextField::paste() {
    insert(clipboard_);
}
void TextField::set_clipboard(std::string text) {
    if (valid_text(text)) {
        clipboard_ = std::move(text);
    }
}
void TextField::place_caret(std::size_t offset) {
    const auto s = stops();
    move_to(*(std::upper_bound(s.begin(), s.end(), std::min(offset, text_.size())) - 1), false);
}
void TextField::clear() {
    text_.clear();
    caret_ = 0;
    anchor_.reset();
    changed();
}

bool FieldSink::ctrl() const {
    return held_.contains(key_code("ControlLeft")) || held_.contains(key_code("ControlRight"));
}
void FieldSink::apply_key(int code, int value) {
    if (is_modifier(code)) {
        if (value != 0) {
            held_.insert(code);
        } else {
            held_.erase(code);
        }
        return;
    }
    if (value == 0) {
        return; // Every edit happens on the press and its repeats.
    }
    const bool shift = held_.contains(key_code("ShiftLeft")) || held_.contains(key_code("ShiftRight"));
    const bool control = ctrl();
    // The field has no Alt or Super shortcuts.
    for (const char* other : {"AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (held_.contains(key_code(other))) {
            return;
        }
    }
    const auto& name = key_name(code);
    if (control && name == "KeyA") {
        field_.select_all();
    } else if (control && name == "KeyC") {
        field_.copy();
    } else if (control && name == "KeyX") {
        field_.cut();
    } else if (control && name == "KeyV") {
        if (!field_.clipboard().empty()) {
            field_.paste();
            ++served_;
        }
    } else if (name == "Backspace") {
        field_.erase_backward();
    } else if (name == "Delete") {
        field_.erase_forward();
    } else if (name == "ArrowLeft" || name == "ArrowRight") {
        field_.move_horizontal(name == "ArrowLeft" ? -1 : 1, shift, control);
    } else if (name == "ArrowUp" || name == "ArrowDown") {
        field_.move_vertical(name == "ArrowUp" ? -1 : 1, shift);
    } else if (name == "Home" || name == "End") {
        field_.move_line_edge(name == "End", shift, control);
    } else if (!control && (name == "Enter" || name == "NumpadEnter")) {
        field_.insert("\n");
    } else if (!control && name == "Tab") {
        field_.insert("\t");
    }
}
void FieldSink::apply_text(std::size_t deletions, const std::string& text) {
    // Each deletion acts as a Backspace in the application, as the
    // compositor's delete-left action does.
    for (std::size_t i = 0; i < deletions; ++i) {
        field_.erase_backward();
    }
    field_.insert(text);
}
Delivery FieldSink::deliver_key(int code, int value) {
    if (value == 0) {
        // A release lands at once, and ends a press of it still unconfirmed.
        if (pending_ && !pending_->text && pending_->code == code) {
            pending_.reset();
        }
        apply_key(code, 0);
        return Delivery::Success;
    }
    if (!slow_) {
        pending_.reset();
        apply_key(code, value);
        return Delivery::Success;
    }
    // The keyboard asks again for the same press until it is confirmed.
    if (pending_ && !pending_->text && pending_->code == code && pending_->value == value) {
        if (now_ < pending_->ready) {
            return Delivery::Waiting;
        }
        pending_.reset();
        apply_key(code, value);
        return Delivery::Success;
    }
    pending_ = Pending{code, value, {}, 0, now_ + slow_delay};
    return Delivery::Waiting;
}
bool FieldSink::deliver_text(std::size_t deletions, const std::string& text) {
    // Refused before submission, as the transport refuses it: nothing waits.
    if (text.empty() || !valid_text(text) ||
        g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size())) > 32 || deletions > 64) {
        pending_.reset();
        return false;
    }
    if (!slow_) {
        pending_.reset();
        apply_text(deletions, text);
        return true;
    }
    // As GamescopeText: false while the same request waits, then true once.
    if (pending_ && pending_->text == text && pending_->deletions == deletions) {
        if (now_ < pending_->ready) {
            return false;
        }
        pending_.reset();
        apply_text(deletions, text);
        return true;
    }
    pending_ = Pending{0, 0, text, deletions, now_ + slow_delay};
    return false;
}
std::uint64_t FieldSink::write(const std::string& text) {
    field_.set_clipboard(text);
    return ++writes_;
}
} // namespace framekeyboard::simulator
