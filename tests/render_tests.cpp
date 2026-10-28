#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
                    throw std::runtime_error(std::string(phase) + ": pixel " + std::to_string(x) + "," +
                                             std::to_string(y) + " is not premultiplied");
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
    // Invisible and Disabled draw the same pixels, but a switch between them,
    // from the shell or a reload, must still reach the laser mask.
    view.laser_through_gaps = true;
    if (!renderer.paint(view, 1.5)) {
        throw std::runtime_error("a change of laser_through_gaps repaints, so the mask follows");
    }
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
// With Background Disabled, a top-row emoji's skin tones rise above the grid
// into the gap under the tabs; they take the laser there too.
void emoji_tone_hits() {
    const auto profiles = load_profiles({}, {});
    auto layout = profiles.layouts.at("en-us-full");
    layout.keys.clear(); // Only the picker's own areas.
    NullSink sink;
    KeyboardState keyboard(sink);
    PanelView view;
    view.layout = &layout;
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.case_opacity = 0;
    view.laser_through_gaps = true;
    EmojiPickerView picker;
    picker.width = layout.width;
    picker.tabs = {200, 70, 900, 40};
    picker.grid = {200, 150, 900, 300};
    view.emoji = picker;
    auto inside = [&](double x, double y) {
        const auto areas = panel_hit_areas(view, false, 29);
        return std::any_of(areas.begin(), areas.end(), [&](const Rect& r) { return r.contains(x, y); });
    };
    if (inside(400, 127) || !inside(400, 90) || !inside(400, 200)) {
        throw std::runtime_error("the gap between the picker's tabs and grid passes the laser");
    }
    view.emoji->popup = EmojiPickerView::Popup{{300, 105, 400, 40}, {}};
    if (!inside(400, 127)) {
        throw std::runtime_error("skin tones over that gap take the laser");
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
int main() {
    try {
        handle_states();
        const auto profiles = load_profiles({}, {});
        for (const auto& [id, language] : profiles.languages) {
            const auto layout = id == "ko-cheonjiin"                           ? "ko-12key"
                                : !language.flick.empty()                      ? "ja-flick-12"
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
                 profiles.languages.at("en-us"), semi_transparent_case_opacity);
        straight_alpha();
        emoji_tone_hits();
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
