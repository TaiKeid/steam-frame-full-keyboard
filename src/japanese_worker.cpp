#include "framekeyboard/japanese_worker.hpp"
#include <algorithm>
#include <chrono>
#include <glib.h>
#include <iostream>
#include <sstream>

namespace framekeyboard {
JapaneseConverter& JapaneseWorker::Counted::inner() {
    if (!inner_) {
        inner_ = factory_ ? factory_() : anthy_converter();
    }
    return *inner_;
}
std::vector<JapaneseConverter::Segment> JapaneseWorker::Counted::convert(const std::string& reading) {
    ++changes; // Even one that fails: whatever it held may be gone.
    return inner().convert(reading);
}
std::vector<JapaneseConverter::Segment> JapaneseWorker::Counted::resize(int index, int delta) {
    ++changes;
    return inner().resize(index, delta);
}
JapaneseWorker::JapaneseWorker(JapaneseComposer::Factory factory) : converter_(std::move(factory)) {
}
JapaneseWorker::~JapaneseWorker() {
    finish();
}
std::uint64_t JapaneseWorker::post(Kind kind, JapaneseComposer composer, Step step) {
    Job job;
    job.kind = kind;
    job.composer = std::move(composer);
    job.step = std::move(step);
    return enqueue(std::move(job));
}
std::uint64_t JapaneseWorker::post_task(std::function<void()> work, std::function<void()> done) {
    Job job;
    job.kind = Kind::Task;
    job.work = std::move(work);
    job.done = std::move(done);
    return enqueue(std::move(job));
}
std::uint64_t JapaneseWorker::enqueue(Job job) {
    std::lock_guard guard(lock_);
    if (closing_ || finished_) {
        return 0;
    }
    job.id = ++next_id_;
    // A live request waiting at the end of the queue is out of date: the
    // newest takes its place. Never past another request, which may need
    // what that one converted.
    if (job.kind == Kind::Live && !jobs_.empty() && jobs_.back().kind == Kind::Live) {
        jobs_.back() = std::move(job);
    } else {
        jobs_.push_back(std::move(job));
        ++outstanding_;
    }
    const auto id = jobs_.back().id;
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { run(); });
    }
    wake_.notify_one();
    return id;
}
std::vector<JapaneseWorker::Answer> JapaneseWorker::take() {
    if (ready_.load(std::memory_order_acquire) == 0) {
        return {};
    }
    std::vector<Answer> taken;
    {
        std::lock_guard guard(lock_);
        taken.assign(std::make_move_iterator(answers_.begin()), std::make_move_iterator(answers_.end()));
        answers_.clear();
        ready_.store(0, std::memory_order_release);
        outstanding_ -= taken.size();
    }
    return taken; // Old copies are freed here, outside the lock.
}
std::size_t JapaneseWorker::outstanding() const {
    std::lock_guard guard(lock_);
    return outstanding_;
}
void JapaneseWorker::drop_waiting() {
    std::deque<Job> dropped;
    {
        std::lock_guard guard(lock_);
        const auto kept = std::stable_partition(jobs_.begin(), jobs_.end(), [](const Job& job) {
            return job.kind == Kind::Learn || job.kind == Kind::Task;
        });
        dropped.assign(std::make_move_iterator(kept), std::make_move_iterator(jobs_.end()));
        jobs_.erase(kept, jobs_.end());
        outstanding_ -= dropped.size();
    }
    idle_.notify_all(); // Someone may wait for exactly those.
}
void JapaneseWorker::wait() {
    std::unique_lock guard(lock_);
    idle_.wait(guard, [&] { return jobs_.empty() && !running_; });
}
void JapaneseWorker::finish(std::function<void()> last) {
    {
        std::lock_guard guard(lock_);
        if (finished_) {
            return;
        }
        closing_ = true;
    }
    drop_waiting();
    if (!thread_.joinable() && last) {
        // Nothing ever started the thread, and so nothing ever converted.
        thread_ = std::thread([this] { run(); });
    }
    {
        std::lock_guard guard(lock_);
        if (last) {
            Job job;
            job.kind = Kind::Task;
            job.work = std::move(last);
            jobs_.push_back(std::move(job)); // run() frees the converter before it.
            ++outstanding_;
        }
        finished_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}
void JapaneseWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock guard(lock_);
            wake_.wait(guard, [&] { return finished_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                break; // Finished, with nothing left to run.
            }
            // The last one finish() asked for runs after the converter, and
            // with it Mozc's session, went.
            if (finished_ && jobs_.size() == 1 && jobs_.front().id == 0) {
                guard.unlock();
                converter_.release();
                guard.lock();
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            running_ = true;
        }
        wakeups_.fetch_add(1);
        auto done = answer(std::move(job));
        {
            std::lock_guard guard(lock_);
            answers_.push_back(std::move(done));
            ready_.store(answers_.size(), std::memory_order_release);
            running_ = false;
        }
        idle_.notify_all();
    }
    converter_.release(); // On this thread, which made it.
    std::lock_guard guard(lock_);
    running_ = false;
    idle_.notify_all();
}
JapaneseWorker::Answer JapaneseWorker::answer(Job job) {
    Answer result;
    result.id = job.id;
    result.kind = job.kind;
    if (job.kind == Kind::Task) {
        try {
            job.work();
        } catch (const std::exception&) {
            // A task reports what failed through what `done` finds.
        }
        result.done = std::move(job.done);
        return result;
    }
    auto& copy = *job.composer;
    copy.use_converter(&converter_);
    const auto before = converter_.changes;
#ifdef FRAMEKEYBOARD_FRAME_TRACE
    // From here: a learning step's converting its phrase again counts too.
    const auto started = std::chrono::steady_clock::now();
#endif
    bool run_step = true;
    if (job.kind != Kind::Learn) {
        copy.set_record(record_);
    } else if (copy.record().conversion != record_.conversion) {
        // The converter converted another reading since the phrase's: it
        // learns from the conversion it holds, so that one comes back
        // first, keeping its number.
        try {
            run_step = copy.reconvert();
        } catch (const std::exception&) {
            run_step = false;
        }
    }
    const auto restored = converter_.changes;
    if (run_step) {
        try {
            result.result = job.step(copy);
        } catch (const std::exception& error) {
            result.error = error.what(); // The copy stays as the throw left it.
        }
    }
#ifdef FRAMEKEYBOARD_FRAME_TRACE
    // Measuring (FRAMEKEYBOARD_FRAME_TRACE): how long each conversion took,
    // and the reading's length. Never its text.
    if (converter_.changes != before) {
        std::ostringstream line;
        line << "conversion-trace kind="
             << (job.kind == Kind::Live ? "live" : job.kind == Kind::Learn ? "learn" : "step")
             << " ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
             << " kana=" << g_utf8_strlen(copy.reading().c_str(), -1)
             << " conversions=" << converter_.changes - before << '\n';
        std::cout << line.str() << std::flush;
    }
#endif
    auto record = copy.record(); // fell_back() while the converter is still attached.
    if (converter_.changes != restored) {
        record.conversion = ++conversions_;
    }
    if (run_step) {
        record_ = record;
    } else if (converter_.changes != before) {
        // Converting the phrase again failed: what the converter holds now
        // is no conversion anyone has.
        record_ = {};
        record_.conversion = ++conversions_;
    }
    copy.set_record(record);
    copy.use_converter(nullptr); // The copy goes to another thread.
    if (job.kind != Kind::Learn) {
        result.composer = std::move(copy); // Learning's: nobody reads it.
    }
    return result;
}
} // namespace framekeyboard
