#pragma once
#include <cairo.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace framekeyboard {
// Emoji images from the system's color emoji font. Color fonts store large
// bitmaps that are slow to scale, so a worker thread shapes and scales each
// emoji once and the panel only copies the result. The VR thread never waits
// for it: a cell without its image yet draws empty.
class EmojiGlyphs {
  public:
    // Images held at once. A request keeps the earlier images among its
    // first ones that fit, and draws only the first `prefetch` it lacks: about
    // three screens around the view on the bundled layouts.
    static constexpr std::size_t memory_limit = 8u << 20;
    static constexpr std::size_t prefetch = 300;
    EmojiGlyphs();
    ~EmojiGlyphs();
    EmojiGlyphs(const EmojiGlyphs&) = delete;
    EmojiGlyphs& operator=(const EmojiGlyphs&) = delete;
    // Checks, once, which of these texts the font draws as a single image.
    void check(std::vector<std::string> texts);
    bool checked() const;
    // After checked(): whether this text draws as one image.
    bool renders(const std::string& text) const;
    // Replaces the queue: images of these texts, `size` pixels square, in this
    // order. Images not requested any more are released.
    void request(int size, const std::vector<std::string>& texts);
    // Drops the queue and frees every image, as for a closed picker.
    void release();
    // The finished image, or null. Valid until the next request() or release().
    cairo_surface_t* image(const std::string& text) const;
    // How many images of this size fit the memory limit.
    static std::size_t capacity(int size) {
        return memory_limit / (static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
    }
    // Whether checks or images finished since the last call.
    bool take_ready();
    // Nothing checked or drawn is waiting.
    bool idle() const;
    std::uint64_t generation() const { return generation_; }

  private:
    struct Font;
    void run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_{};
    std::vector<std::string> to_check_;
    bool check_requested_{}, checked_{};
    std::set<std::string> renders_;
    int size_{};
    std::vector<std::string> queue_;
    std::size_t next_{};
    std::map<std::string, cairo_surface_t*> images_; // All at size_.
    bool fresh_{}, drawing_{};
    std::uint64_t generation_{};
    std::thread worker_; // Last, so it starts after everything it uses.
};
} // namespace framekeyboard
