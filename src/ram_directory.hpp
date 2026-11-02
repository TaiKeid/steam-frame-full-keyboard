#pragma once

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <linux/magic.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>

namespace framekeyboard {
// Private initialization files must stay on RAM-backed storage. This helper
// owns only the random child it creates, never a supplied parent directory.
class RamDirectory {
  public:
    RamDirectory() : RamDirectory(default_parents()) {}
    explicit RamDirectory(const std::vector<std::filesystem::path>& parents) {
        for (const auto& parent : parents) {
            if (!parent.is_absolute()) {
                continue;
            }
            std::error_code error;
            const auto canonical = std::filesystem::canonical(parent, error);
            if (error) {
                continue;
            }
            Fd base{open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
            struct stat info{};
            if (base.value < 0 || fstat(base.value, &info) != 0 || !S_ISDIR(info.st_mode) ||
                !trusted_parent(info) || !ram_filesystem(base.value)) {
                continue;
            }
            auto temporary = (canonical / "framekeyboard-pyzy-XXXXXX").string();
            char* created = mkdtemp(temporary.data());
            if (!created) {
                continue;
            }
            // Nothing below this point can strand the newly created empty
            // child, including allocation failure while assigning path_.
            EmptyChild guard{created};
            if (chmod(created, 0700) != 0) {
                continue;
            }
            Fd child{open(created, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
            if (child.value < 0 || fstat(child.value, &info) != 0 || !S_ISDIR(info.st_mode) ||
                info.st_uid != getuid() || (info.st_mode & 0777) != 0700 ||
                !ram_filesystem(child.value)) {
                continue;
            }
            path_ = created;
            guard.path = nullptr;
            return;
        }
        throw std::runtime_error("Chinese input needs a private RAM initialization directory.");
    }
    ~RamDirectory() noexcept {
        if (path_.empty()) {
            return;
        }
        try {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        } catch (...) {
            // Allocation failure must not turn exception cleanup into another
            // exception. An empty child can still be removed without allocating.
            rmdir(path_.c_str());
        }
    }
    RamDirectory(const RamDirectory&) = delete;
    RamDirectory& operator=(const RamDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
    void remove() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            throw std::runtime_error("Could not remove Chinese input initialization files.");
        }
        path_.clear();
    }

  private:
    struct Fd {
        int value;
        explicit Fd(int descriptor) : value(descriptor) {}
        Fd(const Fd&) = delete;
        Fd& operator=(const Fd&) = delete;
        ~Fd() {
            if (value >= 0) {
                close(value);
            }
        }
    };
    struct EmptyChild {
        const char* path;
        explicit EmptyChild(const char* created) : path(created) {}
        EmptyChild(const EmptyChild&) = delete;
        EmptyChild& operator=(const EmptyChild&) = delete;
        ~EmptyChild() {
            if (path) {
                rmdir(path);
            }
        }
    };
    static bool ram_filesystem(int fd) {
        struct statfs filesystem{};
        return fstatfs(fd, &filesystem) == 0 &&
               (filesystem.f_type == TMPFS_MAGIC || filesystem.f_type == RAMFS_MAGIC);
    }
    static bool trusted_parent(const struct stat& info) {
        const bool private_owner = info.st_uid == getuid() && (info.st_mode & 0077) == 0;
        const bool trusted_sticky =
            (info.st_uid == 0 || info.st_uid == getuid()) && (info.st_mode & S_ISVTX) != 0;
        return private_owner || trusted_sticky;
    }
    static std::vector<std::filesystem::path> default_parents() {
        std::vector<std::filesystem::path> parents{std::filesystem::path("/run/user") /
                                                   std::to_string(getuid())};
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
            parents.emplace_back(runtime);
        }
        parents.emplace_back("/dev/shm");
        parents.emplace_back("/tmp");
        return parents;
    }
    std::filesystem::path path_;
};
} // namespace framekeyboard
