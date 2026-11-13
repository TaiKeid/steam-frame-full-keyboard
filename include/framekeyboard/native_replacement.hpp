#pragma once
#include "input.hpp"
#include "localization.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sys/types.h>
#include <utility>

namespace framekeyboard {
// Version 1: one app-scoped helper, private SOCK_SEQPACKET IPC, metadata-only status.
// Frame IPC is nonblocking; hidden idle sleep can wake on session metadata.
// CDP waits stay in the helper. Steam restores its own fields; an
// app-scoped OpenVR guard restores exact Gamescope overlays independently.
class NativeReplacement : public KeySink {
  public:
    // An owned IPC endpoint can be supplied by isolated receiver tests.
    using Connect = std::function<int()>;
    explicit NativeReplacement(KeySink& local, int transport = -1, Connect connect = {})
        : local_(local), connect_(std::move(connect)), fd_(transport) {}
    ~NativeReplacement() override;
    struct State {
        std::uint64_t request{};
        bool open{}, eligible{}, active{}, available{}, local{};
        // The retained Steam field holds line breaks, so Shift+Enter is a paragraph break.
        bool multiline{};
        // Steam's own keyboard serves a request the bridge left with it, such
        // as a field in an embedded web page.
        bool steam{};
        // A game streamed with Steam Link holds the request. Its text is typed by the streaming
        // client through the helper's private channel, never by Steam or the local input backend.
        bool stream{};
    };
    void poll(double now);
    // Between hidden frames, wake the idle sleep when session metadata changes.
    void wait_until(std::chrono::steady_clock::time_point deadline);
    // `shown`: the panel is on screen. A click on a streamed game moves typing to the game only then.
    void update(bool enabled, bool ready, std::uint64_t acknowledgement, bool fallback, double now,
                bool watch_main_loop = false, bool standby = false, bool shown = false);
    const State& state() const { return state_; }
    // Why replacement is unavailable, as a key App shows; empty while it works.
    UiText unavailable_reason() const;
    void route(bool remote) { remote_ = remote; }
    // Whether Streaming support is on: the helper keeps the streaming client's
    // socket only while it is (R176). Sent with every frame.
    void set_streaming_support(bool on) { streaming_support_ = on; }
    bool single_line_target() const override { return remote_ && state_.active && !state_.multiline; }
    bool steam_field_target() const override { return remote_ && state_.active; }
    // A Steam field is no window of Gamescope's.
    std::uint32_t focused_window() const override { return remote_ ? 0 : local_.focused_window(); }
    void set_character_gap(double seconds) override { local_.set_character_gap(seconds); }
    void set_desktop_paste(Clipboard* clipboard) override { local_.set_desktop_paste(clipboard); }
    bool desktop_focused() const override { return local_.desktop_focused(); }
    void set_stream_paste(Clipboard* clipboard) override { local_.set_stream_paste(clipboard); }
    bool stream_focused() const override { return !remote_ && local_.stream_focused(); }
    // A Steam field is never a terminal, whatever app Gamescope has focused.
    EditKeys edit_keys() const override { return remote_ ? EditKeys::Standard : local_.edit_keys(); }
    bool edit_keys_known() const override { return remote_ || local_.edit_keys_known(); }
    void set_terminal_hint(bool terminal) override { local_.set_terminal_hint(terminal); }
    bool remote() const { return remote_; }
    void dismiss_request();
    // A prepared update hands the open request back to Steam, as Quit does,
    // so the successor's first helper can adopt it (adopt_update_handoff).
    void shutdown_for_update();
    // Helpers started after an in-app update adopt a native keyboard that the
    // outgoing version's cleanup left open, until one becomes available or
    // `until` (monotonic seconds) passes: a first start that fails, such as an
    // install timeout under startup load, leaves the adoption to its retry.
    // Steam reports no session identity, so the retry adopts the keyboard open
    // then. Restarts after the first available helper never adopt.
    void adopt_update_handoff(double until) { adopt_until_ = until; }
    void retry_now(double now);
    bool take_unsupported_control();
    // The native keyboard button pressed since the last call: it closed our
    // open request (toggle), or opened a new request without a field (summon).
    // Each is the request number the bridge reported with it, 0 for none, so
    // a press superseded by a newer request in the same reply can be told
    // apart. Keep open closes its held panel on either.
    struct ButtonPress {
        std::uint64_t toggle{}, summon{};
    };
    ButtonPress take_button_press();
    // Prepare before the VR watchdog starts; descriptors remain stable until shutdown.
    bool prepare_watchdog();
    void watchdog_tick(double now, double slow_work_deadline = 0) const;
    // Main-thread scopes identify external runtime waits. Nested calls share a
    // fixed frame deadline; returning grants only a short completion margin.
    void begin_runtime_wait(double now);
    void end_runtime_wait(double now);
    void frame_completed();
    double runtime_wait_deadline() const { return runtime_until_.load(); }
    void send(int code, int value) override;
    Delivery deliver_key(int code, int value) override;
    void cancel_delivery() override;
    bool pump() override { return local_.pump(); }
    bool take_input_reset() override;
    bool can_resume() const override { return local_.can_resume(); }
    bool text_available() override { return local_.text_available(); }
    bool commit_text(const std::string& text) override;
    // Steam fields order Backspace through acknowledged helper keys instead.
    bool replaces_text() override { return !remote_ && local_.replaces_text(); }
    bool replace_text(std::size_t delete_before, const std::string& text) override {
        return !remote_ && !pending_id_ && local_.replace_text(delete_before, text);
    }
    bool text_pending() const override { return pending_id_ != 0 || local_.text_pending(); }
    // Steam fields receive each text as one helper request.
    std::size_t delivered_characters() const override {
        return remote_ ? 0 : local_.delivered_characters();
    }
    bool character_in_flight() const override {
        return remote_ ? text_pending() : local_.character_in_flight();
    }
    void cancel_text() override;
    bool output_blocked() const override { return !remote_ && local_.output_blocked(); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return local_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return local_.num_lock_state(); }

