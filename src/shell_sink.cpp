#include "framekeyboard/shell_sink.hpp"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <utility>

namespace framekeyboard {
namespace {
const char* modifier_name(int code) {
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return "Ctrl";
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return "Shift";
    case KEY_LEFTALT:
        return "Alt";
    case KEY_RIGHTALT:
        return "AltGr";
    default:
        return "Super";
    }
}
} // namespace

void ShellRouter::set_on(bool on) {
    if (on == on_) {
        return;
    }
    on_ = on;
    in_flight_.reset();
    pending_.clear();
    modifiers_.clear();
}

void ShellRouter::set_context(std::string context) {
    if (context != context_) {
        // Nothing typed for the old field may arrive as the new one's.
        in_flight_.reset();
        pending_.clear();
        modifiers_.clear();
    }
    context_ = std::move(context);
}

std::vector<ShellRouter::Event> ShellRouter::take_events() {
    return std::exchange(pending_, {});
}

void ShellRouter::acknowledge(const std::string& context, std::uint64_t seq) {
    if (in_flight_ && context == context_ && seq >= in_flight_->event.seq &&
        in_flight_->answer == InFlight::Answer::None) {
        in_flight_->answer = InFlight::Answer::Accepted;
    }
}

void ShellRouter::reject(const std::string& context, std::uint64_t seq) {
    if (in_flight_ && context == context_ && seq == in_flight_->event.seq &&
        in_flight_->answer == InFlight::Answer::None) {
        in_flight_->answer = InFlight::Answer::Rejected;
    }
}

std::vector<std::string> ShellRouter::modifier_names() const {
    std::vector<std::string> names;
    for (const int code : modifiers_) {
        std::string name = modifier_name(code);
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

// None while the offered event waits; true or false once the shell answered.
std::optional<bool> ShellRouter::answer(const Event& offered) {
    if (in_flight_) {
        const auto& sent = in_flight_->event;
        const bool same = sent.kind == offered.kind && sent.text == offered.text &&
                          sent.key == offered.key && sent.repeat == offered.repeat;
        if (!same) {
            return std::nullopt; // the queue offers one entry at a time; wait for it
        }
        if (in_flight_->answer == InFlight::Answer::None) {
            return std::nullopt;
        }
        const bool accepted = in_flight_->answer == InFlight::Answer::Accepted;
        in_flight_.reset();
        return accepted;
    }
    Event event = offered;
    event.seq = ++seq_;
    event.context = context_;
    pending_.push_back(event);
    in_flight_ = InFlight{std::move(event)};
    return std::nullopt;
}

bool ShellRouter::commit_text(const std::string& text) {
    if (!on_) {
        return inner_.commit_text(text);
    }
    Event event;
    event.kind = Event::Kind::Commit;
    event.text = text;
    const auto result = answer(event);
    return result.value_or(false);
}

Delivery ShellRouter::deliver_key(int code, int value) {
    if (!on_) {
        return inner_.deliver_key(code, value);
    }
    if (is_modifier(code)) {
        // A modifier alone is never an event; it rides on the next key.
        if (value == 1 && std::find(modifiers_.begin(), modifiers_.end(), code) == modifiers_.end()) {
            modifiers_.push_back(code);
        } else if (value == 0) {
            std::erase(modifiers_, code);
        }
        return Delivery::Success;
    }
    if (value == 0) {
        return Delivery::Success; // one `key` event is the press and the release
    }
    Event event;
    event.kind = Event::Kind::Key;
    event.key = key_name(code);
    event.modifiers = modifier_names();
    event.repeat = value == 2;
    if (event.key.empty()) {
        return Delivery::Success; // nothing a shell could name
    }
    const auto result = answer(event);
    return !result ? Delivery::Waiting : *result ? Delivery::Success : Delivery::Failure;
}

void ShellRouter::send(int code, int value) {
    if (!on_) {
        inner_.send(code, value);
        return;
    }
    // Lock synchronization and other direct writes: only modifier state matters.
    if (is_modifier(code)) {
        if (value == 1 && std::find(modifiers_.begin(), modifiers_.end(), code) == modifiers_.end()) {
            modifiers_.push_back(code);
        } else if (value == 0) {
            std::erase(modifiers_, code);
        }
    }
}

void ShellRouter::cancel_delivery() {
    if (!on_) {
        inner_.cancel_delivery();
        return;
    }
    // Nothing cancelled may still go out at the end of the frame.
    in_flight_.reset();
    pending_.clear();
    modifiers_.clear();
}

void ShellRouter::cancel_text() {
    if (!on_) {
        inner_.cancel_text();
        return;
    }
    in_flight_.reset();
    pending_.clear();
}
} // namespace framekeyboard
