// Loads the pass-through library the way SDL does, plays overlay events through it and acts as Full
// Keyboard on the private socket. The SDL functions the library looks up are defined here and exported
// to it.
//
// Arguments: the library, then the stand-in for libopenvr_api.so (tests/stream_fake_openvr.c).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    kOverlayFunctions = 82,
    kPollNextOverlayEvent = 48,
    kMouseButtonDown = 301,
    kKeyboardCharInput = 1201,
    kEventBytes = 60,
    kSdlKeyDown = 0x300,
    kSdlKeyUp = 0x301,
    kSdlTextInput = 0x303,
};

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t window_id;
    const char* text;
} TextEvent;

typedef struct {
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
} KeyEvent;

// Everything pushed, in order: a text has scancode 0.
typedef struct {
    char text[64];
    uint32_t scancode;
    uint32_t key; // the SDL key code
    uint16_t mod;
    uint8_t down;
    uint32_t window_id;
} Pushed;

static Pushed pushed[128];
static int pushed_count;
static int failures;

bool SDL_PushEvent(void* event) {
    const TextEvent* text = event;
    if (pushed_count >= 128) {
        ++failures;
        return false;
    }
    Pushed* entry = &pushed[pushed_count++];
    memset(entry, 0, sizeof *entry);
    entry->window_id = text->window_id;
    if (text->type == kSdlTextInput) {
        snprintf(entry->text, sizeof entry->text, "%s", text->text);
    } else if (text->type == kSdlKeyDown || text->type == kSdlKeyUp) {
        const KeyEvent* key = event;
        entry->scancode = key->scancode;
        entry->key = key->key;
        entry->mod = key->mod;
        entry->down = key->down;
        if ((key->type == kSdlKeyDown) != (key->down != 0)) {
            ++failures;
        }
    } else {
        ++failures;
    }
    return true;
}
void* SDL_GetKeyboardFocus(void) {
    return (void*)0x77;
}
uint32_t SDL_GetWindowID(void* window) {
    return window == (void*)0x77 ? 5 : 0;
}

static void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static bool is_text(int index, const char* text) {
    return index < pushed_count && pushed[index].scancode == 0 && strcmp(pushed[index].text, text) == 0;
}

static bool is_key(int index, uint32_t scancode, uint16_t mod, uint8_t down) {
    return index < pushed_count && pushed[index].scancode == scancode && pushed[index].mod == mod &&
           pushed[index].down == down && pushed[index].window_id == 5;
}

static char directory[64]; // private to this run: the socket and the switch files

static const char* in_directory(const char* name) {
    static char path[160];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    return path;
}

static void switch_file(const char* name, bool present) {
    if (present) {
        FILE* file = fopen(in_directory(name), "w");
        check(file != NULL, "switch file created");
        if (file) {
            fclose(file);
        }
    } else {
        unlink(in_directory(name));
    }
}

static bool (*poll_overlay)(uint64_t, void*, uint32_t);
static unsigned char event[kEventBytes];

// One event pump with no overlay events: the library types its next step.
static bool idle_pump(void) {
    return !poll_overlay(1, event, sizeof event);
}

static void key(int keyboard, uint16_t scancode, uint16_t mod, bool down) {
    unsigned char packet[6] = {'K'};
    memcpy(packet + 1, &scancode, sizeof scancode);
    memcpy(packet + 3, &mod, sizeof mod);
    packet[5] = down;
    check(send(keyboard, packet, sizeof packet, MSG_NOSIGNAL) == (ssize_t)sizeof packet,
          "key packet sent");
}

