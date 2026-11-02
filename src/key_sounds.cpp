#include "framekeyboard/key_sounds.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <numbers>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef FRAMEKEYBOARD_PIPEWIRE
#include <dlfcn.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace framekeyboard {
namespace {
// Tuned by ear on the Frame's speakers. Pitch barely moves: a wide sweep sounds
// like a laser, not a pop. Overtones carry the lowest ones on small speakers.
struct Pop {
    double press_from, press_to, release_from, release_to;
    double pitch_drop, decay;
    double second{}, third{}; // Overtone levels.
    double tick{};            // A burst of soft noise at the start.
};
// Three pitches, high to low, each plain and then with a click.
constexpr std::array<Pop, 6> pops{{
    {300, 240, 370, 300, .004, .012},
    {280, 230, 350, 290, .003, .007, 0, 0, .25},
    {230, 185, 285, 230, .004, .013},
    {230, 185, 285, 230, .004, .013, 0, 0, .25},
    {165, 135, 205, 168, .003, .008, .15},
    {165, 135, 205, 168, .003, .008, .15, 0, .3},
}};
std::size_t effect_index(const std::string& effect) {
    return effect.size() == 1 && effect[0] >= 'a' &&
                   static_cast<std::size_t>(effect[0] - 'a') < pops.size()
               ? static_cast<std::size_t>(effect[0] - 'a')
               : 0;
}
} // namespace

float sound_gain(int volume) {
    return std::pow(static_cast<float>(volume) / 100, 5.f / 3);
}

std::vector<float> key_sound(KeyFeedback kind, int rate, const std::string& effect) {
    if (kind == KeyFeedback::Hover) {
        return {};
    }
    const Pop& pop = pops[effect_index(effect)];
    const bool press = kind == KeyFeedback::Press;
    const double from = press ? pop.press_from : pop.release_from;
    const double to = press ? pop.press_to : pop.release_to;
    const double attack = .0005, tick_decay = .0012, fade = .008, length = .06, peak = .28;
    std::vector<float> samples(static_cast<std::size_t>(length * rate));
    std::minstd_rand noise(1); // The same tick every time, and for every key.
    double phase = 0, soft_noise = 0, loudest = 0;
    std::vector<double> shape(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double time = static_cast<double>(i) / rate;
        phase += 2 * std::numbers::pi * (to + (from - to) * std::exp(-time / pop.pitch_drop)) / rate;
        const double tone =
            std::sin(phase) + pop.second * std::sin(2 * phase) + pop.third * std::sin(3 * phase);
        const double white = 2 * static_cast<double>(noise()) / std::minstd_rand::max() - 1;
        soft_noise += .25 * (white - soft_noise);
        // The tail fades to exactly zero, so the end of a pop can't click.
        shape[i] = ((1 - std::exp(-time / attack)) * std::exp(-time / pop.decay) * tone +
                    pop.tick * std::exp(-time / tick_decay) * soft_noise) *
                   std::min(1., (length - time) / fade);
        loudest = std::max(loudest, std::abs(shape[i]));
    }
    // Every effect peaks alike, so a choice never changes the volume.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(shape[i] / loudest * peak);
    }
    return samples;
}

#ifdef FRAMEKEYBOARD_PIPEWIRE
namespace {
constexpr int sound_rate = 48000;
// Some graph latency is unavoidable; this much keeps a pop on the key without
// forcing the whole graph into very short cycles.
constexpr const char* sound_latency = "512/48000";
// How soon a keyboard in use tries PipeWire again.
constexpr auto connect_retry = std::chrono::seconds(5);

struct PipeWire {
    decltype(&::pw_init) init{};
    decltype(&::pw_properties_new) properties_new{};
    decltype(&::pw_thread_loop_new) thread_loop_new{};
    decltype(&::pw_thread_loop_get_loop) thread_loop_get_loop{};
    decltype(&::pw_thread_loop_start) thread_loop_start{};
    decltype(&::pw_thread_loop_stop) thread_loop_stop{};
    decltype(&::pw_thread_loop_lock) thread_loop_lock{};
    decltype(&::pw_thread_loop_unlock) thread_loop_unlock{};
    decltype(&::pw_thread_loop_destroy) thread_loop_destroy{};
    decltype(&::pw_stream_new_simple) stream_new_simple{};
    decltype(&::pw_stream_connect) stream_connect{};
    decltype(&::pw_stream_set_active) stream_set_active{};
    decltype(&::pw_stream_flush) stream_flush{};
    decltype(&::pw_stream_dequeue_buffer) stream_dequeue_buffer{};
    decltype(&::pw_stream_queue_buffer) stream_queue_buffer{};
    decltype(&::pw_stream_destroy) stream_destroy{};

