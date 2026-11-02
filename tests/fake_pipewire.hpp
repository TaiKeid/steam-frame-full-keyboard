#pragma once

#include <cstddef>
#include <cstdint>

struct SoundPlayerSnapshot {
    unsigned connections{}, disconnections{}, callbacks{}, activations{}, deactivations{};
    unsigned drains_requested{}, drains_completed{}, resumed_during_drain{};
    unsigned inactive_connections{}, realtime_connections{};
    unsigned control_failures{};
    unsigned flush_failures{};
    unsigned loop_locks{}, events_created{}, events_destroyed{}, events_signaled{};
    unsigned signal_interruptions{};
    std::uint64_t queued_frames{}, played_frames{};
};

extern "C" {
void framekeyboard_test_sound_snapshot(SoundPlayerSnapshot* result);
void framekeyboard_test_sound_hold_drain(bool hold);
void framekeyboard_test_sound_fail_active(bool activating);
void framekeyboard_test_sound_fail_flush();
void framekeyboard_test_sound_pause_processing(bool paused);
void framekeyboard_test_sound_interrupt_signal();
std::size_t framekeyboard_test_sound_samples(float* destination, std::size_t capacity);
}
