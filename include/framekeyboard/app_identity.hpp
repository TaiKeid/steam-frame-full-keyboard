#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Which app has the keys, as far as it can be told (docs/shortcut-bar-design.md,
// Apps). Kept apart from the shortcut model so the input backends can report
// it without including that model. Identities name programs and apps the
// user runs: they are kept in memory, and in shortcuts.json for an app the
// user assigned, and never logged.
namespace framekeyboard {
struct AppIdentity {
    // Steam: a Steam app ID in decimal. Program: the focused process's
    // program file name. Desktop: the app ID SteamOS's Desktop reported.
    enum class Kind { Steam, Program, Desktop };
    Kind kind{Kind::Program};
    std::string id;
    bool operator==(const AppIdentity&) const = default;
};
// Whether shortcuts.json could hold this identity, by the same rules
// validate_shortcuts applies to a tie: not empty, at most app_id_bytes,
// valid UTF-8 without control characters; a Steam ID is a nonzero 32-bit
// number, and a program name has no folder part. An identity that fails is
// no identity, so an assign can never make the file invalid.
bool app_identity_valid(const AppIdentity& app);
// The app with the keys and Gamescope's focused window, read together so a
// new window never comes with the last one's app. No identity for Steam's
// dashboard and fields, Steam's own windows, nothing focused, or an app that
// can't be told. `program` is the focused process's program file name,
// beside a Steam identity too, where it is the name to fall back on.
struct FocusedApp {
    std::uint32_t window{};
    std::optional<AppIdentity> app;
    std::string program;
    bool operator==(const FocusedApp&) const = default;
};
} // namespace framekeyboard
