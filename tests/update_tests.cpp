#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
void wait_label(Updates& updates, const std::string& label, double now) {
    for (int i = 0; i < 300; ++i) {
        updates.tick(now);
        if (updates.label() == label) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("unexpected update label: " + updates.label());
}
} // namespace
int main(int argc, char** argv) {
    try {
        // Used only by the ARM64 integration test in a disposable installation.
        if (argc == 4 && std::string(argv[1]) == "--handoff-fixture") {
            const std::string helper = argv[2], config = argv[3];
            Updates updates("0.5.0", {"--preview", "--duration", "6", "--config-dir", config}, helper);
            {
                VrInstance instance(std::filesystem::path(config) / "instance");
                require(instance.is_owner(), "fixture owns its private instance lock");
                NullSink sink;
                Options options;
                options.mode = "preview";
                options.config_dir = config;
                App app(options, sink, {}, &updates);
                app.show_settings();
                const auto press = [&] {
                    app.down(0, 800, 30, monotonic_seconds());
                    app.up(0, 800, 30);
                };
                press();
                bool installing = false;
                const double deadline = monotonic_seconds() + 30;
                while (!app.quitting() && monotonic_seconds() < deadline) {
                    app.tick(monotonic_seconds());
                    if (!installing && updates.label() == "Download and install update") {
                        press();
                        installing = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                require(app.quitting() && updates.prepared(), "prepared update exits the app");
                app.flush_settings();
            }
            require(updates.handoff(), "handoff after input and instance cleanup");
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--preview") {
                UpdateStartup startup;
                for (int j = 1; j + 1 < argc; ++j) {
                    if (std::string(argv[j]) == "--config-dir") {
                        std::ofstream(std::filesystem::path(argv[j + 1]) / "rollback-started")
                            << "ready";
                    }
                }
                const auto start = monotonic_seconds();
                while (monotonic_seconds() - start < 4) {
                    update_startup_ready(monotonic_seconds());
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                return 0;
            }
        }
        {
            int sockets[2];
            require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0,
                    "private startup test socket");
            setenv("FRAMEKEYBOARD_UPDATE_READY_FD", std::to_string(sockets[1]).c_str(), 1);
            UpdateStartup startup;
            char byte{};
            const auto pending = [&] {
                return recv(sockets[0], &byte, 1, MSG_DONTWAIT) < 0 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK);
            };
            update_startup_ready(10);
            require(pending(), "first completed loop cannot commit startup");
            update_startup_ready(14);
            require(pending(), "a stalled loop cannot commit startup");
            for (double now = 14.25; now < 17; now += .25) {
                update_startup_ready(now);
                require(pending(), "hidden-loop cadence must finish its probation");
            }
            update_startup_ready(17);
            require(recv(sockets[0], &byte, 1, 0) == 1 && byte == '1',
                    "three seconds of continuing progress confirms startup while hidden");
            close(sockets[0]);
        }
        char temporary[] = "/tmp/full-keyboard-update-ui-XXXXXX";
        const auto directory = std::filesystem::path(mkdtemp(temporary));
        const auto helper = directory / "helper.py";
        std::ofstream(helper) << R"PY(import json,socket,sys
peer=socket.socket(fileno=3)
peer.setblocking(True)
mode=sys.argv[-1]
while True:
    packet=peer.recv(2048)
    if not packet: break
    command=json.loads(packet)
    if command['op']=='check':
        if mode=='exit': break
        value={'state':'latest'} if mode=='latest' else {'state':'available','version':'9.0.0'}
        peer.send(json.dumps(value).encode())
    elif command['op']=='install':
        assert command['version']=='9.0.0'
        if mode in ('failure','unknown-failure'):
            peer.send(json.dumps({'state':'error','reason':'checksum-failed' if mode=='failure' else 'untrusted exception text'}).encode())
        else:
            peer.send(b'{"state":"progress","phase":"verify"}')
            peer.send(b'{"state":"prepared"}')
)PY";
        {
            Updates updates("0.6.0", {"latest"}, helper);
            require(updates.label() == "Check for updates", "initial label");
            updates.click(10);
            require(updates.busy(), "check is asynchronous");
            updates.click(10); // A second pointer cannot duplicate an in-flight operation.
            wait_label(updates, "You are on latest version", 20);
            updates.tick(24.999);
            require(updates.label() == "You are on latest version", "latest remains for five seconds");
            updates.click(24.999);
            updates.tick(25);
            require(updates.label() == "Check for updates", "latest expires exactly after five seconds");
        }
        {
            Updates updates("0.6.0", {"available"}, helper);
            updates.click(10);
            wait_label(updates, "Download and install update", 20);
            updates.tick(100000);
            require(updates.label() == "Download and install update", "offer never expires");
            require(!updates.prepared(), "checking never installs");
            updates.click(100001);
            require(updates.busy(), "install is asynchronous");
            wait_label(updates, "Restarting keyboard...", 100002);
            require(updates.prepared(), "only a validated preparation requests restart");
        } // Cancellation here stops only the disposable helper, without activating anything.
        {
            Updates updates("0.6.0", {"available"}, helper);
            NullSink sink;
            Options options;
            options.mode = "preview";
            options.config_dir = directory / "config";
            App app(options, sink, {}, &updates);
            app.show_settings();
            app.down(0, 800, 30, monotonic_seconds());
            app.up(0, 800, 30);
            app.down(1, 800, 30, monotonic_seconds()); // Held while the async label changes.
            for (int i = 0; i < 300 && updates.label() != "Download and install update"; ++i) {
                app.tick(monotonic_seconds());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(updates.label() == "Download and install update", "App polls the update offer");
            app.up(1, 800, 30);
            require(!updates.busy(), "an old held press cannot accept a newly arrived offer");
            app.back();
            app.show_settings();
            const auto view = app.view();
            const auto button =
                std::find_if(view.controls.begin(), view.controls.end(),
                             [](const auto& control) { return control.id == "updates"; });
            require(button != view.controls.end() && button->label == "Download and install update",
                    "reopening Settings preserves the offer");
        }
        {
            Updates updates("0.6.0", {"failure"}, helper);
            updates.click(10);
            wait_label(updates, "Download and install update", 20);
            updates.click(21);
            wait_label(updates, "Update failed. Try again", 22);
            require(updates.failure_reason() == "Update checksum verification failed.",
                    "fixed helper failure reason reaches the UI");
            updates.tick(27);
            require(updates.failure_reason().empty(), "expired error clears the status reason");
            require(updates.label() == "Download and install update",
                    "failed install retains the offer");
            updates.click(28);
            wait_label(updates, "Update failed. Try again", 29);
        }
        {
            Updates updates("0.6.0", {"unknown-failure"}, helper);
            NullSink sink;
            Options options;
            options.mode = "preview";
            options.config_dir = directory / "reason-config";
            App app(options, sink, {}, &updates);
            app.show_settings();
            updates.click(10);
            wait_label(updates, "Download and install update", 20);
            updates.click(21);
            wait_label(updates, "Update failed. Try again", 22);
            require(app.view().status == "The update could not be completed. Try again.",
                    "Settings status uses a fixed fallback and never displays helper exception text");
            updates.tick(27);
            require(app.view().status != "The update could not be completed. Try again.",
                    "Settings status recovers after the temporary failure");
        }
        {
            Updates updates("0.6.0", {"exit"}, helper);
            updates.click(10);
            wait_label(updates, "Update failed. Try again", 20);
            updates.tick(26);
            require(updates.label() == "Check for updates", "failed check can retry");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Update button timing, persistent offers, duplicate clicks, cancellation and retry "
                     "passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
