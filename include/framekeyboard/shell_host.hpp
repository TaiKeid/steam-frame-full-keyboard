#pragma once

#include "panel.hpp"
#include "placement.hpp"
#include "shell_protocol.hpp"
#include "updates.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace framekeyboard {
// Pieces of the hosted VR loop that need no OpenVR (docs/integration.md):
// matrices and texture coordinates in JSON, update states, and preview renders.

// A 3×4 row-major matrix, as 12 numbers or 3 rows of 4; none if invalid.
std::optional<Transform> json_matrix(json_object* value);
json_object* matrix_json(const Transform& transform);
// A panel rectangle as texture coordinates, 0–1 from the top left of the
// whole overlay texture, popup margins included.
json_object* rect_uv(const Rect& rect);
// Panel coordinates for a texture position.
std::pair<double, double> uv_to_panel(double u, double v);

// The update helper's state as protocol events name it.
const char* update_state_name(Updates::State state);
// The helper's failure code as the protocol's short reason.
std::string update_error_code(const std::string& reason);

// The timing of a hosted update's handover (docs/integration.md, Handover).
// Once an update is prepared and the keyboard is idle, or the shell asked
// for `now`, the shell is asked to quiesce. Without its `quiesced` within two
// seconds the update returns to prepared, and the next request waits five.
class UpdateHandover {
  public:
    // `drain_time` bounds each wait for the shell to read: first the replies
    // before `restarting`, then `restarting` itself.
    static constexpr double timeout = 2, retry_after = 5, drain_time = 1;
    // Whether to ask now: `ready` is a prepared update the keyboard may activate.
    bool ask(double now, bool ready) {
        if (!ready || asking_ || draining_ || now < retry_at_) {
            return false;
        }
        asking_ = true;
        deadline_ = now + timeout;
        return true;
    }
    // True once when the shell let the deadline pass.
    bool expired(double now) {
        if (!asking_ || now <= deadline_) {
            return false;
        }
        asking_ = false;
        retry_at_ = now + retry_after;
        return true;
    }
    bool asking() const { return asking_; }
    // `quiesced` arrived: the replies before it drain over the next frames.
    void quiesced(double now) {
        asking_ = restarting_ = false;
        draining_ = true;
        deadline_ = now + drain_time;
    }
    bool draining() const { return draining_; }
    // The replies are read and `restarting` is written; it drains next.
    void restart(double now) {
        restarting_ = true;
        deadline_ = now + drain_time;
    }
    bool restarting() const { return restarting_; }
    // The shell has not read what is waiting in time.
    bool drain_expired(double now) const { return draining_ && now > deadline_; }
    // Handed over, or the update was cancelled.
    void end() { asking_ = draining_ = restarting_ = false; }
    // Not ready after all at `quiesced` or while draining, or the session
    // could not be drained: back to prepared, asked again later.
    void postpone(double now) {
        end();
        retry_at_ = now + retry_after;
    }
    // Whether a prepared update may activate: at once for `now`; for `idle`,
    // with the panel hidden and not about to show, and nothing unfinished.
    static bool ready(const std::string& when, bool shown, bool visible, bool idle) {
        return when == "now" || (!shown && !visible && idle);
    }

  private:
    bool asking_{}, draining_{}, restarting_{};
    double deadline_{}, retry_at_{};
};

// Renders isolated sample state on a worker, one at a time, into PNGs in the
// keyboard's private runtime directory (docs/integration.md, Preview render).
// It never draws the live editor, composition or the user's settings.
class PreviewRenderer {
  public:
    explicit PreviewRenderer(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    struct Request {
        std::int64_t id{};
        std::string view{"keyboard"}, layout, language, theme;
        std::filesystem::path data_dir;
        // The shell's own theme as it sent it, when `theme` is "shell".
        std::string theme_json;
        // Pixels wide, 64 to 2048; zero keeps the panel's own width.
        int width{};
    };
    // False while another render runs. A finished render nobody took, such
    // as one failed during an update handover, is dropped.
    bool start(Request request);
    struct Result {
        std::int64_t id{};
        std::string path, error;
    };
    // The result for request `id` once it is ready; any other is dropped.
    std::optional<Result> take(std::int64_t id);

  private:
    void run(Request request);
    std::filesystem::path directory_;
    std::thread worker_;
    std::mutex mutex_;
    std::optional<Result> result_;
    std::atomic<bool> busy_{false};
    int next_{};
};
} // namespace framekeyboard
