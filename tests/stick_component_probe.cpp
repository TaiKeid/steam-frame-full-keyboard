#include "framekeyboard/grip.hpp"
#include "openvr.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
framekeyboard::Transform convert(const vr::HmdMatrix34_t& matrix) {
    framekeyboard::Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = matrix.m[r][c];
        }
    }
    return result;
}
framekeyboard::Transform component(const char* model, float x, float y,
                                   const char* name = "thumbstick") {
    vr::VRControllerState_t input{};
    input.rAxis[0].x = x;
    input.rAxis[0].y = y;
    if (std::string_view(name) == "trigger") {
        input.rAxis[1].x = x;
    }
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    if (!vr::VRRenderModels()->GetComponentState(model, name, &input, &mode, &state)) {
        throw std::runtime_error("render component unavailable");
    }
    return convert(state.mTrackingToComponentRenderModel);
}
constexpr std::array<const char*, 2> frame_models{"{frame_controller}frame_controller_left",
                                                  "{frame_controller}frame_controller_right"};
// How a component's pose differs from a reference: its rotation in degrees
// (component_rotation_degrees ignores translation) and its translation in
// millimeters, in the controller's tracking space.
struct PoseChange {
    double degrees{};
    std::array<double, 3> millimeters{};
    bool moved() const {
        return degrees > 1e-3 || std::abs(millimeters[0]) > 1e-3 || std::abs(millimeters[1]) > 1e-3 ||
               std::abs(millimeters[2]) > 1e-3;
    }
};
PoseChange change(const framekeyboard::Transform& reference, const framekeyboard::Transform& current) {
    PoseChange result;
    result.degrees = framekeyboard::component_rotation_degrees(reference, current);
    for (std::size_t r = 0; r < 3; ++r) {
        result.millimeters[r] = (current[r][3] - reference[r][3]) * 1000;
    }
    return result;
}
std::ostream& operator<<(std::ostream& out, const PoseChange& value) {
    return out << "deg=" << value.degrees << " mm=" << value.millimeters[0] << ','
               << value.millimeters[1] << ',' << value.millimeters[2];
}
// The thumbstick's synthetic pose for a controller state.
framekeyboard::Transform stick_pose(const char* model, const vr::VRControllerState_t& input,
                                    std::uint32_t* properties = nullptr) {
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    if (!vr::VRRenderModels()->GetComponentState(model, "thumbstick", &input, &mode, &state)) {
        throw std::runtime_error("thumbstick component unavailable");
    }
    if (properties) {
        *properties = state.uProperties;
    }
    return convert(state.mTrackingToComponentRenderModel);
}
// Which controller state bits move the thumbstick in the model: each
// pressed and each touched bit alone against a state with none, so one run
// shows which one a stick click animates. Synthetic states only.
void stick_click_bits(const char* model, const char* hand) {
    const vr::VRControllerState_t rest{};
    const auto neutral = stick_pose(model, rest);
    int moving = 0;
    for (const bool touched : {false, true}) {
        for (std::uint32_t bit = 0; bit < 64; ++bit) {
            vr::VRControllerState_t input{};
            (touched ? input.ulButtonTouched : input.ulButtonPressed) = std::uint64_t{1} << bit;
            std::uint32_t properties = 0;
            const auto moved = change(neutral, stick_pose(model, input, &properties));
            if (moved.moved()) {
                ++moving;
                std::cout << hand << (touched ? " touched" : " pressed") << " bit=" << bit << ' '
                          << moved << " properties=" << properties << '\n';
            }
        }
    }
    std::cout << hand << " bits that move the thumbstick=" << moving << '\n';
}
// One hand's live thumbstick, read from its render component as the
// keyboard reads the grip and bumper: what SteamVR animates, which the
// dashboard doesn't mask. Tilt is printed too, to tell it from a click.
struct LiveStick {
    bool ready{};
    std::string model;
    vr::VRInputValueHandle_t source{};
    framekeyboard::Transform neutral{};
    framekeyboard::ComponentAxis x, y;
};
LiveStick live_stick(std::size_t hand) {
    LiveStick stick;
    const auto role =
        hand == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
    const auto device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
    char model[1024]{};
    vr::ETrackedPropertyError error{};
    vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                   sizeof(model), &error);
    if (error || model != std::string_view(frame_models[hand]) ||
        vr::VRInput()->GetInputSourceHandle(hand == 0 ? "/user/hand/left" : "/user/hand/right",
                                            &stick.source) != vr::VRInputError_None) {
        return stick;
    }
    stick.model = model;
    const vr::VRControllerState_t rest{};
    vr::VRControllerState_t right{}, up{};
    right.rAxis[0].x = 1;
    up.rAxis[0].y = 1;
    stick.neutral = stick_pose(model, rest);
    stick.ready = stick.x.calibrate(stick.neutral, stick_pose(model, right)) &&
                  stick.y.calibrate(stick.neutral, stick_pose(model, up));
    return stick;
}
void print_live(const char* hand, const LiveStick& stick) {
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    if (!stick.ready || !vr::VRRenderModels()->GetComponentStateForDevicePath(
                            stick.model.c_str(), "thumbstick", stick.source, &mode, &state)) {
        std::cout << ' ' << hand << " unavailable";
        return;
    }
    const auto current = convert(state.mTrackingToComponentRenderModel);
    std::cout << ' ' << hand << ' ' << change(stick.neutral, current) << " x=" << stick.x.read(current)
              << " y=" << stick.y.read(current) << " properties=" << state.uProperties;
}
// --stick-click: for the shortcut bar's rename-in-place press. Reads only:
// nothing is sent to hardware, overlays or applications.
void stick_click_probe() {
    std::cout << std::fixed << std::setprecision(3);
    const std::array<const char*, 2> hands{"left", "right"};
    for (std::size_t i = 0; i < 2; ++i) {
        stick_click_bits(frame_models[i], hands[i]);
    }
    std::array<LiveStick, 2> sticks{live_stick(0), live_stick(1)};
    std::cout << "live: 30 s at 10 per second; click, tilt and rest each stick\n" << std::flush;
    const auto start = std::chrono::steady_clock::now();
    for (int sample = 0; sample < 300; ++sample) {
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        }
        const auto at = start + std::chrono::milliseconds(100 * sample);
        std::this_thread::sleep_until(at);
        std::cout << "t=" << sample / 10.0;
        for (std::size_t i = 0; i < 2; ++i) {
            print_live(hands[i], sticks[i]);
        }
        std::cout << '\n' << std::flush;
    }
}
// The default mode: the synthetic model mappings the keyboard relies on.
void model_checks() {
    // Synthetic states animate a model calculation only. No input is sent to
    // hardware, overlays or applications, and no physical presses are read.
    for (const auto* model :
         {"{frame_controller}frame_controller_left", "{frame_controller}frame_controller_right"}) {
        framekeyboard::ComponentAxis axis;
        if (!axis.calibrate(component(model, 0, 0), component(model, 0, 1))) {
            throw std::runtime_error("thumbstick calibration failed");
        }
        double maximum_error = 0;
        for (float x : {-1.f, -.5f, 0.f, .5f, 1.f}) {
            for (float y : {-1.f, -.5f, 0.f, .5f, 1.f}) {
                maximum_error = std::max(maximum_error, std::abs(axis.read(component(model, x, y)) - y));
            }
        }
        std::cout << model << " maximum axis error=" << maximum_error << '\n';
        if (maximum_error > .02) {
            throw std::runtime_error("thumbstick axis extraction failed");
        }
        const auto neutral = component(model, 0, 0, "trigger");
        maximum_error = 0;
        for (float pull : {0.f, .25f, .5f, 1.f}) {
            const auto angle =
                framekeyboard::component_rotation_degrees(neutral, component(model, pull, 0, "trigger"));
            maximum_error = std::max(maximum_error, std::abs(angle - 12.5 * pull));
        }
        std::cout << model << " maximum trigger angle error=" << maximum_error << '\n';
        if (maximum_error > .1) {
            throw std::runtime_error("trigger release fallback model mapping failed");
        }
        // The dashboard laser also clicks on the bumper, which the model
        // animates from controller button 0 through 2.6 degrees.
        vr::VRControllerState_t rest{}, pressed{};
        pressed.ulButtonPressed = vr::ButtonMaskFromId(vr::k_EButton_System);
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t bumper_rest{}, bumper_pressed{};
        if (!vr::VRRenderModels()->GetComponentState(model, "bumper", &rest, &mode, &bumper_rest) ||
            !vr::VRRenderModels()->GetComponentState(model, "bumper", &pressed, &mode,
                                                     &bumper_pressed)) {
            throw std::runtime_error("bumper component unavailable");
        }
        const double bumper = framekeyboard::component_rotation_degrees(
            convert(bumper_rest.mTrackingToComponentRenderModel),
            convert(bumper_pressed.mTrackingToComponentRenderModel));
        std::cout << model << " bumper press angle=" << bumper << '\n';
        if (std::abs(bumper - 2.6) > .1) {
            throw std::runtime_error("bumper release fallback model mapping failed");
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    const bool stick_click = argc == 2 && std::string_view(argv[1]) == "--stick-click";
    if (argc > 1 && !stick_click) {
        std::cerr << "usage: stick-component-probe [--stick-click]\n";
        return 2;
    }
    vr::EVRInitError error{};
    // Live component reads need SteamVR running; the model checks don't.
    vr::VR_Init(&error, stick_click ? vr::VRApplication_Background : vr::VRApplication_Utility);
    if (error) {
        return 1;
    }
    int result = 0;
    try {
        if (stick_click) {
            stick_click_probe();
        } else {
            model_checks();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    vr::VR_Shutdown();
    return result;
}
