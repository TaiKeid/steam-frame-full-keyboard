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
    static constexpr double timeout = 2, retry_after = 5;
    // Whether to ask now: `ready` is a prepared update the keyboard may activate.
    bool ask(double now, bool ready) {
        if (!ready || asking_ || now < retry_at_) {
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
    // `quiesced` arrived, or the update was cancelled.
    void end() { asking_ = false; }

  private:
    bool asking_{};
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
    };
    // False while another render runs.
    bool start(Request request);
    struct Result {
        std::int64_t id{};
        std::string path, error;
    };
    std::optional<Result> take();

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
