#pragma once

namespace framekeyboard {
// The guard needs a short timer only for an armed restore deadline and while a
// restore waits for its keyboard to open. Otherwise helper IPC wakes it
// immediately; the idle timeout only keeps OpenVR quit handling responsive.
//
// After a restore it waits for Steam to report its keyboard open, then closed.
// Once open, the user may type on it for minutes, and noticing the close late
// only delays the guard's exit, so it watches at the idle pace. If the open
// report never comes within native_guard_open_wait seconds, it keeps watching
// at that pace too: it must stay, since a keyboard it reopened closes when it
// exits.
constexpr double native_guard_open_wait = 2;
constexpr int native_guard_poll_ms(bool armed, bool restored, bool saw_open = false,
                                   double since_restore = 0) {
    const bool opening = restored && !saw_open && since_restore < native_guard_open_wait;
    return armed || opening ? 25 : 250;
}
} // namespace framekeyboard
