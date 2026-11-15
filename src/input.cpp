#include "framekeyboard/input.hpp"
#include "framekeyboard/localization.hpp"
#include "framekeyboard/output.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
namespace {
const std::map<std::string, int> key_codes = {{"Escape", KEY_ESC},
                                              {"Backquote", KEY_GRAVE},
                                              {"Digit1", KEY_1},
                                              {"Digit2", KEY_2},
                                              {"Digit3", KEY_3},
                                              {"Digit4", KEY_4},
                                              {"Digit5", KEY_5},
                                              {"Digit6", KEY_6},
                                              {"Digit7", KEY_7},
                                              {"Digit8", KEY_8},
                                              {"Digit9", KEY_9},
                                              {"Digit0", KEY_0},
                                              {"Minus", KEY_MINUS},
                                              {"Equal", KEY_EQUAL},
                                              {"Backspace", KEY_BACKSPACE},
                                              {"Tab", KEY_TAB},
                                              {"KeyQ", KEY_Q},
                                              {"KeyW", KEY_W},
                                              {"KeyE", KEY_E},
                                              {"KeyR", KEY_R},
                                              {"KeyT", KEY_T},
                                              {"KeyY", KEY_Y},
                                              {"KeyU", KEY_U},
                                              {"KeyI", KEY_I},
                                              {"KeyO", KEY_O},
                                              {"KeyP", KEY_P},
                                              {"BracketLeft", KEY_LEFTBRACE},
                                              {"BracketRight", KEY_RIGHTBRACE},
                                              {"Backslash", KEY_BACKSLASH},
                                              {"KeyA", KEY_A},
                                              {"KeyS", KEY_S},
                                              {"KeyD", KEY_D},
                                              {"KeyF", KEY_F},
                                              {"KeyG", KEY_G},
                                              {"KeyH", KEY_H},
                                              {"KeyJ", KEY_J},
                                              {"KeyK", KEY_K},
                                              {"KeyL", KEY_L},
                                              {"Semicolon", KEY_SEMICOLON},
                                              {"Quote", KEY_APOSTROPHE},
                                              {"Enter", KEY_ENTER},
                                              {"KeyZ", KEY_Z},
                                              {"KeyX", KEY_X},
                                              {"KeyC", KEY_C},
                                              {"KeyV", KEY_V},
                                              {"KeyB", KEY_B},
                                              {"KeyN", KEY_N},
                                              {"KeyM", KEY_M},
                                              {"Comma", KEY_COMMA},
                                              {"Period", KEY_DOT},
                                              {"Slash", KEY_SLASH},
                                              {"Space", KEY_SPACE},
                                              {"IntlBackslash", KEY_102ND},
                                              {"IntlYen", KEY_YEN},
                                              {"IntlRo", KEY_RO},
                                              {"NumpadComma", KEY_KPCOMMA},
                                              {"Convert", KEY_HENKAN},
                                              {"NonConvert", KEY_MUHENKAN},
                                              {"KanaMode", KEY_KATAKANAHIRAGANA},
                                              {"ZenkakuHankaku", KEY_ZENKAKUHANKAKU},
                                              {"ControlLeft", KEY_LEFTCTRL},
                                              {"ControlRight", KEY_RIGHTCTRL},
                                              {"ShiftLeft", KEY_LEFTSHIFT},
                                              {"ShiftRight", KEY_RIGHTSHIFT},
                                              {"AltLeft", KEY_LEFTALT},
                                              {"AltRight", KEY_RIGHTALT},
                                              {"MetaLeft", KEY_LEFTMETA},
                                              {"MetaRight", KEY_RIGHTMETA},
                                              {"ContextMenu", KEY_COMPOSE},
                                              {"CapsLock", KEY_CAPSLOCK},
                                              {"NumLock", KEY_NUMLOCK},
                                              {"ScrollLock", KEY_SCROLLLOCK},
                                              {"PrintScreen", KEY_SYSRQ},
                                              {"Pause", KEY_PAUSE},
                                              {"F1", KEY_F1},
                                              {"F2", KEY_F2},
                                              {"F3", KEY_F3},
                                              {"F4", KEY_F4},
                                              {"F5", KEY_F5},
                                              {"F6", KEY_F6},
                                              {"F7", KEY_F7},
                                              {"F8", KEY_F8},
                                              {"F9", KEY_F9},
                                              {"F10", KEY_F10},
                                              {"F11", KEY_F11},
                                              {"F12", KEY_F12},
                                              {"Insert", KEY_INSERT},
                                              {"Delete", KEY_DELETE},
                                              {"Home", KEY_HOME},
                                              {"End", KEY_END},
                                              {"PageUp", KEY_PAGEUP},
                                              {"PageDown", KEY_PAGEDOWN},
                                              {"ArrowLeft", KEY_LEFT},
                                              {"ArrowRight", KEY_RIGHT},
                                              {"ArrowUp", KEY_UP},
                                              {"ArrowDown", KEY_DOWN},
                                              {"Numpad0", KEY_KP0},
                                              {"Numpad1", KEY_KP1},
                                              {"Numpad2", KEY_KP2},
                                              {"Numpad3", KEY_KP3},
                                              {"Numpad4", KEY_KP4},
                                              {"Numpad5", KEY_KP5},
                                              {"Numpad6", KEY_KP6},
                                              {"Numpad7", KEY_KP7},
                                              {"Numpad8", KEY_KP8},
                                              {"Numpad9", KEY_KP9},
                                              {"NumpadDecimal", KEY_KPDOT},
                                              {"NumpadAdd", KEY_KPPLUS},
                                              {"NumpadSubtract", KEY_KPMINUS},
                                              {"NumpadMultiply", KEY_KPASTERISK},
                                              {"NumpadDivide", KEY_KPSLASH},
                                              {"NumpadEnter", KEY_KPENTER}};
void checked_ioctl(int fd, unsigned long request, int value) {
    if (ioctl(fd, request, value) < 0) {
        throw std::system_error(errno, std::generic_category(), "uinput setup");
    }
}
void write_event(int fd, unsigned short type, unsigned short code, int value) {
    input_event event{};
    event.type = type;
    event.code = code;
    event.value = value;
    ssize_t written;
    do {
        written = write(fd, &event, sizeof(event));
    } while (written < 0 && errno == EINTR);
    if (written != sizeof(event)) {
        throw std::runtime_error("uinput event write failed");
    }
}
} // namespace

