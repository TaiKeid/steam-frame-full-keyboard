// A private PipeWire substitute. No server, audio device or user session is
// contacted. The real-time thread runs only between loop start/stop.
#include "fake_pipewire.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <pipewire/pipewire.h>
#include <thread>
#include <vector>

struct pw_stream;
struct pw_thread_loop {
    std::recursive_mutex mutex;
    std::thread realtime;
    std::atomic<bool> running{};
    pw_stream* stream{}; // Set before starting, cleared after its thread stops.
};
struct pw_stream {
    pw_thread_loop* loop{};
    const pw_stream_events* events{};
    void* listener{};
    std::atomic<bool> active{};
    float samples[512]{};
    spa_chunk chunk{};
    spa_data data{};
    spa_buffer spa{};
    pw_buffer buffer{};
};

namespace {
std::mutex observations;
std::mutex processing;
SoundPlayerSnapshot snapshot;
std::vector<float> samples;
std::atomic<bool> pause_processing{}, fail_connect{};
} // namespace

extern "C" {
void framekeyboard_test_sound_snapshot(SoundPlayerSnapshot* result) {
    std::lock_guard lock(observations);
    *result = snapshot;
}
void framekeyboard_test_sound_pause_processing(bool paused) {
    std::lock_guard lock(processing);
    pause_processing = paused;
}
void framekeyboard_test_sound_fail_connect() {
    fail_connect = true;
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
    loop->realtime = std::thread([loop] {
        while (loop->running) {
            {
                std::lock_guard data(processing);
                auto* stream = loop->stream;
                if (stream->active.load() && !pause_processing.load()) {
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
    if (loop->realtime.joinable()) {
        loop->realtime.join();
    }
}
void pw_thread_loop_lock(pw_thread_loop* loop) {
    loop->mutex.lock();
}
void pw_thread_loop_unlock(pw_thread_loop* loop) {
    loop->mutex.unlock();
}
void pw_thread_loop_destroy(pw_thread_loop* loop) {
    delete loop;
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
    std::lock_guard record(observations);
    ++snapshot.connections;
    if (fail_connect.exchange(false)) {
        ++snapshot.connect_failures;
        return -EIO;
    }
    stream->active = !(flags & PW_STREAM_FLAG_INACTIVE);
    snapshot.active_connections += !(flags & PW_STREAM_FLAG_INACTIVE);
    snapshot.realtime_connections += (flags & PW_STREAM_FLAG_RT_PROCESS) != 0;
    return 0;
}
pw_buffer* pw_stream_dequeue_buffer(pw_stream* stream) {
    return &stream->buffer;
}
int pw_stream_queue_buffer(pw_stream* stream, pw_buffer*) {
    const auto count = stream->chunk.size / sizeof(float);
    std::lock_guard record(observations);
    snapshot.queued_frames += count;
    snapshot.audible_frames += static_cast<std::uint64_t>(std::count_if(
        stream->samples, stream->samples + count, [](float sample) { return sample != 0; }));
    samples.insert(samples.end(), stream->samples, stream->samples + count);
    return 0;
}
void pw_stream_destroy(pw_stream* stream) {
    {
        std::lock_guard record(observations);
        ++snapshot.disconnections;
    }
    stream->loop->stream = nullptr;
    delete stream;
}
}
