#pragma once

#include <cstdint>
#include <functional>
#include <json-c/json.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace framekeyboard {
// The shell integration protocol (docs/integration.md). A shell launches the
// keyboard with `--hosted FD`; this class owns that inherited connection:
// framing, limits, the session (`ready`, `hello`, `ping`, `subscribe`) and
// the lease. What each operation does belongs to the VR loop and App.
using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;
inline JsonPtr json_ptr(json_object* object) {
    return {object, json_object_put};
}

// Exit statuses a shell can tell apart (docs/integration.md, Exit status).
namespace hosted_exit {
constexpr int invalid_descriptor = 10, quit_refused = 11, lock_taken = 12, shell_gone = 13;
}
constexpr int shell_protocol_version = 1;

// Whether `fd` is a connected AF_UNIX stream socket whose peer runs as this
// user. On success it becomes close-on-exec and nonblocking.
bool adopt_control_descriptor(int fd);

struct ShellRequest {
    std::optional<std::int64_t> id; // Notifications such as `pointer` carry none.
    std::string op;
    JsonPtr body{nullptr, json_object_put};
};

class ShellConnection {
  public:
    static constexpr std::size_t max_message = 64 * 1024, max_inbound = 128 * 1024;
    // Outbound bytes past which the shell is treated as not reading at all.
    static constexpr std::size_t max_outbound = 4 * 1024 * 1024;
    // Telemetry is queued only while less than this is waiting to be written.
    static constexpr std::size_t telemetry_room = 64 * 1024;
    static constexpr int max_messages_per_frame = 64;
    static constexpr double hello_deadline = 10, lease_grace = 30;

    // `features` lists what works now, for `hello`. `session` counts the
    // processes on this connection, update successors included.
    ShellConnection(int fd, std::string version, std::uint64_t session,
                    std::function<std::vector<std::string>()> features);
    ~ShellConnection();
    ShellConnection(const ShellConnection&) = delete;
    ShellConnection& operator=(const ShellConnection&) = delete;

    int fd() const { return fd_; }
    std::uint64_t session() const { return session_; }
    // Sends `ready`; the shell has `hello_deadline` seconds to answer.
    void ready(double now);
    // Reads what has arrived, at most max_messages_per_frame messages. The
    // session operations are answered here; every other request is returned
    // in order, after `hello`.
    std::vector<ShellRequest> poll(double now);
    // One response per request id.
    void respond(std::int64_t id, JsonPtr fields = json_ptr(nullptr));
    void fail(std::int64_t id, const std::string& code, const std::string& message);
    // Ordered events. `ready`, `update`, `regions` and sink events always go
    // out; the others only once subscribed.
    void event(const std::string& name, JsonPtr fields);
    // The latest value of a stream (geometry, levels, progress) replaces an
    // unsent one, and is dropped while the shell is slow to read.
    void telemetry(const std::string& name, JsonPtr fields);
    // End of a completed VR loop iteration: answers pings, queues telemetry
    // and writes what the socket takes.
    void frame_completed(double now);
    bool flush();
    bool outbound_empty() const { return outbound_.size() == written_; }

    enum class Status { Open, Closed, Violated, NoHello, LeaseGone };
    Status status(double now) const;
    bool hello_done() const { return hello_; }
    const std::string& client() const { return client_; }
    // The lease ran out with the connection still open: the shell may only
    // have stalled. Edge-triggered, once per lapse. It does not run while
    // reading is stopped.
    bool take_lease_lapsed(double now);
    // A message arrived after a lapse.
    bool take_lease_renewed() { return std::exchange(renewed_, false); }
    bool lease_lapsed() const { return lapsed_; }
    bool subscribed(const std::string& event) const { return subscriptions_.contains(event); }
    // Handover (docs/integration.md, Handover): reading stops, and the
    // descriptor passes to the update helper with what is left unread. None
    // of it blocks: the VR loop keeps running while the shell reads.
    void stop_reading() { reading_ = false; }
    // The lease restarts: the shell was not heard while reading stopped.
    void resume_reading(double now) {
        reading_ = true;
        last_message_ = now;
    }
    // Answers every ping received so far, as a completed loop iteration does.
    void answer_pings();
    // After `restarting`, lines wait here instead: the successor reports its
    // own state. They follow the queue if the handover is called off, and go
    // with this process otherwise.
    void hold() { holding_ = true; }
    void unhold();
    // The descriptor once every queued byte is written, as far as the
    // socket takes them now; -1, with the connection kept, until then.
    int release();

  private:
    void handle(ShellRequest& request, double now);
    void violate(const std::string& message);
    void write_line(json_object* object);
    int fd_;
    std::string version_;
    std::uint64_t session_;
    std::function<std::vector<std::string>()> features_;
    std::string inbound_, outbound_, held_;
    std::size_t written_{};
    bool hello_{}, closed_{}, violated_{}, reading_{true}, holding_{};
    double ready_at_{-1}, last_message_{};
    int lease_ms_{2000};
    bool lapsed_{}, renewed_{};
    std::string client_;
    std::set<std::string> subscriptions_;
    std::vector<std::int64_t> pings_;
    std::map<std::string, JsonPtr> telemetry_;
};
} // namespace framekeyboard
