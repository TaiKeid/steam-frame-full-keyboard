#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/vr_haptics.hpp"
#include "openvr.h"
#include "vk_texture.h"

#include <algorithm>
#include <chrono>
#include <fstream>
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
std::optional<Transform> read_controller(vr::TrackedDeviceIndex_t device) {
    if (device >= vr::k_unMaxTrackedDeviceCount ||
        vr::VRSystem()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
        return {};
    }
    // Dashboard overlays receive laser events even when legacy controller input
    // is unavailable. Query tracking separately; a missing button state is not
    // evidence that the controller is untracked or its trigger was released.
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                    vr::k_unMaxTrackedDeviceCount);
    const auto& pose = poses[device];
    if (!pose.bPoseIsValid || !pose.bDeviceIsConnected) {
        return {};
    }
    return from_vr(pose.mDeviceToAbsoluteTracking);
}
struct GrabSample {
    bool active{}, held{}, pressed{};
    vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
    double stick_y{};
    std::array<double, 3> ray_direction{0, 0, -1};
};
class GrabInput {
  public:
    void reset() {
        for (auto& latch : latches_) {
            latch.reset();
        }
    }
    bool connect() {
        auto* input = vr::VRInput();
        if (!input || !vr::VRRenderModels()) {
            return false;
        }
        ready_ = input->GetInputSourceHandle("/user/hand/left", &hands_[0]) == vr::VRInputError_None &&
                 input->GetInputSourceHandle("/user/hand/right", &hands_[1]) == vr::VRInputError_None;
        return ready_;
    }
    std::array<GrabSample, 2> poll() {
        std::array<GrabSample, 2> result{};
        if (!ready_) {
            return result;
        }
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            const auto role =
                i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            const auto device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
            if (device != devices_[i]) {
                latches_[i].reset();
                devices_[i] = device;
                sticks_[i] = {};
            }
            char model[1024]{};
            vr::ETrackedPropertyError error{};
            vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String,
                                                           model, sizeof(model), &error);
            const std::string expected = i == 0 ? "{frame_controller}frame_controller_left"
                                                : "{frame_controller}frame_controller_right";
            if (error || model != expected || !vr::VRSystem()->IsTrackedDeviceConnected(device)) {
                latches_[i].reset();
                sticks_[i] = {};
                continue;
            }
            vr::RenderModel_ControllerMode_State_t mode{};
            vr::RenderModel_ComponentState_t current{}, neutral{};
            vr::VRControllerState_t released{};
            auto* models = vr::VRRenderModels();
            // Frame's dashboard masks grip actions and legacy controller state.
            // Its render component still animates the physical squeeze. Supplying
            // an all-released state gives a stable rest pose, even when launched
            // while the user already holds the grip. This is Frame-model specific.
            const bool valid =
                models->GetComponentStateForDevicePath(model, "button_grip", hands_[i], &mode,
                                                       &current) &&
                models->GetComponentState(model, "button_grip", &released, &mode, &neutral);
            if (!valid) {
                latches_[i].reset();
                continue;
            }
            const double angle =
                component_rotation_degrees(from_vr(neutral.mTrackingToComponentRenderModel),
                                           from_vr(current.mTrackingToComponentRenderModel));
            const auto grip = latches_[i].update(angle);
            result[i] = {true, grip.held, grip.pressed, device};
            if (grip.held) {
                auto& stick = sticks_[i];
                if (!stick.attempted) {
                    stick.attempted = true;
                    vr::RenderModel_ComponentState_t rest{}, up{}, tip{};
                    vr::VRControllerState_t full_up{};
                    full_up.rAxis[0].y = 1;
                    stick.ready =
                        models->GetComponentState(model, "thumbstick", &released, &mode, &rest) &&
                        models->GetComponentState(model, "thumbstick", &full_up, &mode, &up) &&
                        stick.axis.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                             from_vr(up.mTrackingToComponentRenderModel));
                    if (models->GetComponentState(model, "tip", &released, &mode, &tip)) {
                        for (std::size_t r = 0; r < 3; ++r) {
                            stick.ray_direction[r] = -tip.mTrackingToComponentLocal.m[r][2];
                        }
                    }
                }
                vr::RenderModel_ComponentState_t current_stick{};
                if (stick.ready && models->GetComponentStateForDevicePath(model, "thumbstick", hands_[i],
                                                                          &mode, &current_stick)) {
                    result[i].stick_y =
                        stick.axis.read(from_vr(current_stick.mTrackingToComponentRenderModel));
                }
                result[i].ray_direction = stick.ray_direction;
            }
        }
        return result;
    }

  private:
    struct Stick {
        ComponentAxis axis;
        std::array<double, 3> ray_direction{0, 0, -1};
        bool attempted{}, ready{};
    };
    std::array<Stick, 2> sticks_{};
    std::array<vr::VRInputValueHandle_t, 2> hands_{};
    std::array<GripLatch, 2> latches_{};
    bool ready_{};
    std::array<vr::TrackedDeviceIndex_t, 2> devices_{
        {vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid}};
};
class LaserDrag {
  public:
    bool start(const GrabSample& input, const PanelPlacement& placement, double now) {
        const auto pose = read_controller(input.device);
        if (!pose) {
            return false;
        }
        device_ = input.device;
        started_ = updated_ = now;
        transform_.begin(*pose, placement.transform(), input.ray_direction);
        active_ = true;
        return true;
    }
    bool update(PanelPlacement& placement, double now, double stick_y) {
        const auto pose = read_controller(device_);
        if (!pose || now - started_ > 30) {
            stop();
            return false;
        }
        transform_.move_depth(stick_y, now - updated_);
        updated_ = now;
        placement.set_transform(transform_.update(*pose));
        return true;
    }
    bool active() const { return active_; }
    vr::TrackedDeviceIndex_t device() const { return device_; }
    void stop() { active_ = false; }

