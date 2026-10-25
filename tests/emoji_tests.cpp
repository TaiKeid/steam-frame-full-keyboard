#include "framekeyboard/app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-emoji-tests-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
bool overlaps(const Rect& a, const Rect& b) {
    return std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x) > .001 &&
           std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y) > .001;
}
std::pair<double, double> center(const Rect& r) {
    return {r.x + r.width / 2, r.y + r.height / 2};
}

void table_tests() {
    const auto& catalog = bundled_emoji();
    require(catalog.entries.size() == 1898, "bundled table has every fully-qualified emoji");
    require(catalog.entries.front().base == "😀" && catalog.entries.front().group == EmojiGroup::Smileys,
            "table starts in CLDR order");
    require(catalog.entries.back().group == EmojiGroup::Flags, "flags come last");
    const auto* wave = catalog.find("👋");
    require(wave && wave->toned() && wave->text(3) == "👋🏽" && wave->text(0) == "👋",
            "toned emoji keep all five tones");
    const auto* holding = catalog.find("🧑‍🤝‍🧑");
    require(holding && holding->text(5) == "🧑🏿‍🤝‍🧑🏿", "people share one tone");
    require(!catalog.find("😀")->toned() && catalog.find("😀")->text(4) == "😀", "untoned ignores tone");
    for (const auto* bad : {"0\t😀\n", "1\n", "1\t😀\t🏻\n", "1\t\n"}) {
        bool threw = false;
        try {
            parse_emoji_table(bad);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "malformed table lines are rejected");
    }
}

void preference_tests() {
    TemporaryDirectory directory;
    const auto path = directory.path / "emoji.json";
    require(load_emoji_preferences(path) == EmojiPreferences{}, "missing file gives defaults");
    EmojiPreferences preferences;
    for (const auto* emoji : {"😀", "👍", "❤️", "😀"}) {
        remember_emoji(preferences, emoji);
    }
    require((preferences.recent == std::vector<std::string>{"😀", "❤️", "👍"}),
            "a typed emoji moves to the front once");
    for (int i = 0; i < 100; ++i) {
        remember_emoji(preferences, std::string("x") + std::to_string(i));
    }
    require(preferences.recent.size() == EmojiPreferences::max_recent, "recent list is capped");
    preferences.tone = 4;
    save_emoji_preferences(path, preferences);
    require(load_emoji_preferences(path) == preferences, "preferences round trip");
    std::ofstream(path) << R"({"schema_version":1,"tone":9,"recent":[]})";
    require(load_emoji_preferences(path) == EmojiPreferences{}, "an invalid tone gives defaults");
    std::ofstream(path) << "not json";
    require(load_emoji_preferences(path) == EmojiPreferences{}, "a damaged file gives defaults");
}

void layout_tests() {
    const auto profiles = load_profiles({}, {});
    for (const auto& [id, main] : profiles.layouts) {
        for (const double visible : {main.width, main.width - 260}) {
            const auto picker = emoji_layout(main, visible);
            const auto& keys = picker.layout.keys;
            require(picker.layout.width == main.width && picker.layout.height == main.height,
                    "the picker shares the keyboard's scale");
            auto find = [&](const std::string& action) {
                return std::find_if(keys.begin(), keys.end(),
                                    [&](const Key& key) { return key.action == action; });
            };
            for (const auto* action : {"Space", "Backspace", "Enter", "ArrowUp", "ArrowLeft",
                                       "ArrowDown", "ArrowRight", "copy", "paste", "picker"}) {
                require(find(action) != keys.end(), "picker has its keys");
            }
            require(find("picker")->icon == Icon::Back, "the emoji key turns into Back");
            for (std::size_t i = 0; i < keys.size(); ++i) {
                require(keys[i].bounds.x + keys[i].bounds.width <= visible + .001,
                        "keys stay within the visible width");
                require(!overlaps(keys[i].bounds, picker.grid) && !overlaps(keys[i].bounds, picker.tabs),
                        "keys stay clear of the grid and tabs");
                for (std::size_t j = i + 1; j < keys.size(); ++j) {
                    require(!overlaps(keys[i].bounds, keys[j].bounds), "picker keys do not overlap");
                }
            }
            require(!overlaps(picker.grid, picker.tabs), "tabs sit above the grid");
        }
    }
}

