#include "framekeyboard/theme_previews.hpp"
#include "framekeyboard/panel.hpp"

#include <cstdio>
#include <glib.h>
#include <system_error>

#ifndef FRAMEKEYBOARD_VERSION
#define FRAMEKEYBOARD_VERSION "dev"
#endif

namespace framekeyboard {
namespace {
// A theme ID as a file name's first half: anything but letters, digits, dot,
// dash and underscore becomes an underscore, so "~", which separates it from
// the key, never appears in it.
std::string file_stem(const std::string& id) {
    std::string stem;
    for (const char c : id) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '-' || c == '_';
        stem += plain ? c : '_';
    }
    return stem.empty() ? std::string("_") : stem;
}
std::shared_ptr<cairo_surface_t> own(cairo_surface_t* surface) {
    return {surface, cairo_surface_destroy};
}
} // namespace

std::string theme_preview_key(const Theme& t) {
    std::string text = std::string("theme preview 3 ") + FRAMEKEYBOARD_VERSION + " " +
                       std::to_string(theme_preview_width) + "x" + std::to_string(theme_preview_height);
    char buffer[64];
    const auto add = [&](double value) {
        std::snprintf(buffer, sizeof buffer, " %.5f", value);
        text += buffer;
    };
    const auto color = [&](const Color& c) {
        add(c.r);
        add(c.g);
        add(c.b);
    };
    for (const auto* c : {&t.surface, &t.top, &t.middle, &t.bottom, &t.side_top, &t.side_bottom,
                          &t.legend, &t.hover, &t.latched, &t.disabled, &t.warning, &t.error, &t.alert,
                          &t.latched_top, &t.latched_bottom, &t.latched_legend, &t.pressed_legend}) {
        color(*c);
    }
    for (const auto* c : {&t.edge, &t.edge_hover, &t.pressed}) {
        text += *c ? " +" : " -";
        if (*c) {
            color(**c);
        }
    }
    text += " " + t.latched_finish;
    for (const double value : {t.surface_radius, t.radius, t.depth, t.travel, t.duration_ms, t.font_size,
                               t.small_font_size, t.padding}) {
        add(value);
    }
    auto* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text.c_str(), -1);
    std::string key(hash);
    g_free(hash);
    return key;
}

std::shared_ptr<cairo_surface_t> ThemePreviews::find(const Theme& theme) const {
    const auto found = previews_.find(theme.id);
    return found != previews_.end() && found->second.key == theme_preview_key(theme)
               ? found->second.image
               : nullptr;
}

bool ThemePreviews::fill_one(const std::vector<const Theme*>& themes, const Profiles& profiles) {
    for (const auto* theme : themes) {
        const auto key = theme_preview_key(*theme);
        if (const auto found = previews_.find(theme->id);
            found != previews_.end() && found->second.key == key) {
            continue; // Current, or known not to draw.
        }
        previews_[theme->id] = {key, load_or_draw(*theme, key, profiles)};
        return true;
    }
    return false;
}

void ThemePreviews::clear() {
    previews_.clear();
    keymap_.reset();
}

std::shared_ptr<cairo_surface_t> ThemePreviews::load_or_draw(const Theme& theme, const std::string& key,
                                                             const Profiles& profiles) {
    const auto stem = file_stem(theme.id);
    const auto file = directory_ / (stem + "~" + key + ".png");
    std::error_code error;
    if (!directory_.empty() && fs::is_regular_file(file, error)) {
        auto* loaded = cairo_image_surface_create_from_png(file.c_str());
        if (cairo_surface_status(loaded) == CAIRO_STATUS_SUCCESS &&
            cairo_image_surface_get_width(loaded) == theme_preview_width &&
            cairo_image_surface_get_height(loaded) == theme_preview_height) {
            return own(loaded);
        }
        cairo_surface_destroy(loaded); // Drawn again below, and saved over it.
    }
    const auto layout = profiles.layouts.find("en-us-full");
    const auto language = profiles.languages.find("en-us");
    if (layout == profiles.layouts.end() || language == profiles.languages.end()) {
        return nullptr;
    }
    if (!keymap_) {
        try {
            keymap_ = std::make_unique<LanguageMap>(language->second);
        } catch (const std::exception&) {
            return nullptr;
        }
    }
    auto drawn = own(render_theme_preview(theme, layout->second, language->second, *keymap_,
                                          theme_preview_width, theme_preview_height));
    if (cairo_surface_status(drawn.get()) != CAIRO_STATUS_SUCCESS) {
        return nullptr;
    }
    // Saved for the next picker; a failure only means drawing it again then.
    if (!directory_.empty()) {
        fs::create_directories(directory_, error);
        auto partial = file;
        partial += ".partial";
        if (!error && cairo_surface_write_to_png(drawn.get(), partial.c_str()) == CAIRO_STATUS_SUCCESS) {
            fs::rename(partial, file, error);
            // This theme's older previews, drawn from what it was before.
            for (const auto& entry : fs::directory_iterator(directory_, error)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with(stem + "~") && entry.path() != file) {
                    fs::remove(entry.path(), error);
                }
            }
        } else {
            fs::remove(partial, error);
        }
    }
    return drawn;
}
} // namespace framekeyboard
