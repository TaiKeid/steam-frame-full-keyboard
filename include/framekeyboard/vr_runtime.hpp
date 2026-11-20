#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

// Every overlay this process creates, by handle, so the stall watchdog hides
// them all and a new one can't be left out (R405). Creating registers and
// destroying unregisters, on the VR thread; the watchdog only reads. Fixed
// storage of lock-free atomics: the watchdog takes no lock the stalled thread
// could hold. A handle read just before its overlay is destroyed gets a hide
// SteamVR refuses, which is harmless.
class OverlayRegistry {
  public:
    static constexpr std::size_t capacity = 16;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    // False, registering nothing, for handle 0 or when every place is taken.
    bool add(std::uint64_t handle) noexcept {
        if (handle == 0) {
            return false;
        }
        for (auto& place : handles_) {
            std::uint64_t empty = 0;
            if (place.compare_exchange_strong(empty, handle, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }
    void remove(std::uint64_t handle) noexcept {
        for (auto& place : handles_) {
            std::uint64_t expected = handle;
            if (handle != 0 && place.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                return;
            }
        }
    }
    std::size_t size() const noexcept {
        std::size_t count = 0;
        for (const auto& place : handles_) {
            count += place.load(std::memory_order_acquire) != 0;
        }
        return count;
    }
    // The watchdog's, on a stall: hide(handle) for each registered overlay,
    // then a new generation, so every owner's OverlayShown reads hidden.
    template <class Hide> void hide_all(Hide&& hide) noexcept {
        for (const auto& place : handles_) {
            if (const auto handle = place.load(std::memory_order_acquire)) {
                hide(handle);
            }
        }
        hides_.fetch_add(1, std::memory_order_acq_rel);
    }
    // How many times hide_all ran.
    std::uint64_t hides() const noexcept { return hides_.load(std::memory_order_acquire); }

  private:
    std::array<std::atomic<std::uint64_t>, capacity> handles_{};
    std::atomic<std::uint64_t> hides_{};
};
// An owner's record that its overlay shows. A stall's hide_all hides it
// behind the owner's back: it then reads false, so the owner's next decision
// to show it shows it again (R405). Assigned and read like a bool.
class OverlayShown {
  public:
    explicit OverlayShown(const OverlayRegistry& registry) noexcept : registry_(&registry) {}
    OverlayShown& operator=(bool shown) noexcept {
        shown_ = shown;
        seen_ = registry_->hides();
        return *this;
    }
    operator bool() const noexcept { return shown_ && seen_ == registry_->hides(); }

  private:
    const OverlayRegistry* registry_;
    bool shown_{};
    std::uint64_t seen_{};
};
// The stall check watches a keyboard shown under replacement or a shell, and
// SteamVR's laser mode held for the window laser whatever else, as only the
// VR loop could otherwise end it: with replacement off over a floating
// window, or a panel hidden under a shell while a stream is typed (R405).
constexpr bool stall_watched(bool replace_or_hosted, bool visible, bool window_laser) {
    return (replace_or_hosted && visible) || window_laser;
}
// The watchdog thread runs while replacement or a shell may need it, or the
// window laser is on.
constexpr bool watchdog_enabled(bool replace_or_hosted, bool window_laser) {
    return replace_or_hosted || window_laser;
}

// Only a watched panel someone can see needs the faster stall check.
constexpr int overlay_watchdog_poll_ms(bool watched, bool worn) {
    return watched && worn ? 50 : 250;
}

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
