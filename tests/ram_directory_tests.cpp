#include "../src/ram_directory.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>

using framekeyboard::RamDirectory;
namespace fs = std::filesystem;
namespace {
bool fail_after_creation{}, fail_next{};
std::array<char, 4096> created_path{};
void require(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}
bool refused(const fs::path& parent) {
    try {
        RamDirectory child({parent});
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}
bool ram(const fs::path& path) {
    struct statfs info{};
    return statfs(path.c_str(), &info) == 0 &&
           (info.f_type == TMPFS_MAGIC || info.f_type == RAMFS_MAGIC);
}
struct NativeRamBase {
    fs::path path;
    NativeRamBase() {
        // Establish the environment independently: a broken RamDirectory
        // must fail this group, rather than turn every case into a skip.
        std::vector<fs::path> parents{"/dev/shm", "/tmp",
                                      fs::path("/run/user") / std::to_string(getuid())};
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
            parents.emplace_back(runtime);
        }
        for (const auto& parent : parents) {
            if (!parent.is_absolute() || !ram(parent)) {
                continue;
            }
            auto name = (parent / "framekeyboard-ram-test-XXXXXX").string();
            char* created = mkdtemp(name.data());
            if (!created) {
                continue;
            }
            try {
                path = created;
            } catch (...) {
                rmdir(created);
                throw;
            }
            return;
        }
    }
    ~NativeRamBase() {
        std::error_code ignored;
        if (!path.empty()) {
            fs::remove_all(path, ignored);
        }
    }
};
void check_private(const fs::path& path) {
    struct stat info{};
    require(lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == getuid() &&
                (info.st_mode & 0777) == 0700 && ram(path),
            "the child is an owner-only directory on RAM storage");
}
void creation_and_cleanup(const fs::path& base) {
    std::ofstream(base / "parent-marker") << "fixture";
    fs::path child_path;
    {
        RamDirectory child({base});
        child_path = child.path();
        check_private(child_path);
        require(child_path.parent_path() == fs::canonical(base),
                "the random child stays inside its base");
        std::ofstream(child_path / "initialization.db") << "synthetic metadata";
        fs::create_directory(child_path / "nested");
        std::ofstream(child_path / "nested" / "temporary") << "fixture";
    }
    require(!fs::exists(child_path) && fs::exists(base / "parent-marker"),
            "destruction removes contents and child, never its supplied base");
    try {
        RamDirectory child({base});
        child_path = child.path();
        std::ofstream(child_path / "initialization.db") << "fixture";
        throw std::runtime_error("injected initialization failure");
    } catch (const std::runtime_error&) {
    }
    require(!fs::exists(child_path) && fs::exists(base / "parent-marker"),
            "exception cleanup removes only the owned child");
    {
        RamDirectory child({base});
        child_path = child.path();
        child.remove();
        child.remove();
        require(child.path().empty() && !fs::exists(child_path),
                "explicit cleanup is immediate and idempotent");
    }
}
void rejected_parents(const fs::path& base) {
    require(chmod(base.c_str(), 0777) == 0, "make a deliberately unsafe RAM parent");
    require(refused(base), "a writable non-sticky parent is refused even on RAM");
    require(fs::exists(base / "parent-marker"), "rejection does not remove the supplied parent");
    require(chmod(base.c_str(), 01777) == 0, "make an owner-held sticky RAM parent");
    {
        RamDirectory child({base});
        check_private(child.path());
    }
    require(chmod(base.c_str(), 0700) == 0, "restore the private test base");
    require(refused("relative-parent"), "relative parents do not fall back to persistent storage");
    const auto current = fs::current_path();
    if (!ram(current)) {
        auto temporary = (current / ".ram-directory-test-XXXXXX").string();
        char* created = mkdtemp(temporary.data());
        require(created != nullptr, "create a private non-RAM parent");
        bool rejected = false;
        try {
            rejected = refused(created);
        } catch (...) {
            rmdir(created);
            throw;
        }
        rmdir(created);
        require(rejected, "a writable private persistent-filesystem parent is refused");
    } else {
        std::cout << "Persistent-filesystem case unavailable on this test filesystem.\n";
    }
}
void allocation_failure(const fs::path& base) {
    created_path[0] = '\0';
    fail_after_creation = true;
    bool failed = false;
    try {
        RamDirectory child({base});
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    fail_after_creation = fail_next = false;
    require(failed && created_path[0] && !fs::exists(created_path.data()),
            "allocation failure after mkdtemp cannot strand its new empty child");
    require(fs::exists(base / "parent-marker"), "allocation failure preserves the supplied base");
}
} // namespace

extern "C" char* __real_mkdtemp(char* pattern);
extern "C" char* __wrap_mkdtemp(char* pattern) {
    char* result = __real_mkdtemp(pattern);
    if (result && fail_after_creation) {
        std::snprintf(created_path.data(), created_path.size(), "%s", result);
        fail_after_creation = false;
        fail_next = true;
    }
    return result;
}
void* operator new(std::size_t size) {
    if (fail_next) {
        fail_next = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    try {
        NativeRamBase base;
        if (base.path.empty()) {
            std::cout << "RAM directory tests skipped: no usable RAM filesystem.\n";
            return 77;
        }
        {
            RamDirectory defaults;
            check_private(defaults.path());
        }
        creation_and_cleanup(base.path);
        rejected_parents(base.path);
        allocation_failure(base.path);
    } catch (const std::exception& error) {
        std::cerr << "RAM directory test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Private RAM initialization and cleanup passed.\n";
}
