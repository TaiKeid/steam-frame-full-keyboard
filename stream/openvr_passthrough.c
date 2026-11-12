// Pass-through OpenVR library for the Steam Link streaming client on the Frame.
//
// The streaming client is an SDL program that shows the stream through SDL's OpenVR video driver. That
// driver keeps only the first byte of each character SteamVR's keyboard sends, so nothing but ASCII
// reaches the PC, although the client and its protocol carry any text. This library gives Full Keyboard
// its own way in.
//
// SDL loads this file instead of libopenvr_api.so when the SDL_OPENVR_LIBRARY environment variable names
// it. The real library is a link-time dependency (DT_NEEDED), so:
//   - every OpenVR symbol SDL asks for, except VR_GetGenericInterface, resolves straight to the real
//   library;
//   - if the real library cannot be found, loading this file fails as a whole, exactly as stock would
//   fail.
//
// The one wrapped call hands SDL a copy of the overlay function table whose PollNextOverlayEvent is
// ours. It returns every event untouched and, once per event pump, types what Full Keyboard sent over a
// private local socket: text as SDL text events, keys as SDL key events. Steam's own keyboard and every
// other keyboard keep their stock behaviour.
//
// Nothing here stores or logs what is typed.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The interface SDL's driver requests at the revision Steam ships (SDL 3.4.0+1576 and +1616,
// openvr_capi.h). Any other version string is returned unwrapped, because the table layout below is only
// known for this one.
#define FK_OVERLAY_TABLE "FnTable:IVROverlay_026"
#define FK_SOCKET_NAME "framekeyboard-stream.sock"

enum {
    kOverlayFunctions = 82,     // function pointers in VR_IVROverlay_FnTable for IVROverlay_026
    kPollNextOverlayEvent = 48, // its slot in that table
    kMouseButtonDown = 301,     // EVREventType_VREvent_MouseButtonDown, the first field of VREvent_t
    kSdlTextInput = 0x303,      // SDL_EVENT_TEXT_INPUT
    kSdlKeyDown = 0x300,        // SDL_EVENT_KEY_DOWN
    kSdlKeyUp = 0x301,          // SDL_EVENT_KEY_UP
    kSdlEventBytes = 128,       // sizeof(SDL_Event)
    kTextSlots = 32,            // texts kept alive for events still in SDL's queue
    kTextBytes = 2048,          // longest text sent as one message
    kQueue = 8192,              // pending key events and texts, in typing order: 128 KB
    kKeysPerCodePoint = 20,     // Ctrl+Shift+U (6 events), up to 6 hex digits (12), commit key (2)
    kScancodes = 512,           // SDL_SCANCODE_COUNT
    kScancodeA = 4,             // SDL scancodes are USB HID usages
    kScancode1 = 30,
    kScancode0 = 39,
    kScancodeReturn = 40,
    kScancodeBackspace = 42,
    kScancodeTab = 43,
    kScancodeSpace = 44,
    kScancodeSlash = 56,
    kScancodeDelete = 76,
    kScancodeLeftCtrl = 224,
    kScancodeLeftShift = 225,
    kScancodeRightGui = 231, // the last modifier key
    kModLeftShift = 0x0001,  // SDL_KMOD_LSHIFT
    kModLeftCtrl = 0x0040,   // SDL_KMOD_LCTRL
    kProtocol = 1,
};

typedef intptr_t (*GetGenericInterfaceFn)(const char* version, int* error);
typedef bool (*PollFn)(uint64_t overlay, void* event, uint32_t size);
typedef bool (*PushEventFn)(void* event);
typedef void* (*GetKeyboardFocusFn)(void);
typedef uint32_t (*GetWindowIdFn)(void* window);

typedef union {
    struct { // SDL_TextInputEvent
        uint32_t type;
        uint32_t reserved;
        uint64_t timestamp; // 0: SDL fills it in
        uint32_t window_id;
        const char* text; // must outlive the queued event; see text_slots
    } text;
    struct { // SDL_KeyboardEvent
        uint32_t type;
        uint32_t reserved;
        uint64_t timestamp;
        uint32_t window_id;
        uint32_t which;
        uint32_t scancode;
        uint32_t key;
        uint16_t mod;
        uint16_t raw;
        uint8_t down;
        uint8_t repeat;
    } key;
    uint8_t padding[kSdlEventBytes];
} SdlEvent;