EmojiCatalog sample_catalog() {
    EmojiCatalog catalog;
    auto add = [&](EmojiGroup group, std::string base, bool toned) {
        EmojiEntry entry;
        entry.group = group;
        entry.base = base;
        if (toned) {
            for (int i = 0; i < 5; ++i) {
                entry.tones[static_cast<std::size_t>(i)] = base + std::to_string(i + 1);
            }
        }
        catalog.entries.push_back(entry);
    };
    for (int i = 0; i < 40; ++i) {
        add(EmojiGroup::Smileys, "s" + std::to_string(i), false);
    }
    add(EmojiGroup::People, "wave", true);
    for (int i = 0; i < 30; ++i) {
        add(EmojiGroup::Flags, "f" + std::to_string(i), false);
    }
    return catalog;
}
std::array<std::vector<const EmojiEntry*>, emoji_group_count> sections(const EmojiCatalog& catalog,
                                                                       std::size_t recent = 0) {
    std::array<std::vector<const EmojiEntry*>, emoji_group_count> result;
    for (const auto& entry : catalog.entries) {
        result[static_cast<std::size_t>(entry.group)].push_back(&entry);
    }
    for (std::size_t i = 0; i < recent; ++i) {
        result[0].push_back(&catalog.entries[i % catalog.entries.size()]);
    }
    return result;
}
const EmojiPickerView::Cell* cell(const EmojiPickerView& view, const std::string& text) {
    const auto found = std::find_if(view.cells.begin(), view.cells.end(),
                                    [&](const auto& c) { return c.text == text; });
    return found == view.cells.end() ? nullptr : &*found;
}

