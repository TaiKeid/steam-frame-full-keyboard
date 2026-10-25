#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/native_replacement.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/resizer.hpp"
#include "framekeyboard/updates.hpp"
#include "framekeyboard/vr_haptics.hpp"
#include "framekeyboard/vr_lifecycle.hpp"
#include "framekeyboard/vr_runtime.hpp"
#include "framekeyboard/vr_texture.hpp"
#include "openvr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace framekeyboard {
namespace {
// Only the VR/main thread installs this binding. The overlay watchdog never
// writes main-loop runtime state, even when its own OpenVR call blocks.
thread_local NativeReplacement* runtime_watch = nullptr;
class RuntimeBinding {
  public:
    explicit RuntimeBinding(NativeReplacement& watch) : previous_(runtime_watch) {
        runtime_watch = &watch;
    }
    ~RuntimeBinding() { runtime_watch = previous_; }

  private:
    NativeReplacement* previous_;
};
class RuntimeWait {
  public:
    RuntimeWait() : watch_(runtime_watch) {
        if (watch_) {
            watch_->begin_runtime_wait(monotonic_seconds());
        }
    }
    ~RuntimeWait() {
        if (watch_) {
            watch_->end_runtime_wait(monotonic_seconds());
        }
    }
    RuntimeWait(const RuntimeWait&) = delete;
    RuntimeWait& operator=(const RuntimeWait&) = delete;