const char* shortcut_key(const std::string& action) {
    return action == "copy" ? "KeyC" : action == "cut" ? "KeyX" : "KeyV";
}

int key_code(const std::string& name) {
    const auto found = key_codes.find(name);
    return found == key_codes.end() ? 0 : found->second;
}
const std::string& key_name(int code) {
    static const auto names = [] {
        std::map<int, std::string> result;
        for (const auto& [name, value] : key_codes) {
            result.emplace(value, name);
        }
        return result;
    }();
    static const std::string none;
    const auto found = names.find(code);
    return found == names.end() ? none : found->second;
}
bool is_modifier(int code) {
    return code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTSHIFT ||
           code == KEY_RIGHTSHIFT || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
           code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}
UInputSink::UInputSink() {
    fd_ = open("/dev/uinput", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/uinput");
    }
    try {
        checked_ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        checked_ioctl(fd_, UI_SET_EVBIT, EV_LED);
        checked_ioctl(fd_, UI_SET_LEDBIT, LED_NUML);
        for (const auto& [name, code] : key_codes) {
            (void)name;
            checked_ioctl(fd_, UI_SET_KEYBIT, code);
        }
        // Repeat is scheduled by KeyboardState. Enabling EV_REP would double it.
        uinput_setup setup{};
        setup.id.bustype = BUS_VIRTUAL;
        std::strncpy(setup.name, "Full Keyboard", sizeof(setup.name) - 1);
        if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
            throw std::system_error(errno, std::generic_category(), "create virtual keyboard");
        }
    } catch (...) {
        close(fd_);
        fd_ = -1;
        throw;
    }
}
UInputSink::~UInputSink() {
    if (fd_ < 0) {
        return;
    }
    // Destruction also runs after a write failure; never throw during cleanup.
    for (int code : held_) {
        try {
            write_event(fd_, EV_KEY, static_cast<unsigned short>(code), 0);
        } catch (...) {
        }
    }
    try {
        write_event(fd_, EV_SYN, SYN_REPORT, 0);
    } catch (...) {
    }
    ioctl(fd_, UI_DEV_DESTROY);
    if (led_fd_ >= 0) {
        close(led_fd_);
    }
    close(fd_);
}
fs::path UInputSink::event_node() const {
    char name[128]{};
    if (ioctl(fd_, UI_GET_SYSNAME(sizeof(name)), name) < 0) {
        throw std::runtime_error("cannot identify the created input device");
    }
    const fs::path directory = fs::path("/sys/devices/virtual/input") / name;
    if (!fs::exists(directory)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto node = entry.path().filename().string();
        if (node.starts_with("event")) {
            return fs::path("/dev/input") / node;
        }
    }
    return {};
}
void UInputSink::send(int code, int value) {
    if (code <= 0 || code > KEY_MAX || value < 0 || value > 2) {
        throw std::runtime_error("invalid key event");
    }
    // Remember down before writing so partial delivery is still released on failure.
    if (value == 1) {
        held_.insert(code);
    }
    write_event(fd_, EV_KEY, static_cast<unsigned short>(code), value);
    write_event(fd_, EV_SYN, SYN_REPORT, 0);
    if (value == 0) {
        held_.erase(code);
    }
}
bool UInputSink::pump() {
    // LED changes are delivered through uinput. Query only our own event node
    // as well, since Linux suppresses an initial LED-off event.
    if (led_fd_ < 0) {
        const auto path = event_node();
        if (!path.empty()) {
            led_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
    }
    unsigned long leds{};
    if (led_fd_ >= 0 && ioctl(led_fd_, EVIOCGLED(sizeof(leds)), &leds) >= 0) {
        num_lock_ = (leds & (1ul << LED_NUML)) != 0;
    }
    input_event event{};
    while (read(fd_, &event, sizeof(event)) == sizeof(event)) {
        if (event.type == EV_LED && event.code == LED_NUML) {
            num_lock_ = event.value != 0;
        }
    }
    return true;
}

LanguageMap::LanguageMap(const Language& language) : language_(language) {
    context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context_) {
        throw std::runtime_error("cannot create XKB context");
    }
    xkb_rule_names names{language.rules.c_str(), language.model.c_str(), language.keymap.c_str(),
                         language.variant.c_str(), language.options.c_str()};
    keymap_ = xkb_keymap_new_from_names(context_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap_) {
        xkb_context_unref(context_);
        context_ = nullptr;
        throw std::runtime_error("language keymap is unavailable");
    }
    // The letters' base level decides the direction; punctuation is neutral.
    int letters = 0, rtl = 0;
    auto* state = xkb_state_new(keymap_);
    for (int code :
         {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D,
          KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M}) {
        const auto ch = state ? xkb_state_key_get_utf32(state, static_cast<xkb_keycode_t>(code + 8)) : 0;
        if (!g_unichar_isalpha(ch)) {
            continue;
        }
        ++letters;
        switch (g_unichar_get_script(ch)) {
        case G_UNICODE_SCRIPT_ARABIC:
        case G_UNICODE_SCRIPT_HEBREW:
        case G_UNICODE_SCRIPT_SYRIAC:
        case G_UNICODE_SCRIPT_THAANA:
        case G_UNICODE_SCRIPT_NKO:
            ++rtl;
            break;
        default:
            break;
        }
    }
    if (state) {
        // XKB keycodes are evdev codes plus 8.
        altgr_ = xkb_state_key_get_one_sym(state, static_cast<xkb_keycode_t>(KEY_RIGHTALT + 8)) ==
                 XKB_KEY_ISO_Level3_Shift;
        xkb_state_unref(state);
    }
    right_to_left_ = rtl * 2 > letters;
}
LanguageMap::~LanguageMap() {
    if (compose_) {
        xkb_compose_state_unref(compose_);
    }
    if (keymap_) {
        xkb_keymap_unref(keymap_);
    }
    if (context_) {
        xkb_context_unref(context_);
    }
}
xkb_keysym_t LanguageMap::symbol(const Key& key, const std::set<int>& modifiers, bool caps,
                                 bool num) const {
    const int code = key_code(key.action);
    // XKB's evdev rules reserve keycodes 0..7. This offset is unrelated to Steam codes.
    auto* state = xkb_state_new(keymap_);
    if (!state) {
        return XKB_KEY_NoSymbol;
    }
    for (int modifier : modifiers) {
        xkb_state_update_key(state, static_cast<xkb_keycode_t>(modifier + 8), XKB_KEY_DOWN);
    }
    if (caps) {
        // The lock itself, not its key: Colemak makes that key a Backspace.
        const auto group = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);
        xkb_state_update_mask(state, xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
                              xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
                              xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED) |
                                  1u << xkb_keymap_mod_get_index(keymap_, XKB_MOD_NAME_CAPS),
                              0, 0, group);
    }
    if (num) {
        xkb_state_update_key(state, KEY_NUMLOCK + 8, XKB_KEY_DOWN);
        xkb_state_update_key(state, KEY_NUMLOCK + 8, XKB_KEY_UP);
    }
    auto symbol = xkb_state_key_get_one_sym(state, static_cast<xkb_keycode_t>(code + 8));
    xkb_state_unref(state);
    // Japanese composes on a US keymap, which has nothing on the JIS ¥ and ろ
    // keys: in Latin mode they type what a JIS keyboard's do (xkb jp(OADG109A)).
    if (symbol == XKB_KEY_NoSymbol && language_.locale == "ja-JP" &&
        (code == KEY_YEN || code == KEY_RO)) {
        const bool shift = modifiers.contains(KEY_LEFTSHIFT) || modifiers.contains(KEY_RIGHTSHIFT);
        if (code == KEY_YEN) {
            symbol = shift ? XKB_KEY_bar : XKB_KEY_yen;
        } else {
            symbol = shift ? XKB_KEY_underscore : XKB_KEY_backslash;
        }
    }
    return symbol;
}
bool LanguageMap::printable(xkb_keysym_t symbol) {
    const auto ch = xkb_keysym_to_utf32(symbol);
    return (ch >= 32 && ch != 127) || (symbol >= XKB_KEY_dead_grave && symbol <= XKB_KEY_dead_greek);
}
std::string LanguageMap::compose(xkb_keysym_t symbol) {
    if (!compose_) {
        auto* table =
            xkb_compose_table_new_from_locale(context_, "C.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (!table) {
            throw UiError(ui("input.accent-table-unavailable"));
        }
        compose_ = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb_compose_table_unref(table);
        if (!compose_) {
            throw UiError(ui("input.accent-composition-failed"));
        }
    }
    xkb_compose_state_feed(compose_, symbol);
    char buffer[128]{};
    switch (xkb_compose_state_get_status(compose_)) {
    case XKB_COMPOSE_COMPOSING:
        return {};
    case XKB_COMPOSE_COMPOSED:
        xkb_compose_state_get_utf8(compose_, buffer, sizeof(buffer));
        xkb_compose_state_reset(compose_);
        return buffer;
    case XKB_COMPOSE_CANCELLED:
        xkb_compose_state_reset(compose_);
        throw UiError(ui("input.accent-unavailable"));
    default:
        xkb_keysym_to_utf8(symbol, buffer, sizeof(buffer));
        return buffer;
    }
}
bool LanguageMap::composing() const {
    return compose_ && xkb_compose_state_get_status(compose_) == XKB_COMPOSE_COMPOSING;
}
void LanguageMap::cancel_compose() {
    if (compose_) {
        xkb_compose_state_reset(compose_);
    }
}
std::string LanguageMap::legend(const Key& key, const std::set<int>& modifiers, bool caps,
                                bool num) const {
    if (const auto it = language_.overrides.find(key.id); it != language_.overrides.end()) {
        return it->second;
    }
    if (key.action_kind != ActionKind::Key || key.action == "Space") {
        return key.label;
    }
    const auto symbol = this->symbol(key, modifiers, caps, num);
    char buffer[64]{};
    const int size = xkb_keysym_to_utf8(symbol, buffer, sizeof(buffer));
    // A right Alt that selects the third level is AltGr in this keymap, also on
    // geometry whose own label says Alt.
    if (symbol == XKB_KEY_ISO_Level3_Shift) {
        return "AltGr";
    }
    // Dead keys have no standalone UTF-8 output. Show the accent the target
    // keymap will compose, instead of falling back to a misleading US legend.
    switch (symbol) {
    case XKB_KEY_dead_grave:
        return "`";
    case XKB_KEY_dead_acute:
        return "´";
    case XKB_KEY_dead_circumflex:
        return "^";
    case XKB_KEY_dead_tilde:
        return "~";
    case XKB_KEY_dead_diaeresis:
        return "¨";
    case XKB_KEY_dead_cedilla:
        return "¸";
    case XKB_KEY_dead_macron:
        return "¯";
    case XKB_KEY_dead_breve:
        return "˘";
    case XKB_KEY_dead_abovedot:
        return "˙";
    case XKB_KEY_dead_abovering:
        return "˚";
    case XKB_KEY_dead_doubleacute:
        return "˝";
    case XKB_KEY_dead_caron:
        return "ˇ";
    case XKB_KEY_dead_ogonek:
        return "˛";
    case XKB_KEY_dead_iota:
        return "ͺ";
    case XKB_KEY_dead_abovecomma:
        return "᾿";
    case XKB_KEY_dead_abovereversedcomma:
        return "῾";
    case XKB_KEY_dead_currency:
        return "¤";
    // Accents without a spacing form are shown on a dotted circle, as on keycaps.
    case XKB_KEY_dead_belowdot:
        return "◌̣";
    case XKB_KEY_dead_hook:
        return "◌̉";
    case XKB_KEY_dead_horn:
        return "◌̛";
    case XKB_KEY_dead_stroke:
        return "◌̸";
    case XKB_KEY_dead_belowcomma:
        return "◌̦";
    case XKB_KEY_dead_doublegrave:
        return "◌̏";
    case XKB_KEY_dead_invertedbreve:
        return "◌̑";
    case XKB_KEY_dead_belowmacron:
        return "◌̱";
    case XKB_KEY_dead_greek: // Prefixes a Latin letter to type its Greek counterpart.
        return "α";
    default:
        break;
    }
    if (size > 0 && size < static_cast<int>(sizeof(buffer)) &&
        static_cast<unsigned char>(buffer[0]) >= 32 && static_cast<unsigned char>(buffer[0]) != 127) {
        return buffer;
    }
    return key.label;
}

