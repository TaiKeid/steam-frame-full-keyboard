#include "framekeyboard/mozc.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace framekeyboard {
namespace {
// Protocol buffers' wire format, as much as Mozc's commands need
// (src/protocol/commands.proto in Mozc).
enum Wire { varint = 0, fixed64 = 1, length = 2, group_start = 3, group_end = 4, fixed32 = 5 };
void put_varint(std::string& out, std::uint64_t value) {
    do {
        auto byte = static_cast<char>(value & 0x7F);
        value >>= 7;
        out += static_cast<char>(byte | (value ? 0x80 : 0));
    } while (value);
}
void put_number(std::string& out, int field, std::uint64_t value) {
    put_varint(out, static_cast<std::uint64_t>(field) << 3 | varint);
    put_varint(out, value);
}
void put_bytes(std::string& out, int field, std::string_view bytes) {
    put_varint(out, static_cast<std::uint64_t>(field) << 3 | length);
    put_varint(out, bytes.size());
    out += bytes;
}
// One field of a message: a number, or bytes (a string, a message, a group's
// contents).
struct Field {
    int number{};
    std::uint64_t value{};
    std::string_view bytes;
};
std::uint64_t get_varint(std::string_view data, std::size_t& at) {
    std::uint64_t value = 0;
    for (int shift = 0; at < data.size() && shift < 64; shift += 7) {
        const auto byte = static_cast<unsigned char>(data[at++]);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            return value;
        }
    }
    throw std::runtime_error("Mozc sent a broken message");
}
// The fields at `at`, up to the end or the group `group` ends; `at` is left
// after that group's end tag, and `end` at its start.
std::vector<Field> fields(std::string_view data, std::size_t& at, int group, std::size_t& end) {
    std::vector<Field> result;
    while (at < data.size()) {
        const auto tag_start = at;
        const auto tag = get_varint(data, at);
        const auto number = static_cast<int>(tag >> 3);
        switch (tag & 7) {
        case varint:
            result.push_back({number, get_varint(data, at), {}});
            break;
        case fixed64:
            at += 8;
            break;
        case fixed32:
            at += 4;
            break;
        case length: {
            const auto size = get_varint(data, at);
            if (size > data.size() - at) {
                throw std::runtime_error("Mozc sent a broken message");
            }
            result.push_back({number, 0, data.substr(at, size)});
            at += size;
            break;
        }
        case group_start: {
            const auto begin = at;
            std::size_t contents_end = 0;
            fields(data, at, number, contents_end);
            result.push_back({number, 0, data.substr(begin, contents_end - begin)});
            break;
        }
        case group_end:
            if (number == group) {
                end = tag_start;
                return result;
            }
            throw std::runtime_error("Mozc sent a broken message");
        default:
            throw std::runtime_error("Mozc sent a broken message");
        }
    }
    if (group) {
        throw std::runtime_error("Mozc sent a broken message");
    }
    return result;
}
std::vector<Field> fields(std::string_view data) {
    std::size_t at = 0, end = 0;
    return fields(data, at, 0, end);
}
// commands.proto's numbers.
enum Command {
    create_session = 1,
    delete_session = 2,
    send_key = 3,
    send_command = 5,
    set_config = 7,
    sync_data = 8,
    clear_user_history = 11,
    clear_user_prediction = 12
};
enum SessionCommand { submit = 2, select_candidate = 3 };
enum Special { space = 4, left = 6, right = 7, escape = 10 };
constexpr int shift = 4;     // KeyEvent.ModifierKey.SHIFT
constexpr int highlight = 2; // Preedit.Segment.Annotation.HIGHLIGHT
struct Key {
    std::uint32_t code{};
    int special{};
    bool shifted{};
    std::string text; // Kana typed as such.
};
std::string input(Command type, std::uint64_t session, const Key* key) {
    std::string message;
    put_number(message, 1, type);
    if (session) {
        put_number(message, 2, session);
    }
    if (key) {
        std::string event;
        if (key->special) {
            put_number(event, 3, static_cast<std::uint64_t>(key->special));
        } else {
            put_number(event, 1, key->code);
        }
        if (key->shifted) {
            put_number(event, 4, shift);
        }
        if (!key->text.empty()) {
            put_bytes(event, 5, key->text);
        }
        put_bytes(message, 3, event);
    }
    return message;
}
// A session command: a candidate to select, or the phrase to submit.
std::string command_input(std::uint64_t session, SessionCommand type, std::int32_t id = 0) {
    std::string command;
    put_number(command, 1, type);
    if (type == select_candidate) {
        put_number(command, 2, static_cast<std::uint64_t>(static_cast<std::int64_t>(id)));
    }
    std::string message;
    put_number(message, 1, send_command);
    put_number(message, 2, session);
    put_bytes(message, 4, command);
    return message;
}
// Mozc's own character form rules (config_handler.cc), digits, Latin
// letters and % converted half-width as chat writes them (2日, 1000円,
// Tシャツ, 10%) instead of as last chosen, which starts full-width.
std::string config_input() {
    enum Form { half = 0, full = 1, last = 2 };
    const std::pair<std::string_view, std::pair<Form, Form>> rules[] = {
        {"ア", {full, full}},      {"A", {full, half}},          {"0", {full, half}},
        {"%", {full, half}},       {"(){}[]", {full, last}},     {".,", {full, last}},
        {"。、", {full, full}},    {"・「」", {full, full}},     {"\"'", {full, last}},
        {":;", {full, last}},      {"#&@$^_|`\\", {full, last}}, {"~", {full, last}},
        {"<>=+-/*", {full, last}}, {"?!", {full, last}}};
    std::string config;
    for (const auto& [group, forms] : rules) {
        std::string rule;
        put_bytes(rule, 1, group);
        put_number(rule, 2, forms.first);
        put_number(rule, 3, forms.second);
        put_bytes(config, 54, rule);
    }
    std::string message;
    put_number(message, 1, set_config);
    put_bytes(message, 5, config);
    return message;
}
// What Mozc shows after a command: the session, the preedit's segments with
// the focused one, and that one's candidates. A command for a session Mozc
// no longer has, as one it dropped after an hour unused, fails: its answer
// has no session and an error.
struct Output {
    std::uint64_t session{};
    bool failed = false;
    struct Piece {
        std::string key, value;
    };
    std::vector<Piece> segments;
    int focused = -1;
    std::vector<std::string> candidates;
    std::vector<std::int32_t> ids; // Each candidate's, to select it.
};
Output parse(std::string_view data) {
    Output result;
    for (const auto& field : fields(data)) {
        if (field.number == 1) {
            result.session = field.value;
        } else if (field.number == 11) { // error_code
            result.failed = field.value != 0;
        } else if (field.number == 5) { // Preedit
            for (const auto& part : fields(field.bytes)) {
                if (part.number != 2) { // Segment group
                    continue;
                }
                Output::Piece piece;
                for (const auto& item : fields(part.bytes)) {
                    if (item.number == 3 && item.value == highlight) {
                        result.focused = static_cast<int>(result.segments.size());
                    } else if (item.number == 4) {
                        piece.value = item.bytes;
                    } else if (item.number == 6) {
                        piece.key = item.bytes;
                    }
                }
                result.segments.push_back(std::move(piece));
            }
        } else if (field.number == 14) { // all_candidate_words
            for (const auto& word : fields(field.bytes)) {
                if (word.number == 2) {
                    std::int32_t id = 0;
                    std::string value;
                    for (const auto& item : fields(word.bytes)) {
                        if (item.number == 1) {
                            id = static_cast<std::int32_t>(item.value);
                        } else if (item.number == 4) {
                            value = item.bytes;
                        }
                    }
                    result.candidates.push_back(std::move(value));
                    result.ids.push_back(id);
                }
            }
        }
    }
    return result;
}
using Clock = std::chrono::steady_clock;
// The server, started once per process and kept running as long as it is:
// started again when it is found gone, unless it keeps dying soon after
// it starts (gone()), and stopped only with us. Its profile is `base`/mozc: Mozc keeps it in
// $XDG_CONFIG_HOME/mozc, which must exist.
struct Server {
    std::mutex lock;
    pid_t pid = -1;
    bool wanted = false; // Between start_mozc() and stop_mozc().
    // Started in bubblewrap (sandbox_command()) unless bubblewrap itself
    // fails; `confined` once the sandbox is seen to work.
    bool sandbox = true, sandboxed = false, confined = false;
    // When the running one started; quick deaths in a row, and when the
    // next start may be (gone()).
    Clock::time_point started, next_start;
    int deaths = 0;
    std::filesystem::path path, base;
    std::filesystem::path words; // Our words for its user dictionary (write_dictionary()).
    // Requests it didn't answer in time, with none answered since; Anthy
    // converts until `quiet_until` after each (MozcConverter::stalled()).
    int stalls = 0;
    Clock::time_point quiet_until;
    ~Server() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};
