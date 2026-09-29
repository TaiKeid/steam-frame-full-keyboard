#include "framekeyboard/app.hpp"
#include "openvr.h"
#include "vk_texture.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace framekeyboard {
namespace {
void check(vr::EVROverlayError error, const char* operation) {
    if (error != vr::VROverlayError_None) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
    }
}
Transform from_vr(const vr::HmdMatrix34_t& pose) {
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = pose.m[r][c];
        }
    }
    return result;
}
struct ControllerSample {
    Transform pose;
    std::uint64_t buttons;
};
std::optional<ControllerSample> read_controller(vr::TrackedDeviceIndex_t device) {
    if (device >= vr::k_unMaxTrackedDeviceCount ||
        vr::VRSystem()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
        return {};
    }
    vr::VRControllerState_t state{};
    vr::TrackedDevicePose_t pose{};
    if (!vr::VRSystem()->GetControllerStateWithPose(vr::TrackingUniverseStanding, device, &state,
                                                    sizeof(state), &pose) ||
        !pose.bPoseIsValid || !pose.bDeviceIsConnected) {
        return {};
    }
    return ControllerSample{from_vr(pose.mDeviceToAbsoluteTracking), state.ulButtonPressed};
}
class LaserDrag {
  public:
    bool start(const vr::VREvent_t& event, const PanelPlacement& placement, double now) {
        const auto trigger = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
        auto device = event.trackedDeviceIndex;
        auto sample = read_controller(device);
        if (!sample || !(sample->buttons & trigger)) {
            // cursorIndex is primary/secondary laser, NOT a tracked-device ID.
            // Some runtimes omit the source device: accept only one held trigger
            // rather than guessing which hand owns the click.
            sample.reset();
            for (vr::TrackedDeviceIndex_t candidate = 1; candidate < vr::k_unMaxTrackedDeviceCount;
                 ++candidate) {
                auto current = read_controller(candidate);
                if (current && (current->buttons & trigger)) {
                    if (sample) {
                        return false;
                    }
                    sample = current;
                    device = candidate;
                }
            }
        }
        if (!sample) {
            return false;
        }
        device_ = device;
        pointer_ = event.data.mouse.cursorIndex;
        started_ = now;
        transform_.begin(sample->pose, placement.transform());
        active_ = true;
        return true;
    }
    bool update(PanelPlacement& placement, double now) {
        const auto sample = read_controller(device_);
        if (!sample || !(sample->buttons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) ||
            now - started_ > 30) {
            stop();
            return false;
        }
        placement.set_transform(transform_.update(sample->pose));
        return true;
    }
    bool active() const { return active_; }
    unsigned pointer() const { return pointer_; }
    void stop() { active_ = false; }

