// The shell connection and the text sink (docs/integration.md), with a fake
// shell on the other end of a socket pair. No VR, input device or target app.
#include "framekeyboard/instance.hpp"
#include "framekeyboard/output.hpp"
#include "framekeyboard/shell_host.hpp"
#include "framekeyboard/shell_protocol.hpp"
#include "framekeyboard/shell_sink.hpp"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

// The shell's end: writes lines, reads what the keyboard sent.
struct FakeShell {
    int fd{-1};
    std::string buffer;
    void send(const std::string& line) {
        const auto data = line + "\n";
        require(::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size()),
                "the fake shell writes");
    }
    std::vector<std::string> lines() {
        char chunk[65536];
        for (;;) {
            const auto count = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (count <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(count));
        }
        std::vector<std::string> result;
        for (auto end = buffer.find('\n'); end != std::string::npos; end = buffer.find('\n')) {
            result.push_back(buffer.substr(0, end));
            buffer.erase(0, end + 1);
        }
        return result;
    }
    bool saw(const std::vector<std::string>& lines, const std::string& part) {
        for (const auto& line : lines) {
            if (line.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

struct Pair {
    int keyboard{-1};
    FakeShell shell;
    Pair() {
        int fds[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socket pair");
        keyboard = fds[0];
        shell.fd = fds[1];
    }
    ~Pair() { close(shell.fd); }
};

void descriptor_tests() {
    Pair pair;
    require(adopt_control_descriptor(pair.keyboard), "a connected stream pair of this user is accepted");
    require(fcntl(pair.keyboard, F_GETFD) & FD_CLOEXEC, "the descriptor becomes close-on-exec");
    int datagrams[2];
    require(socketpair(AF_UNIX, SOCK_DGRAM, 0, datagrams) == 0, "datagram pair");
    require(!adopt_control_descriptor(datagrams[0]), "a datagram socket is refused");
    require(!adopt_control_descriptor(1), "the standard streams are refused");
    const int file = open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(!adopt_control_descriptor(file), "a file is refused");
    close(file);
    close(datagrams[0]);
    close(datagrams[1]);
    close(pair.keyboard);
}

void session_tests() {
    Pair pair;
    adopt_control_descriptor(pair.keyboard);
    ShellConnection connection(pair.keyboard, "0.11.0", 3,
                               [] { return std::vector<std::string>{"sink"}; });
    connection.ready(10);
    auto lines = pair.shell.lines();
    require(lines.size() == 1 &&
                lines[0] == R"({"event":"ready","protocol":[1],"version":"0.11.0","session":3})",
            "ready names the protocols, version and session");
    pair.shell.send(R"({"id":1,"op":"state"})");
    require(connection.poll(10.5).empty(), "nothing passes before hello");
    connection.frame_completed(10.5);
    lines = pair.shell.lines();
    require(pair.shell.saw(lines, R"("id":1,"ok":false,"error":"no_hello")"),
            "requests before hello are refused");
    pair.shell.send(R"({"id":2,"op":"hello","protocol":2})");
    connection.poll(11);
    connection.frame_completed(11);
    require(pair.shell.saw(pair.shell.lines(), R"("error":"incompatible")"),
            "an unknown protocol is refused");
    pair.shell.send(R"({"id":3,"op":"hello","protocol":1,"client":"Test Shell","lease_ms":1000})");
    connection.poll(11);
    connection.frame_completed(11);
    lines = pair.shell.lines();
    require(
        pair.shell.saw(lines, R"("id":3,"ok":true,"protocol":1,"version":"0.11.0","features":["sink"])"),
        "hello answers with the features available now");
    require(connection.client() == "Test Shell", "the client name is kept for the sink notice");
    pair.shell.send(R"({"id":4,"op":"ping"})");
    connection.poll(11.2);
    require(pair.shell.lines().empty(), "a ping waits for the loop iteration to complete");
    connection.frame_completed(11.2);
    require(pair.shell.saw(pair.shell.lines(), R"({"id":4,"ok":true})"), "then it is answered");
    pair.shell.send(R"({"id":5,"op":"show","view":"keyboard"})");
    pair.shell.send(R"({"op":"pointer","hand":"left","phase":"move","u":0.5,"v":0.5})");
    const auto requests = connection.poll(11.3);
    require(requests.size() == 2 && requests[0].id == 5 && requests[0].op == "show" && !requests[1].id &&
                requests[1].op == "pointer",
            "other requests and notifications pass in order");
    // Subscriptions filter events; ready, update, regions and sink events always go.
    connection.event("visibility", json_ptr(json_object_new_object()));
    connection.event("update", json_ptr(json_object_new_object()));
    connection.frame_completed(11.3);
    lines = pair.shell.lines();
    require(!pair.shell.saw(lines, "visibility") && pair.shell.saw(lines, R"("event":"update")"),
            "an unsubscribed event is not sent");
    pair.shell.send(R"({"id":6,"op":"subscribe","events":["visibility","geometry"]})");
    connection.poll(11.4);
    connection.event("visibility", json_ptr(json_object_new_object()));
    connection.frame_completed(11.4);
    require(pair.shell.saw(pair.shell.lines(), R"("event":"visibility")"), "a subscribed event is sent");
    // Telemetry keeps only the latest value.
    for (int i = 0; i < 5; ++i) {
        auto fields = json_ptr(json_object_new_object());
        json_object_object_add(fields.get(), "width", json_object_new_int(i));
        connection.telemetry("geometry", std::move(fields));
    }
    connection.frame_completed(11.5);
    lines = pair.shell.lines();
    require(lines.size() == 1 && lines[0] == R"({"event":"geometry","width":4})",
            "telemetry is coalesced");
    // The lease: lapses once, renews on any message, then ends after the grace.
    require(!connection.take_lease_lapsed(12.3), "within the lease");
    require(connection.take_lease_lapsed(12.6) && !connection.take_lease_lapsed(12.7),
            "a lapse is reported once");
    require(connection.status(20) == ShellConnection::Status::Open, "a lapsed shell gets the grace");
    pair.shell.send(R"({"op":"ack","context":"c","seq":1})");
    connection.poll(20);
    require(connection.take_lease_renewed() && !connection.lease_lapsed(),
            "any message renews the lease");
    require(connection.take_lease_lapsed(21.5), "it can lapse again");
    require(connection.status(21.5 + 1 + ShellConnection::lease_grace) ==
                ShellConnection::Status::LeaseGone,
            "past the grace the keyboard exits");
}

// The handover: pings answered on demand, and the descriptor released only
// once every queued byte is written.
void drain_tests() {
    Pair pair;
    adopt_control_descriptor(pair.keyboard);
    ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
    connection.ready(0);
    pair.shell.send(R"({"id":1,"op":"hello","protocol":1})");
    pair.shell.send(R"({"id":2,"op":"ping"})");
    connection.poll(1);
    connection.answer_pings();
    connection.flush();
    require(pair.shell.saw(pair.shell.lines(), R"({"id":2,"ok":true})"),
            "a ping before quiesced is answered");
    // A large reply the shell is not reading yet: the socket fills.
    auto fields = json_ptr(json_object_new_object());
    json_object_object_add(fields.get(), "blob",
                           json_object_new_string(std::string(600000, 'x').c_str()));
    connection.respond(3, std::move(fields));
    require(connection.release(.2) < 0 && connection.fd() >= 0,
            "an undrained session is not handed over");
    std::string received;
    std::thread reader([&] {
        char chunk[65536];
        for (;;) {
            const auto count = recv(pair.shell.fd, chunk, sizeof(chunk), 0);
            if (count <= 0) {
                break;
            }
            received.append(chunk, static_cast<std::size_t>(count));
            if (received.ends_with("\n") && received.find(R"("restarting")") != std::string::npos) {
                break;
            }
        }
    });
    auto restarting = json_ptr(json_object_new_object());
    json_object_object_add(restarting.get(), "state", json_object_new_string("restarting"));
    connection.event("update", std::move(restarting));
    const int handed = connection.release(5);
    reader.join();
    require(handed >= 0 && connection.fd() < 0, "once drained, the descriptor passes");
    require(received.ends_with(R"({"event":"update","state":"restarting"})"
                               "\n") &&
                received.find(R"({"id":3,"ok":true,"blob":")") != std::string::npos,
            "every line arrives whole, in order, restarting last");
    close(handed);
}

void limits_tests() {
    {
        Pair pair;
        adopt_control_descriptor(pair.keyboard);
        ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
        connection.ready(0);
        pair.shell.lines();
        require(connection.status(ShellConnection::hello_deadline + .5) ==
                    ShellConnection::Status::NoHello,
                "no hello within the deadline ends the keyboard");
    }
    {
        Pair pair;
        adopt_control_descriptor(pair.keyboard);
        ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
        connection.ready(0);
        pair.shell.send("{not json");
        connection.poll(1);
        require(connection.status(1) == ShellConnection::Status::Violated,
                "malformed JSON ends the session");
        require(pair.shell.saw(pair.shell.lines(), R"("error":"bad_request")"),
                "with one bad_request error");
    }
    {
        Pair pair;
        adopt_control_descriptor(pair.keyboard);
        ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
        connection.ready(0);
        pair.shell.send(R"({"id":1,"op":"hello","protocol":1})");
        connection.poll(1);
        pair.shell.send(R"({"op":"state"})");
        connection.poll(1);
        require(connection.status(1) == ShellConnection::Status::Violated,
                "a request without an id is refused");
    }
    {
        Pair pair;
        adopt_control_descriptor(pair.keyboard);
        ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
        connection.ready(0);
        std::string huge = R"({"id":1,"op":"hello","client":")";
        huge.append(ShellConnection::max_message, 'x');
        huge += R"("})";
        // Written in pieces, as the socket takes them.
        for (std::size_t at = 0; at < huge.size();) {
            const auto count =
                ::send(pair.shell.fd, huge.data() + at, std::min<std::size_t>(16384, huge.size() - at),
                       MSG_NOSIGNAL | MSG_DONTWAIT);
            if (count > 0) {
                at += static_cast<std::size_t>(count);
            }
            connection.poll(1);
            if (connection.status(1) == ShellConnection::Status::Violated) {
                break;
            }
        }
        require(connection.status(1) == ShellConnection::Status::Violated,
                "an oversize message ends the session");
    }
    {
        Pair pair;
        adopt_control_descriptor(pair.keyboard);
        ShellConnection connection(pair.keyboard, "0.11.0", 1, {});
        connection.ready(0);
        close(pair.shell.fd);
        pair.shell.fd = open("/dev/null", O_RDONLY);
        connection.poll(1);
        require(connection.status(1) == ShellConnection::Status::Closed,
                "a closed connection ends the session");
    }
}

// A backend that records what reaches it while the sink is off.
struct Backend : KeySink {
    std::vector<std::string> sent;
    bool paused{}, reset{};
    bool pump() override { return !paused; }
    bool take_input_reset() override { return std::exchange(reset, false); }
    // A French AZERTY target: Ctrl+A is the physical Q key.
    int shortcut_code(xkb_keysym_t, int fallback) override {
        return fallback == KEY_A ? KEY_Q : fallback;
    }
    void send(int code, int value) override {
        sent.push_back(std::to_string(code) + ":" + std::to_string(value));
    }
    bool text_available() override { return true; }
    bool commit_text(const std::string& text) override {
        sent.push_back("text:" + text);
        return true;
    }
};

void sink_tests() {
    Backend backend;
    ShellRouter router(backend);
    require(router.commit_text("off") && backend.sent.back() == "text:off", "off, text passes through");
    router.set_on(true);
    router.set_context("c-1");
    require(!router.commit_text("Hello") && router.text_pending(), "a commit waits for the shell");
    auto events = router.take_events();
    require(events.size() == 1 && events[0].seq == 1 && events[0].context == "c-1" &&
                events[0].text == "Hello",
            "the commit becomes an event for its context");
    require(!router.commit_text("Hello") && router.take_events().empty(),
            "offered again, it is not sent twice");
    router.acknowledge("other", 1);
    require(!router.commit_text("Hello"), "an answer for another context is ignored");
    router.acknowledge("c-1", 1);
    require(router.commit_text("Hello") && !router.text_pending(), "the shell's ack completes it");
    require(backend.sent.size() == 1, "nothing reaches the backend while the sink is on");
    // Named keys carry their logical ID and the modifiers held before them.
    require(router.deliver_key(KEY_LEFTCTRL, 1) == Delivery::Success, "a modifier alone is no event");
    require(router.deliver_key(KEY_C, 1) == Delivery::Waiting, "Ctrl+C waits for its ack");
    events = router.take_events();
    require(events.size() == 1 && events[0].key == "KeyC" &&
                events[0].modifiers == std::vector<std::string>{"Ctrl"},
            "Copy arrives as KeyC with Ctrl");
    router.acknowledge("c-1", events[0].seq);
    require(router.deliver_key(KEY_C, 1) == Delivery::Success &&
                router.deliver_key(KEY_C, 0) == Delivery::Success &&
                router.deliver_key(KEY_LEFTCTRL, 0) == Delivery::Success,
            "the press completes; its release sends nothing");
    require(router.deliver_key(KEY_BACKSPACE, 2) == Delivery::Waiting, "a repeat is its own event");
    events = router.take_events();
    require(events.size() == 1 && events[0].key == "Backspace" && events[0].repeat &&
                events[0].modifiers.empty(),
            "marked as a repeat, without modifiers once released");
    router.reject("c-1", events[0].seq);
    require(router.deliver_key(KEY_BACKSPACE, 2) == Delivery::Failure, "a reject fails that entry");
    // A context change drops what was in flight for the old field.
    require(!router.commit_text("late"), "in flight");
    router.take_events();
    router.set_context("c-2");
    require(!router.text_pending() && router.settled(), "switching contexts drops it");
    router.set_on(false);
    require(router.commit_text("back") && backend.sent.back() == "text:back",
            "off again, the backend types");

    // The sink owns readiness and key IDs: the system backend's pause, reset
    // and keymap apply only to normal output.
    backend.paused = backend.reset = true;
    require(!router.pump() && router.shortcut_code(XKB_KEY_a, KEY_A) == KEY_Q,
            "off, the backend governs");
    router.set_on(true);
    require(router.pump() && !router.take_input_reset() &&
                router.shortcut_code(XKB_KEY_a, KEY_A) == KEY_A,
            "on, a paused or remapped backend does not change typing into the shell");
    backend.paused = false;
    router.set_on(false);

    // Cancelled before it was written: it never goes out.
    router.set_on(true);
    router.set_context("c-3");
    require(!router.commit_text("gone"), "queued");
    router.cancel_delivery();
    require(!router.commit_text("next"), "a fresh commit after the cancel");
    events = router.take_events();
    require(events.size() == 1 && events[0].text == "next", "only the fresh commit is sent");
    router.set_on(false);

    // Through the real output queue: a word waits for the ack, then the next.
    Backend second;
    ShellRouter routed(second);
    routed.set_on(true);
    routed.set_context("c");
    OutputController output(routed);
    output.set_enabled(true);
    require(output.commit_text("one") && output.commit_text("two"), "the queue takes both words");
    output.tick(0);
    events = routed.take_events();
    require(events.size() == 1 && events[0].text == "one", "only one event is in flight");
    routed.acknowledge("c", events[0].seq);
    output.tick(.1);
    events = routed.take_events();
    require(events.size() == 1 && events[0].text == "two", "the next follows the ack");
    output.cancel();
    require(routed.settled(), "cancelling the queue settles the sink");
}
// The update handover's timing: a quiesce request, a two-second answer, a
// five-second pause before asking again.
void handover_tests() {
    UpdateHandover handover;
    require(!handover.ask(0, false), "nothing to hand over");
    require(handover.ask(1, true) && !handover.ask(1.5, true),
            "asked once while waiting for the answer");
    require(!handover.expired(2.9) && handover.expired(3.1) && !handover.expired(3.2),
            "an unanswered quiesce expires once after two seconds");
    require(!handover.ask(7, true) && handover.ask(8.2, true), "and is asked again five seconds later");
    handover.end();
    require(!handover.asking() && !handover.expired(100), "quiesced ends it");
    require(handover.ask(200, true), "asked again");
    handover.postpone(201);
    require(!handover.asking() && !handover.ask(203, true) && handover.ask(206.5, true),
            "a postponed handover waits before asking again");
    require(UpdateHandover::ready("idle", false, false, true) &&
                !UpdateHandover::ready("idle", true, false, true) &&
                !UpdateHandover::ready("idle", false, true, true) &&
                !UpdateHandover::ready("idle", false, false, false) &&
                UpdateHandover::ready("now", true, true, false),
            "idle needs a hidden panel not about to show and nothing unfinished; now does not");
    // The idle check, matched against a keyboard's own state, belongs to the caller.
    std::vector<std::string> names;
    for (const auto state :
         {Updates::State::Checking, Updates::State::Installing, Updates::State::Prepared}) {
        names.emplace_back(update_state_name(state));
    }
    require(names == std::vector<std::string>{"checking", "downloading", "prepared"},
            "update states keep protocol names");
    require(update_error_code("rate-limit") == "rate_limit" &&
                update_error_code("install-busy") == "locked" &&
                update_error_code("incompatible") == "incompatible" &&
                update_error_code("odd") == "failed",
            "failures become short reasons");
    auto matrix = json_ptr(json_tokener_parse("[1,0,0,0.5, 0,1,0,1, 0,0,1,-2]"));
    const auto pose = json_matrix(matrix.get());
    require(pose && (*pose)[0][3] == .5 && (*pose)[2][3] == -2, "a flat 3x4 matrix reads row-major");
    auto rows = json_ptr(matrix_json(*pose));
    require(json_matrix(rows.get()) == pose, "rows of four read the same");
    auto short_matrix = json_ptr(json_tokener_parse("[1,2,3]"));
    require(!json_matrix(short_matrix.get()), "a short matrix is refused");
}

// Preview renders: sample state at the width asked for, with an installed
// theme or the shell's own.
void preview_tests() {
    char pattern[] = "/tmp/framekeyboard-preview-XXXXXX";
    require(mkdtemp(pattern), "temporary directory");
    const std::filesystem::path directory(pattern);
    auto render = [&](PreviewRenderer& previews, PreviewRenderer::Request request) {
        require(previews.start(request), "a render starts");
        require(!previews.start(request), "one render at a time");
        for (int i = 0; i < 600; ++i) {
            if (auto result = previews.take()) {
                return *result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("the preview did not finish");
    };
    {
        PreviewRenderer previews(directory);
        PreviewRenderer::Request request;
        request.id = 7;
        request.layout = "en-us-full";
        request.language = "en-us";
        request.theme = "graphite";
        request.width = 400;
        const auto result = render(previews, request);
        require(result.id == 7 && result.error.empty() && std::filesystem::exists(result.path),
                "a preview is written in the private directory");
        cairo_surface_t* image = cairo_image_surface_create_from_png(result.path.c_str());
        const int width = cairo_image_surface_get_width(image);
        cairo_surface_destroy(image);
        require(width == 400, "at the width asked for");
        request.theme = "shell";
        request.width = 0;
        request.theme_json = R"({"schema_version":1,"id":"x","name":"x","surface":"#101010",
            "surface_radius":20,"key_top":"#303030","key_middle":"#282828","key_bottom":"#202020",
            "key_side_top":"#181818","key_side_bottom":"#101010","legend":"#ffffff","key_radius":8,
            "key_depth":3.5,"key_press_travel":3,"press_duration_ms":80,"font_size":20,"small_font_size":13,
            "padding":20,"hover":"#404040","latched":"#335577","disabled":"#151515"})";
        require(render(previews, request).error.empty(), "the shell's own theme renders too");
        request.theme = "missing";
        request.theme_json.clear();
        require(!render(previews, request).error.empty(), "an unknown theme is an error");
    }
    require(!std::filesystem::exists(directory / "preview-0.png"),
            "previews go when the keyboard exits");
    std::filesystem::remove_all(directory);
}

// Running instances (docs/integration.md): one eviction, then the lock.
void instance_tests() {
    char pattern[] = "/tmp/framekeyboard-shell-XXXXXX";
    require(mkdtemp(pattern), "temporary directory");
    const std::filesystem::path base(pattern);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto runtime = base / "framekeyboard";
    {
        // A hosted owner answers a plain launch that a shell manages it.
        VrInstance owner(runtime, VrInstance::Launch::Notify, true);
        require(owner.is_owner(), "the first launch owns VR");
        auto plain = std::async(std::launch::async, [&] {
            return VrInstance(runtime, VrInstance::Launch::Notify).outcome();
        });
        int launched = 0;
        while (plain.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            const auto request = owner.poll();
            require(request != VrInstance::Request::Recenter, "a hosted owner never takes a recenter");
            launched += request == VrInstance::Request::Launched;
        }
        require(plain.get() == VrInstance::Outcome::ShellManaged,
                "the plain launch learns a shell manages it");
        require(launched == 1, "and the owner can tell its shell the user launched it");
    }
    {
        // A standalone owner quits for a hosted launch, which then takes the lock.
        auto owner = std::make_unique<VrInstance>(runtime, VrInstance::Launch::Notify);
        auto hosted = std::async(std::launch::async, [&] {
            return VrInstance(runtime, VrInstance::Launch::Hosted, true).outcome();
        });
        bool quit = false;
        while (!quit) {
            quit = owner->poll() == VrInstance::Request::Quit;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        owner.reset(); // exits as Settings → Quit does
        require(hosted.get() == VrInstance::Outcome::Owner,
                "the hosted launch owns VR after the eviction");
    }
    {
        // An owner that never answers quit-v1, like a release before protocol 1.
        VrInstance owner(runtime, VrInstance::Launch::Notify);
        const auto outcome = VrInstance(runtime, VrInstance::Launch::Hosted, true).outcome();
        require(outcome == VrInstance::Outcome::QuitRefused, "an unanswered quit exits with status 11");
    }
    {
        // An owner that accepts but keeps the lock: the launch gives up, once.
        VrInstance owner(runtime, VrInstance::Launch::Notify);
        auto hosted = std::async(std::launch::async, [&] {
            return VrInstance(runtime, VrInstance::Launch::Hosted, true).outcome();
        });
        int quits = 0;
        while (hosted.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
            quits += owner.poll() == VrInstance::Request::Quit;
        }
        require(hosted.get() == VrInstance::Outcome::LockTaken && quits == 1,
                "a lock still held after the quit exits with status 12, without evicting again");
    }
    {
        // An update successor adopts the lock its predecessor handed over.
        auto owner = std::make_unique<VrInstance>(runtime, VrInstance::Launch::Notify, true);
        const int lock = owner->release_lock();
        owner.reset();
        require(lock >= 0, "the lock is handed over");
        require(VrInstance(runtime, VrInstance::Launch::Silent).outcome() ==
                    VrInstance::Outcome::Running,
                "nobody else can take it in the gap");
        VrInstance successor(runtime, lock);
        require(successor.is_owner(), "the successor owns VR on the inherited lock");
    }
    std::filesystem::remove_all(base);
}
} // namespace

int main() {
    try {
        descriptor_tests();
        session_tests();
        limits_tests();
        drain_tests();
        sink_tests();
        handover_tests();
        preview_tests();
        instance_tests();
        std::cout << "Shell tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
