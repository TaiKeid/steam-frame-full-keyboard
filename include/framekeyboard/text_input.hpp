#pragma once
#include <functional>
#include <memory>
#include <string>
namespace framekeyboard {
// Optional Gamescope UTF-8 transport. No clipboard or application text is read.
// Discovery and commits progress only through nonblocking dispatch in ready().
class GamescopeText {
  public:
    using Clock = std::function<double()>;
    explicit GamescopeText(const std::string& socket, Clock clock = {});
    ~GamescopeText();
    bool ready();
    bool failed() const;
    // Deletions run as Gamescope delete_left actions before the text, in the
    // same request stream, so they cannot interleave with its temporary keymap.
    // With per_character, each character is its own commit, sent after the
    // previous one is acknowledged and at least the character gap later: Gamescope then types
    // characters its ordinary keymap has without a temporary keymap, and they
    // arrive spaced apart.
    bool commit(const std::string& text, std::size_t delete_before = 0, bool per_character = false);
    // The minimum gap between per-character commits. Dictation's Send sets a
    // longer one while it types: under heavy load, 20 ms still let IBus swap
    // neighbouring characters now and then (docs/dictation.md, M3b results).
    static constexpr double default_character_gap = .02;
    void set_character_gap(double seconds);
    bool pending() const;
    // Characters of the most recent commit that Gamescope acknowledged. Only a
    // per-character commit can end with part of its text acknowledged.
    std::size_t acknowledged() const;
    // Cancellation/timeout cannot undo an already submitted Wayland request.
    // Keep its ordering barrier until the sync arrives, even if the caller stops waiting.
    bool busy() const;
    void cancel();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace framekeyboard