  private:
    PanelDrag transform_;
    vr::TrackedDeviceIndex_t device_{vr::k_unTrackedDeviceIndexInvalid};
    unsigned pointer_{};
    double started_{};
    bool active_{};
};
class VrPanel {
  public:
    ~VrPanel() {
        if (!connected_) {
            return;
        }
        if (handle_) {
            vr::VROverlay()->HideOverlay(handle_);
            vr::VROverlay()->ClearOverlayTexture(handle_);
            vr::VROverlay()->DestroyOverlay(handle_);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        // The compositor can retain submitted images. End the VR connection before
        // destroying Vulkan resources, including on partially completed startup.
        vr::VR_Shutdown();
        texture_.destroy();
        vulkan_.destroy();
    }
    void connect() {
        vr::EVRInitError error = vr::VRInitError_None;
        // A manual keyboard launch must not start SteamVR behind the user's back.
        vr::VR_Init(&error, vr::VRApplication_Background);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        vr::VR_Shutdown();
        vr::VR_Init(&error, vr::VRApplication_Overlay);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        connected_ = true;
        if (!vr::VRSystem() || !vr::VRCompositor() || !vr::VROverlay()) {
            throw std::runtime_error("required OpenVR interfaces are unavailable");
        }
        auto* overlay = vr::VROverlay();
        check(overlay->CreateOverlay("org.framekeyboard.panel", "FrameKeyboard", &handle_),
              "CreateOverlay");
        check(overlay->SetOverlayWidthInMeters(handle_, 1.15f), "SetOverlayWidthInMeters");
        check(overlay->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_Mouse),
              "SetOverlayInputMethod");
        vr::HmdVector2_t scale{{panel_width, panel_height}};
        check(overlay->SetOverlayMouseScale(handle_, &scale), "SetOverlayMouseScale");
        for (const auto flag :
             {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
              vr::VROverlayFlags_MultiCursor}) {
            check(overlay->SetOverlayFlag(handle_, flag, true), "SetOverlayFlag");
        }
        std::string message;
        if (!vulkan_.init(message) || !texture_.create(vulkan_, panel_width, panel_height, message)) {
            throw std::runtime_error(message);
        }
    }
    bool read_head(Transform& head) {
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bPoseIsValid) {
            return false;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                head[row][col] = pose.mDeviceToAbsoluteTracking.m[row][col];
            }
        }
        return true;
    }
    void place(const PanelPlacement& placement) {
        const auto world = placement.transform();
        vr::HmdMatrix34_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                transform.m[row][col] = static_cast<float>(world[row][col]);
            }
        }
        check(vr::VROverlay()->SetOverlayTransformAbsolute(handle_, vr::TrackingUniverseStanding,
                                                           &transform),
              "Place keyboard");
        check(vr::VROverlay()->SetOverlayWidthInMeters(handle_, static_cast<float>(placement.width())),
              "Resize keyboard");
    }
    void submit(PanelRenderer& renderer) {
        const auto rgba = renderer.rgba();
        std::string error;
        if (!texture_.update(handle_, rgba.data(), error)) {
            throw std::runtime_error(error);
        }
    }
    vr::VROverlayHandle_t handle() const { return handle_; }

  private:
    bool connected_{};
    vr::VROverlayHandle_t handle_{};
    VulkanContext vulkan_;
    OverlayTexture texture_;
};
} // namespace
int run_vr(App& app, VrInstance& instance, double duration) {
    VrPanel panel;
    panel.connect();
    struct ReleaseBeforeVrShutdown {
        App& app;
        ~ReleaseBeforeVrShutdown() {
            try {
                app.cancel();
            } catch (...) {
            }
        }
    } release{app};
    const double start = monotonic_seconds();
    bool done = false, visible = false;
    bool recenter_pending = true, transform_dirty = false, show_pending = false;
    bool waiting_for_tracking = false;
    PanelPlacement placement;
    LaserDrag drag;
    auto stop_drag = [&] {
        if (drag.active()) {
            drag.stop();
            app.set_dragging(false);
        }
    };
    std::vector<PlacementAction> adjustments;
    double next_tracking_check = 0;
    while (!done && !app.quitting() && !interrupted) {
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        if (instance.poll_recenter()) {
            stop_drag();
            std::cout << "Launch request received.\n" << std::flush;
            app.summon();
            show_pending = true;
        }
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                done = true;
            }
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                stop_drag();
                app.cancel();
                recenter_pending = true;
            }
            if (event.eventType == vr::VREvent_InputFocusChanged) {
                stop_drag();
                app.cancel();
            }
        }
        while (vr::VROverlay()->PollNextOverlayEvent(panel.handle(), &event, sizeof(event))) {
            // OpenVR reports bottom-left coordinates; Cairo uses top-left.
            const double x = event.data.mouse.x, y = panel_height - event.data.mouse.y;
            const auto pointer = event.data.mouse.cursorIndex;
            switch (event.eventType) {
            case vr::VREvent_MouseMove:
                if (!drag.active()) {
                    app.move(pointer, x, y);
                }
                break;
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    if (drag.active()) {
                        break;
                    }
                    if (app.drag_handle_contains(x, y) && placement.ready()) {
                        app.cancel();
                        if (drag.start(event, placement, now)) {
                            app.set_dragging(true);
                        } else {
                            app.report_status(
                                "Point one controller at the handle and hold its trigger.");
                        }
                    } else {
                        app.down(pointer, x, y, now);
                    }
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    if (drag.active()) {
                        if (pointer == drag.pointer()) {
                            stop_drag();
                        }
                    } else {
                        app.up(pointer, x, y);
                    }
                }
                break;
            case vr::VREvent_FocusLeave:
                // Keep capture when the laser briefly leaves the moving panel.
                // Physical trigger polling still ends the drag outside its bounds.
                if (!drag.active()) {
                    app.cancel();
                }
                break;
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
                stop_drag();
                app.cancel();
                break;
            case vr::VREvent_OverlayClosed:
                done = true;
                break;
            default:
                break;
            }
        }
        if (done || app.quitting()) {
            break;
        }
        if (app.take_recenter()) {
            stop_drag();
            recenter_pending = true;
            adjustments.clear();
            show_pending = true;
            next_tracking_check = 0;
        }
        const auto actions = app.take_placement_actions();
        adjustments.insert(adjustments.end(), actions.begin(), actions.end());
        if (!actions.empty()) {
            next_tracking_check = 0;
        }
        if (drag.active()) {
            if (drag.update(placement, now)) {
                panel.place(placement);
            } else {
                app.set_dragging(false);
            }
        }
        if (now >= next_tracking_check) {
            Transform head{};
            if (!panel.read_head(head)) {
                if (recenter_pending && !waiting_for_tracking) {
                    std::cout << "Waiting for valid headset pose before recentering.\n" << std::flush;
                }
                waiting_for_tracking = true;
                if (visible) {
                    stop_drag();
                    app.cancel();
                    check(vr::VROverlay()->HideOverlay(panel.handle()), "Hide keyboard");
                    visible = false;
                    // A wake or tracking-origin change can invalidate old world placement.
                    recenter_pending = true;
                }
            } else {
                waiting_for_tracking = false;
                if (recenter_pending) {
                    placement.recenter(head);
                    recenter_pending = false;
                    transform_dirty = true;
                    std::cout << "Keyboard recentered at current headset heading.\n" << std::flush;
                }
                for (const auto action : adjustments) {
                    if (action == PlacementAction::FaceMe) {
                        placement.face(head);
                    } else {
                        placement.adjust(action);
                    }
                    transform_dirty = true;
                }
                adjustments.clear();
                if (transform_dirty) {
                    panel.place(placement);
                    transform_dirty = false;
                }
                // Read actual compositor visibility rather than trusting our cached flag.
                // Another UI can hide an overlay without destroying its owner process.
                const bool compositor_visible = vr::VROverlay()->IsOverlayVisible(panel.handle());
                if (placement.ready() && (show_pending || !visible || !compositor_visible)) {
                    app.paint(now);
                    panel.submit(app.renderer);
                    check(vr::VROverlay()->ShowOverlay(panel.handle()), "Show keyboard");
                    visible = true;
                    show_pending = false;
                    std::cout << "FrameKeyboard overlay visible.\n" << std::flush;
                }
            }
            next_tracking_check = now + .1;
        }
        if (visible && app.tick(now)) {
            app.paint(now);
            panel.submit(app.renderer);
        } else {
            app.tick(now);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds((drag.active() || app.renderer.animating()) ? 16 : 20));
    }
    app.cancel();
    if (!placement.ready()) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
