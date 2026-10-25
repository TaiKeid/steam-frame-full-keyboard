#include "framekeyboard/app.hpp"
#include "framekeyboard/feedback.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/resizer.hpp"
#include <fstream>

#include <algorithm>
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
void dashboard_tab_tests() {
    fk::DashboardAnchor tracker;
    const fk::Transform main{{{1, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, -1}}};
    const fk::Transform bar{{{1, 0, 0, 0}, {0, 1, 0, .85}, {0, 0, 1, -.8}}};
    const fk::Transform moved_bar{{{0, 0, 1, 2}, {0, 1, 0, .9}, {-1, 0, 0, -3}}};
    require(tracker.update(main, bar) == main, "visible Steam tab calibrates bottom-center mount");
    auto expected = fk::move_with_dashboard(main, bar, moved_bar);
    auto followed = tracker.update({}, moved_bar);
    require(followed.has_value(), "app tabs retain a live dashboard anchor");
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*followed)[r][c], expected[r][c], "hidden main follows bar translation and yaw");
        }
    }
    require(!tracker.update({}, {}), "missing visible overlays supply no stale anchor");
    require(tracker.update({}, moved_bar) == followed, "temporary hide retains calibrated placement");
    fk::DashboardAnchor reopened;
    reopened.restore(followed, tracker.bar());
    auto returned = reopened.update({}, bar);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*returned)[r][c], main[r][c], "reopening an app tab uses saved bar reference");
        }
    }
    fk::DashboardAnchor fresh;
    require(fresh.update({}, moved_bar) == moved_bar, "first app launch uses live bar fallback");
    require(fresh.update(main, bar) == bar, "tab switches keep the existing bar-relative mount");

    const double angle = 35 * std::numbers::pi / 180;
    const fk::Transform tilt{{{1, 0, 0, 0},
                              {0, std::cos(angle), -std::sin(angle), .4},
                              {0, std::sin(angle), std::cos(angle), 0}}};
    auto tilt_pose = [&](fk::Transform pose) {
        const auto original = pose;
        for (std::size_t col = 0; col < 4; ++col) {
            pose[1][col] = std::cos(angle) * original[1][col] - std::sin(angle) * original[2][col];
            pose[2][col] = std::sin(angle) * original[1][col] + std::cos(angle) * original[2][col];
        }
        pose[1][3] += .4;
        return pose;
    };
    const auto tilted_bar = tilt_pose(bar);
    fk::DashboardAnchor rotating;
    rotating.update(main, bar);
    auto tilted_mount = rotating.update({}, tilted_bar);
    const auto expected_mount = tilt_pose(main);
    fk::PanelPlacement keyboard;
    keyboard.recenter(head(), &main);
    const auto followed_keyboard = fk::move_with_dashboard(keyboard.transform(), main, *tilted_mount);
    const auto expected_keyboard = tilt_pose(keyboard.transform());
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*tilted_mount)[r][c], expected_mount[r][c], "app-tab anchor follows dashboard pitch");
            near(followed_keyboard[r][c], expected_keyboard[r][c],
                 "dashboard pitch carries keyboard rotation and positional offset together");
        }
    }
    require(rotating.update(expected_mount, tilted_bar) == tilted_mount,
            "switching to Steam preserves full-rotation reference");

    // The previous release saved a level bar, even though its mesh was tilted.
    auto old_bar = bar;
    old_bar[1][3] = tilted_bar[1][3];
    old_bar[2][3] = tilted_bar[2][3];
    fk::DashboardAnchor migrated;
    migrated.restore(main, old_bar, false);
    require(migrated.update({}, tilted_bar) == main, "legacy saves do not jump to the bar mesh tilt");
    const auto tilted_again = tilt_pose(tilted_bar);
    const auto after_migration = migrated.update({}, tilted_again);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*after_migration)[r][c], expected_mount[r][c],
                 "migrated saves follow subsequent pitch");
        }
    }
    auto scaled_tilt = tilt;
    for (auto& row : scaled_tilt) {
        row[0] *= .7;
        row[1] *= .5;
        row[2] *= .3;
    }
    const auto normalized = fk::dashboard_anchor(scaled_tilt);
    require(normalized.has_value(), "scaled tilted dashboard remains usable");
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*normalized)[r][c], tilt[r][c], "scale removal preserves dashboard pitch");
        }
    }
}
void placement_tests() {
    fk::PanelPlacement resized;
    resized.set_width(.7);
    require(!resized.ready(), "restoring size alone must not make an old tracking pose usable");
    resized.recenter(head());
    near(resized.width(), .7, "recenter in a new tracking space preserves the restored size");
    fk::PanelPlacement panel;
    panel.recenter(head());
    near(panel.width(), .95, "default width is four 5 cm steps below 1.15 m");
    auto pose = panel.transform();
    near(pose[1][1], std::cos(50 * std::numbers::pi / 180),
         "default surface is tilted 50 degrees from upright");
    near(pose[1][2], std::sin(50 * std::numbers::pi / 180), "typing surface faces upward");
    require(pose[2][1] < 0, "top edge slopes away from the viewer");
    near(pose[0][3], 1, "centered horizontally");
    near(pose[1][3], 1.05, "fallback is 65 cm below eye level");
    near(pose[2][3], 1.15, "in front of headset");
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            double dot = 0;
            for (int row = 0; row < 3; ++row) {
                dot += pose[row][a] * pose[row][b];
            }
            near(dot, a == b ? 1 : 0, "recenter rotation is orthonormal");
        }
    }
    panel.set_width(.9);
    near(panel.width(), .9, "width is set in meters");
    require(panel.transform() == pose, "setting the width preserves the entire pose");
    panel.set_width(.1);
    near(panel.width(), .45, "minimum width");
    panel.set_width(5);
    near(panel.width(), 2, "maximum width");
    // A straight-down headset must retain the default desk tilt, not add head pitch.
    const fk::Transform looking_down{{{1, 0, 0, 0}, {0, 0, 1, 1.7}, {0, -1, 0, 0}}};
    panel.recenter(looking_down);
    pose = panel.transform();
    near(pose[1][1], std::cos(50 * std::numbers::pi / 180), "recenter ignores head pitch");
    near(pose[2][3], -.85, "vertical gaze uses stable horizontal heading");
    near(panel.width(), 2, "recenter preserves chosen size");
    const fk::Transform turned{{{0, 0, 1, 3}, {0, 1, 0, 1.7}, {-1, 0, 0, 4}}};
    panel.recenter(turned);
    pose = panel.transform();
    near(pose[0][3], 2.15, "recenter uses current heading");
    near(pose[2][3], 4, "recenter clears old position offsets");
    auto mount = head();
    mount[0][3] = .3;
    mount[1][3] = .55;
    mount[2][3] = -1;
    // Steam's parented overlay transform may include nonuniform scale.
    mount[0][0] = .7;
    mount[1][1] = .5;
    mount[2][2] = .3;
    const auto anchor = fk::dashboard_anchor(mount);
    require(anchor.has_value(), "scaled dashboard has a usable anchor");
    panel.recenter(head(), &*anchor);
    pose = panel.transform();
    near(pose[0][3], .3, "centered beneath the dashboard");
    near(pose[1][3] + panel.width() * .375 / 2 * pose[1][1], .55 - .06,
         "top edge leaves 6 cm below dashboard at any width");
    near(pose[2][3], -.76, "center is 24 cm toward viewer from dashboard bottom");
    near(pose[0][0], 1, "dashboard scale is not inherited");
    near(pose[1][2], std::sin(50 * std::numbers::pi / 180), "dashboard gets requested pitch");
    near(panel.width(), 2, "dashboard recenter preserves chosen width");
    const fk::Transform moved_dashboard{{{0, 0, 1, 5}, {0, 1, 0, 1}, {-1, 0, 0, -4}}};
    const auto followed = fk::move_with_dashboard(pose, *anchor, moved_dashboard);
    near(followed[0][3], 5.24, "dashboard translation and yaw carry keyboard offset");
    near(followed[2][3], -4, "keyboard stays centered after walking across room");
    near(followed[1][3] - 1, pose[1][3] - .55, "vertical custom offset is preserved");
    const auto returned = fk::move_with_dashboard(followed, moved_dashboard, *anchor);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(returned[r][c], pose[r][c], "dashboard movement preserves full relative pose");
        }
    }
    mount[0][3] = std::numeric_limits<double>::quiet_NaN();
    require(!fk::dashboard_anchor(mount), "invalid dashboard is rejected");
    require(!fk::dashboard_anchor({}), "missing dashboard cannot anchor at room origin");
}
void drag_tests() {
    fk::PanelPlacement panel;
    panel.recenter(head());
    panel.set_width(1);
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
    near(panel.width(), 1.0, "drag retains size");
    // A 90-degree controller turn rotates the original offset around that hand.
    moved = {{{0, 0, 1, 1}, {0, 1, 0, 1.7}, {-1, 0, 0, 2}}};
    const auto turned = drag.update(moved);
    near(turned[0][3], .15, "controller yaw rotates laser distance around hand");
    near(turned[2][3], 2, "rotation preserves grab distance");
    panel.set_transform(turned);
    panel.recenter(head());
    near(panel.transform()[0][3], 1, "recenter resets dragged position");
    near(panel.transform()[1][1], std::cos(50 * std::numbers::pi / 180),
         "recenter restores desk tilt after dragging");
}
// Turns a pose's rotation about a unit axis through its center, as a hand tilt does.
fk::Transform lean(const fk::Transform& pose, const std::array<double, 3>& axis, double degrees) {
    const double a = degrees * std::numbers::pi / 180, c = std::cos(a), s = std::sin(a);
    auto result = pose;
    for (int column = 0; column < 3; ++column) {
        const std::array<double, 3> v{pose[0][column], pose[1][column], pose[2][column]};
        const std::array<double, 3> cross{axis[1] * v[2] - axis[2] * v[1],
                                          axis[2] * v[0] - axis[0] * v[2],
                                          axis[0] * v[1] - axis[1] * v[0]};
        const double along = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        for (int r = 0; r < 3; ++r) {
            result[r][column] = v[r] * c + cross[r] * s + axis[r] * along * (1 - c);
        }
    }
    return result;
}
// The right edge's angle above level: the lean auto leveling removes.
double lean_degrees(const fk::Transform& pose) {
    return std::atan2(pose[1][0], std::hypot(pose[0][0], pose[2][0])) * 180 / std::numbers::pi;
}
void horizon_tests() {
    // A level pose with 30-degree yaw and 20-degree desk tilt, independent of
    // the default recenter angle. Horizon assistance must preserve both.
    const double yaw = 30 * std::numbers::pi / 180, pitch = -20 * std::numbers::pi / 180;
    const fk::Transform level{
        {{std::cos(yaw), std::sin(yaw) * std::sin(pitch), std::sin(yaw) * std::cos(pitch), 1},
         {0, std::cos(pitch), -std::sin(pitch), 1.05},
         {-std::sin(yaw), std::cos(yaw) * std::sin(pitch), std::cos(yaw) * std::cos(pitch), 1.15}}};
    // A sideways hand tilt turns the panel about the level axis square to its
    // right edge, here (sin yaw, 0, cos yaw); a positive angle raises the right edge.
    auto rolled = [&](double degrees) {
        return lean(level, {std::sin(yaw), 0, std::cos(yaw)}, degrees);
    };
    auto roll_degrees = [](const fk::Transform& pose) { return lean_degrees(pose); };
    for (double degrees : {-5., -2., 2., 5.}) {
        fk::HorizonAlignment horizon;
        const auto raw = rolled(degrees);
        near(roll_degrees(horizon.update(raw, 10)), degrees, "alignment starts without a jump");
        near(roll_degrees(horizon.update(raw, 10.125)), degrees * .84375, "500 ms smoothstep eases in");
        require(horizon.animating(), "horizon requests fast updates during easing");
        near(roll_degrees(horizon.update(raw, 10.25)), degrees * .5, "halfway roll");
        const auto aligned = horizon.update(raw, 10.5);
        require(!horizon.animating(), "completed easing permits idle polling");
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                near(aligned[r][c], level[r][c], "alignment preserves heading, pitch and position");
            }
        }
        near(roll_degrees(horizon.update(raw, 12)), 0, "stationary pose stays level after release");
        // Regrab the displayed pose, then cancel assistance. Neither may expose
        // the original raw roll or accumulate another rotation correction.
        fk::PanelDrag drag;
        drag.begin(head(), aligned);
        horizon.reset();
        near(roll_degrees(horizon.update(drag.update(head()), 13)), 0, "regrab stays level");
    }
    fk::HorizonAlignment grabbed;
    grabbed.update(rolled(4), 1);
    grabbed.update(rolled(4), 1.25);
    require(grabbed.update(rolled(3), 2, true) == rolled(3),
            "grab bypasses an existing alignment animation");
    require(grabbed.update(rolled(-4), 12, true) == rolled(-4),
            "long grab preserves exact hand pose within snapping threshold");
    require(!grabbed.animating(), "grab does not advance horizon easing");
    near(roll_degrees(grabbed.update(rolled(-4), 13)), -4, "release begins without a snap");
    near(roll_degrees(grabbed.update(rolled(-4), 13.25)), -2, "release eases halfway after 250 ms");
    near(roll_degrees(grabbed.update(rolled(-4), 13.5)), 0, "release levels after 500 ms");
    fk::HorizonAlignment disabled;
    disabled.update(rolled(4), 1);
    const auto visible = disabled.update(rolled(4), 1.25);
    near(roll_degrees(visible), 2, "toggle test starts during partial leveling");
    // The VR layer adopts this displayed pose when the setting changes.
    require(disabled.update(visible, 1.25, false, false) == visible &&
                disabled.update(visible, 12, false, false) == visible && !disabled.animating(),
            "disabling leveling freezes the visible pose and stops animation");
    require(disabled.update(rolled(-4), 13, true, false) == rolled(-4) &&
                disabled.update(rolled(-4), 14, false, false) == rolled(-4),
            "disabled leveling preserves exact hand and release angles");
    near(roll_degrees(disabled.update(rolled(-4), 15)), -4,
         "reenabling starts without stale correction or a jump");
    near(roll_degrees(disabled.update(rolled(-4), 15.5)), 0,
         "reenabling restores the normal release correction");
    fk::HorizonAlignment horizon;
    near(roll_degrees(horizon.update(rolled(5.01), 20)), 5.01, "outside range is untouched");
    horizon.update(rolled(4), 21);
    const auto partial = horizon.update(rolled(4), 21.25);
    near(roll_degrees(partial), 2, "partial correction");
    near(roll_degrees(horizon.update(rolled(6), 21.25)), 4,
         "leaving range retains correction initially");
    near(roll_degrees(horizon.update(rolled(6), 21.5)), 5, "leaving range eases correction away");
    near(roll_degrees(horizon.update(rolled(6), 21.75)), 6, "larger deliberate lean is restored");
    horizon.update(rolled(-4), 23);
    near(roll_degrees(horizon.update(rolled(-4), 23.5)), 0, "reentry aligns opposite roll");
    horizon.reset();
    near(roll_degrees(horizon.update(partial, 25)), 2, "cancel freezes visible intermediate pose");
    const fk::Transform flat{{{1, 0, 0, 0}, {0, 0, 1, 1}, {0, -1, 0, 2}}};
    require(horizon.update(flat, 26) == flat, "horizontal panel has no horizon roll");
    require(horizon.update(rolled(4), std::numeric_limits<double>::quiet_NaN()) == rolled(4),
            "invalid timing does not corrupt placement");
}
void dashboard_leveling_tests() {
    // Rotations as row-major 3x3 blocks of a Transform; position stays put.
    using M = std::array<std::array<double, 3>, 3>;
    auto multiply = [](const M& a, const M& b) {
        M r{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 3; ++k) {
                    r[i][j] += a[i][k] * b[k][j];
                }
            }
        }
        return r;
    };
    auto axis = [](int which, double degrees) {
        const double a = degrees * std::numbers::pi / 180, c = std::cos(a), s = std::sin(a);
        if (which == 0) {
            return M{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
        }
        if (which == 1) {
            return M{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
        }
        return M{{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
    };
    auto pose = [](const M& m) {
        return fk::Transform{{{m[0][0], m[0][1], m[0][2], 1},
                              {m[1][0], m[1][1], m[1][2], 1.05},
                              {m[2][0], m[2][1], m[2][2], 1.15}}};
    };
    // reference * yaw (about up) * pitch (one end higher) * roll (the desk tilt).
    auto panel = [&](const M& reference, double yaw, double pitch, double tilt) {
        return pose(
            multiply(multiply(multiply(reference, axis(1, yaw)), axis(2, pitch)), axis(0, tilt)));
    };
    auto same = [](const fk::Transform& a, const fk::Transform& b, const char* message) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                near(a[r][c], b[r][c], message);
            }
        }
    };
    auto eased = [](const fk::Transform& raw, const std::optional<fk::Transform>& reference) {
        fk::HorizonAlignment horizon;
        horizon.update(raw, 1, false, true, reference);
        return horizon.update(raw, 1.5, false, true, reference);
    };
    // A dashboard at a 30-degree heading, rolled 3 degrees, and a keyboard near it
    // turned 2 degrees, one end 1 degree low and tilted 50 degrees back.
    const M dashboard = multiply(axis(1, 30), axis(2, 3));
    const auto raw = panel(dashboard, 2, -1, -50);
    same(eased(raw, pose(dashboard)), panel(dashboard, 0, 0, -50),
         "near the dashboard yaw and pitch are removed and the tilt is kept");
    // In the room, facing the viewer at a 70-degree heading: the same alignment.
    const M facing = axis(1, 70);
    same(eased(panel(facing, 3, 2, -50), pose(facing)), panel(facing, 0, 0, -50),
         "facing the viewer, yaw and pitch are removed and the tilt is kept");
    // Without a reference only the pitch is removed; the chosen heading stays.
    same(eased(panel(facing, 3, 2, -50), std::nullopt), panel(facing, 3, 0, -50),
         "without a reference only the pitch is removed");
    // A deliberate turn beyond the capture range is left alone.
    same(eased(panel(facing, 6, 0, -50), pose(facing)), panel(facing, 6, 0, -50),
         "a turn beyond 5 degrees is kept");
    // Changing reference eases from the correction shown, without a jump.
    fk::HorizonAlignment horizon;
    horizon.update(raw, 1, false, true, pose(dashboard));
    const auto aligned = horizon.update(raw, 1.5, false, true, pose(dashboard));
    horizon.retarget(2);
    same(horizon.update(raw, 2), aligned, "changing reference starts without a jump");
    require(horizon.animating(), "changing reference eases");
    near(lean_degrees(horizon.update(raw, 2.5)), 0, "after easing the pitch is removed in the room");
    // A keyboard leveled in the room and carried by a dashboard turned 90 degrees
    // stays leveled: its room reference turns along instead of facing the old way.
    auto at = [&](const M& m, double x, double y, double z) {
        auto result = pose(m);
        result[0][3] = x;
        result[1][3] = y;
        result[2][3] = z;
        return result;
    };
    const auto before = at(dashboard, 0, 1.2, -1),
               after = at(multiply(axis(1, 90), dashboard), -1, 1.3, 0);
    const auto leveled = panel(facing, 0, 0, -50);
    const auto carried = fk::move_with_dashboard(leveled, before, after);
    const auto turned = fk::turn_with_dashboard(pose(facing), before, after, carried);
    same(turned, at(axis(1, 160), carried[0][3], carried[1][3], carried[2][3]),
         "the room reference turns by the dashboard's heading and sits at the keyboard");
    same(eased(carried, turned), carried, "a carried leveled keyboard needs no correction");
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
    // A dragged pose has arbitrary heading/tilt/roll and must survive resize
    // and save/restore without being reconstructed from the recenter defaults.
    const fk::Transform dragged{{{.8, -.6, 0, 1.2}, {0, 0, 1, 1.1}, {-.6, -.8, 0, 2.3}}};
    original.set_transform(dragged);
    original.set_width(1);
    require(original.transform() == dragged, "a width change preserves a dragged pose");
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
    near(reopened.transform()[1][1], std::cos(50 * std::numbers::pi / 180),
         "explicit relaunch restores default desk tilt");
    near(reopened.width(), original.width(), "recenter retains saved size");
    require(!saved->dashboard, "legacy world-space placement still loads");
    auto anchored = *saved;
    anchored.dashboard_full_rotation = true;
    anchored.dashboard = head();
    anchored.dashboard_bar = head();
    (*anchored.dashboard_bar)[1][3] -= .15;
    fk::save_placement(path, anchored);
    require(fk::load_placement(path)->dashboard == anchored.dashboard,
            "dashboard anchor survives close and reopen");
    require(fk::load_placement(path)->dashboard_bar == anchored.dashboard_bar,
            "dashboard bar reference survives close and reopen");
    require(fk::load_placement(path)->dashboard_full_rotation, "full-rotation marker persists");
    auto bad_anchor = anchored;
    (*bad_anchor.dashboard)[0][0] = 2;
    bool bad_anchor_rejected = false;
    try {
        fk::save_placement(path, bad_anchor);
    } catch (const std::exception&) {
        bad_anchor_rejected = true;
    }
    require(bad_anchor_rejected, "scaled saved dashboard anchor is rejected");
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
void haptic_routing_tests() {
    fk::KeyHaptics feedback;
    using Pulse = std::pair<unsigned, fk::KeyFeedback>;
    feedback.hover(2);
    feedback.press(0, 1);
    feedback.hover(1);
    require(feedback.take(1) ==
                std::vector<Pulse>{{1, fk::KeyFeedback::Press}, {2, fk::KeyFeedback::Hover}},
            "click wins only on its controller; other hand keeps its hover");
    feedback.hover(1);
    require(feedback.take(1.01).empty(), "hover cannot extend a click into the next frame");
    feedback.hover(1);
    require(feedback.take(1.04) == std::vector<Pulse>{{1, fk::KeyFeedback::Hover}},
            "hover resumes after click");
    feedback.press(1, 2);
    feedback.release(0);
    require(feedback.take(2) ==
                std::vector<Pulse>{{1, fk::KeyFeedback::Release}, {2, fk::KeyFeedback::Press}},
            "release belongs to original press hand while other hand presses");
    feedback.release(0);
    require(feedback.take(3).empty(), "duplicate release cannot vibrate");
    feedback.press(0, 1);
    feedback.press(1, 2);
    feedback.cancel_pointer(0);
    require(feedback.take(3.5) == std::vector<Pulse>{{2, fk::KeyFeedback::Press}},
            "one controller leaving preserves the other controller's feedback");
    feedback.release(0);
    require(feedback.take(3.6).empty(), "canceled controller cannot emit a release pulse");
    feedback.cancel();
    feedback.release(1);
    require(feedback.take(4).empty(), "canceled key has no release pulse");
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
void trigger_release_tests() {
    fk::TriggerReleaseGuard release;
    release.begin(10);
    for (double now : {10., 10.05, 10.14, 10.16, 10.19}) {
        require(!release.update(0, now), "quick release waits for grace and repeated rest samples");
    }
    require(release.update(0, 10.22), "lost mouse-up recovers without ever seeing the trigger held");

    release.begin(20);
    require(!release.update(0, 20) && !release.update(0, 20.04),
            "stale initial rest samples cannot cancel a new capture");
    require(!release.update(12.5, 20.06) && !release.update(12.5, 30),
            "a real hold survives both startup and a long movement");
    require(release.update(0, 30.01), "observed hold retains immediate release recovery");
    release.begin(40);
    require(!release.update(12.5, 40.01) && release.update(0, 40.02),
            "an observed quick click releases even inside startup grace");

    release.begin(50);
    require(!release.update(0, 50.16) && !release.update(0, 50.19) && !release.update(.75, 50.2) &&
                !release.update(0, 50.21) && !release.update(0, 50.24) && release.update(0, 50.27),
            "partially pulled trigger interrupts the released-sample window");

    release.begin(60);
    require(!release.update(0, 60.16) && !release.update(std::nullopt, 60.19) &&
                !release.update(0, 60.22) &&
                !release.update(std::numeric_limits<double>::quiet_NaN(), 60.24) &&
                !release.update(0, 60.27) && !release.update(0, 60.29) && release.update(0, 60.33),
            "missing and invalid samples cannot count as continuously released input");

    release.begin(70);
    require(!release.update(0, 70.16) && !release.update(0, 71) && !release.update(0, 71.02) &&
                release.update(0, 71.06),
            "a polling gap requires a fresh sequence of released samples");

    release.begin(80);
    for (int i = 0; i < 60; ++i) {
        require(!release.update(std::nullopt, 80 + i / 60.),
                "unavailable trigger data never manufactures release");
    }
    release.update(12.5, 81);
    release.begin(90);
    require(!release.update(0, 90.01), "a new capture clears the preceding held state");
}
void instance_tests() {
    TemporaryDirectory temp;
    const auto runtime = temp.path / "runtime";
    {
        fk::VrInstance owner(runtime);
        require(owner.is_owner(), "first launch owns instance");
        {
            fk::VrInstance background(runtime, false);
            require(!background.is_owner() && !owner.poll_recenter(),
                    "duplicate boot startup leaves the existing panel hidden");
        }
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
    bool ready{true}, recoverable{}, reset{};
    bool can_resume() const override { return recoverable; }
    bool take_input_reset() override { return std::exchange(reset, false); }
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool pump() override { return ready; }
};
void type_a(fk::App& app, bool release = true, const std::string& id = "KeyA") {
    // Find a real rendered key hit region, not a hardcoded toolbar coordinate.
    const auto view = app.view();
    for (int y = 96; y < fk::panel_height; y += 2) {
        for (int x = 0; x < fk::panel_width; x += 2) {
            auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->id == id) {
                app.down(0, x, y, 1);
                if (release) {
                    app.up(0, x, y);
                }
                return;
            }
        }
    }
    throw std::runtime_error("A key hit region missing");
}
void typing_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    auto type_key = [&] { type_a(app); };
    type_key();
    require(sink.events == std::vector<std::pair<int, int>>{{30, 1}, {30, 0}},
            "typing launcher delivers key events");
    app.summon();
    type_key();
    require(sink.events.size() == 4, "relaunch preserves typing choice");
    app.reload();
    type_key();
    require(sink.events.size() == 6, "profile reload keeps an enabled matching backend usable");
    app.apply({"international-full", "de-de", "midnight"});
    type_key();
    require(sink.events.size() == 6, "mismatched language cannot type");
    app.apply({"en-us-full", "en-us", "midnight"});
    type_key();
    require(sink.events.size() == 8, "matching selection resumes without a pause button");
    app.set_dragging(true);
    type_key();
    require(sink.events.size() == 8, "drag capture suppresses keys");
    app.set_dragging(false);
    type_key();
    require(sink.events.size() == 10, "ending a drag restores typing");
    app.report_status("Tracking recovered");
    app.dirty = false;
    app.set_dragging(false);
    require(app.view().status == "Tracking recovered" && !app.dirty,
            "repeated drag cleanup leaves status and rendering alone");
    type_a(app, false);
    app.set_dragging(false);
    require(sink.events.size() == 11 && sink.events.back() == std::pair<int, int>{30, 1},
            "repeated drag cleanup cannot cancel a newly held key");
    app.cancel();
    sink.ready = false;
    app.tick(2);
    type_key();
    require(sink.events.size() == 12, "lost backend disables typing");
    require(app.view().status.find("connection lost") != std::string::npos, "lost backend is visible");
}
void connection_recovery_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    app.set_interaction_active(false);
    sink.ready = false;
    app.tick(1);
    app.set_interaction_active(true);
    require(app.view().status.find("connection lost") != std::string::npos,
            "disconnect while hidden reports connection loss instead of keymap mismatch");
    sink.recoverable = true;
    app.tick(2);
    require(app.view().status.find("Waiting for the compositor") != std::string::npos,
            "temporary pause has a distinct status");
    sink.ready = true;
    sink.reset = true;
    app.tick(3);
    require(app.view().status.empty(), "resume clears the pause status");
    type_a(app, false);
    require(sink.events.size() == 1, "fresh press works after resume");
    // A pause and resume delivered together must still discard the old UI hold.
    sink.reset = true;
    app.tick(4);
    require(!app.pointer_pressed(0) && sink.events.size() == 2,
            "same-batch interruption clears stale key state");
    app.tick(100);
    require(sink.events.size() == 2, "resuming cannot replay a stale repeat");
    app.set_interaction_active(false);
    sink.ready = false;
    app.tick(101);
    sink.ready = true;
    sink.reset = true;
    app.tick(102);
    type_a(app);
    require(sink.events.size() == 2, "resume while hidden cannot enable typing");
    app.set_interaction_active(true);
    type_a(app);
    require(sink.events.size() == 4, "visible typing recovers after a hidden pause");
}
void hidden_input_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    type_a(app, true, "ControlLeft");
    type_a(app, false);
    require(sink.events == std::vector<std::pair<int, int>>{{29, 1}, {30, 1}},
            "modified key held before hiding");
    app.set_interaction_active(false);
    require(sink.events.back() == std::pair{29, 0}, "hide releases modifier before disabling gate");
    type_a(app);
    app.summon();
    app.reload();
    app.apply({"en-us-full", "en-us", "graphite"});
    type_a(app);
    app.tick(20);
    require(sink.events.size() == 4, "hidden dashboard blocks clicks, repeat, reload and relaunch");
    app.set_interaction_active(true);
    type_a(app, false);
    require(sink.events.back() == std::pair{30, 1}, "reopening permits fresh press without stale Ctrl");
    app.set_interaction_active(false);
    require(sink.events.back() == std::pair{30, 0}, "hide releases ordinary held key");
    const auto count = sink.events.size();
    app.tick(50);
    require(sink.events.size() == count, "hidden held key cannot repeat");
    sink.ready = false;
    app.set_interaction_active(true);
    type_a(app);
    require(sink.events.size() == count, "dashboard reopening cannot revive disconnected input");
}
void language_recovery_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    {
        fk::App first(options, sink);
        first.apply({"international-full", "de-de", "graphite"});
    }
    fk::App reopened(options, sink);
    type_a(reopened);
    require(sink.events.empty(), "saved mismatch opens without emitting input");
    reopened.show_settings();
    require(reopened.view().settings, "mismatched saved language leaves Settings reachable");
    reopened.apply({"en-us-full", "en-us", "graphite"});
    reopened.summon();
    type_a(reopened);
    require(sink.events.size() == 2, "matching selection recovers after reopening");

    const std::string original = R"PROFILE({
  "schema_version": 1,
  "id": "en-us",
  "name": "English (US)",
  "locale": "en-US",
  "keymap": {
    "rules": "evdev",
    "model": "pc105",
    "layout": "us",
    "variant": "",
    "options": []
  },
  "legends": {
    "source": "keymap",
    "overrides": {}
  },
  "font_families": [
    "Noto Sans",
    "sans-serif"
  ]
}
)PROFILE";
    auto changed = original;
    const auto offset = changed.find("\"layout\": \"us\"");
    require(offset != std::string::npos, "language fixture has a US keymap");
    changed.replace(offset, std::string("\"layout\": \"us\"").size(), "\"layout\": \"de\"");
    fk::fs::create_directories(temp.path / "languages");
    const auto path = temp.path / "languages/en-us.json";
    {
        std::ofstream file(path);
        file << changed;
    }
    reopened.reload();
    require(reopened.view().language->keymap == "de", "same-ID edit loads for preview");
    type_a(reopened);
    reopened.apply({"en-us-full", "en-us", "midnight"});
    type_a(reopened);
    require(sink.events.size() == 2, "Reload and Apply cannot redefine the launch target keymap");
    reopened.flush_settings(); // The following Reload exercises idle profile preparation.
    {
        std::ofstream file(path);
        file << original;
    }
    reopened.reload();
    type_a(reopened);
    require(sink.events.size() == 4, "restoring the declared keymap restores typing");

    options.start_enabled = false;
    fk::App preview(options, sink);
    preview.reload();
    type_a(preview);
    require(sink.events.size() == 4, "recovery never enables a launch without explicit opt-in");
}
void ray_hit_tests() {
    // Upright panel 1 m ahead of the origin, facing back toward it (+z).
    fk::Transform panel{{{1, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, -1}}};
    const double aspect = .625;
    auto hit = fk::panel_ray_hit(panel, 1, aspect, {0, 1, 0}, {0, 0, -1});
    require(hit.has_value(), "a ray at the center hits the panel");
    near(hit->u, .5, "center hit is horizontally centered");
    near(hit->v, .5, "center hit is vertically centered");
    near(hit->distance, 1, "hit distance is in meters");
    near(hit->point[2], -1, "hit point lies on the panel");
    // Toward a point a quarter in from the left and top edges.
    const std::array<double, 3> corner{-.25, 1 + .25 * aspect, -1};
    const double length = std::hypot(corner[0], corner[1] - 1, corner[2]);
    hit = fk::panel_ray_hit(panel, 1, aspect, {0, 1, 0},
                            {corner[0] / length, (corner[1] - 1) / length, corner[2] / length});
    require(hit.has_value(), "an off-center ray hits the panel");
    near(hit->u, .25, "u runs from the left edge");
    near(hit->v, .25, "v runs from the top edge");
    require(!fk::panel_ray_hit(panel, 1, aspect, {0, 1, 0}, {0, 0, 1}), "a ray pointing away misses");
    require(!fk::panel_ray_hit(panel, 1, aspect, {0, 1, -2}, {0, 0, 1}),
            "a ray from behind the panel misses");
    require(!fk::panel_ray_hit(panel, 1, aspect, {.6, 1, 0}, {0, 0, -1}),
            "a ray beside the panel misses");
    require(!fk::panel_ray_hit(panel, 1, aspect, {0, 1, -1.5}, {0, 0, -1}),
            "the panel behind the ray's origin is not hit");
    // The tilted default placement is hit at its center from straight above its normal.
    fk::PanelPlacement placed;
    placed.recenter(head());
    const auto pose = placed.transform();
    const std::array<double, 3> normal{pose[0][2], pose[1][2], pose[2][2]};
    hit = fk::panel_ray_hit(
        pose, placed.width(), aspect,
        {pose[0][3] + normal[0] * .4, pose[1][3] + normal[1] * .4, pose[2][3] + normal[2] * .4},
        {-normal[0], -normal[1], -normal[2]});
    require(hit.has_value(), "a ray along the normal hits the recentered panel");
    near(hit->u, .5, "tilted center hit is horizontally centered");
    near(hit->v, .5, "tilted center hit is vertically centered");
    near(hit->distance, .4, "tilted hit distance follows the normal");
}
void resizer_tests() {
    // Upright panel 1 m wide, centered 1 m ahead at eye height 1, facing +z.
    const fk::Transform panel{{{1, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, -1}}};
    const fk::PanelOffset right{.5, -.1875}, left{-.5, -.1875};
    // The default theme's 20 px case radius on a 1600 px, 1 m panel.
    const fk::ResizerShape shape{.0125};
    auto toward = [](const fk::Vector3& target) {
        return fk::Ray{{target[0], target[1], 0}, {0, 0, -1}};
    };
    const auto pose = fk::resizer_transform(panel, right);
    near(pose[0][3], .5, "the resizer is centered on the case corner");
    near(pose[1][3], .8125, "the resizer sits at the case's bottom edge");
    near(pose[2][3], -.997, "the resizer floats just in front of the panel");
    near(pose[0][0], 1, "the resizer shares the panel's rotation");

    // The bracket is concentric with the case corner a few millimeters out.
    const auto bend = fk::resizer_target(panel, right, fk::ResizeCorner::Right, shape);
    const double from_arc_center =
        std::hypot(bend[0] - (.5 - shape.case_radius), bend[1] - (.8125 + shape.case_radius));
    require(from_arc_center > shape.case_radius + .003 && from_arc_center < shape.case_radius + .008,
            "the bracket follows the case's corner radius at a small gap");
    require(bend[0] > .5 && bend[1] < .8125, "the bracket bends outside the case corner");
    const auto mirrored = fk::resizer_target(panel, left, fk::ResizeCorner::Left, shape);
    near(mirrored[0], -bend[0], "the left bracket mirrors the right");
    // Proximity follows the ray itself, not a pointer on the keyboard surface.
    near(fk::ray_distance(toward(bend), bend), 0, "a ray through the bracket has no distance");
    near(fk::ray_distance({{bend[0] + .03, bend[1], 0}, {0, 0, -1}}, bend), .03,
         "distance is perpendicular to the ray");
    near(fk::ray_distance({{bend[0], bend[1], -2}, {0, 0, -1}}, bend), 1,
         "a point behind the ray's origin measures from the origin");
    near(fk::resizer_proximity(0), 1, "a laser on the bracket shows it fully");
    near(fk::resizer_proximity(.035), 1, "a laser a few centimeters away still shows it fully");
    near(fk::resizer_proximity(.06), .5, "the resizer fades between near and far");
    near(fk::resizer_proximity(.085), 0, "a distant laser hides it");
    near(fk::resizer_proximity(1), 0, "a laser elsewhere hides it");

    // The quadrant over the case never takes the laser, so corner keys stay reachable.
    auto hits = [&](fk::PanelOffset corner, fk::ResizeCorner side, double x, double y) {
        return fk::resizer_hit(panel, corner, side, shape, toward({x, y, -1})).has_value();
    };
    require(hits(right, fk::ResizeCorner::Right, .51, .82), "beside the right edge takes the laser");
    require(hits(right, fk::ResizeCorner::Right, .51, .80), "past the corner takes the laser");
    require(hits(right, fk::ResizeCorner::Right, .49, .80), "below the bottom edge takes the laser");
    require(!hits(right, fk::ResizeCorner::Right, .49, .82), "the case corner keeps the laser");
    require(!hits(right, fk::ResizeCorner::Right, .56, .80), "beyond the resizer misses");
    require(hits(left, fk::ResizeCorner::Left, -.51, .82), "beside the left edge takes the laser");
    require(!hits(left, fk::ResizeCorner::Left, -.49, .82), "the left case corner keeps the laser");
    require(!fk::resizer_hit(panel, right, fk::ResizeCorner::Right, shape, {{.51, .82, 0}, {0, 0, 1}}),
            "a ray pointing away misses the resizer");

    // The image grows with the radius so the legs always fit.
    require(fk::ResizerShape{.0125}.pixels() == 128, "the default resizer is 6.4 cm at 20 px/cm");
    const fk::ResizerShape large{.025};
    require(large.half_size() >= large.case_radius + .014 && large.pixels() % 2 == 0,
            "a large keyboard's resizer keeps room for its legs");
    const int size = shape.pixels();
    const auto right_pixels = fk::resizer_pixels(fk::ResizeCorner::Right, shape);
    const auto left_pixels = fk::resizer_pixels(fk::ResizeCorner::Left, shape);
    require(right_pixels.size() == static_cast<std::size_t>(size * size * 4), "RGBA bracket image");
    auto alpha = [&](const std::vector<std::uint8_t>& pixels, int x, int y) {
        return pixels[static_cast<std::size_t>((y * size + x) * 4 + 3)];
    };
    // 1.5 cm inside both case edges: over the keys, behind the rounded corner.
    require(alpha(right_pixels, size / 2 - 30, size / 2 - 30) == 0, "the case's interior stays clear");
    std::uint8_t strongest = 0;
    bool mirror = true, clear_edges = true;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            strongest = std::max(strongest, alpha(right_pixels, x, y));
            mirror &= alpha(right_pixels, x, y) == alpha(left_pixels, size - 1 - x, y);
            if (x == 0 || y == 0 || x == size - 1 || y == size - 1) {
                clear_edges &= alpha(right_pixels, x, y) == 0;
            }
        }
    }
    require(strongest > 200 && mirror, "a solid bracket, mirrored for the left corner");
    require(clear_edges, "the bracket is never clipped by the overlay's edges");
    const auto large_pixels = fk::resizer_pixels(fk::ResizeCorner::Right, large);
    bool large_clear = true;
    for (int i = 0; i < large.pixels(); ++i) {
        large_clear &= large_pixels[static_cast<std::size_t>(i * 4 + 3)] == 0;
    }
    require(large_clear, "a large radius still fits its overlay");
    const auto mask = fk::resizer_mask(fk::ResizeCorner::Right, shape);
    require(mask[0] == std::array<float, 4>{64, 0, 64, 128} &&
                mask[1] == std::array<float, 4>{0, 64, 64, 64},
            "the right mask covers the outward half and the quadrant below the case");

    fk::ResizerFade fade;
    near(fade.update(1, 10), 0, "a fade starts from hidden");
    near(fade.update(1, 10.06), .5, "fading in takes 0.12 s");
    near(fade.update(1, 11), 1, "a long frame finishes the fade");
    near(fade.update(0, 11.1), .5, "fading out takes 0.2 s");
    near(fade.hold(11.11), 1, "a held resizer is fully opaque at once");
    near(fade.update(1, 11.2), 1, "releasing near the laser keeps it opaque");
    fade.reset();
    near(fade.value(), 0, "reset hides immediately");

    // Grab the bottom-right corner; the center stays put.
    fk::PanelResize resize;
    require(resize.begin(panel, 1, toward({.5, .8125, -1})), "a press on the plane starts");
    auto width = resize.update(toward({.5, .8125, -1}));
    require(width.has_value(), "a press on the plane has a width");
    near(*width, 1, "pressing never jumps");
    auto along = [&](double t) { return toward({.5 * t, 1 - .1875 * t, -1}); };
    width = resize.update(along(1.5));
    require(width.has_value(), "dragging along the diagonal resizes");
    near(*width, 1.5, "the dragged corner follows the laser about the fixed center");
    // Sideways motion, perpendicular to the center's diagonal, changes nothing.
    const auto sideways = toward({.5 + .1875 * .1, .8125 + .5 * .1, -1});
    near(*resize.update(sideways), 1, "perpendicular motion keeps the size");
    near(*resize.update(along(5)), 2, "the maximum width still applies");
    near(*resize.update(along(.1)), .45, "the minimum width still applies");
    // Any corner works: the opposite one grows the keyboard the same way.
    fk::PanelResize from_left;
    require(from_left.begin(panel, 1, toward({-.5, .8125, -1})), "the left corner starts too");
    near(*from_left.update(toward({-.75, 1 - .1875 * 1.5, -1})), 1.5,
         "dragging the left corner outward grows the keyboard");
    require(!resize.update({{.5, .8, 0}, {0, 0, 1}}), "a ray off the plane keeps the last size");
    require(!resize.begin(panel, 1, toward({0, 1, -1})), "a press at the center cannot start");

    fk::MoveCapture capture;
    require(capture.begin(fk::MoveSource::Resize, 3, 1) && capture.owns_pointer(3, 1) &&
                !capture.owns_pointer(3, 0),
            "a resize belongs to its laser's pointer");
}
void passthrough_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    auto find = [](const fk::App& app, const std::string& id) -> std::optional<fk::Control> {
        for (const auto& control : app.view().controls) {
            if (control.id == id) {
                return control;
            }
        }
        return {};
    };
    fk::App app(options, sink);
    require(!app.passthrough(), "passthrough starts off");
    const auto pin = find(app, "pin");
    auto button = find(app, "passthrough");
    require(pin && button && button->icon == fk::Icon::PassthroughOff && !button->selected,
            "passthrough button shows its off state");
    require(button->bounds.x >= pin->bounds.x + pin->bounds.width,
            "passthrough button follows the pin button");
    const double x = button->bounds.x + button->bounds.width / 2;
    const double y = button->bounds.y + button->bounds.height / 2;
    app.down(0, x, y, 1);
    app.up(0, x, y);
    button = find(app, "passthrough");
    require(app.passthrough() && button->icon == fk::Icon::PassthroughOn && button->selected,
            "clicking turns passthrough on");
    require(!app.pinned(), "passthrough leaves pin unchanged");
    app.flush_settings();
    fk::App restarted(options, sink);
    require(!restarted.passthrough(), "passthrough is not saved");
    app.down(0, x, y, 2);
    app.up(0, x, y);
    require(!app.passthrough(), "clicking again turns passthrough off");
}
void ui_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    fk::App app(options, sink);
    require(!app.view().settings, "keyboard starts on the main view");
    for (const auto& control : app.view().controls) {
        require(control.id != "position" && control.id != "input" && control.id != "release" &&
                    control.id != "size-smaller" && control.id != "size-larger",
                "removed toolbar controls absent; corner resizers replace the size icons");
    }
    // Corner resizers work in either movement mode; the pin hides them.
    require(app.allows_movement(fk::MoveSource::Resize), "an unpinned keyboard can be resized");
    for (const auto& control : app.view().controls) {
        if (control.id == "pin") {
            const double x = control.bounds.x + control.bounds.width / 2;
            const double y = control.bounds.y + control.bounds.height / 2;
            app.down(0, x, y, 1);
            app.up(0, x, y);
        }
    }
    require(app.pinned() && !app.allows_movement(fk::MoveSource::Resize),
            "a pinned keyboard cannot be resized");
    for (const auto& control : app.view().controls) {
        if (control.id == "pin") {
            const double x = control.bounds.x + control.bounds.width / 2;
            const double y = control.bounds.y + control.bounds.height / 2;
            app.down(0, x, y, 2);
            app.up(0, x, y);
        }
    }
    require(!app.pinned(), "the pin can be released again");
    app.summon();
    require(!app.view().settings && app.take_recenter(),
            "summon returns to keyboard and requests recenter");
    require(!app.take_recenter(), "recenter request consumed once");
}
} // namespace
int main() {
    try {
        placement_tests();
        ray_hit_tests();
        resizer_tests();
        passthrough_tests();
        dashboard_tab_tests();
        drag_tests();
        horizon_tests();
        dashboard_leveling_tests();
        stick_depth_tests();
        grip_tests();
        trigger_release_tests();
        persistence_tests();
        key_feedback_tests();
        haptic_routing_tests();
        instance_tests();
        ui_tests();
        typing_tests();
        hidden_input_tests();
        connection_recovery_tests();
        language_recovery_tests();
        std::cout << "Placement, repeated-launch IPC, stale-owner recovery and placement UI passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
