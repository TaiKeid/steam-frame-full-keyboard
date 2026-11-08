#pragma once
#include "localization.hpp"

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
    const std::string& version() const { return version_; }
    // The Updates button's words, and why the last attempt failed, as keys
    // App shows in the interface language.
    UiText label() const;
    UiText failure_reason() const;
    bool busy() const;
    void click(double now);
    bool tick(double now);
    // Call every tick, with a clock that counts sleep so a headset that mostly
    // sleeps still comes due. Checks a while after startup and every few hours
    // until an update is on offer, without a visible state or any error report
    // (docs/updates.md, Automatic checks).
    void check_periodically(double clock);
    // A newer release is known, whatever the button says right now.
    bool offered() const { return !offer_.empty(); }
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
    bool send(const std::string& operation, bool silent = false);
    void stop();
    void fail(double now, const std::string& reason = {});
    void release_helper();
    std::string version_, offer_, reason_;
    UiText progress_, failure_;
    std::uint64_t done_{}, total_{};
    // Installed for a shell, whose keyboard waits for idleness while
    // Prepared: losing the helper then must not leave Prepared behind.
    bool hosted_install_{};
    std::vector<std::string> arguments_;
    std::filesystem::path helper_;
    State state_{State::Idle};
    double reset_at_{};
    // A silent check leaves state_ alone until it finds an offer.
    bool silent_check_{};
    // A failed check is followed sooner; check_periodically has the clock.
    bool check_failed_{};
    int failed_checks_{};
    double next_check_{-1}, last_clock_{-1};
    int fd_{-1}, child_{-1};
    bool handed_off_{};
};

// A private socket acknowledges three seconds of completed-loop progress after an update.
class UpdateStartup {
  public:
    UpdateStartup();
    ~UpdateStartup();
    // Started by the update helper, before or after service adoption. The user
    // started the update, so the keyboard shows again; only a shell's hides.
    bool update_launch() const { return update_launch_; }

  private:
    bool update_launch_{};
};
void update_startup_ready(double now);
// A hosted update successor tells its shell `ready` only once the helper has
// committed the update (docs/integration.md, Handover). True for any other launch.
bool update_committed();
} // namespace framekeyboard
