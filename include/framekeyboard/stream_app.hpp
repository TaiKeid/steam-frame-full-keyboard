#pragma once
#include "app_identity.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Which app a stream from a PC shows (docs/shortcut-bar-design.md, Apps).
// Gamescope reports nothing for a stream, but Steam's streaming client runs
// on the headset with the streamed game's Steam app ID among its arguments.
// That line also carries the account and the PC's address: only the number
// after --appid is read, nothing else of it is kept, and nothing of it is
// logged. Read on a thread of its own, never the VR thread.
namespace framekeyboard {
// The one identity for a stream with no usable app ID, such as the PC's
// desktop or a shortcut that isn't a Steam game.
inline constexpr const char* pc_stream_id = "pc";
// Steam starts a desktop stream with the ID of its own Remote Play Client app: the PC's stream, never
// a Steam game (R362).
inline constexpr std::uint32_t remote_play_client_app_id = 202355;
AppIdentity pc_stream_identity();

// The argument lists of the streaming clients running as this user, or none
// when they couldn't be read. Runs on the finder's thread. Tests give their
// own; App without one never reads /proc and every stream is the PC's.
using StreamClientSource = std::function<std::optional<std::vector<std::vector<std::string>>>()>;
// Reads /proc for processes named streaming_client owned by this user.
StreamClientSource system_stream_clients();

// The streamed game's app ID: the argument after exactly "--appid", all
// decimal digits, without a leading zero, nonzero and within 32 bits. None
// for anything else, a second --appid included. Pure, for tests.
std::optional<std::uint32_t> streamed_app_id(const std::vector<std::string>& arguments);
// The app a stream shows: the game's Steam identity when exactly one client
// runs and it names a usable ID; the PC's stream identity for the Remote Play
// Client's ID and for any surprise (none read, none running, two, or no usable
// ID), never a guess.
AppIdentity stream_identity(const std::optional<std::vector<std::vector<std::string>>>& clients);

// Answers which app a stream session shows, off the VR thread: a request
// names the session (any number the caller tells sessions apart by), and its
// answer carries that number back, so a late answer for an ended session can
// be told apart and dropped. Destroying this never waits for a read.
class StreamAppFinder {
  public:
    struct Result {
        std::uint64_t session{};
        AppIdentity app;
    };
    explicit StreamAppFinder(StreamClientSource source);
    ~StreamAppFinder();
    StreamAppFinder(const StreamAppFinder&) = delete;
    StreamAppFinder& operator=(const StreamAppFinder&) = delete;
    void request(std::uint64_t session);
    // A finished answer, oldest first; none while there is none.
    std::optional<Result> take();
    // Shared with the reading threads; defined in stream_app.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
} // namespace framekeyboard
