#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
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
    void frame(const PanelView& view, const char* phase) {
        now += 1.0 / 60;
        auto* a = incremental.surface();
        const auto size = static_cast<std::size_t>(cairo_image_surface_get_stride(a)) * texture_height;
        before.assign(cairo_image_surface_get_data(a), cairo_image_surface_get_data(a) + size);
        const bool painted = incremental.paint(view, now);
        full.paint(view, now, true);
        require_damage(painted, phase);
        auto* b = full.surface();
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
}
void exercise(Layout layout, Theme theme, Language language) {
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(language);
    PanelView view;
    view.layout = &layout;
    view.theme = &theme;
    view.language = &language;
    view.keymap = &keymap;
    view.keyboard = &keyboard;
    view.controls = {{"settings", "Settings", {18, 12, 60, 42}, false, Icon::Settings}};
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
    const auto a = find_key(layout, flick ? "a" : "KeyA"), b = find_key(layout, flick ? "ka" : "KeyB");
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
    if (flick) {
        // A flick relabels only the pressed key and hides its hints.
        const auto& slot = language.flick.at("a");
        view.flick_preview =
            PanelView::FlickPreview{a.id, {slot.tap.front(), slot.left, slot.up, slot.right, slot.down}};
        keyboard.down(0, a, compare.now);
        compare.frame(view, "flick preview during press animation");
        view.hovered_keys = {b.id}; // A neighbour under the preview repaints partially.
        compare.frame(view, "hover beneath the flick preview");
        view.flick_preview->selected = 2;
        compare.settle(view, "flick direction");
        view.preedit = "う";
        compare.frame(view, "reading line repaints alone");
        view.preedit = "うか";
        compare.frame(view, "reading line grows");
        keyboard.up(0);
        view.flick_preview.reset();
        view.hovered_keys.clear();
        view.key_labels[a.id] = language.flick.at("a").tap.front();
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
    view.settings = true;
    compare.frame(view, "settings");
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
    std::cout << language.id << ": " << compare.frames << " frames pixel-identical\n";
}
} // namespace
int main() {
    try {
        handle_states();
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
