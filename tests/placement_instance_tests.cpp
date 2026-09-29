#include "framekeyboard/app.hpp"

#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-8, message);
}
struct TemporaryDirectory {
    fk::fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/fk-instance-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fk::fs::remove_all(path, ignored);
    }
};
fk::Transform head() {
    return {{{1, 0, 0, 1}, {0, 1, 0, 1.7}, {0, 0, 1, 2}}};
}
void placement_tests() {
    fk::PanelPlacement panel;
    panel.recenter(head());
    auto pose = panel.transform();
    near(pose[0][3], 1, "centered horizontally");
    near(pose[1][3], 1.45, "below eye level");
    near(pose[2][3], 1.15, "in front of headset");
    panel.adjust(fk::PlacementAction::Right);
    panel.adjust(fk::PlacementAction::Up);
    panel.adjust(fk::PlacementAction::Nearer);
    pose = panel.transform();
    near(pose[0][3], 1.025, "move right");
    near(pose[1][3], 1.475, "move up");
    near(pose[2][3], 1.175, "move closer");
    panel.adjust(fk::PlacementAction::TurnLeft);
    panel.adjust(fk::PlacementAction::TiltUp);
    panel.adjust(fk::PlacementAction::RollRight);
    const auto rotated = panel.transform();
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            double dot = 0;
            for (int row = 0; row < 3; ++row) {
                dot += rotated[row][a] * rotated[row][b];
            }
            near(dot, a == b ? 1 : 0, "rotation remains orthonormal");
        }
        near(rotated[a][3], pose[a][3], "rotate around panel center");
    }
    auto moved_head = head();
    moved_head[0][3] += 1;
    panel.face(moved_head);
    auto faced = panel.transform();
    for (int row = 0; row < 3; ++row) {
        near(faced[row][3], pose[row][3], "face me preserves position");
    }
    near(faced[1][1], 1, "face me levels roll and pitch");
    require(faced[0][2] > 0, "normal points toward moved viewer");
    for (int i = 0; i < 100; ++i) {
        panel.adjust(fk::PlacementAction::Smaller);
    }
    near(panel.width(), .45, "minimum width");
    for (int i = 0; i < 100; ++i) {
        panel.adjust(fk::PlacementAction::Larger);
    }
    near(panel.width(), 2, "maximum width");
    // A straight-down headset must still produce an upright, usable recenter pose.
    const fk::Transform looking_down{{{1, 0, 0, 0}, {0, 0, 1, 1.7}, {0, -1, 0, 0}}};
    panel.recenter(looking_down);
    pose = panel.transform();
    near(pose[1][1], 1, "recenter removes head pitch");
    near(pose[2][3], -.85, "vertical gaze uses stable horizontal heading");
    near(panel.width(), 2, "recenter preserves chosen size");
    const fk::Transform turned{{{0, 0, 1, 3}, {0, 1, 0, 1.7}, {-1, 0, 0, 4}}};
    panel.recenter(turned);
    pose = panel.transform();
    near(pose[0][3], 2.15, "recenter uses current heading");
    near(pose[2][3], 4, "recenter clears old position offsets");
}
void instance_tests() {
    TemporaryDirectory temp;
    const auto runtime = temp.path / "runtime";
    {
        fk::VrInstance owner(runtime);
        require(owner.is_owner(), "first launch owns instance");
        for (int repeat = 0; repeat < 3; ++repeat) {
            auto launch = std::async(std::launch::async, [&] {
                fk::VrInstance second(runtime);
                return second.is_owner();
            });
            bool received = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            do {
                received |= owner.poll_recenter();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (launch.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
                     std::chrono::steady_clock::now() < deadline);
            require(!launch.get(), "second launch never becomes a VR owner");
            require(received, "second launch delivers recenter request");
            require(fk::fs::exists(runtime / "vr.sock"), "client destruction leaves owner's socket");
        }
    }
    require(!fk::fs::exists(runtime / "vr.sock"), "owner removes endpoint at normal exit");
    require(fk::fs::exists(runtime / "vr.lock"), "lock inode must remain stable across launches");
    // Simulate abrupt process death. The next launch must recover the stale socket
    // using the kernel-released file lock, without killing or trusting a saved PID.
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        try {
            fk::VrInstance crashed(runtime);
            _exit(crashed.is_owner() ? 0 : 2);
        } catch (...) {
            _exit(3);
        }
    }
    int status = 0;
    waitpid(child, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "crashed owner setup");
    require(fk::fs::exists(runtime / "vr.sock"), "abrupt exit leaves a stale endpoint");
    fk::VrInstance recovered(runtime);
    require(recovered.is_owner(), "new launch recovers after abrupt exit");
}
void ui_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    fk::App app(options, sink);
    app.show_placement();
    const auto view = app.view();
    require(view.settings, "placement controls hide key hit regions");
    bool clicked = false;
    for (const auto& control : view.controls) {
        if (control.id != "move-right") {
            continue;
        }
        const double x = control.bounds.x + control.bounds.width / 2;
        const double y = control.bounds.y + control.bounds.height / 2;
        app.down(0, x, y, 1);
        app.up(0, x, y);
        clicked = true;
    }
    require(clicked, "movement control exists");
    require(app.take_placement_actions() == std::vector{fk::PlacementAction::Right},
            "UI queues placement command");
    app.summon();
    require(!app.view().settings && app.take_recenter(),
            "summon returns to keyboard and requests recenter");
    require(!app.take_recenter(), "recenter request consumed once");
}
} // namespace
int main() {
    try {
        placement_tests();
        instance_tests();
        ui_tests();
        std::cout << "Placement, repeated-launch IPC, stale-owner recovery and placement UI passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
