#include "framekeyboard/app.hpp"
#include "openvr.h"
#include "vk_texture.h"

#include <chrono>
#include <iostream>
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
    std::vector<PlacementAction> adjustments;
    double next_tracking_check = 0;
    while (!done && !app.quitting() && !interrupted) {
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        if (instance.poll_recenter()) {
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
                app.cancel();
                recenter_pending = true;
            }
            if (event.eventType == vr::VREvent_InputFocusChanged) {
                app.cancel();
            }
        }
        while (vr::VROverlay()->PollNextOverlayEvent(panel.handle(), &event, sizeof(event))) {
            // OpenVR reports bottom-left coordinates; Cairo uses top-left.
            const double x = event.data.mouse.x, y = panel_height - event.data.mouse.y;
            const auto pointer = event.data.mouse.cursorIndex;
            switch (event.eventType) {
            case vr::VREvent_MouseMove:
                app.move(pointer, x, y);
                break;
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    app.down(pointer, x, y, now);
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    app.up(pointer, x, y);
                }
                break;
            case vr::VREvent_FocusLeave:
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
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
        if (now >= next_tracking_check) {
            Transform head{};
            if (!panel.read_head(head)) {
                if (recenter_pending && !waiting_for_tracking) {
                    std::cout << "Waiting for valid headset pose before recentering.\n" << std::flush;
                }
                waiting_for_tracking = true;
                if (visible) {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(app.renderer.animating() ? 16 : 20));
    }
    app.cancel();
    if (!placement.ready()) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
