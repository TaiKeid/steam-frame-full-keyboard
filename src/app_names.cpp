#include "framekeyboard/app_names.hpp"
#include "framekeyboard/shortcuts.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <glib.h>
#include <mutex>
#include <thread>

namespace framekeyboard {
namespace fs = std::filesystem;
namespace {
// At most `limit` bytes from the start of a regular file; empty if unreadable.
std::string read_start(const fs::path& path, std::size_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    std::string data(limit, '\0');
    file.read(data.data(), static_cast<std::streamsize>(limit));
    data.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));
    return data;
}
// Steam's KeyValues text, as far as names need it: quoted strings, with a
// backslash escaping a backslash or a quote, and braces. Unquoted tokens and
// comments are skipped.
struct VdfToken {
    enum class Kind { Text, Open, Close } kind{};
    std::string text;
};
std::vector<VdfToken> vdf_tokens(std::string_view data) {
    std::vector<VdfToken> tokens;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const char c = data[i];
        if (c == '{' || c == '}') {
            tokens.push_back({c == '{' ? VdfToken::Kind::Open : VdfToken::Kind::Close, {}});
        } else if (c == '/' && i + 1 < data.size() && data[i + 1] == '/') {
            i = data.find('\n', i);
            if (i == std::string_view::npos) {
                break;
            }
        } else if (c == '"') {
            std::string text;
            bool closed = false;
            for (++i; i < data.size(); ++i) {
                if (data[i] == '\\' && i + 1 < data.size() &&
                    (data[i + 1] == '\\' || data[i + 1] == '"')) {
                    text += data[++i];
                } else if (data[i] == '"') {
                    closed = true;
                    break;
                } else {
                    text += data[i];
                }
            }
            if (!closed) {
                break; // Cut off inside a string: what came before stands.
            }
            tokens.push_back({VdfToken::Kind::Text, std::move(text)});
        }
    }
    return tokens;
}
// Each value whose key is `key`, `depth` braces deep.
std::vector<std::string> vdf_values(std::string_view data, std::string_view key, int depth) {
    std::vector<std::string> values;
    const auto tokens = vdf_tokens(data);
    int level = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        switch (tokens[i].kind) {
        case VdfToken::Kind::Open:
            ++level;
            break;
        case VdfToken::Kind::Close:
            --level;
            break;
        case VdfToken::Kind::Text:
            if (i + 1 < tokens.size() && tokens[i + 1].kind == VdfToken::Kind::Text) {
                if (level == depth && tokens[i].text == key) {
                    values.push_back(tokens[i + 1].text);
                }
                ++i; // A key and its value.
            }
            break;
        }
    }
    return values;
}
std::string steam_name(const std::string& id, const std::vector<fs::path>& roots) {
    // The Steam folder is a library itself, and the first libraryfolders.vdf
    // lists the rest: other drives, a card.
    std::vector<fs::path> libraries;
    for (const auto& root : roots) {
        libraries.push_back(root);
        const auto listed = read_start(root / "steamapps" / "libraryfolders.vdf", library_folders_bytes);
        for (const auto& path : vdf_values(listed, "path", 2)) {
            if (!path.empty()) {
                libraries.emplace_back(path);
            }
        }
    }
    for (const auto& library : libraries) {
        const auto manifest =
            read_start(library / "steamapps" / ("appmanifest_" + id + ".acf"), app_name_file_bytes);
        const auto names = vdf_values(manifest, "name", 1);
        if (!names.empty()) {
            if (auto name = clean_app_name(names.front()); !name.empty()) {
                return name;
            }
        }
    }
    return {};
}
// Name= of a desktop file's [Desktop Entry] group, not a translated Name[..]=.
std::string desktop_name(std::string_view data) {
    bool entry = false;
    std::size_t start = 0;
    while (start < data.size()) {
        auto end = data.find('\n', start);
        if (end == std::string_view::npos) {
            end = data.size();
        }
        auto line = data.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with('[')) {
            entry = line == "[Desktop Entry]";
            continue;
        }
        if (!entry || !line.starts_with("Name")) {
            continue;
        }
        auto rest = line.substr(4);
        while (!rest.empty() && rest.front() == ' ') {
            rest.remove_prefix(1);
        }
        if (!rest.starts_with('=')) {
            continue; // Name[de]= and NameX=
        }
        rest.remove_prefix(1);
        // The format's escapes; \n and \t become control characters, removed.
        std::string value;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == '\\' && i + 1 < rest.size()) {
                const char next = rest[++i];
                value += next == 's' ? ' ' : next == 'n' ? '\n' : next == 't' ? '\t' : next;
            } else {
                value += rest[i];
            }
        }
        return clean_app_name(value);
    }
    return {};
}
// A Desktop app ID used as a file name: no folder part, nothing hidden.
bool plain_file_name(const std::string& id) {
    return !id.empty() && id.front() != '.' && id.find('/') == std::string::npos &&
           id.find('\0') == std::string::npos;
}
} // namespace

