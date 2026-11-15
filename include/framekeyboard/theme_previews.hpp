#pragma once
#include "config.hpp"
#include "input.hpp"

#include <cairo.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// The theme picker's card previews (render_theme_preview), kept on disk so a
// later picker loads them instead of drawing them again. Each file is named by
// the theme's ID and a hash of everything it is drawn from (theme_preview_key):
// an edited theme, or a release whose keys look different, draws it again and
// removes the old file. In memory they live only while the picker is open.
class ThemePreviews {
  public:
    // Where the files go; created when the first preview is saved.
    void set_directory(fs::path directory) { directory_ = std::move(directory); }
    // The preview of this theme as it is now; null until loaded or drawn, or
    // when it cannot be drawn.
    std::shared_ptr<cairo_surface_t> find(const Theme& theme) const;
    // Loads or draws the first of `themes` without a current preview and
    // keeps it; true if one was added. The picker calls it once a frame with
    // the cards in view first, so no frame draws more than one.
    bool fill_one(const std::vector<const Theme*>& themes, const Profiles& profiles);
    // Frees every preview, and the keymap drawing them needs.
    void clear();

  private:
    struct Entry {
        std::string key;
        std::shared_ptr<cairo_surface_t> image;
    };
    std::shared_ptr<cairo_surface_t> load_or_draw(const Theme& theme, const std::string& key,
                                                  const Profiles& profiles);
    fs::path directory_;
    std::map<std::string, Entry> previews_; // by theme ID
    std::unique_ptr<LanguageMap> keymap_;   // en-us, for the legends
};
// A hash of what a preview is drawn from: every value of the theme the keys
// use, the keyboard's version and the preview's size. Hex SHA-256.
std::string theme_preview_key(const Theme& theme);
} // namespace framekeyboard
