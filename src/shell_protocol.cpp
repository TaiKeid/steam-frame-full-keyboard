#include "framekeyboard/shell_protocol.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace framekeyboard {
namespace {
// Operations that carry no id and get no response: low-latency pointer
// samples, sink acknowledgements and the handover's last message.
bool notification(const std::string& op) {
    return op == "pointer" || op == "ack" || op == "reject" || op == "quiesced";
}
// Events that go out without a subscription.
bool always_sent(const std::string& name) {
    return name == "ready" || name == "update" || name == "regions" || name == "commit" ||
           name == "key" || name == "saved";
}
} // namespace

bool adopt_control_descriptor(int fd) {
    if (fd < 3) {
        return false; // never the standard streams
    }
    int type = 0;
    socklen_t size = sizeof(type);
    sockaddr_storage address{};
    socklen_t address_size = sizeof(address);
    ucred peer{};
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        address.ss_family != AF_UNIX ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 || peer.uid != getuid()) {
        return false;
    }
    // A connected pair: the peer must still be there.
    sockaddr_storage remote{};
    socklen_t remote_size = sizeof(remote);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&remote), &remote_size) != 0) {
        return false;
    }
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0;
}

ShellConnection::ShellConnection(int fd, std::string version, std::uint64_t session,
                                 std::function<std::vector<std::string>()> features)
    : fd_(fd), version_(std::move(version)), session_(session), features_(std::move(features)) {
}

ShellConnection::~ShellConnection() {
    if (fd_ >= 0) {
        flush();
        close(fd_);
    }
}

int ShellConnection::release() {
    flush();
    return std::exchange(fd_, -1);
}

void ShellConnection::ready(double now) {
    ready_at_ = last_message_ = now;
    auto fields = json_ptr(json_object_new_object());
    auto protocols = json_object_new_array();
    json_object_array_add(protocols, json_object_new_int(shell_protocol_version));
    json_object_object_add(fields.get(), "protocol", protocols);
    json_object_object_add(fields.get(), "version", json_object_new_string(version_.c_str()));
    json_object_object_add(fields.get(), "session",
                           json_object_new_int64(static_cast<int64_t>(session_)));
    event("ready", std::move(fields));
    flush();
}

void ShellConnection::violate(const std::string& message) {
    if (violated_) {
        return;
    }
    // One error, then the connection ends, and with it the keyboard.
    auto object = json_ptr(json_object_new_object());
    json_object_object_add(object.get(), "id", nullptr);
    json_object_object_add(object.get(), "ok", json_object_new_boolean(false));
    json_object_object_add(object.get(), "error", json_object_new_string("bad_request"));
    json_object_object_add(object.get(), "message", json_object_new_string(message.c_str()));
    write_line(object.get());
    flush();
    violated_ = true;
}

