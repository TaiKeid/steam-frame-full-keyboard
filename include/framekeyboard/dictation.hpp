#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace framekeyboard {
namespace fs = std::filesystem;

// Offline dictation runs in a separate `framekeyboard-dictate` process. It owns
// the microphone and the whisper.cpp model, so a crash, a slow model load or a
// stuck audio device never blocks the VR loop or strands keyboard state.
//
// This class only manages that child and parses its line protocol. It never
// delivers input: App decides whether recognised text may be committed through
// its InputGate. Transcripts are never logged or written to disk.
class Dictation {
  public:
    enum class State { Idle, Listening, Transcribing };
    struct Event {
        enum class Kind { Listening, Hearing, Transcribing, Text, Error } kind;
        // Text: recognised UTF-8 (may be empty). Error: a short user-facing reason.
        std::string payload;
    };

    Dictation(fs::path helper, fs::path model);
    ~Dictation();
    Dictation(const Dictation&) = delete;
    Dictation& operator=(const Dictation&) = delete;

    // Helper executable and model file are present. Checked on each start too.
    bool available() const;
    const fs::path& model() const { return model_; }
    // Spawns the helper and starts recording immediately. Throws on failure.
    void start();
    // Ends recording; the helper transcribes and reports a Text event, then exits.
    void finish();
    // Stops the microphone and discards any pending result. Idempotent.
    void cancel();
    // Non-blocking. Reads complete protocol lines and reaps the child on exit.
    std::vector<Event> poll(double now);
    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    // True after speech energy has been detected in the current recording.
    bool hearing() const { return hearing_; }

  private:
    void close_child();
    fs::path helper_, model_;
    pid_t pid_{-1};
    // One AF_UNIX stream socket is the child's stdin and stdout. send() with
    // MSG_NOSIGNAL means a dead helper cannot raise SIGPIPE in the keyboard.
    int fd_{-1};
    std::string buffer_;
    State state_{State::Idle};
    bool hearing_{};
    bool result_seen_{};
    double transcribe_started_{};
};

// Default model lookup: an explicit path, then a user override in the config
// directory, then the packaged model, then a development build's copy.
fs::path find_speech_model(const fs::path& explicit_model, const fs::path& config_dir,
                           const fs::path& data_dir);
// The helper is installed beside the main executable.
fs::path find_speech_helper();

// Normalises recogniser output for typing: removes bracketed non-speech
// annotations such as "[BLANK_AUDIO]" or "(music)", collapses whitespace and
// trims. Returns an empty string when nothing speech-like remains.
std::string clean_transcript(const std::string& text);
// Keeps one copy of any run of four or more words repeated back to back.
std::string collapse_repeats(const std::string& text);
// Splits valid UTF-8 into chunks of at most `limit` code points without
// breaking a multi-byte sequence. Gamescope commits are limited to 32.
std::vector<std::string> split_utf8(const std::string& text, std::size_t limit);

} // namespace framekeyboard
