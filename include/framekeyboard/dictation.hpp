#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// The keyboard's side of the dictation helper (docs/dictation.md, helper
// protocol). Capture, phrase detection and requests run in a private Python
// child. The VR thread only exchanges bounded, nonblocking packets and never
// waits for the child, except in wait_stopped() at shutdown.
class Dictation {
  public:
    struct Event {
        enum class Kind { Level, Phrase, Ready, Text, Dropped, Stopped, Error, Test };
        Kind kind{};
        std::uint64_t id{};
        int level{};
        bool ok{};
        std::string text;   // Recognized text; never logged.
        std::string reason; // Always one of the protocol's fixed codes.
    };
    struct Target {
        std::string server; // host[:port], the user's own server
        std::string url;    // or a full base URL from config.json
        std::string model, language;
    };
    // `helper` and `arguments` exist for tests; empty uses the installed helper.
    explicit Dictation(std::filesystem::path key_file, std::filesystem::path helper = {},
                       std::vector<std::string> arguments = {});
    // Waits briefly for the helper to exit; only App's shutdown destroys this.
    ~Dictation();
    Dictation(const Dictation&) = delete;
    Dictation& operator=(const Dictation&) = delete;

    bool running() const { return fd_ >= 0; }
    // Starts the helper when needed. False when it cannot be started or reached.
    bool configure(const Target& target);
    bool capture(bool on);
    // The context for phrase `id`; at most the last 200 code points are sent.
    bool prompt(std::uint64_t id, const std::string& text);
    // Stops capture and drops every pending phrase. Events the helper sent
    // before it saw the cancel are discarded, never delivered late.
    bool cancel();
    bool test();
    // Sends the keep-alive, reads a bounded number of packets and notices a
    // helper that exited. Never blocks.
    void tick(double now);
    std::optional<Event> next();
    // Closing the socket makes the helper stop the recorder and exit. Returns
    // at once; tick() or the destructor reaps the child.
    void stop();
    // For shutdown and update handoff only, never during a VR frame.
    bool wait_stopped(double seconds);

  private:
    bool start();
    bool send(const std::string& packet);
    void lost();
    void reap();
    std::filesystem::path key_file_, helper_;
    std::vector<std::string> arguments_;
    std::deque<Event> events_;
    double alive_at_{};
    int fd_{-1};
    int child_{-1};
    unsigned cancels_{}; // Sent and not yet confirmed by `cancelled`.
};
} // namespace framekeyboard
