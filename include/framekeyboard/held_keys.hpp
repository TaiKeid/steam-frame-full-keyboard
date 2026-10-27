#pragma once
#include <map>
#include <utility>
#include <vector>

namespace framekeyboard {
// Repeating keys, such as Backspace, held after their laser left the panel,
// one per pointer, so both hands can hold one (docs/architecture.md, Input and
// focus). Each stays down until its own trigger's release, read from the
// controller by `Watch::released(now)` since no button event arrives off our
// overlay, or for at most 30 seconds.
template <class Watch> class OffPanelHolds {
  public:
    static constexpr double limit = 30;

    // Replaces any earlier hold of the same pointer.
    void hold(unsigned pointer, Watch watch, double now) {
        holds_.insert_or_assign(pointer, Hold{now, std::move(watch)});
    }
    // The pointers whose keys must be released now. Holds whose key
    // `still_held(pointer)` no longer reports, because it was released back
    // over the panel or cancelled, are dropped without a release.
    template <class StillHeld> std::vector<unsigned> expired(double now, StillHeld still_held) {
        std::vector<unsigned> release;
        for (auto it = holds_.begin(); it != holds_.end();) {
            if (!still_held(it->first)) {
                it = holds_.erase(it);
            } else if (it->second.watch.released(now) || now - it->second.since > limit) {
                release.push_back(it->first);
                it = holds_.erase(it);
            } else {
                ++it;
            }
        }
        return release;
    }
    std::size_t size() const { return holds_.size(); }

  private:
    struct Hold {
        double since;
        Watch watch;
    };
    std::map<unsigned, Hold> holds_;
};
} // namespace framekeyboard
