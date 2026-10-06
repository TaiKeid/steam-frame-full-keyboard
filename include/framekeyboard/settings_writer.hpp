#pragma once

#include "config.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace framekeyboard {
// Owns only preference snapshots, never App/input state. One worker serializes
// atomic writes; pending snapshots coalesce so an older write cannot win later.
class SettingsWriter {
  public:
    using Write = std::function<void(const Settings&)>;
    struct Result {
        std::string error;
    };
    explicit SettingsWriter(fs::path directory, Write write = {});
    ~SettingsWriter();
    SettingsWriter(const SettingsWriter&) = delete;
    SettingsWriter& operator=(const SettingsWriter&) = delete;
    void enqueue(Settings settings);
    std::optional<Result> take_result();
    // Explicit durability barrier for shutdown, config reload and tests.
    void flush();

  private:
    void run();
    Write write_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Settings> pending_;
    std::optional<Result> result_;
    bool writing_{}, stopping_{};
    std::thread worker_;
};
} // namespace framekeyboard
