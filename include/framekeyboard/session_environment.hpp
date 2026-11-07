#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace framekeyboard {
// SteamOS's Desktop runs Plasma nested inside Steam's session, with its own
// runtime directory, session bus and X display. A keyboard started there would
// watch the Desktop's display for focus, so it could not type into the apps on
// Steam's, and could not reach the service manager. It takes these variables
// from that manager instead, which holds Steam's session values.
inline constexpr std::array<const char*, 9> session_variables{
    "XDG_RUNTIME_DIR",     "DBUS_SESSION_BUS_ADDRESS",  "DISPLAY",
    "XAUTHORITY",          "WAYLAND_DISPLAY",           "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP", "GAMESCOPE_WAYLAND_DISPLAY", "LIBEI_SOCKET"};
// A runtime directory inside the login one, /run/user/UID, as the Desktop's is.
bool nested_runtime_directory(const std::filesystem::path& runtime, uid_t uid);
// The KEY=VALUE strings in busctl --json=short output for the manager's
// Environment property, or nothing when the output is not that.
std::optional<std::vector<std::string>> parse_manager_environment(std::string_view json);
// Each session variable with the manager's value, or nothing to unset it.
// Empty unless the manager's runtime directory is the login one and it names
// a Gamescope display, as Steam's session does.
using SessionChanges = std::vector<std::pair<std::string, std::optional<std::string>>>;
SessionChanges session_changes(const std::vector<std::string>& manager, uid_t uid);
// Applies the changes when started in a nested session and the manager answers
// in time; returns whether it did. Call before any thread or helper starts.
bool adopt_session_environment();
} // namespace framekeyboard
