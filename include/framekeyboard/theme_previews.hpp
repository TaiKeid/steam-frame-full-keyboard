#pragma once
#include "config.hpp"
#include "input.hpp"

#include <cairo.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace framekeyboard {
// The theme picker's pictures on disk, so a later picker loads them instead
// of drawing them again. The folder holds one picture per theme, named by its
// ID, and `index.txt`: the version that drew them, then each theme's ID and a
// short hash of the values its picture shows (theme_preview_hash).
//
// The first time the picker opens in a process, the index is read: one from
// another version, or none, clears every picture, as other code drew them.
// After that a picture is trusted while its theme's hash matches the index;
// an edited theme is drawn again. Each sync also removes the pictures of
// themes no longer present. Hashes are computed once per theme in a process,
// not per frame. App keeps this for the whole process; the pictures in memory
// are ThemePreviews', which go with the picker.
class ThemePreviewStore {
  public:
    // Where the pictures go; created when the first one is saved. Empty: none
    // are saved, and every picker draws them.
    void set_directory(fs::path directory) { directory_ = std::move(directory); }
    // As the picker opens, and after a Reload: reads the index the first
    // time, then removes the pictures of themes not in `themes`.
    void sync(const std::vector<const Theme*>& themes);
    // This theme's saved picture, if the index says it is current and the
    // file holds one; null otherwise.
    std::shared_ptr<cairo_surface_t> load(const Theme& theme);
    // Saves a picture just drawn and records its hash in the index.
    void save(const Theme& theme, cairo_surface_t* picture);

  private:
    const std::string& hash(const Theme& theme);
    fs::path picture_file(const std::string& id) const;
    void read_index();
    void write_index();
    fs::path directory_;
    bool read_{};
    std::map<std::string, std::string> recorded_; // theme ID -> hash, as in the index
    struct Hashed {
        Theme theme; // the values hashed: a different theme with this ID is hashed again
        std::string hash;
    };
    std::map<std::string, Hashed> hashes_; // this process's, by theme ID
    std::set<std::string> synced_;         // the IDs present at the last sync
};
// The pictures in memory while the picker is open and shown (App::ThemesPopup
// owns this): loaded from the store or drawn, one a frame.
class ThemePreviews {
  public:
    // The picture of this theme as it is now; null until loaded or drawn, or
    // when it cannot be drawn. A comparison of the theme, no hashing: the
    // picker asks for every card in view each time it builds a view.
    std::shared_ptr<cairo_surface_t> find(const Theme& theme) const;
    // Loads or draws the first of `themes` without a current picture, saving
    // a drawn one in `store`; true if one was added. The picker calls it once
    // a frame with the cards in view first, so no frame draws more than one.
    // Once every theme has its picture it only compares them.
    bool fill_one(const std::vector<const Theme*>& themes, const Profiles& profiles,
                  ThemePreviewStore& store);
    // Frees every picture, and the keymap drawing them needs.
    void clear();

  private:
    struct Entry {
        Theme theme; // as drawn: a different theme with this ID draws again
        std::shared_ptr<cairo_surface_t> image;
    };
    std::shared_ptr<cairo_surface_t> draw(const Theme& theme, const Profiles& profiles);
    std::map<std::string, Entry> previews_; // by theme ID
    std::unique_ptr<LanguageMap> keymap_;   // en-us, for the legends
};
// A short hash (16 hex digits of SHA-256) of the theme values a picture shows.
// The version and the picture's size are the index's first lines instead.
std::string theme_preview_hash(const Theme& theme);
} // namespace framekeyboard