std::string clean_app_name(std::string_view text) {
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return {};
    }
    std::string result;
    std::size_t characters = 0;
    for (const char* at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
        const gunichar c = g_utf8_get_char(at);
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) {
            continue;
        }
        if (c == ' ' && (result.empty() || result.back() == ' ')) {
            continue; // Leading spaces, and runs left where a control character was.
        }
        if (characters == shortcut_limits::app_name_characters) {
            break;
        }
        result.append(at, static_cast<std::size_t>(g_utf8_next_char(at) - at));
        ++characters;
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}
std::string fallback_app_name(const AppIdentity& app, const std::string& program) {
    switch (app.kind) {
    case AppIdentity::Kind::Program:
        return clean_app_name(app.id);
    case AppIdentity::Kind::Desktop: {
        const auto dot = app.id.rfind('.');
        const auto last =
            dot == std::string::npos || dot + 1 == app.id.size() ? app.id : app.id.substr(dot + 1);
        return clean_app_name(last);
    }
    case AppIdentity::Kind::Steam:
        if (auto name = clean_app_name(program); !name.empty()) {
            return name;
        }
        return "App " + app.id; // English until the interface's languages come
    }
    return clean_app_name(app.id);
}
AppNameFolders system_app_name_folders(const fs::path& home) {
    AppNameFolders folders;
    if (!home.empty()) {
        folders.steam = {home / ".local/share/Steam", home / ".steam/steam",
                         home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"};
        folders.applications = {home / ".local/share/applications",
                                home / ".local/share/flatpak/exports/share/applications"};
    }
    for (const char* folder : {"/var/lib/flatpak/exports/share/applications",
                               "/usr/local/share/applications", "/usr/share/applications"}) {
        folders.applications.emplace_back(folder);
    }
    return folders;
}
std::string look_up_app_name(const AppIdentity& app, const AppNameFolders& folders) {
    if (!app_identity_valid(app)) {
        return {};
    }
    switch (app.kind) {
    case AppIdentity::Kind::Steam:
        return steam_name(app.id, folders.steam);
    case AppIdentity::Kind::Desktop:
        if (!plain_file_name(app.id)) {
            return {};
        }
        for (const auto& folder : folders.applications) {
            const auto data = read_start(folder / (app.id + ".desktop"), app_name_file_bytes);
            if (auto name = desktop_name(data); !name.empty()) {
                return name;
            }
        }
        return {};
    case AppIdentity::Kind::Program:
        return {}; // Its file name is its name.
    }
    return {};
}
AppNameSource system_app_names() {
    const char* home = std::getenv("HOME");
    return [folders = system_app_name_folders(home ? home : "")](const AppIdentity& app) {
        return look_up_app_name(app, folders);
    };
}

struct AppNameLookup::State {
    AppNameSource source;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<AppIdentity> waiting;
    std::optional<AppIdentity> working;
    std::deque<Result> done;
    bool started{}, stop{};
};
namespace {
void look_up(const std::shared_ptr<AppNameLookup::State>& state) {
    std::unique_lock lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&] { return state->stop || !state->waiting.empty(); });
        if (state->stop) {
            return;
        }
        state->working = state->waiting.front();
        state->waiting.pop_front();
        const auto app = *state->working;
        lock.unlock();
        auto name = state->source(app);
        lock.lock();
        state->working.reset();
        state->done.push_back({app, clean_app_name(name)});
    }
}
} // namespace
AppNameLookup::AppNameLookup(AppNameSource source) : state_(std::make_shared<State>()) {
    state_->source = std::move(source);
}
AppNameLookup::~AppNameLookup() {
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    state_->wake.notify_all();
}
void AppNameLookup::request(const AppIdentity& app) {
    std::lock_guard lock(state_->mutex);
    if (!state_->source) {
        state_->done.push_back({app, {}}); // Nothing to read: the fallback stands.
        return;
    }
    if (state_->working == app ||
        std::find(state_->waiting.begin(), state_->waiting.end(), app) != state_->waiting.end()) {
        return;
    }
    state_->waiting.push_back(app);
    if (!state_->started) {
        // Started at the first lookup, and detached with its own share of
        // the state: a slow read never holds up the keyboard's exit.
        state_->started = true;
        std::thread([state = state_] { look_up(state); }).detach();
    }
    state_->wake.notify_one();
}
std::optional<AppNameLookup::Result> AppNameLookup::take() {
    std::lock_guard lock(state_->mutex);
    if (state_->done.empty()) {
        return {};
    }
    auto result = std::move(state_->done.front());
    state_->done.pop_front();
    return result;
}
} // namespace framekeyboard
