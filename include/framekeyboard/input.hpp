#pragma once

#include "app_identity.hpp"
#include "config.hpp"
#include "terminal_keys.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

namespace framekeyboard {
class Clipboard;
class OutputController;
enum class Delivery { Waiting, Success, Failure };
int key_code(const std::string& name);
// The letter key a shortcut action presses with Ctrl.
const char* shortcut_key(const std::string& action);
// The logical key ID for an evdev code, such as "KeyA" or "Enter"; empty when unknown.
const std::string& key_name(int code);
bool is_modifier(int code);
// Whether a held key repeats into a stream session only at stream_key_on_the_way (R420): one that
// edits or moves, whose overrun can't be seen before it does harm. Keys that type a character, Enter
// and Tab keep key_repeat_gap. The numpad's keys count while Num Lock is off (`num_lock`), as they
// then act as the navigation keys.
bool stream_paced_key(int code, bool num_lock);

// How long one key-down of ours holds the way to a Windows PC in a stream session, the same key
// again included (R369): the stream library's kSameKeyOnTheWayMs, as the helper's
// STREAM_KEY_ON_THE_WAY. A held editing or navigation key repeats into a stream session no faster
// than that (R420, stream_paced_key), so its taps never pile up inside the stream, where nothing of
// ours can take them back.
inline constexpr double stream_key_on_the_way = .14;
// How often a held key repeats into any other target, and a key that types a character everywhere.
inline constexpr double key_repeat_gap = .04;
// The sink receives Linux evdev codes. No renderer or profile can bypass this boundary.
class KeySink {
  public:
    virtual ~KeySink() = default;
    virtual void send(int code, int value) = 0;
    // Repeating the same operation consumes its asynchronous result, not another key.
    // Releases always execute immediately, even with an outstanding down result.
    virtual Delivery deliver_key(int code, int value) {
        send(code, value);
        return Delivery::Success;
    }
    virtual void cancel_delivery() { cancel_text(); }
    virtual bool pump() { return true; }
    // A pause/resume can happen within one pump. The app must still discard old holds.
    virtual bool take_input_reset() { return false; }
    virtual bool can_resume() const { return false; }
    virtual bool text_available() { return false; }
    virtual bool commit_text(const std::string&) { return false; }
    // Delete characters before the cursor, then insert text, as one ordered
    // transport request. Only transports that order both actions offer it.
    virtual bool replaces_text() { return false; }
    virtual bool replace_text(std::size_t, const std::string&) { return false; }
    // Optional asynchronous transports retain one commit until the caller consumes its result.
    virtual bool text_pending() const { return false; }
    virtual void cancel_text() {}
    // Characters at the start of the last text commit that the target
    // acknowledged before it failed, so a retry can skip them. Zero when the
    // commit went out as one request, whose delivery is unknown as a whole.
    virtual std::size_t delivered_characters() const { return 0; }
    // Whether part of the text commit going out was sent and not yet
    // acknowledged, so it may have arrived; none is between the characters
    // of a commit sent one at a time. Asked before a cancel ends the commit.
    virtual bool character_in_flight() const { return text_pending(); }
    // An outstanding text request can still order native downs after the caller
    // cancels or consumes a timeout. Releases remain allowed through this barrier.
    virtual bool output_blocked() const { return false; }
    virtual int shortcut_code(xkb_keysym_t, int fallback) { return fallback; }
    // The target holds one line, so Shift+Enter cannot break it, such as a
    // Steam search field. Unknown targets take line breaks.
    virtual bool single_line_target() const { return false; }
    // The target is a Steam field reached through Steam itself, not the
    // compositor's focused application, so the clipboard cannot reach it.
    virtual bool steam_field_target() const { return false; }
    // Of those, a game or app streamed from a PC with Streaming support: the
    // stream takes real key presses with their modifiers, so combinations
    // reach it, though the clipboard still cannot (steam_field_target stays
    // true for it). False for one of Steam's own fields.
    virtual bool stream_session_target() const { return false; }
    // When the keys sent so far are estimated through to the target, in the
    // frame loop's seconds: a stream session's, whose keys reach the PC only a
    // while after they are handed on (R369). 0 where keys are through once
    // delivered.
    virtual double keys_through() const { return 0; }
    // Gamescope's focused window, which typed text reaches; 0 when unknown or
    // when text goes elsewhere, such as a Steam field or the shell. Never blocks.
    virtual std::uint32_t focused_window() const { return 0; }
    // Changes when the window with the keys may have changed where
    // focused_window() can't show it: each activation SteamOS's Desktop
    // reports inside its one Gamescope window. Beside the window, never in
    // place of it; 0 where nothing reports. Never blocks.
    virtual std::uint64_t focus_serial() const { return 0; }
    // The app with the keys, with focused_window() in one snapshot
    // (docs/shortcut-bar-design.md, Apps): no identity where it can't be
    // told, as for uinput, a Steam field or a shell's field. Never blocks.
    virtual FocusedApp focused_app() const { return {focused_window(), {}, {}}; }
    // The minimum gap between characters typed one at a time, for transports
    // that pace them (GamescopeText::set_character_gap).
    virtual void set_character_gap(double) {}
    // Where text a nested desktop cannot take from Gamescope goes: null types
    // it as Unicode entry, a clipboard pastes it (General, KDE Desktop Unicode).
    virtual void set_desktop_paste(Clipboard*) {}
    // A nested desktop has the keys: its applications paste from its own
    // clipboard (Clipboard::desktop_result), not Gamescope's.
    virtual bool desktop_focused() const { return false; }
    // A game-streaming client, such as Moonlight, has the keys: it sends them
    // to the streamed computer as physical keys, so text beyond them goes
    // through this clipboard and the client's own paste shortcut, which types
    // it there (docs/streamed-games.md). Null: such text is refused.
    virtual void set_stream_paste(Clipboard*) {}
    virtual bool stream_focused() const { return false; }
    // The focused application's own Cut/Copy/Paste keys, such as a known
    // terminal's (terminal_keys.hpp). Standard when unknown, or when output
    // goes somewhere that is no terminal, such as a Steam field. Never blocks.
    virtual EditKeys edit_keys() const { return EditKeys::Standard; }
    // Whether edit_keys() knows the focused app yet: inside SteamOS's
    // Desktop, not until its KWin reports the active app, at most a few
    // seconds. Typing suggestions wait, so a terminal's commands aren't
    // learned meanwhile. Never blocks.
    virtual bool edit_keys_known() const { return true; }
    // A shell says the field it activated is a terminal (its `terminal`
    // purpose hint). A sink that pastes by itself, as the Desktop's Unicode
    // paste does, applies it as App does to its own keys (combine_edit_keys).
    virtual void set_terminal_hint(bool) {}
    // Unknown until the receiving system reports its lock state.
    virtual std::optional<bool> num_lock_state() { return {}; }
};
class NullSink : public KeySink {
  public:
    void send(int, int) override {};
};
class UInputSink : public KeySink {
  public:
    UInputSink();
    ~UInputSink() override;
    UInputSink(const UInputSink&) = delete;
    UInputSink& operator=(const UInputSink&) = delete;
    void send(int code, int value) override;
    bool pump() override;
    std::optional<bool> num_lock_state() override { return num_lock_; }
    fs::path event_node() const;