std::vector<ShellRequest> ShellConnection::poll(double now) {
    std::vector<ShellRequest> requests;
    if (fd_ < 0 || closed_ || violated_ || !reading_) {
        return requests;
    }
    char buffer[16384];
    while (inbound_.size() < max_inbound) {
        const auto count = recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) {
            inbound_.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            closed_ = true;
        } else if (errno == EINTR) {
            continue;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            closed_ = true;
        }
        break;
    }
    int handled = 0;
    std::size_t start = 0;
    while (handled < max_messages_per_frame && !violated_) {
        const auto end = inbound_.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        const auto length = end - start;
        if (length > max_message) {
            violate("Message too long.");
            break;
        }
        const std::string line = inbound_.substr(start, length);
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        ++handled;
        json_tokener* tokener = json_tokener_new();
        auto parsed =
            json_ptr(json_tokener_parse_ex(tokener, line.c_str(), static_cast<int>(line.size())));
        const bool complete = json_tokener_get_error(tokener) == json_tokener_success &&
                              json_tokener_get_parse_end(tokener) == line.size();
        json_tokener_free(tokener);
        json_object* op = nullptr;
        if (!parsed || !complete || !json_object_is_type(parsed.get(), json_type_object) ||
            !json_object_object_get_ex(parsed.get(), "op", &op) ||
            !json_object_is_type(op, json_type_string)) {
            violate("Malformed message.");
            break;
        }
        ShellRequest request;
        request.op = json_object_get_string(op);
        json_object* id = nullptr;
        if (json_object_object_get_ex(parsed.get(), "id", &id) && id) {
            if (!json_object_is_type(id, json_type_int)) {
                violate("The id must be an integer.");
                break;
            }
            request.id = json_object_get_int64(id);
        } else if (!notification(request.op)) {
            violate("A request needs an id.");
            break;
        }
        request.body = std::move(parsed);
        // Any message renews the lease (docs/integration.md, Lease).
        last_message_ = now;
        if (lapsed_) {
            lapsed_ = false;
            renewed_ = true;
        }
        handle(request, now);
        if (!request.op.empty()) {
            requests.push_back(std::move(request));
        }
    }
    inbound_.erase(0, start);
    // A line still without its end that is already past the message limit.
    if (const auto end = inbound_.find('\n');
        (end == std::string::npos ? inbound_.size() : end) > max_message) {
        violate("Message too long.");
    }
    return requests;
}

// Session operations, answered here. Others pass through, or are refused
// before `hello`. A handled request leaves `op` empty.
void ShellConnection::handle(ShellRequest& request, double now) {
    (void)now;
    const auto id = request.id;
    json_object* body = request.body.get();
    auto get = [&](const char* key) -> json_object* {
        json_object* value = nullptr;
        return json_object_object_get_ex(body, key, &value) ? value : nullptr;
    };
    if (request.op == "hello") {
        if (hello_) {
            fail(*id, "bad_request", "Already greeted.");
        } else {
            auto* protocol = get("protocol");
            auto* lease = get("lease_ms");
            auto* client = get("client");
            if (!protocol || !json_object_is_type(protocol, json_type_int) ||
                json_object_get_int(protocol) != shell_protocol_version) {
                fail(*id, "incompatible", "Protocol 1 is the one supported.");
            } else if (lease &&
                       (!json_object_is_type(lease, json_type_int) || json_object_get_int(lease) < 500 ||
                        json_object_get_int(lease) > 5000)) {
                fail(*id, "bad_request", "lease_ms must be 500 to 5000.");
            } else {
                hello_ = true;
                lease_ms_ = lease ? json_object_get_int(lease) : 2000;
                if (client && json_object_is_type(client, json_type_string)) {
                    client_ = std::string(json_object_get_string(client)).substr(0, 64);
                }
                auto fields = json_ptr(json_object_new_object());
                json_object_object_add(fields.get(), "protocol",
                                       json_object_new_int(shell_protocol_version));
                json_object_object_add(fields.get(), "version",
                                       json_object_new_string(version_.c_str()));
                auto features = json_object_new_array();
                for (const auto& feature : features_ ? features_() : std::vector<std::string>{}) {
                    json_object_array_add(features, json_object_new_string(feature.c_str()));
                }
                json_object_object_add(fields.get(), "features", features);
                respond(*id, std::move(fields));
            }
        }
        request.op.clear();
        return;
    }
    if (!hello_) {
        if (id) {
            fail(*id, "no_hello", "Send hello first.");
        }
        request.op.clear();
        return;
    }
    if (request.op == "ping") {
        pings_.push_back(*id); // answered once this loop iteration completes
        request.op.clear();
    } else if (request.op == "subscribe") {
        auto* events = get("events");
        if (!events || !json_object_is_type(events, json_type_array)) {
            fail(*id, "bad_request", "events must be an array.");
        } else {
            subscriptions_.clear();
            for (std::size_t i = 0; i < json_object_array_length(events); ++i) {
                auto* name = json_object_array_get_idx(events, i);
                if (json_object_is_type(name, json_type_string)) {
                    subscriptions_.insert(json_object_get_string(name));
                }
            }
            respond(*id);
        }
        request.op.clear();
    }
}

