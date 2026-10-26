// framekeyboard-whisper: on-device speech recognition for the dictation
// helper (docs/dictation.md, On-device recognition). The helper starts it with
// a model path and talks to it through stdin and stdout only; it opens no
// files besides the model and never listens on the network.
//
// Start: framekeyboard-whisper --model PATH --threads N
// After loading, one line: {"ready":true}, or {"error":"model"} and exit.
// Request: one JSON line {"samples":N,"language":"en","prompt":"..."}, then
// exactly N 16 kHz mono 16-bit little-endian samples.
// Reply: one JSON line {"text":"..."} or {"error":"code"}. EOF ends it.
//
// Audio and text stay in memory; nothing is logged and core dumps are off.
#include <whisper.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <json-c/json.h>
#include <memory>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <vector>

namespace {
// Phrases end by 25 s; anything longer is not a phrase.
constexpr std::size_t max_samples = 30 * 16000;
constexpr std::size_t max_header = 4096;

void reply(json_object* object) {
    std::fputs(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    json_object_put(object);
}

void reply_error(const char* code) {
    auto* object = json_object_new_object();
    json_object_object_add(object, "error", json_object_new_string(code));
    reply(object);
}

// One header line; false at EOF or when it is too long to be one.
bool read_line(std::string& line) {
    line.clear();
    for (int c; (c = std::fgetc(stdin)) != EOF;) {
        if (c == '\n') {
            return true;
        }
        if (line.size() >= max_header) {
            return false;
        }
        line.push_back(static_cast<char>(c));
    }
    return false;
}

std::string text_field(json_object* object, const char* name) {
    json_object* value{};
    if (!json_object_object_get_ex(object, name, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return {};
    }
    return json_object_get_string(value);
}
} // namespace

int main(int argc, char** argv) {
    // Audio and text pass through this process's memory.
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    // Recognition yields to VR and games (docs/dictation.md, On-device recognition).
    setpriority(PRIO_PROCESS, 0, 10);
    std::string model;
    int threads = 4;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--model") == 0) {
            model = argv[i + 1];
        } else if (std::strcmp(argv[i], "--threads") == 0) {
            threads = std::max(1, std::min(8, std::atoi(argv[i + 1])));
        }
    }
    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
    auto params = whisper_context_default_params();
    params.use_gpu = false; // the Frame's GPU made VR unresponsive (docs/dictation.md, M0)
    std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
        model.empty() ? nullptr : whisper_init_from_file_with_params(model.c_str(), params),
        whisper_free);
    if (!context) {
        reply_error("model");
        return 1;
    }
    {
        auto* ready = json_object_new_object();
        json_object_object_add(ready, "ready", json_object_new_boolean(true));
        reply(ready);
    }
    std::string line;
    std::vector<std::int16_t> pcm;
    std::vector<float> audio;
    while (read_line(line)) {
        std::unique_ptr<json_object, decltype(&json_object_put)> header(json_tokener_parse(line.c_str()),
                                                                        json_object_put);
        json_object* samples{};
        if (!header || !json_object_object_get_ex(header.get(), "samples", &samples) ||
            !json_object_is_type(samples, json_type_int) || json_object_get_int64(samples) < 0 ||
            static_cast<std::size_t>(json_object_get_int64(samples)) > max_samples) {
            reply_error("invalid");
            return 1; // the stream can no longer be followed
        }
        pcm.resize(static_cast<std::size_t>(json_object_get_int64(samples)));
        if (std::fread(pcm.data(), sizeof(std::int16_t), pcm.size(), stdin) != pcm.size()) {
            return 0;
        }
        audio.resize(pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            audio[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }
        const auto language = text_field(header.get(), "language");
        const auto prompt = text_field(header.get(), "prompt");
        // As the speech server is run: greedy with temperature fallback,
        // Whisper's 30 s window, no timestamps, non-speech tokens suppressed.
        auto full = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        full.n_threads = threads;
        full.language = language.empty() ? "auto" : language.c_str();
        full.translate = false;
        full.no_timestamps = true;
        full.suppress_nst = true;
        full.print_progress = full.print_realtime = full.print_timestamps = full.print_special = false;
        if (!prompt.empty()) {
            full.initial_prompt = prompt.c_str();
        }
        if (whisper_full(context.get(), full, audio.data(), static_cast<int>(audio.size())) != 0) {
            reply_error("failed");
            continue;
        }
        std::string text;
        for (int s = 0; s < whisper_full_n_segments(context.get()); ++s) {
            text += whisper_full_get_segment_text(context.get(), s);
        }
        auto* answer = json_object_new_object();
        json_object_object_add(answer, "text",
                               json_object_new_string_len(text.data(), static_cast<int>(text.size())));
        reply(answer);
    }
    return 0;
}