  private:
    int fd_{-1};
    int led_fd_{-1};
    std::set<int> held_;
    std::optional<bool> num_lock_;
};

class LanguageMap {
  public:
    explicit LanguageMap(const Language& language);
    ~LanguageMap();
    LanguageMap(const LanguageMap&) = delete;
    LanguageMap& operator=(const LanguageMap&) = delete;
    std::string legend(const Key& key, const std::set<int>& modifiers, bool caps, bool num) const;
    xkb_keysym_t symbol(const Key& key, const std::set<int>& modifiers, bool caps, bool num) const;
    static bool printable(xkb_keysym_t symbol);
    // Whether the letter keys type a right-to-left script, so their legends
    // are laid out as in that script's text.
    bool right_to_left() const { return right_to_left_; }
    // Whether right Alt selects the third level (AltGr) in this keymap.
    bool altgr() const { return altgr_; }
    std::string compose(xkb_keysym_t symbol);
    bool composing() const;
    void cancel_compose();

  private:
    Language language_;
    xkb_context* context_{};
    xkb_keymap* keymap_{};
    xkb_compose_state* compose_{};
    bool right_to_left_{}, altgr_{};
};

// Hold-to-lock, in seconds from a sticky modifier's press: its ring stays
// hidden until `fade_start`, so a quick tap doesn't flash it (a slow one held
// past it shows part of the ring), and fades in until `fade_end`; the key
// locks at `lock` and the ring closes at `ring_end`. The lock comes a little
// before the ring closes, so a hand that lets go as the ring closes still locks.
struct LockTiming {
    double fade_start{}, fade_end{}, lock{}, ring_end{};
};
inline constexpr LockTiming lock_timing{.15, .25, .5, .6};

class KeyboardState {
  public:
    explicit KeyboardState(KeySink& sink) : sink_(sink) {}
    explicit KeyboardState(OutputController& output);
    ~KeyboardState();
    // `uses_modifiers` false leaves a latched Shift for the next key, as
    // Korean's 한/영 does: it switches the keys, typing nothing.
    // `shortcut_modifiers`: what a Cut, Copy or Paste key holds with its
    // letter (`native_code`), such as a terminal's Ctrl+Shift; Ctrl if unset.
    bool down(unsigned pointer, const Key& key, double now, bool local = false, int native_code = 0,
              bool uses_modifiers = true, std::optional<std::vector<int>> shortcut_modifiers = {});
    // A local press, still held, goes on as a native one already past its
    // repeat delay: `code` goes down now and repeats from the next step.
    bool continue_native(unsigned pointer, int code, double now);
    bool up(unsigned pointer);
    bool pointer_pressed(unsigned pointer) const { return presses_.contains(pointer); }
    // The pointer holds a key that repeats, or will once held long enough.
    bool repeats(unsigned pointer) const {
        const auto press = presses_.find(pointer);
        return press != presses_.end() && press->second.repeat_code != 0;
    }
    void cancel_pointer(unsigned pointer);
    // Both end every press, latch and lock. cancel_all() also abandons output
    // accepted but not yet delivered; release_all() lets it finish, for a
    // change of pointer or view that keeps the target.
    void cancel_all();
    void release_all();
    // Ends every press as cancel_pointer() does, and any other output hold,
    // keeping latches and locks: the keys come up, not the modifier state.
    void release_presses();
    // Before a shortcut button's combinations, which carry their own
    // modifiers: latched ones are cleared, not added, and held or locked
    // ones kept down let up, as Cut, Copy and Paste do; a lock stays locked
    // and goes down again with the next key that uses it. False, changing
    // nothing, while another pointer holds a key down.
    bool clear_for_shortcut();
    bool tick(double now);
    bool pressed(const std::string& id) const;
    std::set<int> modifiers() const;
    // Something modified by these modifiers ended a latch and keeps a held one
    // from latching on release; down() does this for keys.
    void use_modifiers();
    bool caps() const { return caps_; }
    bool num() const { return num_; }
    // A sticky modifier (Shift, Ctrl, Alt, AltGr or the left Super) held
    // until lock_timing.lock without another key locks: it applies to every
    // key, as a held one would, until a tap on any locked key unlocks them all.
    // A second tap within this long of the first locks it, as holding does.
    static constexpr double double_tap_seconds = .3;
    bool locked(int code) const { return locked_.contains(code); }
    const std::set<int>& locked_modifiers() const { return locked_; }
    // When the lock ring of this key started: while it is held toward a lock,
    // and after it locked until its ring has gone round. None otherwise.
    std::optional<double> lock_ring(const std::string& id) const;
    // Pointers whose hold or double tap locked a modifier since the last
    // call, for a haptic pulse in the hand that did it.
    std::vector<unsigned> take_new_locks() { return std::exchange(new_locks_, {}); }
    // Whether right Alt picks characters (AltGr) in the typing language: it
    // then goes down with each key, as Shift does, rather than staying down.
    void set_altgr(std::function<bool()> altgr) { altgr_ = std::move(altgr); }
    // Restore the local lock state without sending a lock key to the target.
    void set_num(bool enabled) { num_ = enabled; }
    bool active() const { return !presses_.empty(); }

