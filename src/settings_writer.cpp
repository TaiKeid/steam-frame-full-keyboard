#include "framekeyboard/settings_writer.hpp"

#include <stdexcept>

namespace framekeyboard {
SettingsWriter::SettingsWriter(fs::path directory, Write write)
    : write_(write ? std::move(write)
                   : Write([directory = std::move(directory)](const Settings& settings) {
                         save_settings(directory, settings);
                     })),
      worker_([this] { run(); }) {
}
SettingsWriter::~SettingsWriter() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    worker_.join(); // The worker drains the final snapshot before stopping.
}
void SettingsWriter::enqueue(Settings settings) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(settings);
        result_.reset();
    }
    changed_.notify_all();
}
std::optional<SettingsWriter::Result> SettingsWriter::take_result() {
    std::lock_guard lock(mutex_);
    if (pending_ || writing_) {
        return {}; // Never report an obsolete error while a newer save is pending.
    }
    auto result = std::move(result_);
    result_.reset();
    return result;
}
void SettingsWriter::flush() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return !pending_ && !writing_; });
}
void SettingsWriter::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        changed_.wait(lock, [&] { return pending_ || stopping_; });
        if (!pending_) {
            return;
        }
        auto settings = std::move(*pending_);
        pending_.reset();
        writing_ = true;
        lock.unlock();
        Result result;
        try {
            write_(settings);
        } catch (const std::exception& error) {
            result.error = error.what();
        } catch (...) {
            result.error = "unknown settings write failure";
        }
        lock.lock();
        writing_ = false;
        result_ = std::move(result);
        changed_.notify_all();
    }
}
} // namespace framekeyboard
