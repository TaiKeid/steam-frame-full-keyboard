#pragma once
// Japanese conversion off the VR thread (docs/architecture.md, Japanese
// conversion). One thread owns the converter and makes every call to Mozc
// and Anthy, in the order they were asked for, so a slow conversion holds
// up no frame. It runs steps of the composer on copies of it and hands the
// copies back as answers, which the asking thread takes in that order. It
// sleeps while nothing is asked: no timed wakeups.
#include "framekeyboard/japanese.hpp"
#include "framekeyboard/localization.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace framekeyboard {
class JapaneseWorker {
  public:
    // What a request is for, which decides what may go without its answer.
    enum class Kind {
        Live,  // Live candidates: a newer one replaces it while it waits to start.
        Step,  // Another step of the composer's: 変換, a resize, a pick.
        Learn, // Learning a committed phrase: never dropped.
        Task,  // Other engine work, such as Mozc's start, stop and Clear: never dropped.
    };
    // A step run on a copy of the composer, and its result, such as
    // choose_live()'s.
    using Step = std::function<bool(JapaneseComposer&)>;
    struct Answer {
        std::uint64_t id{};
        Kind kind{};
        // The copy after the step, without the worker's converter; none for
        // tasks and learning.
        std::optional<JapaneseComposer> composer;
        bool result{};
        ErrorText error;            // What the step threw, if it did.
        std::function<void()> done; // A task's, for the asking thread.
    };
    // The converter is made with `factory` on the worker, when a step first
    // needs it, and freed there as it finishes.
    explicit JapaneseWorker(JapaneseComposer::Factory factory);
    ~JapaneseWorker();
    JapaneseWorker(const JapaneseWorker&) = delete;
    JapaneseWorker& operator=(const JapaneseWorker&) = delete;
    // Runs `step` on `composer`, a copy, with the worker's converter and its
    // record of what that converter last converted. A learning step keeps
    // the copy's own record, and the converter is first made to hold that
    // phrase's conversion again if it converted something else since.
    // Nothing once finished: 0.
    std::uint64_t post(Kind kind, JapaneseComposer composer, Step step);
    // Runs `work` on the worker, then hands back `done` as an answer.
    std::uint64_t post_task(std::function<void()> work, std::function<void()> done = {});
    // The answers ready, in the order asked. The VR thread asks every frame:
    // with none, this is one atomic read.
    std::vector<Answer> take();
    // Requests asked whose answers are not taken yet.
    std::size_t outstanding() const;
    // Drops live candidates and steps not yet started, as for a composition
    // discarded: their answers never come. Learning and tasks still run.
    void drop_waiting();
    // Waits until every request asked so far has its answer ready. Only the
    // candidates report and tests wait; the keyboard never does.
    void wait();
    // Ends the worker: drops what drop_waiting() drops, runs the rest, frees
    // the converter, runs `last` (as stopping Mozc, after its session went)
    // and joins the thread. Later requests are ignored.
    void finish(std::function<void()> last = {});
    // The times the thread woke to run something, for tests of its idling.
    std::uint64_t wakeups() const { return wakeups_.load(); }
    // Whether the thread was started, by a request or a finish with work.
    bool started() const {
        std::lock_guard guard(lock_);
        return thread_.joinable();
    }

  private:
    struct Job {
        std::uint64_t id{};
        Kind kind{};
        std::optional<JapaneseComposer> composer;
        Step step;
        std::function<void()> work, done;
    };
    // The converter as the worker's copies see it: made on first use, and
    // counting the conversions and resizes it makes, which change what it
    // last converted.
    class Counted final : public JapaneseConverter {
      public:
        explicit Counted(JapaneseComposer::Factory factory) : factory_(std::move(factory)) {}
        std::vector<Segment> convert(const std::string& reading) override;
        std::vector<Segment> resize(int index, int delta) override;
        void learn(const std::vector<std::string>& chosen) override { inner().learn(chosen); }
        std::vector<std::string> predict(const std::string& reading) override {
            return inner().predict(reading);
        }
        void learn_prediction(const std::string& reading, const std::string& text) override {
            inner().learn_prediction(reading, text);
        }
        bool weighs_context() const override { return inner_ && inner_->weighs_context(); }
        bool fell_back() const override { return inner_ && inner_->fell_back(); }
        void release() { inner_.reset(); }
        std::uint64_t changes{};

      private:
        JapaneseConverter& inner();
        JapaneseComposer::Factory factory_;
        std::unique_ptr<JapaneseConverter> inner_;
    };
    std::uint64_t enqueue(Job job);
    void run();
    Answer answer(Job job);
    Counted converter_; // The worker thread's alone.
    // The worker's record of what its converter last converted, and its
    // count of conversions. The worker thread's alone.
    JapaneseComposer::Record record_;
    std::uint64_t conversions_{};
    mutable std::mutex lock_; // Held only to hand jobs and answers over.
    std::condition_variable wake_, idle_;
    std::deque<Job> jobs_;
    std::deque<Answer> answers_;
    std::atomic<std::size_t> ready_{};
    std::size_t outstanding_{};
    std::uint64_t next_id_{};
    bool running_{}, closing_{}, finished_{};
    std::atomic<std::uint64_t> wakeups_{};
    std::thread thread_; // Started by the first request.
};
} // namespace framekeyboard
