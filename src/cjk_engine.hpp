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
    // Text finished ahead of the reading, for the app to send at once: two-set
    // Hangul's syllables, a full Quick code's character, a long Pinyin or
    // Zhuyin reading's front. It stays part of text() until dropped after delivery.
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
    // Enter: what is left of the reading goes as typed, after any choices, as
    // Pinyin's letters do on a PC; text() says so until the reading changes.
    // Engines without it keep the conversion.
    virtual void as_typed() {}
    // Zhuyin's cursor inside the reading (McBopomofo): move() takes it that
    // many syllables, stopping at the ends, and the candidates follow the
    // phrase there. Engines without one return false from has_cursor(), and
    // the arrows commit as other keys do.
    virtual bool has_cursor() const { return false; }
    virtual void move(int syllables) { (void)syllables; }
    // Removes the syllable after the cursor, as Delete.
    virtual void delete_forward() {}
    // Choosing a candidate for the phrase at the cursor, which Space and ↓
    // start; any change to the reading ends it.
    virtual bool choosing() const { return false; }
    virtual void set_choosing(bool choosing) { (void)choosing; }
    // Punctuation into the reading at the cursor, to convert with it: the
    // key's character (`<` for ，) and the text it types. False where the
    // engine has no such reading, and the app types it instead.
    virtual bool insert_symbol(char symbol, const std::string& text) {
        (void)symbol, (void)text;
        return false;
    }
    // In preedit(), as byte offsets: the phrase the candidates are for, and
    // the cursor. False when there is nothing to show.
    virtual bool highlight(std::size_t& begin, std::size_t& end, std::size_t& caret) const {
        (void)begin, (void)end, (void)caret;
        return false;
    }
};
// The method's ID, and the folder for what the engine learns: empty learns
// nothing and writes nothing.
using CreateCjkEngine = CjkEngine* (*)(const char* method, const char* learning_dir);
} // namespace framekeyboard
