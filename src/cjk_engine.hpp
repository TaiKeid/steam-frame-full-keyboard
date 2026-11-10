#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace framekeyboard {
// A message for the user from an engine module, by its key in the interface
// language files: a module has no language tables, so CjkComposer passes it
// on as a UiError.
class CjkMessage : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
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
    // text() reached the field; the app clears the reading next. An engine
    // that learns does so only when `learn` is set (never in a password, PIN
    // or terminal field) and it was given a folder to keep it in.
    virtual void committed(bool learn) { (void)learn; }
    // Zhuyin engines whose open syllable takes Space as its first tone.
    virtual bool takes_space() const { return false; }
};
// The method's ID, and the folder for what the engine learns: empty learns
// nothing and writes nothing.
using CreateCjkEngine = CjkEngine* (*)(const char* method, const char* learning_dir);
} // namespace framekeyboard
