#include "framekeyboard/dictation.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Ticks until an event of `kind` arrives, returning it; other events are kept in `seen`.
Dictation::Event wait_for(Dictation& dictation, Dictation::Event::Kind kind,
                          std::vector<Dictation::Event>* seen = nullptr, double seconds = 10) {
    const double deadline = now() + seconds;
    while (now() < deadline) {
        dictation.tick(now());
        while (auto event = dictation.next()) {
            if (event->kind == kind) {
                return *event;
            }
            if (seen) {
                seen->push_back(*event);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("expected dictation event did not arrive");
}

// A stand-in helper that scripts the protocol from the helper's side and
// records what the keyboard sent. It never touches audio or the network.
const char* fake_helper = R"PY(
import json, socket, sys, time, traceback
peer = socket.socket(fileno=3)
args = sys.argv[1:]
log = open(args[args.index("--log") + 1], "w")
sys.excepthook = lambda *e: log.write("".join(traceback.format_exception(*e)))
scenario = args[args.index("--scenario") + 1]
if scenario == "exit":
    sys.exit(0)
def send(**m):
    peer.send(json.dumps(m).encode())
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    log.write(json.dumps(message) + "\n"); log.flush()
    if message["op"] == "configure":
        send(op="level", value=300)
        send(op="phrase", id=1)
        send(op="ready", id=1)
    elif message["op"] == "prompt":
        send(op="text", id=message["id"], text="got:" + message["text"])
        peer.send(b'{"op":"text","id":2,"text":"\xff\xfe"}')
        send(op="error", reason="rm -rf /")
        send(op="dropped", id=3, reason="too-long")
        send(op="test", ok=False, reason="unreachable")
        send(op="ready", id=0)
        send(op="nonsense")
    elif message["op"] == "test":
        peer.send(json.dumps({"op": "text", "id": 9, "text": "x" * 20000}).encode())
)PY";

void scripted_protocol(const fs::path& directory) {
    const auto script = directory / "fake_helper.py";
    std::ofstream(script) << fake_helper;
    const auto log = directory / "received.log";
    Dictation dictation(directory / "dictation-key", script,
                        {"--scenario", "main", "--log", log.string()});
    require(!dictation.running(), "the helper starts only when needed");
    require(!dictation.configure({}), "a target needs a server or a URL");
    require(!dictation.configure({"host", "https://x/v1", "", ""}), "not both a server and a URL");
    require(dictation.configure({"127.0.0.1:8031", "", "", "en"}) && dictation.running(),
            "configure starts the helper");

    std::vector<Dictation::Event> seen;
    const auto ready = wait_for(dictation, Dictation::Event::Kind::Ready, &seen);
    require(ready.id == 1, "ready names its phrase");
    require(seen.size() == 2 && seen[0].kind == Dictation::Event::Kind::Level && seen[0].level == 255 &&
                seen[1].kind == Dictation::Event::Kind::Phrase && seen[1].id == 1,
            "levels are clamped and phrases announced");

    std::string context;
    for (int i = 0; i < 300; ++i) {
        context += i < 100 ? "a" : "é";
    }
    require(!dictation.prompt(1, "bad \xff"), "invalid UTF-8 context is refused");
    require(dictation.prompt(1, context), "context is sent");
    const auto text = wait_for(dictation, Dictation::Event::Kind::Text);
    std::string expected = "got:";
    for (int i = 0; i < 200; ++i) {
        expected += "é";
    }
    require(text.id == 1 && text.text == expected, "only the last 200 code points of context are sent");

    seen.clear();
    const auto test = wait_for(dictation, Dictation::Event::Kind::Test, &seen);
    require(!test.ok && test.reason == "unreachable", "test results keep their fixed code");
    require(seen.size() == 3, "malformed packets are skipped");
    require(seen[0].kind == Dictation::Event::Kind::Dropped && seen[0].id == 2 &&
                seen[0].reason == "invalid",
            "invalid UTF-8 text never reaches App");
    require(seen[1].kind == Dictation::Event::Kind::Error && seen[1].reason == "bad-reply",
            "unknown reasons are replaced by a fixed code");
    require(seen[2].kind == Dictation::Event::Kind::Dropped && seen[2].reason == "too-long",
            "dropped phrases keep their reason");

    // Over a second of ticks sends keep-alives at least every 0.5 s.
    const double until = now() + 1.1;
    while (now() < until) {
        dictation.tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(dictation.test(), "test is sent");
    const auto lost = wait_for(dictation, Dictation::Event::Kind::Error);
    require(lost.reason == "helper-exited" && !dictation.running(),
            "a packet over the protocol's bound ends the helper");
    require(dictation.wait_stopped(5), "the helper exits once its socket closes");

    std::ifstream input(log);
    std::string all((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::size_t alive = 0;
    for (auto at = all.find("\"alive\""); at != std::string::npos; at = all.find("\"alive\"", at + 1)) {
        ++alive;
    }
    require(alive >= 3, "keep-alives are sent while the keyboard runs");
    require(all.find("\"language\": \"en\"") != std::string::npos &&
                all.find("\"url\"") == std::string::npos,
            "configure sends only the fields that are set");
}

void exit_is_reported(const fs::path& directory) {
    const auto script = directory / "fake_helper.py";
    Dictation dictation(directory / "dictation-key", script,
                        {"--scenario", "exit", "--log", (directory / "exit.log").string()});
    require(dictation.configure({"host", "", "", ""}), "configure starts the helper");
    require(wait_for(dictation, Dictation::Event::Kind::Error).reason == "helper-exited",
            "a helper that exits is reported");
    require(!dictation.running(), "a lost helper is no longer running");
}

void deliberate_stop_is_quiet(const fs::path& directory) {
    const auto script = directory / "fake_helper.py";
    Dictation dictation(directory / "dictation-key", script,
                        {"--scenario", "main", "--log", (directory / "stop.log").string()});
    require(dictation.configure({"host", "", "", ""}), "configure starts the helper");
    dictation.stop();
    require(!dictation.running() && dictation.wait_stopped(5), "stop ends the helper");
    dictation.tick(now());
    while (auto event = dictation.next()) {
        require(event->kind != Dictation::Event::Kind::Error, "a deliberate stop is not an error");
    }
}

void real_helper(const fs::path& directory, const fs::path& helper) {
    // A closed local port: the real helper must report it as unreachable.
    const int probe = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    require(bind(probe, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
                getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "pick a free local port");
    close(probe);
    const auto port = ntohs(address.sin_port);
    Dictation dictation(directory / "dictation-key", helper);
    require(dictation.configure({"127.0.0.1:" + std::to_string(port), "", "", "en"}),
            "start the real helper");
    require(dictation.test(), "send a test");
    const auto result = wait_for(dictation, Dictation::Event::Kind::Test);
    require(!result.ok && result.reason == "unreachable", "the real helper speaks the same protocol");
    dictation.stop();
    require(dictation.wait_stopped(5), "the real helper exits when its socket closes");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: dictation-tests <dictation_helper.py>");
        char pattern[] = "/tmp/framekeyboard-dictation-XXXXXX";
        require(mkdtemp(pattern), "temporary directory");
        const fs::path directory(pattern);
        scripted_protocol(directory);
        exit_is_reported(directory);
        deliberate_stop_is_quiet(directory);
        real_helper(directory, argv[1]);
        if (!std::getenv("FK_KEEP")) {
            fs::remove_all(directory);
        }
        std::cout << "Dictation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