void ShellConnection::write_line(json_object* object) {
    if (fd_ < 0 || violated_ || closed_) {
        return;
    }
    outbound_ +=
        json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    outbound_ += '\n';
    if (outbound_.size() - written_ > max_outbound) {
        // The shell stopped reading. Its responses cannot be kept forever.
        closed_ = true;
    }
}

void ShellConnection::respond(std::int64_t id, JsonPtr fields) {
    auto object = fields ? std::move(fields) : json_ptr(json_object_new_object());
    // `id` and `ok` lead the line for readability; json-c keeps insertion order.
    auto line = json_ptr(json_object_new_object());
    json_object_object_add(line.get(), "id", json_object_new_int64(id));
    json_object_object_add(line.get(), "ok", json_object_new_boolean(true));
    json_object_object_foreach(object.get(), key, value) {
        json_object_object_add(line.get(), key, json_object_get(value));
    }
    write_line(line.get());
}

void ShellConnection::fail(std::int64_t id, const std::string& code, const std::string& message) {
    auto line = json_ptr(json_object_new_object());
    json_object_object_add(line.get(), "id", json_object_new_int64(id));
    json_object_object_add(line.get(), "ok", json_object_new_boolean(false));
    json_object_object_add(line.get(), "error", json_object_new_string(code.c_str()));
    json_object_object_add(line.get(), "message", json_object_new_string(message.c_str()));
    write_line(line.get());
}

void ShellConnection::event(const std::string& name, JsonPtr fields) {
    if (!always_sent(name) && !subscriptions_.contains(name)) {
        return;
    }
    auto line = json_ptr(json_object_new_object());
    json_object_object_add(line.get(), "event", json_object_new_string(name.c_str()));
    if (fields) {
        json_object_object_foreach(fields.get(), key, value) {
            json_object_object_add(line.get(), key, json_object_get(value));
        }
    }
    write_line(line.get());
}

void ShellConnection::telemetry(const std::string& name, JsonPtr fields) {
    if (!always_sent(name) && !subscriptions_.contains(name)) {
        return;
    }
    telemetry_.insert_or_assign(name, std::move(fields));
}

void ShellConnection::frame_completed(double now) {
    (void)now;
    for (const auto id : pings_) {
        respond(id);
    }
    pings_.clear();
    if (outbound_.size() - written_ < telemetry_room) {
        for (auto& [name, fields] : telemetry_) {
            event(name, std::move(fields));
        }
        telemetry_.clear();
    }
    flush();
}

bool ShellConnection::flush() {
    while (fd_ >= 0 && written_ < outbound_.size()) {
        const auto count = send(fd_, outbound_.data() + written_, outbound_.size() - written_,
                                MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count > 0) {
            written_ += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                closed_ = true;
            }
            break;
        }
    }
    if (written_ == outbound_.size()) {
        outbound_.clear();
        written_ = 0;
    } else if (written_ > 64 * 1024) {
        outbound_.erase(0, written_);
        written_ = 0;
    }
    return outbound_.empty();
}

bool ShellConnection::take_lease_lapsed(double now) {
    if (!hello_ || lapsed_ || now - last_message_ <= lease_ms_ / 1000.0) {
        return false;
    }
    lapsed_ = true;
    return true;
}

ShellConnection::Status ShellConnection::status(double now) const {
    if (violated_) {
        return Status::Violated;
    }
    if (closed_ || fd_ < 0) {
        return Status::Closed;
    }
    if (!hello_ && ready_at_ >= 0 && now - ready_at_ > hello_deadline) {
        return Status::NoHello;
    }
    if (lapsed_ && now - last_message_ > lease_ms_ / 1000.0 + lease_grace) {
        return Status::LeaseGone;
    }
    return Status::Open;
}
} // namespace framekeyboard
