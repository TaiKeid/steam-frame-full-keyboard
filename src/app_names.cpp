#include "framekeyboard/app_names.hpp"
#include "framekeyboard/shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <glib.h>
#include <json-c/json.h>
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
// Steam's own app cache, appcache/appinfo.vdf, for a game with no manifest here, such as one streamed
// from a PC (R362). Its format is Steam's private one, read only at the version known: a header (magic,
// universe, the string table's offset), entries of a stated size (app ID, size, 60 bytes, then binary
// VDF whose keys are indices into the string table), and the string table at the end. Anything else
// gives no name. Only the entry asked for is read; the rest are skipped by their size.
constexpr std::uint32_t appinfo_magic = 0x07564429;
constexpr std::uintmax_t appinfo_bytes = 64ull * 1024 * 1024;
class AppInfo {
  public:
    explicit AppInfo(const fs::path& path) : file_(path, std::ios::binary) {
        std::error_code error;
        size_ = fs::file_size(path, error);
        ok_ = !error && file_ && size_ >= 16 && size_ <= appinfo_bytes;
    }
    // `id`'s appinfo/common/name, or empty.
    std::string name(std::uint32_t id) {
        std::uint32_t magic = 0;
        std::int64_t table = 0;
        if (!ok_ || !read_at(0, magic) || magic != appinfo_magic || !read_at(8, table) || table < 16 ||
            static_cast<std::uintmax_t>(table) + 4 > size_ ||
            !find_keys(static_cast<std::uint64_t>(table))) {
            return {};
        }
        for (std::uint64_t at = 16; at + 8 <= static_cast<std::uint64_t>(table);) {
            std::uint32_t app = 0, bytes = 0;
            if (!read_at(at, app) || !read_at(at + 4, bytes) || app == 0) {
                return {};
            }
            const std::uint64_t end = at + 8 + bytes;
            if (bytes < 60 || end > static_cast<std::uint64_t>(table)) {
                return {}; // a size that points outside the entries
            }
            if (app == id) {
                std::string data(bytes - 60, '\0');
                file_.seekg(static_cast<std::streamoff>(at + 8 + 60));
                if (!file_.read(data.data(), static_cast<std::streamsize>(data.size()))) {
                    return {};
                }
                return find_name(data);
            }
            at = end;
        }
        return {};
    }