  private:
    NativeReplacement* watch_;
};
// Start the scope before fetching the interface. It lasts through the complete
// API expression, and also covers methods using a retained local interface.
template <typename Interface> class RuntimeInterface {
  public:
    explicit RuntimeInterface(Interface* (*get)()) : value_(get()) {}
    Interface* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

  private:
    RuntimeWait wait_;
    Interface* value_;
};
auto vr_system() {
    return RuntimeInterface<vr::IVRSystem>(vr::VRSystem);
}
auto vr_overlay() {
    return RuntimeInterface<vr::IVROverlay>(vr::VROverlay);
}
auto vr_input() {
    return RuntimeInterface<vr::IVRInput>(vr::VRInput);
}
auto vr_models() {
    return RuntimeInterface<vr::IVRRenderModels>(vr::VRRenderModels);
}
auto vr_compositor() {
    return RuntimeInterface<vr::IVRCompositor>(vr::VRCompositor);
}
void check(vr::EVROverlayError error, const char* operation) {
    if (error != vr::VROverlayError_None) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 vr_overlay()->GetOverlayErrorNameFromEnum(error));
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
bool pointer_device(vr::TrackedDeviceIndex_t device) {
    if (device >= vr::k_unMaxTrackedDeviceCount) {
        return false;
    }
    const auto kind = vr_system()->GetTrackedDeviceClass(device);
    return kind == vr::TrackedDeviceClass_Controller || kind == vr::TrackedDeviceClass_HMD;
}
std::optional<Transform> read_pointer_pose(vr::TrackedDeviceIndex_t device) {
    if (!pointer_device(device)) {
        return {};
    }
    // Dashboard overlays receive laser events even when legacy controller input
    // is unavailable. Query tracking separately; a missing button state is not
    // evidence that the controller is untracked or its trigger was released.
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
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
        auto input = vr_input();
        if (!input || !vr_models()) {
            return false;
        }
        ready_ = input->GetInputSourceHandle("/user/hand/left", &hands_[0]) == vr::VRInputError_None &&
                 input->GetInputSourceHandle("/user/hand/right", &hands_[1]) == vr::VRInputError_None;
        return ready_;
    }
    std::array<GrabSample, 2> poll(std::optional<vr::TrackedDeviceIndex_t> laser_device = {}) {
        std::array<GrabSample, 2> result{};
        if (!ready_) {
            return result;
        }
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            const auto role =
                i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            const auto device = vr_system()->GetTrackedDeviceIndexForControllerRole(role);
            if (laser_device && device != *laser_device) {
                latches_[i].reset();
                continue;
            }
            if (device != devices_[i]) {
                latches_[i].reset();
                devices_[i] = device;
                sticks_[i] = {};
            }
            char model[1024]{};
            vr::ETrackedPropertyError error{};
            vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                        sizeof(model), &error);
            const std::string expected = i == 0 ? "{frame_controller}frame_controller_left"
                                                : "{frame_controller}frame_controller_right";
            if (error || model != expected || !vr_system()->IsTrackedDeviceConnected(device)) {
                latches_[i].reset();
                sticks_[i] = {};
                continue;
            }
            vr::RenderModel_ControllerMode_State_t mode{};
            vr::RenderModel_ComponentState_t current{}, neutral{};
            vr::VRControllerState_t released{};
            auto models = vr_models();
            result[i] = {true, false, false, device};
            if (laser_device) {
                // A trigger-held line capture needs only this hand's stick.
                // Grip availability/state must not gate laser depth control.
                latches_[i].reset();
            } else {
                // Frame's dashboard masks grip actions and legacy controller
                // state. Compare the live component with a synthetic rest pose.
                const bool valid =
                    models->GetComponentStateForDevicePath(model, "button_grip", hands_[i], &mode,
                                                           &current) &&
                    models->GetComponentState(model, "button_grip", &released, &mode, &neutral);
                if (!valid) {
                    latches_[i].reset();
                    result[i] = {};
                    continue;
                }
                const double angle =
                    component_rotation_degrees(from_vr(neutral.mTrackingToComponentRenderModel),
                                               from_vr(current.mTrackingToComponentRenderModel));
                const auto grip = latches_[i].update(angle);
                result[i].held = grip.held;
                result[i].pressed = grip.pressed;
            }
            if (laser_device || result[i].held) {
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
    GrabSample laser_sample(vr::TrackedDeviceIndex_t device) {
        GrabSample fallback;
        fallback.device = device;
        // AUX captures the HMD, which has no thumbstick. Never borrow another
        // device's stick, and retain pose-only dragging if sampling is unavailable.
        if (device >= vr::k_unMaxTrackedDeviceCount ||
            vr_system()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
            return fallback;
        }
        for (const auto& sample : poll(device)) {
            if (sample.active && sample.device == device) {
                return sample;
            }
        }
        return fallback;
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
    bool start(const GrabSample& input, const PanelPlacement& placement, double now, MoveSource source,
               unsigned pointer = 0) {
        const auto pose = read_pointer_pose(input.device);
        if (!pose || !capture_.begin(source, input.device, pointer)) {
            return false;
        }
        started_ = updated_ = now;
        transform_.begin(*pose, placement.transform(), input.ray_direction);
        trigger_release_.begin(now);
        trigger_neutral_.reset();
        if (source == MoveSource::Laser) {
            prepare_trigger();
        }
        return true;
    }
    // A corner resizer press: the panel scales with the holding laser's ray.
    bool start_resize(vr::TrackedDeviceIndex_t device, unsigned pointer, const PanelResize& resize,
                      double now) {
        if (!capture_.begin(MoveSource::Resize, device, pointer)) {
            return false;
        }
        resize_ = resize;
        started_ = updated_ = now;
        trigger_release_.begin(now);
        trigger_neutral_.reset();
        prepare_trigger();
        return true;
    }
    // A lost ray or the movement time limit ends the resize; a ray off the
    // panel plane keeps the last size.
    bool resize(PanelPlacement& placement, double now, const std::optional<Ray>& ray) {
        if (!ray || now - started_ > 30) {
            stop();
            return false;
        }
        if (const auto width = resize_.update(*ray)) {
            placement.set_width(*width);
        }
        return true;
    }
    bool update(PanelPlacement& placement, double now, double stick_y) {
        const auto pose = read_pointer_pose(device());
        if (!pose || now - started_ > 30) {
            stop();
            return false;
        }
        transform_.move_depth(stick_y, now - updated_);
        updated_ = now;
        placement.set_transform(transform_.update(*pose));
        return true;
    }
    bool active() const { return capture_.active(); }
    vr::TrackedDeviceIndex_t device() const { return capture_.device(); }
    std::optional<MoveSource> source() const { return capture_.source(); }
    bool owns_pointer(unsigned device, unsigned pointer) const {
        return capture_.owns_pointer(device, pointer);
    }
    // Whether a laser capture can end on the trigger's own release, without
    // a pointer event: only for a Frame controller with a calibrated trigger.
    bool watches_trigger() const { return source() != MoveSource::Grip && trigger_neutral_.has_value(); }
    bool trigger_released(double now) {
        if (source() == MoveSource::Grip || !trigger_neutral_) {
            return false;
        }
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t current{};
        if (!vr_models()->GetComponentStateForDevicePath(trigger_model_.c_str(), "trigger",
                                                         trigger_hand_, &mode, &current)) {
            return trigger_release_.update(std::nullopt, now);
        }
        const auto angle = component_rotation_degrees(*trigger_neutral_,
                                                      from_vr(current.mTrackingToComponentRenderModel));
        return trigger_release_.update(angle, now);
    }
    void stop() { capture_.reset(); }

  private:
    void prepare_trigger() {
        auto models = vr_models();
        auto input = vr_input();
        if (!models || !input) {
            return;
        }
        const auto role = vr_system()->GetControllerRoleForTrackedDeviceIndex(device());
        if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) {
            return;
        }
        const bool left = role == vr::TrackedControllerRole_LeftHand;
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(device(), vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        const std::string expected = left ? "{frame_controller}frame_controller_left"
                                          : "{frame_controller}frame_controller_right";
        if (error || model != expected ||
            input->GetInputSourceHandle(left ? "/user/hand/left" : "/user/hand/right", &trigger_hand_) !=
                vr::VRInputError_None) {
            return;
        }
        vr::VRControllerState_t released{};
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t neutral{};
        if (models->GetComponentState(model, "trigger", &released, &mode, &neutral)) {
            trigger_model_ = model;
            trigger_neutral_ = from_vr(neutral.mTrackingToComponentRenderModel);
        }
    }
    PanelDrag transform_;
    PanelResize resize_;
    MoveCapture capture_;
    double started_{}, updated_{};
    std::string trigger_model_;
    vr::VRInputValueHandle_t trigger_hand_{};
    std::optional<Transform> trigger_neutral_;
    TriggerReleaseGuard trigger_release_;
};
// Passthrough pointer. Outside the dashboard SteamVR sends overlay mouse events
// only to overlays that claim scene input, which takes input from the focused
// app. Instead read each Frame controller's pose and trigger component: neither
// read takes anything away, so the app receives the trigger too.
struct SceneSample {
    vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
    std::optional<PanelHit> hit;
    std::optional<Ray> ray;
    // lost: the hand or its trigger stopped reporting while held; cancel, don't release.
    bool pressed{}, released{}, lost{};
};
class ScenePointer {
  public:
    bool connect() {
        auto input = vr_input();
        ready_ =
            input && vr_models() &&
            input->GetInputSourceHandle("/user/hand/left", &hands_[0].source) == vr::VRInputError_None &&
            input->GetInputSourceHandle("/user/hand/right", &hands_[1].source) == vr::VRInputError_None;
        return ready_;
    }
    // Forget presses without reporting them; the caller has already cancelled input.
    void reset() {
        for (auto& hand : hands_) {
            hand.armed = hand.held = false;
        }
    }
    std::array<SceneSample, 2> poll(const PanelPlacement& panel, double height_over_width) {
        std::array<SceneSample, 2> result{};
        if (!ready_ || !panel.ready()) {
            return result;
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                     vr::k_unMaxTrackedDeviceCount);
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            auto& hand = hands_[i];
            auto& sample = result[i];
            const auto device = vr_system()->GetTrackedDeviceIndexForControllerRole(
                i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
            if (device != hand.device) {
                // A different controller never inherits the old one's press.
                sample.lost = hand.held;
                sample.device = hand.device;
                const auto source = hand.source;
                const bool warned = hand.warned;
                hand = {};
                hand.source = source;
                hand.device = device;
                hand.warned = warned;
                if (sample.lost) {
                    continue;
                }
            }
            sample.device = device;
            if (!hand.calibrated && device < vr::k_unMaxTrackedDeviceCount) {
                calibrate(hand, i == 0);
            }
            const auto angle = device < vr::k_unMaxTrackedDeviceCount && hand.travel > 0
                                   ? trigger_angle(hand)
                                   : std::nullopt;
            const auto& pose = device < vr::k_unMaxTrackedDeviceCount ? poses[device] : poses[0];
            if (!angle || device >= vr::k_unMaxTrackedDeviceCount || !pose.bPoseIsValid ||
                !pose.bDeviceIsConnected) {
                sample.lost = hand.held;
                hand.armed = hand.held = false;
                continue;
            }
            // Require a released trigger after startup, a new controller or a gap,
            // so a pull already in progress never clicks the key it lands on.
            if (!hand.armed) {
                hand.armed = *angle < hand.release;
            } else {
                const bool held = *angle >= (hand.held ? hand.release : hand.press);
                sample.pressed = held && !hand.held;
                sample.released = !held && hand.held;
                hand.held = held;
            }
            const auto controller = from_vr(pose.mDeviceToAbsoluteTracking);
            std::array<double, 3> origin{}, direction{};
            for (std::size_t r = 0; r < 3; ++r) {
                origin[r] = controller[r][3];
                for (std::size_t c = 0; c < 3; ++c) {
                    origin[r] += controller[r][c] * hand.tip[c][3];
                    direction[r] -= controller[r][c] * hand.tip[c][2];
                }
            }
            sample.ray = Ray{origin, direction};
            sample.hit =
                panel_ray_hit(panel.transform(), panel.width(), height_over_width, origin, direction);
        }
        return result;
    }
    // Pointer IDs for the two hands; SteamVR's own cursor indexes stay small.
    static unsigned pointer(std::size_t hand) { return 0x100 + static_cast<unsigned>(hand); }

  private:
    struct Hand {
        vr::VRInputValueHandle_t source{};
        vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
        std::string model;
        bool warned{}, calibrated{}, armed{}, held{};
        // The laser's origin and -Z direction in controller space.
        Transform tip{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        Transform neutral{};
        double travel{}, press{}, release{}; // Trigger rotation in degrees.
    };
    // Retried each frame until it succeeds: a controller can connect before its model is known.
    void calibrate(Hand& hand, bool left) {
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(hand.device, vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        const std::string expected = left ? "{frame_controller}frame_controller_left"
                                          : "{frame_controller}frame_controller_right";
        if (error || model != expected) {
            return;
        }
        auto models = vr_models();
        vr::VRControllerState_t released{}, pulled{};
        for (auto& axis : pulled.rAxis) {
            axis.x = 1;
        }
        pulled.ulButtonPressed = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t rest{}, full{}, tip{};
        if (!models->GetComponentState(model, "trigger", &released, &mode, &rest) ||
            !models->GetComponentState(model, "trigger", &pulled, &mode, &full)) {
            if (!std::exchange(hand.warned, true)) {
                std::cout << "Passthrough pointer: no trigger component on the "
                          << (left ? "left" : "right") << " controller.\n"
                          << std::flush;
            }
            return;
        }
        hand.calibrated = true;
        if (models->GetComponentState(model, "tip", &released, &mode, &tip)) {
            hand.tip = from_vr(tip.mTrackingToComponentLocal);
        }
        hand.model = model;
        hand.neutral = from_vr(rest.mTrackingToComponentRenderModel);
        hand.travel =
            component_rotation_degrees(hand.neutral, from_vr(full.mTrackingToComponentRenderModel));
        // Click near a full pull like SteamVR's laser; without a measured travel,
        // use the thresholds the line-capture release guard already relies on.
        hand.press = hand.travel >= 2 ? hand.travel * .7 : 1;
        hand.release = hand.travel >= 2 ? hand.travel * .4 : .5;
        if (hand.travel < 2) {
            hand.travel = 1;
        }
        std::cout << "Passthrough pointer ready on the " << (left ? "left" : "right") << " controller.\n"
                  << std::flush;
    }
    std::optional<double> trigger_angle(const Hand& hand) {
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t current{};
        if (!vr_models()->GetComponentStateForDevicePath(hand.model.c_str(), "trigger", hand.source,
                                                         &mode, &current)) {
            return {};
        }
        return component_rotation_degrees(hand.neutral,
                                          from_vr(current.mTrackingToComponentRenderModel));
    }
    std::array<Hand, 2> hands_{};
    bool ready_{};
};
// A dot where each passthrough ray meets the panel, since SteamVR draws no
// laser outside the dashboard. Each is its own small overlay, so moving it
// never repaints the keyboard texture.
class SceneCursors {
  public:
    ~SceneCursors() {
        for (auto& dot : dots_) {
            if (dot.handle) {
                vr_overlay()->DestroyOverlay(dot.handle);
            }
        }
    }
    void show(std::size_t hand, const Transform& panel, const PanelHit& hit) {
        auto& dot = dots_[hand];
        if (!dot.handle && !create(hand)) {
            return;
        }
        Transform world = panel;
        for (std::size_t r = 0; r < 3; ++r) {
            // Just in front of the face, so the keyboard never covers it.
            world[r][3] = hit.point[r] + panel[r][2] * .003;
        }
        if (world != dot.placed) {
            vr::HmdMatrix34_t transform{};
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 4; ++c) {
                    transform.m[r][c] = static_cast<float>(world[r][c]);
                }
            }
            vr_overlay()->SetOverlayTransformAbsolute(dot.handle, vr::TrackingUniverseStanding,
                                                      &transform);
            dot.placed = world;
        }
        if (!dot.visible) {
            vr_overlay()->ShowOverlay(dot.handle);
            dot.visible = true;
        }
    }
    void hide(std::size_t hand) {
        auto& dot = dots_[hand];
        if (dot.visible) {
            vr_overlay()->HideOverlay(dot.handle);
            dot.visible = false;
        }
    }
    void hide() {
        for (std::size_t hand = 0; hand < dots_.size(); ++hand) {
            hide(hand);
        }
    }

  private:
    struct Dot {
        vr::VROverlayHandle_t handle{};
        bool visible{}, failed{};
        Transform placed{};
    };
    bool create(std::size_t hand) {
        auto& dot = dots_[hand];
        if (dot.failed) {
            return false;
        }
        constexpr int size = 32;
        std::array<std::uint8_t, size * size * 4> pixels{};
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const double r = std::hypot(x + .5 - size / 2.0, y + .5 - size / 2.0);
                // White center, dark ring for contrast on light keys, soft edge.
                const double alpha = std::clamp(15.5 - r, 0.0, 1.0);
                const std::uint8_t shade = r < 11 ? 255 : 30;
                auto* p = &pixels[static_cast<std::size_t>((y * size + x) * 4)];
                p[0] = p[1] = p[2] = shade;
                p[3] = static_cast<std::uint8_t>(alpha * 255);
            }
        }
        auto overlay = vr_overlay();
        const std::string key = "org.framekeyboard.cursor." + std::to_string(hand);
        dot.failed = overlay->CreateOverlay(key.c_str(), "Full Keyboard pointer", &dot.handle) !=
                         vr::VROverlayError_None ||
                     overlay->SetOverlayRaw(dot.handle, pixels.data(), size, size, 4) !=
                         vr::VROverlayError_None ||
                     overlay->SetOverlayWidthInMeters(dot.handle, .012f) != vr::VROverlayError_None ||
                     overlay->SetOverlaySortOrder(dot.handle, 1) != vr::VROverlayError_None;
        if (dot.failed && dot.handle) {
            overlay->DestroyOverlay(dot.handle);
            dot.handle = 0;
        }
        return !dot.failed;
    }
    std::array<Dot, 2> dots_{};
};
// Each pointer's laser in the world: a controller's from its tip component
// along -Z, as SteamVR draws it, and the head cursor's straight ahead.
class LaserRays {
  public:
    std::optional<Ray> read(vr::TrackedDeviceIndex_t device) {
        const auto pose = read_pointer_pose(device);
        if (!pose) {
            return {};
        }
        const auto tip = vr_system()->GetTrackedDeviceClass(device) == vr::TrackedDeviceClass_Controller
                             ? controller_tip(device)
                             : identity;
        Ray ray{};
        ray.direction = {};
        for (std::size_t r = 0; r < 3; ++r) {
            ray.origin[r] = (*pose)[r][3];
            for (std::size_t c = 0; c < 3; ++c) {
                ray.origin[r] += (*pose)[r][c] * tip[c][3];
                ray.direction[r] -= (*pose)[r][c] * tip[c][2];
            }
        }
        return ray;
    }