KeyboardState::KeyboardState(OutputController& output) : sink_(output), output_(&output) {
}
KeyboardState::~KeyboardState() {
    try {
        cancel_all();
    } catch (...) {
    }
}
void KeyboardState::acquire(int code) {
    auto& count = references_[code];
    if (count++ == 0) {
        if (!output_) {
            sink_.send(code, 1);
        }
        // Both controllers can hold the same lock key. Its state changes once
        // per backend key-down, not once per pointer.
        if (code == KEY_CAPSLOCK) {
            caps_ = !caps_;
        }
        if (code == KEY_NUMLOCK) {
            num_ = !num_;
        }
    }
}
void KeyboardState::release(int code) {
    auto found = references_.find(code);
    if (found == references_.end()) {
        return;
    }
    if (--found->second == 0) {
        if (!output_) {
            sink_.send(code, 0);
        }
        references_.erase(found);
    }
}
bool KeyboardState::down(unsigned pointer, const Key& key, double now, bool local, int native_code,
                         bool uses_modifiers, std::optional<std::vector<int>> shortcut_modifiers) {
    if (presses_.contains(pointer)) {
        return false;
    }
    // A second pointer must not turn Copy into Ctrl+Shift+C while a chord is held.
    if (key.action_kind == ActionKind::Shortcut && !references_.empty()) {
        return false;
    }
    now_ = now;
    Press press;
    press.id = key.id;
    const int code =
        key.action_kind == ActionKind::Key ? (native_code ? native_code : key_code(key.action)) : 0;
    if (is_modifier(code)) {
        press.modifier = code;
        press.used = !key.sticky || references_.contains(code);
        press.at = now;
        press.unlocks = locked_.contains(code);
        // Every sticky modifier locks: Shift, Ctrl, Alt, AltGr and the left
        // Super. The right Super is momentary (not sticky) and never does.
        press.lockable = key.sticky && !press.unlocks;
        press.double_tap = press.lockable && !press.used && last_tap_ == code &&
                           now - last_tap_at_ <= double_tap_seconds && latched_.contains(code);
        if (!key.sticky) {
            // A momentary Super key must reach the OS even without a chord.
            // Keep its own reference so overlapping hands share one down/up.
            press.codes.push_back(code);
        }
        presses_.emplace(pointer, std::move(press));
        if (!key.sticky) {
            acquire(code);
            if (output_) {
                output_->native_press(pointer, {code}, 0, now);
            }
        }
        return true;
    }
    // A modifier tap latches visually but emits nothing until used. This avoids
    // leaving Ctrl physically held while the user navigates another application.
    // A shortcut holds its own chord's modifiers, in order, never latched ones.
    std::set<int> modifiers_used;
    if (key.action_kind == ActionKind::Shortcut) {
        press.codes = shortcut_modifiers.value_or(std::vector<int>{KEY_LEFTCTRL});
    } else {
        modifiers_used = modifiers();
        press.codes.assign(modifiers_used.begin(), modifiers_used.end());
    }
    const int action_code = key.action_kind == ActionKind::Shortcut
                                ? (native_code ? native_code : key_code(shortcut_key(key.action)))
                                : code;
    press.codes.push_back(action_code);
    if (local) {
        if (code == KEY_CAPSLOCK || code == KEY_NUMLOCK) {
            const bool already_held =
                std::any_of(presses_.begin(), presses_.end(),
                            [code](const auto& item) { return item.second.local_lock == code; });
            press.local_lock = code;
            if (!already_held) {
                if (code == KEY_CAPSLOCK) {
                    caps_ = !caps_;
                } else {
                    num_ = !num_;
                }
            }
        }
        press.codes.clear(); // Local IME keeps visuals/holds but emits no physical keys.
    }
    if (!local && key.action_kind == ActionKind::Key && code != KEY_CAPSLOCK && code != KEY_NUMLOCK &&
        code != KEY_SCROLLLOCK && code != KEY_PAUSE && code != KEY_SYSRQ) {
        press.repeat_code = code;
        press.repeat_at = now + .5;
    }
    if (uses_modifiers) {
        use_modifiers();
    }
    // A held or locked Ctrl, Alt or Super goes down ahead of the first key
    // that uses it and stays down after the key (stays_down). The key still
    // takes it in its own chord, so its release order is unchanged.
    if (!press.codes.empty()) {
        for (const int modifier : modifiers_used) {
            if (stays_down(modifier) && wanted_down(modifier) && !held_down_.contains(modifier)) {
                hold_down(modifier);
            }
        }
    }
    // Store the release sequence first so an exception cannot strand an earlier down.
    presses_.emplace(pointer, press);
    for (int key_value : press.codes) {
        acquire(key_value);
    }
    if (output_) {
        output_->native_press(pointer, press.codes, press.repeat_code, now);
    }
    return true;
}

