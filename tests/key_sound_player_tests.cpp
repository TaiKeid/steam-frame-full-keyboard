#include "fake_pipewire.hpp"
#include "framekeyboard/key_sounds.hpp"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace framekeyboard;
using namespace std::chrono_literals;
namespace {
void require(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}
struct Fake {
    decltype(&framekeyboard_test_sound_snapshot) read_snapshot;
    decltype(&framekeyboard_test_sound_hold_drain) hold_drain;
    decltype(&framekeyboard_test_sound_fail_active) fail_active;
    decltype(&framekeyboard_test_sound_fail_flush) fail_flush;
    decltype(&framekeyboard_test_sound_samples) samples;
    Fake() {
        // Validate the substitute before constructing a player. A bad test
        // environment must exit rather than connect to the real audio server.
        void* library = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(library, "the private sound substitute loads");
        read_snapshot = reinterpret_cast<decltype(read_snapshot)>(
            dlsym(library, "framekeyboard_test_sound_snapshot"));
        hold_drain = reinterpret_cast<decltype(hold_drain)>(
            dlsym(library, "framekeyboard_test_sound_hold_drain"));
        fail_active = reinterpret_cast<decltype(fail_active)>(
            dlsym(library, "framekeyboard_test_sound_fail_active"));
        fail_flush = reinterpret_cast<decltype(fail_flush)>(
            dlsym(library, "framekeyboard_test_sound_fail_flush"));
        samples =
            reinterpret_cast<decltype(samples)>(dlsym(library, "framekeyboard_test_sound_samples"));
        require(read_snapshot && hold_drain && fail_active && fail_flush && samples,
                "only the private sound substitute may run");
    }
    SoundPlayerSnapshot snapshot() const {
        SoundPlayerSnapshot result;
        read_snapshot(&result);
        return result;
    }
};
template <class Done> void wait_for(Done done, const char* message, std::chrono::seconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(done(), message);
}
void quiet(Fake& fake) {
    const auto before = fake.snapshot().callbacks;
    std::this_thread::sleep_for(50ms);
    require(fake.snapshot().callbacks == before, "an inactive stream requests no silent buffers");
}
void complete_sound(Fake& fake, std::size_t offset, KeyFeedback kind) {
    const auto expected = key_sound(kind, 48000, "e");
    std::vector<float> actual(fake.samples(nullptr, 0));
    fake.samples(actual.data(), actual.size());
    require(actual.size() >= offset + expected.size(), "all 60 ms of sound were queued");
    require(std::equal(expected.begin(), expected.end(), actual.begin() + offset),
            "a sound includes its complete waveform and tail");
}
void playback_and_idle(Fake& fake) {
    {
        KeySounds player;
        player.set_awake(true);
        wait_for([&] { return fake.snapshot().connections == 1; }, "the player connects");
        require(fake.snapshot().inactive_connections == 1 && fake.snapshot().realtime_connections == 1,
                "the stream starts inactive and retains real-time processing");
        quiet(fake);

        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().deactivations == 1; }, "the first sound drains");
        auto first = fake.snapshot();
        require(first.drains_completed == 1 && first.queued_frames == first.played_frames,
                "deactivation follows playback of every queued frame");
        complete_sound(fake, 0, KeyFeedback::Press);
        quiet(fake);

        const auto second_at = first.queued_frames;
        player.play(KeyFeedback::Release, "e", 100);
        wait_for([&] { return fake.snapshot().deactivations == 2; }, "a later request reactivates");
        complete_sound(fake, second_at, KeyFeedback::Release);
        quiet(fake);

        fake.hold_drain(true);
        const auto third_at = fake.snapshot().queued_frames;
        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().drains_requested == 3; }, "the third sound is draining");
        const auto waiting_at = fake.snapshot();
        player.play(KeyFeedback::Release, "e", 100);
        std::this_thread::sleep_for(20ms);
        require(fake.snapshot().activations == waiting_at.activations,
                "a new sound waits for the old drain rather than resuming it");
        fake.hold_drain(false);
        wait_for([&] { return fake.snapshot().deactivations == 4; }, "the waiting sound also plays");
        complete_sound(fake, third_at, KeyFeedback::Press);
        complete_sound(fake, waiting_at.queued_frames, KeyFeedback::Release);
        require(fake.snapshot().resumed_during_drain == 0, "a pending drain is never resumed");

        fake.hold_drain(true);
        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().drains_requested == 5; }, "a drain can be hidden");
        player.play(KeyFeedback::Release, "e", 100);
        player.set_awake(false);
        wait_for([&] { return fake.snapshot().disconnections == 1; }, "hide disconnects while draining");
        fake.hold_drain(false);
        player.set_awake(true);
        wait_for([&] { return fake.snapshot().connections == 2; }, "show reconnects");
        quiet(fake); // The release queued before hide must not replay.
        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().deactivations == 5; }, "typing after show still works");
    }
    require(fake.snapshot().disconnections == 2, "shutdown disconnects an inactive player");
}
void shutdown_during_drain(Fake& fake) {
    const auto before = fake.snapshot();
    {
        KeySounds player;
        fake.hold_drain(true);
        player.set_awake(true);
        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().drains_requested > before.drains_requested; },
                 "the final player is draining");
    }
    require(fake.snapshot().disconnections == before.disconnections + 1,
            "shutdown does not wait for drain completion");
    fake.hold_drain(false);
}
void control_failure_recovery(Fake& fake, bool activating) {
    const auto before = fake.snapshot();
    KeySounds player;
    fake.fail_active(activating);
    player.set_awake(true);
    player.play(KeyFeedback::Press, "e", 100);
    wait_for([&] { return fake.snapshot().disconnections > before.disconnections; },
             "a failed activation or deactivation disconnects");
    require(fake.snapshot().control_failures == before.control_failures + 1,
            "the injected stream control failure was consumed");
    wait_for([&] { return fake.snapshot().connections == before.connections + 2; },
             "a failed connection retries", 7s);
    quiet(fake); // Failed requests must not play after reconnect.
    const auto drained = fake.snapshot().drains_completed;
    player.play(KeyFeedback::Press, "e", 100);
    wait_for([&] { return fake.snapshot().drains_completed > drained; },
             "new sound requests work after a stream control failure");
}
void flush_failure_recovery(Fake& fake) {
    const auto before = fake.snapshot();
    KeySounds player;
    fake.fail_flush();
    player.set_awake(true);
    player.play(KeyFeedback::Press, "e", 100);
    wait_for([&] { return fake.snapshot().flush_failures > before.flush_failures; },
             "the drain flush fails");
    const auto queued = fake.snapshot().queued_frames;
    wait_for([&] { return fake.snapshot().disconnections > before.disconnections; },
             "a failed drain disconnects without RT locking");
    require(fake.snapshot().queued_frames == queued,
            "a failed drain stops supplying silent buffers while recovery waits");
    wait_for([&] { return fake.snapshot().connections == before.connections + 2; },
             "a failed drain retries", 7s);
    quiet(fake);
    const auto drained = fake.snapshot().drains_completed;
    player.play(KeyFeedback::Press, "e", 100);
    wait_for([&] { return fake.snapshot().drains_completed > drained; },
             "new sounds work after a failed drain");
}
} // namespace
int main() {
    try {
        Fake fake;
        playback_and_idle(fake);
        shutdown_during_drain(fake);
        control_failure_recovery(fake, true);
        control_failure_recovery(fake, false);
        flush_failure_recovery(fake);
    } catch (const std::exception& error) {
        std::cerr << "sound player test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "sound player tests passed\n";
}
