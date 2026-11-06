#include "framekeyboard/output.hpp"

#include <algorithm>
#include <exception>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <utility>

namespace framekeyboard {
namespace {
constexpr auto waiting_status =
    "Waiting for compositor text delivery. Try the key again when it completes.";
// Gamescope resets its temporary keymap after 100 ms idle. Count from the
// previous result, with some headroom, rather than queueing a burst of maps.
constexpr double composition_gap = .12;
} // namespace
OutputController::~OutputController() {
    try {
        cancel();
    } catch (...) {
    }
}
void OutputController::status(std::string message) {
    if (status_ != message) {
        status_ = std::move(message);
        status_changed_ = true;
    }
}
std::optional<std::string> OutputController::take_status() {
    if (std::exchange(status_changed_, false)) {
        return status_;
    }
    return {};
}
bool OutputController::admit(bool produces_output, double now) {
    now_ = now;
    if (!produces_output || !enabled_) {
        return true;
    }
    drain();
    if (failed_ && retry_.empty()) {
        failed_ = false; // A new deliberate press can follow a discarded native action.
    }
    if (failed_) {
        status("Text not sent. Check the target, then press Enter to retry. Later input was discarded.");
        return false;
    }
    // A cancelled request may still have a Wayland sync outstanding. It has no
    // owned result to consume or retry; keep B12/B14's explicit rejection.
    if (queue_.empty() && target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    if (queue_.size() >= capacity || active_.size() >= capacity) {
        status("Input queue full. Wait for delivery, then try the key again.");
        return false;
    }
    return true;
}
void OutputController::set_enabled(bool value) {
    if (enabled_ && !value) {
        cancel();
    }
    enabled_ = value;
}
void OutputController::send(int code, int value) {
    if (value == 0) {
        if (auto held = direct_.find(code); held != direct_.end()) {
            held->second->held = false;
            release_codes(*held->second);
            direct_.erase(held);
        }
    } else if (value == 1 && enabled_ && !direct_.contains(code) && admit(true)) {
        auto entry = std::make_shared<Entry>();
        entry->codes = {code};
        entry->held = true;
        direct_[code] = entry;
        queue_.push_back(entry);
        ++queued_;
        drain();
    }
}
void OutputController::native_press(unsigned pointer, const std::vector<int>& codes, int repeat_code,
                                    double now) {
    now_ = now;
    if (!enabled_ || codes.empty()) {
        return;
    }
    auto entry = std::make_shared<Entry>();
    entry->codes = codes;
    entry->repeat_code = repeat_code;
    entry->held = true;
    active_[pointer] = entry;
    queue_.push_back(entry);
    ++queued_;
    drain();
}
bool OutputController::sendable(const std::string& text) {
    return text.find('\0') == std::string::npos &&
           g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) &&
           static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1)) <= commit_limit;
}
bool OutputController::taps_then_text(int code, std::size_t count, const std::string& text) {
    if (!enabled_ || failed_ || text.empty() || !sendable(text) ||
        queue_.size() + count + 1 > capacity) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto entry = std::make_shared<Entry>();
        entry->codes = {code}; // Not held: released as soon as it is delivered.
        queue_.push_back(entry);
        ++queued_;
    }
    return enqueue_text(text); // Drains the whole sequence.
}
bool OutputController::tap(const std::vector<int>& codes) {
    if (!enabled_ || failed_ || codes.empty() || queue_.size() >= capacity) {
        return false;
    }
    auto entry = std::make_shared<Entry>();
    entry->codes = codes; // Not held: released as soon as all are delivered.
    queue_.push_back(entry);
    ++queued_;
    drain();
    return true;
}
bool OutputController::enqueue_text(const std::string& text, std::shared_ptr<Entry> entry) {
    if (text.empty()) {
        return true;
    }
    if (!enabled_) {
        return false; // No target: the app shakes Focus mode's icon instead
                      // (App::refuse_without_target).
    }
    if (failed_ || queue_.size() >= capacity || !sendable(text)) {
        status("Text not queued. Check the target and input queue.");
        return false;
    }
    if (!entry) {
        entry = std::make_shared<Entry>();
    }
    entry->text = text;
    queue_.push_back(entry);
    ++queued_;
    drain();
    return true; // Ownership transferred, including to the failure retry buffer.
}
bool OutputController::text_press(unsigned pointer, const std::string& text, double now) {
    now_ = now;
    auto entry = std::make_shared<Entry>();
    entry->held = true;
    if (!text.empty() && enabled_) {
        active_[pointer] = entry;
    }
    return enqueue_text(text, entry);
}
bool OutputController::replace_text(std::size_t delete_before, const std::string& text) {
    if (failed_ || (queue_.empty() && target_.output_blocked())) {
        return false;
    }
    auto entry = std::make_shared<Entry>();
    entry->delete_before = delete_before;
    return enqueue_text(text, entry);
}
bool OutputController::commit_text(const std::string& text) {
    if (failed_) {
        return false;
    }
    if (queue_.empty() && target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    if (text.size() > max_commit_codepoints * 4 || text.find('\0') != std::string::npos ||
        !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr)) {
        status("Text not queued. Check the target and input queue.");
        return false;
    }
    const auto count = static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1));
    if (count > commit_limit) {
        const auto chunks = (count + commit_limit - 1) / commit_limit;
        if (count > max_commit_codepoints || !can_queue(chunks)) {
            status("Text not queued. Check the target and input queue.");
            return false;
        }
        // Build the complete queue before changing ownership or draining it.
        // A partial admission followed by failure would leave the composer
        // unable to know which prefix may already have been delivered.
        auto pending = queue_;
        const char* start = text.c_str();
        for (std::size_t left = count; left > 0;) {
            const auto size = std::min(commit_limit, left);
            const char* end = g_utf8_offset_to_pointer(start, static_cast<glong>(size));
            auto entry = std::make_shared<Entry>();
            entry->text.assign(start, end);
            entry->paced = true;
            pending.push_back(std::move(entry));
            start = end;
            left -= size;
        }
        queue_.swap(pending);
        drain();
        // Even an immediate first-chunk refusal belongs to the retry queue:
        // the entire batch was admitted, so the composer must release it.
        return true;
    }
    const bool idle = queue_.empty();
    const bool accepted = enqueue_text(text);
    if (accepted && idle && failed_ && retry_.size() == 1 && retry_.front()->text == text) {
        // A synchronous preedit rejection has not transferred ownership. The
        // editor retains it and can attempt another explicit commit, as before.
        retry_.clear();
        failed_ = false;
        status_.clear();
        status_changed_ = false;
        return false;
    }
    return accepted;
}
std::string OutputController::retry_text() const {
    std::string text;
    for (const auto& entry : retry_) {
        text += entry->text;
    }
    return text;
}
bool OutputController::retry() {
    if (!enabled_ || retry_.empty()) {
        return false;
    }
    if (target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    failed_ = false;
    queue_ = std::move(retry_);
    retry_.clear();
    drain();
    return !failed_;
}
void OutputController::release_codes(Entry& entry, std::optional<int> only) {
    for (auto it = entry.acquired.rbegin(); it != entry.acquired.rend(); ++it) {
        const int code = *it;
        if (only && code != *only) {
            continue;
        }
        if (auto held = delivered_.find(code); held != delivered_.end()) {
            if (--held->second == 0) {
                delivered_.erase(held);
                target_.deliver_key(code, 0);
            }
        }
    }
    if (only) {
        std::erase(entry.acquired, *only);
    } else {
        entry.acquired.clear();
    }
}
void OutputController::release(unsigned pointer) {
    if (auto found = active_.find(pointer); found != active_.end()) {
        auto& entry = *found->second;
        entry.held = false;
        // A release never needs queue capacity or waits behind unrelated text.
        release_codes(entry);
        active_.erase(found);
    }
}
void OutputController::release_modifier(int code) {
    for (auto& [pointer, entry] : active_) {
        (void)pointer;
        entry->released_modifiers.insert(code);
        release_codes(*entry, code);
    }
}
void OutputController::drop_holds() {
    for (auto& [pointer, entry] : active_) {
        (void)pointer;
        entry->held = false;
    }
    active_.clear();
    for (auto& [code, entry] : direct_) {
        (void)code;
        entry->held = false;
    }
    direct_.clear();
    // Non-modifiers before modifiers. Attempt every release even if one fails.
    std::exception_ptr error;
    for (bool modifiers : {false, true}) {
        for (const auto& [code, count] : delivered_) {
            (void)count;
            if (is_modifier(code) != modifiers) {
                continue;
            }
            try {
                target_.deliver_key(code, 0);
            } catch (...) {
                error = std::current_exception();
            }
        }
    }
    delivered_.clear();
    if (error) {
        std::rethrow_exception(error);
    }
}
void OutputController::release_holds() {
    std::exception_ptr error;
    auto attempt = [&](auto&& release) {
        try {
            release();
        } catch (...) {
            error = error ? error : std::current_exception();
        }
    };
    // Attempt every release even if one fails. Queued repeats of these holds
    // are dropped when drained, since their owners are no longer held.
    while (!active_.empty()) {
        const auto pointer = active_.begin()->first;
        attempt([&] { release(pointer); });
        active_.erase(pointer);
    }
    while (!direct_.empty()) {
        const auto held = direct_.begin()->second;
        direct_.erase(direct_.begin());
        held->held = false;
        attempt([&] { release_codes(*held); });
    }
    if (error) {
        std::rethrow_exception(error);
    }
}
// A run of output is for the window focused when it began (0: none named).
// Unknown since then counts as moved, so nothing goes out unchecked.
bool OutputController::window_moved() {
    if (queue_.empty()) {
        window_ = 0;
        return false;
    }
    if (!window_) {
        window_ = target_.focused_window();
        return false;
    }
    return target_.focused_window() != window_;
}
void OutputController::cancel() {
    ++cancels_;
    window_ = 0;
    queue_.clear();
    retry_.clear();
    failed_ = false;
    target_.cancel_delivery();
    drop_holds();
    status("");
}
void OutputController::fail(std::size_t delivered, bool uncertain) {
    ++failures_;
    retry_.clear();
    retry_uncertain_ = uncertain;
    bool boundary = false, discarded = false;
    for (auto& entry : queue_) {
        // A replacement deletes before it inserts, which is not safe to repeat:
        // a timeout leaves its delivery unknown, and if it was applied a retry
        // would delete text before the word. Like native actions, it ends the
        // retained prefix; the word can be chosen or typed again.
        if (entry->text.empty() || entry->delete_before) {
            boundary = true;
        }
        if (!boundary && (!entry->repeat || entry == queue_.front())) {
            auto saved = std::make_shared<Entry>();
            saved->text = entry->text;
            saved->paced = entry->paced;
            if (entry == queue_.front() && delivered) {
                // Retyping the acknowledged characters would duplicate them. A
                // count beyond the text must not walk past its end.
                const auto length = static_cast<std::size_t>(g_utf8_strlen(entry->text.c_str(), -1));
                const char* rest = g_utf8_offset_to_pointer(
                    entry->text.c_str(), static_cast<glong>(std::min(delivered, length)));
                saved->text.erase(0, static_cast<std::size_t>(rest - entry->text.c_str()));
            }
            if (!saved->text.empty()) {
                retry_.push_back(saved);
            }
        } else {
            discarded = true;
        }
    }
    queue_.clear();
    window_ = 0; // A retry is for the window focused then.
    failed_ = true;
    discarded_ = discarded;
    drop_holds();
    status(retry_.empty() ? "Input not sent. Queued input was discarded."
           : discarded    ? "Text not sent. Check the target, then press Enter to retry. "
                            "Queued actions and later text were discarded."
                          : "Text not sent. Check the target, then press Enter to retry.");
}
bool OutputController::settling() const {
    if (queue_.empty() || queue_.front()->started) {
        return false;
    }
    return paced_time_pending_ || now_ < text_ready_at_ ||
           (queue_.front()->paced &&
            (text_time_pending_ || (last_text_at_ >= 0 && now_ < last_text_at_ + composition_gap)));
}
void OutputController::note_text_time(bool paced, bool fresh_tick) {
    if (fresh_tick) {
        last_text_at_ = now_;
        if (paced) {
            text_ready_at_ = now_ + composition_gap;
        }
    } else {
        // Toolbar commits and synchronous results can happen long after the
        // last tick. A cached clock must not backdate their settling period.
        text_time_pending_ = true;
        paced_time_pending_ |= paced;
    }
}
void OutputController::drain(bool fresh_tick) {
    if (!enabled_ || failed_) {
        return;
    }
    // Another window took focus while this run went out, so the rest would
    // reach it. Text the target finished before then arrived in the window
    // before: the loop still collects its result, and stops what follows.
    const bool moved = window_moved();
    while (!queue_.empty()) {
        auto entry = queue_.front();
        if (moved && !entry->submitted) {
            target_.cancel_delivery(); // ends a native key waiting for its result
            fail(0, false);            // no text of it went out
            return;
        }
        if (!entry->started && entry->repeat) {
            auto owner = entry->owner.lock();
            if (!owner || !owner->held) {
                queue_.pop_front();
                continue;
            }
        }
        if (settling()) {
            return;
        }
        if (!entry->started && target_.output_blocked()) {
            return;
        }
        if (!entry->text.empty()) {
            // Never attempt a Unicode commit while a physical chord is held.
            if (!delivered_.empty()) {
                return;
            }
            if (!entry->started) {
                note_text_time(entry->paced, fresh_tick);
                entry->started = true;
            }
            if (!(entry->delete_before ? target_.replace_text(entry->delete_before, entry->text)
                                       : target_.commit_text(entry->text))) {
                if (target_.text_pending()) {
                    if (moved) {
                        // Still going out: its acknowledged characters arrived,
                        // and one in flight may have; between characters, none
                        // is. Read both before the cancel ends the commit.
                        const auto delivered = target_.delivered_characters();
                        const bool uncertain = target_.character_in_flight();
                        target_.cancel_delivery();
                        fail(delivered, uncertain);
                        return;
                    }
                    entry->submitted = true;
                    return;
                }
                note_text_time(entry->paced, fresh_tick);
                // A refusal before submission sent nothing; the target may
                // still report the count of an earlier, finished commit.
                fail(entry->submitted ? target_.delivered_characters() : 0, entry->submitted);
                return;
            }
            note_text_time(entry->paced, fresh_tick);
        } else {
            while (entry->next < entry->codes.size()) {
                const int code = entry->codes[entry->next];
                if (!entry->waiting) {
                    entry->started = true;
                    entry->acquired.push_back(code);
                    if (delivered_[code]++ != 0) {
                        ++entry->next;
                        continue;
                    }
                }
                const auto result = target_.deliver_key(code, entry->repeat ? 2 : 1);
                if (result == Delivery::Waiting) {
                    entry->waiting = true;
                    return;
                }
                entry->waiting = false;
                if (result == Delivery::Failure) {
                    fail();
                    return;
                }
                ++entry->next;
            }
        }
        queue_.pop_front();
        ++completed_;
        if (entry->held) {
            entry->repeat_at = now_ + .5;
            for (int code : entry->released_modifiers) {
                release_codes(*entry, code);
            }
        } else {
            release_codes(*entry);
        }
        if (auto owner = entry->owner.lock()) {
            owner->repeat_at = now_ + .04;
        }
    }
    window_ = 0; // The run is out; the next one belongs to the window then.
    if (status_ != waiting_status || !target_.output_blocked()) {
        status("");
    }
}
bool OutputController::tick(double now) {
    now_ = now;
    // Anchor out-of-tick submissions/results to an observed frame, including
    // after cancel: only timestamps survive, never old text or target state.
    if (std::exchange(text_time_pending_, false)) {
        last_text_at_ = now;
    }
    if (std::exchange(paced_time_pending_, false)) {
        text_ready_at_ = now + composition_gap;
    }
    const auto completed = completed_;
    drain(true);
    if (queue_.empty() && !failed_ && enabled_) {
        if (status_ == waiting_status && !target_.output_blocked()) {
            status("");
        }
        std::set<int> repeated;
        for (const auto& [pointer, held] : active_) {
            (void)pointer;
            if (!held->started || !held->held || now < held->repeat_at ||
                (held->text.empty() && !held->repeat_code)) {
                continue;
            }
            if (held->text.empty() && !repeated.insert(held->repeat_code).second) {
                continue;
            }
            auto entry = std::make_shared<Entry>();
            entry->text = held->text;
            entry->repeat = true;
            entry->owner = held;
            if (entry->text.empty()) {
                // Repeat borrows the original hold, so it never owns another up.
                const auto result = target_.deliver_key(held->repeat_code, 2);
                if (result == Delivery::Failure) {
                    fail();
                    break;
                }
                if (result == Delivery::Waiting) {
                    entry->codes = {held->repeat_code};
                    entry->started = entry->waiting = true;
                    queue_.push_back(entry);
                    ++queued_;
                }
            } else {
                queue_.push_back(entry);
                ++queued_;
            }
            held->repeat_at = now + .04;
            if (!queue_.empty()) {
                drain(true);
                break;
            }
        }
    }
    // Waiting for a chunk's settling deadline does not change anything the UI
    // can draw. Keep normal pending-delivery behavior and report real progress.
    return status_changed_ || (!queue_.empty() && (!settling() || completed_ != completed));
}
} // namespace framekeyboard
