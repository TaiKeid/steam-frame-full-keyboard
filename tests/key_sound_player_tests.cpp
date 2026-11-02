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
    decltype(&framekeyboard_test_sound_pause_processing) pause_processing;
    decltype(&framekeyboard_test_sound_fail_connect) fail_connect;
    decltype(&framekeyboard_test_sound_samples) samples;
    Fake() {
        // Validate the substitute before constructing a player. A bad test
        // environment must exit rather than connect to the real audio server.
        void* library = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(library, "the private sound substitute loads");
        read_snapshot = reinterpret_cast<decltype(read_snapshot)>(
            dlsym(library, "framekeyboard_test_sound_snapshot"));
        pause_processing = reinterpret_cast<decltype(pause_processing)>(
            dlsym(library, "framekeyboard_test_sound_pause_processing"));
        fail_connect = reinterpret_cast<decltype(fail_connect)>(
            dlsym(library, "framekeyboard_test_sound_fail_connect"));
        samples =
            reinterpret_cast<decltype(samples)>(dlsym(library, "framekeyboard_test_sound_samples"));
        require(read_snapshot && pause_processing && fail_connect && samples,
                "only the private sound substitute may run");
    }
    SoundPlayerSnapshot snapshot() const {
        SoundPlayerSnapshot result;
        read_snapshot(&result);
        return result;
    }
    std::vector<float> recording() const {
        // Playback may append meanwhile; this obtains a stable earlier prefix.
        std::vector<float> result(samples(nullptr, 0));
        samples(result.data(), result.size());
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
void quiet_but_warm(Fake& fake) {
    const auto before = fake.snapshot();
    wait_for([&] { return fake.snapshot().callbacks >= before.callbacks + 3; },
             "a warm stream keeps making progress between sounds");
    const auto after = fake.snapshot();
    require(after.callbacks > before.callbacks && after.queued_frames > before.queued_frames,
            "a warm stream continues processing between sounds");
    require(after.audible_frames == before.audible_frames,
            "the warm stream supplies silence while no sound is pending");
    require(after.disconnections == before.disconnections,
            "idle sound gaps do not close the warm output");
}
void complete_sound(Fake& fake, std::size_t offset, KeyFeedback kind) {
    const auto expected = key_sound(kind, 48000, "e");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool found = false;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        const auto actual = fake.recording();
        if (offset <= actual.size()) {
            found = std::search(actual.begin() + static_cast<std::ptrdiff_t>(offset), actual.end(),
                                expected.begin(), expected.end()) != actual.end();
        }
        if (!found) {
            std::this_thread::sleep_for(1ms);
        }
    }
    require(found, "the warm stream queues the complete waveform and tail");
}
void playback_and_sleep(Fake& fake) {
    KeySounds player;
    player.set_awake(false);
    require(fake.snapshot().connections == 0, "sleeping initially does not create a stream");
    player.set_awake(true);
    wait_for([&] { return fake.snapshot().connections == 1; }, "the warm output connects");
    require(fake.snapshot().active_connections == 1 && fake.snapshot().realtime_connections == 1,
            "the stream starts active with real-time processing");
    quiet_but_warm(fake);

    for (const auto kind :
         {KeyFeedback::Press, KeyFeedback::Release, KeyFeedback::Press, KeyFeedback::Release}) {
        const auto offset = static_cast<std::size_t>(fake.snapshot().queued_frames);
        player.play(kind, "e", 100);
        complete_sound(fake, offset, kind);
        quiet_but_warm(fake);
    }
    require(fake.snapshot().connections == 1, "separate sounds retain the same warm connection");

    // Queue a request that the data thread cannot consume, then sleep.
    fake.pause_processing(true);
    player.play(KeyFeedback::Release, "e", 100);
    player.set_awake(false);
    wait_for([&] { return fake.snapshot().disconnections == 1; }, "sleep disconnects the output");
    const auto sleeping = fake.snapshot();
    fake.pause_processing(false);
    player.play(KeyFeedback::Press, "e", 100);
    std::this_thread::sleep_for(35ms);
    require(fake.snapshot().callbacks == sleeping.callbacks &&
                fake.snapshot().connections == sleeping.connections,
            "sleeping neither processes nor connects for a sound request");

    player.set_awake(true);
    wait_for([&] { return fake.snapshot().connections == 2; }, "warming reconnects after sleep");
    quiet_but_warm(fake);
    require(fake.snapshot().audible_frames == sleeping.audible_frames,
            "a request queued before sleep does not replay when warming again");
    const auto offset = static_cast<std::size_t>(fake.snapshot().queued_frames);
    player.play(KeyFeedback::Press, "e", 100);
    complete_sound(fake, offset, KeyFeedback::Press);
}
void shutdown_while_playing(Fake& fake) {
    const auto before = fake.snapshot();
    std::chrono::steady_clock::time_point shutdown_started;
    {
        KeySounds player;
        player.set_awake(true);
        wait_for([&] { return fake.snapshot().connections > before.connections; },
                 "shutdown fixture connects");
        player.play(KeyFeedback::Press, "e", 100);
        wait_for([&] { return fake.snapshot().audible_frames > before.audible_frames; },
                 "shutdown fixture starts a pop");
        shutdown_started = std::chrono::steady_clock::now();
    }
    require(fake.snapshot().disconnections == before.disconnections + 1,
            "shutdown destroys a playing stream");
    require(std::chrono::steady_clock::now() - shutdown_started < 2s,
            "shutdown does not wait for audio to drain");
}
void failed_connection_recovery(Fake& fake) {
    const auto before = fake.snapshot();
    KeySounds player;
    fake.fail_connect();
    player.set_awake(true);
    player.play(KeyFeedback::Press, "e", 100);
    wait_for([&] { return fake.snapshot().disconnections > before.disconnections; },
             "a failed connection is destroyed");
    require(fake.snapshot().connect_failures == before.connect_failures + 1,
            "the connection failure was injected");
    wait_for([&] { return fake.snapshot().connections == before.connections + 2; },
             "a failed warm connection retries", 7s);
    quiet_but_warm(fake);
    require(fake.snapshot().audible_frames == before.audible_frames,
            "failed requests do not replay after retry");
    const auto offset = static_cast<std::size_t>(fake.snapshot().queued_frames);
    player.play(KeyFeedback::Press, "e", 100);
    complete_sound(fake, offset, KeyFeedback::Press);
}
} // namespace
int main() {
    try {
        Fake fake;
        playback_and_sleep(fake);
        shutdown_while_playing(fake);
        failed_connection_recovery(fake);
        require(fake.snapshot().connections == fake.snapshot().disconnections,
                "every connection attempt has been destroyed");
    } catch (const std::exception& error) {
        std::cerr << "sound player test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "warm sound player tests passed\n";
}