  private:
    enum class Failure {
        None,
        DebuggerUnavailable,
        DebuggerBlocked,
        SteamIncompatible,
        InstallTimeout,
        ReplyTimeout,
        ReplacementBusy,
        HelperUnavailable,
        ConnectionLost
    };
    bool packet(const std::string& data);
    bool start(double now);
    const char* failure_code() const;
    void diagnostic(const char* code, double now, std::uint64_t request = 0, int retry = 0);
    void schedule_retry(double now);
    void stop(bool restore_native = true);
    KeySink& local_;
    Connect connect_;
    int fd_{-1};
    pid_t child_{-1};
    int watchdog_read_{-1}, watchdog_write_{-1};
    std::atomic<double> runtime_until_{};
    double runtime_frame_deadline_{};
    unsigned runtime_depth_{};
    State state_;
    Failure failure_{Failure::None};
    bool remote_{}, reset_{}, force_fallback_{}, unsupported_control_{}, streaming_support_{};
    ButtonPress button_;
    double last_reply_{}, install_until_{}, next_start_{}, now_{}, pending_since_{}, adopt_until_{};
    // How long the pending text may wait for its result: a second, or for a text into a stream,
    // which the helper reports only once the streaming client has typed it (R164): its 2 s to start
    // and 1 s and a tenth of a second per byte to finish, both counted from this send (R257), and
    // half a second more.
    double pending_limit_{1.0};
    double retry_delay_{5};
    double log_window_{-1}, last_failure_log_{-60}, logged_retry_{};
    unsigned log_lines_{};
    Failure logged_failure_{Failure::None};
    std::uint64_t event_sequence_{};
    bool enabled_{};
    std::uint64_t progress_{}, sequence_{}, pending_id_{};
    std::string pending_text_;
    std::optional<bool> pending_result_;
    std::uint64_t native_id_{};
    int native_code_{}, native_value_{};
    double native_since_{};
    std::optional<bool> native_result_;
    struct RemoteKey {
        std::uint64_t id, request;
        bool pending;
        std::string text; // Named control sent at press; repeats send the same one.
    };
    // Shift in a multiline Steam field is held only to make Enter a paragraph break.
    bool paragraph_shift(int code) const;
    std::map<int, RemoteKey> remote_keys_;
    std::set<int> modifiers_, local_keys_;
    // Keys held down in a streamed game, by Linux key code, with the request they were pressed for.
    // The helper presses them on the PC through the streaming client and lets go of them itself
    // when that session ends.
    std::map<int, std::uint64_t> stream_keys_;
    bool stream_key(std::uint64_t request, int code, int value);
};

// Keep panel dismissal separate from process lifetime. All visibility decisions use the
// same request snapshot, so closing X cannot acknowledge or reopen the old request.
class ReplacementPresentation {
  public:
    explicit ReplacementPresentation(bool start_hidden = false) : startup_hidden_(start_hidden) {}
    void update(bool enabled, NativeReplacement::State state) {
        if (enabled != enabled_) {
            managed_ = dismissed_ = false;
            bypass_request_ = dismissed_request_ = 0;
        }
        enabled_ = enabled;
        state_ = state;
        if (!state.available) {
            bypass_request_ = dismissed_request_ = 0;
        }
        if (enabled && state.available && state.open) {
            managed_ = true;
            startup_hidden_ = false;
        }
    }
    void dismiss() {
        managed_ = dismissed_ = true;
        dismissed_request_ = state_.request;
    }
    void summon() {
        startup_hidden_ = false;
        dismissed_ = false;
        if (!state_.open) {
            managed_ = false;
        }
    }
    void fallback() {
        bypass_request_ = state_.request;
        managed_ = true;
    }
    // Dictation keeps the panel open after Steam's field closes, so its text
    // can be sent to another field (docs/dictation.md, typing target lost);
    // so does Keep open (docs/configuration.md).
    void hold(bool on) { held_ = on; }
    // Shown only because dictation or Keep open holds it: there is no Steam
    // field. Dictation's text must not reach the focused application; Keep
    // open types there (VrLifecycle::field_needed).
    bool held_without_field() const { return holding() && enabled_ && managed_ && !requested(); }
    bool show() const {
        return !startup_hidden_ && (!enabled_ || (!managed_ && !dismissed_) || requested() || holding());
    }
    bool interactive() const {
        return !startup_hidden_ && (!enabled_ || (!managed_ && !dismissed_) ||
                                    (show() && state_.active) || held_without_field());
    }
    bool remote() const { return enabled_ && managed_ && show() && state_.active && !state_.local; }
    bool fallback_requested() const { return state_.open && state_.request == bypass_request_; }

  private:
    // The stock keyboard always wins over dictation's and Keep open's holds:
    // after our fallback, or while it serves a field we cannot type into.
    bool holding() const { return held_ && !fallback_requested() && !state_.steam; }
    bool requested() const {
        return state_.available && state_.open && state_.eligible && state_.request != bypass_request_ &&
               state_.request != dismissed_request_;
    }
    NativeReplacement::State state_;
    bool enabled_{}, managed_{}, dismissed_{};
    bool startup_hidden_{}, held_{};
    std::uint64_t bypass_request_{}, dismissed_request_{};
};
} // namespace framekeyboard
