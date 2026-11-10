#pragma once
#include "output.hpp"
#include <deque>
#include <glib.h>
#include <string>

namespace framekeyboard {
// A bounded receipt ledger, not a record of admitted-but-unacknowledged text.
// The owner invalidates it at every field/edit/input-session boundary.
class DeliveredContext {
  public:
    struct Snapshot {
        std::uint64_t completed{}, failures{}, cancels{};
        std::size_t pending{};
    };
    struct Ticket {
        Snapshot before;
        std::uint64_t generation{};
    };
    static Snapshot snapshot(const OutputController& output) {
        return {output.completed(), output.failures(), output.cancels(), output.pending_entries()};
    }
    static std::string tail(const std::string& text) {
        const auto size = g_utf8_strlen(text.c_str(), -1);
        return size <= 2 ? text : std::string(g_utf8_offset_to_pointer(text.c_str(), size - 2));
    }
    bool active() const { return !text_.empty() || !pending_.empty(); }
    bool waiting() const { return !pending_.empty(); }
    std::uint64_t generation() const { return generation_; }
    const std::string& text() const { return text_; }
    void clear(Snapshot now) {
        text_.clear();
        pending_.clear();
        seen_ = now;
        initialized_ = true;
        ++generation_;
    }
    void observe(Snapshot now) {
        if (!initialized_) {
            clear(now);
            return;
        }
        const auto end = pending_.empty() ? seen_.completed : pending_.back().completed;
        if (now.failures != seen_.failures || now.cancels != seen_.cancels || now.completed > end ||
            now.pending != end - now.completed) {
            clear(now); // Foreign output, a partial failure or cancellation is uncertainty.
            return;
        }
        while (!pending_.empty() && pending_.front().completed <= now.completed) {
            const auto& receipt = pending_.front();
            if (receipt.generation == generation_) {
                text_ = tail(text_ + receipt.tail);
            }
            pending_.pop_front();
        }
        seen_ = now;
    }
    Ticket begin(Snapshot before) {
        observe(before);
        return {before, generation_};
    }
    void admitted(Ticket ticket, Snapshot after, const std::string& text) {
        if (ticket.generation != generation_) {
            return; // A cleared input epoch can never be resurrected by its old receipt.
        }
        if (after.failures != ticket.before.failures || after.cancels != ticket.before.cancels ||
            pending_.size() >= OutputController::capacity) {
            clear(after);
            return;
        }
        const auto count = static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1));
        if (!count) {
            return;
        }
        const auto chunks =
            (count + OutputController::commit_limit - 1) / OutputController::commit_limit;
        pending_.push_back(
            {ticket.before.completed + ticket.before.pending + chunks, generation_, tail(text)});
        observe(after);
    }

  private:
    struct Receipt {
        std::uint64_t completed{}, generation{};
        std::string tail;
    };
    bool initialized_{};
    std::uint64_t generation_{};
    Snapshot seen_;
    std::string text_;
    std::deque<Receipt> pending_;
};
} // namespace framekeyboard
