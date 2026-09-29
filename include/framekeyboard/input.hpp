#pragma once

#include "config.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace framekeyboard {
int key_code(const std::string& name);
bool is_modifier(int code);

// The sink receives Linux evdev codes. No renderer or profile can bypass this boundary.
class KeySink {
  public:
    virtual ~KeySink() = default;
    virtual void send(int code, int value) = 0;
    virtual bool pump() { return true; }
};
class NullSink : public KeySink {
  public:
    void send(int, int) override {};
};
class UInputSink : public KeySink {
  public:
    UInputSink();
    ~UInputSink() override;
    UInputSink(const UInputSink&) = delete;
    UInputSink& operator=(const UInputSink&) = delete;
    void send(int code, int value) override;
    fs::path event_node() const;

  private:
    int fd_{-1};
    std::set<int> held_;
};

class LanguageMap {
  public:
    explicit LanguageMap(const Language& language);
    ~LanguageMap();
    LanguageMap(const LanguageMap&) = delete;
    LanguageMap& operator=(const LanguageMap&) = delete;
    std::string legend(const Key& key, const std::set<int>& modifiers, bool caps, bool num) const;

  private:
    Language language_;
    xkb_context* context_{};
    xkb_keymap* keymap_{};
};

class KeyboardState {
  public:
    explicit KeyboardState(KeySink& sink) : sink_(sink) {}
    ~KeyboardState();
    void down(unsigned pointer, const Key& key, double now);
    void up(unsigned pointer);
    void cancel_all();
    bool tick(double now);
    bool pressed(const std::string& id) const;
    std::set<int> modifiers() const;
    bool caps() const { return caps_; }
    bool num() const { return num_; }
    bool active() const { return !presses_.empty(); }

  private:
    struct Press {
        std::string id;
        int modifier{};
        bool used{};
        std::vector<int> codes;
        int repeat_code{};
        double repeat_at{}, expires_at{};
    };
    void acquire(int code);
    void release(int code);
    KeySink& sink_;
    std::map<unsigned, Press> presses_;
    std::map<int, unsigned> references_;
    std::set<int> latched_;
    bool caps_{false}, num_{false};
};
} // namespace framekeyboard
