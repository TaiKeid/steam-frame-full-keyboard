#pragma once
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stop_token>

namespace framekeyboard {
bool steamvr_running(const std::filesystem::path& proc = "/proc");
void require_steamvr();

// Tracking recovery must remain observable after a replacement lease hides the panel.
class TrackingReadiness {
  public:
    bool should_sample(bool dashboard_visible, bool panel_requested, bool panel_visible,
                       double now) const;
    void sampled(bool valid, double now);
    void request_sample() { next_check_ = 0; }
    bool waiting() const { return waiting_; }

  private:
    bool waiting_{};
    double next_check_{};
};

// Paces the replacement watchdog thread. With replacement off there is no helper
// reading health and nothing to hide, so the thread blocks instead of polling.
// The VR thread owns the enabled flag; the watchdog thread only waits on it.
class WatchdogSchedule {
  public:
    void set_enabled(bool enabled);
    // Returns false once stop is requested. While disabled, waits for enable or
    // stop; while enabled, waits one period (stop still interrupts it).
    bool wait(std::stop_token stop, std::chrono::milliseconds period);

  private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool enabled_{};
};
} // namespace framekeyboard