  private:
    template <class T> bool read_at(std::uint64_t at, T& value) {
        if (at + sizeof value > size_) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(at));
        return static_cast<bool>(file_.read(reinterpret_cast<char*>(&value), sizeof value));
    }
    // The string table's indices of the three keys, compared without case. Every spelling is kept,
    // a few at most, as the table may hold "name" and "Name" both, each used by other entries (R407).
    bool find_keys(std::uint64_t table) {
        std::uint32_t count = 0;
        if (!read_at(table, count)) {
            return false;
        }
        const char* wanted[] = {"appinfo", "common", "name"};
        std::string text;
        file_.seekg(static_cast<std::streamoff>(table + 4));
        for (std::uint32_t index = 0; index < count; ++index) {
            text.clear();
            for (int c; (c = file_.get()) != 0;) {
                if (c == EOF || text.size() > 64) {
                    if (c == EOF) {
                        return false;
                    }
                    continue; // longer than any key wanted: not one
                }
                text.push_back(static_cast<char>(std::tolower(c)));
            }
            for (int k = 0; k < 3; ++k) {
                if (text == wanted[k] && keys_[k].size() < key_spellings) {
                    keys_[k].push_back(static_cast<std::int32_t>(index));
                }
            }
        }
        count_ = count;
        return !keys_[0].empty() && !keys_[1].empty() && !keys_[2].empty();
    }
    bool is_key(int k, std::int32_t index) const {
        return std::find(keys_[k].begin(), keys_[k].end(), index) != keys_[k].end();
    }
    // appinfo / common / name in an entry's binary VDF: the string, or empty.
    std::string find_name(const std::string& data) {
        std::size_t at = 0;
        int depth = 0;   // maps open
        int matched = 0; // how many of them are the path's
        std::string found;
        while (at < data.size()) {
            const auto type = static_cast<unsigned char>(data[at++]);
            if (type == 0x08) {
                if (depth == 0) {
                    return {};
                }
                if (matched == depth) {
                    --matched;
                }
                --depth;
                continue;
            }
            std::int32_t key = 0;
            if (at + 4 > data.size()) {
                return {};
            }
            std::memcpy(&key, data.data() + at, 4);
            at += 4;
            if (key < 0 || static_cast<std::uint32_t>(key) >= count_) {
                return {}; // an index outside the table
            }
            std::size_t skip = 0;
            switch (type) {
            case 0x00:
                if (++depth > 64) {
                    return {};
                }
                if (matched == depth - 1 && matched < 2 && is_key(matched, key)) {
                    ++matched;
                }
                continue;
            case 0x01: {
                const auto end = data.find('\0', at);
                if (end == std::string::npos) {
                    return {};
                }
                if (matched == 2 && depth == 2 && is_key(2, key)) {
                    return data.substr(at, end - at);
                }
                at = end + 1;
                continue;
            }
            case 0x05: { // a wide string, ended by two NUL bytes
                std::size_t end = at;
                while (end + 1 < data.size() && (data[end] != '\0' || data[end + 1] != '\0')) {
                    end += 2;
                }
                if (end + 1 >= data.size()) {
                    return {};
                }
                at = end + 2;
                continue;
            }
            case 0x02:
            case 0x03:
            case 0x04:
            case 0x06:
                skip = 4;
                break;
            case 0x07:
            case 0x0A:
                skip = 8;
                break;
            default:
                return {}; // an unknown type: the rest can't be read
            }
            if (at + skip > data.size()) {
                return {};
            }
            at += skip;
        }
        return found;
    }
    std::ifstream file_;
    std::uintmax_t size_{};
    bool ok_{};
    // Spellings kept per key: the few case variants a table could hold.
    static constexpr std::size_t key_spellings = 8;
    std::vector<std::int32_t> keys_[3];
    std::uint32_t count_{};
};
// A Steam ID's name from the app cache under the first of `roots` that has one: none when the name
// isn't UTF-8 or is longer than a name may be.
std::string appinfo_name(const std::string& id, const std::vector<fs::path>& roots) {
    const auto number = std::stoull(id); // a valid Steam ID: digits within 32 bits
    for (const auto& root : roots) {
        std::error_code error;
        const auto path = root / "appcache" / "appinfo.vdf";
        if (!fs::is_regular_file(path, error)) {
            continue;
        }
        const auto name = AppInfo(path).name(static_cast<std::uint32_t>(number));
        if (name.empty() || !g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr) ||
            g_utf8_strlen(name.data(), static_cast<gssize>(name.size())) >
                static_cast<glong>(shortcut_limits::app_name_characters)) {
            return {};
        }
        return clean_app_name(name);
    }
    return {};
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
// `key`= of a desktop file's [Desktop Entry] group, unescaped, not a
// translated key[..]=; none when absent.
std::optional<std::string> desktop_value(std::string_view data, std::string_view key) {
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
        if (!entry || !line.starts_with(key)) {
            continue;
        }
        auto rest = line.substr(key.size());
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
        return value;
    }
    return {};
}
// Name= of a desktop file's [Desktop Entry] group, cleaned.
std::string desktop_name(std::string_view data) {
    const auto name = desktop_value(data, "Name");
    return name ? clean_app_name(*name) : std::string();
}
// The Name= of the first desktop file, in folder order and by file name
// within a folder, whose StartupWMClass= is `window_class` as written: a
// Flatpak's window reports a class its file isn't named by (R363). At most
// desktop_class_files files and desktop_class_bytes read in all.
std::string name_by_window_class(const std::string& window_class, const std::vector<fs::path>& folders) {
    std::size_t files = 0, bytes = 0;
    for (const auto& folder : folders) {
        std::vector<fs::path> paths;
        std::error_code error;
        for (fs::directory_iterator entry(folder, error); !error && entry != fs::directory_iterator();
             entry.increment(error)) {
            if (entry->path().extension() == ".desktop") {
                paths.push_back(entry->path());
            }
            if (paths.size() >= desktop_class_listed) {
                break; // a folder this large is cut off before sorting
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            if (files >= desktop_class_files || bytes >= desktop_class_bytes) {
                return {};
            }
            const auto data = read_start(path, app_name_file_bytes);
            ++files;
            bytes += data.size();
            if (desktop_value(data, "StartupWMClass") == window_class) {
                if (auto name = desktop_name(data); !name.empty()) {
                    return name;
                }
            }
        }
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
namespace {
// A program's own name: what follows the last folder separator, a Windows
// one too, as Wine names a game's process by its Windows path (R346).
std::string_view program_file(std::string_view program) {
    const auto separator = program.find_last_of("/\\");
    return separator == std::string_view::npos ? program : program.substr(separator + 1);
}
} // namespace
std::string fallback_app_name(const AppIdentity& app, const std::string& program) {
    switch (app.kind) {
    case AppIdentity::Kind::Program:
        return clean_app_name(program_file(app.id));
    case AppIdentity::Kind::Desktop: {
        const auto dot = app.id.rfind('.');
        const auto last =
            dot == std::string::npos || dot + 1 == app.id.size() ? app.id : app.id.substr(dot + 1);
        return clean_app_name(last);
    }
    case AppIdentity::Kind::Steam:
        if (auto name = clean_app_name(program_file(program)); !name.empty()) {
            return name;
        }
        return "App " + app.id; // English until the interface's languages come
    case AppIdentity::Kind::Stream:
        return "PC desktop stream"; // App shows its own words (shortcuts.app.stream)
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
        if (auto name = steam_name(app.id, folders.steam); !name.empty()) {
            return name;
        }
        // No manifest here, as for a game streamed from a PC: Steam's own app cache (R362).
        return appinfo_name(app.id, folders.steam);
    case AppIdentity::Kind::Desktop:
        if (!plain_file_name(app.id)) {
            return {};
        }
        {
            bool named_file = false;
            for (const auto& folder : folders.applications) {
                std::error_code error;
                named_file |= fs::is_regular_file(folder / (app.id + ".desktop"), error);
                const auto data = read_start(folder / (app.id + ".desktop"), app_name_file_bytes);
                if (auto name = desktop_name(data); !name.empty()) {
                    return name;
                }
            }
            // No file named by the ID: the one whose window class it is.
            return named_file ? std::string() : name_by_window_class(app.id, folders.applications);
        }
    case AppIdentity::Kind::Program:
        return {}; // Its file name is its name.
    case AppIdentity::Kind::Stream:
        return {}; // No file names it: App gives its own words.
    }
    return {};
}
fs::path app_name_record_path() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (cache && *cache == '/') { // a relative one is ignored, as the XDG spec asks
        return fs::path(cache) / "framekeyboard/app-names.json";
    }
    if (home && *home) {
        return fs::path(home) / ".cache/framekeyboard/app-names.json";
    }
    return {};
}
bool recorded_kind(const AppIdentity& app) {
    return app.kind == AppIdentity::Kind::Steam || app.kind == AppIdentity::Kind::Desktop;
}
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
const char* record_kind(AppIdentity::Kind kind) {
    return kind == AppIdentity::Kind::Steam ? "steam" : "desktop";
}
} // namespace
std::string serialize_app_name_record(const std::vector<RecordedName>& names) {
    Json document(json_object_new_object(), json_object_put);
    json_object_object_add(document.get(), "version", json_object_new_int(1));
    auto* list = json_object_new_array();
    for (const auto& entry : names) {
        auto* object = json_object_new_object();
        json_object_object_add(object, "kind", json_object_new_string(record_kind(entry.app.kind)));
        json_object_object_add(
            object, "id",
            json_object_new_string_len(entry.app.id.data(), static_cast<int>(entry.app.id.size())));
        json_object_object_add(
            object, "name",
            json_object_new_string_len(entry.name.data(), static_cast<int>(entry.name.size())));
        json_object_array_add(list, object);
    }
    json_object_object_add(document.get(), "names", list);
    return std::string(json_object_to_json_string_ext(
               document.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE)) +
           '\n';
}
std::vector<RecordedName> parse_app_name_record(const std::string& text) {
    if (text.size() > app_name_record_bytes) {
        return {};
    }
    Json document(json_tokener_parse(text.c_str()), json_object_put);
    json_object* version = nullptr;
    json_object* list = nullptr;
    if (!document || !json_object_is_type(document.get(), json_type_object) ||
        !json_object_object_get_ex(document.get(), "version", &version) ||
        !json_object_is_type(version, json_type_int) || json_object_get_int(version) != 1 ||
        !json_object_object_get_ex(document.get(), "names", &list) ||
        !json_object_is_type(list, json_type_array) ||
        json_object_array_length(list) > app_name_record_size) {
        return {};
    }
    std::vector<RecordedName> names;
    for (std::size_t i = 0; i < json_object_array_length(list); ++i) {
        auto* entry = json_object_array_get_idx(list, i);
        const auto text_of = [&](const char* key) -> std::optional<std::string> {
            json_object* value = nullptr;
            if (!json_object_is_type(entry, json_type_object) ||
                !json_object_object_get_ex(entry, key, &value) ||
                !json_object_is_type(value, json_type_string)) {
                return {};
            }
            return std::string(json_object_get_string(value),
                               static_cast<std::size_t>(json_object_get_string_len(value)));
        };
        const auto kind = text_of("kind"), id = text_of("id"), name = text_of("name");
        if (!kind || !id || !name || (*kind != "steam" && *kind != "desktop")) {
            return {};
        }
        RecordedName recorded{
            {*kind == "steam" ? AppIdentity::Kind::Steam : AppIdentity::Kind::Desktop, *id},
            clean_app_name(*name)};
        if (!app_identity_valid(recorded.app) || recorded.name.empty() ||
            std::any_of(names.begin(), names.end(),
                        [&](const RecordedName& kept) { return kept.app == recorded.app; })) {
            return {};
        }
        names.push_back(std::move(recorded));
    }
    return names;
}
bool record_app_name(std::vector<RecordedName>& names, const AppIdentity& app, std::string_view name) {
    auto cleaned = clean_app_name(name);
    if (!recorded_kind(app) || !app_identity_valid(app) || cleaned.empty()) {
        return false;
    }
    const auto kept = std::find_if(names.begin(), names.end(),
                                   [&](const RecordedName& entry) { return entry.app == app; });
    if (kept != names.end()) {
        if (kept->name == cleaned) {
            return false; // the same name found again: kept where it was
        }
        names.erase(kept);
    }
    names.push_back({app, std::move(cleaned)});
    if (names.size() > app_name_record_size) {
        names.erase(names.begin()); // the oldest found
    }
    return true;
}
std::string recorded_app_name(const std::vector<RecordedName>& names, const AppIdentity& app) {
    const auto kept = std::find_if(names.begin(), names.end(),
                                   [&](const RecordedName& entry) { return entry.app == app; });
    return kept == names.end() ? std::string() : kept->name;
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
        // the state: a slow read never holds up the keyboard's exit. Without
        // a thread none is found, and the next lookup tries again (R407).
        try {
            std::thread([state = state_] { look_up(state); }).detach();
            state_->started = true;
        } catch (const std::exception&) {
            state_->waiting.pop_back();
            state_->done.push_back({app, {}});
            return;
        }
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
