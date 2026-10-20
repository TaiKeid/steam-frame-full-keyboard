#pragma once

namespace framekeyboard {
// The guard needs a short timer only for an armed restore deadline and while a
// restored native keyboard is open. Otherwise helper IPC wakes it immediately;
// the idle timeout only keeps OpenVR quit handling responsive.
constexpr int native_guard_poll_ms(bool armed, bool restored) {
    return armed || restored ? 25 : 250;
}
} // namespace framekeyboard
