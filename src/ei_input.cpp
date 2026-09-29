#include "framekeyboard/ei_input.hpp"
#include <chrono>
#include <poll.h>
#include <stdexcept>

namespace framekeyboard {
EiSink::EiSink(const fs::path& socket) {
    text_socket_ = socket.parent_path() / "gamescope-0";
    context_ = ei_new_sender(nullptr);
    if (!context_) {
        throw std::runtime_error("Cannot allocate compositor input connection");
    }
    try {
        ei_configure_name(context_, "FrameKeyboard");
        if (ei_setup_backend_socket(context_, socket.c_str()) != 0) {
            throw std::runtime_error("Cannot connect to compositor keyboard socket: " + socket.string());
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!pump() && !disconnected_ && std::chrono::steady_clock::now() < deadline) {
            pollfd fd{ei_get_fd(context_), POLLIN, 0};
            poll(&fd, 1, 50);
        }
        if (!resumed_) {
            throw std::runtime_error("Compositor did not provide an active keyboard");
        }
    } catch (...) {
        if (keyboard_) {
            keyboard_ = ei_device_unref(keyboard_);
        }
        context_ = ei_unref(context_);
        throw;
    }
}
bool EiSink::text_available() {
    if (!text_attempted_) {
        text_attempted_ = true;
        try {
            text_ = std::make_unique<GamescopeText>(text_socket_.string());
        } catch (const std::exception&) {
            return false;
        }
    }
    return text_ && text_->ready();
}
bool EiSink::commit_text(const std::string& text) {
    return held_.empty() && pump() && text_available() && text_->commit(text);
}
bool EiSink::pump() {
    if (disconnected_) {
        return false;
    }
    if (text_) {
        text_->ready(); // Flush/dispatch only; no extra thread or blocking roundtrip.
    }
    ei_dispatch(context_);
    while (auto* event = ei_get_event(context_)) {
        auto* device = ei_event_get_device(event);
        switch (ei_event_get_type(event)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, nullptr);
            break;
        case EI_EVENT_DEVICE_ADDED:
            if (!keyboard_ && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                keyboard_ = ei_device_ref(device);
            }
            break;
        case EI_EVENT_DEVICE_RESUMED:
            if (device == keyboard_ && !disconnected_) {
                resumed_ = true;
                ei_device_start_emulating(keyboard_, ++sequence_);
            }
            break;
        case EI_EVENT_DEVICE_PAUSED:
        case EI_EVENT_DEVICE_REMOVED:
            if (device == keyboard_) {
                // EIS releases keys when pausing/removing a device. Forget our
                // local holds too, so reconnecting cannot replay a stale chord.
                resumed_ = false;
                // Latch failure until the app is reopened. A pause and resume
                // in the same dispatch must not silently revive stale UI holds.
                disconnected_ = true;
                held_.clear();
                if (ei_event_get_type(event) == EI_EVENT_DEVICE_REMOVED) {
                    keyboard_ = ei_device_unref(keyboard_);
                }
            }
            break;
        case EI_EVENT_DISCONNECT:
            disconnected_ = true;
            resumed_ = false;
            held_.clear();
            break;
        default:
            break;
        }
        ei_event_unref(event);
    }
    return resumed_ && !disconnected_;
}
void EiSink::send(int code, int value) {
    // EIS carries physical down/up transitions; the compositor owns repeat.
    if (!pump() || value == 2) {
        return;
    }
    if (value == 1) {
        if (!held_.insert(code).second) {
            return;
        }
    } else if (!held_.erase(code)) {
        return;
    }
    ei_device_keyboard_key(keyboard_, static_cast<std::uint32_t>(code), value != 0);
    ei_device_frame(keyboard_, ei_now(context_));
    ei_dispatch(context_);
}
EiSink::~EiSink() {
    if (resumed_ && !disconnected_ && keyboard_) {
        for (int code : held_) {
            ei_device_keyboard_key(keyboard_, static_cast<std::uint32_t>(code), false);
        }
        ei_device_frame(keyboard_, ei_now(context_));
        ei_device_stop_emulating(keyboard_);
        ei_dispatch(context_);
    }
    if (keyboard_) {
        ei_device_unref(keyboard_);
    }
    if (context_) {
        ei_unref(context_);
    }
}
} // namespace framekeyboard