// The library looks for the keyboard at most once a second.
static int accept_keyboard(int server) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        idle_pump();
        const int keyboard = accept(server, NULL, NULL);
        if (keyboard >= 0) {
            // Room for the longest burst sent here without a pump in between (295 key packets): a
            // default socket holds about 278, and a blocked send would hang the test.
            const int room = 1 << 20;
            setsockopt(keyboard, SOL_SOCKET, SO_SNDBUF, &room, sizeof room);
            unsigned char hello[16];
            uint32_t pid = 0;
            const bool said_hello = recv(keyboard, hello, sizeof hello, MSG_DONTWAIT) == 6 &&
                                    hello[0] == 'H' && hello[1] == 1;
            if (said_hello) {
                memcpy(&pid, hello + 2, sizeof pid);
            }
            check(said_hello && pid == (uint32_t)getpid(), "hello carries the protocol and the process");
            return keyboard;
        }
        usleep(50000);
    }
    return -1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    const char* temporary = getenv("TMPDIR");
    if (snprintf(directory, sizeof directory, "%s/fk-stream.XXXXXX", temporary ? temporary : "/tmp") >=
            (int)sizeof directory ||
        !mkdtemp(directory)) {
        printf("FAIL: no private directory\n");
        return 1;
    }
    // The library finds the socket here, and reads switch files beside itself.
    setenv("XDG_RUNTIME_DIR", directory, 1);
    char library_path[160];
    snprintf(library_path, sizeof library_path, "%s", in_directory("framekeyboard-stream.so"));
    check(symlink(argv[1], library_path) == 0, "library linked into the private directory");

    // Full Keyboard's side of the private socket, listening before the library first looks for it.
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof address.sun_path, "%s", in_directory("framekeyboard-stream.sock"));
    const int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    check(server >= 0 && bind(server, (struct sockaddr*)&address, sizeof address) == 0 &&
              listen(server, 1) == 0,
          "keyboard socket listens");

    // The streaming client has the real library on its search path; here it is loaded by name first.
    void* fake = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    void* library = fake ? dlopen(library_path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!library) {
        printf("FAIL: dlopen: %s\n", dlerror());
        return 1;
    }
    void (*add)(uint32_t, const char*, int) =
        (void (*)(uint32_t, const char*, int))dlsym(fake, "fake_add_event");
    void (*reset)(void) = (void (*)(void))dlsym(fake, "fake_reset");
    void** fake_table = dlsym(fake, "fake_overlay_table");
    void* fake_other = dlsym(fake, "fake_other_table");

    // A handle's lookup covers the library and what it depends on: these resolve through DT_NEEDED.
    check(dlsym(library, "VR_InitInternal") == dlsym(fake, "VR_InitInternal"),
          "VR_InitInternal is the real one");
    check(dlsym(library, "VR_GetVRInitErrorAsEnglishDescription") != NULL,
          "error text function resolves");
    intptr_t (*get)(const char*, int*) =
        (intptr_t (*)(const char*, int*))dlsym(library, "VR_GetGenericInterface");
    check(get != (intptr_t (*)(const char*, int*))dlsym(fake, "VR_GetGenericInterface"),
          "interface call is wrapped");

    int error = 9;
    check((void*)get("FnTable:IVRSystem_022", &error) == fake_other, "other interfaces pass through");
    check((void*)get("FnTable:IVROverlay_027", &error) == fake_other,
          "unknown overlay version is not wrapped");
    void** table = (void**)get("FnTable:IVROverlay_026", &error);
    check(table != fake_table, "overlay table is a copy");
    for (int i = 0; i < kOverlayFunctions; ++i) {
        if (i != kPollNextOverlayEvent) {
            check(table[i] == fake_table[i], "other slots are unchanged");
        }
    }
    poll_overlay = (bool (*)(uint64_t, void*, uint32_t))table[kPollNextOverlayEvent];

    // 1. Another keyboard's characters reach SDL untouched, whatever they are, and nothing is typed for
    // them.
    reset();
    add(kKeyboardCharInput, "a", 1);
    add(kKeyboardCharInput, "\xe3\x81\x82", 3);
    add(300, "\xe3\x81\x82", 3);
    for (int i = 0; i < 3; ++i) {
        check(poll_overlay(1, event, sizeof event), "event is returned");
    }
    check(event[12] == 0xe3, "the event is unchanged");
    check(idle_pump() && idle_pump() && pushed_count == 0, "nothing is typed for other keyboards");

    // 2. The private socket: the library connected and said hello.
    int keyboard = accept_keyboard(server);
    check(keyboard >= 0, "library connected to the keyboard");

    // 3. A mixed commit: ASCII as key presses, the rest as text, in order, one step per pump.
    const char commit[] = "Ta\xd0\x91!"; // a, Cyrillic Be, exclamation mark
    check(send(keyboard, commit, sizeof commit - 1, 0) == (ssize_t)sizeof commit - 1, "commit sent");
    check(idle_pump() && pushed_count == 2 && is_key(0, 4, 0, 1) && is_key(1, 4, 0, 0),
          "the letter is one tap");
    check(idle_pump() && pushed_count == 3 && is_text(2, "\xd0\x91"), "then the text");
    check(idle_pump() && pushed_count == 6 && is_key(3, 225, 1, 1) && is_key(4, 30, 1, 1) &&
              is_key(5, 30, 1, 0),
          "then Shift+1");
    check(idle_pump() && pushed_count == 7 && is_key(6, 225, 0, 0), "then Shift released");
    check(idle_pump() && pushed_count == 7, "nothing more");

    // 4. A click on the stream is reported to the keyboard, and the event still reaches SDL.
    reset();
    add(kMouseButtonDown, "", 0);
    check(poll_overlay(1, event, sizeof event), "the click reaches SDL");
    unsigned char told[8];
    check(recv(keyboard, told, sizeof told, MSG_DONTWAIT) == 1 && told[0] == 'F',
          "the keyboard hears of the click");
    check(idle_pump(), "queue ends after the click");

    // 5. "unicode-keys": a character becomes Ctrl+Shift+U, its code in hex and Space.
    switch_file("unicode-keys", true);
    pushed_count = 0;
    check(send(keyboard, "T\xd0\xbf", 3, 0) == 3, "character sent"); // U+043F
    check(idle_pump() && pushed_count == 4 && is_key(0, 224, 0x40, 1) && is_key(1, 225, 0x41, 1) &&
              is_key(2, 24, 0x41, 1) && is_key(3, 24, 0x41, 0),
          "Ctrl+Shift+U");
    check(idle_pump() && is_key(4, 225, 0x40, 0), "Shift up");
    check(idle_pump() && is_key(5, 224, 0, 0), "Ctrl up");
    check(idle_pump() && is_key(6, 33, 0, 1) && is_key(7, 33, 0, 0), "4");
    check(idle_pump() && is_key(8, 32, 0, 1) && is_key(9, 32, 0, 0), "3");
    check(idle_pump() && is_key(10, 9, 0, 1) && is_key(11, 9, 0, 0), "f");
    check(idle_pump() && pushed_count == 14 && is_key(12, 44, 0, 1) && is_key(13, 44, 0, 0), "Space");
    switch_file("unicode-keys", false);

    // 6. Key events: a tap lands in one pump, a held key stays down until the keyboard asks for a
    // release.
    pushed_count = 0;
    key(keyboard, 42, 0, true);
    key(keyboard, 42, 0, false);
    key(keyboard, 224, 0x40, true);
    check(idle_pump() && pushed_count == 2 && is_key(0, 42, 0, 1) && is_key(1, 42, 0, 0),
          "Backspace tap");
    check(idle_pump() && pushed_count == 3 && is_key(2, 224, 0x40, 1), "Ctrl held");
    check(send(keyboard, "R", 1, 0) == 1 && idle_pump() && pushed_count == 4 && is_key(3, 224, 0, 0),
          "a release request lets go of it");
    // Each key carries the key code SDL itself gives it on a US layout: a letter, a digit, Esc,
    // punctuation, Delete, and F1, which types nothing.
    const struct {
        uint16_t scancode;
        uint32_t key;
    } codes[] = {{4, 'a'},   {39, '0'}, {41, 0x1b}, {45, '-'},  {49, '\\'},
                 {52, '\''}, {55, '.'}, {56, '/'},  {76, 0x7f}, {58, 0x40000000u | 58}};
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
        pushed_count = 0;
        key(keyboard, codes[i].scancode, 0, true);
        key(keyboard, codes[i].scancode, 0, false);
        check(idle_pump() && pushed_count == 2 && pushed[0].key == codes[i].key &&
                  pushed[1].key == codes[i].key,
              "SDL's own key code");
    }

    // 6b. A session that ends keeps what was committed into the stream: the text is still typed, the
    // session's own key events still queued are dropped.
    pushed_count = 0;
    check(send(keyboard, "Tabc", 4, 0) == 4, "commit sent");
    key(keyboard, 224, 0x40, true);
    check(idle_pump() && pushed_count == 2 && is_key(0, 4, 0, 1) && is_key(1, 4, 0, 0), "a");
    check(send(keyboard, "R", 1, 0) == 1, "the session ends");
    check(idle_pump() && pushed_count == 4 && is_key(2, 5, 0, 1) && is_key(3, 5, 0, 0), "then b");
    check(idle_pump() && pushed_count == 6 && is_key(4, 6, 0, 1) && is_key(5, 6, 0, 0), "then c");
    check(idle_pump() && pushed_count == 6, "and not the session's Ctrl");
    // A whole tap of the session's between two commits stays with them: "ab", Backspace, "c" arrives
    // as typed, though the session ends before it is.
    pushed_count = 0;
    check(send(keyboard, "Tab", 3, 0) == 3, "a commit");
    key(keyboard, 42, 0, true);
    key(keyboard, 42, 0, false);
    check(send(keyboard, "Tc", 2, 0) == 2, "another after a Backspace");
    check(idle_pump() && pushed_count == 2 && is_key(0, 4, 0, 1), "a");
    check(send(keyboard, "R", 1, 0) == 1, "the session ends");
    check(idle_pump() && pushed_count == 4 && is_key(2, 5, 0, 1), "then b");
    check(idle_pump() && pushed_count == 6 && is_key(4, 42, 0, 1) && is_key(5, 42, 0, 0),
          "then Backspace");
    check(idle_pump() && pushed_count == 8 && is_key(6, 6, 0, 1), "then c");
    check(idle_pump() && pushed_count == 8, "and nothing else");
    // A shortcut cut by a session's end goes whole: Ctrl+C was pressed behind a commit, but Ctrl is not
    // let go of before the session ends, so its press is dropped and C must not be typed alone.
    pushed_count = 0;
    check(send(keyboard, "Txy", 3, 0) == 3, "a commit");
    key(keyboard, 224, 0x40, true);
    key(keyboard, 6, 0x40, true);
    key(keyboard, 6, 0x40, false);
    check(idle_pump() && pushed_count == 2 && is_key(0, 27, 0, 1), "x");
    check(send(keyboard, "R", 1, 0) == 1, "the session ends with Ctrl down");
    {
        unsigned y = 0, presses = 0;
        for (int i = 0; i < 4; ++i) {
            idle_pump();
        }
        for (int k = 2; k < pushed_count; ++k) {
            y += pushed[k].scancode == 28 && pushed[k].down;
            presses += pushed[k].down && (pushed[k].scancode == 6 || pushed[k].scancode == 224);
        }
        check(y == 1 && presses == 0, "y is typed, and neither Ctrl nor C is pressed");
    }
    key(keyboard, 224, 0, false);
    idle_pump();
    // Ctrl is down and Ctrl+C waits behind a commit when the session ends with Ctrl still held: Ctrl
    // is let go of at once, so C must not be typed after it.
    key(keyboard, 224, 0x40, true);
    pushed_count = 0;
    check(idle_pump() && pushed_count == 1 && is_key(0, 224, 0x40, 1), "Ctrl held");
    check(send(keyboard, "Txy", 3, 0) == 3, "a commit behind it");
    key(keyboard, 6, 0x40, true);
    key(keyboard, 6, 0x40, false);
    check(idle_pump() && pushed_count == 3 && is_key(1, 27, 0, 1), "x");
    check(send(keyboard, "R", 1, 0) == 1, "the session ends with Ctrl down");
    {
        unsigned ctrl_up = 0, y = 0, c = 0;
        for (int i = 0; i < 4; ++i) {
            idle_pump();
        }
        for (int k = 3; k < pushed_count; ++k) {
            ctrl_up += pushed[k].scancode == 224 && !pushed[k].down;
            y += pushed[k].scancode == 28 && pushed[k].down;
            c += pushed[k].scancode == 6;
        }
        check(ctrl_up == 1 && y == 1 && c == 0,
              "Ctrl goes up, y is typed, C is neither pressed nor released");
    }
    // The panel's Shift is held when typed text's own capital lets Shift go first, so the session's
    // later release is skipped: the session no longer holds Shift, and a session's end in the next
    // capital's Shift does not take a whole Shift+X tap for one that counts on a Shift let go of.
    key(keyboard, 225, 0x01, true);
    idle_pump();
    check(send(keyboard, "TA", 2, 0) == 2, "a capital");
    idle_pump();
    idle_pump();
    key(keyboard, 225, 0, false);
    idle_pump();
    check(send(keyboard, "TB", 2, 0) == 2, "another capital");
    key(keyboard, 225, 0x01, true);
    key(keyboard, 27, 0x01, true);
    key(keyboard, 27, 0x01, false);
    key(keyboard, 225, 0, false);
    pushed_count = 0;
    check(idle_pump() && pushed_count == 3 && is_key(1, 5, 0x01, 1), "B's Shift and B");
    check(send(keyboard, "R", 1, 0) == 1, "the session ends in B's Shift");
    {
        unsigned x = 0;
        for (int i = 0; i < 6; ++i) {
            idle_pump();
        }
        for (int k = 0; k < pushed_count; ++k) {
            x += pushed[k].scancode == 27 && pushed[k].down;
        }
        check(x == 1, "the session's Shift+X is typed");
    }

    // 6c. A commit that does not fit is dropped with what follows it until the queue has drained, so the
    // text is cut short, never missing a middle piece; a release still goes through.
    {
        char big[2001];
        big[0] = 'T';
        memset(big + 1, 'a', 2000); // 4000 entries; two of them fill all but 192 of the 8192
        check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big &&
                  send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big,
              "two long commits sent");
        memset(big + 1, 'b', 2000);
        check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big, "one that does not fit");
        check(send(keyboard, "Tc", 2, 0) == 2, "a short one after it");
        key(keyboard, 40, 0, true);
        key(keyboard, 40, 0, false);
        unsigned letters = 0, enter_up = 0, other = 0;
        for (int i = 0; i < 4100; ++i) {
            pushed_count = 0;
            idle_pump();
            for (int k = 0; k < pushed_count; ++k) {
                if (pushed[k].scancode == 4) {
                    ++letters;
                } else if (pushed[k].scancode == 40 && !pushed[k].down) {
                    ++enter_up;
                } else {
                    ++other;
                }
            }
        }
        check(letters == 8000 && enter_up == 0 && other == 0,
              "the long commits are typed; b, c and Enter are not, not even Enter's release");
        pushed_count = 0;
        check(send(keyboard, "Td", 2, 0) == 2 && idle_pump() && pushed_count == 2 && is_key(0, 7, 0, 1),
              "a drained queue takes commits again");

        // A release that finds the queue full while its press still waits there cancels the press.
        memset(big + 1, 'a', 2000);
        check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big &&
                  send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big,
              "8000 entries");
        idle_pump(); // takes them and types the first tap: 7998 entries wait
        // Each pump takes up to 32 messages, then types one tap (2 entries), so the queue fills by the
        // seventh pump and then has 2 free entries as each pump begins. 94 y taps and z's press (189
        // messages) all fit; w presses fill the rest, and z's release, the 295th message, comes seventh in
        // its pump's batch, after the two w presses that took the room.
        for (int i = 0; i < 94; ++i) {
            key(keyboard, 28, 0, true);
            key(keyboard, 28, 0, false);
        }
        key(keyboard, 29, 0, true);
        for (int i = 0; i < 105; ++i) {
            key(keyboard, 26, 0, true);
        }
        key(keyboard, 29, 0, false);
        unsigned z_down = 0;
        for (int i = 0; i < 4200; ++i) {
            pushed_count = 0;
            idle_pump();
            for (int k = 0; k < pushed_count; ++k) {
                z_down += pushed[k].scancode == 29 && pushed[k].down;
            }
        }
        check(z_down == 0, "the press whose release found no room is never typed");
        key(keyboard, 26, 0, false); // let go of w again
        pushed_count = 0;
        check(idle_pump() && pushed_count == 1 && is_key(0, 26, 0, 0), "w released");

        // Ctrl is down and Ctrl+Y taps wait behind a backlog when Ctrl's release finds no room: it goes
        // at once, and the Y presses counting on it are cancelled, never typed as plain Y. The same
        // batching as above puts Ctrl's release seventh in its pump's batch.
        key(keyboard, 224, 0x40, true);
        pushed_count = 0;
        check(idle_pump() && pushed_count == 1 && is_key(0, 224, 0x40, 1), "Ctrl down");
        memset(big + 1, 'a', 2000);
        check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big &&
                  send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big,
              "8000 entries behind it");
        idle_pump();
        for (int i = 0; i < 94; ++i) {
            key(keyboard, 28, 0x40, true);
            key(keyboard, 28, 0x40, false);
        }
        for (int i = 0; i < 106; ++i) {
            key(keyboard, 26, 0, true);
        }
        key(keyboard, 224, 0, false);
        unsigned y_down = 0, ctrl_up = 0;
        for (int i = 0; i < 4200; ++i) {
            pushed_count = 0;
            idle_pump();
            for (int k = 0; k < pushed_count; ++k) {
                y_down += pushed[k].scancode == 28 && pushed[k].down;
                ctrl_up += pushed[k].scancode == 224 && !pushed[k].down;
            }
        }
        check(ctrl_up == 1 && y_down == 0, "Ctrl goes up at once and no Y is typed without it");
        key(keyboard, 26, 0, false);
        idle_pump();

        // A Ctrl press the queue cannot take, while an overflow drains, is remembered: a C pressed with
        // it after the queue has drained is not typed as plain C, and after Ctrl's release keys type
        // again.
        memset(big + 1, 'a', 2000);
        for (int i = 0; i < 3; ++i) {
            check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big, "a long commit");
        }
        key(keyboard, 224, 0x40, true); // dropped: the third commit overflowed
        unsigned c_down = 0, ctrl_down = 0;
        for (int i = 0; i < 4100; ++i) {
            pushed_count = 0;
            idle_pump();
        }
        key(keyboard, 6, 0x40, true);
        key(keyboard, 6, 0x40, false);
        key(keyboard, 224, 0, false);
        key(keyboard, 6, 0, true);
        key(keyboard, 6, 0, false);
        pushed_count = 0;
        for (int i = 0; i < 4; ++i) {
            idle_pump();
        }
        for (int k = 0; k < pushed_count; ++k) {
            c_down += pushed[k].scancode == 6 && pushed[k].down && pushed[k].mod == 0;
            ctrl_down += pushed[k].scancode == 224 && pushed[k].down;
            check(!(pushed[k].scancode == 6 && pushed[k].down && pushed[k].mod != 0),
                  "no C with Ctrl's bit but no Ctrl");
        }
        check(c_down == 1 && ctrl_down == 0, "after Ctrl's release, a plain C types again");

        // Ctrl is down on the PC, and behind a backlog wait Ctrl's release, a whole new Ctrl+C and
        // another Ctrl press. A Ctrl release that finds the queue full belongs to that last press: only
        // it is cancelled, and the Ctrl+C is still typed with Ctrl (R230). Same batching as above.
        key(keyboard, 224, 0x40, true);
        idle_pump();
        memset(big + 1, 'a', 2000);
        check(send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big &&
                  send(keyboard, big, sizeof big, 0) == (ssize_t)sizeof big,
              "8000 entries");
        idle_pump();
        key(keyboard, 224, 0, false);
        key(keyboard, 224, 0x40, true);
        key(keyboard, 6, 0x40, true);
        key(keyboard, 6, 0x40, false);
        key(keyboard, 224, 0, false);
        key(keyboard, 224, 0x40, true);
        for (int i = 0; i < 91; ++i) {
            key(keyboard, 28, 0, true);
            key(keyboard, 28, 0, false);
        }
        for (int i = 0; i < 106; ++i) {
            key(keyboard, 26, 0, true);
        }
        key(keyboard, 224, 0, false);
        {
            unsigned c_with_ctrl = 0;
            for (int i = 0; i < 4200; ++i) {
                pushed_count = 0;
                idle_pump();
                for (int k = 0; k < pushed_count; ++k) {
                    c_with_ctrl += pushed[k].scancode == 6 && pushed[k].down && pushed[k].mod == 0x40;
                }
            }
            check(c_with_ctrl == 1, "the whole Ctrl+C behind the cancelled press is typed");
        }
        key(keyboard, 26, 0, false);
        idle_pump();
    }

    // 7. "off": the library lets go of held keys, hangs up and types nothing.
    pushed_count = 0;
    key(keyboard, 225, 1, true);
    check(idle_pump() && pushed_count == 1 && is_key(0, 225, 1, 1), "Shift held");
    switch_file("off", true);
    check(idle_pump() && pushed_count == 2 && is_key(1, 225, 0, 0), "off: the held key is released");
    check(recv(keyboard, told, sizeof told, MSG_DONTWAIT) == 0, "off: the library hung up");
    send(keyboard, "Tzz", 3, MSG_NOSIGNAL);
    check(idle_pump() && idle_pump() && pushed_count == 2, "off: nothing is typed");
    close(keyboard);
    switch_file("off", false);

    // 8. The library comes back by itself. With Ctrl held and typing still queued, the keyboard goes
    // away: the key is released and the rest is dropped.
    keyboard = accept_keyboard(server);
    check(keyboard >= 0, "library reconnected after off");
    pushed_count = 0;
    key(keyboard, 224, 0x40, true);
    check(idle_pump() && pushed_count == 1 && is_key(0, 224, 0x40, 1), "Ctrl held again");
    send(keyboard, "Tzz", 3, MSG_NOSIGNAL);
    close(keyboard);
    check(idle_pump() && idle_pump(), "pumps after the keyboard closed");
    check(pushed_count == 2 && is_key(1, 224, 0, 0),
          "the held key is released and the queued text is dropped");

    close(server);
    for (const char** name = (const char*[]){"framekeyboard-stream.sock", "framekeyboard-stream.so",
                                             "off", "unicode-keys", NULL};
         *name; ++name) {
        unlink(in_directory(*name));
    }
    rmdir(directory);
    printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