void picker_tests() {
    const auto catalog = sample_catalog();
    const Rect tabs{100, 100, 900, 50}, grid{100, 170, 700, 290};
    EmojiPicker picker;
    picker.set_geometry(tabs, grid);
    picker.set_content(sections(catalog), 0, true);
    auto view = picker.view();
    require(view.cells.empty() && view.hint == "Loading emoji…", "loading shows only a hint");
    require(view.tab_list.size() == emoji_group_count, "every tab shows while loading");

    picker.set_content(sections(catalog), 0, false);
    view = picker.view();
    require(view.hint.find("Recently used") == 0, "an empty Recent explains itself");
    require(view.tab_list.size() == 4, "empty categories have no tab");
    require(view.tab_list.front().icon == Icon::Recent && view.tab_list.front().active,
            "Recent comes first");
    const auto* first = cell(view, "s0");
    const auto* second = cell(view, "s1");
    const auto* sixth = cell(view, "s5");
    require(first && second && sixth, "smileys are in view");
    require(second->bounds.x == first->bounds.x && second->bounds.y > first->bounds.y,
            "cells fill down each column");
    require(sixth->bounds.x > first->bounds.x && sixth->bounds.y == first->bounds.y, "then across");

    // A tap types on release, over the same cell.
    auto [x, y] = center(first->bounds);
    require(picker.down(1, x, y, 0).redraw, "pressing a cell shows it");
    auto event = picker.up(1, x, y, 0);
    require(event.type == "s0" && event.typed == &catalog.entries[0], "a released cell types");
    picker.down(1, x, y, 0);
    picker.move(1, x, y + first->bounds.height, 0);
    require(picker.up(1, x, y + first->bounds.height, 0).type.empty(),
            "releasing elsewhere types nothing");

    // A drag scrolls and types nothing.
    picker.down(1, x, y, 0);
    picker.move(1, x - 5, y, 0);
    require(picker.scroll_offset() == 0, "a few pixels still count as a tap");
    picker.move(1, x - 120, y, 0);
    require(picker.scroll_offset() == 120, "dragging scrolls the grid");
    require(picker.up(1, x - 120, y, 0).type.empty(), "a drag types nothing");
    picker.down(1, x, y, 0);
    picker.move(1, x + 1000, y, 0);
    picker.up(1, x + 1000, y, 0);
    require(picker.scroll_offset() == 0, "scrolling stops at the start");

    // An emoji without tones starts scrolling sooner than one being held for its tones.
    picker.down(1, x, y, 0.5);
    picker.move(1, x - 14, y, 0.6);
    picker.up(1, x - 14, y, 0.7);
    require(picker.scroll_offset() == 14, "a small move on a plain emoji scrolls");
    picker.down(1, x + 14, y, 0.8);
    picker.move(1, x + 28, y, 0.9);
    picker.up(1, x + 28, y, 1.0);
    require(picker.scroll_offset() == 0, "and back");

    // A quick drag glides on after release, slowing until it stops; a press catches it.
    picker.down(1, x, y, 1);
    for (int i = 1; i <= 6; ++i) {
        picker.move(1, x - 24.0 * i, y, 1 + .02 * i); // 1200 pixels a second.
    }
    picker.up(1, x - 144, y, 1.12);
    require(picker.gliding(), "a quick drag glides");
    require(picker.scroll_offset() == 144, "the release itself does not jump");
    // The VR loop's frame time can precede the release's own time.
    picker.tick(1.11);
    picker.tick(1.12);
    require(picker.gliding() && picker.scroll_offset() == 144, "a tick no later than the release waits");
    picker.tick(1.12 + 1.0 / 60);
    const double after_frame = picker.scroll_offset();
    require(after_frame > 160 && after_frame < 170, "the glide starts at the release speed");
    for (int frame = 2; frame < 300 && picker.gliding(); ++frame) {
        picker.tick(1.12 + frame / 60.0);
    }
    require(!picker.gliding(), "the glide stops by itself");
    const double glided = picker.scroll_offset() - 144;
    require(glided > 380 && glided < 430, "it covers about a third of a second of the release speed");
    picker.reset();
    picker.down(1, x, y, 10);
    picker.move(1, x - 100, y, 10.1);
    picker.move(1, x - 100, y, 10.25);
    picker.up(1, x - 100, y, 10.3);
    require(!picker.gliding(), "pausing before release leaves nothing to glide");
    picker.reset();
    picker.down(1, x, y, 20);
    picker.move(1, x - 150, y, 20.05);
    picker.up(1, x - 150, y, 20.06);
    picker.tick(20.1);
    require(picker.gliding(), "glides again");
    event = picker.down(1, x, y, 20.12);
    require(!picker.gliding(), "a press catches the glide");
    require(picker.up(1, x, y, 20.15).type.empty(), "and its release types nothing");
    picker.down(1, x, y, 21);
    picker.move(1, x - 150, y, 21.05);
    picker.up(1, x - 150, y, 21.06);
    require(picker.scroll(x, y, 1) && !picker.gliding(), "the stick stops a glide");
    picker.down(1, x, y, 22);
    picker.move(1, x - 150, y, 22.05);
    picker.up(1, x - 150, y, 22.06);
    picker.cancel();
    require(!picker.gliding(), "cancelling stops a glide");
    picker.reset();
    // A glide past either end overshoots a little and settles back on it.
    for (const bool forward : {true, false}) {
        picker.reset();
        const double limit = picker.scroll_limit();
        // Two cells from the end it will run past.
        picker.scroll(x, y, forward ? limit / (2 * grid.height / 5) - 1 : 1);
        const double start = picker.scroll_offset();
        const double step = forward ? -40 : 40; // 2000 pixels a second toward the end.
        picker.down(1, x, y, 40);
        for (int i = 1; i <= 5; ++i) {
            picker.move(1, x + step * i, y, 40 + .02 * i);
        }
        picker.up(1, x + step * 5, y, 40.1);
        require(picker.gliding() || picker.scroll_offset() != start, "the fling glides");
        const double end = forward ? limit : 0;
        double furthest = 0;
        for (int frame = 1; frame < 600 && picker.gliding(); ++frame) {
            picker.tick(40.1 + frame / 60.0);
            furthest = std::max(furthest,
                                forward ? picker.scroll_offset() - end : end - picker.scroll_offset());
        }
        require(furthest > 20 && furthest < 70, "it runs a little past the end");
        require(!picker.gliding() && picker.scroll_offset() == end, "and settles exactly on it");
    }
    // A press during the bounce puts the grid back on its end.
    picker.reset();
    picker.scroll(x, y, 1);
    picker.down(1, x, y, 50);
    picker.move(1, x + 60, y, 50.02);
    picker.up(1, x + 100, y, 50.04);
    double now = 50.04;
    while (picker.gliding() && picker.scroll_offset() >= 0 && now < 51) {
        picker.tick(now += 1.0 / 60);
    }
    require(picker.gliding() && picker.scroll_offset() < 0, "bouncing past the start");
    picker.down(1, x, y, now);
    require(picker.scroll_offset() == 0 && !picker.gliding(), "a press puts it back at the start");
    picker.up(1, x, y, now);
    // A drag from empty grid space, here the empty Recent, scrolls too, and a tap there types nothing.
    picker.reset();
    picker.down(1, grid.x + 5, grid.y + 5, 60);
    require(picker.up(1, grid.x + 5, grid.y + 5, 60.1).type.empty(), "empty space types nothing");
    picker.down(1, grid.x + 5, grid.y + 5, 61);
    picker.move(1, grid.x - 95, grid.y + 5, 61.5);
    picker.up(1, grid.x - 95, grid.y + 5, 62);
    require(picker.scroll_offset() == 100, "a drag from empty space scrolls");
    // The tab row never glides.
    const auto tabs_center = center(picker.view().tab_list[1].bounds);
    picker.down(1, tabs_center.first, tabs_center.second, 30);
    picker.move(1, tabs_center.first - 150, tabs_center.second, 30.05);
    picker.up(1, tabs_center.first - 150, tabs_center.second, 30.06);
    require(!picker.gliding(), "the tab row does not glide");

    // Only one press at a time.
    picker.down(1, x, y, 0);
    require(!picker.down(2, x, y, 0).redraw, "another hand's press waits");
    picker.cancel_pointer(1);
    require(picker.up(1, x, y, 0).type.empty(), "a cancelled press types nothing");

    // Stick notches scroll; tabs jump to their category.
    require(picker.scroll(x, y, 1) && picker.scroll_offset() > 0, "the stick scrolls the grid");
    require(!picker.scroll(x, y, -1000) || picker.scroll_offset() == 0, "and stops at the start");
    view = picker.view();
    const auto flags = std::find_if(view.tab_list.begin(), view.tab_list.end(),
                                    [](const auto& tab) { return tab.icon == Icon::Flags; });
    std::tie(x, y) = center(flags->bounds);
    picker.down(1, x, y, 0);
    picker.up(1, x, y, 0);
    view = picker.view();
    require(picker.scroll_offset() == picker.scroll_limit() && cell(view, "f29"),
            "a tab jumps to its category");
    require(std::find_if(
                view.tab_list.begin(), view.tab_list.end(),
                [](const auto& tab) {
                    return tab.active;
                })->icon == Icon::Flags,
            "the chosen tab stays active at the end of the strip");

    // Holding a toned emoji opens its tones; the choice types and becomes the tone.
    picker.reset();
    picker.scroll(grid.x + 1, grid.y + 1, 7);
    view = picker.view();
    const auto* wave = cell(view, "wave");
    require(wave, "the toned emoji is in view");
    std::tie(x, y) = center(wave->bounds);
    picker.down(1, x, y, 10);
    picker.move(1, x + 14, y - 14, 10.2);
    require(!picker.tick(10.3).redraw, "a short hold does nothing");
    event = picker.tick(10.6);
    require(event.redraw && picker.popup_open(),
            "a long hold opens the tones, despite a slightly unsteady hand");
    view = picker.view();
    require(view.popup && view.popup->choices.size() == 6 && view.popup->choices[0].text == "wave" &&
                view.popup->choices[5].text == "wave5",
            "default first, then light to dark");
    const auto choice = center(view.popup->choices[3].bounds);
    require(picker.move(1, choice.first, choice.second, 0).redraw, "moving onto a tone highlights it");
    event = picker.up(1, choice.first, choice.second, 0);
    require(event.type == "wave3" && event.tone == 3, "releasing on a tone types it and keeps it");
    require(cell(picker.view(), "wave3"), "the grid shows the new tone");
    picker.down(1, x, y, 20);
    picker.tick(20.6);
    event = picker.up(1, grid.x + grid.width - 1, grid.y + grid.height - 1, 0);
    require(event.type.empty() && !event.tone, "releasing off the tones types nothing");
    view = picker.view();
    const auto* plain = cell(view, "s39");
    if (plain) {
        std::tie(x, y) = center(plain->bounds);
        picker.down(1, x, y, 30);
        require(!picker.tick(31).redraw, "an untoned emoji has no tones");
        require(picker.up(1, x, y, 0).type == "s39", "and types once on release");
    }

    // Recent: reserved width, column order, Clear in its bottom-right cell.
    picker.set_content(sections(catalog), 0, false);
    picker.reset();
    const auto empty_start = cell(picker.view(), "s0")->bounds;
    for (const std::size_t count : {std::size_t{1}, std::size_t{17}, std::size_t{29}, std::size_t{40}}) {
        picker.set_content(sections(catalog, count), 0, false);
        picker.reset();
        view = picker.view();
        const auto clear =
            std::find_if(view.cells.begin(), view.cells.end(), [](const auto& c) { return c.clear; });
        require(clear != view.cells.end(), "Recent has Clear");
        require(std::next(clear) != view.cells.end() && std::next(clear)->bounds == empty_start,
                "Recent keeps its width however full it is");
        const double cell_size = view.cells.front().bounds.width;
        const int column = static_cast<int>(std::lround((clear->bounds.x - grid.x) / cell_size));
        require(clear->bounds.y + clear->bounds.height == grid.y + grid.height,
                "Clear sits on the bottom row");
        require(column == EmojiPicker::recent_columns - 1, "Clear sits in Recent's last column");
        const auto shown =
            std::count_if(view.cells.begin(), clear, [](const auto& c) { return !c.clear; });
        require(static_cast<std::size_t>(shown) == std::min(count, picker.recent_capacity()),
                "Recent shows what fits beside Clear");
        std::tie(x, y) = center(clear->bounds);
        picker.down(1, x, y, 0);
        require(picker.up(1, x, y, 0).clear, "Clear empties Recent");
    }
    require(picker.recent_capacity() == 29, "Recent holds six columns less Clear");
}

