#pragma once
#include "input.hpp"
#include <cstdint>
#include <sys/types.h>

namespace framekeyboard {
// Version 1: one app-scoped helper, private SOCK_SEQPACKET IPC, metadata-only status.
// No IPC or CDP wait runs on the VR thread. Steam restores its own fields; an
// app-scoped OpenVR guard restores exact Gamescope overlays independently.
class NativeReplacement : public KeySink {
  public:
    // An owned IPC endpoint can be supplied by isolated receiver tests.
    explicit NativeReplacement(KeySink& local, int transport = -1) : local_(local), fd_(transport) {}
    ~NativeReplacement() override;
    struct State {
        std::uint64_t request{};
        bool open{}, eligible{}, active{}, available{}, local{};
    };
    void poll(double now);
    void update(bool enabled, bool ready, std::uint64_t acknowledgement, bool fallback, double now,
                bool watch_panel = false);
    const State& state() const { return state_; }
    void route(bool remote) { remote_ = remote; }
    bool remote() const { return remote_; }
    void dismiss_request();
    bool take_unsupported_control();
    void send(int code, int value) override;
    bool pump() override { return local_.pump(); }
    bool take_input_reset() override;
    bool can_resume() const override { return local_.can_resume(); }
    bool text_available() override { return local_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool text_pending() const override { return pending_id_ != 0; }
    void cancel_text() override;
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return local_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return local_.num_lock_state(); }

  private:
    bool packet(const std::string& data);
    void start(double now);
    void stop();
    KeySink& local_;
    int fd_{-1};
    pid_t child_{-1};
    State state_;
    bool remote_{}, reset_{}, force_fallback_{}, unsupported_control_{};
    double last_reply_{}, next_start_{}, now_{}, pending_since_{};
    std::uint64_t progress_{}, sequence_{}, pending_id_{};
    std::string pending_text_;
    std::optional<bool> pending_result_;
    struct RemoteKey {
        std::uint64_t id, request;
        bool pending;
    };
    std::map<int, RemoteKey> remote_keys_;
    std::set<int> modifiers_, local_keys_;
};

// Keep panel dismissal separate from process lifetime. All visibility decisions use the
// same request snapshot, so closing X cannot acknowledge or reopen the old request.
class ReplacementPresentation {
  public:
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
        }
    }
    void dismiss() {
        managed_ = dismissed_ = true;
        dismissed_request_ = state_.request;
    }
    void summon() {
        dismissed_ = false;
        if (!state_.open) {
            managed_ = false;
        }
    }
    void fallback() {
        bypass_request_ = state_.request;
        managed_ = true;
    }
    bool show() const {
        return !enabled_ || (!managed_ && !dismissed_) ||
               (state_.available && state_.open && state_.eligible &&
                state_.request != bypass_request_ && state_.request != dismissed_request_);
    }
    bool interactive() const {
        return !enabled_ || (!managed_ && !dismissed_) || (show() && state_.active);
    }
    bool remote() const { return enabled_ && managed_ && show() && state_.active && !state_.local; }
    bool fallback_requested() const { return state_.open && state_.request == bypass_request_; }

  private:
    NativeReplacement::State state_;
    bool enabled_{}, managed_{}, dismissed_{};
    std::uint64_t bypass_request_{}, dismissed_request_{};
};
} // namespace framekeyboard