  private:
    struct Press {
        std::string id;
        int modifier{}, local_lock{};
        bool used{};
        double at{};
        // A sticky modifier's tap may lock if held, or if it is the second of
        // a double tap; one pressed while any is locked unlocks all on release.
        bool lockable{}, locks{}, unlocks{}, double_tap{};
        std::vector<int> codes;
        int repeat_code{};
        double repeat_at{};
    };
    void acquire(int code);
    void release(int code);
    void end_all(bool cancel_delivery);
    // Ctrl, Alt and Super pick no characters: held or locked, they go down
    // with the first key that uses them and stay down until let go or
    // unlocked, as on a physical keyboard, so Alt held over several Tabs
    // keeps the window switcher open. Shift and AltGr go down with each key.
    bool stays_down(int code) const;
    // A modifier held by a sticky press, or locked: kept down once used.
    bool wanted_down(int code) const;
    void hold_down(int code);
    // Lets up every modifier kept down that is no longer held or locked.
    void settle_held_down();
    // Whether a Cut, Copy or Paste whose chord holds `chord` may go down now;
    // lets up kept-down modifiers it doesn't hold, so they don't join it.
    bool make_room_for_shortcut(const std::vector<int>& chord);
    // Records a lock for take_new_locks(), keeping only the newest few.
    void note_new_lock(unsigned pointer);
    KeySink& sink_;
    OutputController* output_{};
    std::map<unsigned, Press> presses_;
    std::map<int, unsigned> references_;
    std::set<int> latched_, locked_;
    // Modifiers kept down on the target (stays_down), each with one reference
    // of its own in references_ and, with an output queue, a direct hold there.
    std::set<int> held_down_;
    // Locks whose ring still runs after their key was let go, by key ID: when it started.
    std::map<std::string, double> rings_;
    std::vector<unsigned> new_locks_;
    // The last tap that latched a modifier, for a double tap, and the latest
    // time down() or tick() saw, for up(), which takes none.
    int last_tap_{};
    double last_tap_at_{}, now_{};
    std::function<bool()> altgr_;
    bool caps_{false}, num_{false};
};
} // namespace framekeyboard
