#pragma once

#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// Local Pinyin/Hangul composition. The app owns delivery and only clears this
// state after a successful text commit. No engine learning API is used.
// 천지인 (korean-cheonjiin) composes in this library and loads no engine.
class CjkComposer {
  public:
    explicit CjkComposer(const std::string& method);
    ~CjkComposer();
    void type(char key);
    void backspace();
    void cancel();
    void cycle(int direction);
    void choose(int index);
    std::string preedit() const;
    std::string text() const;
    std::vector<std::string> candidates() const;
    int selected() const;
    bool empty() const;
    bool has_reading() const;
    bool chinese() const;
    // 12-key input (천지인): what a key typed, replacing the last unit for a
    // repeated toggle tap, and that unit, which the toggle must still find.
    bool flick_input() const;
    void flick(const std::string& text, bool replace);
    std::string last_flick() const;
    static bool supported(const std::string& method);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace framekeyboard
