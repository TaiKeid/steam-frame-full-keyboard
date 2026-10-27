#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace framekeyboard {
// Boot opt-in is checked once. User-requested update restarts are normal launches.
std::vector<std::string> update_restart_arguments(const std::vector<std::string>& arguments);
// The UI only polls private packets. Network and installation work runs in a child.
class Updates {
  public:
    Updates(std::string version, std::vector<std::string> arguments, std::filesystem::path helper = {});
    ~Updates();
    Updates(const Updates&) = delete;
    Updates& operator=(const Updates&) = delete;
    std::string label() const;
    std::string failure_reason() const;
    bool busy() const;
    void click(double now);
    bool tick(double now);
    bool prepared() const { return state_ == State::Prepared; }
    // Call after App, input, VR, replacement and the instance lock are gone.
    bool handoff();
    // A hosted keyboard's handover (docs/integration.md, Handover): the
    // shell's connection and the instance lock go to the helper, which starts
    // the successor on both as session `session + 1`.
    bool handoff(int control, int lock, std::uint64_t session);

    // Shell requests (docs/integration.md, Updates); no UI is involved.
    bool check(double now);
    // The newest stable release, or exactly `version`. `protocol` is the
    // shell protocol in use, which the new package must support.
    bool install(double now, const std::string& version, int protocol);
    // Before activation only: stops the helper, which removes its staging.
    bool cancel(double now);
    enum class State { Idle, Checking, Latest, Available, Installing, Error, Prepared };
    State state() const { return state_; }
    const std::string& latest() const { return offer_; }
    // The helper's last failure code, such as "network-unavailable".
    const std::string& failure_code() const { return reason_; }
    // Bytes of the release archive downloaded so far, and its size.
    std::pair<std::uint64_t, std::uint64_t> downloaded() const { return {done_, total_}; }

  private:
    bool start();
    bool send(const std::string& operation);
    void stop();
    void fail(double now, const std::string& reason = {});
    std::string version_, offer_, progress_, failure_, reason_;
    std::uint64_t done_{}, total_{};
    std::vector<std::string> arguments_;
    std::filesystem::path helper_;
    State state_{State::Idle};
    double reset_at_{};
    int fd_{-1}, child_{-1};
    bool handed_off_{};
};

// A private socket acknowledges three seconds of completed-loop progress after an update.
class UpdateStartup {
  public:
    UpdateStartup();
    ~UpdateStartup();
    // Update/rollback is explicit; hidden presentation does not recheck boot opt-in.
    bool background_start() const { return background_; }
    // Started by the update helper, before or after service adoption.
    bool update_launch() const { return update_launch_; }

  private:
    bool background_{}, update_launch_{};
};
void update_startup_ready(double now);
// A hosted update successor tells its shell `ready` only once the helper has
// committed the update (docs/integration.md, Handover). True for any other launch.
bool update_committed();
} // namespace framekeyboard
