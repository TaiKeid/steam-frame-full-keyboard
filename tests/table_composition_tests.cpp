#include "../src/table_engine.hpp"
#include "framekeyboard/app.hpp"

#include <cerrno>
#include <fstream>
#include <glib.h>
#include <iostream>
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
c.executemany('insert into phrases(tabkeys,phrase,freq,user_freq) values(?,?,1,0)', [('a','日'),('b','月')])
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
    std::size_t calls{}, refuse_call{};
    std::vector<std::string> commits;
    std::vector<std::pair<int, int>> keys;
    void send(int code, int value) override { keys.emplace_back(code, value); }
    bool text_available() override { return available; }
    bool commit_text(const std::string& text) override {
        ++calls;
        require(g_utf8_strlen(text.c_str(), -1) <= 32, "each transport submission fits its limit");
        if (calls == refuse_call) {
            return false;
        }
        commits.push_back(text);
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
} // namespace

// The only wrapped function substitutes a private table path. App, composer,
// engine, output admission and retry logic are the unmodified production code.
// The GNU C++ ABI symbol is shared by the host GCC and ARM64 Clang/libstdc++ builds.
std::optional<InputTable> fixture_table_for(const std::string& method) asm(
    "__wrap__ZN13framekeyboard9table_forERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
std::optional<InputTable> fixture_table_for(const std::string& method) {
    if (method == "chinese-quick" || method == "chinese-cangjie") {
        return InputTable{fixture_table, method == "chinese-quick" ? 2u : 5u};
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
