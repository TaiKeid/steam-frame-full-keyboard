#include "../src/table_engine.hpp"
#include "framekeyboard/app.hpp"
#include "framekeyboard/delivered_context.hpp"

#include <cerrno>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
using namespace framekeyboard;
static_assert(TableEngine::retained_limit <= OutputController::max_commit_codepoints,
              "an empty output queue must fit the entire retained composition");
namespace {
fs::path fixture_table;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-table-composition-XXXXXX";
        require(mkdtemp(name.data()), "create the private fixture directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
bool make_table(const fs::path& directory) {
    const auto script = directory / "table.py";
    fixture_table = directory / "table.db";
    std::ofstream(script) << R"PY(import sys
try:
    import sqlite3
except ImportError:
    sys.exit(77)
c = sqlite3.connect(sys.argv[1])
c.execute('create table phrases(id integer primary key, tabkeys text, phrase text, freq integer, user_freq integer)')
c.executemany('insert into phrases(tabkeys,phrase,freq,user_freq) values(?,?,1,0)', [('a','日'),('b','月'),('q','日'),('q','月'),('q','木'),('qqa','日'),('qqb','月'),('qqc','木')])
c.commit()
)PY";
    auto filename = script.string(), database = fixture_table.string();
    char python[] = "python3", isolated[] = "-I";
    char* arguments[]{python, isolated, filename.data(), database.data(), nullptr};
    pid_t child{};
    const int result = posix_spawnp(&child, python, nullptr, nullptr, arguments, environ);
    if (result == ENOENT) {
        return false;
    }
    require(result == 0, "start the private SQLite fixture creator");
    int status{};
    while (waitpid(child, &status, 0) < 0) {
        require(errno == EINTR, "wait for fixture creation");
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
        return false;
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "create the fixture table");
    return TableEngine::available(fixture_table);
}
struct Sink final : KeySink {
    bool available{};
    bool delayed{}, waiting{}, move_on_commit{};
    unsigned credits{};
    std::uint32_t window{1};
    std::size_t calls{}, refuse_call{};
    std::vector<std::string> commits;
    std::vector<std::pair<int, int>> keys;
    void send(int code, int value) override { keys.emplace_back(code, value); }
    bool text_available() override { return available; }
    std::uint32_t focused_window() const override { return window; }
    bool text_pending() const override { return waiting; }
    void cancel_text() override { waiting = false; }
    bool commit_text(const std::string& text) override {
        ++calls;
        require(g_utf8_strlen(text.c_str(), -1) <= 32, "each transport submission fits its limit");
        if (calls == refuse_call) {
            waiting = false;
            return false;
        }
        if (delayed && !credits) {
            waiting = true;
            return false;
        }
        if (delayed) {
            --credits;
        }
        waiting = false;
        commits.push_back(text);
        if (std::exchange(move_on_commit, false)) {
            ++window;
        }
        return true;
    }
    std::string delivered() const {
        std::string text;
        for (const auto& commit : commits) {
            text += commit;
        }
        return text;
    }
};
void delivery_ledger() {
    Sink sink;
    sink.available = sink.delayed = true;
    OutputController output(sink);
    output.set_enabled(true);
    DeliveredContext ledger;
    const auto submit = [&](const std::string& text) {
        const auto ticket = ledger.begin(DeliveredContext::snapshot(output));
        require(output.commit_text(text), "the private receiver admits text");
        ledger.admitted(ticket, DeliveredContext::snapshot(output), text);
    };
    submit("日");
    require(ledger.text().empty() && ledger.waiting(), "admission is not a delivery receipt");
    sink.credits = 1;
    output.tick(.1);
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text() == "日" && !ledger.waiting(), "a confirmed entry updates context once");
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text() == "日", "the same receipt is not appended twice");
    submit("月");
    ledger.clear(DeliveredContext::snapshot(output));
    sink.credits = 1;
    output.tick(.2);
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text().empty(), "an old acknowledgement cannot resurrect an invalidated epoch");
    submit("木");
    output.cancel();
    ledger.observe(DeliveredContext::snapshot(output));
    require(!ledger.active(), "cancellation clears pending and retained context");

    require(output.commit_text("外"), "a foreign backlog is admitted");
    submit("日");
    sink.credits = 1;
    output.tick(.3);
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text().empty(), "a foreign completion cannot count as the owned receipt");
    sink.credits = 1;
    output.tick(.4);
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text() == "日", "the owned receipt follows its entire foreign backlog");

    std::string long_text;
    for (int i = 0; i < 40; ++i) {
        long_text += "月";
    }
    output.tick(.6);
    sink.credits = 1;
    submit(long_text);
    require(output.pending_entries() == 1 && ledger.text() == "日",
            "a first chunk is insufficient to claim the whole committed tail");
    sink.refuse_call = sink.calls + 1;
    output.tick(.9);
    output.tick(1.1); // The first fresh tick anchors a synchronous chunk's settling interval.
    ledger.observe(DeliveredContext::snapshot(output));
    require(!ledger.active(), "a partial failure removes the uncertain context");
    sink.refuse_call = 0;
    sink.credits = 1;
    require(output.retry(), "only the private output queue retries its remainder");
    ledger.observe(DeliveredContext::snapshot(output));
    require(!ledger.active(), "a later retry does not revive the old epoch");
    output.tick(1.4);
    sink.credits = 2;
    submit(long_text);
    output.tick(1.5);
    output.tick(1.7);
    output.tick(1.9);
    ledger.observe(DeliveredContext::snapshot(output));
    require(ledger.text() == "月月", "a completely acknowledged chunk batch contributes its final tail");
    output.tick(2.2);
    sink.credits = 1;
    submit("日𨋢月");
    require(ledger.text() == "𨋢月", "context retains Unicode characters rather than truncating bytes");
    sink.credits = 0;
    submit("日");
    require(output.tap({KEY_BACKSPACE}), "a foreign private key can queue behind owned text");
    ledger.observe(DeliveredContext::snapshot(output));
    require(!ledger.active(), "unexpected queued native editing invalidates the entire context run");
    sink.credits = 1;
    output.tick(2.3);
    ledger.observe(DeliveredContext::snapshot(output));
    require(!ledger.active(), "a foreign completion cannot repopulate an invalidated run");
}
struct Fixture {
    TemporaryDirectory directory;
    Sink sink;
    std::unique_ptr<App> app;
    double now{10};
    explicit Fixture(const char* language) {
        std::ofstream(directory.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":"en-us-full","language":")" << language
            << R"(","theme":"graphite"},"dictation":"off"})";
        Options options;
        options.mode = Mode::Vr; // App only; no VR runtime or actual backend is constructed.
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.config_dir = directory.path;
        options.models_dir = directory.path / "models";
        app = std::make_unique<App>(options, sink);
    }
    void tick(double seconds) { app->tick(now += seconds); }
    void press(const char* action) {
        const auto view = app->view();
        for (const auto& key : view.layout->keys) {
            if (key.action != action) {
                continue;
            }
            const auto r = panel_rect(view, key.bounds);
            const double x = r.x + r.width / 2, y = r.y + r.height / 2;
            require(app->down(0, x, y, now), "the local composition key is accepted");
            app->up(0, x, y);
            tick(.01);
            return;
        }
        throw std::runtime_error("fixture key missing");
    }
    std::string accumulate(std::size_t characters) {
        std::string expected;
        for (std::size_t i = 0; i < characters; ++i) {
            press(i % 2 ? "KeyB" : "KeyA");
            press("Digit1");
            expected += i % 2 ? "月" : "日";
        }
        require(app->view().preedit == expected, "refused choices remain in local composition");
        require(sink.commits.empty(), "unavailable Unicode delivery sends nothing");
        return expected;
    }
    void recover() {
        sink.available = true;
        tick(.01);
    }
};
void contextual_tables(const fs::path& directory) {
    const auto counts = directory / "context-counts.bin";
    {
        std::ofstream file(counts, std::ios::binary);
        for (const std::uint32_t number : {0u, 0x65e5u, 0x6728u, 100000u}) {
            for (int shift = 0; shift < 32; shift += 8) {
                file.put(static_cast<char>(number >> shift));
            }
        }
    }
    setenv("FRAMEKEYBOARD_TABLE_CONTEXT_COUNTS", counts.c_str(), 1);
    struct Reset {
        ~Reset() { unsetenv("FRAMEKEYBOARD_TABLE_CONTEXT_COUNTS"); }
    } reset;
    {
        const auto table = TableEngine::load(fixture_table);
        TableEngine quick(table, 2, true);
        quick.type('q');
        const auto before = quick.candidates();
        quick.set_context("日");
        require(quick.candidates() == before, "a receipt never moves an already presented menu");
        quick.backspace();
        quick.type('q');
        require(quick.candidates() == std::vector<std::string>{"日", "木", "月"},
                "Quick keeps its first choice and every alternative while ranking later choices");
        TableEngine cangjie(table, 5);
        cangjie.set_context("日");
        cangjie.type('q');
        require(cangjie.candidates() == before, "common Cangjie exact matches remain protected");
        cangjie.type('q');
        require(cangjie.candidates().front() == "木", "context ranks Cangjie prefix completions");
        cangjie.cancel();
        cangjie.type('q');
        cangjie.type('q');
        require(cangjie.candidates().front() == "木",
                "a local composer reset retains delivered context");
        cangjie.clear_context();
        require(cangjie.candidates().front() == "木", "invalidation keeps the presented choices fixed");
        cangjie.backspace();
        cangjie.type('q');
        require(cangjie.candidates().front() == "日", "the next reading mutation uses cleared context");
    }
    const auto first = [](const App& app) {
        const auto view = app.view();
        require(view.candidates && !view.candidates->chips.empty(), "the fixture candidate is visible");
        return view.candidates->chips.front().text;
    };
    {
        Fixture f("zh-tw-cangjie");
        f.recover();
        f.press("KeyA");
        f.press("Digit1");
        require(f.app->holds_swipe_state(),
                "Chinese context activates the existing off-key caret watch");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 木", "a successful commit feeds the next code's context");
        f.app->cancel_pointer(0, true);
        require(first(*f.app) == "1 木" && !f.app->holds_swipe_state(),
                "an observed edit clears context without changing any presented choice");
        f.press("Backspace");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "normal reading mutation applies the cleared context");
    }
    {
        Fixture f("zh-tw-cangjie");
        f.sink.delayed = true;
        f.recover();
        f.press("KeyA");
        f.press("Digit1");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "unacknowledged admission is not previous text");
        const auto chip = f.app->view().candidates->chips.front().bounds;
        f.app->down(9, chip.x + chip.width / 2, chip.y + chip.height / 2, f.now);
        f.sink.credits = 1;
        f.tick(.1);
        require(first(*f.app) == "1 日", "a late receipt cannot change a pressed candidate");
        f.app->up(9, chip.x + chip.width / 2, chip.y + chip.height / 2);
        f.sink.credits = 1;
        f.tick(.1);
        require(f.sink.delivered() == "日日", "release delivers the originally pressed candidate");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 木", "the next normal lookup uses acknowledged context");
        f.sink.window = 2;
        f.app->down(8, -100, -100, f.now);
        require(first(*f.app) == "1 木", "target invalidation does not rewrite the presented list");
        f.press("Backspace");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "a new lookup cannot borrow the old cached target context");
    }
    {
        Fixture f("zh-tw-cangjie");
        f.sink.window = 0;
        f.recover();
        f.press("KeyA");
        f.press("Digit1");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "unknown real direct targets never retain table context");
    }
    {
        Fixture f("zh-tw-quick");
        f.recover();
        f.press("KeyQ");
        f.press("KeyQ");
        f.press("KeyQ"); // Auto-front sends 日 and starts the next code.
        require(f.sink.delivered() == "日", "Quick auto-front reaches the private receiver once");
        require(f.app->view().candidates->chips.at(1).text == "2 月",
                "an immediate receipt does not reorder the newly presented first-key menu");
        f.press("KeyQ");
        require(first(*f.app) == "1 日" && f.app->view().candidates->chips.at(1).text == "2 木",
                "Quick's next regular lookup uses its acknowledged auto-front context");
        f.app->apply({"en-us-full", "zh-tw-cangjie", "graphite"});
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "a profile/language change does not borrow prior context");
    }
    {
        Fixture f("zh-tw-quick");
        f.recover();
        f.press("KeyQ");
        f.press("KeyQ");
        f.press("Digit2");
        require(f.sink.delivered() == "月", "the user can choose a nonfirst table character");
        f.press("KeyQ");
        f.press("KeyQ");
        require(f.app->view().candidates->chips.at(1).text == "2 月",
                "context comes from the actual selected character, not an oracle/native first choice");
    }
    {
        Fixture f("zh-tw-cangjie");
        f.sink.delayed = true;
        f.recover();
        f.press("KeyA");
        f.press("Digit1");
        f.app->release_input(false); // Kept delivery must not revive cleared input-session context.
        f.sink.credits = 1;
        f.tick(.1);
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 日", "an old kept delivery cannot reseed an invalidated epoch");
    }
    for (const auto* purpose : {"password", "pin", "terminal"}) {
        Fixture f("zh-tw-cangjie");
        f.recover();
        App::HostHints hints;
        hints.purpose = purpose;
        f.app->set_context(std::string("private-fixture"), hints);
        f.press("KeyA");
        f.press("Digit1");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 日" && !f.app->holds_swipe_state(),
                "sensitive/terminal fields do not retain contextual table text");
    }
    {
        Fixture f("zh-tw-cangjie");
        f.recover();
        f.press("KeyA");
        f.press("Digit1");
        f.press("KeyQ");
        f.press("KeyQ");
        require(first(*f.app) == "1 木", "the ranked menu was actually presented before target change");
        const auto old = f.app->view().candidates->chips.front().bounds;
        f.sink.window = 2;
        f.app->down(9, old.x + old.width / 2, old.y + old.height / 2, f.now);
        f.app->up(9, old.x + old.width / 2, old.y + old.height / 2);
        require(f.sink.delivered() == "日木",
                "invalidation on down preserves the exact presented candidate identity");
    }
    for (const auto* action : {"Space", "Digit2"}) {
        for (const bool cancel : {false, true}) {
            Fixture f("zh-tw-cangjie");
            f.recover();
            f.press("KeyA");
            f.press("Digit1");
            f.press("KeyQ");
            f.press("KeyQ");
            const auto label =
                f.app->view().candidates->chips.at(std::string(action) == "Space" ? 0 : 1).text;
            const auto expected = label.substr(label.find(' ') + 1);
            if (cancel) {
                f.app->cancel(false);
            } else {
                f.sink.window = 2;
            }
            f.press(action);
            require(f.sink.delivered() == "日" + expected,
                    "keyboard acceptance after uncertainty delivers the presented label, never a "
                    "retargeted one");
        }
    }
    {
        Fixture f("zh-tw-cangjie");
        f.recover();
        f.press("KeyA");
        const auto view = f.app->view();
        const auto key = std::find_if(view.layout->keys.begin(), view.layout->keys.end(),
                                      [](const Key& value) { return value.action == "Digit1"; });
        require(key != view.layout->keys.end(), "the acknowledgement-scope fixture has a choice key");
        const auto r = panel_rect(view, key->bounds);
        f.sink.move_on_commit = true;
        f.app->down(7, r.x + r.width / 2, r.y + r.height / 2, f.now);
        require(!f.app->holds_swipe_state(),
                "an acknowledgement that changes target cannot temporarily seed the new scope");
        f.app->up(7, r.x + r.width / 2, r.y + r.height / 2);
    }
}
void recovery(const char* language, std::size_t count) {
    Fixture f(language);
    const auto expected = f.accumulate(count);
    f.recover();
    f.press("Enter");
    require(f.app->view().preedit.empty(), "accepted output takes ownership of the whole composition");
    require(f.sink.commits.size() == 1, "only the first chunk is sent immediately");
    if (count > 32) {
        require(f.sink.delivered() != expected, "the following chunk waits for a later paced tick");
        f.tick(.05);
        require(f.sink.commits.size() == 1, "chunks do not burst in consecutive short ticks");
        f.tick(.2);
    }
    require(f.sink.delivered() == expected, "recovery sends every character exactly once");
    require(f.sink.commits.size() == (count + 31) / 32, "text is split at Unicode boundaries");
    require(f.sink.keys.empty(), "the commit Enter does not submit the target field");
}
void backspace(const char* language) {
    Fixture f(language);
    auto expected = f.accumulate(33);
    f.press("Backspace");
    expected.resize(expected.size() - 3); // Each fixture character is one three-byte code point.
    require(f.app->view().preedit == expected, "Backspace removes one retained Unicode character");
    f.recover();
    f.press("Enter");
    require(f.sink.delivered() == expected, "the other retained characters remain sendable");
}
void retry_suffix(const char* language, bool cancel) {
    Fixture f(language);
    const auto expected = f.accumulate(33);
    f.sink.refuse_call = 2;
    f.recover();
    f.press("Enter");
    f.tick(.2);
    require(f.sink.calls == 2 && f.sink.delivered() == expected.substr(0, 32 * 3),
            "the second chunk can fail after the first was accepted");
    require(f.app->view().preedit.empty(), "output alone owns the failed suffix");
    f.tick(1);
    require(f.sink.calls == 2, "recovery never retries text automatically");
    if (cancel) {
        f.app->cancel();
        f.press("Enter");
        f.tick(1);
        require(f.sink.calls == 2, "cancellation drops the old retry before another Enter");
    } else {
        f.press("Enter");
        f.tick(.2);
        require(f.sink.delivered() == expected && f.sink.calls == 3,
                "Enter retries only the unsent suffix without duplicating the accepted prefix");
        require(f.sink.keys.empty(), "retry Enter does not reach the target");
    }
}
void hide_queued_suffix(const char* language) {
    Fixture f(language);
    const auto expected = f.accumulate(33);
    f.recover();
    f.press("Enter");
    require(f.sink.delivered() == expected.substr(0, 32 * 3), "the first chunk arrived");
    f.app->set_interaction_active(false);
    f.app->set_interaction_active(true);
    f.tick(1);
    require(f.sink.calls == 1, "hiding drops a queued suffix before it can reach a later target");
}
void text_loss_drops_queued_suffix(const char* language) {
    Fixture f(language);
    const auto expected = f.accumulate(33);
    f.recover();
    f.press("Enter");
    require(f.sink.delivered() == expected.substr(0, 32 * 3), "the first chunk arrived");
    f.sink.available = false;
    f.tick(.01);
    f.recover();
    f.press("Enter");
    f.tick(1);
    require(f.sink.calls == 1 && f.sink.delivered() == expected.substr(0, 32 * 3),
            "text transport loss cancels the owned suffix rather than replaying it after recovery");
}
// A table of every kind of row, in id order: ranked characters, an unranked
// one, Extension A and B, a compatibility ideograph, a symbol, a ranked
// character on a lower frequency tier (a second code for it), completions
// (one character with two longer codes), and a code with more exact matches
// than a page.
fs::path make_ordering_table(const fs::path& directory) {
    const auto script = directory / "ordering.py";
    const auto table = directory / "ordering.db";
    std::ofstream(script) << R"PY(import sys, sqlite3
c = sqlite3.connect(sys.argv[1])
c.execute('create table phrases(id integer primary key, tabkeys text, phrase text, freq integer, user_freq integer)')
rows = [('hi', '\u34d8', 1000), ('hi', '\u4e1f', 1000), ('hi', '我', 1000), ('hi', '的', 1000),
        ('hi', '\u2192', 1000), ('hi', '\uf900', 1000), ('hi', '\U00020000', 1000), ('hi', '是', 900),
        ('hij', '\u3400', 1000), ('hij', '人', 1000)]
rows += [('zz', chr(0x4e00 + i), 1000) for i in range(60)] + [('zza', '大', 1000), ('hik', '人', 1000)]
c.executemany('insert into phrases(tabkeys,phrase,freq,user_freq) values(?,?,?,0)', rows)
c.commit()
)PY";
    const auto filename = script.string(), database = table.string();
    char python[] = "python3", isolated[] = "-I";
    std::string file = filename, out = database;
    char* arguments[]{python, isolated, file.data(), out.data(), nullptr};
    pid_t child{};
    require(posix_spawnp(&child, python, nullptr, nullptr, arguments, environ) == 0,
            "start the ordering fixture");
    int status{};
    while (waitpid(child, &status, 0) < 0) {
        require(errno == EINTR, "wait for the ordering fixture");
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "create the ordering fixture");
    return table;
}
std::string joined(const std::vector<std::string>& texts) {
    std::string result;
    for (const auto& text : texts) {
        result += text;
    }
    return result;
}
std::vector<std::string> offered(const fs::path& table, const char* keys) {
    TableEngine engine(TableEngine::load(table), 5);
    for (const char* key = keys; *key; ++key) {
        engine.type(*key);
    }
    return engine.candidates();
}
// Cangjie and Quick offer a code's characters by the rank list, within the
// table's own frequency tiers; without a usable list, in the table's order.
void ranked_order(const fs::path& directory) {
    const auto table = make_ordering_table(directory);
    const auto ranks = directory / "ranks.txt", malformed = directory / "malformed.txt";
    std::ofstream(ranks) << "是\n的\n我\n人\n";
    std::ofstream(malformed) << "的我\n";
    set_character_ranks(ranks);
    require(joined(offered(table, "hi")) == "的我是人\u4e1f\u34d8\u3400\U00020000\uf900\u2192",
            "ranked exact matches by tier and rank, then ranked completions, each character once, then "
            "the rare ones by block, exact matches before completions, compatibility ideographs and "
            "symbols");
    // Characters after the list's empty line are in its repertoire but unranked:
    // rare, so they follow the completions.
    const auto bounded = directory / "bounded.txt";
    std::ofstream(bounded) << "是\n的\n人\n\n我\n";
    set_character_ranks(bounded);
    require(joined(offered(table, "hi")) == "的是人我\u4e1f\u34d8\u3400\U00020000\uf900\u2192",
            "an unranked character of the repertoire comes after common completions, before other rare "
            "ones");
    set_character_ranks(ranks);
    const auto many = offered(table, "zz");
    require(many.size() == 60, "every exact match is offered, and no completion past a full page");
    require(TableEngine::load(table) == TableEngine::load(table), "composers of a table share it");
    for (const auto& unusable : {malformed, directory / "missing.txt"}) {
        set_character_ranks(unusable);
        require(joined(offered(table, "hi")) == "\u34d8\u4e1f我的\u2192\uf900\U00020000是\u3400人",
                "without a usable rank list, the table's own order");
    }
    set_character_ranks({});
}
} // namespace

