#pragma once
#include "placement.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace framekeyboard {
struct SavedPlacement {
    bool operator==(const SavedPlacement&) const = default;
    Transform transform;
    double width;
    std::uint64_t universe;
    std::optional<Transform> dashboard{};
    std::optional<Transform> dashboard_bar{};
    bool dashboard_full_rotation{};
    // The curved keyboard's bend, kept while Curved keyboard is off; 0 flat.
    double curvature{};
};
std::optional<SavedPlacement> load_placement(const std::filesystem::path& path);
void save_placement(const std::filesystem::path& path, const SavedPlacement& placement);

// When to save the placement while the keyboard runs, so a crash or a kill,
// which skip the save at exit, lose no move, resize or bend that ended (R301):
// once it has stayed the same for `quiet` seconds after a change. The VR loop
// doesn't ask while a move, resize or bend holds the keyboard (R304). The
// first placement seen is the one already on disk (restored or recentered),
// so it counts as saved.
class PlacementSettle {
  public:
    explicit PlacementSettle(double quiet = 2) : quiet_(quiet) {}
    // Called every frame with the placement as it would be saved; true when
    // it should be saved now.
    bool due(const SavedPlacement& now, double time);
    // The last save failed: ask again for that placement once a delay has
    // passed that doubles from 5 s up to five minutes, so a failing disk isn't
    // written to every frame (R303). Only that placement waits; any other is
    // saved once it settles, as always (R307).
    void retry(double time);
    // A save worked: the next failure's retry waits 5 s again (R308), and a
    // retry an older failure armed is dropped, as the worked write came after
    // it (R311).
    void worked() {
        retry_delay_ = 0;
        retrying_ = false;
    }

  private:
    double quiet_;
    std::optional<SavedPlacement> seen_, saved_;
    double seen_since_{};
    bool retrying_{};
    double retry_at_{}, retry_delay_{};
};

// Writes placements on a worker thread: save_placement syncs the file, which
// the VR loop must never wait for. Owns only the snapshots it is given; the
// newest pending one replaces an older one not yet written, so an older
// placement can never land after a newer one.
class PlacementWriter {
  public:
    explicit PlacementWriter(std::filesystem::path path);
    // Writes what is still pending, then stops.
    ~PlacementWriter();
    PlacementWriter(const PlacementWriter&) = delete;
    PlacementWriter& operator=(const PlacementWriter&) = delete;
    void save(const SavedPlacement& placement);
    // Returns once every placement given so far is on disk (or failed), for
    // exit and update handoff.
    void flush();
    // How the latest write since the last call went, for PlacementSettle's
    // retry() and worked(); None when nothing was written.
    enum class Result { None, Worked, Failed };
    Result take_result() { return result_.exchange(Result::None); }

  private:
    void run();
    std::filesystem::path path_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<SavedPlacement> pending_;
    bool writing_{}, stopping_{};
    std::atomic<Result> result_{Result::None}; // Set by the worker, taken by the VR loop.
    std::thread worker_;                       // Last, so it starts after every member it uses.
};
} // namespace framekeyboard
