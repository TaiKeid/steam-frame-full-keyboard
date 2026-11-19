// R380: Background Disabled lets the laser through between key groups. Every
// hit test uses pointer_bounds(); these check, in the App's own views, what
// it gives for every bundled layout and the views over it.
#include "bundled_profiles.hpp"
#include "framekeyboard/app.hpp"
#include "framekeyboard/shortcuts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-laser-areas-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct Sink : KeySink {
    void send(int, int) override {}
    bool text_available() override { return true; }
    bool commit_text(const std::string&) override { return true; }
};
// A stand-in dictation helper: it answers a pause and nothing else, so the
// mode opens without audio or the network.
const char* helper = R"PY(
import json, socket
peer = socket.socket(fileno=3)
while True:
    data = peer.recv(65536)
    if not data:
        break
    message = json.loads(data)
    if message.get("op") == "capture" and not message.get("on"):
        peer.send(json.dumps({"op": "paused"}).encode())
)PY";
struct Case {
    std::string layout, language;
    bool hide{}, bar{};
    std::string surface{"disabled"};
};
std::string name(const Case& c) {
    return c.layout + " with " + c.language + (c.hide ? ", numpad hidden" : "") + (c.bar ? ", bar on" : "") +
           ", " + c.surface;
}
struct Rig {
    TemporaryDirectory directory;
    Sink sink;
    std::unique_ptr<App> app;
    double now = 100;
    explicit Rig(const Case& c) {
        std::ofstream(directory.path / "config.json")
            << R"({"schema_version":1,"active":{"layout":")" << c.layout << R"(","language":")" << c.language
            << R"(","theme":"graphite"},"hide_numpad":)" << (c.hide ? "true" : "false")
            << R"(,"shortcuts":)" << (c.bar ? "true" : "false") << R"(,"background_surface":")" << c.surface
            << R"(","dictation":"server","dictation_server":"127.0.0.1"})";
        if (c.bar) {
            ShortcutModel model;
            ShortcutButton button;
            button.label = "A";
            button.action.kind = ShortcutAction::Kind::Keys;
            button.action.chords = {{{}, "KeyA"}};
            model.profiles.push_back({"p", "Profile", {button, button, button}});
            model.free_profile = "p";
            std::ofstream(directory.path / "shortcuts.json") << serialize_shortcuts(model);
        }
        std::ofstream(directory.path / "helper.py") << helper;
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.dictation_helper = directory.path / "helper.py";
        app = std::make_unique<App>(options, sink);
        tick();
    }
    void tick(int times = 1) {
        for (int i = 0; i < times; ++i) {
            app->tick(now += .01);
        }
    }
    PanelView view() const { return app->input_view(); }
    const Key* key(const std::string& id) const {
        const auto v = view();
        for (const auto& k : v.layout->keys) {
            if (k.id == id && !hidden_key(v, k)) {
                return &k;
            }
        }
        return nullptr;
    }
    Rect rect(const std::string& id) const { return panel_rect(view(), key(id)->bounds); }
    void press(const std::string& id) {
        const auto r = rect(id);
        app->down(0, r.x + r.width / 2, r.y + r.height / 2, now);
        app->up(0, r.x + r.width / 2, r.y + r.height / 2);
        tick();
    }
};
bool hits(const PanelView& view, double x, double y) {
    return takes_pointer(view, x, y);
}
// The midpoint of the gap between two keys, along the row.
std::pair<double, double> between(const Rect& left, const Rect& right) {
    const double top = std::max(left.y, right.y);
    const double bottom = std::min(left.y + left.height, right.y + right.height);
    return {(left.x + left.width + right.x) / 2, (top + bottom) / 2};
}
// The checks every view under Disabled passes: within the mask with room
// for the move handle, no fallback, and every key and control it shows hits.
std::size_t areas_fit(const PanelView& view, const std::string& what) {
    const auto areas = laser_areas(view, 1);
    require(areas.fallback == 0 && areas.areas.size() + 1 <= max_laser_areas,
            what + ": the areas fit SteamVR's mask (" + std::to_string(areas.areas.size()) + ")");
    for (const auto& key : view.layout->keys) {
        if (hidden_key(view, key)) {
            continue;
        }
        const auto r = panel_rect(view, key.bounds);
        require(hits(view, r.x + r.width / 2, r.y + r.height / 2), what + ": " + key.id + " takes the laser");
    }
    for (const auto& control : view.controls) {
        const auto& r = control.bounds;
        const double x = r.x + r.width / 2, y = r.y + r.height / 2;
        if (!control.clip || control.clip->contains(x, y)) {
            require(hits(view, x, y), what + ": the control " + control.id + " takes the laser");
        }
    }
    return areas.areas.size();
}
void keyboard_views() {
    const auto profiles = load_profiles({}, {});
    std::vector<Case> cases;
    for (const auto& [id, layout] : profiles.layouts) {
        const auto language = profiles.languages.contains(layout.language) ? layout.language : "en-us";
        for (const bool hide : {false, true}) {
            for (const bool bar : {false, true}) {
                cases.push_back({id, language, hide, bar});
            }
        }
    }
    cases.push_back({"en-us-full", "ja-romaji"}); // the band
    cases.push_back({"en-us-full", "ko-kr"});
    cases.push_back({"en-us-full", "zh-cn-pinyin"});
    for (const auto& c : cases) {
        Rig rig(c);
        const auto view = rig.view();
        const auto count = areas_fit(view, name(c));
        std::cout << "laser areas: " << name(c) << ": " << count << '\n';
        if (rig.key("Escape") && rig.key("F1")) {
            // Blocks stay apart where the gaps are wide; keys of a block don't.
            const auto [ex, ey] = between(rig.rect("Escape"), rig.rect("F1"));
            require(!hits(view, ex, ey), name(c) + ": the laser passes between Esc and F1");
            const auto [qx, qy] = between(rig.rect("KeyQ"), rig.rect("KeyW"));
            require(hits(view, qx, qy), name(c) + ": between Q and W is the keys'");
            const auto [bx, by] = between(rig.rect("Backspace"), rig.rect("Insert"));
            require(!hits(view, bx, by), name(c) + ": the laser passes between the blocks");
            const auto left = rig.rect("ArrowLeft"), up = rig.rect("ArrowUp");
            require(!hits(view, left.x + left.width / 2, up.y + up.height / 2),
                    name(c) + ": beside ↑, over ←, the laser passes");
        }
        if (rig.key("Flick-undo") && rig.key("Cut")) {
            // The flick layout: its side column apart from the grid.
            const auto cut = rig.rect("Cut"), undo = rig.rect("Flick-undo");
            require(!hits(view, (cut.x + cut.width + undo.x) / 2, cut.y + cut.height / 2),
                    name(c) + ": the laser passes between the side column and the grid");
        }
        // Settings, and a press that began on the keys, take the whole case.
        const auto body = panel_case_bounds(view);
        const double gap_x = body.x + 2, gap_y = body.y + body.height - 2;
        require(!hits(view, gap_x, gap_y), name(c) + ": the case's corner passes the laser");
        const auto held = rig.key("KeyQ") ? rig.rect("KeyQ") : rig.rect(view.layout->keys.back().id);
        rig.app->down(0, held.x + held.width / 2, held.y + held.height / 2, rig.now);
        require(rig.view().pointer_held && hits(rig.view(), gap_x, gap_y),
                name(c) + ": a press on the keys gives the whole case back");
        rig.app->up(0, held.x + held.width / 2, held.y + held.height / 2);
        rig.tick();
        require(!rig.view().pointer_held && !hits(rig.view(), gap_x, gap_y),
                name(c) + ": and its end takes it again");
        rig.app->show_settings();
        rig.tick();
        require(hits(rig.view(), gap_x, gap_y) && laser_areas(rig.view()).areas[0] == panel_case_bounds(rig.view()),
                name(c) + ": Settings takes the whole case");
    }
}
// R385: dictation mode's status takes the laser only while it is too long to
// show whole, the case that scrolls under the laser; one that fits leaves
// the wide space between the toolbar's button groups open.
void status_line(PanelView view, const std::string& what) {
    const auto line = dictation_status_bounds(view);
    const double x = line.x + line.width / 2, y = line.y + line.height / 2;
    view.status_card = false;
    view.status = "Listening";
    require(!dictation_status_scrolls(view) && !hits(view, x, y),
            what + ": a status that fits lets the laser pass");
    view.status.clear();
    for (int i = 0; i < 12; ++i) {
        view.status += "The dictation server could not be reached. ";
    }
    require(dictation_status_scrolls(view) && hits(view, x, y) && laser_areas(view, 1).fallback == 0,
            what + ": a status too long for its line takes the laser, so it can scroll");
}
// The emoji picker and dictation mode over a full and the flick layout.
void other_views() {
    for (const Case& c : {Case{"en-us-full", "en-us", false, true}, Case{"en-us-full", "en-us", true, false},
                          Case{"ja-flick-12", "ja-flick", false, true}}) {
        {
            Rig rig(c);
            rig.press("Emoji");
            for (int i = 0; i < 400 && !rig.app->emoji_open(); ++i) {
                usleep(10000);
                rig.tick();
            }
            if (rig.app->emoji_open()) {
                const auto view = rig.view();
                std::cout << "laser areas: " << name(c) << ", emoji picker: " << areas_fit(view, name(c) + ", emoji")
                          << '\n';
                const auto grid = view.emoji->grid;
                require(hits(view, grid.x + grid.width / 2, grid.y + grid.height / 2) &&
                            hits(view, view.emoji->tabs.x + 4, view.emoji->tabs.y + view.emoji->tabs.height / 2),
                        name(c) + ": the picker's grid and tabs take the laser");
            } else {
                std::cout << "No color emoji font here; skipping the picker's areas.\n";
            }
        }
        {
            Rig rig(c);
            rig.press("Dictation");
            rig.press("Dictation");
            for (int i = 0; i < 300 && !rig.app->dictation_open(); ++i) {
                usleep(5000);
                rig.tick();
            }
            require(rig.app->dictation_open(), name(c) + ": dictation mode opens");
            const auto view = rig.view();
            std::cout << "laser areas: " << name(c) << ", dictation mode: "
                      << areas_fit(view, name(c) + ", dictation") << '\n';
            const auto& editor = view.dictation->bounds;
            require(hits(view, editor.x + editor.width / 2, editor.y + editor.height / 2),
                    name(c) + ": the editor takes the laser");
            status_line(view, name(c));
        }
    }
}
// The flick layout's English and digit pages, reached with its mode key.
void flick_pages() {
    for (const bool bar : {false, true}) {
        Case c{"ja-flick-12", "ja-flick", false, bar};
        Rig rig(c);
        for (const char* page : {"English page", "digits page"}) {
            rig.press("Flick-mode");
            const auto view = rig.view();
            std::cout << "laser areas: " << name(c) << ", " << page << " (" << view.layout->id
                      << "): " << areas_fit(view, name(c) + ", " + page) << '\n';
            const auto cut = rig.rect("Cut");
            const auto first = std::min_element(view.layout->keys.begin(), view.layout->keys.end(),
                                                [&](const Key& a, const Key& b) {
                                                    const bool side_a = a.bounds.x < 1, side_b = b.bounds.x < 1;
                                                    return side_a != side_b ? side_b : a.bounds.x < b.bounds.x;
                                                });
            const auto block = panel_rect(view, first->bounds);
            require(!hits(view, (cut.x + cut.width + block.x) / 2, cut.y + cut.height / 2),
                    name(c) + ", " + page + ": the laser passes between the side column and the keys");
        }
    }
}
// The shortcut editor takes the whole case, drawn and under the laser.
void shortcut_editor() {
    Rig rig({"en-us-full", "en-us", false, true});
    const auto tap = [&](const Rect& r) {
        rig.app->move(0, r.x + r.width / 2, r.y + r.height / 2);
        rig.app->down(0, r.x + r.width / 2, r.y + r.height / 2, rig.now);
        rig.app->up(0, r.x + r.width / 2, r.y + r.height / 2);
        rig.tick();
    };
    tap(rig.view().shortcut_bar->picker);
    tap(rig.view().shortcut_bar->menu->manage);
    const auto view = rig.view();
    require(view.shortcut_editor.has_value(), "the editor opens");
    const auto body = panel_case_bounds(view);
    require(hits(view, body.x + 2, body.y + body.height - 2) && laser_areas(view).areas[0] == body,
            "the shortcut editor takes the whole case");
}
// R386: SteamVR's mask follows a press's hold without a repaint. A press on
// a toolbar button slid off it and let go in a gap is canceled and draws
// nothing, yet the case must give the gaps back that frame.
void mask_follows_presses() {
    for (const char* surface : {"disabled", "visible"}) {
        const bool disabled = std::string(surface) == "disabled";
        Rig rig({"en-us-full", "en-us", false, false, surface});
        rig.app->take_mask_change();
        const auto view = rig.view();
        const auto button = std::find_if(view.controls.begin(), view.controls.end(), [](const Control& c) {
            return c.style != ControlStyle::MenuItem && c.bounds.width > 0;
        });
        require(button != view.controls.end(), "a toolbar button");
        const auto& r = button->bounds;
        const auto body = panel_case_bounds(view);
        const double gap_x = body.x + 2, gap_y = body.y + body.height - 2;
        rig.app->move(0, r.x + r.width / 2, r.y + r.height / 2);
        rig.app->down(0, r.x + r.width / 2, r.y + r.height / 2, rig.now);
        require(rig.app->take_mask_change() == disabled,
                std::string(surface) + ": the press masks at once only under Disabled");
        rig.app->move(0, gap_x, gap_y);
        rig.tick();
        rig.app->paint(rig.now);
        require(!rig.app->take_mask_change(), std::string(surface) + ": nothing changes while it is held");
        rig.app->up(0, gap_x, gap_y);
        rig.tick();
        require(!rig.app->paint(rig.now), std::string(surface) + ": the canceled release draws nothing");
        require(rig.app->take_mask_change() == disabled,
                std::string(surface) + ": yet under Disabled the mask gives the gaps back");
        require(!rig.app->take_mask_change(), std::string(surface) + ": once");
    }
}
// Visible, Semi-transparent and Invisible keep the case under the laser as
// before: the case first, then the menus and the bar, nothing else.
void other_backgrounds() {
    for (const char* surface : {"visible", "semi-transparent", "invisible"}) {
        Case c{"en-us-full", "en-us", false, true, surface};
        Rig rig(c);
        const auto view = rig.view();
        const auto areas = laser_areas(view, 1);
        std::vector<Rect> before{panel_case_bounds(view)};
        for (const auto& area : shortcut_bar_bounds(view)) {
            before.push_back(area);
        }
        require(areas.fallback == 0 && areas.areas == before, name(c) + ": the case takes the laser as before");
        require(!view.click_through, name(c) + ": not click-through");
        // A long dictation status adds nothing here: the case already has it.
        auto dictating = view;
        dictating.dictation = DictationView{};
        dictating.status = std::string(400, 'x');
        require(laser_areas(dictating, 1).areas == before, name(c) + ": a long status adds no area");
    }
}
} // namespace

int main() {
    try {
        keyboard_views();
        other_views();
        flick_pages();
        shortcut_editor();
        other_backgrounds();
        mask_follows_presses();
        std::cout << "Laser areas passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
