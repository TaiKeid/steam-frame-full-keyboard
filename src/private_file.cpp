#include "framekeyboard/private_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace framekeyboard {
namespace {
namespace fs = std::filesystem;
// Where a save writes before putting the file in its place.
fs::path temporary_of(const fs::path& file) {
    auto temporary = file;
    temporary += ".tmp";
    return temporary;
}
// Writes all of `data` to `fd`, retrying interrupted and partial writes.
bool write_all(int fd, const std::string& data) {
    for (std::size_t offset = 0; offset < data.size();) {
        const auto written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}
} // namespace

bool write_private_file(const fs::path& file, const std::string& text, bool make_folder) {
    std::error_code error;
    const auto folder = file.parent_path();
    if (make_folder && !folder.empty() && !fs::exists(folder, error) && mkdir(folder.c_str(), 0700) != 0 &&
        errno != EEXIST) {
        return false;
    }
    const auto temporary = temporary_of(file);
    unlink(temporary.c_str());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const bool written = write_all(fd, text);
    if (close(fd) != 0 || !written) {
        unlink(temporary.c_str());
        return false;
    }
    fs::rename(temporary, file, error);
    if (error) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}
void remove_private_file(const fs::path& file) {
    std::error_code ignored;
    fs::remove(file, ignored);
    fs::remove(temporary_of(file), ignored);
}
} // namespace framekeyboard
