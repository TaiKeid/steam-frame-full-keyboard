#pragma once

#include <filesystem>
#include <string>

namespace framekeyboard {

// One VR owner per login session. A second launch sends a recenter request and
// exits before initializing OpenVR or creating another virtual input device.
// A hosted launch (docs/integration.md, Running instances) asks the owner to
// quit instead, once, then waits for the lock.
class VrInstance {
  public:
    enum class Launch { Notify, Silent, Hosted };
    enum class Outcome { Owner, Notified, Running, ShellManaged, QuitRefused, LockTaken };
    enum class Request { None, Recenter, Quit };
    explicit VrInstance(const std::filesystem::path& directory = runtime_directory(),
                        Launch launch = Launch::Notify, bool hosted_owner = false);
    // An update successor in hosted mode: the lock is already held by the
    // inherited descriptor, so no other launch could take it in the gap.
    VrInstance(const std::filesystem::path& directory, int inherited_lock);
    ~VrInstance();
    VrInstance(const VrInstance&) = delete;
    VrInstance& operator=(const VrInstance&) = delete;
    bool is_owner() const { return owner_; }
    Outcome outcome() const { return outcome_; }
    // At most one request per call: a quit outranks recenters.
    Request poll();
    bool poll_recenter() { return poll() == Request::Recenter; }
    // Stops answering requests but keeps the lock held, and hands its
    // descriptor over for the update helper to pass to a successor.
    int release_lock();
    static std::filesystem::path runtime_directory();

  private:
    void open_lock(const std::filesystem::path& directory);
    void start_listener();
    // The owner's answer, or empty when there was none.
    std::string notify_owner(const char* request);
    bool try_lock();
    std::filesystem::path socket_path_;
    int lock_{-1};
    int listener_{-1};
    bool owner_{}, hosted_owner_{};
    Outcome outcome_{Outcome::Running};
};
} // namespace framekeyboard