// One step of typing. A text owns its bytes until it is pushed.
typedef struct {
    char* text; // NULL for a key event
    uint16_t length;
    uint16_t scancode;
    uint16_t mod; // modifier state the event reports, as a real keyboard's would
    bool down;
    bool typed; // from a commit ('T'), so balanced: every key it presses it also releases
} Queued;

static void* overlay_table[kOverlayFunctions];
static PollFn real_poll;
static char text_slots[kTextSlots][kTextBytes];
static unsigned next_text_slot;
static char directory[400];

static Queued queue[kQueue];
static unsigned queue_head;
static unsigned queue_count;
static bool held[kScancodes]; // keys pushed down and not yet up, released when Full Keyboard goes away
static bool session_held[kScancodes]; // of those, pressed by a session's own key events ('K')
// Modifiers the session holds that are up on the PC for typed text, until a session press counts on
// them again (R242).
static bool lifted[kScancodes];
// A commit did not fit the queue: until it has drained, later commits and key presses are dropped as
// well, so what arrives is cut short rather than missing a piece in the middle.
static bool overflowed;
// Modifiers whose press was dropped while an overflow drained: a later press counting on one would
// reach the PC without it, so it is dropped as well, until the modifier's release (R219).
static unsigned dropped_mods;

static int stream_socket = -1;
static uint64_t next_connect_ms;

static bool switch_file(const char* name) {
    char path[512];
    return directory[0] != '\0' &&
           snprintf(path, sizeof path, "%s/%s", directory, name) < (int)sizeof path &&
           access(path, F_OK) == 0;
}