    // Stays loaded for the process: PipeWire keeps global state.
    bool load() {
        void* library = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return false;
        }
        bool complete = true;
        auto bind = [&](auto& function, const char* name) {
            function =
                reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            complete = complete && function;
        };
        bind(init, "pw_init");
        bind(properties_new, "pw_properties_new");
        bind(thread_loop_new, "pw_thread_loop_new");
        bind(thread_loop_get_loop, "pw_thread_loop_get_loop");
        bind(thread_loop_start, "pw_thread_loop_start");
        bind(thread_loop_stop, "pw_thread_loop_stop");
        bind(thread_loop_lock, "pw_thread_loop_lock");
        bind(thread_loop_unlock, "pw_thread_loop_unlock");
        bind(thread_loop_destroy, "pw_thread_loop_destroy");
        bind(stream_new_simple, "pw_stream_new_simple");
        bind(stream_connect, "pw_stream_connect");
        bind(stream_set_active, "pw_stream_set_active");
        bind(stream_flush, "pw_stream_flush");
        bind(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
        bind(stream_queue_buffer, "pw_stream_queue_buffer");
        bind(stream_destroy, "pw_stream_destroy");
        return complete;
    }
};
} // namespace

struct KeySounds::Player {
    Player() : worker([this] { run(); }) {}
    ~Player() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }
    void set_awake(bool awake) {
        {
            std::lock_guard lock(mutex);
            wanted = awake;
        }
        changed.notify_all();
    }
    // A request packs its effect and volume, so it stays one atomic. Zero is none.
    void request(KeyFeedback kind, const std::string& effect, int volume) {
        const auto packed = static_cast<unsigned>(effect_index(effect) << 8 | 1u << 16) |
                            static_cast<unsigned>(std::clamp(volume, 0, 100));
        {
            std::lock_guard lock(mutex);
            (kind == KeyFeedback::Press ? press_requests : release_requests)
                .store(packed, std::memory_order_relaxed);
            ++request_generation;
        }
        changed.notify_all();
    }

    // Connection setup and activation happen here, so waiting for PipeWire
    // never holds up a VR frame. The connection lasts only while awake;
    // a hidden keyboard disconnects, and failed connections are retried.
    void run() {
        for (std::size_t effect = 0; effect < pops.size(); ++effect) {
            const std::string name(1, static_cast<char>('a' + effect));
            sounds[effect] = {key_sound(KeyFeedback::Press, sound_rate, name),
                              key_sound(KeyFeedback::Release, sound_rate, name)};
        }
        if (!pw.load()) {
            std::cerr << "Haptic sounds unavailable: PipeWire not found.\n";
            return;
        }
        pw.init(nullptr, nullptr);
        bool reported = false;
        std::unique_lock lock(mutex);
        while (true) {
            changed.wait(lock, [&] { return stopping || wanted; });
            if (stopping) {
                break;
            }
            broken = false;
            auto generation = request_generation;
            lock.unlock();
            voices = {}; // No data thread runs here; a pop cut off by sleep ends.
            draining.store(false, std::memory_order_relaxed);
            processing_failed.store(false, std::memory_order_relaxed);
            active = false;
            const bool connected = connect();
            lock.lock();
            while (connected && !stopping && wanted && !broken) {
                // PipeWire callbacks may acquire mutex. Never keep it while
                // taking the PipeWire loop lock, or a state callback can deadlock.
                lock.unlock();
                pw.thread_loop_lock(loop);
                bool failed = false;
                if (!active && !draining.load(std::memory_order_acquire) && pending()) {
                    failed = pw.stream_set_active(stream, true) < 0;
                    active = !failed;
                }
                const bool stream_active = active;
                pw.thread_loop_unlock(loop);
                lock.lock();
                broken |= failed;
                auto wake = [&] {
                    return stopping || !wanted || broken || request_generation != generation ||
                           processing_failed.load(std::memory_order_acquire);
                };
                if (stream_active) {
                    // The RT callback cannot take mutex to report a flush
                    // failure. Check it only during active playback/drain;
                    // an inactive stream sleeps without a periodic timer.
                    changed.wait_for(lock, std::chrono::milliseconds(100), wake);
                } else {
                    changed.wait(lock, wake);
                }
                broken |= processing_failed.load(std::memory_order_acquire);
                generation = request_generation;
            }
            const bool failed = !connected || broken;
            lock.unlock();
            disconnect();
            // Pops requested as the keyboard hid must not play when it returns.
            press_requests.store(0, std::memory_order_relaxed);
            release_requests.store(0, std::memory_order_relaxed);
            lock.lock();
            if (failed) {
                if (!std::exchange(reported, true)) {
                    std::cerr << "Haptic sounds lost PipeWire; trying again.\n";
                }
                changed.wait_for(lock, connect_retry, [&] { return stopping || !wanted; });
                // Pops pressed meanwhile are too late to play.
                press_requests.store(0, std::memory_order_relaxed);
                release_requests.store(0, std::memory_order_relaxed);
            } else {
                reported = false;
            }
        }
    }
    bool connect() {
        loop = pw.thread_loop_new("framekeyboard-sounds", nullptr);
        if (!loop) {
            return false;
        }
        static const pw_stream_events events = [] {
            pw_stream_events result{};
            result.version = PW_VERSION_STREAM_EVENTS;
            // PipeWire going away reads as unconnected, a stream error as error.
            result.state_changed = [](void* player, pw_stream_state, pw_stream_state state,
                                      const char*) {
                if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
                    static_cast<Player*>(player)->lost();
                }
            };
            result.process = [](void* player) { static_cast<Player*>(player)->process(); };
            result.drained = [](void* player) { static_cast<Player*>(player)->drained(); };
            return result;
        }();
        stream = pw.stream_new_simple(
            pw.thread_loop_get_loop(loop), "Haptic sounds",
            pw.properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                              PW_KEY_MEDIA_ROLE, "Notification", PW_KEY_APP_NAME, "Full Keyboard",
                              PW_KEY_NODE_LATENCY, sound_latency, nullptr),
            &events, this);
        if (!stream || pw.thread_loop_start(loop) < 0) {
            return false;
        }
        std::uint8_t storage[256];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof storage);
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.rate = sound_rate;
        format.channels = 1;
        const spa_pod* parameters[] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        // RT_PROCESS: process() takes no lock and allocates nothing.
        const auto flags =
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE);
        pw.thread_loop_lock(loop);
        const int connected =
            pw.stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, parameters, 1);
        pw.thread_loop_unlock(loop);
        return connected >= 0;
    }
    void lost() {
        {
            std::lock_guard lock(mutex);
            broken = true;
        }
        changed.notify_all();
    }
    bool pending() const {
        return press_requests.load(std::memory_order_relaxed) ||
               release_requests.load(std::memory_order_relaxed);
    }
    // Normal PipeWire loop callback, after the last queued sample played. A
    // request received during the drain waits here, rather than resuming an
    // unfinished drain and letting its late callback cut off a new sound.
    void drained() {
        const bool failed = pw.stream_set_active(stream, false) < 0;
        active = false;
        draining.store(false, std::memory_order_release);
        {
            std::lock_guard lock(mutex);
            broken |= failed;
            ++request_generation;
        }
        changed.notify_all();
    }
    // The loop stops first, so no event races what follows. Destroying the
    // stream takes it off the data thread, which runs process(), and ends that
    // thread with its context; its last state change calls lost() from here.
    void disconnect() {
        if (loop) {
            pw.thread_loop_stop(loop);
        }
        if (stream) {
            pw.stream_destroy(stream);
        }
        if (loop) {
            pw.thread_loop_destroy(loop);
        }
        stream = nullptr;
        loop = nullptr;
    }
    // On PipeWire's data thread. Requests landing within one cycle play once.
    void process() {
        if (draining.load(std::memory_order_relaxed)) {
            return;
        }
        pw_buffer* buffer = pw.stream_dequeue_buffer(stream);
        if (!buffer) {
            return;
        }
        spa_data& data = buffer->buffer->datas[0];
        if (auto* out = static_cast<float*>(data.data)) {
            std::size_t frames = data.maxsize / sizeof(float);
            if (buffer->requested) {
                frames = std::min<std::size_t>(frames, buffer->requested);
            }
            start(press_requests.exchange(0, std::memory_order_relaxed), 0);
            start(release_requests.exchange(0, std::memory_order_relaxed), 1);
            std::fill_n(out, frames, 0.f);
            for (auto& voice : voices) {
                const std::size_t count =
                    voice.sound ? std::min(frames, voice.sound->size() - voice.position) : 0;
                for (std::size_t i = 0; i < count; ++i) {
                    out[i] += (*voice.sound)[voice.position + i] * voice.gain;
                }
                voice.position += count;
                if (voice.sound && voice.position == voice.sound->size()) {
                    voice.sound = nullptr;
                }
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float);
            data.chunk->size = static_cast<std::uint32_t>(frames * sizeof(float));
        }
        pw.stream_queue_buffer(stream, buffer);
        if (!pending() &&
            std::none_of(voices.begin(), voices.end(), [](const Voice& voice) { return voice.sound; })) {
            // flush(..., true) is RT safe. Deactivation waits for its normal
            // drained callback so the final buffer's tail is not discarded.
            draining.store(true, std::memory_order_release);
            if (pw.stream_flush(stream, true) < 0) {
                // Stop supplying data. The active worker's bounded check
                // tears this connection down without locking on the RT thread.
                processing_failed.store(true, std::memory_order_release);
            }
        }
    }
    void start(unsigned request, std::size_t which) {
        auto voice = std::find_if(voices.begin(), voices.end(),
                                  [](const Voice& candidate) { return !candidate.sound; });
        if (request && voice != voices.end()) {
            *voice = {&sounds[(request >> 8 & 0xff) % pops.size()][which], 0,
                      sound_gain(static_cast<int>(request & 0xff))};
        }
    }

    struct Voice {
        const std::vector<float>* sound{};
        std::size_t position{};
        float gain{};
    };
    PipeWire pw;
    pw_thread_loop* loop{};
    pw_stream* stream{};
    std::array<std::array<std::vector<float>, 2>, pops.size()> sounds; // Press, then release.
    std::array<Voice, 4> voices{};                                     // Data thread only.
    std::atomic<unsigned> press_requests{}, release_requests{};
    std::atomic<bool> draining{};
    std::atomic<bool> processing_failed{};
    bool active{}; // Worker under the PipeWire loop lock, or its normal callbacks.
    std::mutex mutex;
    std::condition_variable changed;
    bool wanted{}, stopping{}, broken{};
    std::uint64_t request_generation{}; // Mutex-protected worker wakeups, including drain completion.
    std::thread worker;                 // Last, so everything above exists when it starts.
};
#else
struct KeySounds::Player {
    void set_awake(bool) {}
    void request(KeyFeedback, const std::string&, int) {}
};
#endif

KeySounds::KeySounds() = default;
KeySounds::~KeySounds() = default;
void KeySounds::set_awake(bool awake) {
    if (awake == awake_) {
        return;
    }
    awake_ = awake;
    if (!player_) {
        player_ = std::make_unique<Player>();
    }
    player_->set_awake(awake);
}
void KeySounds::play(KeyFeedback kind, const std::string& effect, int volume) {
    if (awake_) {
        player_->request(kind, effect, volume);
    }
}
} // namespace framekeyboard
