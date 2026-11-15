#include "framekeyboard/theme_previews.hpp"
#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <glib.h>
#include <system_error>
#include <unistd.h>

#ifndef FRAMEKEYBOARD_VERSION
#define FRAMEKEYBOARD_VERSION "dev"
#endif

namespace framekeyboard {
namespace {
// A theme ID as a file name: anything but letters, digits, dot, dash and
// underscore (all a theme ID uses) becomes an underscore.
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
constexpr const char* index_name = "index.txt";
constexpr const char* index_title = "Full Keyboard theme pictures";
// Raised when the pictures are drawn differently within one version, as
// between development builds, so the index clears them.
constexpr int drawing_revision = 4;
// The index's second line: what drew the pictures. Any difference, such as
// another release, clears them all.
std::string drawn_by() {
    return std::string("drawn by ") + FRAMEKEYBOARD_VERSION + ", " +
           std::to_string(theme_preview_width) + "x" + std::to_string(theme_preview_height) +
           ", drawing " + std::to_string(drawing_revision);
}
// What a picture was drawn from: its theme's hash under this version's
// drawing, 16 hex digits. Each picture carries it, so one the index vouches
// for wrongly (a crash between saving it and writing the index, or another
// process's index) is drawn again instead of shown (R295).
std::string picture_stamp(const std::string& theme_hash) {
    const auto text = drawn_by() + "\n" + theme_hash;
    auto* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text.c_str(), -1);
    std::string stamp(hash, 16);
    g_free(hash);
    return stamp;
}
// A preview file: this header, then the surface's rows as Cairo keeps them
// (premultiplied ARGB in the machine's byte order; the cache never leaves it).
struct FileHeader {
    char magic[4];
    std::uint32_t width, height, stride;
    char stamp[16]; // picture_stamp
};
static_assert(sizeof(FileHeader) == 32, "no padding: the file holds the header as it is");
constexpr char file_magic[4] = {'F', 'K', 'T', '2'};
constexpr const char* file_extension = ".argb";
std::uint32_t preview_stride() {
    return static_cast<std::uint32_t>(
        cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, theme_preview_width));
}
// Null unless the file holds exactly one preview of the current size,
// drawn from what `stamp` says.
std::shared_ptr<cairo_surface_t> read_preview(const fs::path& file, const std::string& stamp) {
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
        header.stride == preview_stride() && stamp.size() == sizeof header.stamp &&
        std::memcmp(header.stamp, stamp.data(), sizeof header.stamp) == 0) {
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
bool write_preview(const fs::path& file, cairo_surface_t* image, const std::string& stamp) {
    cairo_surface_flush(image);
    FileHeader header{{file_magic[0], file_magic[1], file_magic[2], file_magic[3]},
                      static_cast<std::uint32_t>(cairo_image_surface_get_width(image)),
                      static_cast<std::uint32_t>(cairo_image_surface_get_height(image)),
                      static_cast<std::uint32_t>(cairo_image_surface_get_stride(image)),
                      {}};
    std::memcpy(header.stamp, stamp.data(), std::min(stamp.size(), sizeof header.stamp));
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
// Each process writes its own partial file and renames it into place, so two
// keyboards (the app and the simulator, say) never mix their bytes.
fs::path partial_of(const fs::path& file) {
    auto partial = file;
    partial += "." + std::to_string(getpid()) + ".partial";
    return partial;
}
} // namespace

std::string theme_preview_hash(const Theme& t) {
    std::string text = "theme picture";
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
    std::string result(hash, 16);
    g_free(hash);
    return result;
}

void ThemePreviewStore::sync(const std::vector<const Theme*>& themes) {
    if (!read_) {
        read_index();
    }
    std::set<std::string> ids;
    for (const auto* theme : themes) {
        ids.insert(theme->id);
    }
    if (ids == synced_) {
        return;
    }
    synced_ = ids;
    // Themes deleted since their pictures were saved.
    bool removed = false;
    for (auto entry = recorded_.begin(); entry != recorded_.end();) {
        if (ids.contains(entry->first)) {
            ++entry;
            continue;
        }
        std::error_code ignored;
        fs::remove(picture_file(entry->first), ignored);
        hashes_.erase(entry->first);
        entry = recorded_.erase(entry);
        removed = true;
    }
    if (removed) {
        write_index();
    }
}

std::shared_ptr<cairo_surface_t> ThemePreviewStore::load(const Theme& theme) {
    if (directory_.empty()) {
        return nullptr;
    }
    if (!read_) {
        read_index();
    }
    const auto found = recorded_.find(theme.id);
    if (found == recorded_.end() || found->second != hash(theme)) {
        return nullptr;
    }
    return read_preview(picture_file(theme.id), picture_stamp(found->second));
}

void ThemePreviewStore::save(const Theme& theme, cairo_surface_t* picture) {
    if (directory_.empty()) {
        return;
    }
    if (!read_) {
        read_index();
    }
    std::error_code error;
    fs::create_directories(directory_, error);
    const auto file = picture_file(theme.id);
    const auto partial = partial_of(file);
    bool saved = false;
    if (!error && write_preview(partial, picture, picture_stamp(hash(theme)))) {
        fs::rename(partial, file, error);
        saved = !error;
    }
    if (saved) {
        recorded_[theme.id] = hash(theme);
    } else {
        // A failure only means drawing it again next time.
        std::error_code ignored;
        fs::remove(partial, ignored);
        recorded_.erase(theme.id);
    }
    write_index();
}

const std::string& ThemePreviewStore::hash(const Theme& theme) {
    auto found = hashes_.find(theme.id);
    if (found == hashes_.end() || !(found->second.theme == theme)) {
        found = hashes_.insert_or_assign(theme.id, Hashed{theme, theme_preview_hash(theme)}).first;
    }
    return found->second.hash;
}

fs::path ThemePreviewStore::picture_file(const std::string& id) const {
    return directory_ / (file_stem(id) + file_extension);
}

void ThemePreviewStore::read_index() {
    read_ = true;
    recorded_.clear();
    if (directory_.empty()) {
        return;
    }
    std::ifstream in(directory_ / index_name);
    std::string title, drawn;
    if (std::getline(in, title) && std::getline(in, drawn) && title == index_title &&
        drawn == drawn_by()) {
        std::string id, hash;
        while (in >> id >> hash) {
            recorded_[id] = hash;
        }
        return;
    }
    // No index, or another version's: other code drew these pictures. Only
    // files of ours go (older builds saved PNGs); no exceptions, as a folder
    // that changes while it is read only stops the clean-up.
    std::error_code error;
    fs::directory_iterator entry(directory_, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        const auto extension = entry->path().extension();
        if (extension == file_extension || extension == ".png" || extension == ".partial") {
            std::error_code ignored;
            fs::remove(entry->path(), ignored);
        }
    }
    write_index();
}

void ThemePreviewStore::write_index() {
    if (directory_.empty()) {
        return;
    }
    std::error_code error;
    fs::create_directories(directory_, error);
    if (error) {
        return;
    }
    const auto file = directory_ / index_name;
    const auto partial = partial_of(file);
    std::ofstream out(partial, std::ios::trunc);
    out << index_title << '\n' << drawn_by() << '\n';
    for (const auto& [id, hash] : recorded_) {
        out << id << ' ' << hash << '\n';
    }
    out.close();
    if (out) {
        fs::rename(partial, file, error);
    }
    if (!out || error) {
        std::error_code ignored;
        fs::remove(partial, ignored);
    }
}

std::shared_ptr<cairo_surface_t> ThemePreviews::find(const Theme& theme) const {
    const auto found = previews_.find(theme.id);
    return found != previews_.end() && found->second.theme == theme ? found->second.image : nullptr;
}

bool ThemePreviews::fill_one(const std::vector<const Theme*>& themes, const Profiles& profiles,
                             ThemePreviewStore& store) {
    for (const auto* theme : themes) {
        if (const auto found = previews_.find(theme->id);
            found != previews_.end() && found->second.theme == *theme) {
            continue; // Current, or known not to draw.
        }
        auto image = store.load(*theme);
        if (!image) {
            image = draw(*theme, profiles);
            if (image) {
                store.save(*theme, image.get());
            }
        }
        previews_[theme->id] = {*theme, std::move(image)};
        return true;
    }
    return false;
}

void ThemePreviews::clear() {
    previews_.clear();
    keymap_.reset();
}

std::shared_ptr<cairo_surface_t> ThemePreviews::draw(const Theme& theme, const Profiles& profiles) {
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
    return drawn;
}
} // namespace framekeyboard