  private:
    PanelDrag transform_;
    vr::TrackedDeviceIndex_t device_{vr::k_unTrackedDeviceIndexInvalid};
    double started_{}, updated_{};
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
        // Connect only once: a background connect/disconnect followed immediately
        // by an overlay connection can leave SteamVR using the old legacy input
        // context. On this Linux-only app, refuse launch if no VR server is running.
        bool server_running = false;
        for (const auto& entry : fs::directory_iterator("/proc")) {
            std::ifstream name(entry.path() / "comm");
            std::string process_name;
            if (std::getline(name, process_name) && process_name == "vrserver") {
                server_running = true;
                break;
            }
        }
        if (!server_running) {
            throw std::runtime_error("SteamVR must already be running");
        }
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
        check(overlay->SetOverlayWidthInMeters(handle_, static_cast<float>(PanelPlacement{}.width())),
              "SetOverlayWidthInMeters");
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
    std::uint64_t universe() const {
        vr::ETrackedPropertyError error{};
        const auto id = vr::VRSystem()->GetUint64TrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64, &error);
        return error == vr::TrackedProp_Success ? id : 0;
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
    VrHaptics haptics;
    auto manifest = fs::canonical("/proc/self/exe").parent_path().parent_path() /
                    "share/framekeyboard/vr/actions.json";
    if (!fs::is_regular_file(manifest)) {
        manifest = fs::path(FRAMEKEYBOARD_SOURCE_DIR) / "vr/actions.json";
    }
    haptics.connect(manifest);
    KeyHaptics feedback;
    GrabInput grip_input;
    if (!grip_input.connect()) {
        app.report_status("Native grip unavailable; use Recenter to recover the keyboard.");
    }
    std::array<bool, vr::k_unMaxTrackedDeviceCount> hovered_devices{};
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
    const auto placement_path = app.config_dir() / "placement.json";
    std::optional<SavedPlacement> saved;
    try {
        saved = load_placement(placement_path);
    } catch (const std::exception& error) {
        std::cerr << "Saved placement ignored: " << error.what() << '\n';
    }
    bool recenter_pending = !saved, transform_dirty = false, show_pending = false;
    std::uint64_t placement_universe = 0;
    bool waiting_for_tracking = false;
    PanelPlacement placement, displayed;
    HorizonAlignment horizon;
    LaserDrag drag;
    auto stop_drag = [&](bool finish_alignment = false) {
        feedback.cancel();
        if (drag.active()) {
            drag.stop();
            app.set_dragging(false);
        }
        if (!finish_alignment) {
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
        }
    };
    auto place_panel = [&](double now) {
        const auto aligned = horizon.update(placement.transform(), now);
        if (!displayed.ready() || aligned != displayed.transform() ||
            placement.width() != displayed.width() || transform_dirty) {
            displayed.restore(aligned, placement.width());
            panel.place(displayed);
            transform_dirty = false;
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
        // Hidden panels cannot be grabbed. Do not query both render models in
        // standby; require a released grip sample again when the panel returns.
        if (!visible) {
            grip_input.reset();
        }
        const auto grips = visible ? grip_input.poll() : std::array<GrabSample, 2>{};
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
                saved.reset();
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
            const auto device = event.trackedDeviceIndex;
            const bool controller =
                device < vr::k_unMaxTrackedDeviceCount &&
                vr::VRSystem()->GetTrackedDeviceClass(device) == vr::TrackedDeviceClass_Controller;
            switch (event.eventType) {
            case vr::VREvent_MouseMove: {
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] =
                        x >= 0 && x < panel_width && y >= 0 && y < panel_height;
                }
                if (!drag.active()) {
                    if (app.move(pointer, x, y) && controller) {
                        feedback.hover(device);
                    }
                }
                break;
            }
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left && !drag.active()) {
                    // Grabbing cannot also type if grip and trigger arrive together.
                    const bool grabbing = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                        return grip.active && grip.held && grip.device == event.trackedDeviceIndex;
                    });
                    if (!grabbing) {
                        const bool key_pressed = app.down(pointer, x, y, now);
                        if (key_pressed && controller) {
                            feedback.press(pointer, device);
                        }
                    }
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left && !drag.active()) {
                    if (app.up(pointer, x, y)) {
                        feedback.release(pointer);
                    }
                }
                break;
            case vr::VREvent_FocusLeave:
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] = false;
                } else {
                    hovered_devices.fill(false);
                }
                if (!drag.active()) {
                    feedback.cancel();
                    // Laser leave is not a change of the application's input target.
                    app.cancel(false);
                }
                break;
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
                hovered_devices.fill(false);
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
        if (drag.active()) {
            const bool held = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                return grip.active && grip.held && grip.device == drag.device();
            });
            if (!held) {
                stop_drag(true);
            }
        } else if (visible && placement.ready()) {
            for (const auto& grip : grips) {
                if (grip.active && grip.pressed && grip.device < hovered_devices.size() &&
                    hovered_devices[grip.device] && drag.start(grip, displayed, now)) {
                    // Capture the visible pose, not the uncorrected pose behind
                    // horizon assistance, so regrabbing never snaps the keyboard.
                    placement.restore(displayed.transform(), displayed.width());
                    horizon.reset();
                    feedback.cancel();
                    app.set_dragging(true);
                    break;
                }
            }
        }
        for (const auto& [device, kind] : feedback.take(now)) {
            if (!drag.active() && visible) {
                haptics.send(device, kind);
            }
        }
        if (app.take_recenter()) {
            stop_drag();
            saved.reset();
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
            double stick_y = 0;
            for (const auto& input : grips) {
                if (input.active && input.held && input.device == drag.device()) {
                    stick_y = input.stick_y;
                }
            }
            if (drag.update(placement, now, stick_y)) {
                transform_dirty = true;
            } else {
                stop_drag();
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
                    // A brief tracking loss hides the panel without losing its position.
                    // Actual tracking-origin changes request recentering via the event above.
                }
            } else {
                waiting_for_tracking = false;
                if (!adjustments.empty()) {
                    stop_drag();
                }
                if (saved) {
                    const auto universe = panel.universe();
                    if (saved->universe && universe && saved->universe != universe) {
                        recenter_pending = true;
                        std::cout << "Tracking space changed; recentering keyboard.\n";
                    } else {
                        placement.restore(saved->transform, saved->width);
                        placement_universe = universe;
                        transform_dirty = true;
                        std::cout << "Restored saved keyboard placement.\n" << std::flush;
                    }
                    saved.reset();
                }
                if (recenter_pending) {
                    horizon.reset();
                    placement.recenter(head);
                    placement_universe = panel.universe();
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
                    place_panel(now);
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
            next_tracking_check = now + (visible ? .1 : .25);
        }
        if (visible && placement.ready() && (transform_dirty || horizon.animating())) {
            place_panel(now);
        }
        const bool repaint = app.tick(now);
        if (visible && repaint) {
            app.paint(now);
            panel.submit(app.renderer);
        }
        const bool pointing = std::any_of(hovered_devices.begin(), hovered_devices.end(),
                                          [](bool hovered) { return hovered; });
        const bool interactive =
            pointing || drag.active() || horizon.animating() || app.renderer.animating();
        // Redrawing is still event-driven. These intervals only govern input
        // polling; SteamVR continues presenting the existing texture itself.
        const int wait_ms = !visible ? 250 : (interactive ? 16 : 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
    app.cancel();
    if (displayed.ready()) {
        try {
            save_placement(placement_path,
                           {displayed.transform(), displayed.width(), placement_universe});
        } catch (const std::exception& error) {
            std::cerr << "Could not save keyboard placement: " << error.what() << '\n';
        }
    }
    if (!placement.ready()) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
