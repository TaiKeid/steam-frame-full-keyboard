#include "framekeyboard/app.hpp"
#include "framekeyboard/config.hpp"
#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unistd.h>
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
        // Drawn afresh: a key body kept past a change it should have
        // followed shows up as a difference.
        full.forget_key_bodies();
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
    // are, so over a clear case no color channel may exceed its alpha.
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
// Typing suggestions and swipe alternatives are caps, as a composing
// language's candidates are: the keys' gradient rather than a flat fill, and
// partial repaints that match whole ones as they come, light up and go.
void suggestion_caps() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    Control typed{"typing-suggestion-0", "type", {520, 12, 140, 42}};
    typed.action.kind = ControlKind::TypingSuggestion;
    Control swiped{"swipe-candidate-0", "swipe", {680, 12, 140, 42}};
    swiped.action.kind = ControlKind::SwipeCandidate;
    for (const char* theme : {"graphite", "porcelain", "neon", "contrast"}) {
        view.theme = &profiles.themes.at(theme);
        view.controls = {typed};
        view.hovered_controls.clear();
        Comparison compare;
        compare.frame(view, "a typing suggestion");
        if (std::string_view(theme) == "graphite") {
            // Left of the centered word, near the cap's top and near its bottom.
            const auto shade = [&](double y) {
                const auto pixels = compare.incremental.rgba();
                const auto offset =
                    static_cast<std::size_t>((static_cast<int>(y) + popup_margin) * panel_width +
                                             static_cast<int>(typed.bounds.x) + 12) *
                    4;
                return pixels[offset] + pixels[offset + 1] + pixels[offset + 2];
            };
            if (shade(typed.bounds.y + 8) == shade(typed.bounds.y + typed.bounds.height - 8)) {
                throw std::runtime_error("a typing suggestion is flat instead of a cap");
            }
        }
        swiped.selected = true;
        view.controls = {typed, swiped};
        compare.frame(view, "swipe alternatives shown");
        view.hovered_controls = {typed.id};
        compare.frame(view, "laser on a suggestion");
        view.controls = {typed};
        view.hovered_controls.clear();
        compare.frame(view, "swipe alternatives gone");
        swiped.selected = false;
    }
}
// A slow laser on High Contrast: points a pixel apart, so each segment is far
// shorter than its casing's round cap. The casing goes under the path drawn
// before, which stays yellow instead of turning black.
void slow_trail() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("contrast");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    Comparison compare;
    compare.frame(view, "before the swipe");
    const auto key = panel_rect(view, find_key(*view.layout, "KeyA").bounds);
    // Below the legend, from left of the key's middle to past its right edge.
    const double y = std::round(key.y + key.height / 2 + 16), start = key.x + key.width / 2 - 10;
    for (int i = 0; i < 120; ++i) {
        view.swipe_trail.emplace_back(start + i, y);
        compare.frame(view, "a slow swipe");
    }
    for (int i = 0; i < 30; ++i) {
        compare.frame(view, "the comet burns out");
    }
    const auto x = static_cast<int>(start + 28);
    const auto offset =
        static_cast<std::size_t>((static_cast<int>(y) + popup_margin) * panel_width + x) * 4;
    const auto pixel = compare.incremental.rgba();
    if (pixel[offset] < 150 || pixel[offset + 1] < 150) {
        throw std::runtime_error("a slow High Contrast path stays yellow: " +
                                 std::to_string(pixel[offset]) + " red on the path");
    }
}
// The swipe trail: a quiet path that only grows, a comet over its last
// moments, and a fade when the swipe ends. Every frame also paints the same
// incrementally as in full.
// Also on High Contrast, whose path has a black casing and comet a black halo.
void swipe_trail(const char* theme) {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at(theme);
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
                    throw std::runtime_error("a clear pixel has no color");
                }
                continue;
            }
            ++partial;
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned premultiplied = (row[x] >> (16 - 8 * channel)) & 255u;
                // Back to straight color, within the rounding of 8-bit premultiplication.
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
            // The case-colored gutters stay between the preview's caps, so
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
    // A held modifier's lock ring, in every theme: hidden for 0.15 s, then going
    // round until 0.6 s, with the lock at 0.5 s; then the locked key's border,
    // and a tap that unlocks it. Steps of 1/30 s follow the ring's 30 fps steps.
    if (language.id == "en-us" && !transparent && has_key(layout, "AltLeft")) {
        const auto alt = find_key(layout, "AltLeft");
        keyboard.down(0, alt, compare.now);
        for (int i = 0; i < 26; ++i) {
            compare.now += 1.0 / 30;
            keyboard.tick(compare.now);
            compare.frame(view, "lock ring");
        }
        keyboard.up(0);
        compare.settle(view, "locked modifier");
        keyboard.down(0, alt, compare.now);
        keyboard.up(0);
        compare.settle(view, "unlocked modifier");
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
// The rules for repainting only theme cards, on views alone (R300): which
// changes keep to the cards, and which fall back to a full repaint.
void theme_damage_rules() {
    PanelView before;
    before.settings = true;
    PanelView::ThemesView picker;
    picker.panel = themes_popup_bounds;
    picker.well = themes_well_bounds;
    picker.title = "Theme";
    for (int i = 0; i < 3; ++i) {
        PanelView::ThemesView::Card card;
        card.id = "t" + std::to_string(i);
        card.bounds = {100.0 + 400 * i, 100, 365, 212};
        picker.cards.push_back(card);
        before.controls.push_back({std::string(theme_card_prefix) + card.id, card.id, card.bounds});
    }
    before.controls.push_back({"themes-close", "Back", {30, 18, 60, 42}});
    before.themes = picker;
    auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    require(theme_cards_damage(before, before).empty() && theme_hover_damage(before, before).empty(),
            "nothing changed, nothing to repaint");
    auto now = before;
    now.themes->cards[1].preview.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1),
                                       cairo_surface_destroy);
    const auto pictures = theme_cards_damage(before, now);
    require(pictures.size() == 1 && pictures[0].x <= 500 && pictures[0].x + pictures[0].width >= 865,
            "a picture arriving repaints its card");
    now = before;
    for (auto& card : now.themes->cards) {
        card.bounds.y -= 30;
    }
    for (auto& control : now.controls) {
        if (control.id.starts_with(theme_card_prefix)) {
            control.bounds.y -= 30;
        }
    }
    const auto scrolled = theme_cards_damage(before, now);
    require(scrolled.size() == 1 && scrolled[0].width >= themes_well_bounds.width,
            "a scroll repaints the well");
    now = before;
    now.themes->title = "Other";
    require(theme_cards_damage(before, now).empty(), "anything else in the picker repaints whole");
    now = before;
    now.themes->cards[0].preview = before.themes->cards[0].preview;
    now.controls.back().label = "Close";
    now.themes->cards[2].preview.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1),
                                       cairo_surface_destroy);
    require(theme_cards_damage(before, now).empty(), "another control changing repaints whole");
    auto hovered = before;
    hovered.hovered_controls = {std::string(theme_card_prefix) + "t0"};
    now = before;
    now.hovered_controls = {std::string(theme_card_prefix) + "t2"};
    require(theme_hover_damage(hovered, now).size() == 2, "a hover moving between cards repaints both");
    now.hovered_controls.insert("themes-close");
    require(theme_hover_damage(hovered, now).empty(), "a hover on anything else repaints whole");
}
// The theme picker's cards repaint alone (R287): a picture that arrives
// repaints its card, a scroll the well, never the Settings screen under them;
// and Settings draws no card controls under the popup.
void theme_cards() {
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
    view.controls.push_back(
        {"themes-close", "Back", {themes_popup_bounds.x + 16, themes_popup_bounds.y + 10, 60, 42}});
    PanelView::ThemesView popup;
    popup.panel = themes_popup_bounds;
    popup.well = themes_well_bounds;
    popup.title = "Theme";
    std::vector<const Theme*> themes;
    for (const auto& [id, each] : profiles.themes) {
        themes.push_back(&each);
    }
    auto place = [&](double scroll) {
        popup.cards.clear();
        std::erase_if(view.controls, [](const Control& c) { return c.id.starts_with("themes-card-"); });
        for (std::size_t i = 0; i < 12; ++i) {
            PanelView::ThemesView::Card card;
            card.id = themes[i]->id;
            card.name = themes[i]->name;
            card.surface = themes[i]->surface;
            card.bounds = {themes_well_bounds.x + theme_card_pad +
                               static_cast<double>(i % 4) * (theme_card_width + theme_card_gap),
                           themes_well_bounds.y + theme_card_pad +
                               static_cast<double>(i / 4) * (theme_card_height + theme_card_gap) -
                               scroll,
                           theme_card_width, theme_card_height};
            view.controls.push_back({"themes-card-" + card.id, card.name, card.bounds});
            popup.cards.push_back(card);
        }
        popup.thumb = Rect{themes_well_bounds.x + themes_well_bounds.width - 11,
                           themes_well_bounds.y + 8 + scroll / 4, 6, 200};
    };
    place(0);
    view.themes = popup;
    Comparison compare;
    compare.frame(view, "picker open");
    {
        // Settings under the picker is hidden whole, so it is not drawn:
        // with or without its cards, labels and rows the picture is the same
        // (R293).
        auto covered = view;
        covered.cards = {{"General", {18, 78, 760, 476}}, {"Typing", {800, 78, 760, 476}}};
        FieldLabel label;
        label.text = "Theme";
        label.bounds = {48, 140, 300, 52};
        label.clip = {18, 130, 760, 400};
        label.left = true;
        covered.field_labels = {label};
        covered.controls.push_back({"theme", "Graphite", {420, 140, 320, 52}});
        PanelRenderer with, without;
        with.paint(covered, 1, true);
        without.paint(view, 1, true);
        if (with.rgba() != without.rgba()) {
            throw std::runtime_error("Settings hidden under the picker changes no pixel");
        }
    }
    // Drawn under the popup, the cards would show through where it is clipped.
    const auto area = [&] {
        double total = 0;
        for (const auto& r : compare.incremental.damage()) {
            total += r.width * r.height;
        }
        return total;
    };
    std::shared_ptr<cairo_surface_t> picture(
        render_theme_preview(theme, layout, language, keymap, theme_preview_width, theme_preview_height),
        cairo_surface_destroy);
    view.themes->cards[5].preview = picture;
    compare.frame(view, "a picture arrives");
    // Damage is in texture rows, popup_margin below the panel's; none at all
    // means a full repaint.
    const auto card = view.themes->cards[5].bounds;
    if (area() == 0) {
        throw std::runtime_error("a picture that arrives repaints its card, not everything");
    }
    for (const auto& r : compare.incremental.damage()) {
        if (r.x < card.x - 20 || r.y < card.y + popup_margin - 20 ||
            r.x + r.width > card.x + card.width + 20 ||
            r.y + r.height > card.y + card.height + popup_margin + 20) {
            throw std::runtime_error("a picture that arrives repaints more than its card");
        }
    }
    place(37);
    for (auto& each : popup.cards) {
        each.preview = view.themes->cards[&each - popup.cards.data()].preview;
    }
    view.themes = popup;
    compare.frame(view, "the cards scroll");
    if (area() == 0 || area() > themes_well_bounds.width * themes_well_bounds.height * 1.2) {
        throw std::runtime_error("a scroll repaints the well, not Settings");
    }
    // The laser moving from one card to another repaints those two cards,
    // not the screen (R292).
    {
        const auto& from = view.themes->cards[1];
        const auto& to = view.themes->cards[6];
        view.hovered_controls = {"themes-card-" + from.id};
        compare.frame(view, "the laser on a card");
        view.hovered_controls = {"themes-card-" + to.id};
        compare.frame(view, "the laser on another card");
        if (area() == 0) {
            throw std::runtime_error("a hover moving between cards repaints its cards, not everything");
        }
        const auto near = [](const Rect& r, const Rect& card) {
            return r.x >= card.x - 20 && r.y >= card.y + popup_margin - 20 &&
                   r.x + r.width <= card.x + card.width + 20 &&
                   r.y + r.height <= card.y + card.height + popup_margin + 20;
        };
        for (const auto& r : compare.incremental.damage()) {
            if (!near(r, from.bounds) && !near(r, to.bounds)) {
                throw std::runtime_error(
                    "a hover moving between cards repaints more than its two cards");
            }
        }
        view.hovered_controls.clear();
        compare.frame(view, "the laser leaves the cards");
    }
    // Every frame below is compared with a full repaint (R299).
    const auto card_control = [&](const std::string& id) -> Control& {
        return *std::find_if(view.controls.begin(), view.controls.end(),
                             [&](const Control& c) { return c.id == "themes-card-" + id; });
    };
    // A card cut off at the well's top: its box reaches the Back button and the dim.
    view.themes->cards[0].preview = picture;
    compare.frame(view, "a picture for a card cut off at the top");
    // The selection moving, and a card's color changing.
    view.themes->cards[2].selected = true;
    card_control(view.themes->cards[2].id).selected = true;
    compare.frame(view, "the selected card changes");
    view.themes->cards[3].surface = Color{.5, 0, 0};
    compare.frame(view, "a card's color changes");
    // A question over the picker, a picture arriving under it, and its end.
    PanelView::ConfirmView question;
    question.panel = {420, 180, 760, 300};
    question.title = "Leave Settings?";
    question.body = "Changes are not saved.";
    view.confirm = question;
    view.controls.push_back({"confirm-ok", "Leave", {960, 400, 180, 52}});
    compare.frame(view, "a question over the picker");
    view.themes->cards[7].preview = picture;
    compare.frame(view, "a picture arrives under the question");
    view.confirm.reset();
    std::erase_if(view.controls, [](const Control& c) { return c.id.starts_with("confirm-"); });
    compare.frame(view, "the question goes");
    // Hover and scroll at once.
    const auto previews = view.themes->cards;
    place(60);
    for (std::size_t i = 0; i < popup.cards.size(); ++i) {
        popup.cards[i].preview = previews[i].preview;
        popup.cards[i].selected = previews[i].selected;
        popup.cards[i].surface = previews[i].surface;
        card_control(popup.cards[i].id).selected = previews[i].selected;
    }
    view.themes = popup;
    view.hovered_controls = {"themes-card-" + popup.cards[1].id};
    compare.frame(view, "hover and scroll at once");
    view.hovered_controls.clear();
    compare.frame(view, "the hover ends");
    view.themes.reset();
    std::erase_if(view.controls, [](const Control& c) { return c.id.starts_with("themes-"); });
    compare.frame(view, "picker closed");
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
// No key's drawing reaches into another's: a legend's text leaves Cairo's
// current point behind, and the flick layout's emoji icon, drawn next with a
// bare arc, used to join it with a line across the lower left of ← above it.
void no_stray_lines() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("ja-flick"));
    PanelView view;
    view.layout = &profiles.layouts.at("ja-flick-12");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("ja-flick");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    PanelRenderer renderer;
    renderer.paint(view, 1, true);
    // The ← key's extent in the case's pixels, as hit_key takes them.
    int left = panel_width, top = panel_height, right = 0, bottom = 0;
    for (int y = 0; y < panel_height; y += 2) {
        for (int x = 0; x < panel_width; x += 2) {
            if (const auto* key = renderer.hit_key(view, x, y); key && key->id == "ArrowLeft") {
                left = std::min(left, x), top = std::min(top, y);
                right = std::max(right, x), bottom = std::max(bottom, y);
            }
        }
    }
    if (right <= left || bottom <= top) {
        throw std::runtime_error("no stray lines: the flick layout has no ← key");
    }
    // Its lower left quarter holds nothing but the dark face: no legend ink.
    // The texture has room for popups above the case.
    auto* surface = renderer.surface();
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    int bright = 0;
    for (int y = top + (bottom - top) * 6 / 10; y < bottom - 6; ++y) {
        for (int x = left + 8; x < left + (right - left) * 4 / 10; ++x) {
            const auto pixel =
                *reinterpret_cast<const std::uint32_t*>(data + (y + popup_margin) * stride + x * 4);
            const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
            bright += r + g + b > 3 * 150;
        }
    }
    if (bright) {
        renderer.write_png("/tmp/framekeyboard-render-stray.png");
        throw std::runtime_error("no stray lines: " + std::to_string(bright) +
                                 " bright pixels in the flick ← key's lower left");
    }
}
// The shortcut bar (docs/shortcut-bar-design.md): a button's hover, press
// and light, a scroll and the menu's rows repaint only their own areas, each
// frame the same as a full repaint, in a dark and a light theme, over an
// opaque and a see-through case. The tooltip and the status card stand above
// the bar.
void shortcut_bar(const char* theme_id, double opacity) {
    const auto profiles = load_profiles({}, {});
    const auto& layout = profiles.layouts.at("en-us-full");
    const auto& theme = profiles.themes.at(theme_id);
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
    view.case_opacity = opacity;
    view.controls = {{"settings", "Settings", {18, 12, 60, 42}, false, Icon::Settings}};
    const std::vector<std::string> labels{"Undo",
                                          "Redo",
                                          "Save\nall",
                                          "Find",
                                          "A label far too long",
                                          "Two\nlines here",
                                          "Copy",
                                          "Paste",
                                          "Cut",
                                          "Build",
                                          "Run",
                                          "Term",
                                          "Zen\nmode",
                                          "Close",
                                          "Split",
                                          "Last",
                                          "Past the end",
                                          "→"};
    const auto g = shortcut_bar_geometry(view, labels.size());
    struct State {
        double scroll{};
        int hovered{-1}, pressed{-1}, running{-1}, disabled{-1};
        bool picker_hovered{}, menu{};
        int row_hovered{-1};
    };
    const auto bar = [&](const State& state) {
        ShortcutBarView result;
        result.plate = g.plate;
        result.strip = g.strip;
        result.scale = g.button_width / 69.5;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const Rect r{g.strip.x + static_cast<double>(i) * g.pitch - state.scroll, g.button_top,
                         g.button_width, g.button_height};
            if (r.x + r.width <= g.strip.x || r.x >= g.strip.x + g.strip.width) {
                continue;
            }
            const int at = static_cast<int>(i);
            result.buttons.push_back({labels[i], r, at == state.hovered, at == state.pressed,
                                      at != state.disabled, at == state.running});
        }
        result.more_before = state.scroll > .5;
        result.more_after = state.scroll < g.scroll_limit - .5;
        result.picker = g.picker;
        result.picker_button = g.picker_button;
        result.picker_hovered = state.picker_hovered;
        if (state.menu) {
            const auto m = shortcut_menu_geometry(g, 8);
            ShortcutBarView::Menu menu;
            menu.bounds = m.bounds;
            menu.header = m.header;
            menu.list = m.list;
            menu.edit = m.edit;
            menu.title = "Bars";
            menu.edit_label = "Edit shortcuts";
            for (std::size_t i = 0; i < m.visible; ++i) {
                const auto row = m.row(i, 0);
                const int at = static_cast<int>(i);
                menu.rows.push_back({"Bar " + std::to_string(i), row, m.assign(row), i == 0,
                                     at == state.row_hovered, false, false, false, false});
            }
            menu.thumb = Rect{m.bounds.x + m.bounds.width - 6, m.list.y, 3, 60};
            result.menu = menu;
        }
        return result;
    };
    const auto within = [](const std::vector<Rect>& damage, const Rect& area) {
        return !damage.empty() && std::all_of(damage.begin(), damage.end(), [&](const Rect& r) {
            return r.x >= area.x - 2 && r.x + r.width <= area.x + area.width + 2 &&
                   r.y >= area.y + popup_margin - 2 &&
                   r.y + r.height <= area.y + area.height + popup_margin + 2;
        });
    };
    const std::string name = std::string(theme_id) + (opacity < 1 ? " see-through" : "");
    const auto phase = [&](const char* what) { return name + ": " + what; };
    Comparison compare;
    compare.frame(view, phase("keyboard alone").c_str());
    view.shortcut_bar = bar({});
    compare.frame(view, phase("the bar appears").c_str());
    if (opacity == 1 && alpha_at(compare.incremental, static_cast<int>(g.plate.x + 6),
                                 static_cast<int>(g.plate.y + g.plate.height / 2)) != 255) {
        throw std::runtime_error(phase("the plate is the case's color, opaque"));
    }
    // The plate and the picker beside it: a state repaints only within them.
    const Rect both{g.plate.x, g.plate.y, g.picker.x + g.picker.width - g.plate.x, g.plate.height};
    for (const auto& state : {State{0, 2}, State{0, 3}, State{0, 3, 3}, State{0, -1, -1, 3},
                              State{0, -1, -1, -1, 4}, State{0, -1, -1, -1, -1, true}, State{}}) {
        view.shortcut_bar = bar(state);
        compare.frame(view, phase("a button's state").c_str());
        if (!within(compare.incremental.damage(), both)) {
            throw std::runtime_error(phase("a button's state repaints only the bar"));
        }
    }
    for (const double scroll : {g.pitch * .4, g.pitch, g.scroll_limit}) {
        view.shortcut_bar = bar({scroll});
        compare.frame(view, phase("scrolling").c_str());
        if (!within(compare.incremental.damage(), g.plate)) {
            throw std::runtime_error(phase("a scroll repaints only the strip"));
        }
    }
    view.shortcut_bar = bar({0, -1, -1, -1, -1, false, true});
    compare.frame(view, phase("the menu opens").c_str());
    // The menu lies over keys: its rows repaint with everything.
    for (const int row : {2, 3, -1}) {
        view.shortcut_bar = bar({0, -1, -1, -1, -1, false, true, row});
        compare.frame(view, phase("a row's hover").c_str());
    }
    // A button's hover under the open menu still repaints only the bar.
    view.shortcut_bar = bar({0, 1, -1, -1, -1, false, true, -1});
    compare.frame(view, phase("a button's hover with the menu open").c_str());
    if (!within(compare.incremental.damage(), g.plate)) {
        throw std::runtime_error(phase("a button's hover with the menu open repaints only the bar"));
    }
    view.shortcut_bar = bar({});
    compare.frame(view, phase("the menu closes").c_str());
    // The picker's tooltip and a message stand above the bar.
    view.bar_tooltip = "Choose a bar";
    view.bar_tooltip_at = g.picker_button;
    compare.frame(view, phase("the tooltip").c_str());
    const auto above = [&](double x) {
        return alpha_at(compare.incremental, static_cast<int>(x), static_cast<int>(g.plate.y - 33)) >
               200;
    };
    if (!above(g.picker_button.x + g.picker_button.width / 2)) {
        throw std::runtime_error(phase("the tooltip stands above the bar"));
    }
    view.status = "A message that comes while the bar shows";
    compare.frame(view, phase("a message with the tooltip").c_str());
    view.bar_tooltip.clear();
    compare.frame(view, phase("the message alone").c_str());
    if (!above(panel_case_bounds(view).x + panel_case_bounds(view).width / 2)) {
        throw std::runtime_error(phase("the status card stands above the bar"));
    }
    view.status.clear();
    view.shortcut_bar.reset();
    compare.frame(view, phase("the bar goes").c_str());
    if (alpha_at(compare.incremental, static_cast<int>(g.plate.x + 6),
                 static_cast<int>(g.plate.y + g.plate.height / 2)) != 0) {
        throw std::runtime_error(phase("nothing is left where the bar was"));
    }
}
// The shortcut editor's screen, a lifted button and the keyboard's field
// strip and Label field paint the same incrementally as in full.
void shortcut_editor(const char* theme_id, bool wide) {
    const auto profiles = load_profiles({}, {});
    const auto& layout = profiles.layouts.at("en-us-full");
    const auto& theme = profiles.themes.at(theme_id);
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
    view.hide_numpad = !wide;
    const double width = wide ? panel_width : 1326, x0 = (panel_width - width) / 2;
    ShortcutEditorView e;
    e.width = width;
    e.wide = wide;
    e.title = "Edit shortcuts";
    e.notice = "shortcuts.json can't be read (bar 2: the ID is used by another bar).";
    e.bars_caption = "Bars";
    e.column = {x0 + 18, 70, wide ? 286.0 : 236.0, 500};
    e.bars_list = {x0 + 18, 104, e.column.width, 156};
    e.divider = e.column.x + e.column.width + 16;
    e.used = {e.column.x + 12, 420, e.column.width - 12, 150};
    e.used_caption = "Used for";
    e.used_hint = "Shows when one of these apps has the keys.";
    e.ties = {{"Kate", {e.used.x, 454, 110, 40}}};
    const double left = e.divider + 16, right = x0 + width - 18;
    e.strip_well = {left, 70, right - left, 102};
    e.strip = {left + 14, 70, right - left - 28, 102};
    e.scale = 1.054;
    e.strip_hint = "Drag to reorder, drop outside to delete";
    e.label_caption = "Label";
    e.action_caption = "Action";
    e.keys_caption = "Keys";
    e.chords = {{"Ctrl + K", {left + 100, 280, 120, 52}}, {"S", {left + 248, 280, 50, 52}}};
    e.explanation = {"Sends Ctrl + K, then S, once per press.",
                     "Record takes the keys on the keyboard."};
    e.explanation_bounds = {left, 372, right - left, 60};
    std::vector<Control> controls{{"shortcuts-back", "", {x0 + 18, 12, 60, 42}, false, Icon::Back},
                                  {"shortcuts-save", "", {x0 + 88, 12, 60, 42}, false, Icon::Apply}};
    for (int i = 0; i < 3; ++i) {
        Control row{"shortcuts-row-" + std::to_string(i),
                    "Bar " + std::to_string(i),
                    {e.column.x, 106 + 52.0 * i, e.column.width, 48},
                    i == 1};
        row.clip = e.bars_list;
        controls.push_back(row);
    }
    controls.push_back(
        {"shortcuts-rename", "", {e.column.x + e.column.width - 92, 156, 44, 52}, false, Icon::Pencil});
    controls.push_back(
        {"shortcuts-delete", "", {e.column.x + e.column.width - 48, 156, 44, 52}, false, Icon::Clear});
    controls.push_back({"shortcuts-new-bar", "New bar", {e.column.x, 260, e.column.width, 52}});
    controls.push_back({"shortcuts-tie-0", "", {e.used.x + 72, 454, 34, 40}});
    for (int i = 0; i < 4; ++i) {
        Control button{"shortcuts-button-" + std::to_string(i),
                       i == 1 ? "Save\nall" : "Undo",
                       {e.strip.x + 79.6 * i, 84, 73.3, 65.4},
                       i == 0};
        button.clip = e.strip;
        controls.push_back(button);
    }
    Control plus{"shortcuts-add", "", {e.strip.x + 79.6 * 4, 84, 73.3, 65.4}};
    plus.clip = e.strip;
    controls.push_back(plus);
    Control label{"shortcuts-label", "Save\nall", {left + 100, 196, wide ? 300.0 : 250.0, 56}};
    label.style = ControlStyle::TextField;
    controls.push_back(label);
    for (int i = 0; i < 3; ++i) {
        Control segment{"shortcuts-kind-" + std::to_string(i),
                        i == 0   ? "Shortcut"
                        : i == 1 ? "Macro"
                                 : "Run",
                        {label.bounds.x + label.bounds.width + 160 + 110.0 * i, 196, 110, 56},
                        i == 0};
        segment.style = ControlStyle::Segment;
        segment.segments = 3;
        segment.action.index = i;
        segment.enabled = i == 0;
        controls.push_back(segment);
    }
    controls.push_back({"shortcuts-record", "Record", {left + 322, 280, 160, 52}});
    controls.push_back({"shortcuts-clear", "", {left + 494, 280, 60, 52}, false, Icon::Clear});
    view.controls = controls;
    view.shortcut_editor = e;
    const std::string name = std::string(theme_id) + (wide ? "" : " no numpad");
    const auto phase = [&](const char* what) { return name + ": " + what; };
    Comparison compare;
    compare.frame(view, phase("the editor").c_str());
    view.hovered_controls = {"shortcuts-button-2"};
    compare.frame(view, phase("a hovered button").c_str());
    view.hovered_controls = {"shortcuts-row-2"};
    compare.frame(view, phase("a hovered row").c_str());
    for (const double dx : {0.0, 40.0, 120.0}) {
        view.shortcut_editor->lifted_button =
            ShortcutChip{"Undo", {e.strip.x + 20 + dx, 100, 73.3, 65.4}};
        compare.frame(view, phase("a lifted button").c_str());
    }
    view.shortcut_editor->lifted_button = ShortcutChip{"Undo", {e.strip.x + 60, 260, 73.3, 65.4}};
    view.shortcut_editor->lifted_removing = true;
    compare.frame(view, phase("a button off the strip").c_str());
    view.shortcut_editor->lifted_button.reset();
    view.shortcut_editor->lifted_row = ShortcutChip{"Bar 0", {e.column.x, 140, e.column.width, 48}};
    compare.frame(view, phase("a lifted row").c_str());
    // The keyboard with the recording strip, then the Label field on the bar.
    view.shortcut_editor.reset();
    view.hovered_controls.clear();
    const auto body = panel_case_bounds(view);
    ShortcutFieldView field;
    field.kind = ShortcutFieldView::Kind::Keys;
    field.strip = {body.x + 18, 12, body.width - 36, 42};
    field.title = "Recording";
    field.chords = {{"Ctrl + K", {body.x + 192, 12, 120, 42}}};
    field.hint = "Press the next key, or Done";
    field.hint_bounds = {body.x + 340, 12, 300, 42};
    field.fkeys = Rect{body.x + body.width - 650, 62, 476, 116};
    view.shortcut_field = field;
    view.controls = {{"field-done", "Done", {body.x + body.width - 168, 12, 150, 42}, true},
                     {"field-cancel", "Cancel", {body.x + body.width - 308, 12, 130, 42}},
                     {"field-fkey-13", "F13", {body.x + body.width - 642, 70, 70, 46}}};
    compare.frame(view, phase("the recording strip").c_str());
    view.shortcut_field->chords.push_back({"S", {body.x + 340, 12, 50, 42}});
    compare.frame(view, phase("a combination recorded").c_str());
    const auto bar_geometry = shortcut_bar_geometry(view, 3);
    ShortcutBarView bar;
    bar.plate = bar_geometry.plate;
    bar.strip = bar_geometry.strip;
    bar.scale = bar_geometry.button_width / 69.5;
    for (int i = 0; i < 3; ++i) {
        bar.buttons.push_back({"Undo",
                               {bar_geometry.strip.x + i * bar_geometry.pitch, bar_geometry.button_top,
                                bar_geometry.button_width, bar_geometry.button_height}});
    }
    bar.picker = bar_geometry.picker;
    bar.picker_button = bar_geometry.picker_button;
    bar.field = 1;
    bar.field_text = "Save\nal";
    bar.field_cursor = 8;
    view.shortcut_bar = bar;
    view.shortcut_field->kind = ShortcutFieldView::Kind::Label;
    view.shortcut_field->chords.clear();
    view.shortcut_field->fkeys.reset();
    view.bar_tooltip = "Enter saves · Esc cancels · Shift+Enter starts a second line";
    view.bar_tooltip_at = bar.buttons[1].bounds;
    compare.frame(view, phase("the Label field").c_str());
    view.shortcut_bar->field_text = "Save\nall";
    view.shortcut_bar->field_cursor = 9;
    compare.frame(view, phase("a letter typed").c_str());
    view.shortcut_field->kind = ShortcutFieldView::Kind::Name;
    view.shortcut_field->field = {body.x + 192, 12, 600, 42};
    view.shortcut_field->text = "Editor";
    view.shortcut_field->cursor = 6;
    view.shortcut_bar.reset();
    compare.frame(view, phase("the bar name").c_str());
}
// R328: what the App puts outside the shortcut editor is drawn. The bar's
// toolbar button, with the bar shown and hidden, and the note that
// shortcuts.json can't be read; no control outside the editor carries its
// prefix, which the drawing pass leaves to the editor.
struct ConfigFolder {
    std::filesystem::path path;
    ConfigFolder() {
        std::string name = "/tmp/framekeyboard-render-tests-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("temporary directory");
        }
        path = name;
    }
    ~ConfigFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