bool KeyboardState::continue_native(unsigned pointer, int code, double now) {
    const auto found = presses_.find(pointer);
    if (found == presses_.end() || found->second.modifier || !found->second.codes.empty() ||
        is_modifier(code)) {
        return false;
    }
    auto& press = found->second;
    press.codes = {code};
    press.repeat_code = code;
    press.repeat_at = now + .04;
    acquire(code);
    if (output_) {
        output_->native_press(pointer, press.codes, press.repeat_code, now, .04);
    }
    return true;
}
bool KeyboardState::up(unsigned pointer) {
    auto found = presses_.find(pointer);
    if (found == presses_.end()) {
        return false;
    }
    auto& press = found->second;
    // A locked modifier outlives the press that locked it: keys held now keep it.
    const bool stays_locked = press.modifier && locked_.contains(press.modifier) && !press.unlocks;
    if (press.modifier && press.used && !stays_locked) {
        const bool another_hold = std::any_of(presses_.begin(), presses_.end(), [&](const auto& item) {
            return item.first != pointer && item.second.modifier == press.modifier;
        });
        if (!another_hold) {
            if (output_) {
                output_->release_modifier(press.modifier);
            }
            // Chords borrow held modifiers. Releasing the modifier must remove
            // those borrowed references now, even if the ordinary key stays down.
            for (auto& [id, held] : presses_) {
                (void)id;
                const auto code = std::find(held.codes.begin(), held.codes.end(), press.modifier);
                if (code != held.codes.end()) {
                    release(*code);
                    held.codes.erase(code);
                }
            }
        }
    }
    if (press.unlocks) {
        // A tap on any locked key unlocks them all.
        locked_.clear();
        rings_.clear();
        last_tap_ = 0;
    } else if (press.modifier && !press.used && !press.locks) {
        if (press.double_tap) {
            // The second tap of a double tap locks, as holding does.
            latched_.erase(press.modifier);
            locked_.insert(press.modifier);
            new_locks_.push_back(pointer);
            last_tap_ = 0;
        } else {
            if (!latched_.erase(press.modifier)) {
                latched_.insert(press.modifier);
            }
            last_tap_ = press.modifier;
            last_tap_at_ = now_;
        }
    }
    // Release the ordinary key before its modifiers, including shared chords.
    for (auto it = press.codes.rbegin(); it != press.codes.rend(); ++it) {
        release(*it);
    }
    if (output_) {
        output_->release(pointer);
    }
    presses_.erase(found);
    // Last: a modifier this press held, or one it unlocked, now comes up
    // unless another hand holds it or it is still locked.
    settle_held_down();
    return true;
}
bool KeyboardState::stays_down(int code) const {
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        return false;
    }
    if (code == KEY_RIGHTALT) {
        return !(altgr_ && altgr_());
    }
    return is_modifier(code);
}
bool KeyboardState::wanted_down(int code) const {
    // A sticky press holds no code of its own; a momentary Super's does.
    return locked_.contains(code) ||
           std::any_of(presses_.begin(), presses_.end(), [code](const auto& item) {
               return item.second.modifier == code && item.second.codes.empty();
           });
}
void KeyboardState::hold_down(int code) {
    // Recorded before the backend call, so a failing send is still released.
    held_down_.insert(code);
    acquire(code); // Sends it without an output queue.
    if (output_) {
        output_->send(code, 1); // A direct hold, queued ahead of the key's own chord.
    }
}
void KeyboardState::settle_held_down() {
    for (auto it = held_down_.begin(); it != held_down_.end();) {
        const int code = *it;
        if (wanted_down(code)) {
            ++it;
            continue;
        }
        it = held_down_.erase(it);
        release(code);
        if (output_) {
            output_->send(code, 0);
        }
    }
}
std::optional<double> KeyboardState::lock_ring(const std::string& id) const {
    for (const auto& [pointer, press] : presses_) {
        (void)pointer;
        if (press.id == id && press.lockable && !press.used) {
            return press.at;
        }
    }
    if (const auto ring = rings_.find(id); ring != rings_.end()) {
        return ring->second;
    }
    return {};
}

