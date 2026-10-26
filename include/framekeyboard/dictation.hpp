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
        // Paused answers capture(false): every phrase spoken before it has
        // been announced.
        // Partial: the words so far of the phrase being spoken, with live words on.
        // Download: a model download's state in `text` (progress, quantizing,
        // ready or failed), its bytes and, on failure, the reason.
        // Microphone: capture bypasses SteamOS's voice filters, which a
        // running game turned on, so game sound may be picked up.
        enum class Kind {
            Level,
            Phrase,
            Ready,
            Partial,
            Text,
            Dropped,
            Stopped,
            Paused,
            Error,
            Test,
            Download,
            Microphone
        };
        Kind kind{};
        std::uint64_t id{};
        int level{};
        bool ok{};
        std::string text;              // Recognized text; never logged.
        bool device{};                 // Text: recognized on the Frame by the fallback.
        std::uint64_t done{}, total{}; // Download progress in bytes.
        std::string reason;            // Always one of the protocol's fixed codes.
    };
    struct Target {
        std::string server; // host[:port], the user's own server
        std::string url;    // or a full base URL from config.json
        std::string model, language;
        bool live{}; // also recognize each phrase as it grows (Type as I speak)
        // The on-device model file: recognition on the Frame, or with a
        // server and `fallback`, for phrases the server could not take.
        std::string device{};
        bool fallback{};
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
    // Tests `target`, such as an address not yet applied, without changing
    // the one phrases still waiting are recognized with.
    bool test(const Target& target);
    // Fetches an on-device model (`small-q8_0`, `small-q5_1` or `small-q4_0`)
    // into `directory`; progress arrives as Download events.
    bool download(const std::string& model, const std::filesystem::path& directory);
    bool cancel_download();
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
