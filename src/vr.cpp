#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/held_keys.hpp"
#include "framekeyboard/native_replacement.hpp"
#include "framekeyboard/panel_motion.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/resizer.hpp"
#include "framekeyboard/shell_host.hpp"
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
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <time.h>

namespace framekeyboard {
namespace {
// Seconds the system has spent suspended since boot.
double suspended_seconds() {
    timespec boot{}, steady{};
    clock_gettime(CLOCK_BOOTTIME, &boot);
    clock_gettime(CLOCK_MONOTONIC, &steady);
    return static_cast<double>(boot.tv_sec - steady.tv_sec) +
           static_cast<double>(boot.tv_nsec - steady.tv_nsec) / 1e9;
}
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
// Any controller's thumbstick tilt, x right and y up, from its render-model
// component: the dashboard owns the controllers' input actions, but reading
// the model takes nothing from it, as for the grip.
class StickReader {
  public:
    std::optional<std::pair<double, double>> read(vr::TrackedDeviceIndex_t device, double now) {
        if (device >= vr::k_unMaxTrackedDeviceCount ||
            vr_system()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
            return {};
        }
        auto models = vr_models();
        auto& stick = sticks_[device];
        vr::RenderModel_ControllerMode_State_t mode{};
        // A controller can appear before its role or model components are
        // known, so a failed setup is retried, at most once a second.
        if (!stick.ready && now >= stick.retry_at) {
            stick.retry_at = now + 1;
            const auto role = vr_system()->GetControllerRoleForTrackedDeviceIndex(device);
            char model[1024]{};
            vr::ETrackedPropertyError error{};
            vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                        sizeof(model), &error);
            vr::VRControllerState_t released{}, right{}, up{};
            right.rAxis[0].x = 1;
            up.rAxis[0].y = 1;
            vr::RenderModel_ComponentState_t rest{}, full_right{}, full_up{};
            auto input = vr_input();
            stick.model = model;
            stick.ready = !error && models && input &&
                          (role == vr::TrackedControllerRole_LeftHand ||
                           role == vr::TrackedControllerRole_RightHand) &&
                          input->GetInputSourceHandle(role == vr::TrackedControllerRole_LeftHand
                                                          ? "/user/hand/left"
                                                          : "/user/hand/right",
                                                      &stick.source) == vr::VRInputError_None &&
                          models->GetComponentState(model, "thumbstick", &released, &mode, &rest) &&
                          models->GetComponentState(model, "thumbstick", &right, &mode, &full_right) &&
                          models->GetComponentState(model, "thumbstick", &up, &mode, &full_up) &&
                          stick.x.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                            from_vr(full_right.mTrackingToComponentRenderModel)) &&
                          stick.y.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                            from_vr(full_up.mTrackingToComponentRenderModel));
        }
        vr::RenderModel_ComponentState_t current{};
        if (!stick.ready || !models->GetComponentStateForDevicePath(stick.model.c_str(), "thumbstick",
                                                                    stick.source, &mode, &current)) {
            return {};
        }
        const auto pose = from_vr(current.mTrackingToComponentRenderModel);
        return std::pair{stick.x.read(pose), stick.y.read(pose)};
    }
    // Controllers came, went or changed hands.
    void forget() { sticks_ = {}; }

  private:
    struct Stick {
        bool ready{};
        double retry_at{};
        std::string model;
        vr::VRInputValueHandle_t source{};
        ComponentAxis x, y;
    };
    std::array<Stick, vr::k_unMaxTrackedDeviceCount> sticks_{};
};
// A Frame controller's trigger, read from its render model, so a capture can
// end on the trigger's release even when the laser is off our overlays and
// SteamVR sends us no button event. Other controllers are not watched.
class TriggerWatch {
  public:
    void begin(vr::TrackedDeviceIndex_t device, double now) {
        neutral_.reset();
        release_.begin(now);
        auto models = vr_models();
        auto input = vr_input();
        if (!models || !input) {
            return;
        }
        const auto role = vr_system()->GetControllerRoleForTrackedDeviceIndex(device);
        if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) {
            return;
        }
        const bool left = role == vr::TrackedControllerRole_LeftHand;
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        const std::string expected = left ? "{frame_controller}frame_controller_left"
                                          : "{frame_controller}frame_controller_right";
        if (error || model != expected ||
            input->GetInputSourceHandle(left ? "/user/hand/left" : "/user/hand/right", &hand_) !=
                vr::VRInputError_None) {
            return;
        }
        vr::VRControllerState_t released{};
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t neutral{};
        if (models->GetComponentState(model, "trigger", &released, &mode, &neutral)) {
            model_ = model;
            neutral_ = from_vr(neutral.mTrackingToComponentRenderModel);
        }
    }
    void reset() { neutral_.reset(); }
    bool watching() const { return neutral_.has_value(); }
    bool released(double now) {
        if (!neutral_) {
            return false;
        }
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t current{};
        if (!vr_models()->GetComponentStateForDevicePath(model_.c_str(), "trigger", hand_, &mode,
                                                         &current)) {
            return release_.update(std::nullopt, now);
        }
        const auto angle =
            component_rotation_degrees(*neutral_, from_vr(current.mTrackingToComponentRenderModel));
        return release_.update(angle, now);
    }

  private:
    std::string model_;
    vr::VRInputValueHandle_t hand_{};
    std::optional<Transform> neutral_;
    TriggerReleaseGuard release_;
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
        trigger_.reset();
        if (source == MoveSource::Laser) {
            trigger_.begin(input.device, now);
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
        trigger_.begin(device, now);
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
    bool watches_trigger() const { return source() != MoveSource::Grip && trigger_.watching(); }
    bool trigger_released(double now) { return source() != MoveSource::Grip && trigger_.released(now); }
    void stop() { capture_.reset(); }

  private:
    PanelDrag transform_;
    PanelResize resize_;
    MoveCapture capture_;
    double started_{}, updated_{};
    TriggerWatch trigger_;
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
    // A calibrated hand's thumbstick, x right and y up, from the component's
    // tilt; like the trigger, reading it takes nothing from the app.
    std::optional<std::pair<double, double>> stick(std::size_t index) {
        auto& hand = hands_[index];
        if (!hand.calibrated) {
            return {};
        }
        auto models = vr_models();
        vr::RenderModel_ControllerMode_State_t mode{};
        if (!std::exchange(hand.stick_tried, true)) {
            vr::VRControllerState_t released{}, right{}, up{};
            right.rAxis[0].x = 1;
            up.rAxis[0].y = 1;
            vr::RenderModel_ComponentState_t rest{}, full_right{}, full_up{};
            hand.stick_ready =
                models->GetComponentState(hand.model.c_str(), "thumbstick", &released, &mode, &rest) &&
                models->GetComponentState(hand.model.c_str(), "thumbstick", &right, &mode,
                                          &full_right) &&
                models->GetComponentState(hand.model.c_str(), "thumbstick", &up, &mode, &full_up) &&
                hand.stick_x.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                       from_vr(full_right.mTrackingToComponentRenderModel)) &&
                hand.stick_y.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                       from_vr(full_up.mTrackingToComponentRenderModel));
        }
        vr::RenderModel_ComponentState_t current{};
        if (!hand.stick_ready || !models->GetComponentStateForDevicePath(
                                     hand.model.c_str(), "thumbstick", hand.source, &mode, &current)) {
            return {};
        }
        const auto pose = from_vr(current.mTrackingToComponentRenderModel);
        return std::pair{hand.stick_x.read(pose), hand.stick_y.read(pose)};
    }

  private:
    struct Hand {
        vr::VRInputValueHandle_t source{};
        vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
        std::string model;
        bool warned{}, calibrated{}, armed{}, held{};
        bool stick_tried{}, stick_ready{};
        ComponentAxis stick_x, stick_y;
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
    // One tracking-pose read per frame serves every ray in it.
    void begin_frame(double now) {
        now_ = now;
        posed_ = false;
        shared_ = {};
    }
    // A ray the passthrough pointer already computed this frame, from the
    // same tip component; reading it needs no second pose query.
    void share(vr::TrackedDeviceIndex_t device, const Ray& ray) {
        for (auto& slot : shared_) {
            if (!slot) {
                slot = std::pair{device, ray};
                return;
            }
        }
    }
    // Device classes, roles and laser tips change only with these device
    // events; until then they are read once instead of every frame.
    static bool invalidates(std::uint32_t event) {
        return event == vr::VREvent_TrackedDeviceActivated ||
               event == vr::VREvent_TrackedDeviceDeactivated ||
               event == vr::VREvent_TrackedDeviceUpdated ||
               event == vr::VREvent_TrackedDeviceRoleChanged;
    }
    void forget() {
        devices_.fill({});
        hands_.reset();
    }
    // The left and right hand controllers' device indices.
    std::array<vr::TrackedDeviceIndex_t, 2> hands() {
        if (!hands_) {
            hands_ = {
                vr_system()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand),
                vr_system()->GetTrackedDeviceIndexForControllerRole(
                    vr::TrackedControllerRole_RightHand)};
        }
        return *hands_;
    }
    std::optional<Ray> read(vr::TrackedDeviceIndex_t device) {
        if (device >= vr::k_unMaxTrackedDeviceCount) {
            return {};
        }
        for (const auto& slot : shared_) {
            if (slot && slot->first == device) {
                return slot->second;
            }
        }
        auto& info = devices_[device];
        if (!info || (!info->complete && now_ >= info->retry)) {
            info = describe(device);
        }
        if (info->kind != vr::TrackedDeviceClass_Controller &&
            info->kind != vr::TrackedDeviceClass_HMD) {
            return {};
        }
        if (!posed_) {
            vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses_.data(),
                                                         vr::k_unMaxTrackedDeviceCount);
            posed_ = true;
        }
        const auto& state = poses_[device];
        if (!state.bPoseIsValid || !state.bDeviceIsConnected) {
            return {};
        }
        const auto pose = from_vr(state.mDeviceToAbsoluteTracking);
        Ray ray{};
        ray.direction = {};
        for (std::size_t r = 0; r < 3; ++r) {
            ray.origin[r] = pose[r][3];
            for (std::size_t c = 0; c < 3; ++c) {
                ray.origin[r] += pose[r][c] * info->tip[c][3];
                ray.direction[r] -= pose[r][c] * info->tip[c][2];
            }
        }
        return ray;
    }

  private:
    static constexpr Transform identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    struct Device {
        vr::ETrackedDeviceClass kind{};
        // A controller's laser origin and -Z direction; the head cursor's is its own pose.
        Transform tip = identity;
        // A controller may connect before its render model is known: retry each second.
        bool complete{};
        double retry{};
    };
    Device describe(vr::TrackedDeviceIndex_t device) const {
        Device result{vr_system()->GetTrackedDeviceClass(device), identity, true, now_ + 1};
        if (result.kind != vr::TrackedDeviceClass_Controller) {
            return result;
        }
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        vr::VRControllerState_t released{};
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t state{};
        result.complete = !error && model[0] && vr_models() &&
                          vr_models()->GetComponentState(model, "tip", &released, &mode, &state);
        if (result.complete) {
            result.tip = from_vr(state.mTrackingToComponentLocal);
        }
        return result;
    }
    std::array<std::optional<Device>, vr::k_unMaxTrackedDeviceCount> devices_{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses_{};
    std::optional<std::array<vr::TrackedDeviceIndex_t, 2>> hands_;
    std::array<std::optional<std::pair<vr::TrackedDeviceIndex_t, Ray>>, 2> shared_{};
    double now_{};
    bool posed_{};
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
// Whether a resizer's square, in panel pixels, overlaps an open dropdown or a
// flick preview. Both may extend past the case, where the resizer, in front of
// the keyboard, would otherwise take their clicks.
bool resizer_covers_popup(const PanelView& view, double width, std::size_t side,
                          const ResizerShape& shape) {
    const auto body = panel_case_bounds(view);
    const double half = shape.half_size() * panel_width / width;
    const Rect area{(side == 1 ? body.x + body.width : body.x) - half, body.y + body.height - half,
                    2 * half, 2 * half};
    auto overlaps = [&](const std::optional<Rect>& r) {
        return r && r->x < area.x + area.width && area.x < r->x + r->width &&
               r->y < area.y + area.height && area.y < r->y + r->height;
    };
    return overlaps(view.popup) || overlaps(flick_preview_bounds(view));
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
    // A hidden overlay receives no laser events: after one final drain its
    // queue is left alone, saving an IPC call per frame, also while idle.
    bool poll(std::size_t side, vr::VREvent_t& event) {
        auto& corner = corners_[side];
        if (!corner.handle || (!corner.visible && !corner.drain)) {
            return false;
        }
        if (vr_overlay()->PollNextOverlayEvent(corner.handle, &event, sizeof(event))) {
            return true;
        }
        corner.drain = corner.visible;
        return false;
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
        // Held or fading out, the current image stays: replacing it costs a
        // raw upload and flickers.
        if (corner.shape != shape && !((held || target == 0) && corner.shape) && !draw(side, shape)) {
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
    bool visible(std::size_t side) const { return corners_[side].visible; }
    // On screen and opaque enough to be seen: only then may the passthrough
    // pointer press it. A failed overlay or image never counts.
    bool shown(std::size_t side) const {
        const auto& corner = corners_[side];
        return corner.handle && corner.visible && corner.shape && corner.fade.value() > .3;
    }
    void hide() {
        for (auto& corner : corners_) {
            corner.fade.reset();
            hide(corner);
        }
    }
    // At once, without fading, so it never takes the laser meanwhile.
    void hide(std::size_t side) {
        corners_[side].fade.reset();
        hide(corners_[side]);
    }

  private:
    struct Corner {
        vr::VROverlayHandle_t handle{};
        bool visible{}, drain{};
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
            corner.drain = true;
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
        // At most 2 rectangles and about 8 circles, within OpenVR's 32 primitives.
        std::vector<vr::VROverlayIntersectionMaskPrimitive_t> mask;
        const auto area = resizer_mask(corner_of(side), shape);
        for (const auto& r : area.rectangles) {
            auto& primitive = mask.emplace_back();
            primitive.m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Rectangle;
            primitive.m_Primitive.m_Rectangle = {r[0], r[1], r[2], r[3]};
        }
        for (const auto& c : area.circles) {
            auto& primitive = mask.emplace_back();
            primitive.m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Circle;
            primitive.m_Primitive.m_Circle = {c[0], c[1], c[2]};
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
        // Scroll events are switched on only where something scrolls; see accept_scroll().
        for (const auto flag : {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MultiCursor}) {
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
    // `rise` moves the panel along its own up axis, for its entrance and exit.
    void place(const PanelPlacement& placement, double rise = 0) {
        const auto world = placement.transform();
        vr::HmdMatrix34_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                transform.m[row][col] =
                    static_cast<float>(world[row][col] + (col == 3 ? rise * world[row][1] : 0));
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
    // The Frame controller's dashboard binding turns the stick into discrete
    // scroll steps, each with a haptic tick, or into smooth scrolling. Taking
    // no scroll events did not stop the ticks, so the panel asks for discrete
    // steps only while a laser is over something that scrolls, and smooth
    // scrolling elsewhere, whose events it ignores.
    void accept_scroll(bool scroll) {
        if (scroll_ == scroll) {
            return;
        }
        auto overlay = vr_overlay();
        // Drop the old mode before taking the new one, so both are never set.
        const auto old = scroll ? vr::VROverlayFlags_SendVRSmoothScrollEvents
                                : vr::VROverlayFlags_SendVRDiscreteScrollEvents;
        const auto now = scroll ? vr::VROverlayFlags_SendVRDiscreteScrollEvents
                                : vr::VROverlayFlags_SendVRSmoothScrollEvents;
        // Recorded even if the runtime refuses, so a refusal costs two calls
        // per change rather than two every frame; the next change tries again.
        scroll_ = scroll;
        if (overlay->SetOverlayFlag(handle_, old, false) != vr::VROverlayError_None ||
            overlay->SetOverlayFlag(handle_, now, true) != vr::VROverlayError_None) {
            std::cout << "Keyboard scroll mode not changed.\n" << std::flush;
        }
    }
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
    std::optional<bool> scroll_;
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
    std::optional<vr::TrackedDeviceIndex_t> last_pressed; // Controller of the last laser press.
    bool was_listening = false;
    double last_suspended = suspended_seconds(), next_presence_check = 0;
    GrabInput grip_input;
    if (!grip_input.connect() && app.use_drag_to_move()) {
        app.report_status("Native grip unavailable; use Recenter to recover the keyboard.");
    }
    ScenePointer scene_pointer;
    double stick_scrolled = 0; // When the passthrough stick last scrolled the emoji grid.
    if (!scene_pointer.connect()) {
        std::cout << "Passthrough pointer unavailable.\n" << std::flush;
    }
    SceneCursors cursors;
    LaserRays laser_rays;
    StickReader sticks;
    StickScrollDirection stick_direction;
    StickStepRate stick_rate;
    std::map<unsigned, int> stick_leaning; // Last prediction per pointer, for hysteresis.
    std::array<ResizerReach, 2> resizer_reach{};
    // The dashboard's pointing device: the head cursor counts as a laser while it is.
    vr::TrackedDeviceIndex_t dashboard_pointer = vr::k_unTrackedDeviceIndexInvalid;
    double dashboard_pointer_checked = -1;
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
    // Repeating keys held after their laser left the panel (held_keys.hpp).
    OffPanelHolds<TriggerWatch> key_holds;
    // Whether leaving the panel keeps the device's repeating key held. The
    // passthrough pointer reads its trigger itself; a SteamVR laser needs a
    // Frame controller's trigger to watch, or the key is released as before.
    auto keeps_held_key = [&](vr::TrackedDeviceIndex_t device, double now, bool passthrough) {
        for (const auto& [pointer, owner] : pointer_devices) {
            if (owner != device || !app.holds_repeat(pointer)) {
                continue;
            }
            if (passthrough) {
                return true;
            }
            TriggerWatch trigger;
            trigger.begin(device, now);
            if (!trigger.watching()) {
                return false;
            }
            key_holds.hold(pointer, trigger, now);
            return true;
        }
        return false;
    };
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
    // Under a shell the placement is its own, so neither mode moves the other's keyboard.
    const bool hosted = app.hosted();
    const auto placement_path = app.config_dir() / (hosted ? "placement-hosted.json" : "placement.json");
    // A shell's own placement: a pose it set may be left unleveled, and an
    // anchor holds the panel relative to another overlay.
    bool shell_level = true;
    std::optional<std::pair<std::string, Transform>> anchor; // overlay key, pose relative to it
    std::optional<SavedPlacement> saved;
    try {
        saved = load_placement(placement_path);
    } catch (const std::exception& error) {
        std::cerr << "Saved placement ignored: " << error.what() << '\n';
    }
    bool recenter_pending = !saved, transform_dirty = false;
    std::uint64_t placement_universe = 0;
    PanelPlacement placement, displayed;
    // The panel's fade in and out (panel_motion.hpp). An exit keeps the overlay
    // shown, without input, until it has faded; fallback and shutdown cut it.
    PanelMotion motion;
    bool cut_exit = false;
    auto end_exit = [&] {
        motion.cut();
        check(vr_overlay()->HideOverlay(panel.handle()), "Hide keyboard");
    };
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
                    app.panel_hidden();
                    resizers.hide();
                    if (cut_exit) {
                        end_exit();
                    } else {
                        motion.hide(monotonic_seconds());
                    }
                },
            .route_input = [&](bool remote) { replacement.route(remote); },
            .set_interaction = [&](bool active) { app.set_interaction_active(active); },
            .show_overlay =
                [&](double now) {
                    app.paint(now);
                    panel.submit(app.renderer, app.view());
                    motion.show(now);
                    vr_overlay()->SetOverlayAlpha(panel.handle(), static_cast<float>(motion.alpha(now)));
                    if (displayed.ready()) {
                        panel.place(displayed, motion.offset(now));
                    }
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
    // Passthrough keeps the panel open while the dashboard is closed. Under a
    // shell the dashboard gate does not apply: the shell decides.
    auto presentable = [&] { return hosted || app.passthrough() || vr_overlay()->IsDashboardVisible(); };
    lifecycle.set_hosted(hosted);
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
                                            auto_leveling && shell_level && !anchor, reference);
        if (!displayed.ready() || aligned != displayed.transform() ||
            placement.width() != displayed.width() || transform_dirty) {
            displayed.restore(aligned, placement.width());
            panel.place(displayed, motion.offset(now));
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
    replacement.frame_completed();
    bool replacement_was_ready = false;
    // The shell's session (docs/integration.md). Requests are handled at the
    // start of a frame; events, telemetry and pings go out at its end.
    std::unique_ptr<ShellConnection> shell;
    int exit_status = 0;
    bool shell_shown = false, ready_sent = false, was_visible = false, shell_haptics = false;
    std::string hide_reason = "shell", geometry_source = "user";
    std::optional<std::tuple<Transform, double>> reported_geometry;
    double geometry_at = -1;
    std::string reported_regions;
    std::string update_when = "idle";
    UpdateHandover handover;
    Updates::State reported_update = Updates::State::Idle;
    std::string reported_update_error;
    std::array<bool, 2> forwarded_inside{};
    std::array<int, 2> forwarded_moves{};
    std::optional<PreviewRenderer> previews;
    std::optional<std::int64_t> preview_request, check_request;
    if (hosted) {
        const bool installed = [] {
            std::error_code error;
            const auto executable = fs::canonical("/proc/self/exe", error);
            return !error &&
                   executable.parent_path().parent_path().parent_path().filename() == "releases";
        }();
        shell = std::make_unique<ShellConnection>(
            app.options().hosted_fd, FRAMEKEYBOARD_VERSION, app.options().hosted_session,
            [&app, installed] {
                std::vector<std::string> features{"pointer", "sink", "render"};
                const auto state = app.host_state();
                if (json_object_get_boolean(json_object_object_get(state.get(), "swipe"))) {
                    features.emplace_back("swipe");
                }
                features.emplace_back("dictation.server");
                if (app.dictation_model_ready()) {
                    features.emplace_back("dictation.device");
                }
                if (installed && app.updates()) {
                    features.emplace_back("update");
                }
                return features;
            });
        try {
            previews.emplace(VrInstance::runtime_directory());
        } catch (const std::exception& error) {
            std::cerr << "Preview renders unavailable: " << error.what() << '\n';
        }
        std::cout << "Full Keyboard is hosted by a shell; session " << app.options().hosted_session
                  << ".\n"
                  << std::flush;
    }
    auto shell_hide = [&](const char* reason) {
        hide_reason = reason;
        shell_shown = false;
        lifecycle.set_shell_visible(false);
        hide_panel();
    };
    // Pulses the controller in `hand`, or tells a shell that plays its own.
    auto forwarded_haptic = [&](std::size_t hand, unsigned pointer, KeyFeedback kind) {
        if (shell_haptics) {
            auto fields = json_ptr(json_object_new_object());
            json_object_object_add(fields.get(), "hand",
                                   json_object_new_string(hand == 0 ? "left" : "right"));
            json_object_object_add(fields.get(), "kind",
                                   json_object_new_string(kind == KeyFeedback::Press     ? "press"
                                                          : kind == KeyFeedback::Release ? "release"
                                                                                         : "hover"));
            shell->event("haptic", std::move(fields));
            return;
        }
        const auto device = vr_system()->GetTrackedDeviceIndexForControllerRole(
            hand == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        if (device == vr::k_unTrackedDeviceIndexInvalid) {
            return;
        }
        if (kind == KeyFeedback::Press) {
            feedback.press(pointer, device);
        } else if (kind == KeyFeedback::Release) {
            feedback.release(pointer);
        } else {
            feedback.hover(device);
        }
    };
    // Forwarded pointers use their own IDs, apart from SteamVR's cursors.
    auto forwarded_pointer = [](std::size_t hand) { return 200u + static_cast<unsigned>(hand); };
    auto end_forwarded = [&] {
        for (std::size_t hand = 0; hand < 2; ++hand) {
            app.cancel_pointer(forwarded_pointer(hand)); // a cancel never releases into a press
            feedback.cancel_pointer(forwarded_pointer(hand));
            forwarded_inside[hand] = false;
        }
    };
    // The texture areas that take a pointer: the case, an open menu and the
    // flick preview, which can reach past the case.
    auto pointer_areas = [&](const PanelView& view) {
        std::vector<Rect> areas{panel_case_bounds(view)};
        if (view.popup) {
            areas.push_back(*view.popup);
        }
        if (const auto preview = flick_preview_bounds(view)) {
            areas.push_back(*preview);
        }
        return areas;
    };
    auto forward = [&](json_object* body, double now) {
        json_object* value = nullptr;
        const std::string hand_name = json_object_object_get_ex(body, "hand", &value) &&
                                              json_object_is_type(value, json_type_string)
                                          ? json_object_get_string(value)
                                          : "";
        const std::string phase = json_object_object_get_ex(body, "phase", &value) &&
                                          json_object_is_type(value, json_type_string)
                                      ? json_object_get_string(value)
                                      : "";
        if ((hand_name != "left" && hand_name != "right") || !app.forwarded() || !lifecycle.visible()) {
            return;
        }
        const std::size_t hand = hand_name == "left" ? 0 : 1;
        const auto pointer = forwarded_pointer(hand);
        auto number = [&](const char* key) {
            json_object* field = nullptr;
            return json_object_object_get_ex(body, key, &field) ? json_object_get_double(field) : 0.0;
        };
        if (phase == "cancel") {
            app.cancel_pointer(pointer);
            feedback.cancel_pointer(pointer);
            forwarded_inside[hand] = false;
            return;
        }
        if (phase == "scroll") {
            if (forwarded_inside[hand]) {
                app.scroll(pointer, number("dx"), number("dy"));
            }
            return;
        }
        const auto [x, y] = uv_to_panel(number("u"), number("v"));
        const auto areas = pointer_areas(app.input_view());
        const bool inside =
            std::any_of(areas.begin(), areas.end(), [&](const Rect& r) { return r.contains(x, y); });
        if (!inside) {
            if (std::exchange(forwarded_inside[hand], false)) {
                // Leaving the accepted areas cancels the press, as focus leave does.
                app.cancel_pointer(pointer);
                feedback.cancel_pointer(pointer);
            }
            return;
        }
        forwarded_inside[hand] = true;
        if (phase == "move") {
            // Moves during a press are kept for swipe, up to 8 per hand a frame.
            if (app.pointer_pressed(pointer) && ++forwarded_moves[hand] > 8) {
                return;
            }
            if (app.move(pointer, x, y)) {
                forwarded_haptic(hand, pointer, KeyFeedback::Hover);
            }
        } else if (phase == "down") {
            app.move(pointer, x, y);
            if (app.down(pointer, x, y, now)) {
                forwarded_haptic(hand, pointer, KeyFeedback::Press);
            }
        } else if (phase == "up") {
            if (app.up(pointer, x, y)) {
                forwarded_haptic(hand, pointer, KeyFeedback::Release);
            }
        }
    };
    auto read_hints = [](json_object* body, App::HostHints& hints) {
        json_object* object = nullptr;
        if (!json_object_object_get_ex(body, "hints", &object) || !object) {
            return true;
        }
        if (!json_object_is_type(object, json_type_object)) {
            return false;
        }
        for (const auto& [key, field] : {std::pair{"purpose", &hints.purpose},
                                         {"enter", &hints.enter},
                                         {"autocapitalize", &hints.autocapitalize},
                                         {"language", &hints.language}}) {
            json_object* value = nullptr;
            if (json_object_object_get_ex(object, key, &value)) {
                if (!json_object_is_type(value, json_type_string)) {
                    return false;
                }
                *field = json_object_get_string(value);
            }
        }
        return true;
    };
    // `context` in a request: absent, null or a string of at most 64 bytes.
    auto read_context = [](json_object* body, std::optional<std::optional<std::string>>& context) {
        json_object* value = nullptr;
        if (!json_object_object_get_ex(body, "context", &value)) {
            return true;
        }
        if (!value) {
            context = std::optional<std::string>{};
            return true;
        }
        if (!json_object_is_type(value, json_type_string) || json_object_get_string_len(value) > 64) {
            return false;
        }
        context = std::optional<std::string>{json_object_get_string(value)};
        return true;
    };
    auto shell_state = [&] {
        auto state = app.host_state();
        json_object_object_add(state.get(), "visible", json_object_new_boolean(lifecycle.visible()));
        json_object_object_add(state.get(), "reason", json_object_new_string(hide_reason.c_str()));
        json_object_object_add(state.get(), "pointer_mode",
                               json_object_new_string(app.forwarded() ? "forwarded" : "native"));
        if (displayed.ready()) {
            auto* geometry = json_object_new_object();
            json_object_object_add(geometry, "width", json_object_new_double(displayed.width()));
            json_object_object_add(geometry, "matrix", matrix_json(displayed.transform()));
            json_object_object_add(state.get(), "geometry", geometry);
        }
        if (auto* updates = app.updates()) {
            auto* update = json_object_new_object();
            json_object_object_add(update, "state",
                                   json_object_new_string(update_state_name(updates->state())));
            json_object_object_add(state.get(), "update", update);
        }
        return state;
    };
    auto handle_request = [&](ShellRequest& request, double now) {
        json_object* body = request.body.get();
        const auto id = request.id.value_or(-1);
        auto get = [&](const char* key) -> json_object* {
            json_object* value = nullptr;
            return json_object_object_get_ex(body, key, &value) ? value : nullptr;
        };
        auto text = [&](const char* key) {
            auto* value = get(key);
            return value && json_object_is_type(value, json_type_string)
                       ? std::string(json_object_get_string(value))
                       : std::string{};
        };
        auto ok = [&](JsonPtr fields = json_ptr(nullptr)) { shell->respond(id, std::move(fields)); };
        auto fail = [&](const std::string& code, const std::string& message) {
            shell->fail(id, code, message);
        };
        const auto& op = request.op;
        auto* router = app.options().shell_router;
        if (op == "pointer") {
            forward(body, now);
        } else if (op == "ack" || op == "reject") {
            auto* seq = get("seq");
            if (router && seq && json_object_is_type(seq, json_type_int)) {
                const auto number = static_cast<std::uint64_t>(json_object_get_int64(seq));
                op == "ack" ? router->acknowledge(text("context"), number)
                            : router->reject(text("context"), number);
            }
        } else if (op == "quiesced") {
            if (handover.asking() && app.update_waiting()) {
                // Everything before this has been answered; nothing after it is read.
                auto fields = json_ptr(json_object_new_object());
                json_object_object_add(fields.get(), "state", json_object_new_string("restarting"));
                shell->event("update", std::move(fields));
                shell->stop_reading();
                hosted_handover_fd = shell->release();
                handover.end();
                app.activate_update();
            }
        } else if (op == "state") {
            ok(shell_state());
        } else if (op == "show" || op == "context") {
            std::optional<std::optional<std::string>> context;
            App::HostHints hints;
            if (!read_context(body, context) || !read_hints(body, hints)) {
                fail("bad_request", "Invalid context or hints.");
                return;
            }
            if (const auto [code, field] = app.check_hints(hints); !code.empty()) {
                fail(code, field);
                return;
            }
            if (op == "context" && !context) {
                fail("bad_request", "context is required.");
                return;
            }
            if (context) {
                app.set_context(*context, hints);
            }
            if (op == "show") {
                const auto view = text("view");
                if (const auto error = app.show_view(view.empty() ? "keyboard" : view); !error.empty()) {
                    fail(error, error == "composing" ? "The view would commit unfinished input."
                                                     : "That view is not available now.");
                    return;
                }
                hide_reason = "shell";
                shell_shown = true;
                lifecycle.set_shell_visible(true);
            }
            ok();
        } else if (op == "hide") {
            shell_hide("shell");
            if (auto* end = get("end_context"); end && json_object_get_boolean(end)) {
                app.set_context(std::nullopt);
            }
            ok();
        } else if (op == "recenter") {
            anchor.reset();
            stop_drag();
            saved.reset();
            recenter_pending = true;
            geometry_source = "recenter";
            lifecycle.request_recenter();
            ok();
        } else if (op == "set_width") {
            auto* meters = get("meters");
            const double width = meters ? json_object_get_double(meters) : 0;
            if (!meters ||
                !(width >= PanelPlacement::minimum_width && width <= PanelPlacement::maximum_width)) {
                fail("bad_request", "meters must be 0.45 to 2.0.");
                return;
            }
            stop_drag();
            placement.set_width(width);
            geometry_source = "shell";
            transform_dirty = true;
            ok();
        } else if (op == "set_pose") {
            const auto matrix = json_matrix(get("matrix"));
            if (!matrix) {
                fail("bad_request", "matrix must be 3x4, row-major.");
                return;
            }
            stop_drag();
            anchor.reset();
            auto* level = get("level");
            shell_level = !level || json_object_get_boolean(level);
            placement.set_transform(*matrix);
            horizon.reset();
            room_facing.reset();
            recenter_pending = false;
            saved.reset();
            geometry_source = "shell";
            transform_dirty = true;
            ok();
        } else if (op == "anchor") {
            auto* overlay = get("overlay");
            if (!overlay) {
                anchor.reset();
                ok();
                return;
            }
            const auto matrix = json_matrix(get("matrix"));
            if (!json_object_is_type(overlay, json_type_string) || !matrix) {
                fail("bad_request", "anchor needs an overlay key and a 3x4 matrix.");
                return;
            }
            stop_drag();
            anchor = std::pair{std::string(json_object_get_string(overlay)), *matrix};
            recenter_pending = false;
            saved.reset();
            geometry_source = "shell";
            ok();
        } else if (op == "lock") {
            auto flag = [&](const char* key, bool current) {
                auto* value = get(key);
                return value ? static_cast<bool>(json_object_get_boolean(value)) : current;
            };
            app.set_locks(flag("move", app.locked_move()), flag("resize", app.locked_resize()));
            if (drag.active() && !app.allows_movement(*drag.source())) {
                stop_drag();
            }
            ok();
        } else if (op == "chrome") {
            App::HostChrome chrome;
            for (const auto& [key, field] : {std::pair{"close", &chrome.close},
                                             {"settings", &chrome.settings},
                                             {"move", &chrome.move},
                                             {"resize", &chrome.resize},
                                             {"favorites", &chrome.favorites}}) {
                if (auto* value = get(key)) {
                    *field = json_object_get_boolean(value);
                }
            }
            app.set_chrome(chrome);
            ok();
        } else if (op == "pointer_mode") {
            const auto mode = text("mode");
            if (mode != "forwarded" && mode != "native") {
                fail("bad_request", "mode must be forwarded or native.");
                return;
            }
            const auto haptics_owner = text("haptics");
            shell_haptics = haptics_owner == "shell";
            // Every switch releases keys and clears pointers.
            stop_drag();
            end_forwarded();
            hovered_devices.fill(false);
            pointer_devices.clear();
            scene_pointer.reset();
            scene_inside.fill(false);
            cursors.hide();
            resizers.hide();
            app.set_forwarded(mode == "forwarded");
            reported_regions.clear();
            ok();
        } else if (op == "sink") {
            auto* on = get("on");
            if (!on || !json_object_is_type(on, json_type_boolean)) {
                fail("bad_request", "on must be a boolean.");
                return;
            }
            app.set_sink(json_object_get_boolean(on), shell->client());
            ok();
        } else if (op == "theme.select") {
            if (const auto error = app.host_theme_select(text("id"), id); !error.empty()) {
                fail(error, "Unknown theme.");
                return;
            }
            ok();
        } else if (op == "theme.set") {
            auto* theme = get("theme");
            if (!theme || !json_object_is_type(theme, json_type_object)) {
                fail("invalid_theme", "theme must be an object.");
                return;
            }
            const auto [code, message] =
                app.host_theme_set(json_object_to_json_string_ext(theme, JSON_C_TO_STRING_PLAIN));
            if (!code.empty()) {
                fail(code, message);
                return;
            }
            ok();
        } else if (op == "theme.reset") {
            app.host_theme_reset();
            ok();
        } else if (op == "settings.get") {
            auto fields = json_ptr(json_object_new_object());
            json_object_object_add(fields.get(), "values", json_object_get(app.host_settings().get()));
            ok(std::move(fields));
        } else if (op == "settings.set") {
            const auto [code, key] = app.host_settings_set(get("values"), id);
            if (!code.empty()) {
                fail(code, key);
                return;
            }
            ok();
        } else if (op == "select") {
            if (const auto error =
                    app.host_select(text("layout"), text("language"), text("favorite"), id);
                !error.empty()) {
                fail(error, "That layout and language cannot be selected together.");
                return;
            }
            ok();
        } else if (op == "reload") {
            try {
                app.reload();
                ok();
            } catch (const std::exception& error) {
                fail("failed", error.what());
            }
        } else if (op == "profiles.list") {
            ok(app.host_profiles());
        } else if (op.starts_with("dictation.")) {
            auto [code, fields] = app.host_dictation_request(op, body);
            if (!code.empty()) {
                fail(code, code == "composing" ? "Dictation would commit unfinished input."
                                               : "Dictation cannot do that now.");
                return;
            }
            ok(std::move(fields));
        } else if (op == "update.check" || op == "update.install" || op == "update.cancel") {
            auto* updates = app.updates();
            if (!updates) {
                fail("unavailable", "Updates need an installed release.");
                return;
            }
            if (op == "update.check") {
                if (check_request || !updates->check(now)) {
                    fail("busy", "An update is already in progress.");
                    return;
                }
                check_request = id; // answered with the result
                return;
            } else if (op == "update.install") {
                const auto when = text("when");
                if (!when.empty() && when != "idle" && when != "now") {
                    fail("bad_request", "when must be idle or now.");
                    return;
                }
                update_when = when.empty() ? "idle" : when;
                if (!updates->install(now, text("version"), shell_protocol_version)) {
                    fail("busy", "An update is already in progress.");
                    return;
                }
            } else {
                handover.end();
                if (!updates->cancel(now)) {
                    fail("unavailable", "No update is waiting to be activated.");
                    return;
                }
            }
            ok();
        } else if (op == "render") {
            if (!previews) {
                fail("unavailable", "Preview renders are unavailable.");
                return;
            }
            PreviewRenderer::Request render;
            render.id = id;
            render.view = text("view").empty() ? "keyboard" : text("view");
            const auto& selection = app.selection();
            render.layout = text("layout").empty() ? selection.layout : text("layout");
            render.language = text("language").empty() ? selection.language : text("language");
            render.theme = text("theme").empty() ? selection.theme : text("theme");
            render.data_dir = app.options().data_dir;
            if (render.theme == "shell" || (render.view != "keyboard" && render.view != "settings" &&
                                            render.view != "emoji" && render.view != "dictation")) {
                fail("bad_request", "Render an installed theme and a known view.");
                return;
            }
            if (preview_request || !previews->start(render)) {
                fail("busy", "Another preview is being rendered.");
                return;
            }
            preview_request = id; // answered when the worker finishes
        } else {
            fail("unknown_op", "Unknown operation.");
        }
    };
    auto dismiss_panel = [&] {
        if (app.take_dismissal()) {
            if (hosted) {
                // X hides; the shell decides whether to show the panel again.
                app.take_user_hide();
                hide_reason = "user";
                shell_shown = false;
                lifecycle.set_shell_visible(false);
                hide_panel();
                return;
            }
            lifecycle.dismiss();
        }
    };
    while (!done && !app.quitting() && !interrupted) {
        const auto frame_started = std::chrono::steady_clock::now();
        const double now = monotonic_seconds();
        laser_rays.begin_frame(now);
        if (duration > 0 && now - start >= duration) {
            break;
        }
        forwarded_moves.fill(0);
        if (shell) {
            // An update successor reports `ready` only once the update is committed.
            if (!ready_sent && update_committed()) {
                shell->ready(now);
                ready_sent = true;
            }
            for (auto& request : shell->poll(now)) {
                handle_request(request, now);
                if (shell->fd() < 0 || hosted_handover_fd >= 0) {
                    break; // handed over: nothing after `quiesced` is read
                }
            }
            if (hosted_handover_fd < 0) {
                switch (shell->status(now)) {
                case ShellConnection::Status::Open:
                    break;
                case ShellConnection::Status::Closed:
                case ShellConnection::Status::Violated:
                    // The shell's connection is the session: it ends the keyboard.
                    std::cout << "The shell closed its connection.\n" << std::flush;
                    app.request_quit();
                    break;
                case ShellConnection::Status::NoHello:
                case ShellConnection::Status::LeaseGone:
                    std::cout << "The shell stopped answering; exiting.\n" << std::flush;
                    exit_status = hosted_exit::shell_gone;
                    app.request_quit();
                    break;
                }
                if (shell->take_lease_lapsed(now)) {
                    // Hung, perhaps only stalled: input stops, host state stays.
                    std::cout << "The shell's lease expired.\n" << std::flush;
                    shell_hide("lease");
                    end_forwarded();
                    app.set_context(std::nullopt);
                    app.cancel();
                }
            }
            if (app.quitting()) {
                break;
            }
        }
        replacement.poll(now);
        if (replacement.take_unsupported_control()) {
            app.report_status("Control unavailable in Steam fields. Use Steam keyboard for shortcuts.");
        }
        const auto native = replacement.state();
        app.set_replacement_available(native.available, replacement.unavailable_reason());
        app.set_native_session(native.open);
        // Before the lifecycle routes input to a new request's owner.
        app.set_native_request(native.open ? native.request : 0);
        const bool replace = app.replace_native_keyboard();
        // Wake the watchdog before this frame can start the helper that reads
        // its health. A hosted panel is watched too.
        watchdog_schedule.set_enabled(replace || hosted);
        const auto instance_request = instance.poll();
        if (instance_request == VrInstance::Request::Quit) {
            // Another launch takes over, as Settings → Quit would end this one.
            std::cout << "Quit requested by another launch.\n" << std::flush;
            app.request_quit();
            break;
        }
        lifecycle.begin_frame({.replacement_enabled = replace,
                               .native = native,
                               .routed_remote = replacement.remote(),
                               .watchdog_stalled = replacement_stalled.exchange(false),
                               .relaunch = instance_request == VrInstance::Request::Recenter,
                               .dictation_hold = app.dictation_holds_panel(),
                               .now = now});
        app.set_field_needed(lifecycle.held_without_field());
        const bool dashboard_visible = vr_overlay()->IsDashboardVisible();
        if (!hosted && passthrough_requests.take(replace, native, dashboard_visible)) {
            std::cout << "Passthrough turned on for a request outside the dashboard.\n" << std::flush;
            app.set_passthrough(true);
        }
        if (!hosted && app.dashboard_visibility(dashboard_visible)) {
            std::cout << "Passthrough turned on to keep dictation listening.\n" << std::flush;
        }
        // Under a shell the panel shows where the shell decides, dashboard or
        // not; outside the dashboard the keyboard reads the controllers itself,
        // unless the shell forwards its own pointer.
        const bool panel_presentable = hosted || dashboard_visible || app.passthrough();
        lifecycle.dashboard_visibility(panel_presentable);
        const bool scene =
            hosted ? !dashboard_visible && !app.forwarded() : app.passthrough() && !dashboard_visible;
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
            if (LaserRays::invalidates(event.eventType)) {
                laser_rays.forget();
                sticks.forget();
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
                hovered_devices[device] = panel_case_bounds(app.geometry()).contains(x, y);
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
            const auto view = app.input_view();
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
                if (controller) {
                    last_pressed = device;
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
            resizers.hide(1 - side);
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
                    app.quitting() || app.forwarded()) {
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
            // A forwarding shell owns every pointer; SteamVR's laser is off.
            if (!lifecycle.visible() || scene_input || !vr_overlay()->IsDashboardVisible() ||
                app.forwarded()) {
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
                    const double dx = -event.data.scroll.xdelta, dy = -event.data.scroll.ydelta;
                    // Learn which way this stick scrolls, as App::scroll reads the step.
                    // And how fast SteamVR repeats steps, for passthrough to match.
                    if (const auto stick = sticks.read(device, now)) {
                        stick_direction.learn(stick->first, stick->second,
                                              std::abs(dx) > std::abs(dy) ? dx : dy);
                        const double tilt = std::max(std::abs(stick->first), std::abs(stick->second));
                        if (stick_rate.step(now, tilt)) {
                            std::cout << "Dashboard stick scrolls " << stick_rate.rate()
                                      << " steps a second at full tilt.\n"
                                      << std::flush;
                        }
                    }
                    app.scroll(event.data.scroll.cursorIndex, dx, dy);
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
                lifecycle.pointer_left(device, drag.active() || keeps_held_key(device, now, false));
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
            const auto view = app.input_view();
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
            for (const auto& sample : samples) {
                if (sample.ray && !sample.lost) {
                    laser_rays.share(sample.device, *sample.ray);
                }
            }
            // A shown resizer, in front of the keyboard, takes the ray first, as
            // its overlay does SteamVR's laser in the dashboard. Its bend can
            // lie inside the case's sharp corner, outside the rounded case.
            std::array<std::optional<std::pair<std::size_t, Vector3>>, 2> on_resizer{};
            for (std::size_t hand = 0; hand < samples.size(); ++hand) {
                for (std::size_t side = 0; samples[hand].ray && side < 2 && !on_resizer[hand]; ++side) {
                    if (!resizers.shown(side)) {
                        continue;
                    }
                    if (const auto point = resizer_hit(
                            displayed.transform(), case_corner(view, displayed.width(), side == 1, true),
                            corner_of(side), resizer_shape(view, displayed.width()),
                            *samples[hand].ray)) {
                        on_resizer[hand] = std::pair{side, *point};
                    }
                }
            }
            // Where each ray meets the panel's accepted areas, if it does.
            std::array<std::optional<std::pair<double, double>>, 2> inside{};
            for (std::size_t hand = 0; hand < samples.size(); ++hand) {
                if (const auto& hit = samples[hand].hit; hit && !on_resizer[hand]) {
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
                    // The ray may rest on a shown corner resizer, found above.
                    if (const auto& resizer = on_resizer[hand]) {
                        PanelHit hit;
                        hit.point = resizer->second;
                        cursors.show(hand, displayed.transform(), hit);
                        if (sample.pressed) {
                            start_resize(resizer->first, sample.device, pointer);
                        }
                    } else {
                        cursors.hide(hand);
                    }
                    if (std::exchange(scene_inside[hand], false)) {
                        // Leaving the panel ends this hand's presses, as SteamVR's FocusLeave does.
                        if (sample.device < hovered_devices.size()) {
                            hovered_devices[sample.device] = false;
                        }
                        lifecycle.pointer_left(
                            sample.device, drag.active() || keeps_held_key(sample.device, now, true));
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
            // The stick scrolls the emoji grid or the dictation editor under its hand's ray: outside the
            // dashboard SteamVR sends no scroll events. It scrolls at the rate
            // measured from the dashboard's steps, two columns a step.
            const double stick_seconds = std::clamp(now - std::exchange(stick_scrolled, now), 0.0, .1);
            for (std::size_t hand = 0;
                 (app.emoji_open() || app.dictation_open()) && !drag.active() && hand < samples.size();
                 ++hand) {
                // Read the stick only while its ray is over something that scrolls.
                const auto stick =
                    inside[hand] && !samples[hand].lost && app.scroll_room(ScenePointer::pointer(hand))
                        ? scene_pointer.stick(hand)
                        : std::nullopt;
                if (!stick) {
                    continue;
                }
                // As fast as SteamVR's own steps in the dashboard, continuously.
                const double x = stick_rate.steps_per_second(stick->first);
                const double y = stick_rate.steps_per_second(stick->second);
                if (x != 0 || y != 0) {
                    app.scroll(ScenePointer::pointer(hand), x * stick_seconds, -y * stick_seconds);
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
        for (const auto pointer :
             key_holds.expired(now, [&](unsigned held) { return app.holds_repeat(held); })) {
            app.cancel_pointer(pointer);
            feedback.cancel_pointer(pointer);
            pointer_devices.erase(pointer);
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
        // Listening never outlives the wearer (docs/dictation.md, Dashboard,
        // passthrough and focus). The headset enters standby seconds after it
        // is taken off. A system sleep shows as boot time, which counts
        // suspend, running ahead of the monotonic clock, which does not.
        const double suspended = suspended_seconds();
        if (app.listening() && suspended > last_suspended + 1) {
            app.stop_listening("Listening stopped: the headset was asleep.");
        }
        last_suspended = suspended;
        if (app.listening() && now >= next_presence_check) {
            next_presence_check = now + .25;
            if (vr_system()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd) ==
                vr::k_EDeviceActivityLevel_Standby) {
                app.stop_listening("Listening stopped: the headset was taken off.");
            }
        }
        // Felt in the hand that last pressed, which started listening.
        if (const bool listening = app.listening(); listening != was_listening) {
            was_listening = listening;
            if (last_pressed) {
                RuntimeWait runtime_wait;
                haptics.play(*last_pressed, listening_pulses(listening));
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
                // Hosted mode never follows the SteamOS dashboard; a recenter
                // uses the headset fallback.
                const auto dashboard =
                    hosted ? std::optional<Transform>{}
                           : dashboard_anchor.update(
                                 panel.read_dashboard("valve.steam.gamepadui.main", true),
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
        if (anchor && shell_shown) {
            // An anchored panel follows its overlay each frame, without repainting.
            vr::VROverlayHandle_t handle{};
            vr::HmdVector2_t scale{};
            vr::HmdMatrix34_t world{};
            const bool found =
                vr_overlay()->FindOverlay(anchor->first.c_str(), &handle) == vr::VROverlayError_None &&
                vr_overlay()->IsOverlayVisible(handle) &&
                vr_overlay()->GetOverlayMouseScale(handle, &scale) == vr::VROverlayError_None &&
                vr_overlay()->GetTransformForOverlayCoordinates(handle, vr::TrackingUniverseStanding,
                                                                {{scale.v[0] / 2, scale.v[1] / 2}},
                                                                &world) == vr::VROverlayError_None;
            if (!found) {
                std::cout << "The anchor overlay is gone; hiding the keyboard.\n" << std::flush;
                shell_hide("anchor_lost");
            } else {
                Transform parent{};
                for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t col = 0; col < 4; ++col) {
                        parent[row][col] = world.m[row][col];
                    }
                }
                const auto pose = compose(parent, anchor->second);
                if (!placement.ready() || pose != placement.transform()) {
                    placement.set_transform(pose);
                    transform_dirty = true;
                }
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
            // Case geometry only; the full view, with its controls and popup, is
            // built just when a resizer is wanted or shown.
            const auto view = app.geometry();
            const auto shape = resizer_shape(view, displayed.width());
            const auto corner = [&](std::size_t side) {
                return case_corner(view, displayed.width(), side == 1, true);
            };
            // Nearest laser to each bracket; none counts as infinitely far.
            std::array<double, 2> distance{INFINITY, INFINITY};
            std::array<double, 2> target{};
            const bool resizing = drag.active() && drag.source() == MoveSource::Resize;
            if (resizing) {
                target[resize_side] = 1;
            } else if (!drag.active() && app.allows_movement(MoveSource::Resize)) {
                const auto hands = laser_rays.hands();
                // The dashboard pointer changes only on a deliberate switch; 4 Hz suffices.
                if (!scene_input && now - dashboard_pointer_checked >= .25) {
                    dashboard_pointer = vr_overlay()->GetPrimaryDashboardDevice();
                    dashboard_pointer_checked = now;
                }
                const std::array<vr::TrackedDeviceIndex_t, 3> lasers{
                    hands[0], hands[1],
                    !scene_input && dashboard_pointer == vr::k_unTrackedDeviceIndex_Hmd
                        ? dashboard_pointer
                        : vr::k_unTrackedDeviceIndexInvalid};
                for (const auto device : lasers) {
                    const auto ray = laser_rays.read(device);
                    for (std::size_t side = 0; ray && side < 2; ++side) {
                        const auto bend =
                            resizer_target(displayed.transform(), corner(side), corner_of(side), shape);
                        distance[side] = std::min(distance[side], ray_distance(*ray, bend));
                    }
                }
            }
            for (std::size_t side = 0; side < 2; ++side) {
                if (resizer_reach[side].update(distance[side])) {
                    target[side] = 1;
                }
            }
            std::optional<PanelView> full;
            for (std::size_t side = 0; side < 2; ++side) {
                if ((target[side] > 0 || resizers.visible(side)) && !full) {
                    full = app.input_view();
                }
                if (full && resizer_covers_popup(*full, displayed.width(), side, shape)) {
                    resizers.hide(side);
                    continue;
                }
                resizers.update(side, resizer_transform(displayed.transform(), corner(side)), shape,
                                target[side], now, resizing && side == resize_side);
            }
        }
        // Repeat and text commits share the same gate as pointer presses.
        if (!presentable()) {
            hide_panel();
        }
        const bool repaint = app.tick(now);
        // Ask for SteamVR's ticking scroll steps only while some laser is over
        // something its stick would actually scroll: not at an end it leans toward.
        // The stick matters only at an end: elsewhere both directions move.
        bool scroll_steps = false;
        for (const auto& [pointer, device] : pointer_devices) {
            const auto room = app.scroll_room(pointer);
            int leaning = 0;
            if (room && room->back != room->forward) {
                if (const auto stick = sticks.read(device, now)) {
                    leaning =
                        stick_direction.predict(stick->first, stick->second, stick_leaning[pointer]);
                }
            }
            stick_leaning[pointer] = leaning;
            scroll_steps |= room && wants_scroll_steps(*room, leaning);
        }
        std::erase_if(stick_leaning,
                      [&](const auto& item) { return !pointer_devices.contains(item.first); });
        panel.accept_scroll(scroll_steps);
        // A state change that redraws no pixel, like a sub-pixel glide step or
        // an image drawn off-screen, uploads nothing: each upload of the whole
        // texture costs milliseconds.
        if (lifecycle.visible() && repaint && app.paint(now)) {
            panel.submit(app.renderer, app.view());
        }
        bool fallback = app.take_native_fallback();
        if (replacement_stalled.exchange(false)) {
            fallback = true;
        }
        if (fallback) {
            // Steam's own keyboard takes over at once; ours must not linger over it.
            cut_exit = true;
            lifecycle.fallback();
            cut_exit = false;
            if (motion.leaving()) {
                end_exit();
            }
            if (hosted) {
                // The stall watchdog hid the panel; the shell shows it again.
                hide_reason = "stalled";
                shell_shown = false;
                lifecycle.set_shell_visible(false);
            }
        }
        if (motion.gone(now)) {
            end_exit();
        } else if (motion.animating(now)) {
            vr_overlay()->SetOverlayAlpha(panel.handle(), static_cast<float>(motion.alpha(now)));
            if (displayed.ready()) {
                panel.place(displayed, motion.offset(now));
            }
        }
        if (shell && hosted_handover_fd < 0) {
            if (lifecycle.visible() != was_visible) {
                was_visible = lifecycle.visible();
                auto fields = json_ptr(json_object_new_object());
                json_object_object_add(fields.get(), "visible", json_object_new_boolean(was_visible));
                json_object_object_add(fields.get(), "view",
                                       json_object_new_string(app.view_name().c_str()));
                // Hidden while the shell still wants it shown: tracking was lost.
                const char* reason = was_visible   ? "shell"
                                     : shell_shown ? "tracking"
                                                   : hide_reason.c_str();
                json_object_object_add(fields.get(), "reason", json_object_new_string(reason));
                shell->event("visibility", std::move(fields));
            }
            // Committed output for the sink, in order; never dropped.
            if (auto* router = app.options().shell_router) {
                for (const auto& sunk : router->take_events()) {
                    auto fields = json_ptr(json_object_new_object());
                    json_object_object_add(fields.get(), "seq",
                                           json_object_new_int64(static_cast<int64_t>(sunk.seq)));
                    json_object_object_add(fields.get(), "context",
                                           json_object_new_string(sunk.context.c_str()));
                    if (sunk.kind == ShellRouter::Event::Kind::Commit) {
                        json_object_object_add(fields.get(), "text",
                                               json_object_new_string(sunk.text.c_str()));
                        shell->event("commit", std::move(fields));
                    } else {
                        json_object_object_add(fields.get(), "key",
                                               json_object_new_string(sunk.key.c_str()));
                        auto* modifiers = json_object_new_array();
                        for (const auto& modifier : sunk.modifiers) {
                            json_object_array_add(modifiers, json_object_new_string(modifier.c_str()));
                        }
                        json_object_object_add(fields.get(), "modifiers", modifiers);
                        if (sunk.repeat) {
                            json_object_object_add(fields.get(), "repeat",
                                                   json_object_new_boolean(true));
                        }
                        shell->event("key", std::move(fields));
                    }
                }
            }
            for (auto& event : app.take_host_events()) {
                if (event.telemetry) {
                    shell->telemetry(event.name, std::move(event.fields));
                } else {
                    shell->event(event.name, std::move(event.fields));
                }
            }
            // Movement and size, at most ten times a second.
            if (displayed.ready() && now - geometry_at >= .1 &&
                (!reported_geometry ||
                 *reported_geometry != std::tuple{displayed.transform(), displayed.width()})) {
                reported_geometry = std::tuple{displayed.transform(), displayed.width()};
                geometry_at = now;
                const auto view = app.geometry();
                auto fields = json_ptr(json_object_new_object());
                json_object_object_add(fields.get(), "width", json_object_new_double(displayed.width()));
                json_object_object_add(
                    fields.get(), "height",
                    json_object_new_double(displayed.width() * texture_height / panel_width));
                json_object_object_add(fields.get(), "matrix", matrix_json(displayed.transform()));
                json_object_object_add(fields.get(), "keys_uv", rect_uv(panel_case_bounds(view)));
                json_object_object_add(
                    fields.get(), "source",
                    json_object_new_string(drag.active() ? "user" : geometry_source.c_str()));
                shell->telemetry("geometry", std::move(fields));
                geometry_source = "user";
            }
            // The areas a forwarded pointer may use, whenever they change.
            if (app.forwarded() && lifecycle.visible()) {
                auto* rects = json_object_new_array();
                for (const auto& area : pointer_areas(app.input_view())) {
                    json_object_array_add(rects, rect_uv(area));
                }
                const std::string text = json_object_to_json_string_ext(rects, JSON_C_TO_STRING_PLAIN);
                if (text != reported_regions) {
                    reported_regions = text;
                    auto fields = json_ptr(json_object_new_object());
                    json_object_object_add(fields.get(), "rects", rects);
                    shell->event("regions", std::move(fields));
                } else {
                    json_object_put(rects);
                }
            }
            if (auto* updates = app.updates()) {
                const auto state = updates->state();
                if (state != reported_update || (state == Updates::State::Error &&
                                                 updates->failure_code() != reported_update_error)) {
                    reported_update = state;
                    reported_update_error = updates->failure_code();
                    auto fields = json_ptr(json_object_new_object());
                    json_object_object_add(fields.get(), "state",
                                           json_object_new_string(update_state_name(state)));
                    if (!updates->latest().empty()) {
                        json_object_object_add(fields.get(), "latest",
                                               json_object_new_string(updates->latest().c_str()));
                    }
                    if (state == Updates::State::Error) {
                        json_object_object_add(
                            fields.get(), "error",
                            json_object_new_string(update_error_code(updates->failure_code()).c_str()));
                    }
                    shell->event("update", std::move(fields));
                    if (check_request && state != Updates::State::Checking) {
                        if (state == Updates::State::Error) {
                            shell->fail(*check_request, update_error_code(updates->failure_code()),
                                        "The update check failed.");
                        } else {
                            auto result = json_ptr(json_object_new_object());
                            // Up to date: the newest stable release is this one.
                            json_object_object_add(
                                result.get(), "latest",
                                json_object_new_string(updates->latest().empty()
                                                           ? FRAMEKEYBOARD_VERSION
                                                           : updates->latest().c_str()));
                            json_object_object_add(
                                result.get(), "available",
                                json_object_new_boolean(state == Updates::State::Available));
                            shell->respond(*check_request, std::move(result));
                        }
                        check_request.reset();
                    }
                }
                // Activation waits for idleness, checked just before it, and
                // for the shell's last message (docs/integration.md, Handover).
                if (handover.ask(
                        now, app.update_waiting() &&
                                 (update_when == "now" || (!lifecycle.visible() && !motion.leaving() &&
                                                           app.idle_for_update())))) {
                    auto fields = json_ptr(json_object_new_object());
                    json_object_object_add(fields.get(), "state", json_object_new_string("quiesce"));
                    shell->event("update", std::move(fields));
                }
                if (handover.expired(now)) {
                    // No `quiesced` in time: postponed, back to prepared.
                    auto fields = json_ptr(json_object_new_object());
                    json_object_object_add(fields.get(), "state", json_object_new_string("prepared"));
                    shell->event("update", std::move(fields));
                }
            }
            if (previews && preview_request) {
                if (auto rendered = previews->take()) {
                    if (rendered->error.empty()) {
                        auto fields = json_ptr(json_object_new_object());
                        json_object_object_add(fields.get(), "path",
                                               json_object_new_string(rendered->path.c_str()));
                        shell->respond(*preview_request, std::move(fields));
                    } else {
                        shell->fail(*preview_request, "failed", "The preview could not be rendered.");
                    }
                    preview_request.reset();
                }
            }
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
        replacement_watched.store((replace || hosted) && lifecycle.visible());
        if (shell) {
            shell->frame_completed(completed); // pings are answered only here
        }
        // Follow the dashboard every visible frame, even without a laser over
        // the keys. Budget work inside the 60 Hz period instead of adding a full
        // sleep after it. Hidden panels still idle at 4 Hz; paints are event-driven.
        const bool smooth = lifecycle.visible() || motion.leaving();
        const auto period = std::chrono::microseconds(smooth ? 16667 : 250000);
        if (smooth) {
            std::this_thread::sleep_until(frame_started + period);
        } else if (shell && shell->fd() >= 0) {
            // Hidden under a shell: a request wakes the loop at once.
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                frame_started + period - std::chrono::steady_clock::now());
            pollfd descriptor{shell->fd(), POLLIN, 0};
            ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
        } else {
            replacement.wait_until(frame_started + period);
        }
    }
    cut_exit = true;
    hide_panel();
    if (motion.leaving()) {
        end_exit();
    }
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
    return exit_status;
}
} // namespace framekeyboard
