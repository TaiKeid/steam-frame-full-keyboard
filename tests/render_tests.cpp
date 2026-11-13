#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace framekeyboard;
namespace {
const Key& find_key(const Layout& layout, const std::string& action) {
    for (const auto& key : layout.keys) {
        if (key.action == action || key.id == action) {
            return key;
        }
    }
    throw std::runtime_error("missing key " + action);
}
bool has_key(const Layout& layout, const std::string& action) {
    return std::any_of(layout.keys.begin(), layout.keys.end(),
                       [&](const Key& key) { return key.action == action || key.id == action; });
}
class Comparison {
  public:
    PanelRenderer incremental, full;
    double now = 100;
    unsigned frames{};
    std::vector<unsigned char> before;
    bool frame(const PanelView& view, const char* phase) {
        now += 1.0 / 60;
        auto* a = incremental.surface();
        const auto size = static_cast<std::size_t>(cairo_image_surface_get_stride(a)) * texture_height;
        before.assign(cairo_image_surface_get_data(a), cairo_image_surface_get_data(a) + size);
        const bool painted = incremental.paint(view, now);
        // Drawn afresh: a key image kept past a change it should have
        // followed shows up as a difference.
        full.forget_key_images();
        full.paint(view, now, true);
        require_damage(painted, phase);
        auto* b = full.surface();
        if (view.case_opacity < 1) {
            require_premultiplied(phase);
        }
        for (int y = 0; y < texture_height; ++y) {
            const auto* actual = cairo_image_surface_get_data(a) + y * cairo_image_surface_get_stride(a);
            const auto* expected =
                cairo_image_surface_get_data(b) + y * cairo_image_surface_get_stride(b);
            if (std::memcmp(actual, expected, panel_width * 4) != 0) {
                incremental.write_png("/tmp/framekeyboard-render-actual.png");
                full.write_png("/tmp/framekeyboard-render-expected.png");
                throw std::runtime_error(std::string(phase) + ": pixels differ on row " +
                                         std::to_string(y) + " frame " + std::to_string(frames));
            }
        }
        if (incremental.animating() != full.animating()) {
            throw std::runtime_error("animation scheduling differs");
        }
        ++frames;
        return painted;
    }
    // The native texture path uploads Cairo's premultiplied pixels as they
    // are, so over a clear case no colour channel may exceed its alpha.
    void require_premultiplied(const char* phase) {
        auto* a = incremental.surface();
        const int stride = cairo_image_surface_get_stride(a);
        for (int y = 0; y < texture_height; ++y) {
            const auto* row =
                reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(a) + y * stride);
            for (int x = 0; x < panel_width; ++x) {
                const auto pixel = row[x];
                const auto alpha = pixel >> 24;
                if (((pixel >> 16) & 255u) > alpha || ((pixel >> 8) & 255u) > alpha ||
                    (pixel & 255u) > alpha) {
                    char value[16];
                    std::snprintf(value, sizeof value, "%08x", pixel);
                    throw std::runtime_error(std::string(phase) + ": pixel " + std::to_string(x) + "," +
                                             std::to_string(y) + " is not premultiplied (ARGB " + value +
                                             ")");
                }
            }
        }
    }
    // A partial upload copies only the reported damage: every pixel that
    // changed must lie inside it, and nothing may change without a paint.
    void require_damage(bool painted, const char* phase) {
        auto* a = incremental.surface();
        const int stride = cairo_image_surface_get_stride(a);
        const auto* now_pixels = cairo_image_surface_get_data(a);
        const auto& damage = incremental.damage();
        for (int y = 0; y < texture_height; ++y) {
            if (std::memcmp(before.data() + y * stride, now_pixels + y * stride, panel_width * 4) == 0) {
                continue;
            }
            for (int x = 0; x < panel_width; ++x) {
                if (std::memcmp(before.data() + y * stride + x * 4, now_pixels + y * stride + x * 4,
                                4) == 0) {
                    continue;
                }
                const bool covered =
                    painted &&
                    (damage.empty() || std::any_of(damage.begin(), damage.end(), [&](const Rect& r) {
                         return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
                     }));
                if (!covered) {
                    throw std::runtime_error(std::string(phase) + ": pixel " + std::to_string(x) + "," +
                                             std::to_string(y) + " changed outside the damage");
                }
            }
        }
    }
    void settle(const PanelView& view, const char* phase) {
        for (int i = 0; i < 7; ++i) {
            frame(view, phase);
        }
    }
};
void candidate_menu_resize() {
    const auto profiles = load_profiles({}, {});
    for (const auto& [layout, language] :
         {std::pair{"en-us-full", "ja-romaji"}, std::pair{"ja-jis-full", "ja-kana"}}) {
        NullSink sink;
        KeyboardState keyboard(sink);
        LanguageMap keymap(profiles.languages.at(language));
        PanelView view;
        view.layout = &profiles.layouts.at(layout);
        view.theme = &profiles.themes.at("graphite");
        view.language = &profiles.languages.at(language);
        view.keyboard = &keyboard;
        view.keymap = &keymap;
        view.ime_band = view.ime_more = view.composing = true;
        view.preedit = "かか";
        CandidateStrip strip;
        auto row = ime_band(view).candidates;
        row.width = std::max(0.0, row.width - 72);
        const auto list = candidate_list_area(view);
        const auto measure = [&](const std::string& text) {
            return candidate_text_width(text, view.language->font);
        };
        auto set = [&](int count, bool open) {
            std::vector<std::string> texts;
            for (int i = 0; i < count; ++i) {
                texts.push_back("候補" + std::to_string(i));
            }
            strip.set(row, texts, -1, measure, open ? std::optional{list} : std::nullopt, true);
            view.candidates = strip.view();
        };
        Comparison compare;
        set(200, false);
        compare.frame(view, "closed candidate menu");
        set(200, true);
        compare.frame(view, "open tall candidate menu");
        const double tall = view.candidates->list->height;
        set(30, true);
        if (view.candidates->list->height >= tall) {
            throw std::runtime_error("candidate menu fixture must shrink over the keys");
        }
        // Compare every pixel, including the newly uncovered keys and old shadow.
        compare.frame(view, "candidate menu shrinks while open");
        set(200, true);
        compare.frame(view, "candidate menu grows while open");
        set(30, true);
        compare.frame(view, "candidate menu shrinks again");
    }
}
void candidate_scrolling() {
    const auto profiles = load_profiles({}, {});
    for (const auto& [layout, language] :
         {std::pair{"en-us-full", "ja-romaji"}, std::pair{"ja-jis-full", "ja-kana"},
          std::pair{"ja-flick-12", "ja-flick"}}) {
        for (const auto* theme : {"graphite", "porcelain", "neon"}) {
            NullSink sink;
            KeyboardState keyboard(sink);
            LanguageMap keymap(profiles.languages.at(language));
            PanelView view;
            view.layout = &profiles.layouts.at(layout);
            view.theme = &profiles.themes.at(theme);
            view.language = &profiles.languages.at(language);
            view.keyboard = &keyboard;
            view.keymap = &keymap;
            view.ime_band = view.ime_more = view.composing = true;
            view.preedit = "かお";
            view.case_opacity = 0;
            std::vector<std::string> texts;
            for (int i = 0; i < 442; ++i) {
                texts.push_back("(＾▽＾)" + std::to_string(i));
            }
            const auto measure = [&](const std::string& text) {
                return candidate_text_width(text, view.language->font);
            };
            const bool flick = view.language->input_method == "japanese-flick";
            auto row = ime_band(view).candidates;
            if (!flick) {
                row.width = std::max(0.0, row.width - 72);
            }
            CandidateStrip strip;
            Comparison compare;
            for (bool open : {false, true}) {
                strip.set(row, texts, -1, measure,
                          open ? std::optional{candidate_list_area(view)} : std::nullopt, !flick);
                view.candidates = strip.view();
                compare.frame(view, "candidate scrolling starts");
                for (int frame = 0; frame < 8; ++frame) {
                    const auto area = open ? *view.candidates->list : row;
                    if (!strip.scroll(area.x + area.width / 2, area.y + area.height / 2, 1)) {
                        throw std::runtime_error("candidate scrolling fixture must move");
                    }
                    view.candidates = strip.view();
                    compare.frame(view, "scrolled candidates match a full paint");
                    const auto& damage = compare.incremental.damage();
                    if (damage.empty()) {
                        throw std::runtime_error("candidate scrolling must keep partial paints");
                    }
                    for (std::size_t i = 0; i < damage.size(); ++i) {
                        for (std::size_t j = 0; j < damage.size(); ++j) {
                            const auto& a = damage[i];
                            const auto& b = damage[j];
                            if (i != j && a.x >= b.x && a.y >= b.y && a.x + a.width <= b.x + b.width &&
                                a.y + a.height <= b.y + b.height) {
                                throw std::runtime_error("candidate damage repeats a contained pass");
                            }
                        }
                    }
                }
            }
        }
    }
}
void handle_states() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    view.controls = {{"move-handle",
                      "Move keyboard",
                      {580, 612, 440, 60},
                      false,
                      Icon::None,
                      ControlStyle::MoveHandle}};
    Comparison compare;
    auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    auto row = [&] {
        const auto pixels = compare.incremental.rgba();
        const auto offset = static_cast<std::size_t>((642 + popup_margin) * panel_width) * 4;
        return std::vector<unsigned char>(pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                                          pixels.begin() +
                                              static_cast<std::ptrdiff_t>(offset + panel_width * 4));
    };
    auto width = [](const auto& pixels) {
        int count = 0;
        for (int x = 0; x < panel_width; ++x) {
            count += pixels[static_cast<std::size_t>(x) * 4 + 3] != 0;
        }
        return count;
    };
    compare.frame(view, "idle movement line");
    const auto idle = row();
    const auto center = static_cast<std::size_t>(panel_width / 2) * 4;
    require(idle[center] == 61 && idle[center + 1] == 68 && idle[center + 2] == 80 &&
                idle[center + 3] == 255,
            "idle handle uses native opaque #3d4450");
    compare.incremental.write_png("/tmp/framekeyboard-handle-idle.png");
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "start movement hover");
    require(compare.incremental.animating(), "hover schedules its 80 ms transition");
    compare.frame(view, "movement hover transition");
    const auto between = row();
    require(between[center] > idle[center] && between[center] < 255 && width(between) > width(idle),
            "hover interpolates both size and color");
    compare.settle(view, "complete movement hover");
    const auto hovered = row();
    require(hovered[center] == 255 && hovered[center + 1] == 255 && hovered[center + 2] == 255 &&
                std::abs(width(hovered) - width(idle) * 1.1) <= 2 && !compare.incremental.animating(),
            "hover ends white and 10 percent wider, then stops repainting");
    compare.incremental.write_png("/tmp/framekeyboard-handle-hover.png");
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    view.controls.front().selected = true;
    compare.frame(view, "captured movement stays active");
    require(row() == hovered, "capture retains the native active appearance after hover is cleared");
    view.controls.front().selected = false;
    compare.frame(view, "release movement line");
    compare.settle(view, "return to idle movement line");
    require(row() == idle && !compare.incremental.animating(),
            "leaving the handle clears grown pixels and restores idle color");
    view.settings = true;
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "settings movement hover");
    require(compare.incremental.animating(), "hover animates while settings is open too");
    view.controls.clear();
    compare.frame(view, "pin during movement animation");
    require(width(row()) == 0 && !compare.incremental.animating(),
            "pin removes the complete line and stops its animation");

    // A dropdown choice too long for its row scrolls while hovered.
    view.popup = Rect{400, 200, 290, 56};
    view.controls = {{"choose:layout:long",
                      "A layout whose name is much too long for its row in the list",
                      {400, 200, 290, 56},
                      false,
                      Icon::None,
                      ControlStyle::MenuItem}};
    auto item = [&] {
        const auto pixels = compare.incremental.rgba();
        std::vector<unsigned char> result;
        for (int y = 200; y < 256; ++y) {
            const auto start = static_cast<std::size_t>((y + popup_margin) * panel_width + 400) * 4;
            result.insert(result.end(), pixels.begin() + static_cast<std::ptrdiff_t>(start),
                          pixels.begin() + static_cast<std::ptrdiff_t>(start + 290 * 4));
        }
        return result;
    };
    compare.frame(view, "long choice");
    require(!compare.incremental.animating(), "an unhovered long choice stays still");
    const auto still = item();
    view.hovered_controls = {"choose:layout:long"};
    compare.frame(view, "hovered long choice");
    const double hovered_at = compare.now;
    const auto waiting = item();
    require(compare.incremental.animating() &&
                std::abs(compare.incremental.next_change() - (hovered_at + .8)) < 1e-6,
            "a hovered long choice schedules its first move after its wait");
    bool painted_while_waiting = false;
    while (compare.now + 1.0 / 60 < hovered_at + .8) {
        painted_while_waiting |= compare.frame(view, "long choice waits");
    }
    require(!painted_while_waiting && item() == waiting, "nothing repaints while it waits");
    int paints = 0, frames = 0;
    for (; frames < 60; ++frames) {
        // As App::paint: a frame before the next change is skipped.
        if (compare.now + 1.0 / 60 < compare.incremental.next_change()) {
            compare.now += 1.0 / 60;
            continue;
        }
        if (compare.frame(view, "scrolling long choice")) {
            ++paints;
            const auto& damage = compare.incremental.damage();
            // Damage is in texture rows, below the popup margin.
            require(damage.size() == 1 && damage.front().x >= 400 &&
                        damage.front().y >= 200 + popup_margin &&
                        damage.front().x + damage.front().width <= 690 &&
                        damage.front().y + damage.front().height <= 256 + popup_margin,
                    "a moving name repaints only its own box");
        }
    }
    require(item() != waiting && paints > 20 && paints <= 31,
            "and its name moves, at most 30 repaints a second");
    view.hovered_controls.clear();
    compare.frame(view, "long choice left");
    require(!compare.incremental.animating(), "leaving it stops the scroll");
    view.controls.front().label = "Short";
    view.hovered_controls = {"choose:layout:long"};
    compare.frame(view, "hovered short choice");
    require(!compare.incremental.animating(), "a name that fits never scrolls");
    view.popup.reset();
    view.controls.clear();
    view.hovered_controls.clear();
}
// The compatibility texture path uploads straight RGBA: rgba() must undo the
// premultiplication of a clear case's partly transparent edges and shadows.
// A key lights at once under the laser. With Hover fade it dims over a moment
// after the laser leaves; without it, it goes dark at once.
void hover_fade() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    const auto& key = find_key(*view.layout, "KeyG");
    auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    for (const bool fade : {true, false}) {
        view.hover_fade = fade;
        view.hovered_keys.clear();
        Comparison compare;
        compare.frame(view, "idle key");
        // A point low on the key's face, clear of its legend and edge light.
        const auto face = panel_rect(view, key.bounds);
        const auto offset =
            static_cast<std::size_t>((static_cast<int>(face.y + face.height) - 10 + popup_margin) *
                                         panel_width +
                                     static_cast<int>(face.x) + 8) *
            4;
        auto red = [&] { return static_cast<int>(compare.incremental.rgba()[offset]); };
        const int idle = red();
        view.hovered_keys = {key.id};
        compare.frame(view, "laser on the key");
        const int lit = red();
        require(lit > idle && !compare.incremental.animating(), "a key lights at once under the laser");
        view.hovered_keys.clear();
        compare.frame(view, "laser left the key");
        if (!fade) {
            require(red() == idle && !compare.incremental.animating(),
                    "without Hover fade a key goes dark at once");
            continue;
        }
        require(red() == lit && compare.incremental.animating(), "Hover fade starts from the lit key");
        require(compare.incremental.next_change() > compare.now,
                "Hover fade steps at 30 fps instead of asking for every frame");
        // The fade steps at 30 fps: three 60 Hz frames pass at least one step.
        for (int i = 0; i < 3; ++i) {
            compare.frame(view, "the key dims");
        }
        const int between = red();
        require(between < lit && between > idle, "Hover fade dims a key step by step");
        for (int i = 0; i < 12; ++i) {
            compare.frame(view, "the key goes dark");
        }
        require(red() == idle && !compare.incremental.animating(),
                "Hover fade ends on the idle key and stops repainting");
        // Hidden while a key fades, as when Enter closes the panel: shown again, it is dark at once.
        view.hovered_keys = {key.id};
        compare.frame(view, "laser on the key again");
        view.hovered_keys.clear();
        compare.incremental.settle();
        compare.full.settle();
        compare.frame(view, "shown again");
        require(red() == idle && !compare.incremental.animating(), "a fade from before a hide is over");
        // A key still held at a hide is let go of without a paint: shown again, it is at rest.
        keyboard.down(0, key, compare.now);
        for (int i = 0; i < 8; ++i) {
            compare.frame(view, "the key held");
        }
        keyboard.up(0);
        compare.incremental.settle();
        compare.full.settle();
        compare.frame(view, "shown again after a press");
        require(red() == idle && !compare.incremental.animating(), "a press from before a hide is over");
    }
    // High Contrast keeps its keys as they were: its edge under the laser is on or off, and so is the
    // hover face, Hover fade or not.
    view.theme = &profiles.themes.at("contrast");
    view.hover_fade = true;
    Comparison contrast;
    view.hovered_keys = {key.id};
    contrast.frame(view, "laser on a High Contrast key");
    view.hovered_keys.clear();
    contrast.frame(view, "laser left it");
    require(!contrast.incremental.animating(), "a High Contrast key goes dark at once");
}
// The swipe trail: a quiet path that only grows, a comet over its last
// moments, and a fade when the swipe ends. Every frame also paints the same
// incrementally as in full.
void swipe_trail() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    Comparison compare;
    compare.frame(view, "before the swipe");
    const auto idle = compare.incremental.rgba();
    // Along the home row from A to L, trembling as a laser does.
    const auto from = panel_rect(view, find_key(*view.layout, "KeyA").bounds);
    const auto to = panel_rect(view, find_key(*view.layout, "KeyL").bounds);
    for (int i = 0; i <= 30; ++i) {
        view.swipe_trail.emplace_back(from.x + from.width / 2 + (to.x - from.x) * i / 30.0,
                                      from.y + from.height / 2 + 5 * std::sin(i * 1.3));
        compare.frame(view, "the swipe grows");
    }
    require(compare.incremental.animating() && compare.incremental.rgba() != idle,
            "a swipe draws its trail, and the comet keeps repainting");
    for (int i = 0; i < 30; ++i) {
        compare.frame(view, "the laser rests");
    }
    const auto resting = compare.incremental.rgba();
    require(!compare.incremental.animating() && resting != idle,
            "at rest the comet burns out, the path stays and nothing repaints");
    require(!compare.frame(view, "the laser still rests"), "a burnt-out comet asks for no paint");
    view.swipe_trail.clear();
    compare.frame(view, "the swipe ends");
    require(compare.incremental.animating(), "the path fades after the swipe");
    // The fade steps at 30 fps: three 60 Hz frames pass at least one step.
    for (int i = 0; i < 3; ++i) {
        compare.frame(view, "the path fades");
    }
    require(compare.incremental.rgba() != resting && compare.incremental.rgba() != idle,
            "the fade is gradual");
    for (int i = 0; i < 20; ++i) {
        compare.frame(view, "the path fades out");
    }
    require(!compare.incremental.animating() && compare.incremental.rgba() == idle &&
                compare.incremental.trail_layers(),
            "a finished trail leaves nothing behind, and its layers wait for the next swipe");
    // The layers keep their pages for the next word, and give them back once
    // no trail has been drawn for a few seconds (R239, R240).
    const auto kept = compare.incremental.trail_resident_bytes();
    compare.incremental.release_idle_layers(compare.now + 1);
    require(kept > 0 && compare.incremental.trail_resident_bytes() == kept,
            "right after a word the trail's layers keep their pages");
    compare.now += 3.1;
    compare.incremental.release_idle_layers(compare.now);
    require(compare.incremental.trail_layers() && compare.incremental.trail_resident_bytes() == 0,
            "after a few idle seconds all three layers give their pages back");
    // The next word draws on the given-back layers as on kept ones: the full
    // renderer still holds its pages.
    for (int i = 0; i <= 6; ++i) {
        view.swipe_trail.emplace_back(from.x + 20 + 12 * i, from.y + 20);
        compare.frame(view, "a swipe after the pages went");
    }
    view.swipe_trail.clear();
    compare.frame(view, "it ends");
    for (int i = 0; i < 40; ++i) {
        compare.frame(view, "it fades out");
    }
    require(!compare.incremental.animating() && compare.incremental.rgba() == idle,
            "the word after the pages went fades out cleanly");
    // A swipe that starts while the last one fades replaces it at once.
    for (int i = 0; i <= 6; ++i) {
        view.swipe_trail.emplace_back(from.x + 20 + 12 * i, from.y + 20);
        compare.frame(view, "another swipe");
    }
    // The faded word's idle time ended with the new one.
    compare.incremental.release_idle_layers(compare.now + 10);
    require(compare.incremental.trail_resident_bytes() > 0, "a word in use keeps its pages");
    view.swipe_trail.clear();
    compare.frame(view, "it ends");
    for (int i = 0; i <= 6; ++i) {
        view.swipe_trail.emplace_back(to.x + 20 - 12 * i, to.y + 30);
        compare.frame(view, "the next swipe starts during the fade");
    }
    view.settings = true;
    compare.frame(view, "Settings opens over a swipe");
    view.settings = false;
    view.swipe_trail.clear();
    compare.frame(view, "back on the keys");
    require(compare.incremental.rgba() == idle, "Settings ends a trail without a fade");
    // Hidden mid-swipe: shown again, no trail from before is left to fade.
    for (int i = 0; i <= 6; ++i) {
        view.swipe_trail.emplace_back(from.x + 20 + 12 * i, from.y + 20);
        compare.frame(view, "a swipe before a hide");
    }
    view.swipe_trail.clear();
    compare.incremental.settle();
    compare.full.settle();
    require(!compare.incremental.trail_layers(), "hidden, the trail's layers are freed at once");
    compare.frame(view, "shown again");
    require(compare.incremental.rgba() == idle && !compare.incremental.animating(),
            "a trail from before a hide is gone at once");
}
void straight_alpha() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    view.case_opacity = 0;
    PanelRenderer renderer;
    renderer.paint(view, 1, true);
    auto* surface = renderer.surface();
    const auto rgba = renderer.rgba();
    int partial = 0;
    for (int y = 0; y < texture_height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface));
        for (int x = 0; x < panel_width; ++x) {
            const unsigned alpha = row[x] >> 24;
            const auto* out = &rgba[static_cast<std::size_t>(y * panel_width + x) * 4];
            if (out[3] != alpha) {
                throw std::runtime_error("straight RGBA keeps each pixel's alpha");
            }
            if (alpha == 0 || alpha == 255) {
                if (alpha == 0 && (out[0] | out[1] | out[2]) != 0) {
                    throw std::runtime_error("a clear pixel has no colour");
                }
                continue;
            }
            ++partial;
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned premultiplied = (row[x] >> (16 - 8 * channel)) & 255u;
                // Back to straight colour, within the rounding of 8-bit premultiplication.
                if (std::abs(out[channel] * static_cast<double>(alpha) / 255 -
                             static_cast<double>(premultiplied)) > 1) {
                    throw std::runtime_error("straight RGBA does not undo premultiplied alpha");
                }
            }
        }
    }
    if (partial < 1000) {
        throw std::runtime_error("a clear case should leave partly transparent edges to check");
    }
}
// Alpha of a panel pixel, in case coordinates.
int alpha_at(const PanelRenderer& renderer, int x, int y) {
    auto* surface = renderer.surface();
    const auto* row = reinterpret_cast<const std::uint32_t*>(
        cairo_image_surface_get_data(surface) +
        (y + popup_margin) * cairo_image_surface_get_stride(surface));
    return static_cast<int>(row[x] >> 24);
}
// With a transparent background every repaint must clear its damage to
// nothing before drawing, or the case's old pixels or a plate's previous
// alpha would build up where only the dashboard should show.
void exercise(Layout layout, Theme theme, Language language, double opacity = 1) {
    const bool transparent = opacity < 1;
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(language);
    PanelView view;
    view.layout = &layout;
    view.theme = &theme;
    view.language = &language;
    view.keymap = &keymap;
    view.keyboard = &keyboard;
    view.case_opacity = opacity;
    view.controls = {{"settings", "Settings", {18, 12, 60, 42}, false, Icon::Settings}};
    // A point in the case's padding, left of every key: clear, or exactly
    // the case's opacity, however many partial repaints covered it.
    auto require_clear = [&](const Comparison& compare, const char* phase) {
        const int expected = static_cast<int>(std::round(opacity * 255));
        const int alpha =
            alpha_at(compare.incremental, static_cast<int>(panel_case_bounds(view).x) + 3, 300);
        if (transparent && std::abs(alpha - expected) > 1) {
            throw std::runtime_error(std::string(phase) + ": the see-through case's alpha drifted");
        }
    };
    if (!language.composition_keys.empty()) {
        view.composing = true;
        for (const auto& key : layout.keys) {
            if (auto it = language.composition_keys.find(key.action);
                it != language.composition_keys.end()) {
                view.key_labels[key.id] = it->second;
            }
        }
    }
    const bool flick = !language.flick.empty();
    if (flick) {
        // As App shows them: the tap kana on each flick key.
        view.composing = true;
        for (const auto& key : layout.keys) {
            if (const auto it = language.flick.find(key.action); it != language.flick.end()) {
                view.key_labels[key.id] = it->second.tap.front();
            }
        }
    }
    Comparison compare;
    compare.frame(view, "initial");
    view.controls.push_back({"move-handle",
                             "Move keyboard",
                             {580, 612, 440, 60},
                             false,
                             Icon::None,
                             ControlStyle::MoveHandle});
    compare.frame(view, "show movement line");
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "hover movement line");
    view.controls.pop_back();
    compare.frame(view, "hide movement line on pin or grab mode");
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    // The first two 12-key keys stand in for A and B on a flick layout.
    std::vector<Key> flick_keys;
    std::copy_if(layout.keys.begin(), layout.keys.end(), std::back_inserter(flick_keys),
                 [](const Key& key) { return key.action_kind == ActionKind::Flick; });
    const auto a = flick ? flick_keys.at(0) : find_key(layout, "KeyA");
    const auto b = flick ? flick_keys.at(1) : find_key(layout, "KeyB");
    view.hovered_keys = {a.id};
    compare.frame(view, "hover");
    keyboard.down(0, a, compare.now);
    compare.frame(view, "press");
    keyboard.down(1, b, compare.now);
    compare.frame(view, "second controller");
    // Reverse partway through a press; the retained image must erase its old edge.
    keyboard.up(0);
    view.hovered_keys = {b.id};
    compare.settle(view, "release while second hand holds");
    keyboard.cancel_all();
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    compare.settle(view, "cancel");
    require_clear(compare, "after presses");
    if (flick) {
        // A flick relabels only the pressed key and hides its hints.
        const auto& slot = language.flick.at(a.action);
        view.flick_preview =
            PanelView::FlickPreview{a.id, {slot.tap.front(), slot.left, slot.up, slot.right, slot.down}};
        keyboard.down(0, a, compare.now);
        compare.frame(view, "flick preview during press animation");
        view.hovered_keys = {b.id}; // A neighbour under the preview repaints partially.
        compare.frame(view, "hover beneath the flick preview");
        view.flick_preview->selected = 2;
        compare.settle(view, "flick direction");
        if (transparent && !slot.left.empty()) {
            // The case-coloured gutters stay between the preview's caps, so
            // they stand apart from the keys beneath on a clear case too.
            // Between the tap cap and the left one (flick_cells: cells a
            // key's height × 1.15 square, 4 px apart), this pixel column lies
            // wholly inside the left cap's 2 px gutter.
            const auto key = panel_rect(view, a.bounds);
            const double tap_left = key.x + key.width / 2 - key.height * 1.15 / 2;
            const int x = static_cast<int>(std::floor(tap_left - 3));
            const int y = static_cast<int>(std::round(key.y + key.height / 2));
            auto* surface = compare.incremental.surface();
            const auto pixel = reinterpret_cast<const std::uint32_t*>(
                cairo_image_surface_get_data(surface) +
                (y + popup_margin) * cairo_image_surface_get_stride(surface))[x];
            auto near = [](unsigned byte, double channel) {
                return std::abs(static_cast<double>(byte) - channel * 255) <= 1;
            };
            if ((pixel >> 24) != 255 || !near((pixel >> 16) & 255u, theme.surface.r) ||
                !near((pixel >> 8) & 255u, theme.surface.g) || !near(pixel & 255u, theme.surface.b)) {
                throw std::runtime_error("the flick preview lost its gutters on a clear case");
            }
        }
        view.preedit = "う";
        compare.frame(view, "reading line repaints alone");
        view.preedit = "うか";
        compare.frame(view, "reading line grows");
        keyboard.up(0);
        view.flick_preview.reset();
        view.hovered_keys.clear();
        view.key_labels[a.id] = slot.tap.front();
        compare.settle(view, "flick release");
    }
    for (const auto* action :
         {"ShiftLeft", "CapsLock", "NumLock", "ControlLeft", "MetaLeft", "MetaRight"}) {
        if (!has_key(layout, action)) {
            continue; // The flick layout hides modifiers and locks.
        }
        const auto key = find_key(layout, action);
        keyboard.down(0, key, compare.now);
        compare.settle(view, "modifier/lock press");
        keyboard.up(0);
        compare.settle(view, "modifier/lock release");
        keyboard.down(1, a, compare.now);
        compare.settle(view, "modified key");
        keyboard.up(1);
        compare.settle(view, "modified release");
    }
    for (const auto* action : {"Enter", "NumpadEnter", "Space", "Copy", "ArrowUp"}) {
        if (!has_key(layout, action)) {
            continue;
        }
        const auto key = find_key(layout, action);
        view.hovered_keys = {key.id};
        keyboard.down(0, key, compare.now);
        compare.settle(view, "large/icon key press");
        keyboard.up(0);
        view.hovered_keys.clear();
        view.hovered_controls.clear();
        compare.settle(view, "large/icon key release");
    }
    view.hide_numpad = true;
    compare.frame(view, "hide numpad");
    view.hovered_keys = {a.id};
    keyboard.down(0, a, compare.now);
    compare.settle(view, "narrow keyboard press");
    keyboard.up(0);
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    compare.settle(view, "narrow keyboard release");
    view.settings = true;
    compare.frame(view, "narrow keyboard settings");
    view.settings = false;
    compare.frame(view, "narrow keyboard return");
    view.hide_numpad = false;
    compare.frame(view, "restore numpad");
    view.hovered_controls = {"settings"};
    compare.frame(view, "toolbar hover");
    view.status = "Connection status changed";
    compare.frame(view, "status");
    require_clear(compare, "beside the status plate");
    const auto badge = [&](bool on) {
        for (auto& control : view.controls) {
            control.badge = on && control.id == "settings";
        }
    };
    badge(true);
    compare.frame(view, "update dot");
    badge(false);
    compare.frame(view, "update dot cleared");
    view.settings = true;
    compare.frame(view, "settings");
    // Settings hides the keys and key_bounds_ is empty: a speech model
    // download starting under it must not ask for the mic key's bounds.
    view.dictation_download = .5;
    compare.frame(view, "settings with speech model download");
    view.dictation_download.reset();
    compare.frame(view, "settings after speech model download");
    view.settings = false;
    view.status.clear();
    compare.frame(view, "return to keys");
    view.composing = true;
    view.preedit = "にほんご";
    view.key_labels[a.id] = "ち";
    compare.frame(view, "composition geometry");
    keyboard.down(0, a, compare.now);
    compare.settle(view, "kana press");
    keyboard.up(0);
    compare.settle(view, "kana release");
    view.preedit = "日本語";
    view.controls.push_back({"candidate", "日本語", {85, 148, 230, 42}});
    compare.frame(view, "candidate");
    view.composing = false;
    view.preedit.clear();
    view.key_labels.clear();
    compare.frame(view, "composition ended");
    // A key pulled toward its Shift character repaints alone, as it fills.
    for (const double pull : {.5, 1.0}) {
        view.key_flick = PanelView::KeyFlick{a.id, "Ä", pull};
        compare.frame(view, pull < 1 ? "key flick pulled" : "key flick filled");
    }
    view.key_flick.reset();
    compare.frame(view, "key flick ended");
    // Mutate profiles in place, as Reload may reuse existing storage. Pointer
    // equality is not sufficient for invalidating a retained image.
    theme.top = {.6, .2, .1};
    theme.font_size = 40;
    theme.small_font_size = 30;
    theme.travel = 1;
    theme.depth = 2;
    theme.padding = 35;
    layout.keys.front().bounds.width += 3;
    compare.frame(view, "custom theme/layout reload");
    keyboard.down(0, a, compare.now);
    compare.settle(view, "large font press");
    keyboard.up(0);
    compare.settle(view, "large font release");
    view.hovered_keys = {b.id};
    compare.frame(view, "large font hover");
    theme.duration_ms = 0;
    compare.frame(view, "instant animation theme");
    keyboard.down(0, b, compare.now);
    compare.frame(view, "instant press");
    keyboard.up(0);
    compare.frame(view, "instant release");
    if (has_key(layout, "Dictation")) {
        // The mic key's scrolling bars and the dictation editor repaint only
        // their own areas.
        view.dictation_available = true;
        view.dictation_capturing = true;
        view.dictation_levels = {0, 0, 0, 0, 0, 0};
        compare.frame(view, "listening");
        for (int level : {3, 8, 5, 0, 0, 0, 0, 0}) {
            view.dictation_levels.erase(view.dictation_levels.begin());
            view.dictation_levels.push_back(level);
            view.dictation_level_since = compare.now;
            // Each bar glides into place over several frames.
            for (int i = 0; i < 6; ++i) {
                compare.frame(view, "level bars glide");
            }
        }
        view.dictation_capturing = false;
        compare.frame(view, "paused");
    }
    require_clear(compare, "at the end");
    std::cout << language.id << (transparent ? " (see-through)" : "") << ": " << compare.frames
              << " frames pixel-identical\n";
}
} // namespace
// A row carried across the languages popup repaints alone and leaves nothing behind.
void carried_row() {
    const auto profiles = load_profiles({}, {});
    const auto& layout = profiles.layouts.at("en-us-full");
    const auto& theme = profiles.themes.at("graphite");
    const auto& language = profiles.languages.at("en-us");
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(language);
    PanelView view;
    view.layout = &layout;
    view.theme = &theme;
    view.language = &language;
    view.keymap = &keymap;
    view.keyboard = &keyboard;
    view.settings = true;
    PanelView::LanguagesView popup;
    popup.panel = languages_popup_bounds;
    popup.well = languages_well_bounds;
    popup.layouts_list = languages_layouts_bounds;
    popup.languages_list = languages_list_bounds;
    popup.additional_list = additional_list_bounds;
    PanelView::LanguagesView::Row row;
    row.title = "German";
    row.note = "Deutsch";
    row.bounds = {1130, 190, 424, 56};
    popup.additional = {row};
    popup.main = row;
    popup.main.bounds = languages_main_bounds;
    for (int i = 0; i < 16; ++i) {
        auto listed = row;
        listed.id = "row-" + std::to_string(i);
        listed.title = "Language with a long name " + std::to_string(i);
        listed.note = "Its own name";
        listed.bounds = {338 + (i % 2) * 380.0, 116 + (i / 2) * 56.0, 372, 52};
        popup.languages.push_back(listed);
        view.controls.push_back({listed.id, listed.title, listed.bounds});
    }
    popup.hint = "Drag a language here";
    row.bounds = {420, 280, 312, 52};
    popup.lifted = row;
    view.languages = popup;
    Comparison compare;
    compare.frame(view, "popup with a carried row");
    for (int i = 0; i < 16; ++i) {
        auto& lifted = *view.languages->lifted;
        lifted.bounds.x += 37;
        lifted.bounds.y += i % 2 ? -23 : 11;
        lifted.removing = i > 10;
        compare.frame(view, "carried row");
        double area = 0;
        for (const auto& r : compare.incremental.damage()) {
            area += r.width * r.height;
        }
        if (area == 0 || area > panel_width * panel_height / 2) {
            throw std::runtime_error("a carried row repaints the popup");
        }
    }
    view.languages->lifted.reset();
    compare.frame(view, "row dropped");
}
// A status the toolbar's gap can't hold scrolls under the laser, whatever the
// language, and one with no room at all stays out of the way.
void status_line() {
    const auto profiles = load_profiles({}, {});
    const auto& layout = profiles.layouts.at("en-us-full");
    const auto& theme = profiles.themes.at("graphite");
    const auto& language = profiles.languages.at("en-us");
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(language);
    PanelView view;
    view.layout = &layout;
    view.theme = &theme;
    view.language = &language;
    view.keymap = &keymap;
    view.keyboard = &keyboard;
    view.dictation = DictationView{};
    view.dictation->bounds = {100, 120, 1200, 300};
    view.status = "Game sounds may be picked up during a play session.";
    view.status_hovered = true;
    const auto scrolls = [&] {
        PanelRenderer renderer;
        renderer.paint(view, 1);
        return renderer.animating();
    };
    const Control buttons{"buttons", "", {18, 12, 590, 42}};
    if (scrolls()) {
        throw std::runtime_error("a status that fits does not scroll");
    }
    view.controls = {buttons, {"toggle", "", {680, 12, 150, 42}}};
    if (!scrolls()) {
        throw std::runtime_error("a long status in a narrow gap scrolls");
    }
    view.controls = {buttons, {"swipe-candidate-0", "word", {618, 12, 100, 42}}};
    if (scrolls() || dictation_status_bounds(view).width != 0) {
        throw std::runtime_error("a status with no room stays out of the way");
    }
    view.controls = {{"swipe-candidate-0", "word", {300, 12, 100, 42}},
                     {"swipe-candidate-1", "word", {410, 12, 100, 42}}};
    if (dictation_status_bounds(view).x > 300) {
        throw std::runtime_error("a candidate is not a toolbar button");
    }
}
int main() {
    try {
        candidate_menu_resize();
        candidate_scrolling();
        handle_states();
        hover_fade();
        swipe_trail();
        status_line();
        carried_row();
        const auto profiles = load_profiles({}, {});
        for (const auto& [id, language] : profiles.languages) {
            const auto layout = !language.flick.empty()                        ? "ja-flick-12"
                                : (language.keymap == "jp" || id == "ja-kana") ? "ja-jis-full"
                                : language.keymap == "br"                      ? "br-abnt2-full"
                                : !language.required_keys.empty()              ? "international-full"
                                                                               : "en-us-full";
            validate_selection(profiles, {layout, id, "graphite"});
            exercise(profiles.layouts.at(layout), profiles.themes.at("graphite"), language);
        }
        // Every bundled theme paints the same frames incrementally as in full.
        for (const auto& [id, theme] : profiles.themes) {
            validate_selection(profiles, {"en-us-full", "en-us", id});
            exercise(profiles.layouts.at("en-us-full"), theme, profiles.languages.at("en-us"));
        }
        // Transparent background: the same frames over a clear case, for the
        // plain, composing and flick layouts and a light theme.
        exercise(profiles.layouts.at("en-us-full"), profiles.themes.at("graphite"),
                 profiles.languages.at("en-us"), 0);
        exercise(profiles.layouts.at("ja-jis-full"), profiles.themes.at("graphite"),
                 profiles.languages.at("ja-romaji"), 0);
        exercise(profiles.layouts.at("ja-flick-12"), profiles.themes.at("porcelain"),
                 profiles.languages.at("ja-flick"), 0);
        exercise(profiles.layouts.at("en-us-full"), profiles.themes.at("porcelain"),
                 profiles.languages.at("en-us"), 0);
        // Semi-transparent: a half-opaque case under the same partial repaints.
        exercise(profiles.layouts.at("en-us-full"), profiles.themes.at("graphite"),
                 profiles.languages.at("en-us"),
                 semi_transparent_case_opacity(profiles.themes.at("graphite")));
        exercise(profiles.layouts.at("en-us-full"), profiles.themes.at("porcelain"),
                 profiles.languages.at("en-us"),
                 semi_transparent_case_opacity(profiles.themes.at("porcelain")));
        // A light theme's pale case reads far brighter, so it keeps less of itself.
        for (const auto& [id, theme] : profiles.themes) {
            const bool light =
                .2126 * theme.legend.r + .7152 * theme.legend.g + .0722 * theme.legend.b <= .5;
            if (semi_transparent_case_opacity(theme) != (light ? .15 : .5)) {
                throw std::runtime_error("semi-transparent opacity does not follow theme " + id);
            }
        }
        straight_alpha();
        // Notice colours default by the legend's lightness; a theme may set its own.
        const std::string base =
            R"({"schema_version":1,"id":"t","name":"T","surface":"#101010","surface_radius":20,)"
            R"("key_top":"#303030","key_middle":"#282828","key_bottom":"#202020",)"
            R"("key_side_top":"#151515","key_side_bottom":"#0a0a0a","key_radius":8,"key_depth":3.5,)"
            R"("key_press_travel":3,"press_duration_ms":80,)";
        const auto dark = parse_theme(base + R"("legend":"#ffffff"})");
        const auto light = parse_theme(base + R"("legend":"#202020"})");
        const auto custom = parse_theme(base + R"("legend":"#202020","warning":"#ff0000"})");
        if (!(dark.warning == Color{.96, .72, .35} && light.warning.r < .6 && light.error.r < .7 &&
              custom.warning == Color{1, 0, 0})) {
            throw std::runtime_error("notice colours follow the legend unless the theme sets them");
        }
        // An alert icon stands apart from the legend and reads on the keycap in
        // every bundled theme: orange, not the legend's grey, at 3:1 or more.
        const auto luminance = [](Color c) {
            const auto channel = [](double v) {
                return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
            };
            return .2126 * channel(c.r) + .7152 * channel(c.g) + .0722 * channel(c.b);
        };
        const auto contrast = [&](Color a, Color b) {
            const double x = luminance(a) + .05, y = luminance(b) + .05;
            return std::max(x, y) / std::min(x, y);
        };
        // A case plate keeps the legend readable over the brightest and the
        // darkest background behind a transparent case, in every theme.
        for (const auto& [id, theme] : profiles.themes) {
            for (const double behind : {0.0, 1.0}) {
                const auto blend = [&](double c) {
                    return c * case_plate_alpha + behind * (1 - case_plate_alpha);
                };
                const Color plate{blend(theme.surface.r), blend(theme.surface.g),
                                  blend(theme.surface.b)};
                if (contrast(theme.legend, plate) < 4.5) {
                    throw std::runtime_error("case plate text is hard to read in theme " + id);
                }
            }
        }
        for (const auto& [id, theme] : profiles.themes) {
            if (theme.alert.r - theme.alert.b < .5 || contrast(theme.alert, theme.top) < 3 ||
                contrast(theme.alert, theme.legend) < 1.5) {
                throw std::runtime_error("alert icon colour is not distinct in theme " + id);
            }
        }
        const auto alerted = parse_theme(base + R"("legend":"#202020","alert":"#00ff00"})");
        if (!(dark.alert == dark.warning && light.alert != light.warning &&
              alerted.alert == Color{0, 1, 0})) {
            throw std::runtime_error("the alert colour follows the legend unless the theme sets it");
        }
        // The latched fill is flat with the legend on it unless the theme says otherwise.
        const auto lit = parse_theme(
            base + R"("legend":"#ffffff","latched":"#d4a52e","latched_top":"#f6e3a1",)"
                   R"("latched_bottom":"#9a6f17","latched_legend":"#2b1d02","latched_finish":"neon"})");
        if (!(dark.latched_top == dark.latched && dark.latched_bottom == dark.latched &&
              dark.latched_legend == dark.legend && dark.latched_finish == "flat" &&
              lit.latched_top != lit.latched && lit.latched_legend != lit.legend &&
              lit.latched_finish == "neon")) {
            throw std::runtime_error("latched tokens default to a flat fill under the legend");
        }
        // Edges and a pressed fill come only from a theme that sets them.
        const auto edged =
            parse_theme(base + R"("legend":"#ffffff","edge":"#ffffff","pressed":"#ffff00",)"
                               R"("pressed_legend":"#000000"})");
        if (!(!dark.edge && !dark.edge_hover && !dark.pressed && dark.pressed_legend == dark.legend &&
              edged.edge == Color{1, 1, 1} && edged.edge_hover == edged.edge &&
              edged.pressed == Color{1, 1, 0} && edged.pressed_legend == Color{})) {
            throw std::runtime_error(
                "edge and pressed tokens are optional, and edge_hover follows edge");
        }
        // High Contrast's text reaches 7:1 (WCAG AAA) on every face it sits on:
        // idle, hovered, pressed, latched and disabled.
        const auto& high = profiles.themes.at("contrast");
        const Color dimmed{(high.legend.r + high.disabled.r) / 2, (high.legend.g + high.disabled.g) / 2,
                           (high.legend.b + high.disabled.b) / 2};
        const Color sunk{high.bottom.r * .6 + high.surface.r * .4,
                         high.bottom.g * .6 + high.surface.g * .4,
                         high.bottom.b * .6 + high.surface.b * .4};
        for (const auto& [ink, face] :
             std::initializer_list<std::pair<Color, Color>>{{high.legend, high.top},
                                                            {high.legend, high.bottom},
                                                            {high.legend, high.hover},
                                                            {high.pressed_legend, *high.pressed},
                                                            {high.latched_legend, high.latched},
                                                            {dimmed, sunk}}) {
            if (contrast(ink, face) < 7) {
                throw std::runtime_error("High Contrast text is under 7:1 on one of its faces");
            }
        }
        if (!high.edge || contrast(*high.edge, high.surface) < 7 || !high.edge_hover ||
            contrast(*high.edge_hover, high.top) < 7) {
            throw std::runtime_error("High Contrast key edges must stand out from the case and keys");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
