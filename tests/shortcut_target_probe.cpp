// Opt-in live probe of the shortcut bar's key delivery (docs/shortcut-bar-design.md,
// Actions). It sends through the production route, ShortcutRunner over
// OutputController over EiSink, as a bar button's press does, ticked at the
// keyboard's 60 Hz frame. It sends only while its own newly created
// disposable window is Gamescope's focused window (the root's
// GAMESCOPE_FOCUSED_WINDOW), which decides where keys from the EI socket go,
// and has X keyboard focus; it checks both before every frame that may send,
// stops at the first key its window did not get, and looks only at its own
// window's key events. It prints counts and key names only.
//
// The route is the keyboard's own: Steam's Gamescope EI socket,
// /run/user/<uid>/gamescope-0-ei, unless --ei-socket names another, with the
// text socket that pairs with it. The receiver window opens on $DISPLAY,
// which must be Gamescope's own X display: its root carries
// GAMESCOPE_FOCUSED_WINDOW. Inside SteamOS's Desktop the probe's display is
// KWin's, without that property, and KWin's active window can't be confirmed
// from an X client, so it refuses to run there (R365).
#include "framekeyboard/ei_input.hpp"
#include "framekeyboard/input.hpp"
#include "framekeyboard/output.hpp"
#include "framekeyboard/shortcut_runner.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace framekeyboard;
using Clock = std::chrono::steady_clock;

namespace {
void usage() {
    std::cerr << "Usage: shortcut-target-probe (--gaps | --numpad) [--repeats N]\n"
                 "         [--tick-ms MS] [--ei-socket PATH]\n";
}
double seconds_now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// One key event at our window: its level-0 key name, as for the gaps' letters,
// and the name the window's keymap gives it in the event's state.
struct Event {
    bool press{};
    int code{}; // the Linux key code: the X key code less 8
    std::string base, name;
    unsigned state{};
};

class Receiver {
  public:
    Receiver() {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            throw std::runtime_error("no X display");
        }
        int revert{};
        XGetInputFocus(display_, &previous_, &revert);
        revert_ = revert;
        window_ =
            XCreateSimpleWindow(display_, DefaultRootWindow(display_), 0, 0, 500, 180, 0, 0, 0x252525);
        XStoreName(display_, window_, "FrameKeyboard shortcut probe");
        XSelectInput(display_, window_, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
        XMapRaised(display_, window_);
        XSync(display_, False);
        num_lock_ = XInternAtom(display_, "Num Lock", False);
        gamescope_focus_ = XInternAtom(display_, "GAMESCOPE_FOCUSED_WINDOW", False);
    }
    ~Receiver() {
        // Focus goes back only if this window still holds it.
        if (focused() && previous_ != None) {
            XSetInputFocus(display_, previous_, revert_, CurrentTime);
        }
        XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
    }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void take_focus() {
        XSetInputFocus(display_, window_, RevertToPointerRoot, CurrentTime);
        XSync(display_, False);
    }
    bool focused() {
        Window focus{};
        int ignored{};
        XGetInputFocus(display_, &focus, &ignored);
        return focus == window_;
    }
    // Whether this display is Gamescope's: its root carries Gamescope's focus.
    bool on_gamescope() { return gamescope_window().has_value(); }
    // Whether Gamescope's focused window, which keys from its EI socket
    // reach, is ours.
    bool gamescope_focused() { return gamescope_window() == window_; }
    // Every key event at our window since the last call; others are dropped
    // unread.
    std::vector<Event> take() {
        std::vector<Event> events;
        while (XPending(display_)) {
            XEvent event{};
            XNextEvent(display_, &event);
            if (event.xany.window != window_ || (event.type != KeyPress && event.type != KeyRelease)) {
                continue;
            }
            const auto keycode = static_cast<KeyCode>(event.xkey.keycode);
            Event result;
            result.press = event.type == KeyPress;
            result.state = event.xkey.state;
            result.code = static_cast<int>(keycode) - 8;
            result.base = key_name(XkbKeycodeToKeysym(display_, keycode, 0, 0));
            KeySym symbol = NoSymbol;
            unsigned consumed{};
            XkbLookupKeySym(display_, keycode, event.xkey.state, &consumed, &symbol);
            result.name = key_name(symbol);
            events.push_back(std::move(result));
        }
        return events;
    }
    // The Num Lock indicator as the X server reports it, or -1.
    int num_lock() {
        Bool on = False;
        if (num_lock_ == None ||
            !XkbGetNamedIndicator(display_, num_lock_, nullptr, &on, nullptr, nullptr)) {
            return -1;
        }
        return on ? 1 : 0;
    }