// The only wrapped function substitutes a private table path. App, composer,
// engine, output admission and retry logic are the unmodified production code.
// The GNU C++ ABI symbol is shared by the host GCC and ARM64 Clang/libstdc++ builds.
std::optional<InputTable> fixture_table_for(const std::string& method) asm(
    "__wrap__ZN13framekeyboard9table_forERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
std::optional<InputTable> fixture_table_for(const std::string& method) {
    if (method == "chinese-quick" || method == "chinese-cangjie") {
        return InputTable{fixture_table, method == "chinese-quick" ? 2u : 5u, method == "chinese-quick"};
    }
    return {};
}

int main() {
    try {
        TemporaryDirectory database;
        if (!make_table(database.path)) {
            std::cout << "Table composition tests skipped: SQLite or Python unavailable.\n";
            return 77;
        }
        ranked_order(database.path);
        delivery_ledger();
        contextual_tables(database.path);
        for (const auto* language : {"zh-tw-quick", "zh-tw-cangjie"}) {
            recovery(language, 32);
            recovery(language, 33);
            recovery(language, 64);
            backspace(language);
            retry_suffix(language, false);
            retry_suffix(language, true);
            hide_queued_suffix(language);
            text_loss_drops_queued_suffix(language);
        }
    } catch (const std::exception& error) {
        std::cerr << "table composition test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "table composition tests passed\n";
}