  private:
    static constexpr Transform identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    // Cached per device index and render model; a reconnect may change either.
    Transform controller_tip(vr::TrackedDeviceIndex_t device) {
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        auto& tip = tips_[device];
        if (error || tip.model != model) {
            tip = {error ? "" : model, identity};
            vr::VRControllerState_t released{};
            vr::RenderModel_ControllerMode_State_t mode{};
            vr::RenderModel_ComponentState_t state{};
            if (!error && vr_models() &&
                vr_models()->GetComponentState(model, "tip", &released, &mode, &state)) {
                tip.local = from_vr(state.mTrackingToComponentLocal);
            }
        }
        return tip.local;
    }
    struct Tip {
        std::string model;
        Transform local{};
    };
    std::map<vr::TrackedDeviceIndex_t, Tip> tips_;
};
ResizeCorner corner_of(std::size_t side) {
    return side == 0 ? ResizeCorner::Left : ResizeCorner::Right;
}
// A case corner as an offset from the overlay center. The texture adds
// popup_margin above and below the case, so its center is the case's middle row.
PanelOffset case_corner(const PanelView& view, double width, bool right, bool bottom) {
    const auto body = panel_case_bounds(view);
    const double meters = width / panel_width;
    return {((right ? body.x + body.width : body.x) - panel_width / 2.0) * meters,
            (texture_height / 2.0 - popup_margin - (bottom ? body.y + body.height : body.y)) * meters};
}
// The bracket follows the case's corner radius, which scales with the panel.
// Quantized to 0.25 mm so a resize redraws the image only now and then.
ResizerShape resizer_shape(const PanelView& view, double width) {
    const double radius = view.theme->surface_radius * width / panel_width;
    return {std::round(radius * 4000) / 4000};
}
// The two corner resizers, index 0 left and 1 right. Each is its own overlay,
// so fading never repaints the keyboard texture. Both are created up front:
// the watchdog thread hides them by handle.
class CornerResizers {
  public:
    CornerResizers() {
        for (std::size_t side = 0; side < corners_.size(); ++side) {
            create(side);
        }
    }
    ~CornerResizers() {
        for (auto& corner : corners_) {
            if (corner.handle) {
                vr_overlay()->DestroyOverlay(corner.handle);
            }
        }
    }
    CornerResizers(const CornerResizers&) = delete;
    CornerResizers& operator=(const CornerResizers&) = delete;
    std::array<vr::VROverlayHandle_t, 2> handles() const {
        return {corners_[0].handle, corners_[1].handle};
    }
    bool poll(std::size_t side, vr::VREvent_t& event) {
        const auto handle = corners_[side].handle;
        return handle && vr_overlay()->PollNextOverlayEvent(handle, &event, sizeof(event));
    }
    // As for the keyboard, passthrough takes no SteamVR laser input.
    void set_scene_input(bool scene) {
        for (const auto& corner : corners_) {
            if (corner.handle) {
                vr_overlay()->SetOverlayInputMethod(corner.handle, scene
                                                                       ? vr::VROverlayInputMethod_None
                                                                       : vr::VROverlayInputMethod_Mouse);
            }
        }
    }
    // Eases toward `target` opacity. A faded-out resizer is hidden, so it never
    // takes the laser; its image is redrawn only when shown at a new radius.
    // A held resizer stays fully opaque and keeps its image: replacing a raw
    // overlay image every few frames of a resize makes it flicker. It is
    // redrawn for the final radius once released.
    void update(std::size_t side, const Transform& world, const ResizerShape& shape, double target,
                double now, bool held = false) {
        auto& corner = corners_[side];
        const double alpha = held ? corner.fade.hold(now) : corner.fade.update(target, now);
        if (!corner.handle) {
            return;
        }
        if (alpha < .01) {
            hide(corner);
            return;
        }
        if (corner.shape != shape && !(held && corner.shape) && !draw(side, shape)) {
            hide(corner);
            return;
        }
        auto overlay = vr_overlay();
        if (world != corner.placed) {
            vr::HmdMatrix34_t transform{};
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 4; ++c) {
                    transform.m[r][c] = static_cast<float>(world[r][c]);
                }
            }
            overlay->SetOverlayTransformAbsolute(corner.handle, vr::TrackingUniverseStanding,
                                                 &transform);
            corner.placed = world;
        }
        if (std::abs(alpha - corner.shown_alpha) > .004 || (alpha == 1 && corner.shown_alpha != 1)) {
            overlay->SetOverlayAlpha(corner.handle, static_cast<float>(alpha));
            corner.shown_alpha = alpha;
        }
        if (!corner.visible) {
            overlay->ShowOverlay(corner.handle);
            corner.visible = true;
        }
    }
    double alpha(std::size_t side) const { return corners_[side].fade.value(); }
    void hide() {
        for (auto& corner : corners_) {
            corner.fade.reset();
            hide(corner);
        }
    }

  private:
    struct Corner {
        vr::VROverlayHandle_t handle{};
        bool visible{};
        double shown_alpha{-1};
        Transform placed{};
        // The radius the current image was drawn for; none before the first draw.
        std::optional<ResizerShape> shape;
        ResizerFade fade;
    };
    void hide(Corner& corner) {
        if (corner.visible) {
            vr_overlay()->HideOverlay(corner.handle);
            corner.visible = false;
        }
    }
    void create(std::size_t side) {
        auto& corner = corners_[side];
        auto overlay = vr_overlay();
        const std::string key = "org.framekeyboard.resizer." + std::to_string(side);
        if (overlay->CreateOverlay(key.c_str(), "Full Keyboard resizer", &corner.handle) !=
            vr::VROverlayError_None) {
            corner.handle = 0;
            std::cout << "Corner resizer unavailable.\n" << std::flush;
            return;
        }
        bool ok = overlay->SetOverlaySortOrder(corner.handle, 1) == vr::VROverlayError_None &&
                  overlay->SetOverlayInputMethod(corner.handle, vr::VROverlayInputMethod_Mouse) ==
                      vr::VROverlayError_None;
        for (const auto flag : {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MultiCursor}) {
            ok = ok && overlay->SetOverlayFlag(corner.handle, flag, true) == vr::VROverlayError_None;
        }
        if (!ok) {
            overlay->DestroyOverlay(corner.handle);
            corner.handle = 0;
            std::cout << "Corner resizer unavailable.\n" << std::flush;
        }
    }
    // Image, size, mouse scale and laser mask all depend on the radius.
    bool draw(std::size_t side, const ResizerShape& shape) {
        auto& corner = corners_[side];
        const int size = shape.pixels();
        auto pixels = resizer_pixels(corner_of(side), shape);
        vr::HmdVector2_t scale{{static_cast<float>(size), static_cast<float>(size)}};
        std::array<vr::VROverlayIntersectionMaskPrimitive_t, 2> mask{};
        const auto rectangles = resizer_mask(corner_of(side), shape);
        for (std::size_t i = 0; i < mask.size(); ++i) {
            mask[i].m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Rectangle;
            const auto& r = rectangles[i];
            mask[i].m_Primitive.m_Rectangle = {r[0], r[1], r[2], r[3]};
        }
        auto overlay = vr_overlay();
        const bool ok =
            overlay->SetOverlayRaw(corner.handle, pixels.data(), static_cast<std::uint32_t>(size),
                                   static_cast<std::uint32_t>(size), 4) == vr::VROverlayError_None &&
            overlay->SetOverlayWidthInMeters(corner.handle, static_cast<float>(2 * shape.half_size())) ==
                vr::VROverlayError_None &&
            overlay->SetOverlayMouseScale(corner.handle, &scale) == vr::VROverlayError_None &&
            overlay->SetOverlayIntersectionMask(corner.handle, mask.data(),
                                                static_cast<std::uint32_t>(mask.size())) ==
                vr::VROverlayError_None;
        corner.shape = ok ? std::optional{shape} : std::nullopt;
        return ok;
    }
    std::array<Corner, 2> corners_{};
};
class VrPanel {
  public:
    ~VrPanel() {
        if (!connected_) {
            return;
        }
        if (handle_) {
            vr_overlay()->HideOverlay(handle_);
            vr_overlay()->ClearOverlayTexture(handle_);
            vr_overlay()->DestroyOverlay(handle_);
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
        require_steamvr();
        vr::VR_Init(&error, vr::VRApplication_Overlay);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        connected_ = true;
        if (!vr_system() || !vr_compositor() || !vr_overlay()) {
            throw std::runtime_error("required OpenVR interfaces are unavailable");
        }
        auto overlay = vr_overlay();
        check(overlay->CreateOverlay("org.framekeyboard.panel", "Full Keyboard", &handle_),
              "CreateOverlay");
        check(overlay->SetOverlayWidthInMeters(handle_, static_cast<float>(PanelPlacement{}.width())),
              "SetOverlayWidthInMeters");
        check(overlay->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_Mouse),
              "SetOverlayInputMethod");
        vr::HmdVector2_t scale{{panel_width, texture_height}};
        check(overlay->SetOverlayMouseScale(handle_, &scale), "SetOverlayMouseScale");
        for (const auto flag : {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MultiCursor,
                                vr::VROverlayFlags_SendVRDiscreteScrollEvents}) {
            check(overlay->SetOverlayFlag(handle_, flag, true), "SetOverlayFlag");
        }
        std::string message;
        if (!vulkan_.init(message) || !texture_.create(vulkan_, handle_, message)) {
            throw std::runtime_error(message);
        }
    }
    bool read_head(Transform& head) {
        vr::TrackedDevicePose_t pose{};
        vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
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
    std::optional<Transform> read_dashboard(const char* key, bool bottom_center) {
        auto overlay = vr_overlay();
        vr::VROverlayHandle_t dashboard{};
        if (!overlay->IsDashboardVisible() ||
            overlay->FindOverlay(key, &dashboard) != vr::VROverlayError_None ||
            !overlay->IsOverlayVisible(dashboard)) {
            return {};
        }
        vr::HmdVector2_t scale{};
        if (overlay->GetOverlayMouseScale(dashboard, &scale) != vr::VROverlayError_None ||
            !std::isfinite(scale.v[0]) || !std::isfinite(scale.v[1]) || scale.v[0] <= 0 ||
            scale.v[1] <= 0) {
            return {};
        }
        // Invisible tabs return a valid but frozen transform, so reject them
        // above. Coordinates use mouse pixels and include overlay scale.
        vr::HmdVector2_t bottom{{scale.v[0] / 2, bottom_center ? 0 : scale.v[1] / 2}};
        vr::HmdMatrix34_t world{};
        if (overlay->GetTransformForOverlayCoordinates(dashboard, vr::TrackingUniverseStanding, bottom,
                                                       &world) != vr::VROverlayError_None) {
            return {};
        }
        return dashboard_anchor(from_vr(world));
    }
    void place(const PanelPlacement& placement) {
        const auto world = placement.transform();
        vr::HmdMatrix34_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                transform.m[row][col] = static_cast<float>(world[row][col]);
            }
        }
        check(
            vr_overlay()->SetOverlayTransformAbsolute(handle_, vr::TrackingUniverseStanding, &transform),
            "Place keyboard");
        check(vr_overlay()->SetOverlayWidthInMeters(handle_, static_cast<float>(placement.width())),
              "Resize keyboard");
    }
    std::uint64_t universe() const {
        vr::ETrackedPropertyError error{};
        const auto id = vr_system()->GetUint64TrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64, &error);
        return error == vr::TrackedProp_Success ? id : 0;
    }
    void submit(PanelRenderer& renderer, const PanelView& view) {
        RuntimeWait runtime_wait;
        // The enlarged texture has transparent popup margins. Restrict laser
        // intersection to the case, movement handle, open menu and flick preview so unused pixels cannot
        // steal focus from the dashboard.
        std::array<vr::VROverlayIntersectionMaskPrimitive_t, 4> mask{};
        auto rectangle = [&](std::size_t index, Rect bounds) {
            mask[index].m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Rectangle;
            mask[index].m_Primitive.m_Rectangle = {
                static_cast<float>(bounds.x), static_cast<float>(bounds.y + popup_margin),
                static_cast<float>(bounds.width), static_cast<float>(bounds.height)};
        };
        rectangle(0, panel_case_bounds(view));
        std::size_t count = 1;
        if (view.popup) {
            rectangle(count++, *view.popup);
        }
        // A flick may leave the case over its preview; keep tracking the laser there.
        if (const auto preview = flick_preview_bounds(view)) {
            rectangle(count++, *preview);
        }
        if (const auto handle = panel_move_handle_bounds(view)) {
            rectangle(count++, *handle);
        }
        check(vr_overlay()->SetOverlayIntersectionMask(handle_, mask.data(),
                                                       static_cast<std::uint32_t>(count)),
              "Set keyboard intersection mask");
        std::string error;
        if (!texture_.update(renderer, error)) {
            throw std::runtime_error(error);
        }
        const bool native = texture_.native_pixels();
        if (!reported_native_ || *reported_native_ != native) {
            std::cout << (native ? "Using native BGRA overlay texture.\n"
                                 : "Using RGBA compatibility overlay texture.\n")
                      << std::flush;
            reported_native_ = native;
        }
    }
    vr::VROverlayHandle_t handle() const { return handle_; }
    // Outside the dashboard in passthrough the overlay takes no SteamVR laser
    // input at all, so it can never claim the scene's controllers; the
    // passthrough pointer drives it instead.
    void set_scene_input(bool scene) {
        check(vr_overlay()->SetOverlayInputMethod(handle_, scene ? vr::VROverlayInputMethod_None
                                                                 : vr::VROverlayInputMethod_Mouse),
              "SetOverlayInputMethod");
    }

  private:
    bool connected_{};
    vr::VROverlayHandle_t handle_{};
    VulkanContext vulkan_;
    PanelTexture texture_;
    std::optional<bool> reported_native_;
};
} // namespace
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration) {
    RuntimeBinding runtime_binding(replacement);
    app.set_interaction_active(false);
    VrPanel panel;
    panel.connect();
    CornerResizers resizers;
    std::atomic<double> replacement_progress{monotonic_seconds()};
    std::atomic<bool> replacement_watched{false}, replacement_stalled{false};
    replacement.prepare_watchdog();
    // Steam restores stock independently. This thread removes a frozen panel that could
    // otherwise cover or intercept the recovered native keyboard. Apart from the
    // atomic slow-work deadline, it never touches App/input state.
    // Declared before the thread so it outlives the thread's final wait.
    WatchdogSchedule watchdog_schedule;
    std::jthread replacement_watchdog([&](std::stop_token stop) {
        while (watchdog_schedule.wait(stop, std::chrono::milliseconds(50))) {
            replacement.watchdog_tick(monotonic_seconds(), app.slow_work_deadline());
            if (replacement_watched.load() && monotonic_seconds() - replacement_progress.load() > .7) {
                vr_overlay()->HideOverlay(panel.handle());
                for (const auto handle : resizers.handles()) {
                    if (handle) {
                        vr_overlay()->HideOverlay(handle);
                    }
                }
                replacement_stalled.store(true);
            }
        }
    });
    VrHaptics haptics;
    auto manifest = fs::canonical("/proc/self/exe").parent_path().parent_path() /
                    "share/framekeyboard/vr/actions.json";
    if (!fs::is_regular_file(manifest)) {
        manifest = fs::canonical("/proc/self/exe").parent_path() / "vr/actions.json";
    }
    haptics.connect(manifest);
    KeyHaptics feedback;
    GrabInput grip_input;
    if (!grip_input.connect() && app.use_drag_to_move()) {
        app.report_status("Native grip unavailable; use Recenter to recover the keyboard.");
    }
    ScenePointer scene_pointer;
    if (!scene_pointer.connect()) {
        std::cout << "Passthrough pointer unavailable.\n" << std::flush;
    }
    SceneCursors cursors;
    LaserRays laser_rays;
    // The corner being resized; it stays shown while the other fades.
    std::size_t resize_side = 1;
    // Passthrough with the dashboard closed: the panel takes no SteamVR laser
    // events and the passthrough pointer drives it.
    bool scene_input = false;
    PassthroughRequests passthrough_requests;
    std::array<bool, 2> scene_inside{};
    std::array<std::pair<double, double>, 2> scene_last{};
    std::array<bool, vr::k_unMaxTrackedDeviceCount> hovered_devices{};
    std::map<unsigned, vr::TrackedDeviceIndex_t> pointer_devices;
    auto cancel_device = [&](vr::TrackedDeviceIndex_t device) {
        for (auto it = pointer_devices.begin(); it != pointer_devices.end();) {
            if (it->second == device) {
                app.cancel_pointer(it->first);
                feedback.cancel_pointer(it->first);
                it = pointer_devices.erase(it);
            } else {
                ++it;
            }
        }
    };
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
    bool done = false;
    const auto placement_path = app.config_dir() / "placement.json";
    std::optional<SavedPlacement> saved;
    try {
        saved = load_placement(placement_path);
    } catch (const std::exception& error) {
        std::cerr << "Saved placement ignored: " << error.what() << '\n';
    }
    bool recenter_pending = !saved, transform_dirty = false;
    std::uint64_t placement_universe = 0;
    PanelPlacement placement, displayed;
    if (saved) {
        // Size survives tracking-space changes, including a reset event before
        // the first valid head pose. Restore the world pose only after checking its universe.
        placement.set_width(saved->width);
    }
    HorizonAlignment horizon;
    bool auto_leveling = app.auto_leveling();
    std::optional<Transform> previous_dashboard;
    DashboardAnchor dashboard_anchor;
    if (saved && (!saved->universe || !panel.universe() || saved->universe == panel.universe())) {
        dashboard_anchor.restore(saved->dashboard, saved->dashboard_bar, saved->dashboard_full_rotation);
    }
    LaserDrag drag;
    // The room's leveling reference: facing the viewer's head, horizontally, as
    // seen from where the keyboard was released, recentered or restored. It is
    // taken only then, so the keyboard does not swivel after a walking user.
    std::optional<Transform> room_facing;
    // Set once the keyboard was shown with the dashboard closed (passthrough):
    // it then stays leveled to the room, even when the dashboard opens again,
    // until it is moved or recentered.
    bool room_until_moved = false;
    auto face_viewer = [](const Transform& head, const Transform& panel) -> std::optional<Transform> {
        double x = head[0][3] - panel[0][3], z = head[2][3] - panel[2][3];
        const double length = std::hypot(x, z);
        if (!std::isfinite(length) || length < .05) {
            return {};
        }
        x /= length;
        z /= length;
        // Columns: right = up x toward, room up, toward the viewer.
        return Transform{{{z, 0, x, panel[0][3]}, {0, 1, 0, panel[1][3]}, {-x, 0, z, panel[2][3]}}};
    };
    auto stop_drag = [&](bool align_after_release = false) {
        const bool resizing = drag.active() && drag.source() == MoveSource::Resize;
        feedback.cancel();
        // Tracking loss or timeout may already have stopped LaserDrag. Always
        // release the app's input gate as well; repeated cleanup is harmless.
        drag.stop();
        app.set_dragging(false);
        if (resizing) {
            // A resize keeps the pose: no re-facing, and the room or dashboard
            // leveling reference stays as it was. Keep the width reached.
            transform_dirty = true;
        } else if (align_after_release) {
            // Release may arrive without another movement sample. Schedule the
            // first alignment frame even when the hand was completely still.
            horizon.reset();
            transform_dirty = true;
            room_until_moved = false;
            if (Transform head{}; placement.ready() && panel.read_head(head)) {
                room_facing = face_viewer(head, placement.transform());
            }
        } else {
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
        }
    };
    VrLifecycle lifecycle(
        app.background_start(),
        {
            .stop_drag = [&] { stop_drag(); },
            .cancel_input = [&] { app.cancel(); },
            .clear_pointers =
                [&] {
                    hovered_devices.fill(false);
                    pointer_devices.clear();
                    grip_input.reset();
                    scene_pointer.reset();
                    scene_inside.fill(false);
                    cursors.hide();
                },
            .hide_overlay =
                [&] {
                    resizers.hide();
                    check(vr_overlay()->HideOverlay(panel.handle()), "Hide keyboard");
                },
            .route_input = [&](bool remote) { replacement.route(remote); },
            .set_interaction = [&](bool active) { app.set_interaction_active(active); },
            .show_overlay =
                [&](double now) {
                    app.paint(now);
                    panel.submit(app.renderer, app.view());
                    check(vr_overlay()->ShowOverlay(panel.handle()), "Show keyboard");
                    std::cout << "Full Keyboard overlay visible.\n" << std::flush;
                },
            .retry_bridge = [&](double now) { replacement.retry_now(now); },
            .summon =
                [&] {
                    std::cout << "Launch request received.\n" << std::flush;
                    app.summon();
                },
            .dismiss_request = [&] { replacement.dismiss_request(); },
            .cancel_device = [&](std::uint32_t device) { cancel_device(device); },
        });
    auto hide_panel = [&] { lifecycle.hide(); };
    // Passthrough keeps the panel open while the dashboard is closed.
    auto presentable = [&] { return app.passthrough() || vr_overlay()->IsDashboardVisible(); };
    // Near the visible dashboard the panel lines up with the dashboard itself,
    // which is often slightly rolled; elsewhere, with the dashboard closed, and
    // after being shown without it, it lines up with the room facing the viewer.
    std::optional<Transform> level_dashboard;
    enum class LevelReference { None, Room, Dashboard } leveled_to = LevelReference::None;
    auto level_reference = [&]() -> std::pair<LevelReference, std::optional<Transform>> {
        if (!room_until_moved && level_dashboard && placement.ready()) {
            const auto pose = placement.transform();
            const auto& dashboard = *level_dashboard;
            // The default place is about 0.35 m from the dashboard's bottom center.
            if (std::hypot(pose[0][3] - dashboard[0][3], pose[1][3] - dashboard[1][3],
                           pose[2][3] - dashboard[2][3]) <= .6) {
                return {LevelReference::Dashboard, dashboard};
            }
        }
        return {room_facing ? LevelReference::Room : LevelReference::None, room_facing};
    };
    auto place_panel = [&](double now) {
        const auto [level_to, reference] = level_reference();
        if (level_to != leveled_to) {
            // A change of reference eases from the correction shown. Checked here,
            // so a reference that changes in the same frame as a placement eases too.
            leveled_to = level_to;
            horizon.retarget(now);
        }
        const auto aligned = horizon.update(placement.transform(), now,
                                            drag.active() && drag.source() != MoveSource::Resize,
                                            auto_leveling, reference);
        if (!displayed.ready() || aligned != displayed.transform() ||
            placement.width() != displayed.width() || transform_dirty) {
            displayed.restore(aligned, placement.width());
            panel.place(displayed);
            transform_dirty = false;
        }
    };
    if (app.background_start()) {
        // The first texture upload can consume much of the summon deadline.
        // Prepare it while hidden, before the helper can advertise readiness.
        app.paint(monotonic_seconds());
        panel.submit(app.renderer, app.view());
        std::cout << "Full Keyboard started in background.\n" << std::flush;
    }
    auto dismiss_panel = [&] {
        if (app.take_dismissal()) {
            lifecycle.dismiss();
        }
    };
    replacement.frame_completed();
    bool replacement_was_ready = false;
    while (!done && !app.quitting() && !interrupted) {
        const auto frame_started = std::chrono::steady_clock::now();
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        replacement.poll(now);
        if (replacement.take_unsupported_control()) {
            app.report_status("Control unavailable in Steam fields. Use Steam keyboard for shortcuts.");
        }
        const auto native = replacement.state();
        app.set_replacement_available(native.available, replacement.unavailable_reason());
        app.set_native_session(native.open);
        const bool replace = app.replace_native_keyboard();
        // Wake the watchdog before this frame can start the helper that reads its health.
        watchdog_schedule.set_enabled(replace);
        lifecycle.begin_frame({.replacement_enabled = replace,
                               .native = native,
                               .routed_remote = replacement.remote(),
                               .watchdog_stalled = replacement_stalled.exchange(false),
                               .relaunch = instance.poll_recenter(),
                               .now = now});
        const bool dashboard_visible = vr_overlay()->IsDashboardVisible();
        if (passthrough_requests.take(replace, native, dashboard_visible)) {
            std::cout << "Passthrough turned on for a request outside the dashboard.\n" << std::flush;
            app.set_passthrough(true);
        }
        const bool panel_presentable = dashboard_visible || app.passthrough();
        lifecycle.dashboard_visibility(panel_presentable);
        const bool scene = app.passthrough() && !dashboard_visible;
        if (scene != scene_input) {
            // SteamVR's laser and the passthrough pointer never share a press:
            // release everything held, keeping unfinished composition.
            stop_drag();
            app.cancel(false);
            hovered_devices.fill(false);
            pointer_devices.clear();
            scene_pointer.reset();
            scene_inside.fill(false);
            cursors.hide();
            panel.set_scene_input(scene);
            resizers.set_scene_input(scene);
            scene_input = scene;
            std::cout << (scene ? "Passthrough pointer active.\n" : "Dashboard laser active.\n")
                      << std::flush;
        }
        // Hidden panels cannot be grabbed. Do not query both render models in
        // standby; require a released grip sample again when the panel returns.
        if (!lifecycle.visible()) {
            grip_input.reset();
        }
        const auto grips = lifecycle.visible() && app.use_drag_to_move() ? grip_input.poll()
                                                                         : std::array<GrabSample, 2>{};
        if (!app.use_drag_to_move()) {
            grip_input.reset();
        }
        if (drag.active() && !app.allows_movement(*drag.source())) {
            stop_drag();
        }
        vr::VREvent_t event{};
        while (vr_system()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr_system()->AcknowledgeQuit_Exiting();
                done = true;
            }
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                lifecycle.focus_lost();
                recenter_pending = true;
                saved.reset();
                previous_dashboard.reset();
                dashboard_anchor = {};
            }
            if (event.eventType == vr::VREvent_InputFocusChanged) {
                lifecycle.focus_lost();
            }
            if (event.eventType == vr::VREvent_TrackedDeviceDeactivated) {
                lifecycle.device_lost(event.trackedDeviceIndex,
                                      drag.active() ? std::optional{drag.device()} : std::nullopt);
            }
        }
        // SteamVR laser events and the passthrough pointer share these handlers.
        auto pointer_move = [&](unsigned pointer, vr::TrackedDeviceIndex_t device, double x, double y,
                                bool controller) {
            if (device < hovered_devices.size()) {
                hovered_devices[device] = panel_case_bounds(app.view()).contains(x, y);
            }
            if (!drag.active()) {
                if (app.move(pointer, x, y) && controller) {
                    feedback.hover(device);
                }
            }
        };
        auto pointer_down = [&](unsigned pointer, vr::TrackedDeviceIndex_t device, double x, double y,
                                bool controller, bool tracked_pointer) {
            if (drag.active()) {
                return;
            }
            const auto view = app.view();
            const auto handle = panel_move_handle_bounds(view);
            if (app.allows_movement(MoveSource::Laser) && !view.popup && handle &&
                handle->contains(x, y)) {
                const auto input = grip_input.laser_sample(device);
                if (tracked_pointer && placement.ready() &&
                    drag.start(input, displayed, now, MoveSource::Laser, pointer)) {
                    placement.restore(displayed.transform(), displayed.width());
                    horizon.reset();
                    feedback.cancel();
                    app.set_dragging(true);
                    std::cout << "Keyboard movement started: "
                              << (controller ? "controller" : "head cursor") << ".\n"
                              << std::flush;
                } else if (!tracked_pointer) {
                    // Source metadata for a handle click only. Never log
                    // coordinates or pointer/key events over typed keys.
                    std::cout << "Keyboard movement ignored: untracked pointer device " << device
                              << ", cursor " << pointer << ".\n"
                              << std::flush;
                }
                return; // A handle press must never also reach a key.
            }
            // Grabbing cannot also type if grip and trigger arrive together.
            const bool grabbing = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                return grip.active && grip.held && grip.device == device;
            });
            if (app.pinned() || !grabbing) {
                const bool key_pressed = app.down(pointer, x, y, now);
                if (key_pressed && controller) {
                    feedback.press(pointer, device);
                }
            }
        };
        // Either corner scales the keyboard about its center.
        auto start_resize = [&](std::size_t side, vr::TrackedDeviceIndex_t device, unsigned pointer) {
            if (drag.active() || !displayed.ready() || !app.allows_movement(MoveSource::Resize)) {
                return;
            }
            const auto ray = laser_rays.read(device);
            PanelResize resize;
            if (!ray || !resize.begin(displayed.transform(), displayed.width(), *ray) ||
                !drag.start_resize(device, pointer, resize, now)) {
                return;
            }
            // Only the width changes, so leveling keeps running on the same pose.
            resize_side = side;
            feedback.cancel();
            app.set_dragging(true, true);
            std::cout << "Keyboard resize started.\n" << std::flush;
        };
        auto pointer_up = [&](unsigned pointer, vr::TrackedDeviceIndex_t device, double x, double y) {
            if (drag.owns_pointer(device, pointer)) {
                stop_drag(true);
                app.move(pointer, x, y);
            } else if (!drag.active() && app.up(pointer, x, y)) {
                feedback.release(pointer);
            }
        };
        // Resizer events come first: a press there and a quick release over
        // the keyboard in the same frame must start the capture before it ends.
        for (std::size_t side = 0; side < 2; ++side) {
            while (resizers.poll(side, event)) {
                if (!lifecycle.visible() || scene_input || !dashboard_visible || done ||
                    app.quitting()) {
                    continue;
                }
                const auto device = event.trackedDeviceIndex;
                if (event.eventType == vr::VREvent_FocusLeave) {
                    // Without a trigger to watch (the head cursor), a release
                    // away from our overlays would never arrive: leaving the
                    // resizer ends the resize, keeping the size reached.
                    if (drag.active() && drag.source() == MoveSource::Resize &&
                        drag.device() == device && !drag.watches_trigger()) {
                        stop_drag(true);
                    }
                    continue;
                }
                const bool left_button = (event.eventType == vr::VREvent_MouseButtonDown ||
                                          event.eventType == vr::VREvent_MouseButtonUp) &&
                                         event.data.mouse.button == vr::VRMouseButton_Left;
                if (!left_button) {
                    continue;
                }
                const auto pointer = event.data.mouse.cursorIndex;
                if (event.eventType == vr::VREvent_MouseButtonDown) {
                    if (pointer_device(device)) {
                        start_resize(side, device, pointer);
                    }
                } else if (drag.owns_pointer(device, pointer)) {
                    stop_drag(true);
                }
            }
        }
        while (vr_overlay()->PollNextOverlayEvent(panel.handle(), &event, sizeof(event))) {
            // Recheck before every pointer event, not just once per batch. A
            // closing dashboard must not deliver queued clicks to a local app.
            if (!presentable()) {
                hide_panel();
            }
            if (event.eventType == vr::VREvent_OverlayClosed) {
                app.close_panel();
                dismiss_panel();
                if (app.quitting()) {
                    done = true;
                    break;
                }
            }
            // Laser events queued before the dashboard closed belong to the
            // old pointer; the passthrough pointer owns input until it reopens.
            if (!lifecycle.visible() || scene_input || !vr_overlay()->IsDashboardVisible()) {
                continue;
            }
            // OpenVR reports bottom-left coordinates; Cairo uses top-left.
            const double x = event.data.mouse.x, y = texture_height - event.data.mouse.y - popup_margin;
            const auto pointer = event.data.mouse.cursorIndex;
            const auto device = event.trackedDeviceIndex;
            const bool controller =
                device < vr::k_unMaxTrackedDeviceCount &&
                vr_system()->GetTrackedDeviceClass(device) == vr::TrackedDeviceClass_Controller;
            const bool tracked_pointer = pointer_device(device);
            if (tracked_pointer && (event.eventType == vr::VREvent_MouseMove ||
                                    event.eventType == vr::VREvent_MouseButtonDown ||
                                    event.eventType == vr::VREvent_MouseButtonUp)) {
                pointer_devices[pointer] = device;
            }
            switch (event.eventType) {
            case vr::VREvent_MouseMove:
                pointer_move(pointer, device, x, y, controller);
                break;
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    pointer_down(pointer, device, x, y, controller, tracked_pointer);
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    pointer_up(pointer, device, x, y);
                }
                break;
            case vr::VREvent_ScrollDiscrete:
                if (!drag.active()) {
                    app.scroll(event.data.scroll.cursorIndex, -event.data.scroll.xdelta,
                               -event.data.scroll.ydelta);
                }
                break;
            case vr::VREvent_FocusLeave:
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] = false;
                } else {
                    hovered_devices.fill(false);
                }
                // Focus events do not contain cursorIndex. The runtime adapter
                // resolves the device through actual pointer events only.
                lifecycle.pointer_left(device, drag.active());
                break;
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
                hovered_devices.fill(false);
                lifecycle.focus_lost();
                break;
            default:
                break;
            }
            dismiss_panel();
        }
        if (scene_input && lifecycle.visible() && !done && !app.quitting()) {
            const auto view = app.view();
            // The same areas as the intersection mask: transparent margins never take the ray.
            auto accepts = [&](double x, double y) {
                const auto preview = flick_preview_bounds(view);
                const auto handle = panel_move_handle_bounds(view);
                return panel_case_bounds(view).contains(x, y) ||
                       (view.popup && view.popup->contains(x, y)) ||
                       (preview && preview->contains(x, y)) || (handle && handle->contains(x, y));
            };
            const auto samples =
                scene_pointer.poll(displayed, static_cast<double>(texture_height) / panel_width);
            // Where each ray meets the panel's accepted areas, if it does.
            std::array<std::optional<std::pair<double, double>>, 2> inside{};
            for (std::size_t hand = 0; hand < samples.size(); ++hand) {
                if (const auto& hit = samples[hand].hit) {
                    const double x = hit->u * panel_width;
                    const double y = hit->v * texture_height - popup_margin;
                    if (accepts(x, y)) {
                        inside[hand] = std::pair{x, y};
                    }
                }
            }
            // A trigger pulled outside the panel may click another field or cursor
            // position in the app, even with a hand that never pointed at the
            // keyboard. Treat it as the laser leaving, before any key from this
            // batch, so swipe suggestions can no longer replace text there.
            for (std::size_t hand = 0; hand < samples.size(); ++hand) {
                if (!samples[hand].lost && samples[hand].pressed && !inside[hand]) {
                    app.cancel_pointer(ScenePointer::pointer(hand));
                }
            }
            for (std::size_t hand = 0; hand < samples.size(); ++hand) {
                const auto& sample = samples[hand];
                const auto pointer = ScenePointer::pointer(hand);
                if (sample.lost) {
                    // No release was seen: cancel rather than type.
                    cursors.hide(hand);
                    scene_inside[hand] = false;
                    if (drag.owns_pointer(sample.device, pointer)) {
                        stop_drag();
                    }
                    app.cancel_pointer(pointer);
                    feedback.cancel_pointer(pointer);
                    pointer_devices.erase(pointer);
                    continue;
                }
                const auto& at = inside[hand];
                if (at) {
                    scene_last[hand] = *at;
                }
                if (at || drag.owns_pointer(sample.device, pointer)) {
                    pointer_devices[pointer] = sample.device;
                }
                if (at) {
                    cursors.show(hand, displayed.transform(), *sample.hit);
                    scene_inside[hand] = true;
                    pointer_move(pointer, sample.device, at->first, at->second, true);
                    if (sample.pressed) {
                        pointer_down(pointer, sample.device, at->first, at->second, true, true);
                    }
                } else {
                    // The ray may rest on a shown corner resizer, outside the case.
                    std::optional<Vector3> resizer;
                    std::size_t side = 0;
                    for (; sample.ray && side < 2 && !resizer; ++side) {
                        if (resizers.alpha(side) > .3) {
                            resizer = resizer_hit(displayed.transform(),
                                                  case_corner(view, displayed.width(), side == 1, true),
                                                  corner_of(side),
                                                  resizer_shape(view, displayed.width()), *sample.ray);
                        }
                    }
                    if (resizer) {
                        PanelHit hit;
                        hit.point = *resizer;
                        cursors.show(hand, displayed.transform(), hit);
                        if (sample.pressed) {
                            start_resize(side - 1, sample.device, pointer);
                        }
                    } else {
                        cursors.hide(hand);
                    }
                    if (std::exchange(scene_inside[hand], false)) {
                        // Leaving the panel ends this hand's presses, as SteamVR's FocusLeave does.
                        if (sample.device < hovered_devices.size()) {
                            hovered_devices[sample.device] = false;
                        }
                        lifecycle.pointer_left(sample.device, drag.active());
                    }
                }
                if (sample.released) {
                    pointer_up(pointer, sample.device, scene_last[hand].first, scene_last[hand].second);
                }
                if (!app.passthrough()) {
                    // Turned off by this hand: the panel closes below, and the
                    // other hand's press in this batch must not type first.
                    break;
                }
            }
            dismiss_panel();
            // Only the passthrough pointer can turn it off with the dashboard closed.
            if (!app.passthrough() && PassthroughRequests::ends_request(replace, native)) {
                app.close_panel();
                dismiss_panel();
            }
        }
        if (done || app.quitting()) {
            break;
        }
        if (auto_leveling != app.auto_leveling()) {
            // Adopt the visible pose before disabling assistance so a partial
            // correction never jumps back to the original roll. Enabling starts
            // a fresh ease from that same pose, including changes via Reload.
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
            auto_leveling = app.auto_leveling();
            transform_dirty = true;
        }
        // A tracked head cursor or controller may hold a key for minutes. Lost
        // tracking releases only the keys belonging to that pointer device.
        std::set<vr::TrackedDeviceIndex_t> lost_devices;
        for (const auto& [pointer, device] : pointer_devices) {
            if (app.pointer_pressed(pointer) && !read_pointer_pose(device)) {
                lost_devices.insert(device);
            }
        }
        for (const auto device : lost_devices) {
            lifecycle.device_lost(device, std::nullopt);
        }
        if (drag.active() && !app.allows_movement(*drag.source())) {
            stop_drag();
        }
        if (drag.active() && drag.trigger_released(now)) {
            stop_drag(true);
        }
        if (drag.active() && drag.source() == MoveSource::Grip) {
            const bool held = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                return grip.active && grip.held && grip.device == drag.device();
            });
            if (!held) {
                stop_drag(true);
            }
        } else if (!drag.active() && lifecycle.visible() && placement.ready() &&
                   app.allows_movement(MoveSource::Grip)) {
            for (const auto& grip : grips) {
                if (grip.active && grip.pressed && grip.device < hovered_devices.size() &&
                    hovered_devices[grip.device] && drag.start(grip, displayed, now, MoveSource::Grip)) {
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
            if (!drag.active() && lifecycle.visible()) {
                RuntimeWait runtime_wait;
                haptics.send(device, kind);
            }
        }
        if (app.take_recenter()) {
            replacement.retry_now(now);
            stop_drag();
            saved.reset();
            recenter_pending = true;
            lifecycle.request_recenter();
        }
        if (drag.active()) {
            bool followed = false;
            if (drag.source() == MoveSource::Resize) {
                followed = drag.resize(placement, now, laser_rays.read(drag.device()));
            } else {
                double stick_y = 0;
                if (drag.source() == MoveSource::Laser) {
                    stick_y = grip_input.laser_sample(drag.device()).stick_y;
                } else {
                    for (const auto& input : grips) {
                        if (input.active && input.held && input.device == drag.device()) {
                            stick_y = input.stick_y;
                        }
                    }
                }
                followed = drag.update(placement, now, stick_y);
            }
            if (followed) {
                transform_dirty = true;
            } else {
                stop_drag();
            }
        }
        if (!dashboard_visible) {
            level_dashboard.reset();
            if (lifecycle.visible()) {
                room_until_moved = true;
            }
        }
        // place_panel eases to a new leveling reference; it must run even when
        // nothing moved.
        if (level_reference().first != leveled_to) {
            transform_dirty = true;
        }
        const bool panel_requested = lifecycle.panel_requested();
        if (lifecycle.should_sample_tracking(panel_presentable, now)) {
            Transform head{};
            const bool was_waiting = lifecycle.waiting_for_tracking();
            const bool valid_head = panel.read_head(head);
            lifecycle.tracking_sampled(valid_head, now);
            if (!valid_head) {
                if (recenter_pending && !was_waiting) {
                    std::cout << "Waiting for valid headset pose before recentering.\n" << std::flush;
                }
                // Lifecycle hides input; retain placement through brief tracking loss.
            } else if (panel_requested) {
                // A hidden recovery probe only restores readiness. Placement and
                // presentation still require a fresh summon or manual launch.
                if (recenter_pending) {
                    dashboard_anchor = {}; // Recalibrate the mount only on an explicit reset.
                }
                const auto dashboard =
                    dashboard_anchor.update(panel.read_dashboard("valve.steam.gamepadui.main", true),
                                            panel.read_dashboard("valve.steam.gamepadui.bar", false));
                level_dashboard = dashboard;
                if (saved) {
                    const auto universe = panel.universe();
                    if (saved->universe && universe && saved->universe != universe) {
                        recenter_pending = true;
                        std::cout << "Tracking space changed; recentering keyboard.\n";
                    } else {
                        placement.restore(saved->transform, saved->width);
                        room_facing = face_viewer(head, saved->transform);
                        placement_universe = universe;
                        previous_dashboard = saved->dashboard;
                        transform_dirty = true;
                        std::cout << "Restored saved keyboard placement.\n" << std::flush;
                    }
                    saved.reset();
                }
                if (recenter_pending) {
                    horizon.reset();
                    placement.recenter(head, dashboard ? &*dashboard : nullptr,
                                       static_cast<double>(panel_height) / panel_width);
                    room_facing = face_viewer(head, placement.transform());
                    room_until_moved = false;
                    previous_dashboard = dashboard;
                    placement_universe = panel.universe();
                    recenter_pending = false;
                    transform_dirty = true;
                    std::cout << (dashboard ? "Keyboard recentered below current dashboard.\n"
                                            : "Keyboard recentered below current headset heading.\n")
                              << std::flush;
                }
                if (dashboard) {
                    if (previous_dashboard && *previous_dashboard != *dashboard && placement.ready() &&
                        !drag.active()) {
                        placement.set_transform(
                            move_with_dashboard(placement.transform(), *previous_dashboard, *dashboard));
                        // The room reference turns with it; otherwise a keyboard
                        // carried to a new heading would level toward the old one.
                        if (room_facing) {
                            room_facing = turn_with_dashboard(*room_facing, *previous_dashboard,
                                                              *dashboard, placement.transform());
                        }
                        transform_dirty = true;
                    }
                    // While grabbed the hand owns placement; keep the anchor
                    // current so release preserves the new custom offset.
                    previous_dashboard = dashboard;
                }
                if (transform_dirty) {
                    place_panel(now);
                }
                // Read actual compositor visibility rather than trusting our cached flag.
                // Another UI can hide an overlay without destroying its owner process.
                const bool compositor_visible = vr_overlay()->IsOverlayVisible(panel.handle());
                lifecycle.present(placement.ready(), presentable(), compositor_visible, now);
            }
        }
        if (lifecycle.visible() && placement.ready() && (transform_dirty || horizon.animating())) {
            place_panel(now);
        }
        // Corner resizers fade in wherever any laser comes near: either hand's,
        // and the head cursor while it is the dashboard's pointer. Pinning hides them.
        if (!lifecycle.visible() || !displayed.ready()) {
            resizers.hide();
        } else {
            const auto view = app.view();
            const auto shape = resizer_shape(view, displayed.width());
            const auto corner = [&](std::size_t side) {
                return case_corner(view, displayed.width(), side == 1, true);
            };
            std::array<double, 2> target{};
            const bool resizing = drag.active() && drag.source() == MoveSource::Resize;
            if (resizing) {
                target[resize_side] = 1;
            } else if (!drag.active() && app.allows_movement(MoveSource::Resize)) {
                std::vector<vr::TrackedDeviceIndex_t> lasers{
                    vr_system()->GetTrackedDeviceIndexForControllerRole(
                        vr::TrackedControllerRole_LeftHand),
                    vr_system()->GetTrackedDeviceIndexForControllerRole(
                        vr::TrackedControllerRole_RightHand)};
                if (const auto primary = vr_overlay()->GetPrimaryDashboardDevice();
                    !scene_input && primary == vr::k_unTrackedDeviceIndex_Hmd) {
                    lasers.push_back(primary);
                }
                for (const auto device : lasers) {
                    const auto ray = laser_rays.read(device);
                    for (std::size_t side = 0; ray && side < 2; ++side) {
                        const auto bend =
                            resizer_target(displayed.transform(), corner(side), corner_of(side), shape);
                        target[side] =
                            std::max(target[side], resizer_proximity(ray_distance(*ray, bend)));
                    }
                }
            }
            for (std::size_t side = 0; side < 2; ++side) {
                resizers.update(side, resizer_transform(displayed.transform(), corner(side)), shape,
                                target[side], now, resizing && side == resize_side);
            }
        }
        // Repeat and text commits share the same gate as pointer presses.
        if (!presentable()) {
            hide_panel();
        }
        const bool repaint = app.tick(now);
        if (lifecycle.visible() && repaint) {
            app.paint(now);
            panel.submit(app.renderer, app.view());
        }
        bool fallback = app.take_native_fallback();
        if (replacement_stalled.exchange(false)) {
            fallback = true;
        }
        if (fallback) {
            lifecycle.fallback();
        }
        // A hidden resident must remain ready for the next summon, including one that
        // opens the dashboard. Presentation is acknowledged separately for each request.
        const bool backend_ready = app.replacement_ready();
        const bool compositor_visible = lifecycle.ready(backend_ready, panel_presentable) &&
                                        panel_presentable && lifecycle.visible() &&
                                        vr_overlay()->IsOverlayVisible(panel.handle());
        const auto report = lifecycle.frame_report(backend_ready, panel_presentable, compositor_visible);
        const auto completed = monotonic_seconds();
        // Diagnostics with fixed text only. A frame over the 0.9 s lease ends a
        // Steam typing session, so record both causes for later troubleshooting.
        if (completed - now > .5) {
            std::cout << "Slow frame: " << static_cast<int>((completed - now) * 1000) << " ms.\n"
                      << std::flush;
        }
        if (replace && replacement_was_ready && !backend_ready) {
            std::cout << "Native replacement paused: " << app.replacement_block() << ".\n" << std::flush;
        }
        replacement_was_ready = backend_ready;
        replacement.update(replace, report.ready, report.acknowledgement, fallback || report.fallback,
                           completed, true);
        replacement_progress.store(completed);
        app.frame_completed();
        if (!done && !app.quitting() && !interrupted) {
            update_startup_ready(completed);
        }
        replacement.frame_completed();
        replacement_watched.store(replace && lifecycle.visible());
        // Follow the dashboard every visible frame, even without a laser over
        // the keys. Budget work inside the 60 Hz period instead of adding a full
        // sleep after it. Hidden panels still idle at 4 Hz; paints are event-driven.
        const auto period = std::chrono::microseconds(lifecycle.visible() ? 16667 : 250000);
        if (lifecycle.visible()) {
            std::this_thread::sleep_until(frame_started + period);
        } else {
            replacement.wait_until(frame_started + period);
        }
    }
    hide_panel();
    replacement_watched.store(false);
    if (app.updating() && !interrupted) {
        replacement.shutdown_for_update();
    } else {
        replacement.update(false, false, 0, false, monotonic_seconds());
    }
    app.cancel();
    if (displayed.ready()) {
        try {
            save_placement(placement_path, {displayed.transform(), displayed.width(), placement_universe,
                                            previous_dashboard, dashboard_anchor.bar(), true});
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
