#include "framekeyboard/ei_input.hpp"
#include "framekeyboard/local_socket.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace framekeyboard {
fs::path gamescope_text_socket(const fs::path& input_socket, const fs::path& explicit_text) {
    if (!explicit_text.empty()) {
        return explicit_text;
    }
    const auto name = input_socket.filename().string();
    constexpr std::string_view prefix = "gamescope-", suffix = "-ei";
    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) ||
        !name.ends_with(suffix)) {
        return {};
    }
    const auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) {
        return {};
    }
    // Relative names resolve under XDG_RUNTIME_DIR in both libei and Wayland.
    return input_socket.parent_path() / name.substr(0, name.size() - suffix.size());
}
EiSink::EiSink(const fs::path& socket, const fs::path& text_socket, GamescopeText::Clock clock)
    : socket_(socket), text_socket_(gamescope_text_socket(socket, text_socket)),
      clock_(clock ? std::move(clock) : GamescopeText::Clock([] {
          return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      })) {
    try {
        if (!connect()) {
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
        if (context_) {
            context_ = ei_unref(context_);
        }
        throw;
    }
}
bool EiSink::connect() {
    if (keyboard_) {
        keyboard_ = ei_device_unref(keyboard_);
    }
    if (context_) {
        context_ = ei_unref(context_);
    }
    keymap_.reset();
    num_lock_.reset();
    group_ = sequence_ = 0;
    held_.clear();
    resumed_ = false;
    disconnected_ = true;
    context_ = ei_new_sender(nullptr);
    if (!context_) {
        return false;
    }
    ei_configure_name(context_, "Full Keyboard");
    const int fd = connect_local_socket(socket_);
    if (fd < 0) {
        return false;
    }
    // libei owns fd, including on setup failure. No handshake wait here;
    // subsequent frames dispatch it, with a bounded negotiation deadline.
    if (ei_setup_backend_fd(context_, fd) != 0) {
        return false;
    }
    disconnected_ = false;
    connect_deadline_ = clock_() + 3;
    return true;
}
void EiSink::connection_lost() {
    input_reset_ |= resumed_ || keyboard_;
    disconnected_ = true;
    resumed_ = false;
    held_.clear();
    num_lock_.reset();
    keymap_.reset();
    text_.reset();
    text_retry_at_ = 0;
    text_retry_delay_ = 1;
    retry_at_ = clock_() + retry_delay_;
    retry_delay_ = std::min(30.0, retry_delay_ * 2);
}
void EiSink::service_text() {
    if (text_socket_.empty() || disconnected_ || !resumed_) {
        return; // Never let Wayland fall back to an unrelated default display.
    }
    if (text_ && text_->ready()) {
        text_retry_delay_ = 1;
    }
    if (text_ && text_->failed()) {
        text_.reset();
        text_retry_at_ = clock_() + text_retry_delay_;
        text_retry_delay_ = std::min(30.0, text_retry_delay_ * 2);
    }
    if (!text_ && clock_() >= text_retry_at_) {
        try {
            text_ = std::make_unique<GamescopeText>(text_socket_.string(), clock_);
            text_->set_character_gap(character_gap_);
        } catch (const std::exception&) {
            text_retry_at_ = clock_() + text_retry_delay_;
            text_retry_delay_ = std::min(30.0, text_retry_delay_ * 2);
        }
    }
}
void EiSink::set_character_gap(double seconds) {
    character_gap_ = seconds;
    if (text_) {
        text_->set_character_gap(seconds);
    }
}
bool EiSink::text_available() {
    service_text();
    return !disconnected_ && resumed_ && text_ && text_->ready();
}
bool EiSink::commit_text(const std::string& text) {
    if (entry_ || needs_entry(text)) {
        return enter(text, 0);
    }
    last_entry_ = false;
    return pump() && !input_reset_ && held_.empty() && text_available() &&
           text_->commit(text, 0, on_keymap(text));
}
bool EiSink::replace_text(std::size_t delete_before, const std::string& text) {
    if (entry_ || needs_entry(text)) {
        return enter(text, delete_before);
    }
    last_entry_ = false;
    return pump() && !input_reset_ && held_.empty() && text_available() &&
           text_->commit(text, delete_before, on_keymap(text));
}
std::size_t EiSink::delivered_characters() const {
    return last_entry_ ? entry_delivered_ : text_ ? text_->acknowledged() : 0;
}
void EiSink::cancel_text() {
    if (text_) {
        text_->cancel();
    }
    if (!entry_) {
        return;
    }
    auto& entry = *entry_;
    if (!entering()) {
        entry_.reset();
        return;
    }
    // Stop only where IBus is not mid-sequence and no modifier is held:
    // after a whole Backspace tap, once the current character is entered, or
    // once a started Ctrl+V is complete. A paste not yet started never starts.
    const auto deletion_keys = 2 * entry.deletions;
    std::size_t stop = entry.keys.size();
    if (entry.next <= deletion_keys) {
        stop = entry.next + entry.next % 2;
    } else if (!entry.write) {
        stop = *std::find_if(entry.character_ends.begin(), entry.character_ends.end(),
                             [&](auto end) { return end >= entry.next; });
    }
    entry.keys.resize(stop);
    if (entry.write && stop <= entry.paste_at) {
        entry.write.reset();
    }
    entry.cancelled = true;
    if (!entering()) {
        entry_.reset();
    }
}
// The nested desktop reads Gamescope's keys with its own keymap, so text
// beyond printable ASCII cannot go through Gamescope's temporary keymap there.
bool EiSink::desktop_target() {
    if (desktop_focused_) {
        return desktop_focused_();
    }
    if (text_socket_.empty()) {
        return false;
    }
    if (!desktop_) {
        // Gamescope publishes its focus on its first X display, where Steam runs.
        const char* display = std::getenv("DISPLAY");
        desktop_ = std::make_unique<DesktopFocus>(display && *display ? display : ":0");
    }
    return desktop_->focused();
}
bool EiSink::needs_entry(const std::string& text) {
    return desktop_now_ &&
           std::any_of(text.begin(), text.end(), [](char c) { return c < 0x20 || c > 0x7e; });
}
bool EiSink::enter(const std::string& text, std::size_t deletions) {
    if (entry_) {
        // The first call starts entry; repeating it reports the result.
        if (entering() || entry_->cancelled || text != entry_->text || deletions != entry_->deletions) {
            return false;
        }
        const bool ok = !entry_->failed;
        entry_.reset();
        return ok;
    }
    if (!pump() || input_reset_ || !held_.empty() || (text_ && text_->busy()) || text.empty() ||
        text.find('\0') != std::string::npos ||
        !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) ||
        g_utf8_strlen(text.c_str(), -1) > 32 || deletions > 64) {
        return false;
    }
    // Physical keys: the desktop's own keymap reads them, so they must be
    // where U, the hex digits and Space are on US-like layouts.
    static constexpr int digits[]{KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7,
                                  KEY_8, KEY_9, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F};
    Entry entry;
    entry.text = text;
    entry.deletions = deletions;
    const auto tap = [&](int code) {
        entry.keys.emplace_back(code, 1);
        entry.keys.emplace_back(code, 0);
    };
    for (std::size_t i = 0; i < deletions; ++i) {
        tap(KEY_BACKSPACE);
    }
    if (desktop_paste_) {
        // The text may arrive any time after Ctrl+V, so none counts as delivered.
        entry.write = desktop_paste_->write(text);
        entry.paste_at = entry.keys.size();
        entry.keys.insert(entry.keys.end(),
                          {{KEY_LEFTCTRL, 1}, {KEY_V, 1}, {KEY_V, 0}, {KEY_LEFTCTRL, 0}});
        entry.character_ends.assign(static_cast<std::size_t>(g_utf8_strlen(text.c_str(), -1)),
                                    entry.keys.size() + 1);
    }
    for (const char* p = desktop_paste_ ? "" : text.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto character = g_utf8_get_char(p);
        if (character == ' ') {
            tap(KEY_SPACE);
        } else if (character == '\n') {
            tap(KEY_ENTER);
        } else {
            entry.keys.insert(entry.keys.end(), {{KEY_LEFTCTRL, 1}, {KEY_LEFTSHIFT, 1}});
            tap(KEY_U);
            entry.keys.insert(entry.keys.end(), {{KEY_LEFTSHIFT, 0}, {KEY_LEFTCTRL, 0}});
            char hex[16];
            std::snprintf(hex, sizeof hex, "%x", character);
            for (const char* h = hex; *h; ++h) {
                tap(digits[*h <= '9' ? *h - '0' : *h - 'a' + 10]);
            }
            tap(KEY_SPACE); // ends the entry and commits the character
        }
        entry.character_ends.push_back(entry.keys.size());
    }
    entry.next_at = clock_();
    last_entry_ = true;
    entry_delivered_ = 0;
    entry_ = std::move(entry);
    advance_entry();
    return false;
}
// One key event per call, at least entry_gap apart.
void EiSink::advance_entry() {
    if (!entering()) {
        return;
    }
    auto& entry = *entry_;
    if (disconnected_ || !resumed_ || input_reset_ || !keyboard_) {
        // EIS released our keys with the device. Whole characters count as delivered.
        entry.failed = true;
        entry_delivered_ = static_cast<std::size_t>(
            std::count_if(entry.character_ends.begin(), entry.character_ends.end(),
                          [&](auto end) { return end <= entry.next; }));
        if (entry.cancelled) {
            entry_.reset();
        }
        return;
    }
    if (entry.write && entry.next == entry.keys.size()) {
        if (entry.cancelled) {
            entry_.reset(); // a cancelled paste does not wait for the application
            return;
        }
        // Ctrl+V is out: done once an application asks for the text.
        entry.taken = desktop_paste_ && desktop_paste_->served() != entry.served;
        entry.failed = !entry.taken && clock_() >= entry.deadline;
        if (entry.cancelled && !entering()) {
            entry_.reset();
        }
        return;
    }
    if (clock_() < entry.next_at) {
        return;
    }
    if (entry.write && entry.next == entry.paste_at) {
        const auto owned = desktop_paste_ ? desktop_paste_->result(*entry.write) : false;
        if (!owned) {
            return; // the clipboard is still being set
        }
        if (!*owned) {
            entry.failed = true; // nothing pasted, so nothing to finish
            if (entry.cancelled) {
                entry_.reset();
            }
            return;
        }
        entry.served = desktop_paste_->served();
    }
    const auto [code, value] = entry.keys[entry.next++];
    if (value) {
        held_.insert(code);
    } else {
        held_.erase(code);
    }
    ei_device_keyboard_key(keyboard_, static_cast<std::uint32_t>(code), value != 0);
    ei_device_frame(keyboard_, ei_now(context_));
    ei_dispatch(context_);
    entry.next_at = clock_() + entry_gap;
    entry.deadline = clock_() + paste_wait;
    if (entry.cancelled && !entering()) {
        entry_.reset();
    }
}
// Whether Gamescope can type every character of a multi-character text on its
// ordinary keymap, so committing them one by one avoids its temporary keymap.
// Mirrors ime.cpp try_type_keysym: one key level with exactly that keysym,
// reached with at most Shift, Control and Alt. Only the current group counts,
// because Gamescope presses the key without changing the group.
bool EiSink::on_keymap(const std::string& text) const {
    if (per_character_) {
        return *per_character_;
    }
    if (g_utf8_strlen(text.c_str(), -1) < 2) {
        return false;
    }
    if (!keymap_) {
        // Gamescope's libei device sends no keymap. Its ordinary keymap is the
        // layout Steam pushes (US by default), where printable ASCII needs no
        // AltGr; other text keeps the single temporary-keymap commit.
        return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
    }
    auto* map = keymap_.get();
    xkb_mod_mask_t allowed = 0;
    for (const char* name : {XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT}) {
        const auto index = xkb_keymap_mod_get_index(map, name);
        if (index != XKB_MOD_INVALID) {
            allowed |= xkb_mod_mask_t{1} << index;
        }
    }
    const auto typeable = [&](xkb_keysym_t keysym) {
        for (auto key = xkb_keymap_min_keycode(map); key <= xkb_keymap_max_keycode(map); ++key) {
            if (group_ >= xkb_keymap_num_layouts_for_key(map, key)) {
                continue;
            }
            const auto levels = xkb_keymap_num_levels_for_key(map, key, group_);
            for (xkb_level_index_t level = 0; level < levels; ++level) {
                const xkb_keysym_t* symbols = nullptr;
                xkb_mod_mask_t mask{};
                if (xkb_keymap_key_get_syms_by_level(map, key, group_, level, &symbols) == 1 &&
                    symbols[0] == keysym &&
                    xkb_keymap_key_get_mods_for_level(map, key, group_, level, &mask, 1) == 1 &&
                    (mask & ~allowed) == 0) {
                    return true;
                }
            }
        }
        return false;
    };
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        const auto keysym = xkb_utf32_to_keysym(g_utf8_get_char(p));
        if (keysym == XKB_KEY_NoSymbol || !typeable(keysym)) {
            return false;
        }
    }
    return true;
}
bool EiSink::pump() {
    if (disconnected_) {
        advance_entry(); // fails an entry the connection took down
        if (clock_() < retry_at_) {
            return false;
        }
        if (!connect()) {
            retry_at_ = clock_() + retry_delay_;
            retry_delay_ = std::min(30.0, retry_delay_ * 2);
            return false;
        }
    }
    service_text();
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
                read_keymap();
            }
            break;
        case EI_EVENT_KEYBOARD_MODIFIERS:
            if (device == keyboard_) {
                group_ = ei_event_keyboard_get_xkb_group(event);
                if (keymap_) {
                    const auto index = xkb_keymap_mod_get_index(keymap_.get(), XKB_MOD_NAME_NUM);
                    if (index != XKB_MOD_INVALID && index < 32) {
                        num_lock_ = (ei_event_keyboard_get_xkb_mods_locked(event) & (1u << index)) != 0;
                    }
                }
            }
            break;
        case EI_EVENT_DEVICE_RESUMED:
            if (device == keyboard_ && !disconnected_) {
                resumed_ = true;
                connect_deadline_ = 0;
                retry_delay_ = 1;
                ei_device_start_emulating(keyboard_, ++sequence_);
            }
            break;
        case EI_EVENT_DEVICE_PAUSED:
        case EI_EVENT_DEVICE_REMOVED:
            if (device == keyboard_) {
                // EIS releases keys when pausing/removing a device. Forget our
                // local holds too, so reconnecting cannot replay a stale chord.
                resumed_ = false;
                // Remember interruption even if RESUMED is in this same batch.
                // No new output is allowed until the app acknowledges and clears its holds.
                input_reset_ = true;
                held_.clear();
                num_lock_.reset();
                if (ei_event_get_type(event) == EI_EVENT_DEVICE_REMOVED) {
                    keyboard_ = ei_device_unref(keyboard_);
                    keymap_.reset();
                    group_ = 0;
                }
            }
            break;
        case EI_EVENT_DISCONNECT:
            connection_lost();
            break;
        default:
            break;
        }
        ei_event_unref(event);
    }
    if (!disconnected_ && connect_deadline_ != 0 && clock_() >= connect_deadline_) {
        connection_lost();
    }
    desktop_now_ = desktop_target();
    advance_entry();
    return resumed_ && !disconnected_;
}
void EiSink::read_keymap() {
    keymap_.reset();
    group_ = 0;
    num_lock_.reset();
    auto* map = ei_device_keyboard_get_keymap(keyboard_);
    if (!map || ei_keymap_get_type(map) != EI_KEYMAP_TYPE_XKB) {
        return;
    }
    const auto size = ei_keymap_get_size(map);
    if (!size || size > 4 * 1024 * 1024) {
        return;
    }
    std::string text(size, '\0');
    if (pread(ei_keymap_get_fd(map), text.data(), size, 0) != static_cast<ssize_t>(size)) {
        return;
    }
    if (!xkb_) {
        xkb_.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
    }
    if (!xkb_) {
        return;
    }
    keymap_.reset(xkb_keymap_new_from_string(xkb_.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS));
}
int EiSink::shortcut_code(xkb_keysym_t symbol, int fallback) {
    pump();
    if (!keymap_) {
        return fallback;
    }
    // Prefer the current target group. If it is non-Latin, applications usually
    // use a Latin group's physical counterpart for Ctrl shortcuts. Never switch
    // the compositor's group or alter its keyboard configuration here.
    const auto groups = xkb_keymap_num_layouts(keymap_.get());
    for (xkb_layout_index_t pass = 0; pass <= groups; ++pass) {
        const auto group = pass == 0 ? group_ : pass - 1;
        if (group >= groups || (pass && group == group_)) {
            continue;
        }
        for (auto code = xkb_keymap_min_keycode(keymap_.get());
             code <= xkb_keymap_max_keycode(keymap_.get()); ++code) {
            const xkb_keysym_t* symbols = nullptr;
            const int count = xkb_keymap_key_get_syms_by_level(keymap_.get(), code, group, 0, &symbols);
            for (int i = 0; i < count; ++i) {
                if (xkb_keysym_to_lower(symbols[i]) == symbol && code >= 8) {
                    return static_cast<int>(code - 8);
                }
            }
        }
    }
    return fallback;
}
bool EiSink::take_input_reset() {
    const bool reset = input_reset_;
    input_reset_ = false;
    return reset;
}
void EiSink::send(int code, int value) {
    // EIS carries physical down/up transitions; the compositor owns repeat.
    if (!pump() || input_reset_ || value == 2) {
        return;
    }
    if (value == 1) {
        if ((text_ && text_->busy()) || entering()) {
            return; // A native down cannot overtake unresolved Unicode delivery.
        }
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