  private:
    std::optional<Window> gamescope_window() {
        if (gamescope_focus_ == None) {
            return {};
        }
        Atom type{};
        int format{};
        unsigned long count{}, after{};
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display_, DefaultRootWindow(display_), gamescope_focus_, 0, 1, False,
                               XA_CARDINAL, &type, &format, &count, &after, &data) != Success ||
            type != XA_CARDINAL || format != 32 || count < 1 || !data) {
            if (data) {
                XFree(data);
            }
            return {};
        }
        const Window window = reinterpret_cast<unsigned long*>(data)[0];
        XFree(data);
        return window;
    }
    static std::string key_name(KeySym symbol) {
        const char* name = symbol == NoSymbol ? nullptr : XKeysymToString(symbol);
        return name ? name : "none";
    }
    Display* display_{};
    Window window_{}, previous_{};
    int revert_{};
    Atom num_lock_{}, gamescope_focus_{};
};

// The keyboard's own pieces: the bar's runner over the output queue over
// the EI route, ticked in the App's order.
struct Route {
    explicit Route(const std::string& socket)
        : sink(socket), output(sink), shortcuts(output, [] { return true; }), runner(shortcuts) {}
    EiSink sink;
    OutputController output;
    // Letters go through the target's keymap: Unicode mode, as on EI.
    QueuedShortcutOutput shortcuts;
    ShortcutRunner runner;
};

struct ProbeSettings {
    std::string socket;
    int repeats{50};
    double tick{1.0 / 60};
};

class Probe {
  public:
    explicit Probe(const ProbeSettings& settings) : settings_(settings), route_(settings.socket) {}

