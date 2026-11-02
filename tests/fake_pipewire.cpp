// A private PipeWire substitute. No server, audio device or system session is
// contacted. Control callbacks hold the loop lock; RT process callbacks do not.
#include "fake_pipewire.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
// These helpers are inline in headers and exported by libpipewire as well.
// Rename the inline copies so this substitute can export its own versions.
#define pw_loop_add_event framekeyboard_unused_loop_add_event
#define pw_loop_signal_event framekeyboard_unused_loop_signal_event
#define pw_loop_destroy_source framekeyboard_unused_loop_destroy_source
#include <pipewire/pipewire.h>
#undef pw_loop_add_event
#undef pw_loop_signal_event
#undef pw_loop_destroy_source
#include <thread>
#include <utility>
#include <vector>

struct pw_stream;
struct FakeEvent;
struct pw_thread_loop {
    std::recursive_mutex mutex;
    std::mutex data_mutex;
    std::thread control, realtime;
    std::atomic<bool> running{};
    pw_stream* stream{}; // Set before starting, cleared after both threads stop.
    FakeEvent* event{};
};
struct FakeEvent {
    spa_source source{};
    spa_source_event_func_t callback{};
    void* data{};
    std::atomic<bool> pending{};
};
struct pw_stream {
    pw_thread_loop* loop{};
    const pw_stream_events* events{};
    void* listener{};
    std::atomic<bool> active{}, draining{};
    float samples[512]{};
    spa_chunk chunk{};
    spa_data data{};
    spa_buffer spa{};
    pw_buffer buffer{};
};

namespace {
std::mutex observations;
SoundPlayerSnapshot snapshot;
std::vector<float> samples;
std::atomic<bool> hold_drain{};
std::atomic<int> fail_active{-1};
std::atomic<bool> fail_flush{};
std::atomic<bool> pause_processing{};
std::atomic<bool> interrupt_signal{};
std::uint64_t pending_frames{};
} // namespace

