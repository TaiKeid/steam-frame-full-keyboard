// A backend that only pretends: no network, no files, no processes. Every
// task's progress follows the clock, so screens and their timing can be
// looked at on any Linux PC.
#include "backend.hpp"
#include <algorithm>
#include <chrono>

namespace framekeyboard::installer {
namespace {

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// How long each part takes at speed 1, in seconds.
constexpr double releases_time = 1.2, stopping_time = 1, download_time = 7, verify_time = 1.2,
                 install_time = 2.5, uninstall_time = 2.5, finish_time = 1.5;

class SimulatedBackend : public Backend {
  public:
    explicit SimulatedBackend(SimulatedOptions options) : options_(std::move(options)) {
        status_.installed = options_.installed;
    }
    void releases() override { start(Task::Releases); }
    void install(const Release& release) override {
        if (start(Task::Install)) {
            target_ = release.version;
            stopping_ = !status_.installed.empty();
        }
    }
    void uninstall(bool) override { start(Task::Uninstall); }
    void finish(const Choices&) override { start(Task::Finish); }

    Status status() override {
        if (status_.state != State::Running) {
            return status_;
        }
        const double elapsed = (seconds() - started_) * options_.speed;
        switch (status_.task) {
        case Task::Releases:
            if (elapsed >= releases_time) {
                end("releases");
                if (status_.state == State::Done) {
                    status_.releases.clear();
                    for (const auto& version : options_.releases) {
                        status_.releases.push_back({version, version});
                    }
                    if (status_.releases.empty()) {
                        status_.releases.push_back({FRAMEKEYBOARD_VERSION, FRAMEKEYBOARD_VERSION});
                    }
                }
            }
            break;
        case Task::Install: {
            // Download, verify, then stop a running keyboard and install.
            double at = elapsed;
            status_.download = std::clamp(at / download_time, 0.0, 1.0);
            if (at < download_time) {
                status_.stage = Stage::Download;
                if (options_.fail == "download" && at > download_time * .4) {
                    fail("installer.error.network");
                }
                break;
            }
            at -= download_time;
            if (at < verify_time) {
                status_.stage = Stage::Verify;
                break;
            }
            at -= verify_time;
            if (stopping_ && at < stopping_time) {
                status_.stage = Stage::Stopping;
                break;
            }
            at -= stopping_ ? stopping_time : 0;
            status_.stage = Stage::Install;
            if (at >= install_time) {
                end("install");
                if (status_.state == State::Done) {
                    status_.installed = target_;
                }
            }
            break;
        }
        case Task::Uninstall:
            if (elapsed >= uninstall_time) {
                end("uninstall");
                if (status_.state == State::Done) {
                    status_.installed.clear();
                }
            }
            break;
        case Task::Finish:
            if (elapsed >= finish_time) {
                end("finish");
            }
            break;
        case Task::None:
            break;
        }
        return status_;
    }

  private:
    bool start(Task task) {
        if (status_.state == State::Running) {
            return false;
        }
        status_.task = task;
        status_.state = State::Running;
        status_.stage = Stage::Download;
        status_.download = 0;
        status_.error = {};
        started_ = seconds();
        return true;
    }
    void fail(const char* key) {
        status_.state = State::Failed;
        status_.error = ui(key);
    }
    // Ends the task, failing it when it is the one asked to fail.
    void end(const std::string& name) {
        if (options_.fail == name) {
            fail(name == "releases"    ? "installer.error.network"
                 : name == "uninstall" ? "installer.error.uninstall"
                 : name == "finish"    ? "installer.error.settings"
                                       : "installer.error.install");
        } else {
            status_.state = State::Done;
        }
    }

    SimulatedOptions options_;
    Status status_;
    std::string target_;
    bool stopping_{};
    double started_{};
};

} // namespace

std::unique_ptr<Backend> simulated_backend(SimulatedOptions options) {
    return std::make_unique<SimulatedBackend>(std::move(options));
}

} // namespace framekeyboard::installer