    // Connects the route and gives our window focus; throws if either fails.
    void ready() {
        // Gamescope publishes its focus once a window has it: our new window
        // may be the first.
        const auto shown = Clock::now() + std::chrono::seconds(2);
        while (!receiver_.on_gamescope() && Clock::now() < shown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!receiver_.on_gamescope()) {
            throw std::runtime_error(
                "$DISPLAY is not Gamescope's (no GAMESCOPE_FOCUSED_WINDOW), as inside "
                "SteamOS's Desktop; nothing sent");
        }
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!route_.output.pump()) {
            if (Clock::now() > deadline) {
                throw std::runtime_error("the EI route did not become ready");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        route_.output.set_enabled(true);
        receiver_.take_focus();
        const auto focused = Clock::now() + std::chrono::seconds(2);
        while (!(receiver_.focused() && receiver_.gamescope_focused()) && Clock::now() < focused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        receiver_.take();
        if (!receiver_.focused() || !receiver_.gamescope_focused()) {
            throw std::runtime_error("the probe window is not Gamescope's focused window; nothing sent");
        }
    }

    // Starts `action` and ticks until the run ends; then gathers what our
    // window received until `expected` key events or a quiet `wait`.
    std::vector<Event> send(const ShortcutAction& action, std::size_t expected, double wait = .3) {
        require_focus();
        if (!route_.runner.start(action, seconds_now())) {
            throw std::runtime_error("the runner refused the action; stopped");
        }
        std::vector<Event> events;
        const auto gather = [&] {
            auto more = receiver_.take();
            events.insert(events.end(), more.begin(), more.end());
        };
        while (route_.runner.running()) {
            frame();
            gather();
        }
        if (route_.runner.end() != ShortcutRunner::End::Finished) {
            std::cerr << "run ended " << static_cast<int>(route_.runner.end()) << '\n';
        }
        const auto deadline = Clock::now() + std::chrono::duration<double>(wait);
        while (events.size() < expected && Clock::now() < deadline) {
            frame();
            gather();
        }
        // A quiet stretch between runs, so one's late events aren't the next one's.
        for (int i = 0; i < 6; ++i) {
            frame();
            gather();
        }
        return events;
    }
    void set_gap(double seconds) { route_.runner.set_key_gap(seconds); }
    int num_lock() { return receiver_.num_lock(); }
    int code(const std::string& key) const { return key_code(key); }
    const ProbeSettings& settings() const { return settings_; }

  private:
    // One keyboard frame: input route, then the output queue, then the bar's run.
    void frame() {
        require_focus();
        const double now = seconds_now();
        route_.output.pump();
        route_.output.tick(now);
        route_.runner.tick(now);
        std::this_thread::sleep_for(std::chrono::duration<double>(settings_.tick));
    }
    void require_focus() {
        if (!receiver_.focused() || !receiver_.gamescope_focused()) {
            route_.runner.stop();
            route_.output.release_holds();
            throw std::runtime_error(
                "the probe window is no longer Gamescope's focused window; stopped");
        }
    }

    ProbeSettings settings_;
    Receiver receiver_; // before the route: it outlives the route's sends
    Route route_;
};

ShortcutAction keys(std::vector<ShortcutChord> chords) {
    ShortcutAction action;
    action.kind = ShortcutAction::Kind::Keys;
    action.chords = std::move(chords);
    return action;
}

// Ctrl+K then S, as a two-combination shortcut, at several gaps.
int gaps(Probe& probe) {
    const auto action = keys({{{"ControlLeft"}, "KeyK"}, {{}, "KeyS"}});
    // Level-0 names in order; k's press must carry Control, s's must not.
    const std::vector<std::pair<bool, std::string>> expected = {
        {true, "Control_L"}, {true, "k"}, {false, "k"}, {false, "Control_L"}, {true, "s"}, {false, "s"}};
    for (const int gap : {0, 10, 20, 30, 50}) {
        probe.set_gap(gap / 1000.0);
        int complete = 0, reordered = 0;
        for (int run = 0; run < probe.settings().repeats; ++run) {
            const auto events = probe.send(action, expected.size());
            std::vector<std::pair<bool, std::string>> got;
            for (const auto& event : events) {
                got.emplace_back(event.press, event.base);
            }
            const bool control_held = got == expected && (events[1].state & ControlMask) != 0 &&
                                      (events[4].state & ControlMask) == 0;
            auto sorted_got = got;
            auto sorted_expected = expected;
            std::sort(sorted_got.begin(), sorted_got.end());
            std::sort(sorted_expected.begin(), sorted_expected.end());
            if (control_held) {
                ++complete;
            } else if (sorted_got == sorted_expected) {
                ++reordered;
            }
            // A key our window did not get may have gone elsewhere: stop.
            if (sorted_got != sorted_expected) {
                std::printf("gap_ms=%d run=%d stopped: a key did not arrive\n", gap, run + 1);
                throw std::runtime_error("a key did not arrive at the probe window; stopped");
            }
        }
        std::printf("gap_ms=%d runs=%d complete=%d out_of_order=%d\n", gap, probe.settings().repeats,
                    complete, reordered);
        std::fflush(stdout);
    }
    return 0;
}

// Prints a single key's events: the sent code, then each press received,
// its name in the received state and its code.
void print_key(const char* sent, int code, const std::vector<Event>& events, int num_lock = -2) {
    std::printf("sent=%s code=%d", sent, code);
    bool any = false;
    for (const auto& event : events) {
        if (event.press) {
            std::printf(" received=%s received_code=%d", event.name.c_str(), event.code);
            any = true;
        }
    }
    if (!any) {
        std::printf(" received=nothing");
    }
    if (num_lock != -2) {
        std::printf(" num_lock=%s", num_lock < 0 ? "unknown" : num_lock ? "on" : "off");
    }
    std::printf("\n");
    std::fflush(stdout);
}

// Each numpad digit, then Del and Enter, with the X server's Num Lock.
int numpad(Probe& probe) {
    std::vector<std::string> names;
    for (int digit = 0; digit <= 9; ++digit) {
        names.push_back("Numpad" + std::to_string(digit));
    }
    names.push_back("NumpadDecimal");
    names.push_back("NumpadEnter");
    std::printf("num_lock_before=%s\n", probe.num_lock() < 0 ? "unknown"
                                        : probe.num_lock()   ? "on"
                                                             : "off");
    for (const auto& name : names) {
        const auto events = probe.send(keys({{{}, name}}), 2);
        print_key(name.c_str(), probe.code(name), events, probe.num_lock());
        if (std::none_of(events.begin(), events.end(), [](const Event& event) { return event.press; })) {
            throw std::runtime_error("a key did not arrive at the probe window; stopped");
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    ProbeSettings settings;
    settings.socket = "/run/user/" + std::to_string(getuid()) + "/gamescope-0-ei";
    std::string mode;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto value = [&] {
                if (i + 1 >= argc) {
                    throw std::runtime_error(argument + " needs a value");
                }
                return std::string(argv[++i]);
            };
            if (argument == "--gaps" || argument == "--numpad") {
                if (!mode.empty()) {
                    throw std::runtime_error("one mode at a time");
                }
                mode = argument;
            } else if (argument == "--repeats") {
                settings.repeats = std::stoi(value());
                if (settings.repeats < 1 || settings.repeats > 1000) {
                    throw std::runtime_error("--repeats takes 1 to 1000");
                }
            } else if (argument == "--tick-ms") {
                settings.tick = std::stod(value()) / 1000;
                if (settings.tick <= 0 || settings.tick > .25) {
                    throw std::runtime_error("--tick-ms takes more than 0 and up to 250");
                }
            } else if (argument == "--ei-socket") {
                settings.socket = value();
            } else {
                throw std::runtime_error("unknown argument");
            }
        }
        if (mode.empty()) {
            throw std::runtime_error("no mode");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        usage();
        return 2;
    }
    try {
        Probe probe(settings);
        probe.ready();
        std::printf("mode=%s tick_ms=%.3f\n", mode.c_str() + 2, settings.tick * 1000);
        if (mode == "--gaps") {
            return gaps(probe);
        }
        return numpad(probe);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
