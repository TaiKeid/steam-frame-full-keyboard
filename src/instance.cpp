#include "framekeyboard/instance.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace framekeyboard {
namespace {
constexpr char recenter_request[] = "recenter-v1";
constexpr char quit_request[] = "quit-v1";
constexpr char accepted[] = "accepted";
// A hosted keyboard's answer to a recenter: a shell manages it.
constexpr char hosted[] = "hosted";

void fail(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}
void require_private_directory(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) < 0) {
        fail("inspect runtime directory");
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 0077) != 0) {
        throw std::runtime_error("keyboard runtime directory must be private to this user");
    }
}
sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.size() >= sizeof(result.sun_path)) {
        throw std::runtime_error("runtime socket path is too long");
    }
    std::memcpy(result.sun_path, name.c_str(), name.size() + 1);
    return result;
}
// Keep each connection bounded: a local client cannot stall rendering indefinitely.
bool readable(int fd, int timeout_ms) {
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & POLLIN);
}
} // namespace
std::filesystem::path VrInstance::runtime_directory() {
    std::filesystem::path base = "/run/user/" + std::to_string(getuid());
    // Prefer the canonical login-session directory so SSH and a desktop launch
    // find each other even when their environments differ.
    if (!std::filesystem::exists(base)) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            throw std::runtime_error("no user runtime directory is available");
        }
        base = runtime;
    }
    require_private_directory(base);
    return base / "framekeyboard";
}
void VrInstance::open_lock(const std::filesystem::path& directory) {
    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
        fail("create runtime directory");
    }
    require_private_directory(directory);
    lock_ = open((directory / "vr.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) {
        fail("open keyboard instance lock");
    }
    struct stat info{};
    if (fstat(lock_, &info) < 0) {
        close(lock_);
        fail("inspect instance lock");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || info.st_nlink != 1) {
        close(lock_);
        throw std::runtime_error("invalid keyboard instance lock");
    }
}
bool VrInstance::try_lock() {
    if (flock(lock_, LOCK_EX | LOCK_NB) == 0) {
        owner_ = true;
        outcome_ = Outcome::Owner;
        start_listener();
        return true;
    }
    if (errno != EWOULDBLOCK) {
        fail("lock keyboard instance");
    }
    return false;
}
VrInstance::VrInstance(const std::filesystem::path& directory, Launch launch, bool hosted_owner)
    : socket_path_(directory / "vr.sock"), hosted_owner_(hosted_owner) {
    open_lock(directory);
    try {
        if (try_lock()) {
            return;
        }
        if (launch == Launch::Silent) {
            outcome_ = Outcome::Running;
            return;
        }
        if (launch == Launch::Hosted) {
            // One eviction only: an acknowledgement is not ownership, and a
            // second shell's launch must not keep evicting this one.
            if (notify_owner(quit_request) != accepted) {
                outcome_ = Outcome::QuitRefused;
                return;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                if (try_lock()) {
                    return;
                }
            }
            outcome_ = Outcome::LockTaken;
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            const auto answer = notify_owner(recenter_request);
            if (answer == accepted) {
                outcome_ = Outcome::Notified;
                return;
            }
            if (answer == hosted) {
                outcome_ = Outcome::ShellManaged;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            if (try_lock()) {
                return;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("the running keyboard did not answer the recenter request");
    } catch (...) {
        if (listener_ >= 0) {
            close(listener_);
        }
        if (owner_) {
            unlink(socket_path_.c_str());
        }
        close(lock_);
        throw;
    }
}
VrInstance::VrInstance(const std::filesystem::path& directory, int inherited_lock)
    : socket_path_(directory / "vr.sock"), lock_(inherited_lock), hosted_owner_(true) {
    require_private_directory(directory);
    struct stat info{};
    if (fstat(lock_, &info) < 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        fcntl(lock_, F_SETFD, FD_CLOEXEC) != 0) {
        close(lock_);
        throw std::runtime_error("invalid inherited instance lock");
    }
    // Already held through this open file description; this only confirms it.
    if (flock(lock_, LOCK_EX | LOCK_NB) != 0) {
        close(lock_);
        throw std::runtime_error("the inherited instance lock is not held");
    }
    owner_ = true;
    outcome_ = Outcome::Owner;
    start_listener();
}
void VrInstance::start_listener() {
    const auto endpoint = address(socket_path_);
    listener_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) {
        fail("create keyboard control socket");
    }
    // A crash leaves the path behind, but releases flock. Only the new lock owner
    // may remove that stale path; never unlink the lock file itself.
    if (unlink(socket_path_.c_str()) < 0 && errno != ENOENT) {
        fail("remove stale control socket");
    }
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        fail("bind keyboard control socket");
    }
    if (listen(listener_, 8) < 0) {
        fail("listen on keyboard control socket");
    }
}
std::string VrInstance::notify_owner(const char* request) {
    const auto endpoint = address(socket_path_);
    const int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (client < 0) {
        fail("create instance client");
    }
    std::string answer;
    const auto size = std::strlen(request) + 1;
    // A quit takes the owner a frame to consume; allow it a little longer.
    const int timeout = std::strcmp(request, quit_request) == 0 ? 2000 : 500;
    if (connect(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
        send(client, request, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size) &&
        readable(client, timeout)) {
        char message[64]{};
        const auto count = recv(client, message, sizeof(message) - 1, MSG_DONTWAIT);
        if (count > 0 && message[count - 1] == '\0') {
            answer = message;
        }
    }
    close(client);
    return answer;
}
VrInstance::Request VrInstance::poll() {
    if (!owner_) {
        return Request::None;
    }
    Request requested = Request::None;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            fail("accept instance request");
        }
        ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == getuid() &&
            readable(client, 20)) {
            char message[64]{};
            const auto count = recv(client, message, sizeof(message), MSG_DONTWAIT);
            auto is = [&](const char* expected) {
                return count == static_cast<ssize_t>(std::strlen(expected) + 1) &&
                       std::memcmp(message, expected, static_cast<std::size_t>(count)) == 0;
            };
            if (is(quit_request)) {
                // Acknowledged here; the loop then exits as Settings → Quit does.
                requested = Request::Quit;
                send(client, accepted, sizeof(accepted), MSG_NOSIGNAL);
            } else if (is(recenter_request)) {
                if (hosted_owner_) {
                    // A shell decides where and whether this keyboard shows;
                    // it hears that the user launched the keyboard.
                    send(client, hosted, sizeof(hosted), MSG_NOSIGNAL);
                    if (requested == Request::None) {
                        requested = Request::Launched;
                    }
                } else {
                    // Acknowledge only requests actually consumed by the render loop.
                    if (requested == Request::None) {
                        requested = Request::Recenter;
                    }
                    send(client, accepted, sizeof(accepted), MSG_NOSIGNAL);
                }
            }
        }
        close(client);
    }
    return requested;
}
int VrInstance::release_lock() {
    if (!owner_) {
        return -1;
    }
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
    unlink(socket_path_.c_str());
    owner_ = false;
    return std::exchange(lock_, -1);
}
VrInstance::~VrInstance() {
    if (listener_ >= 0) {
        close(listener_);
    }
    if (owner_) {
        unlink(socket_path_.c_str());
    }
    if (lock_ >= 0) {
        close(lock_);
    }
}
} // namespace framekeyboard