// Whether a rectangle of the painted panel is one flat color: nothing drawn.
bool flat(const PanelRenderer& renderer, Rect r) {
    auto* surface = renderer.surface();
    const int stride = cairo_image_surface_get_stride(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const auto at = [&](int x, int y) {
        return reinterpret_cast<const std::uint32_t*>(data + (y + popup_margin) * stride)[x];
    };
    const int x0 = static_cast<int>(std::ceil(r.x)) + 2, y0 = static_cast<int>(std::ceil(r.y)) + 2;
    const int x1 = static_cast<int>(r.x + r.width) - 2, y1 = static_cast<int>(r.y + r.height) - 2;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (at(x, y) != at(x0, y0)) {
                return false;
            }
        }
    }
    return true;
}
void outside_editor(const PanelView& view, const std::string& what) {
    if (view.shortcut_editor) {
        return;
    }
    for (const auto& control : view.controls) {
        if (control.id.starts_with(shortcut_editor_prefix)) {
            throw std::runtime_error(what + ": " + control.id + " carries the shortcut editor's prefix");
        }
    }
}
const Control& control_named(const PanelView& view, const std::string& id) {
    for (const auto& control : view.controls) {
        if (control.id == id) {
            return control;
        }
    }
    throw std::runtime_error("missing control " + id);
}
void app_controls_drawn() {
    ConfigFolder folder;
    Settings settings;
    settings.shortcuts = true;
    save_settings(folder.path, settings);
    ShortcutModel model;
    ShortcutButton copy;
    copy.label = "Copy";
    copy.action.chords = {{{"ControlLeft"}, "KeyC"}};
    model.bars.push_back({"b", "Bar", {copy}});
    model.free_bar = "b";
    std::ofstream(shortcuts_path(folder.path)) << serialize_shortcuts(model);
    Options options;
    options.config_dir = folder.path;
    options.mode = Mode::Render;
    NullSink sink;
    App app(options, sink);
    const auto drawn = [&](const std::string& what) {
        const auto view = app.view();
        outside_editor(view, what);
        PanelRenderer renderer;
        renderer.paint(view, 1);
        if (flat(renderer, control_named(view, "bar-toggle").bounds)) {
            throw std::runtime_error(what + ": the shortcut bar's toolbar button is not drawn");
        }
    };
    if (!app.view().shortcut_bar) {
        throw std::runtime_error("the bar shows");
    }
    drawn("bar shown");
    const auto toggle = control_named(app.view(), "bar-toggle").bounds;
    const double x = toggle.x + toggle.width / 2, y = toggle.y + toggle.height / 2;
    app.move(0, x, y);
    app.down(0, x, y, 1);
    app.up(0, x, y);
    app.move(0, 5, 600);
    if (app.view().shortcut_bar) {
        throw std::runtime_error("the toolbar button hides the bar");
    }
    drawn("bar hidden");
    // Settings with an unreadable file: its note under the switch, below
    // the General card's fold until the card scrolls.
    std::ofstream(shortcuts_path(folder.path)) << R"({"schema_version":1,"bars":[{}]})";
    App invalid(options, sink);
    invalid.show_settings();
    for (int step = 0; step < 40; ++step) {
        const auto view = invalid.view();
        outside_editor(view, "Settings");
        const auto card = std::find_if(view.cards.begin(), view.cards.end(),
                                       [](const auto& c) { return c.title == "General"; });
        if (card == view.cards.end()) {
            throw std::runtime_error("the General card is in view");
        }
        const auto note =
            std::find_if(view.field_labels.begin(), view.field_labels.end(), [](const auto& l) {
                return l.text.starts_with("Your shortcuts file can't be read");
            });
        if (note != view.field_labels.end() && note->bounds.y >= note->clip.y &&
            note->bounds.y + 22 <= note->clip.y + note->clip.height) {
            PanelRenderer renderer;
            renderer.paint(view, 1);
            if (flat(renderer, {note->bounds.x, note->bounds.y, 200, 22})) {
                throw std::runtime_error("the note that shortcuts.json can't be read is not drawn");
            }
            return;
        }
        invalid.move(0, std::max(card->bounds.x, 0.0) + 40, 300);
        invalid.scroll(0, 0, 1);
        for (int i = 0; i < 10; ++i) {
            invalid.tick(10 + step + i * .05);
        }
    }
    throw std::runtime_error("the note that shortcuts.json can't be read never came into view");
}
int main() {
    try {
        app_controls_drawn();
        shortcut_editor("graphite", true);
        shortcut_editor("porcelain", false);
        shortcut_bar("graphite", 1);
        shortcut_bar("porcelain", 1);
        shortcut_bar("graphite", .5);
        shortcut_bar("porcelain", 0);
        no_stray_lines();
        candidate_menu_resize();
        candidate_scrolling();
        handle_states();
        hover_fade();
        suggestion_caps();
        swipe_trail("graphite");
        swipe_trail("contrast");
        swipe_trail("ember"); // a trail in its own colors
        slow_trail();
        theme_damage_rules();
        theme_cards();
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
        // Notice colors default by the legend's lightness; a theme may set its own.
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
            throw std::runtime_error("notice colors follow the legend unless the theme sets them");
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
                throw std::runtime_error("alert icon color is not distinct in theme " + id);
            }
        }
        const auto alerted = parse_theme(base + R"("legend":"#202020","alert":"#00ff00"})");
        if (!(dark.alert == dark.warning && light.alert != light.warning &&
              alerted.alert == Color{0, 1, 0})) {
            throw std::runtime_error("the alert color follows the legend unless the theme sets it");
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
              edged.pressed == Color{1, 1, 0} && edged.pressed_top == *edged.pressed &&
              edged.pressed_bottom == *edged.pressed && edged.pressed_legend == Color{})) {
            throw std::runtime_error(
                "edge and pressed tokens are optional, and edge_hover follows edge");
        }
        // Trail colors, and a pressed gradient, come only from a theme that sets them.
        const auto trailed =
            parse_theme(base + R"("legend":"#ffffff","pressed":"#ff8000","pressed_top":"#ffff00",)"
                               R"("trail":"#0000ff","comet":"#ff0000","comet_tail":"#000000"})");
        if (!(!dark.trail && !dark.comet && !dark.comet_head && !dark.comet_tail &&
              trailed.trail == Color{0, 0, 1} && trailed.comet == Color{1, 0, 0} &&
              !trailed.comet_head && trailed.comet_tail == Color{} &&
              trailed.pressed_top == Color{1, 1, 0} && trailed.pressed_bottom == *trailed.pressed)) {
            throw std::runtime_error("trail, comet and pressed gradient tokens are optional");
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
