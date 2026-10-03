#include "framekeyboard/dictation.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace framekeyboard {
namespace {
constexpr const char* default_model_name = "ggml-small.en.bin";
// Bounds protect the VR loop from a misbehaving helper; real output is tiny.
constexpr std::size_t max_line_buffer = 256 * 1024;
constexpr double max_transcribe_seconds = 120;

bool executable(const fs::path& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0 && fs::is_regular_file(path);
}
fs::path executable_dir() {
    std::error_code error;
    const auto self = fs::canonical("/proc/self/exe", error);
    return error ? fs::path{} : self.parent_path();
}
} // namespace

Dictation::Dictation(fs::path helper, fs::path model)
    : helper_(std::move(helper)), model_(std::move(model)) {
}
Dictation::~Dictation() {
    cancel();
}
bool Dictation::available() const {
    std::error_code error;
    return executable(helper_) && fs::is_regular_file(model_, error);
}
void Dictation::start() {
    if (active()) {
        return;
    }
    close_child();
    if (!executable(helper_)) {
        throw std::runtime_error("Speech helper is missing. Reinstall Full Keyboard.");
    }
    std::error_code exists_error;
    if (!fs::is_regular_file(model_, exists_error)) {
        throw std::runtime_error("Speech model not found: " + model_.filename().string());
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        throw std::runtime_error("Cannot create speech helper channel");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // dup2 clears close-on-exec on the child's copy only. stderr is discarded so
    // library diagnostics can never end up in a log next to recognised text.
    posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const std::string helper = helper_.string(), model = model_.string();
    std::vector<char*> argv{const_cast<char*>(helper.c_str()), const_cast<char*>("--model"),
                            const_cast<char*>(model.c_str()), nullptr};
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, helper.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pair[1]);
    if (spawned != 0) {
        close(pair[0]);
        throw std::runtime_error("Cannot start speech helper: " + std::string(std::strerror(spawned)));
    }
    pid_ = pid;
    fd_ = pair[0];
    buffer_.clear();
    state_ = State::Listening;
    hearing_ = false;
    result_seen_ = false;
}
void Dictation::finish() {
    if (state_ != State::Listening || fd_ < 0) {
        return;
    }
    static constexpr char stop[] = "stop\n";
    // A failed send means the helper already exited; poll() reports that.
    (void)send(fd_, stop, sizeof(stop) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
}
void Dictation::cancel() {
    if (pid_ > 0) {
        // SIGKILL releases the microphone immediately. Reap synchronously so a
        // cancelled recording can never outlive the dashboard session.
        kill(pid_, SIGKILL);
    }
    close_child();
    state_ = State::Idle;
    hearing_ = false;
    buffer_.clear();
}
void Dictation::close_child() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        // The helper exits right after closing its output. Bound the wait, then
        // force it, so an orderly finish never blocks the render loop for long.
        for (int i = 0; i < 50; ++i) {
            const pid_t done = waitpid(pid_, nullptr, WNOHANG);
            if (done == pid_ || (done < 0 && errno != EINTR)) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }
}
std::vector<Dictation::Event> Dictation::poll(double now) {
    std::vector<Event> events;
    if (fd_ < 0) {
        return events;
    }
    bool ended = false;
    char chunk[4096];
    while (true) {
        const ssize_t count = recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (count > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(count));
            if (buffer_.size() > max_line_buffer) {
                cancel();
                events.push_back({Event::Kind::Error, "Speech helper produced too much output."});
                return events;
            }
            continue;
        }
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            ended = true;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    std::size_t start = 0;
    for (std::size_t end; (end = buffer_.find('\n', start)) != std::string::npos; start = end + 1) {
        const std::string line = buffer_.substr(start, end - start);
        const auto space = line.find(' ');
        const std::string verb = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string{} : line.substr(space + 1);
        if (verb == "listening") {
            events.push_back({Event::Kind::Listening, {}});
        } else if (verb == "hearing") {
            hearing_ = true;
            events.push_back({Event::Kind::Hearing, {}});
        } else if (verb == "transcribing") {
            state_ = State::Transcribing;
            transcribe_started_ = now;
            events.push_back({Event::Kind::Transcribing, {}});
        } else if (verb == "text") {
            result_seen_ = true;
            events.push_back({Event::Kind::Text, rest});
        } else if (verb == "error") {
            result_seen_ = true;
            events.push_back({Event::Kind::Error, rest.empty() ? "Speech recognition failed." : rest});
        }
    }
    buffer_.erase(0, start);
    if (ended) {
        const bool had_result = result_seen_;
        close_child();
        state_ = State::Idle;
        hearing_ = false;
        buffer_.clear();
        if (!had_result) {
            events.push_back({Event::Kind::Error, "Speech helper stopped unexpectedly."});
        }
    } else if (state_ == State::Transcribing && now - transcribe_started_ > max_transcribe_seconds) {
        cancel();
        events.push_back({Event::Kind::Error, "Speech recognition timed out."});
    }
    return events;
}

