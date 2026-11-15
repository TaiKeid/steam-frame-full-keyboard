#include "framekeyboard/theme_previews.hpp"
#include "framekeyboard/panel.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <glib.h>
#include <system_error>
#include <unistd.h>

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
// A preview file: this header, then the surface's rows as Cairo keeps them
// (premultiplied ARGB in the machine's byte order; the cache never leaves it).
struct FileHeader {
    char magic[4];
    std::uint32_t width, height, stride;
};
constexpr char file_magic[4] = {'F', 'K', 'T', 'P'};
constexpr const char* file_extension = ".argb";
std::uint32_t preview_stride() {
    return static_cast<std::uint32_t>(
        cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, theme_preview_width));
}
// Null unless the file holds exactly one preview of the current size.
std::shared_ptr<cairo_surface_t> read_preview(const fs::path& file) {
    std::FILE* in = std::fopen(file.c_str(), "rb");
    if (!in) {
        return nullptr;
    }
    std::shared_ptr<cairo_surface_t> result;
    FileHeader header{};
    if (std::fread(&header, sizeof header, 1, in) == 1 &&
        std::memcmp(header.magic, file_magic, sizeof file_magic) == 0 &&
        header.width == static_cast<std::uint32_t>(theme_preview_width) &&
        header.height == static_cast<std::uint32_t>(theme_preview_height) &&
        header.stride == preview_stride()) {
        auto image = own(
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, theme_preview_width, theme_preview_height));
        const auto size = static_cast<std::size_t>(header.stride) * header.height;
        if (cairo_surface_status(image.get()) == CAIRO_STATUS_SUCCESS &&
            static_cast<std::uint32_t>(cairo_image_surface_get_stride(image.get())) == header.stride) {
            cairo_surface_flush(image.get());
            // All of it and nothing more: a cut or overlong file draws again.
            if (std::fread(cairo_image_surface_get_data(image.get()), 1, size, in) == size &&
                std::fgetc(in) == EOF) {
                cairo_surface_mark_dirty(image.get());
                result = std::move(image);
            }
        }
    }
    std::fclose(in);
    return result;
}
bool write_preview(const fs::path& file, cairo_surface_t* image) {
    cairo_surface_flush(image);
    const FileHeader header{{file_magic[0], file_magic[1], file_magic[2], file_magic[3]},
                            static_cast<std::uint32_t>(cairo_image_surface_get_width(image)),
                            static_cast<std::uint32_t>(cairo_image_surface_get_height(image)),
                            static_cast<std::uint32_t>(cairo_image_surface_get_stride(image))};
    std::FILE* out = std::fopen(file.c_str(), "wb");
    if (!out) {
        return false;
    }
    const auto size = static_cast<std::size_t>(header.stride) * header.height;
    bool written = std::fwrite(&header, sizeof header, 1, out) == 1 &&
                   std::fwrite(cairo_image_surface_get_data(image), 1, size, out) == size;
    written = std::fclose(out) == 0 && written;
    return written;
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
    for (const auto* c :
         {&t.surface, &t.top, &t.middle, &t.bottom, &t.side_top, &t.side_bottom, &t.legend, &t.hover,
          &t.latched, &t.disabled, &t.warning, &t.error, &t.alert, &t.latched_top, &t.latched_bottom,
          &t.latched_legend, &t.pressed_top, &t.pressed_bottom, &t.pressed_legend}) {
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
    return found != previews_.end() && found->second.theme == theme ? found->second.image : nullptr;
}

bool ThemePreviews::fill_one(const std::vector<const Theme*>& themes, const Profiles& profiles) {
    for (const auto* theme : themes) {
        if (const auto found = previews_.find(theme->id);
            found != previews_.end() && found->second.theme == *theme) {
            continue; // Current, or known not to draw.
        }
        previews_[theme->id] = {*theme, load_or_draw(*theme, theme_preview_key(*theme), profiles)};
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
    const auto file = directory_ / (stem + "~" + key + file_extension);
    if (!directory_.empty()) {
        if (auto loaded = read_preview(file)) {
            return loaded;
        }
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
    // Each process writes its own partial file, so two keyboards (the app
    // and the simulator, say) saving the same preview never mix their bytes.
    if (!directory_.empty()) {
        std::error_code error;
        fs::create_directories(directory_, error);
        auto partial = file;
        partial += "." + std::to_string(getpid()) + ".partial";
        bool saved = false;
        if (!error && write_preview(partial, drawn.get())) {
            fs::rename(partial, file, error);
            saved = !error;
        }
        if (!saved) {
            std::error_code ignored;
            fs::remove(partial, ignored);
        } else {
            // This theme's older previews, drawn from what it was before. No
            // exceptions: a folder that changes while it is read only stops
            // the clean-up.
            fs::directory_iterator entry(directory_, error);
            for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
                const auto name = entry->path().filename().string();
                std::error_code ignored;
                if (name.starts_with(stem + "~") && entry->path() != file) {
                    fs::remove(entry->path(), ignored);
                }
            }
        }
    }
    return drawn;
}
} // namespace framekeyboard
