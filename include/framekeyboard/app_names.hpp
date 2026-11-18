#pragma once
#include "app_identity.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The names the shortcut bar shows for apps (docs/shortcut-bar-design.md,
// Apps): looked up when the picker's menu opens and when an app is assigned,
// on a thread of its own, never the VR thread. Each lookup reads at most a
// few small files, each cut off at a fixed size, and keeps only the name.
// Names and identities are never logged.
namespace framekeyboard {
// One app's name, or empty when none was found and the caller falls back
// (fallback_app_name). Runs on the lookup's thread. Tests and the simulator
// give their own; App without one never reads a file.
using AppNameSource = std::function<std::string(const AppIdentity&)>;

// What an identity gives by itself, until or unless its name is found: a
// program's file name (after the last / or \, as Wine's are Windows paths),
// the last dot-separated part of a Desktop app ID, and for a Steam app the
// focused program's name, else "App" and the ID.
std::string fallback_app_name(const AppIdentity& app, const std::string& program = {});
// A name as read from a file, made fit to show and keep: none for invalid
// UTF-8; control characters removed, spaces trimmed, at most
// shortcut_limits::app_name_characters code points.
std::string clean_app_name(std::string_view text);

// Where names are looked for, in order.
struct AppNameFolders {
    // Steam's own folders, each with steamapps/libraryfolders.vdf listing
    // the libraries whose steamapps/appmanifest_<id>.acf names a game.
    std::vector<std::filesystem::path> steam;
    // Folders of desktop files, <app ID>.desktop.
    std::vector<std::filesystem::path> applications;
};
// The standard folders under `home` and the system's, named explicitly: the
// keyboard's own XDG_DATA_DIRS is not the Desktop's. Flatpak's exports and
// Flatpak's Steam are included.
AppNameFolders system_app_name_folders(const std::filesystem::path& home);
// A Steam app's `name` from its app manifest, searching the libraries
// libraryfolders.vdf lists; a Desktop app's `Name=` from its desktop file. A
// program's name is its file name, so it reads nothing. Empty when not found.
std::string look_up_app_name(const AppIdentity& app, const AppNameFolders& folders);
// look_up_app_name in system_app_name_folders($HOME), for the headset.
AppNameSource system_app_names();
// A kept record of names found (R362): a Steam game's or a Desktop app's
// name, kept after its files are gone, such as a game streamed from a PC,
// whose manifest Steam keeps only while it streams. Programs and the PC's
// stream need none: their names come from the identity. Oldest found first,
// at most app_name_record_size; the oldest found goes first. Names are
// cleaned (clean_app_name) and never logged.
struct RecordedName {
    AppIdentity app;
    std::string name;
    bool operator==(const RecordedName&) const = default;
};
inline constexpr std::size_t app_name_record_size = 256;
// The record's file is read at most this far; a full one is far smaller.
inline constexpr std::size_t app_name_record_bytes = 256 * 1024;
// $XDG_CACHE_HOME/framekeyboard/app-names.json (an absolute one), else
// ~/.cache/framekeyboard/app-names.json; empty with neither.
std::filesystem::path app_name_record_path();
// Whether an identity's name is kept in the record.
bool recorded_kind(const AppIdentity& app);
// The record as its file holds it, and back. A file that isn't valid in
// every part, or holds more than the limit, reads as none.
std::string serialize_app_name_record(const std::vector<RecordedName>& names);
std::vector<RecordedName> parse_app_name_record(const std::string& text);
// Keeps a name found, as the newest; true when the record changed. Nothing
// for a kind not kept or a name that cleans to nothing.
bool record_app_name(std::vector<RecordedName>& names, const AppIdentity& app, std::string_view name);
// The name kept for an app; empty for none.
std::string recorded_app_name(const std::vector<RecordedName>& names, const AppIdentity& app);
// Each file read is cut off here: a name lies near the start.
inline constexpr std::size_t app_name_file_bytes = 32 * 1024;
// libraryfolders.vdf lists every installed game under each library before
// the next library's path.
inline constexpr std::size_t library_folders_bytes = 256 * 1024;

// Runs a source's lookups on a detached thread of its own, one at a time,
// so destroying this never waits for a slow read. Results are taken on a
// tick, as App takes a save's. Without a source every lookup answers at once
// with no name.
class AppNameLookup {
  public:
    struct Result {
        AppIdentity app;
        std::string name; // empty: none found
    };
    explicit AppNameLookup(AppNameSource source);
    ~AppNameLookup();
    AppNameLookup(const AppNameLookup&) = delete;
    AppNameLookup& operator=(const AppNameLookup&) = delete;
    // Asks for one app's name; a request already waiting or under way isn't repeated.
    void request(const AppIdentity& app);
    // A finished lookup, oldest first; none while there is none.
    std::optional<Result> take();
    // Shared with the thread; defined in app_names.cpp.
    struct State;

  private:
    std::shared_ptr<State> state_;
};
} // namespace framekeyboard