fs::path find_speech_model(const fs::path& explicit_model, const fs::path& config_dir,
                           const fs::path& data_dir) {
    if (!explicit_model.empty()) {
        return explicit_model;
    }
    std::vector<fs::path> candidates;
    if (!config_dir.empty()) {
        candidates.push_back(config_dir / "speech" / "model.bin");
    }
    if (!data_dir.empty()) {
        candidates.push_back(data_dir / "speech" / default_model_name);
    }
    if (const auto dir = executable_dir(); !dir.empty()) {
        candidates.push_back(dir.parent_path() / "share/framekeyboard/speech" / default_model_name);
        candidates.push_back(dir / "speech" / default_model_name);
    }
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    // Report the packaged location when nothing is installed.
    return data_dir.empty() ? fs::path(default_model_name) : data_dir / "speech" / default_model_name;
}
fs::path find_speech_helper() {
    const auto dir = executable_dir();
    return dir.empty() ? fs::path{} : dir / "framekeyboard-dictate";
}

std::string clean_transcript(const std::string& text) {
    if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr)) {
        return {};
    }
    std::string stripped;
    stripped.reserve(text.size());
    // Whisper marks non-speech as [BLANK_AUDIO], (music), *laughs* or ♪ notes.
    char closing = 0;
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        const gunichar ch = g_utf8_get_char(p);
        if (closing) {
            if (ch == static_cast<gunichar>(closing)) {
                closing = 0;
                stripped += ' ';
            }
            continue;
        }
        if (ch == '[' || ch == '(' || ch == '*') {
            closing = ch == '[' ? ']' : ch == '(' ? ')' : '*';
            continue;
        }
        if (ch == 0x266A || ch == 0x266B) {
            continue;
        }
        stripped.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
    }
    std::string result;
    bool space = false, meaningful = false;
    for (const char* p = stripped.c_str(); *p; p = g_utf8_next_char(p)) {
        const gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch)) {
            space = !result.empty();
            continue;
        }
        if (space) {
            result += ' ';
            space = false;
        }
        if (ch == 0x2018 || ch == 0x2019) {
            result += '\'';
            meaningful = true;
            continue;
        }
        if (ch == 0x201C || ch == 0x201D) {
            result += '"';
            meaningful = true;
            continue;
        }
        if (ch == 0x2013 || ch == 0x2014) {
            result += '-';
            meaningful = true;
            continue;
        }
        if (ch == 0x2026) {
            result += "...";
            meaningful = true;
            continue;
        }
        meaningful |= g_unichar_isalnum(ch) != 0;
        result.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
    }
    if (result.find("1234567890") != std::string::npos ||
        result.find("qwertyuiop") != std::string::npos ||
        result.find("asdfghjkl") != std::string::npos) {
        return {};
    }
    return meaningful ? result : std::string{};
}
std::vector<std::string> split_utf8(const std::string& text, std::size_t limit) {
    std::vector<std::string> chunks;
    if (limit == 0 || !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr)) {
        return chunks;
    }
    const char* begin = text.c_str();
    std::size_t count = 0;
    for (const char* p = begin; *p; p = g_utf8_next_char(p)) {
        if (count == limit) {
            chunks.emplace_back(begin, static_cast<std::size_t>(p - begin));
            begin = p;
            count = 0;
        }
        ++count;
    }
    if (count) {
        chunks.emplace_back(begin);
    }
    return chunks;
}
} // namespace framekeyboard
