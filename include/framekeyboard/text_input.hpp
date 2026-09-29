#pragma once
#include <memory>
#include <string>
namespace framekeyboard {
// Optional Gamescope UTF-8 transport. No clipboard or application text is read.
class GamescopeText {
  public:
    explicit GamescopeText(const std::string& socket);
    ~GamescopeText();
    bool ready();
    bool commit(const std::string& text);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace framekeyboard
