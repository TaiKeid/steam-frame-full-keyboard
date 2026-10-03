#include "whisper.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
std::atomic<bool> g_interrupted{false};

void signal_handler(int) {
    g_interrupted = true;
}

pid_t spawn_recorder(int pipe_out) {
    // Try pw-record first, then arecord.
    const char* pw_record_bin = "/usr/bin/pw-record";
    const char* arecord_bin = "/usr/bin/arecord";

    const char* bin = (access(pw_record_bin, X_OK) == 0) ? pw_record_bin : arecord_bin;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_out, STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> argv;
    if (std::strcmp(bin, pw_record_bin) == 0) {
        argv = {const_cast<char*>("pw-record"), const_cast<char*>("--rate"),
                const_cast<char*>("16000"),     const_cast<char*>("--channels"),
                const_cast<char*>("1"),         const_cast<char*>("--format"),
                const_cast<char*>("s16"),       const_cast<char*>("--raw"),
                const_cast<char*>("-"),         nullptr};
    } else {
        argv = {const_cast<char*>("arecord"),
                const_cast<char*>("-D"),
                const_cast<char*>("default"),
                const_cast<char*>("-r"),
                const_cast<char*>("16000"),
                const_cast<char*>("-c"),
                const_cast<char*>("1"),
                const_cast<char*>("-f"),
                const_cast<char*>("S16_LE"),
                const_cast<char*>("-t"),
                const_cast<char*>("raw"),
                const_cast<char*>("-"),
                nullptr};
    }

    pid_t pid = -1;
    int status = posix_spawn(&pid, bin, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (status != 0) {
        return -1;
    }
    return pid;
}

} // namespace

int main(int argc, char** argv) {
    std::string model_path;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        }
    }

    if (model_path.empty()) {
        std::cout << "error Missing --model argument\n" << std::flush;
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;

    whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        std::cout << "error Failed to load model: " << model_path << "\n" << std::flush;
        return 1;
    }

    int pipe_audio[2];
    if (pipe(pipe_audio) != 0) {
        whisper_free(ctx);
        std::cout << "error Failed to create audio pipe\n" << std::flush;
        return 1;
    }

    pid_t rec_pid = spawn_recorder(pipe_audio[1]);
    close(pipe_audio[1]); // Close write end in parent

    if (rec_pid <= 0) {
        close(pipe_audio[0]);
        whisper_free(ctx);
        std::cout << "error Failed to start audio recorder\n" << std::flush;
        return 1;
    }

    std::vector<float> pcmf32;
    std::atomic<bool> heard{false};
    std::atomic<bool> stop_reader{false};

    std::thread reader_thread([&]() {
        uint8_t raw_buffer[2048];
        size_t carry_len = 0;
        uint8_t carry_byte = 0;
        bool header_checked = false;
        int speech_chunks = 0;
        while (!stop_reader && !g_interrupted) {
            ssize_t bytes_read =
                read(pipe_audio[0], raw_buffer + carry_len, sizeof(raw_buffer) - carry_len);
            if (bytes_read <= 0) {
                break;
            }
            size_t total_bytes = carry_len + static_cast<size_t>(bytes_read);
            size_t offset = 0;

            if (!header_checked) {
                header_checked = true;
                // Discard Sun AU (.snd / dns.) or WAV (RIFF) container header if present
                if (total_bytes >= 4 && (std::memcmp(raw_buffer, ".snd", 4) == 0 ||
                                         std::memcmp(raw_buffer, "dns.", 4) == 0)) {
                    offset = total_bytes >= 32 ? 32 : 24;
                } else if (total_bytes >= 44 && std::memcmp(raw_buffer, "RIFF", 4) == 0) {
                    offset = 44;
                }
            }

            size_t available_bytes = total_bytes > offset ? total_bytes - offset : 0;
            size_t num_samples = available_bytes / sizeof(int16_t);
            carry_len = available_bytes % sizeof(int16_t);
            if (carry_len > 0) {
                carry_byte = raw_buffer[offset + num_samples * sizeof(int16_t)];
            }

            const int16_t* samples = reinterpret_cast<const int16_t*>(raw_buffer + offset);
            double sum_sq = 0;
            for (size_t i = 0; i < num_samples; ++i) {
                int16_t s = samples[i];
                sum_sq += static_cast<double>(s) * s;
                pcmf32.push_back(s / 32768.0f);
            }
            if (num_samples > 0) {
                double rms = std::sqrt(sum_sq / num_samples);
                if (rms > 80.0) { // Speech energy threshold
                    speech_chunks++;
                    if (speech_chunks >= 2 && !heard) {
                        heard = true;
                        std::cout << "hearing\n" << std::flush;
                    }
                } else if (speech_chunks > 0) {
                    speech_chunks--;
                }
            }
            if (carry_len > 0) {
                raw_buffer[0] = carry_byte;
            }
        }
    });

    std::cout << "listening\n" << std::flush;

    // Wait on stdin for "stop" or EOF
    std::string line;
    while (!g_interrupted && std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    // Stop recording
    kill(rec_pid, SIGTERM);
    if (reader_thread.joinable()) {
        reader_thread.join();
    }
    close(pipe_audio[0]);

    // Reap child
    int status = 0;
    waitpid(rec_pid, &status, 0);

    if (g_interrupted) {
        whisper_free(ctx);
        return 0;
    }

    std::cout << "transcribing\n" << std::flush;

    if (pcmf32.size() >= 16000 * 0.2) { // At least 200 ms of audio recorded
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.n_threads = std::min(4, static_cast<int>(std::thread::hardware_concurrency()));
        wparams.language = "en";
        wparams.translate = false;
        wparams.no_context = true;
        wparams.single_segment = false;
        wparams.suppress_blank = true;
        wparams.suppress_nst = true;
        wparams.temperature_inc = 0.0f;
        wparams.no_speech_thold = 0.6f;
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = false;

        int ret = whisper_full(ctx, wparams, pcmf32.data(), static_cast<int>(pcmf32.size()));
        if (ret == 0) {
            int n_segments = whisper_full_n_segments(ctx);
            std::string text;
            for (int i = 0; i < n_segments; ++i) {
                const char* seg = whisper_full_get_segment_text(ctx, i);
                if (seg) {
                    text += seg;
                }
            }
            std::cout << "text " << text << "\n" << std::flush;
        } else {
            std::cout << "error Speech recognition failed\n" << std::flush;
        }
    } else {
        std::cout << "text \n" << std::flush;
    }

    whisper_free(ctx);
    return 0;
}
