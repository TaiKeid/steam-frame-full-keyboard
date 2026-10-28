#pragma once
#include "native_replacement.hpp"
#include "vr_runtime.hpp"
#include <functional>
#include <optional>

namespace framekeyboard {
// Runtime effects stay in the OpenVR shell. The coordinator owns their ordering;
// host receivers can exercise the same transitions without a headset or OS input.
struct VrLifecycleEffects {
    std::function<void()> stop_drag, cancel_input, clear_pointers, hide_overlay;
    std::function<void(bool)> route_input, set_interaction;
    std::function<void(double)> show_overlay, retry_bridge;
    std::function<void()> summon, dismiss_request;
    std::function<void(std::uint32_t)> cancel_device;
};

// A keyboard button under a window pinned in the world asks for a keyboard
// while the dashboard is closed; our panel can only appear there in
// passthrough. Turn it on once per such request, so turning it off again
// during that request sticks. Passthrough turned on this way stays on after
// the request ends, like the toolbar toggle.
class PassthroughRequests {
  public:
    bool take(bool replacement_enabled, const NativeReplacement::State& native, bool dashboard_visible) {
        if (!native.open) {
            // The bridge numbers requests from 1 again after Steam's UI reloads.
            handled_ = 0;
        }
        if (!replacement_enabled || !native.available || !native.open || !native.local ||
            native.request == handled_) {
            return false;
        }
        // A request seen in the dashboard is the dashboard's: closing the
        // dashboard while it is still open must not turn passthrough on.
        handled_ = native.request;
        return !dashboard_visible;
    }

    // Turning passthrough off with the dashboard closed hides our panel. Steam
    // still counts its request as open, and would treat the next keyboard
    // button press as closing it; end the request as X does instead.
    static bool ends_request(bool replacement_enabled, const NativeReplacement::State& native) {
        return replacement_enabled && native.available && native.open;
    }

  private:
    std::uint64_t handled_{};
};

class VrLifecycle {
  public:
    VrLifecycle(bool background_start, VrLifecycleEffects effects);
    struct Frame {
        bool replacement_enabled{};
        NativeReplacement::State native;
        bool routed_remote{}, watchdog_stalled{}, relaunch{};
        bool dictation_hold{};                 // App::dictation_holds_panel()
        bool keep_open{};                      // App::keep_open()
        NativeReplacement::ButtonPress button; // NativeReplacement::take_button_press()
        double now{};
    };
    void begin_frame(const Frame& frame);
    void dashboard_visibility(bool visible);
    void hide();
    void dismiss();
    void fallback();
    void focus_lost();
    void device_lost(std::uint32_t device, std::optional<std::uint32_t> drag_owner);
    void pointer_left(std::uint32_t device, bool dragging);
    void request_recenter();
    bool should_sample_tracking(bool dashboard_visible, double now) const;
    void tracking_sampled(bool valid, double now);
    bool waiting_for_tracking() const { return tracking_.waiting(); }
    bool panel_requested() const { return requested(); }
    // Hosted mode (docs/integration.md, Presentation): only the shell shows
    // the panel, and replacement never runs.
    void set_hosted(bool hosted) { hosted_ = hosted; }
    void set_shell_visible(bool visible) {
        if (visible && !shell_visible_) {
            show_pending_ = true;
        }
        shell_visible_ = visible;
    }
    bool held_without_field() const { return presentation_.held_without_field(); }
    // Dictation's hold sends nothing without a field. Keep open's sends to the
    // app with compositor keyboard focus, as without replacement: the user
    // chose to keep typing there (docs/configuration.md).
    bool field_needed() const { return held_without_field() && !keep_armed_; }
    bool visible() const { return visible_; }
    // Call after placement work; only successful runtime presentation consumes a pending show.
    void present(bool placement_ready, bool dashboard_visible, bool compositor_visible, double now);
    bool ready(bool backend_ready, bool dashboard_visible) const;
    struct FrameReport {
        bool ready{}, fallback{};
        std::uint64_t acknowledgement{};
    };
    FrameReport frame_report(bool backend_ready, bool dashboard_visible, bool compositor_visible) const;

  private:
    VrLifecycleEffects effects_;
    ReplacementPresentation presentation_;
    TrackingReadiness tracking_;
    NativeReplacement::State native_;
    bool requested() const { return hosted_ ? shell_visible_ : presentation_.show(); }
    bool interactive() const { return hosted_ ? shell_visible_ : presentation_.interactive(); }
    std::uint64_t last_request_{};
    bool visible_{}, show_pending_{};
    // Keep open holds the panel only once it has been shown, and X or a
    // native fallback lets go until a request or summon shows it again.
    bool keep_armed_{};
    // A button summon over the held panel, dismissed once acknowledged.
    std::uint64_t close_when_acknowledged_{};
    bool hosted_{}, shell_visible_{};
};
} // namespace framekeyboard