Server& server() {
    static Server instance;
    return instance;
}
// The server's socket, in the abstract namespace: "\0tmp/.mozc.<key>.session",
// the key from the profile's .session.ipc (ipc.proto, IPCPathInfo). A crashed
// server leaves its file, whose address then refuses at once.
std::string address(const std::filesystem::path& base) {
    std::ifstream file(base / "mozc" / ".session.ipc", std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    const auto data = std::move(content).str();
    for (const auto& field : fields(data)) {
        if (field.number == 1 && !field.bytes.empty()) {
            return std::string(1, '\0') + "tmp/.mozc." + std::string(field.bytes) + ".session";
        }
    }
    return {};
}
// How long Mozc may take to answer all of one conversion, resize or learned
// phrase. It answers in milliseconds, and its work is on the VR thread,
// whose watchdog gives way to Steam's keyboard after 0.7 s.
constexpr auto answer_time = std::chrono::milliseconds(300);
// After Mozc didn't answer in time, how long Anthy converts before it is
// asked again (MozcConverter::stalled()).
constexpr auto quiet_time = std::chrono::seconds(1);
// Mozc didn't answer in time: stuck, or still slow after it started.
struct Stalled : std::runtime_error {
    Stalled() : std::runtime_error("Mozc did not answer in time") {}
};
// Sets the socket's send or receive timeout to what is left until `deadline`.
void time_out(int socket_fd, int option, Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
    if (left.count() <= 0) {
        throw Stalled();
    }
    const timeval wait{static_cast<time_t>(left.count() / 1000000),
                       static_cast<suseconds_t>(left.count() % 1000000)};
    setsockopt(socket_fd, SOL_SOCKET, option, &wait, sizeof wait);
}
// One request and its answer by `deadline`: Mozc reads until the request's
// write side closes, then answers and closes.
std::string call(const std::string& at, const std::string& request, Clock::time_point deadline) {
    if (at.empty() || at.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Mozc is not running");
    }
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        throw std::runtime_error("Mozc is not running");
    }
    struct Close {
        int fd;
        ~Close() { close(fd); }
    } closer{socket_fd};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, at.data(), at.size());
    // A timeout fails with EAGAIN: a connection waits while the server's
    // backlog is full, as it is once it stopped taking them.
    const auto timed_out = [] { return errno == EAGAIN || errno == EWOULDBLOCK; };
    time_out(socket_fd, SO_SNDTIMEO, deadline);
    if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + at.size())) != 0) {
        if (timed_out()) {
            throw Stalled();
        }
        throw std::runtime_error("Mozc is not running");
    }
    for (std::size_t sent = 0; sent < request.size();) {
        time_out(socket_fd, SO_SNDTIMEO, deadline);
        const auto n = send(socket_fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n < 0 && timed_out()) {
                throw Stalled();
            }
            throw std::runtime_error("Mozc did not answer");
        }
        sent += static_cast<std::size_t>(n);
    }
    shutdown(socket_fd, SHUT_WR);
    std::string response;
    char buffer[65536];
    for (;;) {
        time_out(socket_fd, SO_RCVTIMEO, deadline);
        const auto n = recv(socket_fd, buffer, sizeof buffer, 0);
        if (n == 0) {
            return response;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            if (timed_out()) {
                throw Stalled();
            }
            throw std::runtime_error("Mozc did not answer");
        }
        response.append(buffer, static_cast<std::size_t>(n));
    }
}
// Where Mozc's own client expects its server (sandbox_command()).
constexpr const char* mozc_install_path = "/usr/lib/mozc/mozc_server";
// bubblewrap where distributions put it, or else from PATH; empty without
// it.
std::string bubblewrap() {
    if (access("/usr/bin/bwrap", X_OK) == 0) {
        return "/usr/bin/bwrap";
    }
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const auto end = rest.find(':');
            const auto directory = rest.substr(0, end);
            if (!directory.empty()) {
                const auto candidate = std::string(directory) + "/bwrap";
                if (access(candidate.c_str(), X_OK) == 0) {
                    return candidate;
                }
            }
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
    }
    return "";
}
// Mozc's session watchdog checks its own server through Mozc's client,
// which accepts only a server whose /proc/<pid>/exe is the path Mozc was
// built for, /usr/lib/mozc/mozc_server, and aborts it every three minutes
// otherwise. In bubblewrap the server is at that path, over a throwaway
// overlay of /usr/lib, with the system read-only but for its profile,
// `base`/mozc (make_profile()), not the rest of our config folder; the
// network stays shared, as its socket is an abstract one.
std::vector<std::string> sandbox_command(const std::string& bwrap, const Server& state) {
    const auto profile = (state.base / "mozc").string();
    std::vector<std::string> command{bwrap, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc"};
    command.insert(command.end(), {"--bind", profile, profile});
    command.insert(command.end(),
                   {"--overlay-src", "/usr/lib", "--tmp-overlay", "/usr/lib", "--dir", "/usr/lib/mozc",
                    "--ro-bind", state.path.string(), mozc_install_path});
    command.insert(command.end(), {"--die-with-parent", "--", mozc_install_path});
    return command;
}
// Our words (japanese/mozc-words.tsv, reading<TAB>word) as Mozc's user
// dictionary, user_dictionary.db in its profile: a UserDictionaryStorage
// (user_dictionary_storage.proto) holding one dictionary of nouns, which
// Mozc reads as it starts and segments sentences with (陽キャの集まり).
// Written again only when it differs; nothing without the file.
void write_dictionary(const Server& state) {
    std::ifstream in(state.words);
    if (state.words.empty() || !in) {
        return;
    }
    std::string entries, line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (line.empty() || line.front() == '#' || tab == std::string::npos) {
            continue;
        }
        const auto end = line.find('\t', tab + 1);
        std::string entry;
        put_bytes(entry, 1, std::string_view(line).substr(0, tab));
        put_bytes(
            entry, 2,
            std::string_view(line).substr(tab + 1, end == std::string::npos ? end : end - tab - 1));
        put_number(entry, 5, 1); // NOUN
        put_bytes(entries, 4, entry);
    }
    std::string dictionary;
    put_number(dictionary, 1, 1);
    put_bytes(dictionary, 3, "Full Keyboard");
    dictionary += entries;
    std::string storage;
    put_bytes(storage, 2, dictionary);
    const auto file = state.base / "mozc" / "user_dictionary.db";
    std::ifstream old(file, std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(old), {}) == storage) {
        return;
    }
    const auto part = file.string() + ".part";
    std::ofstream(part, std::ios::binary | std::ios::trunc) << storage;
    std::error_code ignored;
    std::filesystem::rename(part, file, ignored);
}
// The server's profile, which must be there for the sandbox to bind it.
void make_profile(const Server& state) {
    std::error_code error;
    std::filesystem::create_directories(state.base / "mozc", error);
    std::filesystem::permissions(state.base / "mozc", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
}
// Runs `arguments` with `variables` as its environment and nothing on its
// standard streams, in a process group of its own (kill_server()), going
// when we do.
pid_t launch(std::vector<std::string> arguments, std::vector<std::string> variables) {
    // Built before fork(): the child only calls async-signal-safe functions.
    std::vector<char*> argv, envp;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    for (auto& variable : variables) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setpgid(0, 0);
        const int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    if (child > 0) {
        setpgid(child, child); // As the child does, whichever runs first.
    }
    return child > 0 ? child : -1;
}
std::vector<std::string> environment() {
    std::vector<std::string> variables;
    for (char** variable = environ; *variable; ++variable) {
        variables.emplace_back(*variable);
    }
    return variables;
}
// Whether bubblewrap sets the server's sandbox up: it runs true there in
// place of the server, briefly.
bool bubblewrap_runs(const Server& state) {
    const auto bwrap = bubblewrap();
    if (bwrap.empty()) {
        return false;
    }
    make_profile(state);
    auto arguments = sandbox_command(bwrap, state);
    arguments.back() = "true";
    const pid_t child = launch(std::move(arguments), environment());
    int status = 0;
    for (int i = 0; child > 0 && i < 400; ++i) {
        if (waitpid(child, &status, WNOHANG) == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (child > 0) {
        kill(-child, SIGKILL);
        waitpid(child, nullptr, 0);
    }
    return false;
}
// A server going within this of its start went quickly; after `tries` in a
// row it is left stopped. The first is started again at once, the next
// ones after `backoff`, doubled each time.
constexpr auto quick_death = std::chrono::minutes(1);
constexpr int tries = 5;
constexpr auto backoff = std::chrono::milliseconds(500);
// The server went. One that ran a while is started again at once. One that
// went soon, as one that can't start or keeps crashing, is started again
// later each time, and after a few times not at all, until start_mozc()
// starts it anew: Anthy converts. A sandboxed one that never answered may
// be bubblewrap failing, which bubblewrap run alone shows; only then does
// the server run without it. Called with the server's lock held.
void gone(Server& state) {
    state.pid = -1;
    const auto now = Clock::now();
    if (state.sandboxed && !state.confined) {
        state.confined = bubblewrap_runs(state);
        if (!state.confined) {
            std::cerr << "Mozc's sandbox failed; starting it without bubblewrap.\n";
            state.sandbox = false;
            state.next_start = now;
            return;
        }
    }
    if (now - state.started >= quick_death) {
        state.deaths = 0;
        state.next_start = now;
        return;
    }
    if (++state.deaths == tries) {
        std::cerr << "Mozc keeps stopping; Japanese is converted by Anthy alone.\n";
    }
    state.next_start = state.deaths > 1 ? now + backoff * (1 << (state.deaths - 2)) : now;
}
// Whether the server runs; one found gone is reaped, and its going noted.
// Called with the server's lock held.
bool alive(Server& state) {
    if (state.pid <= 0) {
        return false;
    }
    if (waitpid(state.pid, nullptr, WNOHANG) == 0) {
        return true;
    }
    gone(state);
    return false;
}
// Kills the server, bubblewrap and Mozc, as one that is stuck can't be asked
// to go. Called with the server's lock held.
void kill_server(Server& state) {
    if (state.pid > 0) {
        kill(-state.pid, SIGKILL); // Its process group (launch()).
        kill(state.pid, SIGKILL);
        waitpid(state.pid, nullptr, 0);
        state.pid = -1;
    }
}
// Starts the server unless it is running or waits to be started again
// (gone()); it gets ready on its own. Called with the server's lock held.
void spawn(Server& state) {
    if (!state.wanted || alive(state) || state.deaths >= tries || Clock::now() < state.next_start) {
        return;
    }
    make_profile(state);
    write_dictionary(state);
    auto variables = environment();
    std::erase_if(variables, [](const std::string& text) {
        return text.starts_with("HOME=") || text.starts_with("XDG_CONFIG_HOME=");
    });
    variables.push_back("HOME=" + state.base.string());
    variables.push_back("XDG_CONFIG_HOME=" + state.base.string());
    const auto bwrap = state.sandbox ? bubblewrap() : std::string();
    state.pid = launch(bwrap.empty() ? std::vector<std::string>{state.path.string()}
                                     : sandbox_command(bwrap, state),
                       std::move(variables));
    state.sandboxed = !bwrap.empty();
    state.started = Clock::now();
}
// The server's address once it answers: waiting up to `patience` for it,
// starting it if need be, or not at all, as on the keyboard, whose tick
// starts it (App::sync_mozc()). Each try is answered by `deadline` or in
// answer_time.
std::string ready(std::chrono::milliseconds patience, Clock::time_point deadline = {}) {
    auto& state = server();
    std::lock_guard guard(state.lock);
    if (patience.count() > 0) {
        spawn(state);
    }
    const auto probe = input(create_session, 0, nullptr);
    const auto until = Clock::now() + patience;
    for (;;) {
        try {
            const auto by = deadline == Clock::time_point{} ? Clock::now() + answer_time : deadline;
            const auto at = address(state.base);
            const auto session = parse(call(at, probe, by)).session;
            call(at, input(delete_session, session, nullptr), by);
            state.confined = state.confined || state.sandboxed;
            return at;
        } catch (const std::exception&) {
            if (Clock::now() >= until) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
// A mark beside Mozc's profile that it learned something; the profile's
// own files are there from the start.
std::filesystem::path learned_mark(const std::filesystem::path& base) {
    return base / "mozc" / ".framekeyboard-learned";
}
void learned(bool on) {
    std::filesystem::path base;
    {
        auto& state = server();
        std::lock_guard guard(state.lock);
        base = state.base;
    }
    std::error_code ignored;
    if (on) {
        if (!std::filesystem::exists(learned_mark(base), ignored)) {
            std::ofstream(learned_mark(base)).close();
        }
    } else {
        std::filesystem::remove(learned_mark(base), ignored);
    }
}
// Mozc answered, but without our session: it dropped it, and the server is
// fine.
struct LostSession : std::runtime_error {
    LostSession() : std::runtime_error("Mozc lost the session") {}
};
class MozcConverter final : public JapaneseConverter {
  public:
    explicit MozcConverter(std::chrono::milliseconds patience) : patience_(patience) {}
    ~MozcConverter() override {
        try {
            if (session_) {
                call(address_, input(delete_session, session_, nullptr), Clock::now() + answer_time);
            }
        } catch (const std::exception&) {
        }
    }
    // A session Mozc dropped, or lost as it was started again, is made
    // again, and the conversion made in it.
    std::vector<Segment> convert(const std::string& reading) override {
        begin();
        try {
            const bool resumed = session_ != 0;
            try {
                return convert_once(reading);
            } catch (const Stalled&) {
                throw;
            } catch (const std::exception&) {
                if (!resumed) {
                    throw;
                }
                return convert_once(reading);
            }
        } catch (const Stalled&) {
            stalled();
            throw;
        }
    }
    std::vector<Segment> resize(int index, int delta) override {
        if (!session_ || index < 0 || index >= count_ || delta == 0) {
            return {};
        }
        begin();
        try {
            move_to(index);
            Output last = last_;
            for (int i = 0; i < std::abs(delta); ++i) {
                last = send({0, delta < 0 ? left : right, true, {}});
            }
            return collect(std::move(last));
        } catch (const Stalled&) {
            stalled();
            throw;
        }
    }
    // Mozc learns a phrase submitted to it: each segment's choice is
    // selected, then the phrase submitted, and its history saved. A choice
    // it didn't offer, as a usual form of ours, leaves the phrase unlearned.
    void learn(const std::vector<std::string>& chosen) override {
        if (!session_ || chosen.size() != static_cast<std::size_t>(count_) ||
            ids_.size() != chosen.size()) {
            return;
        }
        std::vector<std::int32_t> picks;
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            const auto found = std::find_if(ids_[i].begin(), ids_[i].end(), [&](const auto& candidate) {
                return candidate.first == chosen[i];
            });
            if (found == ids_[i].end()) {
                return;
            }
            picks.push_back(found->second);
        }
        begin();
        try {
            for (std::size_t i = 0; i < picks.size(); ++i) {
                move_to(static_cast<int>(i));
                request(command_input(session_, select_candidate, picks[i]));
            }
            request(command_input(session_, submit));
            count_ = 0;
            last_ = {};
            call(address_, input(sync_data, 0, nullptr), deadline_);
        } catch (const Stalled&) {
            stalled();
            throw;
        }
        learned(true);
    }
    bool weighs_context() const override { return true; }

  private:
    // Each conversion, resize or learned phrase has answer_time, after the
    // patience to wait for a server starting; none while Mozc rests after
    // one it didn't answer in time.
    void begin() {
        {
            auto& state = server();
            std::lock_guard guard(state.lock);
            if (Clock::now() < state.quiet_until) {
                throw std::runtime_error("Mozc is not answering");
            }
        }
        deadline_ = Clock::now() + patience_ + answer_time;
    }
    // Mozc didn't answer in time, and may still be at the request: the
    // session goes, and Anthy converts for quiet_time. A slow Mozc, as the
    // first conversion after it starts may find it while it reads its
    // dictionary in, has answered by then. A second request it doesn't
    // answer, with none answered between, means it's stuck: it is killed,
    // and started again (spawn()).
    void stalled() {
        session_ = 0;
        auto& state = server();
        std::lock_guard guard(state.lock);
        state.quiet_until = Clock::now() + quiet_time;
        if (++state.stalls >= 2) {
            state.stalls = 0;
            std::cerr << "Mozc stopped answering; starting it again.\n";
            kill_server(state);
            gone(state);
        }
    }
    std::vector<Segment> convert_once(const std::string& reading) {
        if (!session_) {
            // After a crash, the restarted server answers within a moment;
            // until it does, this throws once its patience runs out (at once
            // on the keyboard), and Anthy converts.
            address_ = ready(patience_, deadline_);
            call(address_, config_input(), deadline_); // Again after a restart: a new server.
            session_ = parse(call(address_, input(create_session, 0, nullptr), deadline_)).session;
            last_ = {};
            count_ = focus_ = 0;
            if (!session_) {
                throw std::runtime_error("Mozc made no session");
            }
        }
        reset();
        reading_ = reading;
        for (const char* p = reading.c_str(); *p; p = g_utf8_next_char(p)) {
            const std::string character(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
            Key key;
            if (character.size() == 1) {
                key.code = static_cast<unsigned char>(character[0]);
            } else {
                key.text = character;
            }
            send(key);
        }
        return collect(send({0, space, false, {}}));
    }
    // A failed request means the server is gone or stuck: the session goes
    // with it, and the keyboard's tick starts the server again. One
    // Mozc answers without the session also drops it, the segments and what
    // they offered with it, so resizing and learning wait for a conversion
    // in a new one.
    Output send(const Key& key) { return request(input(send_key, session_, &key)); }
    Output request(const std::string& message) {
        try {
            last_ = parse(call(address_, message, deadline_));
        } catch (const std::exception&) {
            session_ = 0;
            throw;
        }
        {
            auto& state = server();
            std::lock_guard guard(state.lock);
            state.stalls = 0; // It answers.
        }
        if (last_.failed || last_.session != session_) {
            session_ = 0;
            last_ = {};
            count_ = focus_ = 0;
            ids_.clear();
            throw LostSession();
        }
        if (last_.focused >= 0) {
            focus_ = last_.focused;
        }
        return last_;
    }
    void reset() {
        for (int i = 0; i < 3 && (count_ > 0 || !last_.segments.empty()); ++i) {
            send({0, escape, false, {}});
            count_ = static_cast<int>(last_.segments.size());
        }
        count_ = 0;
        focus_ = 0;
    }
    void move_to(int index) {
        while (focus_ < index) {
            const int before = focus_;
            send({0, right, false, {}});
            if (focus_ == before) {
                break;
            }
        }
        while (focus_ > index) {
            const int before = focus_;
            send({0, left, false, {}});
            if (focus_ == before) {
                break;
            }
        }
    }
    // Every segment with its candidates, visiting each to have them; the
    // focus goes back to the first.
    std::vector<Segment> collect(Output output) {
        count_ = static_cast<int>(output.segments.size());
        if (output.segments.empty()) {
            return {};
        }
        std::vector<Segment> result(output.segments.size());
        ids_.assign(output.segments.size(), {});
        // Each segment's reading as typed, cut by the lengths of Mozc's,
        // which may write symbols otherwise (? for ？).
        std::vector<std::string> readings;
        const char* at = reading_.c_str();
        for (const auto& piece : output.segments) {
            const char* end = at;
            for (glong i = g_utf8_strlen(piece.key.c_str(), -1); i > 0 && *end; --i) {
                end = g_utf8_next_char(end);
            }
            readings.emplace_back(at, static_cast<std::size_t>(end - at));
            at = end;
        }
        const bool typed = *at == '\0';
        for (int i = 0; i < count_; ++i) {
            if (i != focus_) {
                move_to(i);
                output = last_;
            }
            const auto& piece = output.segments[static_cast<std::size_t>(i)];
            auto& segment = result[static_cast<std::size_t>(i)];
            ids_[static_cast<std::size_t>(i)].clear();
            for (std::size_t c = 0; c < output.candidates.size() && c < output.ids.size(); ++c) {
                ids_[static_cast<std::size_t>(i)].emplace_back(output.candidates[c], output.ids[c]);
            }
            segment.reading = typed               ? readings[static_cast<std::size_t>(i)]
                              : piece.key.empty() ? piece.value
                                                  : piece.key;
            segment.candidates.push_back(piece.value);
            for (auto& text : output.candidates) {
                if (text != piece.value) {
                    segment.candidates.push_back(std::move(text));
                }
            }
        }
        move_to(0);
        return result;
    }
    std::chrono::milliseconds patience_;
    Clock::time_point deadline_; // The current conversion's (begin()).
    std::string address_, reading_;
    std::uint64_t session_{};
    // Each segment's candidates with Mozc's ids, to select them.
    std::vector<std::vector<std::pair<std::string, std::int32_t>>> ids_;
    Output last_;
    int count_{}, focus_{};
};
// Mozc while it answers, Anthy (or another fallback) meanwhile: a conversion
// Mozc can't make goes to the fallback, and the next one tries Mozc again.
// With patience, a failed one is tried once more, waiting that long for the
// server started again. Resizing and learning go to whichever made the last
// conversion.
class FallbackConverter final : public JapaneseConverter {
  public:
    FallbackConverter(std::function<std::unique_ptr<JapaneseConverter>()> fallback,
                      std::chrono::milliseconds patience)
        : make_fallback_(std::move(fallback)), mozc_(patience), tries_(patience.count() > 0 ? 2 : 1) {}
    std::vector<Segment> convert(const std::string& reading) override {
        for (int i = 0; i < tries_; ++i) {
            try {
                auto result = mozc_.convert(reading);
                // A new server not yet in hiragana mode, or another answer
                // without segments, is no conversion.
                if (!result.empty()) {
                    last_ = &mozc_;
                    return result;
                }
            } catch (const std::exception&) {
            }
        }
        last_ = &fallback();
        return last_->convert(reading);
    }
    std::vector<Segment> resize(int index, int delta) override {
        if (!last_) {
            return {};
        }
        try {
            return last_->resize(index, delta);
        } catch (const std::exception&) {
            return {}; // Mozc went in between: the segments stay as they are.
        }
    }
    void learn(const std::vector<std::string>& chosen) override {
        if (!last_) {
            return;
        }
        try {
            last_->learn(chosen);
        } catch (const std::exception&) {
            // Mozc went in between: the phrase stays unlearned.
        }
    }
    bool weighs_context() const override { return last_ && last_->weighs_context(); }
    bool fell_back() const override { return last_ && last_ == fallback_.get(); }
    // Words committed before, as Anthy learned them.
    std::vector<std::string> predict(const std::string& reading) override {
        return fallback().predict(reading);
    }
    void learn_prediction(const std::string& reading, const std::string& text) override {
        fallback().learn_prediction(reading, text);
    }

  private:
    JapaneseConverter& fallback() {
        if (!fallback_) {
            fallback_ = make_fallback_();
        }
        return *fallback_;
    }
    std::function<std::unique_ptr<JapaneseConverter>()> make_fallback_;
    std::unique_ptr<JapaneseConverter> fallback_;
    MozcConverter mozc_;
    int tries_;
    JapaneseConverter* last_{};
};
} // namespace
std::filesystem::path mozc_engine() {
    // Only beside the executable, never a lookalike on the search path:
    // engines/ in a development build, lib/framekeyboard/ in a release.
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
    for (const auto& path : {executable / "engines" / "framekeyboard-mozc",
                             executable.parent_path() / "lib/framekeyboard/framekeyboard-mozc"}) {
        if (!error && access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }
    return {};
}
void start_mozc(const std::filesystem::path& path, const std::filesystem::path& profile,
                const std::filesystem::path& words) {
    auto& state = server();
    std::lock_guard guard(state.lock);
    state.words = words;
    if (state.wanted && state.path == path && state.base == profile) {
        spawn(state); // Started again if it went.
        return;
    }
    if (state.pid > 0) {
        kill(state.pid, SIGTERM); // Another server or profile: this one goes.
        waitpid(state.pid, nullptr, 0);
        state.pid = -1;
    }
    state.path = path;
    state.base = profile;
    state.wanted = true;
    state.deaths = 0; // Tried anew, as when Japanese is added again.
    state.next_start = {};
    spawn(state);
}
bool wait_for_mozc(std::chrono::milliseconds patience) {
    try {
        ready(patience);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
void stop_mozc() {
    auto& state = server();
    std::lock_guard guard(state.lock);
    state.wanted = false;
    if (state.pid > 0) {
        try {
            // What it learned is saved, which may take longer than a
            // conversion and is rare: when Japanese is removed, or we end.
            call(address(state.base), input(sync_data, 0, nullptr),
                 Clock::now() + std::chrono::seconds(1));
        } catch (const Stalled&) {
            kill_server(state); // Stuck, it wouldn't go on being asked.
            return;
        } catch (const std::exception&) {
        }
        kill(state.pid, SIGTERM);
        waitpid(state.pid, nullptr, 0);
        state.pid = -1;
    }
}
bool mozc_running() {
    auto& state = server();
    std::lock_guard guard(state.lock);
    return alive(state);
}
bool mozc_learned(const std::filesystem::path& profile) {
    std::error_code error;
    return std::filesystem::exists(learned_mark(profile), error);
}
bool clear_mozc_learning(const std::filesystem::path& profile) {
    auto& state = server();
    std::lock_guard guard(state.lock); // No start meanwhile (spawn()).
    std::error_code error;
    if (state.base == profile && alive(state)) {
        try {
            const auto at = address(state.base);
            const auto by = Clock::now() + answer_time;
            call(at, input(clear_user_history, 0, nullptr), by);
            call(at, input(clear_user_prediction, 0, nullptr), by);
            call(at, input(sync_data, 0, nullptr), by);
            std::filesystem::remove(learned_mark(profile), error);
            return true;
        } catch (const std::exception&) {
            // Starting, or stuck: it goes, to start again with a new profile.
            kill_server(state);
        }
    }
    // Not running: its profile goes whole, and starts over on next start.
    std::filesystem::remove_all(profile / "mozc", error);
    if (error && std::filesystem::exists(profile / "mozc")) {
        std::ofstream(learned_mark(profile)).close(); // Whatever was left may hold some.
        return false;
    }
    return true;
}
std::unique_ptr<JapaneseConverter>
mozc_converter(std::function<std::unique_ptr<JapaneseConverter>()> fallback,
               std::chrono::milliseconds patience) {
    return std::make_unique<FallbackConverter>(std::move(fallback), patience);
}
} // namespace framekeyboard
