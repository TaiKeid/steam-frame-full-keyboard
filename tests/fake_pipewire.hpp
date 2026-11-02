#pragma once

#include <cstddef>
#include <cstdint>

struct SoundPlayerSnapshot {
    unsigned connections{}, disconnections{}, callbacks{};
    unsigned active_connections{}, realtime_connections{}, connect_failures{};
    std::uint64_t queued_frames{}, audible_frames{};
};

extern "C" {
void framekeyboard_test_sound_snapshot(SoundPlayerSnapshot* result);
void framekeyboard_test_sound_pause_processing(bool paused);
void framekeyboard_test_sound_fail_connect();
std::size_t framekeyboard_test_sound_samples(float* destination, std::size_t capacity);
}
