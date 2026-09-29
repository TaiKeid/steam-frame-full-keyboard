#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/placement_store.hpp"
#include <fstream>

#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
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
void drag_tests() {
    fk::PanelPlacement panel;
    panel.recenter(head());
    panel.adjust(fk::PlacementAction::TiltUp);
    panel.adjust(fk::PlacementAction::Larger);
    const auto original = panel.transform();
    fk::PanelDrag drag;
    drag.begin(head(), original);
    const auto no_jump = drag.update(head());
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(no_jump[r][c], original[r][c], "grab does not jump");
        }
    }
    auto moved = head();
    moved[0][3] += .2;
    moved[2][3] -= .3;
    panel.set_transform(drag.update(moved));
    near(panel.transform()[0][3], original[0][3] + .2, "controller translation moves panel");
    near(panel.transform()[2][3], original[2][3] - .3, "controller depth moves panel");
    near(panel.width(), 1.2, "drag retains size");
    // A 90-degree controller turn rotates the original offset around that hand.
    moved = {{{0, 0, 1, 1}, {0, 1, 0, 1.7}, {-1, 0, 0, 2}}};
    const auto turned = drag.update(moved);
    near(turned[0][3], .15, "controller yaw rotates laser distance around hand");
    near(turned[2][3], 2, "rotation preserves grab distance");
    panel.set_transform(turned);
    panel.recenter(head());
    near(panel.transform()[0][3], 1, "recenter resets dragged position");
    near(panel.transform()[1][1], 1, "recenter levels dragged rotation");
}
void stick_depth_tests() {
    // Tilt the component's neutral frame, as Frame's left/right models do.
    auto component = [](double x, double y) {
        const double a = y * 20 * std::numbers::pi / 180;
        const double b = x * 20 * std::numbers::pi / 180;
        fk::Transform pose{{{std::cos(b), 0, std::sin(b), 0},
                            {std::sin(a) * std::sin(b), std::cos(a), -std::sin(a) * std::cos(b), 0},
                            {-std::cos(a) * std::sin(b), std::sin(a), std::cos(a) * std::cos(b), 0}}};
        const double c = std::cos(.6), d = std::sin(.6);
        for (std::size_t col = 0; col < 3; ++col) {
            const double first = pose[0][col], second = pose[1][col];
            pose[0][col] = c * first - d * second;
            pose[1][col] = d * first + c * second;
        }
        return pose;
    };
    fk::ComponentAxis axis;
    require(axis.calibrate(component(0, 0), component(0, 1)), "stick calibration");
    for (double x : {-1., -.5, 0., .5, 1.}) {
        for (double y : {-1., -.5, 0., .5, 1.}) {
            near(axis.read(component(x, y)), y, "signed Y independent of sideways tilt");
        }
    }
    require(!axis.calibrate(component(0, 0), component(0, 0)), "static component refuses calibration");
    near(axis.read(component(0, 1)), 0, "failed calibration gives no motion");
    fk::PanelPlacement panel;
    panel.recenter(head());
    fk::PanelDrag drag;
    const auto original = panel.transform();
    drag.begin(head(), original);
    drag.move_depth(.19, .05);
    near(drag.update(head())[2][3], original[2][3], "stick dead zone has no drift");
    for (int i = 0; i < 20; ++i) {
        drag.move_depth(1, .05);
    }
    near(drag.update(head())[2][3], original[2][3] - .65, "up moves farther at time-based speed");
    for (int i = 0; i < 20; ++i) {
        drag.move_depth(-1, .05);
    }
    near(drag.update(head())[2][3], original[2][3], "down moves closer");
    for (int i = 0; i < 200; ++i) {
        drag.move_depth(-1, .05);
    }
    near(head()[2][3] - drag.update(head())[2][3], .2, "near limit prevents crossing the controller");
    for (int i = 0; i < 200; ++i) {
        drag.move_depth(1, .05);
    }
    near(head()[2][3] - drag.update(head())[2][3], 3, "far limit");
    drag.begin(head(), original);
    drag.move_depth(1, 100);
    near(drag.update(head())[2][3], original[2][3] - .0325, "stalled frame cannot jump depth");
    drag.begin(head(), original, {1, 0, 0});
    drag.move_depth(1, .05);
    near(drag.update(head())[0][3], original[0][3] + .0325, "depth follows calibrated laser direction");
    near(drag.update(head())[2][3], original[2][3], "sideways laser does not move along controller Z");
    near(drag.update(head())[1][1], original[1][1], "stick leaves orientation unchanged");
}
void persistence_tests() {
    TemporaryDirectory temp;
    const auto path = temp.path / "placement.json";
    require(!fk::load_placement(path), "first launch has no saved placement");
    fk::PanelPlacement original;
    original.recenter(head());
    original.adjust(fk::PlacementAction::Right);
    original.adjust(fk::PlacementAction::TiltUp);
    original.adjust(fk::PlacementAction::RollRight);
    original.adjust(fk::PlacementAction::Larger);
    fk::save_placement(path, {original.transform(), original.width(), 18446744073709551615ULL});
    const auto saved = fk::load_placement(path);
    require(saved && saved->universe == 18446744073709551615ULL, "universe ID retains all bits");
    fk::PanelPlacement reopened;
    reopened.restore(saved->transform, saved->width);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(reopened.transform()[r][c], original.transform()[r][c], "reopen preserves pose");
        }
    }
    near(reopened.width(), original.width(), "reopen preserves width");
    reopened.recenter(head());
    near(reopened.transform()[1][1], 1, "explicit relaunch can recenter restored pose");
    near(reopened.width(), original.width(), "recenter retains saved size");
    auto invalid = *saved;
    invalid.transform[0][0] *= 2;
    bool rejected = false;
    try {
        fk::save_placement(path, invalid);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "scaled transform rejected");
    near(fk::load_placement(path)->transform[0][0], saved->transform[0][0],
         "failed save preserves prior file");
    for (const std::string bad : {"{", "{}", "{\"schema_version\":2}"}) {
        {
            std::ofstream file(path);
            file << bad;
        }
        rejected = false;
        try {
            fk::load_placement(path);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "corrupt or unsupported placement rejected");
    }
    fk::save_placement(path, *saved);
    require(fk::load_placement(path).has_value(), "fresh valid save replaces corrupt placement");
}
void key_feedback_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    fk::App app(options, sink);
    require(!app.down(0, -1, -1, 0), "background press has no key feedback");
    require(!app.down(0, 50, 33, 0), "toolbar press has no key feedback");
    require(!app.up(0, -1, -1), "toolbar release has no key feedback");
    app.cancel();
    const auto view = app.view();
    for (int y = 96; y < fk::panel_height; y += 2) {
        for (int x = 0; x < fk::panel_width; x += 2) {
            const auto* key = app.renderer.hit_key(view, x, y);
            if (!key || key->id != "KeyA") {
                continue;
            }
            require(app.move(0, x, y), "entering a key requests hover feedback");
            require(!app.move(0, x, y), "stationary hover does not buzz");
            require(app.move(1, x, y), "each pointer tracks hover independently");
            require(app.down(0, x, y, 0), "key press requests feedback even in input-disabled preview");
            require(!app.down(0, x, y, .01), "duplicate down does not repeat feedback");
            require(!app.move(0, -1, -1), "leaving a key does not pulse");
            require(!app.move(0, x, y), "held pointer does not generate hover pulses");
            require(app.up(0, -1, -1), "release outside captured key still clicks");
            require(!app.up(0, x, y), "duplicate release does not click");
            require(app.down(0, x, y, .02), "next press requests new feedback");
            app.set_dragging(true);
            require(!app.up(0, x, y), "canceled press has no release click");
            require(!app.move(1, x, y), "dragging suppresses hover feedback");
            require(!app.down(1, x, y, .03), "dragging suppresses key feedback");
            return;
        }
    }
    throw std::runtime_error("A key hit region missing");
}
void grip_tests() {
    const auto released = head();
    auto pressed = released;
    const double radians = 9.5 * std::numbers::pi / 180;
    pressed[0][0] = std::cos(radians);
    pressed[0][1] = -std::sin(radians);
    pressed[1][0] = std::sin(radians);
    pressed[1][1] = std::cos(radians);
    pressed[2][3] += 10; // Component pivot/translation must not alter squeeze travel.
    near(fk::component_rotation_degrees(released, pressed), 9.5, "grip angular travel");
    near(fk::component_rotation_degrees(pressed, pressed), 0, "neutral orientation");
    // The reference orientation need not be axis-aligned in the controller model.
    near(fk::component_rotation_degrees(pressed, released), 9.5, "relative rotation");
    fk::GripLatch grip;
    require(!grip.update(9.5).held, "startup while squeezed cannot start a drag");
    require(!grip.update(0).held, "released sample arms grip");
    require(!grip.update(.8).held, "small travel cannot start a drag");
    auto state = grip.update(1.1);
    require(state.held && state.pressed, "squeeze emits a single rising edge");
    state = grip.update(.8);
    require(state.held && !state.pressed, "hysteresis keeps grip held near press threshold");
    require(grip.update(.55).held, "release dead zone avoids chatter");
    require(!grip.update(.4).held, "release ends capture");
    require(grip.update(2).pressed, "new squeeze captures again");
    require(!grip.update(std::nullopt).held, "missing input releases grip");
    require(!grip.update(2).held, "reconnect while held must wait for release");
    grip.update(0);
    require(grip.update(2).pressed, "release after reconnect re-arms grip");
    require(!grip.update(std::numeric_limits<double>::quiet_NaN()).held,
            "invalid component pose releases grip");
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
struct RecordingSink : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    bool ready{true};
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool pump() override { return ready; }
};
void typing_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = "vr";
    options.input = "ei";
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    auto type_key = [&] {
        // Find a real rendered key hit region, not a hardcoded toolbar coordinate.
        const auto view = app.view();
        for (int y = 96; y < fk::panel_height; y += 2) {
            for (int x = 0; x < fk::panel_width; x += 2) {
                auto* key = app.renderer.hit_key(view, x, y);
                if (key && key->id == "KeyA") {
                    app.down(0, x, y, 1);
                    app.up(0, x, y);
                    return;
                }
            }
        }
        throw std::runtime_error("A key hit region missing");
    };
    type_key();
    require(sink.events == std::vector<std::pair<int, int>>{{30, 1}, {30, 0}},
            "typing launcher delivers key events");
    app.summon();
    type_key();
    require(sink.events.size() == 4, "relaunch preserves typing choice");
    app.set_dragging(true);
    type_key();
    require(sink.events.size() == 4, "drag capture suppresses keys");
    app.set_dragging(false);
    sink.ready = false;
    app.tick(2);
    type_key();
    require(sink.events.size() == 4, "lost backend disables typing");
    require(app.view().status.find("connection lost") != std::string::npos, "lost backend is visible");
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
    for (const auto& control : app.view().controls) {
        require(control.id != "drag", "whole-keyboard grip requires no move handle");
    }
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
        drag_tests();
        stick_depth_tests();
        grip_tests();
        persistence_tests();
        key_feedback_tests();
        instance_tests();
        ui_tests();
        typing_tests();
        std::cout << "Placement, repeated-launch IPC, stale-owner recovery and placement UI passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
