#include "framekeyboard/stream_app.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace framekeyboard {
namespace fs = std::filesystem;

AppIdentity pc_stream_identity() {
    return {AppIdentity::Kind::Stream, pc_stream_id};
}
namespace {
// At most this much of a file under /proc: a process's name, or its
// arguments, which for the streaming client are a few hundred bytes.
constexpr std::streamsize comm_bytes = 64, cmdline_bytes = 64 * 1024;
std::optional<std::string> read_start(const fs::path& path, std::streamsize limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string data(static_cast<std::size_t>(limit), '\0');
    file.read(data.data(), limit);
    if (file.bad()) {
        return {};
    }
    data.resize(static_cast<std::size_t>(file.gcount()));
    return data;
}
bool digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}
} // namespace

std::optional<std::uint32_t> streamed_app_id(const std::vector<std::string>& arguments) {
    std::optional<std::uint32_t> found;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != "--appid") {
            continue;
        }
        if (found || i + 1 == arguments.size()) {
            return {}; // a second --appid, or one with nothing after it
        }
        const auto& value = arguments[i + 1];
        if (!digits(value) || value.front() == '0' || value.size() > 10) {
            return {};
        }
        const auto number = std::stoull(value);
        if (number > std::numeric_limits<std::uint32_t>::max()) {
            return {};
        }
        found = static_cast<std::uint32_t>(number);
    }
    return found;
}
AppIdentity stream_identity(const std::optional<std::vector<std::vector<std::string>>>& clients) {
    if (!clients || clients->size() != 1) {
        return pc_stream_identity();
    }
    if (const auto id = streamed_app_id(clients->front())) {
        return {AppIdentity::Kind::Steam, std::to_string(*id)};
    }
    return pc_stream_identity();
}
StreamClientSource system_stream_clients() {
    return []() -> std::optional<std::vector<std::vector<std::string>>> {
        std::vector<std::vector<std::string>> clients;
        std::error_code error;
        const auto user = getuid();
        for (fs::directory_iterator entry("/proc", error); !error && entry != fs::directory_iterator();
             entry.increment(error)) {
            const auto pid = entry->path().filename().string();
            if (!digits(pid)) {
                continue;
            }
            // The kernel cuts a process's name to 15 bytes; the arguments
            // below tell the whole program name.
            const auto comm = read_start(entry->path() / "comm", comm_bytes);
            if (!comm || comm->rfind("streaming_clien", 0) != 0) {
                continue;
            }
            struct stat owner {};
            if (stat(entry->path().c_str(), &owner) != 0 || owner.st_uid != user) {
                continue;
            }
            const auto cmdline = read_start(entry->path() / "cmdline", cmdline_bytes);
            if (!cmdline) {
                return {}; // seen, but its arguments couldn't be read
            }
            std::vector<std::string> arguments;
            for (std::size_t start = 0; start < cmdline->size();) {
                const auto end = std::min(cmdline->find('\0', start), cmdline->size());
                arguments.push_back(cmdline->substr(start, end - start));
                start = end + 1;
            }
            if (arguments.empty() || fs::path(arguments.front()).filename() != "streaming_client") {
                continue;
            }
            clients.push_back(std::move(arguments));
        }
        if (error) {
            return {}; // the list of processes couldn't be read to its end
        }
        return clients;
    };
}

struct StreamAppFinder::State {
    StreamClientSource source;
    std::mutex mutex;
    std::deque<Result> done;
};
StreamAppFinder::StreamAppFinder(StreamClientSource source) : state_(std::make_shared<State>()) {
    state_->source = std::move(source);
}
StreamAppFinder::~StreamAppFinder() = default;
void StreamAppFinder::request(std::uint64_t session) {
    if (!state_->source) {
        std::lock_guard lock(state_->mutex);
        state_->done.push_back({session, pc_stream_identity()}); // nothing to read
        return;
    }
    // One read per stream session, detached with its own share of the
    // state: a slow /proc never holds up the keyboard's exit. Only the
    // identity leaves the thread; the arguments end with it.
    std::thread([state = state_, session] {
        auto app = stream_identity(state->source());
        std::lock_guard lock(state->mutex);
        state->done.push_back({session, std::move(app)});
    }).detach();
}
std::optional<StreamAppFinder::Result> StreamAppFinder::take() {
    std::lock_guard lock(state_->mutex);
    if (state_->done.empty()) {
        return {};
    }
    auto result = std::move(state_->done.front());
    state_->done.pop_front();
    return result;
}
} // namespace framekeyboard
