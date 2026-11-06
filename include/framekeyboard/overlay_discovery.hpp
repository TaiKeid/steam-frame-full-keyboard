#pragma once
#include <cstddef>
#include <cstdint>
#include <map>

namespace framekeyboard {
// Finds Gamescope's per-window SteamVR overlays, gamescope.<display>.window.<N>,
// by N (WindowOverlays in vr.cpp). OpenVR cannot list overlays and Gamescope
// numbers windows in creation order, so a round, once a second, rechecks the
// known ones, probes the range above the highest and one range of a slow
// sweep for older ones; the first round also probes the low range most
// sessions use, about 10k lookups in all. A lookup costs about a
// microsecond, so a round is spread over frames, at most `per_frame`
// lookups in each, however many callers ask in one frame. Pure
// bookkeeping: `find(N)` looks the overlay up, 0 when there is none.
class OverlayDiscovery {
  public:
    using Handle = std::uint64_t;
    static constexpr std::uint32_t step = 1024, first_end = 8 * step, sweep_end = 65536;
    static constexpr std::size_t per_frame = 512;
    // This frame's share of the work; `now`, the frame's time, tells frames
    // apart. True when it finished a round.
    template <class Find> bool advance(double now, Find&& find) {
        if (now == advanced_) {
            return false;
        }
        advanced_ = now;
        if (phase_ == Phase::idle) {
            if (now < next_round_) {
                return false;
            }
            next_round_ = now + 1;
            phase_ = Phase::recheck;
            cursor_ = 0;
        }
        std::size_t budget = per_frame;
        while (phase_ != Phase::idle) {
            const auto known = phase_ == Phase::recheck ? known_.lower_bound(cursor_) : known_.end();
            if (phase_ == Phase::recheck && known == known_.end()) {
                enter(ready_ ? Phase::top : Phase::first);
            } else if (phase_ != Phase::recheck && cursor_ == end_) {
                finish_range();
            } else if (!budget) {
                break; // The rest waits for the next frame.
            } else {
                --budget;
                const auto number = phase_ == Phase::recheck ? known->first : cursor_;
                cursor_ = number + 1;
                const Handle handle = find(number);
                if (handle && phase_ != Phase::recheck) {
                    known_[number] = handle;
                } else if (!handle && phase_ == Phase::recheck) {
                    known_.erase(known); // Its window closed.
                }
            }
        }
        if (phase_ != Phase::idle) {
            return false;
        }
        ready_ = true;
        return true;
    }
    // Whether a first round finished, so known() lists every window overlay
    // in the ranges most sessions use.
    bool ready() const { return ready_; }
    const std::map<std::uint32_t, Handle>& known() const { return known_; }

  private:
    enum class Phase { idle, recheck, first, top, sweep };
    void enter(Phase phase) {
        phase_ = phase;
        if (phase == Phase::first) {
            cursor_ = 0;
            end_ = first_end;
        } else if (phase == Phase::top) {
            // Above the highest known now, as new windows take higher numbers.
            cursor_ = known_.empty() ? 0 : known_.rbegin()->first + 1;
            end_ = cursor_ + step;
        } else if (phase == Phase::sweep) {
            cursor_ = sweep_;
            end_ = sweep_ + step;
        }
    }
    void finish_range() {
        if (phase_ == Phase::first) {
            enter(Phase::top);
        } else if (phase_ == Phase::top) {
            enter(Phase::sweep);
        } else {
            sweep_ = sweep_ + step >= sweep_end ? 0 : sweep_ + step;
            phase_ = Phase::idle;
        }
    }
    std::map<std::uint32_t, Handle> known_;
    Phase phase_{Phase::idle};
    std::uint32_t cursor_{}, end_{}, sweep_{};
    double next_round_{}, advanced_{-1};
    bool ready_{};
};
} // namespace framekeyboard
