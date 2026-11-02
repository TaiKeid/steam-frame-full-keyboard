#include "framekeyboard/output.hpp"

#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Transport : KeySink {
    std::vector<std::string> events;
    bool pending{}, done{}, success{true}, slow_keys{}, native_pending{}, native_done{};
    bool native_success{true}, barrier{};
    std::size_t delivered{}; // Characters of a failed commit already acknowledged.
    std::string text;
    void send(int code, int value) override {
        events.push_back(std::to_string(code) + ":" + std::to_string(value));
    }
    Delivery deliver_key(int code, int value) override {
        if (!slow_keys || value == 0) {
            send(code, value);
            return Delivery::Success;
        }
        if (native_pending) {
            if (!native_done) {
                return Delivery::Waiting;
            }
            native_pending = false;
            return native_success ? Delivery::Success : Delivery::Failure;
        }
        send(code, value);
        native_pending = true;
        native_done = false;
        return Delivery::Waiting;
    }
    bool commit_text(const std::string& value) override {
        if (pending) {
            require(value == text, "pending text must stay immutable");
            if (!done) {
                return false;
            }
            pending = false;
            return success;
        }
        events.push_back(value);
        text = value;
        pending = true;
        done = false;
        return false;
    }
    bool replaces_text() override { return true; }
    bool replace_text(std::size_t delete_before, const std::string& value) override {
        if (pending) {
            return commit_text(value); // Same pending request and result.
        }
        events.push_back("-" + std::to_string(delete_before) + ":" + value);
        text = value;
        pending = true;
        done = false;
        return false;
    }
    bool text_pending() const override { return pending; }
    std::size_t delivered_characters() const override { return delivered; }
    bool output_blocked() const override { return barrier || pending; }
    void cancel_text() override { pending = false; }
    void cancel_delivery() override { pending = native_pending = false; }
};
void character(OutputController& output, unsigned pointer, const std::string& text, double now = 1) {
    require(output.admit(true), "character admitted");
    require(output.text_press(pointer, text, now), "character queued");
    output.release(pointer);
}
void ordering() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    character(output, 1, "b");
    require(!output.take_status(), "normal pending text and queued presses stay silent");
    require(output.admit(true), "Backspace admitted behind characters");
    output.native_press(2, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(2);
    character(output, 3, "c");
    require(sink.events == std::vector<std::string>{"a"}, "later output waits for the first result");
    sink.done = true;
    output.tick(2);
    require(!output.take_status(), "the next pending delivery stays silent");
    require(sink.events == std::vector<std::string>{"a", "b"}, "second character follows first ack");
    sink.done = true;
    output.tick(3);
    require(sink.events == std::vector<std::string>{"a", "b", "14:1", "14:0", "c"},
            "text, Backspace and text preserve press order and release pairing");
    sink.done = true;
    output.tick(4);
    require(!output.text_pending(), "last result consumed after release");
    require(!output.take_status(), "successful delivery does not flash the status area");
}
void failure_and_retry() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    character(output, 1, "b");
    output.native_press(2, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(2);
    character(output, 3, "c");
    sink.done = true;
    sink.success = false;
    output.tick(2);
    require(sink.events == std::vector<std::string>{"a"} && output.retry_text() == "ab",
            "failed text retains only the prefix before discarded native actions");
    require(output.take_status()->find("discarded") != std::string::npos,
            "discarded native actions are reported");
    output.tick(100);
    require(sink.events.size() == 1, "failed delivery never retries automatically");
    sink.barrier = true;
    require(!output.retry() && sink.events.size() == 1, "retry respects a late compositor barrier");
    sink.barrier = false;
    sink.success = true;
    require(output.retry(), "one explicit retry admits the retained prefix");
    sink.done = true;
    output.tick(101);
    sink.done = true;
    output.tick(102);
    require(sink.events == std::vector<std::string>{"a", "a", "b"},
            "retry never replays Backspace or text beyond its boundary");
}
// A per-character commit can time out after some characters were acknowledged
// ("hé" of "héllo"); retrying the whole text would type them twice.
void partial_retry() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    require(output.admit(true) && output.commit_text("héllo"), "word queued");
    require(output.admit(true) && output.commit_text(" next"), "later text queued");
    sink.delivered = 2;
    sink.done = true;
    sink.success = false;
    output.tick(2);
    require(output.retry_text() == "llo next",
            "retry keeps only the unacknowledged rest and later text");
    sink.delivered = 0;
    sink.success = true;
    require(output.retry(), "the rest can be retried");
    sink.done = true;
    output.tick(3);
    sink.done = true;
    output.tick(4);
    require(sink.events == std::vector<std::string>{"héllo", "llo", " next"},
            "acknowledged characters are never typed again");
    require(output.admit(true) && output.commit_text("again"), "next word queued");
    sink.done = true;
    sink.success = false;
    output.tick(5);
    require(output.retry_text() == "again",
            "a failure with nothing acknowledged retries the whole text");
}
// A replacement that timed out may still have been applied. Retrying it would
// delete again, reaching text before the word ("Xhello" -> "Xhi" -> "hi").
void ambiguous_replacement() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    require(output.replace_text(5, "hi"), "replacement queued behind text");
    character(output, 1, "b");
    sink.done = true;
    sink.success = false;
    output.tick(2);
    require(output.retry_text() == "a", "a failure before a replacement keeps only the text before it");
    sink.success = true;
    require(output.retry(), "the retained text can be retried");
    sink.done = true;
    output.tick(3);
    require(sink.events == std::vector<std::string>{"a", "a"}, "the replacement is not replayed");

    require(output.replace_text(5, "hi"), "replacement queued");
    sink.done = true;
    sink.success = false; // The compositor's answer arrived after the deadline.
    output.tick(4);
    require(output.retry_text().empty() && !output.retry(),
            "a replacement with unknown delivery is never retried");
    require(sink.events == std::vector<std::string>{"a", "a", "-5:hi"},
            "the deletion was sent exactly once");
}
void held_keys() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    output.native_press(1, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.tick(20);
    require(sink.events == std::vector<std::string>{"a"}, "queued held key never repeats early");
    sink.done = true;
    output.tick(20);
    output.tick(20.49);
    require(sink.events == std::vector<std::string>{"a", "14:1"},
            "repeat delay begins at delivery, not acceptance");
    output.tick(20.51);
    output.release(1);
    output.tick(100);
    require(sink.events == std::vector<std::string>{"a", "14:1", "14:2", "14:0"},
            "held key repeats and receives exactly one release");
}
void release_and_capacity() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    for (unsigned i = 0; i < OutputController::capacity; ++i) {
        character(output, i + 1, "x");
    }
    require(!output.admit(true), "press beyond queue bound is rejected");
    require(output.take_status()->find("full") != std::string::npos, "queue bound has a status");
    output.release(0);
    require(sink.events == std::vector<std::string>{"14:1", "14:0"},
            "full queue cannot prevent a delivered key's release");
    output.tick(2);
    require(sink.events.back() == "x", "queued text starts after the physical hold ends");
    output.cancel();
    sink.done = true;
    output.tick(3);
    require(sink.events.size() == 3, "cancellation never drains the old queue");
}
void modifier_snapshot() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    output.native_press(1, {KEY_LEFTCTRL, KEY_C}, KEY_C, 1);
    output.release_modifier(KEY_LEFTCTRL);
    sink.done = true;
    output.tick(2);
    output.release(1);
    require(sink.events == std::vector<std::string>{"a", "29:1", "46:1", "29:0", "46:0"},
            "queued chord retains press-time modifiers even when released before delivery");
}
void native_acknowledgement() {
    Transport sink;
    sink.slow_keys = true;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    output.release(0);
    character(output, 1, "b");
    require(sink.events == std::vector<std::string>{"14:1", "14:0"},
            "release passes while native acknowledgement still blocks later text");
    sink.native_done = true;
    sink.native_success = false;
    output.tick(2);
    require(sink.events.size() == 2 && !output.text_pending(),
            "failed native action discards subsequent text even after its release");
}
void delayed_admission_clock() {
    Transport sink;
    sink.slow_keys = true;
    OutputController output(sink);
    output.set_enabled(true);
    output.native_press(0, {KEY_BACKSPACE}, KEY_BACKSPACE, 1);
    sink.native_done = true;
    require(output.admit(true, 20), "a new key consumes an earlier native acknowledgement");
    output.tick(20.1);
    require(sink.events == std::vector<std::string>{"14:1"},
            "a long gap before admission cannot start repeat from an old frame timestamp");
    output.release(0);
}
void composition_ownership() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "a");
    sink.done = true;
    sink.success = false;
    require(output.commit_text("preedit"),
            "accepted preedit transfers ownership even if an earlier result fails during enqueue");
    require(output.retry_text() == "apreedit" && sink.events == std::vector<std::string>{"a"},
            "failed prefix and accepted preedit are retained once, without another transport send");
}
std::string repeated(const std::string& character, std::size_t count) {
    std::string result;
    while (count-- > 0) {
        result += character;
    }
    return result;
}
void long_composition() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    const auto first = repeated("日", 32);
    require(output.commit_text(first + "月"), "a 33-character composition transfers ownership");
    require(sink.events == std::vector<std::string>{first}, "only the first transport chunk starts");
    sink.done = true;
    output.take_status();
    require(output.tick(1), "acknowledging a chunk reports progress");
    require(!output.tick(1.119), "an unchanged settling wait does not request UI redraws");
    require(sink.events.size() == 1, "the next chunk waits beyond the temporary keymap reset");
    require(output.tick(1.121), "starting the next chunk reports progress");
    require(sink.events == std::vector<std::string>{first, "月"},
            "UTF-8 chunks split on character boundaries");
    sink.done = true;
    output.tick(2);
    require(!output.text_pending(), "both chunks complete");
    character(output, 0, "next", 2.01);
    require(sink.events.size() == 2, "following input cannot immediately replace the last chunk's map");
    output.tick(2.121);
    require(sink.events.back() == "next", "following input resumes after the paced chunk");
}
void long_composition_capacity() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    for (unsigned i = 0; i < OutputController::capacity - 1; ++i) {
        character(output, i, "x");
    }
    require(output.can_queue(1) && !output.can_queue(2), "one queue entry remains");
    require(!output.commit_text(repeated("日", 33)), "a two-chunk batch is refused as a whole");
    require(output.can_queue(1) && sink.events == std::vector<std::string>{"x"},
            "refusal queues and delivers no prefix of the composition");
    output.cancel();
    require(!output.commit_text(repeated("日", 1025)) && sink.events.size() == 1,
            "a composition beyond the bounded queue contract is refused");
    require(output.commit_text(repeated("日", 1024)) && !output.can_queue(1),
            "the maximum composition occupies exactly 32 bounded entries");
}
void long_composition_after_pending_text() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    character(output, 0, "prior");
    const auto text = repeated("日", 33);
    require(output.commit_text(text), "a long composition queues behind prior text");
    sink.done = true;
    output.tick(2);
    require(sink.events == std::vector<std::string>{"prior"},
            "even the first batch chunk waits after prior text's acknowledgement");
    output.tick(2.121);
    require(sink.events == std::vector<std::string>{"prior", repeated("日", 32)},
            "the first chunk follows the earlier request and its settling gap");
}
void long_composition_retry() {
    Transport sink;
    OutputController output(sink);
    output.set_enabled(true);
    const auto first = repeated("日", 32), second = repeated("月", 32);
    require(output.commit_text(first + second + "水火"), "all chunks are accepted together");
    sink.done = true;
    output.tick(1);
    output.tick(1.121);
    sink.done = true;
    sink.success = false;
    sink.delivered = 5;
    output.tick(2);
    require(output.retry_text() == repeated("月", 27) + "水火",
            "retry owns only the unacknowledged suffix and later chunks");
    output.tick(50);
    require(sink.events == std::vector<std::string>{first, second}, "a batch never retries itself");
    sink.success = true;
    sink.delivered = 0;
    require(output.retry(), "Enter can retry the suffix");
    sink.done = true;
    output.tick(51);
    require(sink.events == std::vector<std::string>{first, second, repeated("月", 27)},
            "the completed first chunk and acknowledged second prefix are not replayed");
    output.tick(51.119);
    require(sink.events.size() == 3, "retry keeps its inter-chunk pace");
    output.tick(51.121);
    require(sink.events.back() == "水火", "the final suffix follows after the settling gap");
    sink.done = true;
    output.tick(52);
    require(!output.text_pending() && !output.retry_available(), "retry finishes with one text owner");
}
void synchronous_composition_refusal() {
    struct Refusal : KeySink {
        unsigned calls{};
        void send(int, int) override {}
        bool commit_text(const std::string&) override {
            ++calls;
            return false;
        }
    };
    {
        Refusal sink;
        OutputController output(sink);
        output.set_enabled(true);
        require(!output.commit_text(repeated("日", 32)) && !output.retry_available(),
                "a short synchronous refusal leaves ownership with its composer");
    }
    Refusal sink;
    OutputController output(sink);
    output.set_enabled(true);
    const auto long_text = repeated("日", 33);
    require(output.commit_text(long_text) && output.retry_text() == long_text,
            "a synchronously refused long batch transfers ownership to the single retry buffer");
    output.tick(0); // Observe the out-of-tick refusal before measuring its settling period.
    require(output.retry() && sink.calls == 1, "an immediate retry queues without bypassing pacing");
    output.tick(.119);
    require(sink.calls == 1, "an explicit retry still waits for the prior failed submission");
    output.tick(.121);
    require(sink.calls == 2 && output.retry_text() == long_text,
            "another refusal retains the entire batch once");
}
void long_composition_cancellation() {
    for (bool disable : {false, true}) {
        Transport sink;
        OutputController output(sink);
        output.set_enabled(true);
        require(output.commit_text(repeated("日", 33)), "a composition starts");
        if (disable) {
            output.set_enabled(false);
        } else {
            output.cancel();
        }
        output.set_enabled(true);
        character(output, 0, "new", .01);
        require(sink.events == std::vector<std::string>{repeated("日", 32)},
                "new input still honors the canceled chunk's temporary-map settling time");
        output.tick(.121);
        require(sink.events.size() == 1,
                "the first fresh tick anchors canceled submission timing rather than expiring it");
        output.tick(.242);
        require(sink.events == std::vector<std::string>{repeated("日", 32), "new"} &&
                    !output.retry_available(),
                "the new target receives no old suffix or old retry");
    }
}
void out_of_tick_composition_clock() {
    struct Immediate : KeySink {
        std::vector<std::string> events;
        void send(int code, int value) override {
            events.push_back(std::to_string(code) + ":" + std::to_string(value));
        }
        bool commit_text(const std::string& value) override {
            events.push_back(value);
            return true;
        }
    };
    const auto first = repeated("日", 32), text = first + "月";
    {
        Immediate sink;
        OutputController output(sink);
        output.set_enabled(true);
        output.tick(0);
        require(output.commit_text(text), "a synchronous batch starts outside tick");
        require(sink.events == std::vector<std::string>{first}, "only its first chunk starts");
        output.tick(10); // Its real submission may have been just before this new frame.
        require(sink.events == std::vector<std::string>{first},
                "a stale prior clock cannot expire a new synchronous chunk's settling gap");
        output.tick(10.119);
        require(sink.events.size() == 1, "the gap starts at a fresh clock observation");
        output.tick(10.121);
        require(sink.events == std::vector<std::string>{first, "月"},
                "the second chunk follows the anchored settling deadline");
    }
    {
        Immediate sink;
        OutputController output(sink);
        output.set_enabled(true);
        output.tick(0);
        require(output.commit_text("prior"), "ordinary text completes outside tick");
        require(output.admit(false, 10), "a later IME press supplies a newer admission timestamp");
        require(output.commit_text(text), "the composition queues after the ordinary text");
        require(sink.events == std::vector<std::string>{"prior"},
                "a later admission clock cannot backdate an earlier unanchored completion");
        output.tick(20);
        output.tick(20.119);
        require(sink.events == std::vector<std::string>{"prior"},
                "the first paced chunk waits after the fresh observation");
        output.tick(20.121);
        require(sink.events == std::vector<std::string>{"prior", first},
                "the long composition starts after that anchored gap");
    }
    {
        Immediate sink;
        OutputController output(sink);
        output.set_enabled(true);
        output.tick(0);
        require(output.commit_text(text), "a batch starts before a context cancellation");
        output.cancel();
        output.native_press(0, {KEY_C}, KEY_C, 10);
        output.release(0);
        require(sink.events == std::vector<std::string>{first},
                "cancellation keeps unanchored map timing without keeping old text");
        output.tick(20);
        output.tick(20.119);
        require(sink.events.size() == 1, "new target input waits for the old map's anchored gap");
        output.tick(20.121);
        require(sink.events == std::vector<std::string>{first, "46:1", "46:0"},
                "only fresh target input follows the gap, balanced and without the old suffix");
    }
}
void cancellation() {
    for (bool disable : {false, true}) {
        Transport sink;
        OutputController output(sink);
        output.set_enabled(true);
        character(output, 0, "a");
        character(output, 1, "b");
        output.native_press(2, {KEY_ENTER}, KEY_ENTER, 1);
        if (disable) {
            output.set_enabled(false);
        } else {
            output.cancel(); // Same operation used for request/route changes.
        }
        sink.done = true;
        output.tick(2);
        require(sink.events == std::vector<std::string>{"a"},
                "disable or target cancellation discards all undelivered work");
        output.set_enabled(true);
        character(output, 3, "new");
        require(sink.events == std::vector<std::string>{"a", "new"},
                "new target receives only newly accepted input");
    }
}
} // namespace
int main() {
    try {
        ordering();
        failure_and_retry();
        partial_retry();
        ambiguous_replacement();
        held_keys();
        release_and_capacity();
        modifier_snapshot();
        native_acknowledgement();
        delayed_admission_clock();
        composition_ownership();
        long_composition();
        long_composition_capacity();
        long_composition_after_pending_text();
        long_composition_retry();
        synchronous_composition_refusal();
        long_composition_cancellation();
        out_of_tick_composition_clock();
        cancellation();
        std::cout << "Ordered output, retries, cancellation, bounds and held-key cleanup passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
