#include "framekeyboard/emoji_glyphs.hpp"

#include <algorithm>
#include <hb.h>
#include <optional>
#include <pango/pangocairo.h>
#include <utility>

namespace framekeyboard {
namespace {
constexpr const char* emoji_family = "Noto Color Emoji,emoji";

// The font's glyph for a whole emoji sequence, if it draws it as one image.
// A sequence the font does not know falls apart into several glyphs (a
// family into its people) or has a missing glyph; selectors and joiners
// shape to nothing visible.
std::optional<hb_codepoint_t> single_glyph(hb_font_t* font, hb_buffer_t* buffer,
                                           const std::string& text) {
    hb_buffer_clear_contents(buffer);
    hb_buffer_add_utf8(buffer, text.c_str(), static_cast<int>(text.size()), 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    const auto* info = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* position = hb_buffer_get_glyph_positions(buffer, &count);
    std::optional<hb_codepoint_t> glyph;
    for (unsigned i = 0; i < count; ++i) {
        if (info[i].codepoint == 0) {
            return {};
        }
        if (position[i].x_advance > 0) {
            if (glyph) {
                return {};
            }
            glyph = info[i].codepoint;
        }
    }
    return glyph;
}
} // namespace

// One emoji font at one size. Its Pango, HarfBuzz and Cairo objects belong to
// the worker thread alone, so no lookup is shared with painting.
struct EmojiGlyphs::Font {
    PangoFontMap* map{pango_cairo_font_map_new()};
    PangoContext* context{pango_font_map_create_context(map)};
    PangoFont* font{};
    hb_buffer_t* buffer{hb_buffer_create()};
    int size{};
    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    ~Font() {
        if (font) {
            g_object_unref(font);
        }
        hb_buffer_destroy(buffer);
        g_object_unref(context);
        g_object_unref(map);
    }
    bool load(int pixels) {
        if (font && size == pixels) {
            return true;
        }
        if (font) {
            g_object_unref(font);
        }
        auto* description = pango_font_description_from_string(emoji_family);
        pango_font_description_set_absolute_size(description, pixels * PANGO_SCALE);
        font = pango_font_map_load_font(map, context, description);
        pango_font_description_free(description);
        size = pixels;
        return font != nullptr;
    }
    std::optional<hb_codepoint_t> glyph(const std::string& text) const {
        return single_glyph(pango_font_get_hb_font(font), buffer, text);
    }
    // The glyph centered and fitted, since emoji fonts draw beyond their em box.
    cairo_surface_t* draw(hb_codepoint_t glyph) const {
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        auto* scaled = pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(font));
        cairo_glyph_t g{glyph, 0, 0};
        cairo_text_extents_t extents{};
        cairo_scaled_font_glyph_extents(scaled, &g, 1, &extents);
        if (extents.width > 0 && extents.height > 0) {
            auto* cr = cairo_create(surface);
            const double scale = std::min(1.0, size / std::max(extents.width, extents.height));
            cairo_translate(cr, size / 2.0, size / 2.0);
            cairo_scale(cr, scale, scale);
            cairo_translate(cr, -(extents.x_bearing + extents.width / 2),
                            -(extents.y_bearing + extents.height / 2));
            cairo_set_scaled_font(cr, scaled);
            cairo_show_glyphs(cr, &g, 1);
            cairo_destroy(cr);
        }
        cairo_surface_flush(surface);
        return surface;
    }
};

EmojiGlyphs::EmojiGlyphs() : worker_([this] { run(); }) {
}
EmojiGlyphs::~EmojiGlyphs() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    for (auto& [text, surface] : images_) {
        cairo_surface_destroy(surface);
    }
}
void EmojiGlyphs::check(std::vector<std::string> texts) {
    {
        std::lock_guard lock(mutex_);
        if (check_requested_) {
            return;
        }
        check_requested_ = true;
        to_check_ = std::move(texts);
    }
    wake_.notify_all();
}
bool EmojiGlyphs::checked() const {
    std::lock_guard lock(mutex_);
    return checked_;
}
bool EmojiGlyphs::renders(const std::string& text) const {
    std::lock_guard lock(mutex_);
    return renders_.contains(text);
}
void EmojiGlyphs::request(int size, const std::vector<std::string>& texts) {
    {
        std::lock_guard lock(mutex_);
        const auto keep = std::min(texts.size(), capacity(size));
        queue_.assign(texts.begin(),
                      texts.begin() + static_cast<std::ptrdiff_t>(std::min(keep, prefetch)));
        next_ = 0;
        // Earlier images stay while they are among the nearest that fit: move
        // those across, without copying any text, and free the rest. Only this
        // thread removes images, so the panel never draws a freed one.
        decltype(images_) kept;
        if (size == size_) {
            for (std::size_t i = 0; i < keep && !images_.empty(); ++i) {
                if (auto node = images_.extract(texts[i])) {
                    kept.insert(std::move(node));
                }
            }
        }
        for (auto& [text, surface] : images_) {
            cairo_surface_destroy(surface);
        }
        images_ = std::move(kept);
        size_ = size;
    }
    wake_.notify_all();
}
void EmojiGlyphs::release() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    next_ = 0;
    for (auto& [text, surface] : images_) {
        cairo_surface_destroy(surface);
    }
    images_.clear();
    fresh_ = false; // Nothing left to show.
}
cairo_surface_t* EmojiGlyphs::image(const std::string& text) const {
    std::lock_guard lock(mutex_);
    const auto found = images_.find(text);
    return found == images_.end() ? nullptr : found->second;
}
bool EmojiGlyphs::idle() const {
    std::lock_guard lock(mutex_);
    return (!check_requested_ || checked_) && next_ >= queue_.size() && !drawing_;
}
bool EmojiGlyphs::take_ready() {
    std::lock_guard lock(mutex_);
    const bool fresh = std::exchange(fresh_, false);
    generation_ += fresh;
    return fresh;
}
void EmojiGlyphs::run() {
    Font check, draw;
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock,
                   [&] { return stop_ || (check_requested_ && !checked_) || next_ < queue_.size(); });
        if (stop_) {
            return;
        }
        if (check_requested_ && !checked_) {
            auto texts = std::move(to_check_);
            lock.unlock();
            std::set<std::string> renders;
            // Without a color emoji font, fallback fonts still have a few
            // symbols; none of them count.
            if (check.load(32) && check.glyph("\U0001F600")) {
                for (const auto& text : texts) {
                    if (check.glyph(text)) {
                        renders.insert(text);
                    }
                }
            }
            lock.lock();
            renders_ = std::move(renders);
            checked_ = fresh_ = true;
            continue;
        }
        const auto text = queue_[next_++];
        const int size = size_;
        if (images_.contains(text)) {
            continue;
        }
        drawing_ = true;
        lock.unlock();
        cairo_surface_t* surface = nullptr;
        if (draw.load(size)) {
            if (const auto glyph = draw.glyph(text)) {
                surface = draw.draw(*glyph);
            }
        }
        lock.lock();
        drawing_ = false;
        // The request may have changed meanwhile; keep only what it still wants.
        if (surface && size == size_ && !images_.contains(text) &&
            std::find(queue_.begin(), queue_.end(), text) != queue_.end()) {
            images_.emplace(text, surface);
            fresh_ = true;
        } else if (surface) {
            cairo_surface_destroy(surface);
        }
    }
}
} // namespace framekeyboard