void KeyboardState::cancel_pointer(unsigned pointer) {
    if (auto found = presses_.find(pointer); found != presses_.end()) {
        // An interrupted modifier is not a tap and must never become latched.
        found->second.used = true;
        up(pointer);
    }
}
void KeyboardState::release_presses() {
    std::vector<unsigned> pointers;
    for (const auto& [pointer, press] : presses_) {
        (void)press;
        pointers.push_back(pointer);
    }
    for (const auto pointer : pointers) {
        // A modifier pressed to unlock was interrupted too, so the locks stay.
        presses_.at(pointer).unlocks = false;
        cancel_pointer(pointer);
    }
    // A locked modifier kept down comes up too; it goes down again with the
    // next key that uses it.
    for (const int code : std::exchange(held_down_, {})) {
        release(code); // Sends it without an output queue; release_holds() lets it up there.
    }
    if (output_) {
        output_->release_holds();
    }
}
void KeyboardState::cancel_all() {
    end_all(true);
}
void KeyboardState::release_all() {
    end_all(false);
}
void KeyboardState::end_all(bool cancel_delivery) {
    std::exception_ptr failure;
    if (output_) {
        try {
            if (cancel_delivery) {
                output_->cancel();
            } else {
                output_->release_holds();
            }
        } catch (...) {
            failure = std::current_exception();
        }
    }
    // Release non-modifiers first; try every release even if one backend call fails.
    for (bool modifiers_only : {false, true}) {
        for (const auto& [code, count] : references_) {
            (void)count;
            if (is_modifier(code) != modifiers_only) {
                continue;
            }
            try {
                if (!output_) {
                    sink_.send(code, 0);
                }
            } catch (...) {
                failure = std::current_exception();
            }
        }
    }
    references_.clear();
    presses_.clear();
    latched_.clear();
    locked_.clear();
    held_down_.clear(); // Their references and holds went with the rest above.
    rings_.clear();
    last_tap_ = 0;
    if (failure) {
        std::rethrow_exception(failure);
    }
}
bool KeyboardState::tick(double now) {
    now_ = now;
    bool changed = false;
    for (auto& [pointer, press] : presses_) {
        if (press.lockable && !press.used && !press.locks && now - press.at >= lock_seconds) {
            // Locks while still held, so the key shows it before release.
            press.locks = true;
            latched_.erase(press.modifier);
            locked_.insert(press.modifier);
            rings_[press.id] = press.at; // Its ring goes round even if let go now.
            new_locks_.push_back(pointer);
            changed = true;
        }
    }
    // A ring that has gone round leaves the locked key's border, drawn from locked().
    for (auto ring = rings_.begin(); ring != rings_.end();) {
        if (now - ring->second >= lock_ring_seconds) {
            ring = rings_.erase(ring);
            changed = true;
        } else {
            ++ring;
        }
    }
    if (output_) {
        return changed;
    }
    std::set<int> repeated;
    for (auto& [pointer, press] : presses_) {
        (void)pointer;
        if (press.repeat_code && now >= press.repeat_at) {
            if (repeated.insert(press.repeat_code).second) {
                sink_.send(press.repeat_code, 2);
            }
            // Never replay a backlog after a stall or sleep.
            press.repeat_at = now + .04;
        }
    }
    return changed;
}
bool KeyboardState::pressed(const std::string& id) const {
    return std::any_of(presses_.begin(), presses_.end(),
                       [&](const auto& item) { return item.second.id == id; });
}
void KeyboardState::use_modifiers() {
    for (auto& [id, held] : presses_) {
        (void)id;
        if (held.modifier) {
            // A lock that came while the modifier was still held turns back
            // into a hold once a key uses it: that was a slow chord, such as
            // Alt held while the other hand aims at Tab, and it must come up
            // on release rather than stay locked (and down).
            if (held.locks) {
                held.locks = false;
                locked_.erase(held.modifier);
                rings_.erase(held.id);
            }
            held.used = true;
        }
    }
    latched_.clear();
}

std::set<int> KeyboardState::modifiers() const {
    auto result = latched_;
    result.insert(locked_.begin(), locked_.end());
    for (const auto& [pointer, press] : presses_) {
        (void)pointer;
        if (press.modifier) {
            result.insert(press.modifier);
        }
    }
    for (const auto& [code, count] : references_) {
        (void)count;
        if (is_modifier(code)) {
            result.insert(code);
        }
    }
    return result;
}
} // namespace framekeyboard
