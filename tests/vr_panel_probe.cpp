#include "openvr.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void wait_for_ui() {
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
}
void click(vr::VROverlayHandle_t panel, float x, float y) {
    vr::VREvent_t event{};
    event.data.mouse.x = x;
    event.data.mouse.y = 600 - y;
    event.data.mouse.button = vr::VRMouseButton_Left;
    event.eventType = vr::VREvent_MouseButtonDown;
    vr::VROverlayView()->PostOverlayEvent(panel, &event);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    event.eventType = vr::VREvent_MouseButtonUp;
    vr::VROverlayView()->PostOverlayEvent(panel, &event);
    wait_for_ui();
}
vr::HmdMatrix34_t transform(vr::VROverlayHandle_t panel) {
    vr::HmdMatrix34_t result{};
    vr::ETrackingUniverseOrigin origin{};
    require(vr::VROverlay()->GetOverlayTransformAbsolute(panel, &origin, &result) ==
                vr::VROverlayError_None,
            "cannot read overlay transform");
    return result;
}
} // namespace
int main(int argc, char** argv) {
    bool connected = false;
    try {
        require(argc == 3, "usage: vr-panel-probe --exercise|--check-centered EXPECTED_PID");
        const std::string mode = argv[1];
        require(mode == "--exercise" || mode == "--check-centered", "unknown probe mode");
        const auto pid = static_cast<std::uint32_t>(std::stoul(argv[2]));
        // Synthetic clicks are restricted to a deliberately input-disabled test
        // instance. The probe never injects into Steam's keyboard or a browser.
        std::ifstream command("/proc/" + std::to_string(pid) + "/cmdline");
        const std::string arguments((std::istreambuf_iterator<char>(command)), {});
        require(!arguments.empty() && arguments.find("uinput") == std::string::npos,
                "probe requires an input-disabled test instance");
        vr::EVRInitError error{};
        vr::VR_Init(&error, vr::VRApplication_Background);
        require(error == vr::VRInitError_None, "SteamVR is not available");
        connected = true;
        vr::VROverlayHandle_t panel{};
        require(vr::VROverlay()->FindOverlay("org.framekeyboard.panel", &panel) ==
                    vr::VROverlayError_None,
                "keyboard overlay does not exist");
        require(vr::VROverlay()->GetOverlayRenderingPid(panel) == pid, "unexpected overlay owner");
        require(vr::VROverlay()->IsOverlayVisible(panel), "keyboard overlay is hidden");
        if (mode == "--exercise") {
            require(vr::VROverlayView() != nullptr, "overlay event interface unavailable");
            const auto original = transform(panel);
            float width = 0;
            require(vr::VROverlay()->GetOverlayWidthInMeters(panel, &width) == vr::VROverlayError_None,
                    "cannot read panel width");
            click(panel, 707, 33);  // Open Move / align from the keyboard toolbar.
            click(panel, 605, 138); // Move right twice: 5 cm in its horizontal frame.
            click(panel, 605, 138);
            const auto moved = transform(panel);
            double squared = 0;
            for (int row = 0; row < 3; ++row) {
                squared += std::pow(moved.m[row][3] - original.m[row][3], 2);
            }
            require(std::abs(std::sqrt(squared) - .05) < .005, "position controls did not move 5 cm");
            click(panel, 1385, 240); // Tilt up.
            require(std::abs(transform(panel).m[1][2]) > .05, "tilt control did not rotate the panel");
            click(panel, 605, 444); // Larger.
            float resized = 0;
            vr::VROverlay()->GetOverlayWidthInMeters(panel, &resized);
            require(std::abs(resized - width - .05) < .005, "size control did not enlarge 5 cm");
            click(panel, 995, 444); // Face me, preserving position and clearing tilt/roll.
            require(std::abs(transform(panel).m[1][1] - 1) < .001,
                    "face-me control did not level panel");
            click(panel, 1385, 240); // Leave it tilted; a relaunch should clear this.
            std::cout << "Live overlay move, tilt, resize and face-me controls passed.\n";
        } else {
            const auto current = transform(panel);
            require(std::abs(current.m[1][0]) < .001 && std::abs(current.m[1][1] - 1) < .001 &&
                        std::abs(current.m[1][2]) < .001,
                    "relaunch did not clear rotation offsets");
            std::cout << "Overlay still visible with expected owner and recentered level rotation.\n";
        }
        vr::VR_Shutdown();
        return 0;
    } catch (const std::exception& error) {
        if (connected) {
            vr::VR_Shutdown();
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
