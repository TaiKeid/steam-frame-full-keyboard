#include "framekeyboard/vr_lifecycle.hpp"
#include <utility>

namespace framekeyboard {
VrLifecycle::VrLifecycle(bool background_start, VrLifecycleEffects effects)
    : effects_(std::move(effects)), presentation_(background_start) {
}

void VrLifecycle::begin_frame(const Frame& frame) {
    const auto& native = frame.native;
    native_ = native;
    // The native keyboard button closes a panel Keep open holds, as X does:
    // whether it ended our request or opened a new one without a field. The
    // shell closes it through X's own path (take_button_close), which also
    // ends a dictation editor that would otherwise keep holding the panel.
    const bool summoned = native.open && frame.button.summon && frame.button.summon == native.request;
    // A toggle counts only while its closed request is still the latest: a
    // field focused before this poll has opened a newer one, which takes over.
    const bool toggled = !native.open && frame.button.toggle && frame.button.toggle == native.request;
    const bool button_closes = keep_armed_ && visible_ && (toggled || summoned);
    if (button_closes && summoned) {
        // Steam counts the new request as open, and the bridge ends only a
        // request our panel acknowledged; a refused dismissal would reopen
        // Steam's keyboard. Keep holding the panel until then.
        close_when_acknowledged_ = native.request;
    } else if (button_closes) {
        button_close_ = true; // The press already ended our request.
    }
    if (!frame.keep_open || frame.watchdog_stalled) {
        keep_armed_ = false;
    } else if (visible_) {
        keep_armed_ = true;
    }
    presentation_.hold(frame.dictation_hold || keep_armed_);
    presentation_.update(frame.replacement_enabled, native);
    if (frame.watchdog_stalled) {
        presentation_.fallback();
    }
    if (frame.relaunch) {
        effects_.retry_bridge(frame.now);
        effects_.stop_drag();
        effects_.summon();
        show_pending_ = true;
        presentation_.summon();
    }
    if (close_when_acknowledged_ && (!native.open || native.request != close_when_acknowledged_)) {
        close_when_acknowledged_ = 0; // Ended or replaced before acknowledgement.
    } else if (close_when_acknowledged_ && native.active) {
        // Ending it now lets the next press open ours again.
        close_when_acknowledged_ = 0;
        button_close_ = true;
    }
    const bool remote = presentation_.remote();
    if (last_request_ != native.request || remote != frame.routed_remote) {
        effects_.stop_drag();
        // The sink still addresses the old owner until every held/queued input is cancelled.
        effects_.cancel_input();
        effects_.route_input(remote);
        last_request_ = native.request;
    }
    if (!requested()) {
        hide();
    } else if (visible_) {
        effects_.set_interaction(interactive());
    }
}
void VrLifecycle::hide() {
    effects_.set_interaction(false);
    effects_.stop_drag();
    effects_.clear_pointers();
    if (visible_) {
        effects_.hide_overlay();
        visible_ = false;
    }
}
void VrLifecycle::dashboard_visibility(bool visible) {
    if (!visible) {
        hide();
    }
}
void VrLifecycle::dismiss() {
    presentation_.hold(false); // X discarded dictation; never reshown this frame.
    keep_armed_ = false;
    close_when_acknowledged_ = 0;
    button_close_ = false;
    hide();
    presentation_.dismiss();
    effects_.dismiss_request();
    show_pending_ = false;
}
void VrLifecycle::fallback() {
    presentation_.hold(false);
    keep_armed_ = false;
    close_when_acknowledged_ = 0;
    button_close_ = false;
    presentation_.fallback();
    hide();
}
void VrLifecycle::focus_lost() {
    effects_.stop_drag();
    effects_.cancel_input();
}
void VrLifecycle::device_lost(std::uint32_t device, std::optional<std::uint32_t> drag_owner) {
    effects_.cancel_device(device);
    if (drag_owner == device) {
        effects_.stop_drag();
    }
}
void VrLifecycle::pointer_left(std::uint32_t device, bool dragging) {
    if (!dragging) {
        effects_.cancel_device(device);
    }
}
void VrLifecycle::request_recenter() {
    show_pending_ = true;
    tracking_.request_sample();
}
bool VrLifecycle::should_sample_tracking(bool dashboard_visible, double now) const {
    return tracking_.should_sample(dashboard_visible, requested(), visible_, now);
}
void VrLifecycle::tracking_sampled(bool valid, double now) {
    tracking_.sampled(valid, now);
    if (!valid) {
        hide();
    }
}
void VrLifecycle::present(bool placement_ready, bool dashboard_visible, bool compositor_visible,
                          double now) {
    if (requested() && !tracking_.waiting() && placement_ready && dashboard_visible &&
        (show_pending_ || !visible_ || !compositor_visible)) {
        effects_.show_overlay(now);
        visible_ = true;
        effects_.set_interaction(interactive());
        show_pending_ = false;
    }
}
bool VrLifecycle::ready(bool backend_ready, bool dashboard_visible) const {
    return backend_ready && (!dashboard_visible || !tracking_.waiting());
}
VrLifecycle::FrameReport VrLifecycle::frame_report(bool backend_ready, bool dashboard_visible,
                                                   bool compositor_visible) const {
    const bool available = ready(backend_ready, dashboard_visible);
    const bool presented = available && dashboard_visible && visible_ && compositor_visible;
    return {available, !hosted_ && presentation_.fallback_requested(),
            presented && requested() && !hosted_ ? native_.request : 0};
}
} // namespace framekeyboard