extern "C" {
void framekeyboard_test_sound_snapshot(SoundPlayerSnapshot* result) {
    std::lock_guard lock(observations);
    *result = snapshot;
}
void framekeyboard_test_sound_hold_drain(bool hold) {
    hold_drain = hold;
}
void framekeyboard_test_sound_fail_active(bool activating) {
    fail_active = activating;
}
void framekeyboard_test_sound_fail_flush() {
    fail_flush = true;
}
void framekeyboard_test_sound_pause_processing(bool paused) {
    pause_processing = paused;
}
void framekeyboard_test_sound_interrupt_signal() {
    interrupt_signal = true;
}
std::size_t framekeyboard_test_sound_samples(float* destination, std::size_t capacity) {
    std::lock_guard lock(observations);
    const auto count = std::min(capacity, samples.size());
    std::copy_n(samples.begin(), count, destination);
    return samples.size();
}
void pw_init(int*, char***) {
}
pw_properties* pw_properties_new(const char*, ...) {
    return nullptr;
}
pw_thread_loop* pw_thread_loop_new(const char*, const spa_dict*) {
    return new pw_thread_loop;
}
pw_loop* pw_thread_loop_get_loop(pw_thread_loop* loop) {
    return reinterpret_cast<pw_loop*>(loop);
}
int pw_thread_loop_start(pw_thread_loop* loop) {
    loop->running = true;
    loop->control = std::thread([loop] {
        while (loop->running) {
            {
                std::lock_guard lock(loop->mutex);
                if (loop->event && loop->event->pending.exchange(false)) {
                    loop->event->callback(loop->event->data, 1);
                }
                auto* stream = loop->stream;
                if (stream->draining.load() && !hold_drain.load()) {
                    // Every queued sample is played before drained is emitted.
                    stream->draining = false;
                    {
                        std::lock_guard record(observations);
                        snapshot.played_frames += pending_frames;
                        pending_frames = 0;
                        ++snapshot.drains_completed;
                    }
                    stream->events->drained(stream->listener);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    loop->realtime = std::thread([loop] {
        while (loop->running) {
            {
                std::lock_guard data(loop->data_mutex);
                auto* stream = loop->stream;
                if (stream->active.load() && !stream->draining.load() && !pause_processing.load()) {
                    {
                        std::lock_guard record(observations);
                        ++snapshot.callbacks;
                    }
                    stream->events->process(stream->listener);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    return 0;
}
void pw_thread_loop_stop(pw_thread_loop* loop) {
    loop->running = false;
    if (loop->control.joinable()) {
        loop->control.join();
    }
    if (loop->realtime.joinable()) {
        loop->realtime.join();
    }
}
void pw_thread_loop_lock(pw_thread_loop* loop) {
    loop->mutex.lock();
    std::lock_guard record(observations);
    ++snapshot.loop_locks;
}
void pw_thread_loop_unlock(pw_thread_loop* loop) {
    loop->mutex.unlock();
}
void pw_thread_loop_destroy(pw_thread_loop* loop) {
    delete loop;
}
spa_source* pw_loop_add_event(pw_loop* loop, spa_source_event_func_t callback, void* data) {
    auto* thread_loop = reinterpret_cast<pw_thread_loop*>(loop);
    auto* event = new FakeEvent;
    event->callback = callback;
    event->data = data;
    thread_loop->event = event;
    std::lock_guard record(observations);
    ++snapshot.events_created;
    return &event->source;
}
int pw_loop_signal_event(pw_loop* loop, spa_source* source) {
    auto* thread_loop = reinterpret_cast<pw_thread_loop*>(loop);
    if (!thread_loop->event || &thread_loop->event->source != source) {
        return -EINVAL;
    }
    if (interrupt_signal.exchange(false)) {
        std::lock_guard record(observations);
        ++snapshot.signal_interruptions;
        return -EINTR;
    }
    thread_loop->event->pending = true;
    std::lock_guard record(observations);
    ++snapshot.events_signaled;
    return 0;
}
void pw_loop_destroy_source(pw_loop* loop, spa_source* source) {
    auto* thread_loop = reinterpret_cast<pw_thread_loop*>(loop);
    if (thread_loop->event && &thread_loop->event->source == source) {
        delete std::exchange(thread_loop->event, nullptr);
        std::lock_guard record(observations);
        ++snapshot.events_destroyed;
    }
}
pw_stream* pw_stream_new_simple(pw_loop* loop, const char*, pw_properties*,
                                const pw_stream_events* events, void* listener) {
    auto* stream = new pw_stream;
    stream->loop = reinterpret_cast<pw_thread_loop*>(loop);
    stream->events = events;
    stream->listener = listener;
    stream->loop->stream = stream;
    stream->data.data = stream->samples;
    stream->data.maxsize = sizeof(stream->samples);
    stream->data.chunk = &stream->chunk;
    stream->spa.n_datas = 1;
    stream->spa.datas = &stream->data;
    stream->buffer.buffer = &stream->spa;
    stream->buffer.requested = std::size(stream->samples);
    return stream;
}
int pw_stream_connect(pw_stream* stream, pw_direction, std::uint32_t, pw_stream_flags flags,
                      const spa_pod**, std::uint32_t) {
    stream->active = !(flags & PW_STREAM_FLAG_INACTIVE);
    std::lock_guard record(observations);
    ++snapshot.connections;
    snapshot.inactive_connections += (flags & PW_STREAM_FLAG_INACTIVE) != 0;
    snapshot.realtime_connections += (flags & PW_STREAM_FLAG_RT_PROCESS) != 0;
    return 0;
}
int pw_stream_set_active(pw_stream* stream, bool active) {
    std::lock_guard data(stream->loop->data_mutex);
    std::lock_guard record(observations);
    int failing = active;
    if (fail_active.compare_exchange_strong(failing, -1)) {
        ++snapshot.control_failures;
        return -EIO;
    }
    if (active) {
        ++snapshot.activations;
        snapshot.resumed_during_drain += stream->draining.load();
    } else {
        ++snapshot.deactivations;
    }
    stream->active = active;
    return 0;
}
int pw_stream_flush(pw_stream* stream, bool drain) {
    if (drain) {
        {
            std::lock_guard record(observations);
            if (fail_flush.exchange(false)) {
                ++snapshot.flush_failures;
                return -EIO;
            }
            ++snapshot.drains_requested;
        }
        stream->draining = true;
    }
    return 0;
}
pw_buffer* pw_stream_dequeue_buffer(pw_stream* stream) {
    return &stream->buffer;
}
int pw_stream_queue_buffer(pw_stream* stream, pw_buffer*) {
    const auto count = stream->chunk.size / sizeof(float);
    std::lock_guard record(observations);
    snapshot.queued_frames += count;
    pending_frames += count;
    samples.insert(samples.end(), stream->samples, stream->samples + count);
    return 0;
}
void pw_stream_destroy(pw_stream* stream) {
    {
        std::lock_guard record(observations);
        ++snapshot.disconnections;
        pending_frames = 0; // Hide/shutdown discard the old connection's tail.
    }
    stream->loop->stream = nullptr;
    delete stream;
}
}
