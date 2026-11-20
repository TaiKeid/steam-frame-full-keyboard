#include "framekeyboard/settings_writer.hpp"
#include "framekeyboard/private_file.hpp"
#include "framekeyboard/startup.hpp"
#include "framekeyboard/stream_support.hpp"

#include <stdexcept>
#include <utility>

namespace framekeyboard {
SettingsWriter::SettingsWriter(fs::path directory, Write write, Write reconcile)
    : shortcuts_store_(shortcuts_path(directory)), has_shortcuts_(!directory.empty()),
      write_(write ? std::move(write)
                   : Write([directory = std::move(directory)](const Settings& settings) {
                         save_settings(directory, settings);
                     })),
      reconcile_(std::move(reconcile)), worker_([this] { run(); }) {
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
    queue(Request{std::move(settings), false});
}
void SettingsWriter::reconcile(Settings settings) {
    if (reconcile_) {
        queue(Request{std::move(settings), true});
    }
}
void SettingsWriter::queue(Request request) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(request);
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
bool SettingsWriter::idle() {
    std::lock_guard lock(mutex_);
    return !pending_ && !writing_ && !shortcuts_pending_ && !shortcuts_writing_ && !private_pending_ &&
           !private_writing_;
}
void SettingsWriter::flush() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] {
        return !pending_ && !writing_ && !shortcuts_pending_ && !shortcuts_writing_ &&
               !private_pending_ && !private_writing_;
    });
}
ShortcutsLoad SettingsWriter::load_shortcuts() {
    std::unique_lock lock(mutex_);
    // A save under way owns the store; a load waits for it.
    changed_.wait(lock, [&] { return !shortcuts_pending_ && !shortcuts_writing_; });
    return has_shortcuts_ ? shortcuts_store_.load() : ShortcutsLoad{};
}
void SettingsWriter::save_private(fs::path file, std::string text) {
    {
        std::lock_guard lock(mutex_);
        private_pending_.emplace(std::move(file), std::move(text));
    }
    changed_.notify_all();
}
void SettingsWriter::save_shortcuts(ShortcutModel model) {
    if (!has_shortcuts_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        shortcuts_pending_ = std::move(model);
        shortcuts_result_.reset();
    }
    changed_.notify_all();
}
std::optional<SettingsWriter::ShortcutsSaved> SettingsWriter::take_shortcuts_result() {
    std::lock_guard lock(mutex_);
    if (shortcuts_pending_ || shortcuts_writing_) {
        return {};
    }
    return std::exchange(shortcuts_result_, std::nullopt);
}
void SettingsWriter::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        changed_.wait(lock,
                      [&] { return pending_ || shortcuts_pending_ || private_pending_ || stopping_; });
        if (private_pending_ && !pending_ && !shortcuts_pending_) {
            auto [file, text] = std::move(*private_pending_);
            private_pending_.reset();
            private_writing_ = true;
            lock.unlock();
            write_private_file(file, text, true);
            lock.lock();
            private_writing_ = false;
            changed_.notify_all();
            continue;
        }
        if (shortcuts_pending_ && !pending_) {
            auto model = std::move(*shortcuts_pending_);
            shortcuts_pending_.reset();
            shortcuts_writing_ = true;
            lock.unlock();
            std::optional<std::string> problem;
            try {
                problem = shortcuts_store_.save(model);
            } catch (const std::exception& error) {
                problem = error.what();
            }
            const bool too_large = problem && shortcuts_store_.refused_too_large();
            lock.lock();
            shortcuts_writing_ = false;
            shortcuts_result_ = ShortcutsSaved{std::move(problem), too_large};
            changed_.notify_all();
            continue;
        }
        if (!pending_) {
            return;
        }
        auto request = std::move(*pending_);
        pending_.reset();
        writing_ = true;
        lock.unlock();
        Result result;
        result.reconcile = request.reconcile;
        try {
            (request.reconcile ? reconcile_ : write_)(request.settings);
        } catch (const StartupPreferencesWarning& warning) {
            const auto* streaming = dynamic_cast<const StreamingSupportWarning*>(&warning);
            // Both failed: the startup flags' warning first, then streaming support's.
            const auto& first = streaming && streaming->startup() ? *streaming->startup() : warning;
            result.startup_warning = first.message();
            result.startup_reason = first.reason();
            if (streaming) {
                result.streaming_support = streaming->saved();
                if (streaming->startup()) {
                    result.streaming_warning = warning.message();
                    result.streaming_reason = warning.reason();
                }
            }
        } catch (const std::exception& error) {
            result.error = error_text(error);
        } catch (...) {
            result.error.english = "unknown settings write failure";
        }
        lock.lock();
        writing_ = false;
        result_ = std::move(result);
        changed_.notify_all();
    }
}
} // namespace framekeyboard
