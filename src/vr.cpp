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
    bool place() {
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bPoseIsValid) {
            return false;
        }
        auto transform = pose.mDeviceToAbsoluteTracking;
        // Snapshot the HMD pose, then place the panel below and in front of it.
        // Keeping this world-fixed lets the user look between keyboard and browser.
        for (int row = 0; row < 3; ++row) {
            transform.m[row][3] += transform.m[row][1] * -.25f + transform.m[row][2] * -.85f;
        }
        check(vr::VROverlay()->SetOverlayTransformAbsolute(handle_, vr::TrackingUniverseStanding,
                                                           &transform),
              "Place keyboard");
        return true;
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
int run_vr(App& app, double duration) {
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
    bool done = false, placed = false, visible = false;
    double next_tracking_check = 0;
    while (!done && !app.quitting() && !interrupted) {
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                done = true;
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
        if (app.take_recenter()) {
            placed = false;
        }
        if (now >= next_tracking_check) {
            vr::TrackedDevicePose_t pose{};
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
            if (!pose.bPoseIsValid) {
                if (visible) {
                    app.cancel();
                    check(vr::VROverlay()->HideOverlay(panel.handle()), "Hide keyboard");
                    visible = false;
                }
            } else {
                if (!placed) {
                    placed = panel.place();
                }
                if (placed && !visible) {
                    app.paint(now);
                    panel.submit(app.renderer);
                    check(vr::VROverlay()->ShowOverlay(panel.handle()), "Show keyboard");
                    visible = true;
                    std::cout << "FrameKeyboard overlay visible; input starts off.\n" << std::flush;
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
    if (!placed) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