struct TextSink : KeySink {
    std::vector<std::pair<int, int>> events;
    std::vector<std::string> text;
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        text.push_back(value);
        return true;
    }
};
std::pair<double, double> key_center(const App& app, const std::string& action) {
    const auto view = app.view();
    for (const auto& key : view.layout->keys) {
        if (key.action == action) {
            return center(panel_rect(view, key.bounds));
        }
    }
    throw std::runtime_error("key not found");
}
void wait_for_emoji(App& app) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        app.tick(monotonic_seconds());
        if (!app.emoji_open() || app.emoji_settled()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("emoji images never finished");
}

void app_tests() {
    TemporaryDirectory directory;
    TextSink sink;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    App app(options, sink);
    auto toggle = [&] {
        const auto [x, y] = key_center(app, "picker");
        require(app.down(0, x, y, 1), "the emoji key presses");
        app.up(0, x, y);
    };
    // Shift latched before opening must not reach the emoji or later keys.
    const auto [sx, sy] = key_center(app, "ShiftLeft");
    app.down(0, sx, sy, 1);
    app.up(0, sx, sy);
    require(!app.view().keyboard->modifiers().empty(), "Shift latches");
    toggle();
    require(app.emoji_open(), "the emoji key opens the picker");
    require(app.view().keyboard->modifiers().empty(), "opening releases latched modifiers");
    wait_for_emoji(app);
    if (!app.emoji_open()) {
        std::cout << "No color emoji font here; skipping picker typing checks.\n";
        return;
    }
    auto view = app.view();
    require(view.emoji && view.layout->keys.size() == 10, "the picker has its own keys");
    require(view.emoji->width == app.geometry().emoji->width, "geometry keeps the case width");
    const auto* grin = cell(*view.emoji, "😀");
    require(grin, "the first smiley is in view");
    auto [x, y] = center(grin->bounds);
    const auto [key_x, key_y] = key_center(app, "Enter");
    app.move(0, key_x, key_y);
    require(!app.scroll_wanted(), "no scroll events over the picker's keys");
    require(!app.move(0, x, y) && !app.down(0, x, y, 2), "emoji neither buzz on hover nor on press");
    require(app.scroll_wanted(), "scroll events over the grid");
    require(sink.text.empty(), "nothing types on press");
    app.up(0, x, y);
    require((sink.text == std::vector<std::string>{"😀"}), "the emoji types once on release, whole");
    require(app.emoji_open(), "the picker stays open");
    view = app.view();
    require(view.emoji->cells.front().text == "😀" && view.emoji->cells[1].clear,
            "an emoji from a category appears in Recent at once");
    // Another one goes first; choosing from Recent keeps its order.
    const auto* second = cell(*view.emoji, "😃");
    std::tie(x, y) = center(second->bounds);
    app.down(0, x, y, 2);
    app.up(0, x, y);
    view = app.view();
    require(view.emoji->cells[0].text == "😃" && view.emoji->cells[1].text == "😀",
            "the newest emoji from a category goes first");
    std::tie(x, y) = center(view.emoji->cells[1].bounds);
    app.down(0, x, y, 2);
    app.up(0, x, y);
    view = app.view();
    require(sink.text.back() == "😀" && view.emoji->cells[0].text == "😃" &&
                view.emoji->cells[1].text == "😀",
            "choosing from Recent does not reorder it");
    require(sink.events.empty(), "no key events reach the target");
    // Space types through the usual path.
    const auto [space_x, space_y] = key_center(app, "Space");
    app.down(0, space_x, space_y, 3);
    app.up(0, space_x, space_y);
    require(sink.text.back() == " ", "the picker's space bar types");
    toggle();
    require(!app.emoji_open(), "Back returns to the keyboard");
    require((load_emoji_preferences(directory.path / "emoji.json").recent ==
             std::vector<std::string>{"😃", "😀"}),
            "recent emoji are saved on closing");
    toggle();
    wait_for_emoji(app);
    view = app.view();
    require(view.emoji->cells[0].text == "😃" && view.emoji->cells[1].text == "😀" &&
                view.emoji->cells[2].clear,
            "Recent reopens as it was, then Clear");
    app.set_interaction_active(false);
    require(!app.emoji_open(), "hiding returns to the keyboard");
    app.set_interaction_active(true);

    // Close after choosing.
    Settings settings = load_settings(directory.path, *std::make_unique<std::vector<std::string>>());
    settings.emoji_close_on_select = true;
    save_settings(directory.path, settings);
    TextSink closing_sink;
    App closing(options, closing_sink);
    const auto [ex, ey] = key_center(closing, "picker");
    closing.down(0, ex, ey, 1);
    closing.up(0, ex, ey);
    wait_for_emoji(closing);
    view = closing.view();
    const auto first = std::find_if(view.emoji->cells.begin(), view.emoji->cells.end(),
                                    [](const auto& c) { return !c.clear; });
    std::tie(x, y) = center(first->bounds);
    closing.down(0, x, y, 2);
    closing.up(0, x, y);
    require(closing_sink.text.size() == 1 && !closing.emoji_open(), "choosing can close the picker");
}

void unavailable_tests() {
    TemporaryDirectory directory;
    NullSink sink;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Uinput;
    options.target_language = "en-us";
    options.start_enabled = true;
    App app(options, sink);
    const auto view = app.view();
    require(view.disabled_keys.contains("Emoji"), "physical-key input disables the emoji key");
    const auto [x, y] = key_center(app, "picker");
    require(!app.down(0, x, y, 1), "a disabled emoji key ignores the laser");
    app.up(0, x, y);
    require(!app.emoji_open(), "and never opens the picker");
}
} // namespace

int main() {
    try {
        table_tests();
        preference_tests();
        layout_tests();
        picker_tests();
        app_tests();
        unavailable_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Emoji tests passed\n";
    return 0;
}
