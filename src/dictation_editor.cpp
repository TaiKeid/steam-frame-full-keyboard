#include "framekeyboard/dictation_editor.hpp"

#include <algorithm>
#include <glib.h>

namespace framekeyboard {
namespace {
std::size_t code_points(const std::string& text) {
    return static_cast<std::size_t>(g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size())));
}
// The first `count` code points of valid UTF-8 text.
std::string head(const std::string& text, std::size_t count) {
    const char* end = g_utf8_offset_to_pointer(text.c_str(), static_cast<glong>(count));
    return std::string(text.c_str(), end);
}
bool is_space(gunichar c) {
    return g_unichar_isspace(c);
}
// Characters that attach to the word before them, so no space precedes them.
bool closes(gunichar c) {
    return c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || c == ')' ||
           c == ']' || c == '}' || c == '%' || c == '\'' || c == '"';
}
} // namespace

std::size_t DictationEditor::length() const {
    return code_points(text_);
}

void DictationEditor::replace(std::size_t start, std::size_t end, const std::string& insertion) {
    text_.replace(start, end - start, insertion);
    if (start < uncertain_) {
        uncertain_ = 0; // the user is correcting the marked text
    }
    const auto removed = end - start;
    std::erase_if(phrases_, [&](const Phrase& p) {
        // An edit inside a phrase, or one that removes part of it, invalidates
        // it; one touching only its edge leaves it intact.
        return start < p.start + p.size && end > p.start;
    });
    for (auto& phrase : phrases_) {
        if (phrase.start >= end) {
            phrase.start = phrase.start - removed + insertion.size();
        }
    }
}

bool DictationEditor::type(const std::string& input) {
    if (!g_utf8_validate(input.c_str(), static_cast<gssize>(input.size()), nullptr)) {
        return false;
    }
    std::string clean;
    for (const char* p = input.c_str(); *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (c == '\n' || !g_unichar_iscntrl(c)) {
            clean.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        }
    }
    const auto room = hard_limit - std::min(hard_limit, length());
    clean = head(clean, std::min(room, code_points(clean)));
    if (clean.empty()) {
        return false;
    }
    replace(cursor_, cursor_, clean);
    cursor_ += clean.size();
    return true;
}

bool DictationEditor::backspace() {
    if (cursor_ == 0) {
        return false;
    }
    const auto start = static_cast<std::size_t>(
        g_utf8_find_prev_char(text_.c_str(), text_.c_str() + cursor_) - text_.c_str());
    replace(start, cursor_, "");
    cursor_ = start;
    return true;
}

bool DictationEditor::erase_forward() {
    if (cursor_ >= text_.size()) {
        return false;
    }
    const auto end = static_cast<std::size_t>(g_utf8_next_char(text_.c_str() + cursor_) - text_.c_str());
    replace(cursor_, end, "");
    return true;
}

void DictationEditor::set_cursor(std::size_t offset) {
    offset = std::min(offset, text_.size());
    // A continuation byte is never a boundary; step back to the character's start.
    while (offset > 0 && (static_cast<unsigned char>(text_[offset]) & 0xC0) == 0x80) {
        --offset;
    }
    cursor_ = offset;
}

void DictationEditor::left() {
    if (cursor_ > 0) {
        cursor_ = static_cast<std::size_t>(
            g_utf8_find_prev_char(text_.c_str(), text_.c_str() + cursor_) - text_.c_str());
    }
}

void DictationEditor::right() {
    if (cursor_ < text_.size()) {
        cursor_ = static_cast<std::size_t>(g_utf8_next_char(text_.c_str() + cursor_) - text_.c_str());
    }
}

void DictationEditor::clear() {
    text_.clear();
    cursor_ = 0;
    phrases_.clear();
    uncertain_ = 0;
}

void DictationEditor::erase_front(std::size_t bytes) {
    bytes = std::min(bytes, text_.size());
    replace(0, bytes, "");
    cursor_ = cursor_ > bytes ? cursor_ - bytes : 0;
}

void DictationEditor::mark_uncertain(std::size_t bytes) {
    uncertain_ = std::min(bytes, text_.size());
}

bool DictationEditor::insert_phrase(const std::string& input, bool spaced) {
    if (!g_utf8_validate(input.c_str(), static_cast<gssize>(input.size()), nullptr)) {
        return false;
    }
    gchar* stripped = g_strstrip(g_strdup(input.c_str()));
    std::string phrase(stripped);
    g_free(stripped);
    // Text after the cursor, ignoring spaces, means the sentence continues.
    bool follows = false;
    for (const char* p = text_.c_str() + cursor_; *p; p = g_utf8_next_char(p)) {
        if (!is_space(g_utf8_get_char(p))) {
            follows = true;
            break;
        }
    }
    if (follows && !phrase.empty()) {
        const char* last = g_utf8_find_prev_char(phrase.c_str(), phrase.c_str() + phrase.size());
        const gunichar c = g_utf8_get_char(last);
        if (c == '.' || c == 0x3002) { // full stop or 。
            phrase.erase(static_cast<std::size_t>(last - phrase.c_str()));
        }
    }
    if (phrase.empty()) {
        return false;
    }
    if (spaced) {
        const gunichar first = g_utf8_get_char(phrase.c_str());
        if (cursor_ > 0) {
            const gunichar before =
                g_utf8_get_char(g_utf8_find_prev_char(text_.c_str(), text_.c_str() + cursor_));
            if (!is_space(before) && !closes(first)) {
                phrase.insert(0, " ");
            }
        }
        if (cursor_ < text_.size()) {
            const gunichar after = g_utf8_get_char(text_.c_str() + cursor_);
            if (!is_space(after) && !closes(after)) {
                phrase += ' ';
            }
        }
    }
    const auto room = hard_limit - std::min(hard_limit, length());
    phrase = head(phrase, std::min(room, code_points(phrase)));
    if (phrase.empty()) {
        return false;
    }
    replace(cursor_, cursor_, phrase);
    phrases_.push_back({cursor_, phrase.size(), phrase});
    cursor_ += phrase.size();
    return true;
}

bool DictationEditor::can_undo() const {
    return !phrases_.empty();
}

bool DictationEditor::undo_phrase() {
    if (phrases_.empty()) {
        return false;
    }
    const auto phrase = phrases_.back();
    phrases_.pop_back();
    replace(phrase.start, phrase.start + phrase.size, "");
    if (cursor_ >= phrase.start + phrase.size) {
        cursor_ -= phrase.size;
    } else if (cursor_ > phrase.start) {
        cursor_ = phrase.start;
    }
    return true;
}

std::string DictationEditor::context(std::size_t limit) const {
    const std::string before = text_.substr(0, cursor_);
    const auto count = code_points(before);
    if (count <= limit) {
        return before;
    }
    return std::string(g_utf8_offset_to_pointer(before.c_str(), static_cast<glong>(count - limit)));
}
} // namespace framekeyboard
