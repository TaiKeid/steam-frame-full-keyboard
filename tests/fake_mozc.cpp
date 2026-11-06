// A stand-in for Mozc's server, framekeyboard-mozc, for japanese-tests: it
// answers on Mozc's socket as Mozc does, with as much of its protocol as
// the keyboard uses (src/protocol/commands.proto in Mozc). Every key typed
// converts to one segment, MOZC. The tests steer and watch it through files
// in its profile, $XDG_CONFIG_HOME/mozc:
//   fake-exit     present as it starts: it exits at once, as a broken one.
//   fake-expire   present at a request: its sessions are dropped first, as
//                 Mozc's Cleanup drops idle ones; the file is removed.
//   fake-pid      written: its process, for the tests to stop or kill.
//   fake-starts   a line per start: the path it was started as.
//   fake-creates  a line per session created.
//   fake-cleared  written when asked to clear what it learned.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void put_varint(std::string& out, std::uint64_t value) {
    do {
        const auto byte = static_cast<char>(value & 0x7F);
        value >>= 7;
        out += static_cast<char>(byte | (value ? 0x80 : 0));
    } while (value);
}
void put_number(std::string& out, int field, std::uint64_t value) {
    put_varint(out, static_cast<std::uint64_t>(field) << 3);
    put_varint(out, value);
}
void put_bytes(std::string& out, int field, std::string_view bytes) {
    put_varint(out, static_cast<std::uint64_t>(field) << 3 | 2);
    put_varint(out, bytes.size());
    out += bytes;
}
std::uint64_t get_varint(std::string_view data, std::size_t& at) {
    std::uint64_t value = 0;
    for (int shift = 0; at < data.size() && shift < 64; shift += 7) {
        const auto byte = static_cast<unsigned char>(data[at++]);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            break;
        }
    }
    return value;
}
// Calls `each(field, number, bytes)` for the varint and length fields.
template <typename Each> void fields(std::string_view data, Each each) {
    for (std::size_t at = 0; at < data.size();) {
        const auto tag = get_varint(data, at);
        const int field = static_cast<int>(tag >> 3);
        if ((tag & 7) == 0) {
            each(field, get_varint(data, at), std::string_view{});
        } else if ((tag & 7) == 2) {
            const auto size = get_varint(data, at);
            each(field, 0, data.substr(at, size));
            at += size;
        } else {
            return;
        }
    }
}
struct Request {
    std::uint64_t type{}, session{}, code{}, special{};
    std::string text;
};
Request parse(std::string_view data) {
    Request request;
    fields(data, [&](int field, std::uint64_t number, std::string_view bytes) {
        if (field == 1) {
            request.type = number;
        } else if (field == 2) {
            request.session = number;
        } else if (field == 3) { // KeyEvent
            fields(bytes, [&](int key, std::uint64_t value, std::string_view text) {
                if (key == 1) {
                    request.code = value;
                } else if (key == 3) {
                    request.special = value;
                } else if (key == 5) {
                    request.text = text;
                }
            });
        }
    });
    return request;
}
void note(const fs::path& file, const std::string& line, bool append = true) {
    std::ofstream(file, append ? std::ios::app : std::ios::trunc) << line << '\n';
}
// The preedit of a converted phrase: one segment, focused.
std::string converted(const std::string& reading) {
    std::string segment;
    put_number(segment, 3, 2); // HIGHLIGHT
    put_bytes(segment, 4, "MOZC");
    put_bytes(segment, 6, reading);
    std::string preedit;
    put_varint(preedit, 2 << 3 | 3); // Segment group
    preedit += segment;
    put_varint(preedit, 2 << 3 | 4);
    std::string word;
    put_number(word, 1, 0);
    put_bytes(word, 4, "MOZC");
    std::string words;
    put_bytes(words, 2, word);
    std::string out;
    put_bytes(out, 5, preedit);
    put_bytes(out, 14, words);
    return out;
}
} // namespace
int main(int, char** argv) {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (!config) {
        return 2;
    }
    const fs::path profile = fs::path(config) / "mozc";
    note(profile / "fake-starts", argv[0]);
    if (fs::exists(profile / "fake-exit")) {
        return 1;
    }
    const auto key = "fake" + std::to_string(getpid());
    std::string info;
    put_bytes(info, 1, key);
    std::ofstream(profile / ".session.ipc", std::ios::binary | std::ios::trunc) << info;
    const auto name = std::string(1, '\0') + "tmp/.mozc." + key + ".session";
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.data(), name.size());
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
             static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size())) != 0 ||
        listen(listener, 16) != 0) {
        return 3;
    }
    note(profile / "fake-pid", std::to_string(getpid()), false);
    std::set<std::uint64_t> sessions;
    std::string reading; // The composition, shared: the tests use one session at a time.
    std::uint64_t next = 0x5eed;
    for (;;) {
        const int connection = accept(listener, nullptr, nullptr);
        if (connection < 0) {
            continue;
        }
        std::string data;
        char buffer[4096];
        for (ssize_t n; (n = read(connection, buffer, sizeof buffer)) > 0;) {
            data.append(buffer, static_cast<std::size_t>(n));
        }
        if (fs::remove(profile / "fake-expire")) {
            sessions.clear();
        }
        const auto request = parse(data);
        std::string out;
        if (request.type == 1) { // CREATE_SESSION
            sessions.insert(next);
            put_number(out, 1, next++);
            note(profile / "fake-creates", "session");
        } else if (request.type == 2 || request.type == 3 || request.type == 5) {
            if (!sessions.contains(request.session)) {
                put_number(out, 1, 0);
                put_number(out, 11, 1); // SESSION_FAILURE, as Mozc answers an unknown session.
            } else {
                put_number(out, 1, request.session);
                if (request.type == 2) {
                    sessions.erase(request.session);
                } else if (request.type == 3 && request.special == 10) { // ESCAPE
                    reading.clear();
                } else if (request.type == 3 && request.special) {
                    out += converted(reading); // SPACE, and moving among the segments
                } else if (request.type == 3) {
                    reading += request.text.empty() ? std::string(1, static_cast<char>(request.code))
                                                    : request.text;
                }
            }
        } else {
            if (request.type == 11) { // CLEAR_USER_HISTORY
                note(profile / "fake-cleared", "history");
            }
            put_number(out, 1, 0);
        }
        (void)!write(connection, out.data(), out.size());
        close(connection);
    }
}
