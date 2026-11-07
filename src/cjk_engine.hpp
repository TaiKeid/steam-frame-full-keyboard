#pragma once
#include <string>
#include <vector>

namespace framekeyboard {
// Private interface between modules built and packaged with this executable.
// Destroy engines before unloading the module that owns their virtual table.
class CjkEngine {
  public:
    virtual ~CjkEngine() = default;
    virtual void type(char key) = 0;
    virtual void backspace() = 0;
    virtual void cancel() = 0;
    virtual void cycle(int direction) = 0;
    virtual void choose(int index) = 0;
    virtual std::string preedit() const = 0;
    virtual std::string text() const = 0;
    virtual std::vector<std::string> candidates() const = 0;
    virtual int selected() const = 0;
    virtual bool empty() const = 0;
    virtual bool has_reading() const = 0;
    virtual bool chinese() const = 0;
    // Text two-set Hangul has finished, a syllable at a time, for the app to
    // send at once; it stays part of text() until dropped after delivery.
    virtual std::string finished() const { return {}; }
    virtual void drop_finished() {}
    // Drops what is still open, keeping finished text for its commit.
    virtual void cancel_open() { cancel(); }
};
using CreateCjkEngine = CjkEngine* (*)(const char*);
} // namespace framekeyboard
