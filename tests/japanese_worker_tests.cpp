// The conversion worker (src/japanese_worker.cpp): answers in order, the
// newest live request replacing one still waiting, learning from the right
// conversion, every converter call on the worker, and no wakeups when idle.
#include "framekeyboard/japanese_worker.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <glib.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
// What a converter was asked, and on which thread; a gate that holds its
// conversions, as a slow Mozc would.
struct Log {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::string> calls;
    std::vector<std::thread::id> threads;
    bool held = false, holding = false;
    bool alive = false;
    void note(std::string call) {
        std::unique_lock guard(lock);
        calls.push_back(std::move(call));
        threads.push_back(std::this_thread::get_id());
        holding = true;
        changed.notify_all();
        changed.wait(guard, [&] { return !held; });
        holding = false;
    }
    void hold() {
        std::lock_guard guard(lock);
        held = true;
    }
    void release() {
        std::lock_guard guard(lock);
        held = false;
        changed.notify_all();
    }
    // Until a conversion waits at the gate.
    void until_holding() {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] { return holding; });
    }
    std::vector<std::string> taken() {
        std::lock_guard guard(lock);
        return calls;
    }
};
// Two kana a segment, each segment's candidates its reading in 〈〉 and as typed.
struct Converter : JapaneseConverter {
    Log* log;
    std::string reading;
    explicit Converter(Log* to) : log(to) {
        std::lock_guard guard(log->lock);
        log->alive = true;
        log->threads.push_back(std::this_thread::get_id());
        log->calls.push_back("made");
    }
    ~Converter() override {
        std::lock_guard guard(log->lock);
        log->alive = false;
        log->threads.push_back(std::this_thread::get_id());
        log->calls.push_back("freed");
    }
    std::vector<Segment> convert(const std::string& text) override {
        log->note("convert " + text);
        reading = text;
        std::vector<Segment> result;
        int count = 0;
        for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
            if (count++ % 2 == 0) {
                result.push_back({});
            }
            result.back().reading += std::string(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        }
        for (auto& segment : result) {
            segment.candidates = {"〈" + segment.reading + "〉", segment.reading};
        }
        return result;
    }
    std::vector<Segment> resize(int, int) override { return {}; }
    void learn(const std::vector<std::string>& chosen) override {
        std::string text;
        for (const auto& part : chosen) {
            text += part;
        }
        log->note("learn " + text + " from " + reading);
    }
};
JapaneseComposer::Factory factory(Log& log) {
    return [&log] { return std::make_unique<Converter>(&log); };
}
// The keyboard's composer: converting is the worker's.
JapaneseComposer typing(const std::string& reading) {
    JapaneseComposer composer;
    composer.defer_conversion();
    composer.set_learning(true);
    composer.kana(reading);
    return composer;
}
JapaneseWorker::Step live() {
    return [](JapaneseComposer& composer) {
        composer.prepare_live();
        return true;
    };
}
std::vector<JapaneseWorker::Answer> answers(JapaneseWorker& worker) {
    worker.wait();
    return worker.take();
}
// A copy converted on the worker gives what converting here gives.
void same_as_here() {
    Log log;
    JapaneseWorker worker(factory(log));
    auto composer = typing("きょうはいい");
    try {
        composer.prepare_live();
        require(false, "the keyboard's composer converts nothing itself");
    } catch (const JapaneseComposer::Deferred&) {
    }
    composer = typing("きょうはいい");
    worker.post(JapaneseWorker::Kind::Live, composer, live());
    auto done = answers(worker);
    require(done.size() == 1 && done.front().composer && done.front().error.empty(), "one live answer");
    composer.adopt_live(*done.front().composer);
    JapaneseComposer here(factory(log));
    here.kana("きょうはいい");
    here.prepare_live();
    require(composer.live() == here.live() && composer.live_current(),
            "live candidates as converting here gives");
    // 変換 then uses the live answer, without converting again.
    const auto calls = log.taken().size();
    composer.convert();
    require(log.taken().size() == calls && composer.converting() && composer.segment_count() == 3,
            "変換 reuses the live answer");
}
// While one conversion runs, live requests behind it give way to the newest,
// but never across another request.
void newest_live_request() {
    Log log;
    JapaneseWorker worker(factory(log));
    log.hold();
    const auto first = worker.post(JapaneseWorker::Kind::Live, typing("あ"), live());
    log.until_holding();
    worker.post(JapaneseWorker::Kind::Live, typing("あい"), live());
    const auto step = worker.post(JapaneseWorker::Kind::Step, typing("あいう"), live());
    worker.post(JapaneseWorker::Kind::Live, typing("あいうえ"), live());
    const auto last = worker.post(JapaneseWorker::Kind::Live, typing("あいうえお"), live());
    require(worker.outstanding() == 4, "four answers to come");
    log.release();
    const auto done = answers(worker);
    std::vector<std::uint64_t> ids;
    for (const auto& answer : done) {
        ids.push_back(answer.id);
    }
    require(ids == std::vector<std::uint64_t>{first, first + 1, step, last},
            "answers in order, the waiting live requests replaced by the newest");
    require(done.back().composer->live().front() == "〈あい〉〈うえ〉〈お〉", "the newest reading converted");
    require(worker.outstanding() == 0, "all taken");
}
// Learning a phrase whose conversion the converter no longer holds converts
// it again first, keeping its number; one it holds learns at once.
void learns_from_its_conversion() {
    Log log;
    JapaneseWorker worker(factory(log));
    auto composer = typing("あした");
    worker.post(JapaneseWorker::Kind::Live, composer, live());
    composer.adopt_live(*answers(worker).front().composer);
    composer.convert();
    const auto phrase = composer;
    // Another reading converted meanwhile, as a newer live request might.
    worker.post(JapaneseWorker::Kind::Live, typing("あさって"), live());
    answers(worker);
    worker.post(JapaneseWorker::Kind::Learn, phrase, [](JapaneseComposer& copy) {
        copy.learn();
        return true;
    });
    answers(worker);
    auto calls = log.taken();
    require(calls.size() >= 3 && calls[calls.size() - 2] == "convert あした" &&
                calls.back() == "learn 〈あし〉〈た〉 from あした",
            "the phrase is converted again before it is learned");
    worker.post(JapaneseWorker::Kind::Learn, phrase, [](JapaneseComposer& copy) {
        copy.learn();
        return true;
    });
    answers(worker);
    calls = log.taken();
    require(calls.back() == "learn 〈あし〉〈た〉 from あした" &&
                calls[calls.size() - 2] == "learn 〈あし〉〈た〉 from あした",
            "held already, it is learned without converting");
}
// A discarded composition drops what waits but learning and tasks.
void drops_waiting() {
    Log log;
    JapaneseWorker worker(factory(log));
    log.hold();
    worker.post(JapaneseWorker::Kind::Live, typing("あ"), live());
    log.until_holding();
    worker.post(JapaneseWorker::Kind::Step, typing("い"), live());
    bool ran = false;
    worker.post_task([&] { ran = true; });
    worker.post(JapaneseWorker::Kind::Live, typing("う"), live());
    worker.drop_waiting();
    require(worker.outstanding() == 2, "the running one and the task remain");
    log.release();
    const auto done = answers(worker);
    require(done.size() == 2 && done[1].kind == JapaneseWorker::Kind::Task && ran, "the task ran");
    const auto calls = log.taken();
    require(std::count(calls.begin(), calls.end(), "convert い") == 0 &&
                std::count(calls.begin(), calls.end(), "convert う") == 0,
            "the dropped ones never converted");
}
// The converter is made, used and freed on the worker alone; finishing
// runs learning still waiting, frees the converter, then the last task.
void worker_thread_only() {
    Log log;
    std::vector<std::string> at_last;
    {
        JapaneseWorker worker(factory(log));
        auto composer = typing("ねこ");
        worker.post(JapaneseWorker::Kind::Live, composer, live());
        composer.adopt_live(*answers(worker).front().composer);
        composer.convert();
        log.hold();
        worker.post(JapaneseWorker::Kind::Live, typing("いぬ"), live());
        log.until_holding();
        worker.post(JapaneseWorker::Kind::Live, typing("いぬが"), live());
        worker.post(JapaneseWorker::Kind::Learn, composer, [](JapaneseComposer& copy) {
            copy.learn();
            return true;
        });
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            log.release();
        });
        worker.finish([&] {
            std::lock_guard guard(log.lock);
            at_last.push_back(log.alive ? "converter alive" : "converter freed");
        });
        release.join();
        require(worker.post(JapaneseWorker::Kind::Live, typing("あ"), live()) == 0,
                "nothing is taken once finished");
    }
    const auto calls = log.taken();
    require(std::count(calls.begin(), calls.end(), "convert いぬが") == 0, "a waiting live request is dropped");
    require(calls.back() == "freed" && calls[calls.size() - 2] == "learn 〈ねこ〉 from ねこ",
            "waiting learning runs, then the converter goes");
    require(at_last == std::vector<std::string>{"converter freed"}, "the last task runs after that");
    std::lock_guard guard(log.lock);
    require(std::all_of(log.threads.begin(), log.threads.end(),
                        [&](const std::thread::id id) { return id == log.threads.front(); }) &&
                log.threads.front() != std::this_thread::get_id(),
            "every converter call on the worker thread");
}
// Idle, the worker sleeps: nothing wakes it until it is asked again.
void sleeps_when_idle() {
    Log log;
    JapaneseWorker worker(factory(log));
    require(worker.take().empty() && worker.wakeups() == 0, "no thread before the first request");
    worker.post(JapaneseWorker::Kind::Live, typing("あ"), live());
    answers(worker);
    const auto woken = worker.wakeups();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(worker.wakeups() == woken && worker.take().empty(), "no wakeups while idle");
}
// A step that throws answers with the error and the copy as it was left.
void step_errors() {
    JapaneseWorker worker([]() -> std::unique_ptr<JapaneseConverter> {
        throw std::runtime_error("Japanese conversion needs the Anthy library");
    });
    worker.post(JapaneseWorker::Kind::Step, typing("あ"), [](JapaneseComposer& copy) {
        copy.convert();
        return true;
    });
    const auto done = answers(worker);
    require(done.size() == 1 && done.front().error == "Japanese conversion needs the Anthy library" &&
                !done.front().composer->converting(),
            "the error comes back with the copy");
}
} // namespace
int main() {
    try {
        same_as_here();
        newest_live_request();
        learns_from_its_conversion();
        drops_waiting();
        worker_thread_only();
        sleeps_when_idle();
        step_errors();
    } catch (const std::exception& error) {
        std::cerr << "japanese-worker: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