// Whole, well-formed UTF-8 only: a cut or malformed sequence is never typed.
static bool valid_utf8(const unsigned char* bytes, size_t length) {
    size_t i = 0;
    while (i < length) {
        const unsigned char lead = bytes[i];
        size_t more;
        uint32_t code;
        if (lead < 0x80) {
            more = 0;
            code = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
            code = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            code = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (i + more >= length) {
            return false; // the continuation bytes would run past the text
        }
        for (size_t k = 1; k <= more; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (bytes[i + k] & 0x3F);
        }
        if ((more == 2 && code < 0x800) || (more == 3 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

static PushEventFn push_event;
static GetKeyboardFocusFn keyboard_focus;
static GetWindowIdFn window_id;

static bool sdl_ready(void) {
    static bool looked_up;
    if (!looked_up) {
        looked_up = true;
        // SDL is a dependency of the program itself, so its public functions are in the global scope.
        push_event = (PushEventFn)dlsym(RTLD_DEFAULT, "SDL_PushEvent");
        keyboard_focus = (GetKeyboardFocusFn)dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus");
        window_id = (GetWindowIdFn)dlsym(RTLD_DEFAULT, "SDL_GetWindowID");
    }
    return push_event != NULL;
}

static uint32_t focused_window(void) {
    void* focus = keyboard_focus ? keyboard_focus() : NULL;
    return focus && window_id ? window_id(focus) : 0;
}

static bool push_text(const unsigned char* bytes, size_t length) {
    if (!sdl_ready() || length == 0 || length >= kTextBytes) {
        return false;
    }
    char* slot = text_slots[next_text_slot];
    next_text_slot = (next_text_slot + 1) % kTextSlots;
    memcpy(slot, bytes, length);
    slot[length] = '\0';

    SdlEvent event;
    memset(&event, 0, sizeof event);
    event.text.type = kSdlTextInput;
    event.text.window_id = focused_window();
    event.text.text = slot;
    return push_event(&event);
}

// The key code SDL itself gives a scancode on a US layout (SDL_GetDefaultKeyFromScancode): the
// character for keys that type one, Esc and Delete their control codes, and SDL_SCANCODE_TO_KEYCODE for
// the rest. The client sends it to the PC beside the scancode.
static uint32_t keycode_for(unsigned scancode) {
    // SDL_SCANCODE_1 to SDL_SCANCODE_SLASH, in scancode order.
    static const char symbols[] = "1234567890\r\x1b\b\t -=[]\\#;'`,./";
    _Static_assert(sizeof symbols - 1 == kScancodeSlash - kScancode1 + 1, "one symbol per scancode");
    if (scancode >= kScancodeA && scancode < kScancodeA + 26) {
        return 'a' + (scancode - kScancodeA);
    }
    if (scancode >= kScancode1 && scancode < kScancode1 + sizeof symbols - 1) {
        return (unsigned char)symbols[scancode - kScancode1];
    }
    if (scancode == kScancodeDelete) {
        return 0x7f;
    }
    return 0x40000000u | scancode; // SDL_SCANCODE_TO_KEYCODE
}

// A key counts as down or up only once SDL has taken the event: a release it refuses is tried again at
// the next clean-up (R249).
static void push_key(unsigned scancode, unsigned mod, bool down) {
    if (!sdl_ready() || scancode == 0 || scancode >= kScancodes) {
        return;
    }
    SdlEvent event;
    memset(&event, 0, sizeof event);
    event.key.type = down ? kSdlKeyDown : kSdlKeyUp;
    event.key.window_id = focused_window();
    event.key.scancode = scancode;
    event.key.key = keycode_for(scancode);
    event.key.mod = (uint16_t)mod;
    event.key.down = down;
    if (push_event(&event)) {
        held[scancode] = down;
    }
}

static bool queue_has_room(unsigned items) {
    return queue_count + items <= kQueue;
}

static void queue_key(unsigned scancode, unsigned mod, bool down, bool typed) {
    queue[(queue_head + queue_count++) % kQueue] = (Queued){.text = NULL,
                                                            .scancode = (uint16_t)scancode,
                                                            .mod = (uint16_t)mod,
                                                            .down = down,
                                                            .typed = typed};
}

static bool queue_text(const unsigned char* bytes, size_t length) {
    char* copy = malloc(length);
    if (!copy) {
        return false;
    }
    memcpy(copy, bytes, length);
    queue[(queue_head + queue_count++) % kQueue] =
        (Queued){.text = copy, .length = (uint16_t)length, .typed = true};
    return true;
}

static void queue_tap(unsigned scancode, bool shift) {
    const unsigned mod = shift ? kModLeftShift : 0;
    if (shift) {
        queue_key(kScancodeLeftShift, mod, true, true);
    }
    queue_key(scancode, mod, true, true);
    queue_key(scancode, mod, false, true);
    if (shift) {
        queue_key(kScancodeLeftShift, 0, false, true);
    }
}

// The key a US layout types an ASCII character with. The streaming client sends ASCII as key presses, as
// Steam's own keyboard path does; it drops text events that begin with ASCII.
static bool ascii_key(unsigned char c, unsigned* scancode, bool* shift) {
    static const char plain[] = "-=[]\\\0;'`,./"; // scancodes 45.. ; 50 is unused on a US layout
    static const char shifted[] = "_+{}|\0:\"~<>?";
    static const char digits_shifted[] = "!@#$%^&*()";
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *scancode = kScancodeA + (unsigned)(c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        *scancode = kScancodeA + (unsigned)(c - 'A');
        *shift = true;
    } else if (c >= '1' && c <= '9') {
        *scancode = kScancode1 + (unsigned)(c - '1');
    } else if (c == '0') {
        *scancode = kScancode0;
    } else if (c == ' ') {
        *scancode = kScancodeSpace;
    } else if (c == '\n' || c == '\r') {
        *scancode = kScancodeReturn;
    } else if (c == '\b') {
        *scancode = kScancodeBackspace;
    } else if (c == '\t') {
        *scancode = kScancodeTab;
    } else {
        for (unsigned i = 0; i < 10; ++i) {
            if (c == (unsigned char)digits_shifted[i]) {
                *scancode = kScancode1 + i;
                *shift = true;
                return true;
            }
        }
        for (unsigned i = 0; i < sizeof plain - 1; ++i) {
            if (plain[i] != '\0' && c == (unsigned char)plain[i]) {
                *scancode = 45 + i;
                return true;
            }
            if (shifted[i] != '\0' && c == (unsigned char)shifted[i]) {
                *scancode = 45 + i;
                *shift = true;
                return true;
            }
        }
        return false;
    }
    return true;
}

// One character as the key presses a Linux desktop reads as Unicode entry: Ctrl+Shift+U, the code in
// hex, Space.
static void queue_code_point(uint32_t code) {
    const unsigned ctrl = kModLeftCtrl;
    const unsigned both = kModLeftCtrl | kModLeftShift;
    const unsigned u = kScancodeA + ('u' - 'a');
    queue_key(kScancodeLeftCtrl, ctrl, true, true);
    queue_key(kScancodeLeftShift, both, true, true);
    queue_key(u, both, true, true);
    queue_key(u, both, false, true);
    queue_key(kScancodeLeftShift, ctrl, false, true);
    queue_key(kScancodeLeftCtrl, 0, false, true);
    char hex[8];
    const int digits = snprintf(hex, sizeof hex, "%x", (unsigned)code);
    for (int i = 0; i < digits; ++i) {
        unsigned scancode;
        bool shift;
        if (ascii_key((unsigned char)hex[i], &scancode, &shift)) {
            queue_tap(scancode, false);
        }
    }
    queue_tap(kScancodeSpace, false);
}

static size_t sequence_length(unsigned char lead) {
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

// How many queue entries validated UTF-8 takes, as queue_typing lays it out.
static size_t typing_entries(const unsigned char* bytes, size_t length, bool unicode_keys) {
    size_t entries = 0;
    size_t i = 0;
    while (i < length) {
        if (bytes[i] < 0x80) {
            unsigned scancode;
            bool shift;
            entries += ascii_key(bytes[i], &scancode, &shift) ? (shift ? 4 : 2) : 0;
            ++i;
        } else if (unicode_keys) {
            entries += kKeysPerCodePoint;
            i += sequence_length(bytes[i]);
        } else {
            ++entries; // a whole run of non-ASCII characters is one text
            while (i < length && bytes[i] >= 0x80) {
                i += sequence_length(bytes[i]);
            }
        }
    }
    return entries;
}

// Queues validated UTF-8 in typing order: ASCII as key presses, everything else as text (or, with the
// "unicode-keys" switch, as Unicode-entry key presses). False when the queue cannot take all of it.
static bool queue_typing(const unsigned char* bytes, size_t length) {
    const bool unicode_keys = switch_file("unicode-keys");
    if (queue_count + typing_entries(bytes, length, unicode_keys) > kQueue) {
        return false;
    }
    size_t i = 0;
    while (i < length) {
        if (bytes[i] < 0x80) {
            unsigned scancode;
            bool shift;
            if (ascii_key(bytes[i], &scancode, &shift)) {
                queue_tap(scancode, shift);
            }
            ++i;
            continue;
        }
        if (unicode_keys) {
            const size_t more = sequence_length(bytes[i]) - 1;
            uint32_t code = bytes[i] & (0x3F >> more);
            for (size_t k = 1; k <= more; ++k) {
                code = (code << 6) | (bytes[i + k] & 0x3F);
            }
            queue_code_point(code);
            i += more + 1;
            continue;
        }
        size_t end = i;
        while (end < length && bytes[end] >= 0x80) {
            end += sequence_length(bytes[end]);
        }
        if (!queue_text(bytes + i, end - i)) {
            return false;
        }
        i = end;
    }
    return true;
}

// The SDL_KMOD bit a modifier key sets, 0 for any other key.
static unsigned modifier_bit(unsigned scancode) {
    switch (scancode) {
    case 224:
        return 0x0040; // left Ctrl
    case 225:
        return 0x0001; // left Shift
    case 226:
        return 0x0100; // left Alt
    case 227:
        return 0x0400; // left GUI
    case 228:
        return 0x0080; // right Ctrl
    case 229:
        return 0x0002; // right Shift
    case 230:
        return 0x0200; // right Alt
    case 231:
        return 0x0800; // right GUI
    default:
        return 0;
    }
}

// The modifier bits of the keys down on the PC.
static unsigned held_mods(void) {
    unsigned bits = 0;
    for (unsigned scancode = kScancodeLeftCtrl; scancode <= kScancodeRightGui; ++scancode) {
        bits |= held[scancode] ? modifier_bit(scancode) : 0;
    }
    return bits;
}

// Typed text types its own characters, whatever the session holds (R242): a modifier the session holds
// that a typed press does not count on, such as the panel's Shift under a plain letter, goes up on the
// PC first. It comes down again only when a session press counts on it, so typed characters in a row
// see no modifier taps between them.
static void lift_session_modifiers(unsigned mod) {
    for (unsigned scancode = kScancodeLeftCtrl; scancode <= kScancodeRightGui; ++scancode) {
        const unsigned bit = modifier_bit(scancode);
        if (session_held[scancode] && held[scancode] && !(mod & bit)) {
            push_key(scancode, held_mods() & ~bit, false);
            lifted[scancode] = !held[scancode];
        }
    }
}

// A session press counting on a lifted modifier: the modifier goes down again first, unless the press
// is that modifier's own.
static void restore_session_modifiers(unsigned mod, unsigned pressed) {
    for (unsigned scancode = kScancodeLeftCtrl; scancode <= kScancodeRightGui; ++scancode) {
        const unsigned bit = modifier_bit(scancode);
        if (lifted[scancode] && (mod & bit)) {
            lifted[scancode] = false;
            if (scancode != pressed && !held[scancode]) {
                push_key(scancode, held_mods() | bit, true);
            }
        }
    }
}

// Types the next step: one text, or key events up to and including the next release, so a tap lands in
// one pump and the PC sees typing paced a frame apart.
static void pump_queue(void) {
    while (queue_count != 0) {
        Queued next = queue[queue_head];
        queue_head = (queue_head + 1) % kQueue;
        --queue_count;
        if (next.text) {
            push_text((const unsigned char*)next.text, next.length);
            free(next.text);
            return;
        }
        if (next.typed) {
            if (modifier_bit(next.scancode) && session_held[next.scancode] && held[next.scancode]) {
                // Typed text's own tap of a modifier the session holds down, such as a capital's Shift
                // while the panel's Shift is held: the modifier stays down for the session, whose own
                // release comes later, so later presses counting on it still have it (R241).
                if (!next.down) {
                    return; // the step ends where the release would have ended it
                }
                continue;
            }
            if (next.down) {
                lift_session_modifiers(next.mod);
                if (held[next.scancode]) {
                    // Any other key the session holds, such as Enter while text with a line break
                    // lands, comes up first, so the typed press is a press; the session's own release
                    // then finds it up (R243).
                    push_key(next.scancode, next.mod, false);
                }
            }
        } else if (!next.down && !held[next.scancode]) {
            // A release of the session's for a key the PC does not have down: its press was dropped,
            // or typed text let go of it first, so a key-up would be the only thing the PC sees of it
            // (R221). The session no longer holds it either (R228).
            session_held[next.scancode] = false;
            lifted[next.scancode] = false;
            continue;
        } else if (next.down) {
            restore_session_modifiers(next.mod, next.scancode);
        }
        push_key(next.scancode, next.mod, next.down);
        if (next.down && held[next.scancode]) {
            lifted[next.scancode] = false; // down again, as typed text's capital puts Shift
        }
        if (!next.typed) {
            session_held[next.scancode] = next.down;
        }
        if (!next.down) {
            return;
        }
    }
}

// Drops what is still queued and lets go of every key that is down: Full Keyboard went away or turned
// the library off.
static void release_everything(void) {
    while (queue_count != 0) {
        free(queue[queue_head].text);
        queue_head = (queue_head + 1) % kQueue;
        --queue_count;
    }
    for (unsigned scancode = 0; scancode < kScancodes; ++scancode) {
        if (held[scancode]) {
            push_key(scancode, 0, false);
        }
    }
    memset(session_held, 0, sizeof session_held);
    memset(lifted, 0, sizeof lifted);
    overflowed = false;
    dropped_mods = 0;
}

// A typing session ended ('R'): what was sent into the stream is still typed, in order, the session's
// own whole key taps included (a Backspace or Enter between two commits), so the PC gets what the panel
// showed. A press of the session's that no queued release follows is dropped, and a key it holds down
// with no release queued is let go of now; one whose release is queued comes up in its turn.
static void end_session(void) {
    dropped_mods = 0; // the next session starts with nothing pressed
    static bool keep[kQueue];
    bool released[kScancodes]; // a release is queued that no later press of the key has used
    memset(released, 0, sizeof released);
    for (unsigned i = queue_count; i-- > 0;) {
        const Queued* item = &queue[(queue_head + i) % kQueue];
        keep[i] = true;
        if (item->typed) {
            continue;
        }
        if (!item->down) {
            released[item->scancode] = true;
        } else if (released[item->scancode]) {
            released[item->scancode] = false; // a whole tap
        } else {
            keep[i] = false; // nothing would let it go
        }
    }
    // A modifier the session no longer holds for what is kept: one held down and let go of now, ahead
    // of the taps still queued, or one whose press is dropped. A kept press that counts on it (its
    // bit in `mod`) would type the plain key, Ctrl+C as C, so it goes too; releases always stay.
    unsigned gone = 0;
    for (unsigned scancode = 0; scancode < kScancodes; ++scancode) {
        if (session_held[scancode] && (held[scancode] || lifted[scancode]) && !released[scancode]) {
            gone |= modifier_bit(scancode);
        }
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < queue_count; ++i) {
        const Queued item = queue[(queue_head + i) % kQueue];
        if (keep[i] && !item.typed && item.down && (item.mod & gone)) {
            keep[i] = false;
        }
        if (!keep[i]) {
            gone |= item.typed ? 0 : modifier_bit(item.scancode);
            continue;
        }
        queue[(queue_head + kept++) % kQueue] = item; // never ahead of what is read
    }
    queue_count = kept;
    for (unsigned scancode = 0; scancode < kScancodes; ++scancode) {
        // What is left in `released` is a release for a key already down, or lifted for typed text.
        if (session_held[scancode] && (held[scancode] || lifted[scancode]) && !released[scancode]) {
            if (held[scancode]) {
                push_key(scancode, 0, false);
            }
            session_held[scancode] = false;
            lifted[scancode] = false;
        }
    }
}

static uint64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
}

static void stream_disconnect(void) {
    if (stream_socket >= 0) {
        close(stream_socket);
        stream_socket = -1;
        release_everything();
    }
}

// The library hangs up on Full Keyboard itself, which then ends the typing session (R248). It connects
// again only a second later, so the keyboard sees the two apart.
static void hang_up(void) {
    stream_disconnect();
    next_connect_ms = now_ms() + 1000;
}

// Full Keyboard listens; this side connects, at most once a second while there is nobody there.
static void stream_connect(void) {
    const uint64_t now = now_ms();
    if (now < next_connect_ms) {
        return;
    }
    next_connect_ms = now + 1000;
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!runtime || snprintf(address.sun_path, sizeof address.sun_path, "%s/%s", runtime,
                             FK_SOCKET_NAME) >= (int)sizeof address.sun_path) {
        return;
    }
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    if (connect(fd, (const struct sockaddr*)&address, sizeof address) != 0) {
        close(fd);
        return;
    }
    // Only the same user's keyboard: the runtime directory is private to the user, and the peer must be
    // too.
    struct ucred peer;
    socklen_t peer_size = sizeof peer;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 || peer.uid != getuid()) {
        close(fd);
        return;
    }
    const uint32_t pid = (uint32_t)getpid();
    unsigned char hello[6] = {'H', kProtocol};
    memcpy(hello + 2, &pid, sizeof pid);
    if (send(fd, hello, sizeof hello, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof hello) {
        close(fd);
        return;
    }
    stream_socket = fd;
}

// Messages from Full Keyboard, one per packet:
//   'T' text...                      a commit: UTF-8, typed in order
//   'K' scancode:u16 mod:u16 down:u8  one key event (SDL scancode and modifier bits, little endian)
//   'R'                               the session ended: release the keys its key events hold and drop
//                                    those still queued; committed text is still typed
static void stream_message(const unsigned char* message, size_t length) {
    overflowed = overflowed && queue_count != 0;
    if (length >= 2 && message[0] == 'T') {
        // Whole, well-formed text only. What the queue cannot take is dropped, and so is what follows
        // until the queue has drained.
        if (!overflowed && valid_utf8(message + 1, length - 1) &&
            !queue_typing(message + 1, length - 1)) {
            overflowed = true;
        }
    } else if (length == 6 && message[0] == 'K') {
        uint16_t scancode;
        uint16_t mod;
        memcpy(&scancode, message + 1, sizeof scancode);
        memcpy(&mod, message + 3, sizeof mod);
        const bool down = message[5] != 0;
        if (scancode == 0 || scancode >= kScancodes) {
            return;
        }
        if (!queue_has_room(1)) {
            // No room for a key event: the queue is far behind the user. Left out, a press would type
            // the text around it without a Backspace, an Enter or a shortcut the user pressed, and a
            // release could leave a key down, so the library hangs up instead (R247): every key comes
            // up, what is queued is dropped, and Full Keyboard ends the typing session.
            hang_up();
            return;
        }
        const unsigned bit = modifier_bit(scancode);
        if (down && (mod & dropped_mods)) {
            dropped_mods |= bit; // counts on a modifier the PC never got
            return;
        }
        if (!down) {
            dropped_mods &= ~bit;
        }
        if (!down || !overflowed) {
            queue_key(scancode, mod, down, false);
        } else {
            dropped_mods |= bit; // a press while a commit's overflow drains
        }
    } else if (length == 1 && message[0] == 'R') {
        end_session();
    }
    // Anything else is from a keyboard newer than this library: ignored.
}

static void stream_tick(void) {
    if (switch_file("off")) {
        stream_disconnect();
        return;
    }
    if (stream_socket < 0) {
        stream_connect();
    }
    for (int i = 0; i < 32 && stream_socket >= 0; ++i) {
        unsigned char message[kTextBytes];
        const ssize_t length = recv(stream_socket, message, sizeof message, MSG_DONTWAIT);
        if (length > 0) {
            stream_message(message, (size_t)length);
        } else if (length == 0) {
            stream_disconnect(); // Full Keyboard went away.
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        } else {
            stream_disconnect();
        }
    }
}

static bool fk_poll(uint64_t overlay, void* event, uint32_t size) {
    if (!real_poll(overlay, event, size)) {
        // End of this pump: SDL has every real event. Now type the next step of ours.
        stream_tick();
        pump_queue();
        return false;
    }
    uint32_t type;
    if (size >= sizeof type && stream_socket >= 0) {
        memcpy(&type, event, sizeof type);
        if (type == kMouseButtonDown) {
            // The user clicked the stream. Steam sends no keyboard request for that, so Full Keyboard
            // is told: a panel that is already showing then types here.
            send(stream_socket, "F", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
    }
    return true; // every event reaches SDL as it came: other keyboards keep their stock behaviour
}

static GetGenericInterfaceFn real_get_generic_interface(void) {
    static GetGenericInterfaceFn real;
    if (!real) {
        // Already loaded as this file's dependency; NOLOAD only fetches its handle.
        void* library = dlopen("libopenvr_api.so", RTLD_NOW | RTLD_NOLOAD);
        if (library) {
            real = (GetGenericInterfaceFn)dlsym(library, "VR_GetGenericInterface");
        }
    }
    return real;
}

__attribute__((visibility("default"))) intptr_t VR_GetGenericInterface(const char* version, int* error) {
    const GetGenericInterfaceFn real = real_get_generic_interface();
    if (!real) {
        if (error) {
            *error = 105; // VRInitError_Init_InterfaceNotFound
        }
        return 0;
    }
    const intptr_t table = real(version, error);
    if (!table || !version || strcmp(version, FK_OVERLAY_TABLE) != 0) {
        return table;
    }
    memcpy(overlay_table, (void* const*)table, sizeof overlay_table);
    if (!overlay_table[kPollNextOverlayEvent]) {
        return table;
    }
    real_poll = (PollFn)overlay_table[kPollNextOverlayEvent];
    overlay_table[kPollNextOverlayEvent] = (void*)fk_poll;
    return (intptr_t)overlay_table;
}

__attribute__((constructor)) static void fk_loaded(void) {
    // Switch files live next to this file: "off" (do nothing) and "unicode-keys" (type non-ASCII as
    // Ctrl+Shift+U key presses, for a Linux PC whose desktop does not take text from Steam).
    Dl_info info;
    if (dladdr((void*)fk_loaded, &info) && info.dli_fname) {
        const char* slash = strrchr(info.dli_fname, '/');
        if (slash && snprintf(directory, sizeof directory, "%.*s", (int)(slash - info.dli_fname),
                              info.dli_fname) >= (int)sizeof directory) {
            directory[0] = '\0';
        }
    }
}
