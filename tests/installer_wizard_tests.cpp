// The installer's first-install wizard, driven through its own controls with
// a backend that only records what Finish would save. Nothing is installed,
// downloaded or started, and no settings file is written.
#include "../src/installer/installer.hpp"
#include "framekeyboard/config.hpp"
#include <algorithm>
#include <cairo.h>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace framekeyboard::installer {
extern const unsigned char keyboard_png[];
extern const std::size_t keyboard_png_size;
} // namespace framekeyboard::installer
namespace fk = framekeyboard;
namespace in = framekeyboard::installer;
using A = in::Installer::Action;

namespace {
void require(bool ok, const std::string& message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

// Lists the given versions at once, installs at once and records Finish's choices.
struct RecordingBackend : in::Backend {
    std::vector<std::string> versions;
    in::Status state;
    std::optional<in::Choices> finished;
    bool fail_streaming{}; // Finish fails while Streaming support is chosen
    bool restart{};        // the running Steam needs a restart for Streaming support
    void releases() override {
        state = {};
        state.task = in::Task::Releases;
        state.state = in::State::Done;
        for (const auto& version : versions) {
            state.releases.push_back({"v" + version, version});
        }
        state.streaming_restart = restart;
    }
    void install(const in::Release& release) override {
        state.task = in::Task::Install;
        state.state = in::State::Done;
        state.installed = release.version;
    }
    void uninstall(bool) override {}
    void finish(const in::Choices& choices, bool) override {
        finished = choices;
        state.task = in::Task::Finish;
        state.state = fail_streaming && choices.streaming ? in::State::Failed : in::State::Done;
        state.error =
            state.state == in::State::Failed ? fk::ui("installer.error.streaming") : fk::UiText{};
    }
    in::Status status() override { return state; }
};

// One installer from Home to its first wizard page, on a clock of its own.
struct Run {
    RecordingBackend backend;
    in::Installer installer;
    double now = 0;
    Run(const fk::Profiles& profiles, const fk::Localization& localization,
        std::vector<std::string> versions, int version = 0, bool restart = false)
        : backend(), installer(backend, profiles.themes.at("graphite"),
                               profiles.layouts.at("en-us-full"), localization) {
        backend.versions = std::move(versions);
        backend.restart = restart;
        backend.releases();
        wait(.5);
        if (version != 0) {
            click(A::Versions);
            click(A::Version, version);
        }
        click(A::Install);
        wait(1.2); // the slide into Setup
    }
    void wait(double seconds) {
        for (const double end = now + seconds; now < end;) {
            now += 1.0 / 30;
            installer.tick(now);
        }
    }
    void click(A action, int index = 0) {
        installer.tick(now);
        const auto r = installer.control(action, index);
        require(r.has_value(), "a control the test presses is shown");
        installer.pointer_down(r->x + r->width / 2, r->y + r->height / 2);
        installer.pointer_up(r->x + r->width / 2, r->y + r->height / 2);
        installer.pointer_leave();
        wait(.1);
    }
    int page_of(int feature) const {
        for (int page = 0; page < installer.wizard_pages(); ++page) {
            if (installer.feature(page) == feature) {
                return page;
            }
        }
        return -1;
    }
    // Next until the given page, or Finish.
    void to_page(int page, int from) {
        for (int i = from; i < page; ++i) {
            click(A::Next);
            wait(.6);
        }
    }
    in::Choices finish(int from) {
        to_page(installer.wizard_pages(), from);
        wait(.5);
        click(A::Finish);
        require(backend.finished.has_value(), "Finish saves the choices");
        return *backend.finished;
    }
    // How many of the picture's pixels are bright, as its labels and legends
    // are: fewer once it dims, which lets the stage's own color through.
    long picture_light() const {
        auto* canvas = installer.canvas();
        cairo_surface_flush(canvas);
        const auto* data = cairo_image_surface_get_data(canvas);
        const int stride = cairo_image_surface_get_stride(canvas);
        long sum = 0;
        for (int y = 240; y < 600; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(data + y * stride);
            for (int x = 200; x < 620; ++x) {
                const auto p = row[x];
                sum += ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255) > 480;
            }
        }
        return sum;
    }
};

void shortcuts_page(const fk::Profiles& profiles, const fk::Localization& localization) {
    using I = in::Installer;
    // 0.20.0: its page sits after Curved keyboard and before Streaming support.
    {
        Run run(profiles, localization, {"0.20.0", "0.19.0"});
        require(run.installer.wizard_pages() == 8, "a release with the bar has eight pages");
        const std::vector<int> order{0,
                                     1,
                                     2,
                                     3,
                                     I::numpad_feature,
                                     I::curved_feature,
                                     I::shortcuts_feature,
                                     I::streaming_feature};
        for (int page = 0; page < 8; ++page) {
            require(run.installer.feature(page) == order[static_cast<std::size_t>(page)],
                    "the pages come in order, the shortcut bar before Streaming support");
        }
        // Untouched: No for the bar, Yes for every other page.
        const auto choices = run.finish(0);
        require(!choices.shortcuts && choices.shortcuts_asked,
                "the shortcut bar's page starts at No, and its No is saved");
        require(choices.replace_native && choices.swipe && choices.suggestions &&
                    choices.pull_for_shift && choices.numpad && choices.curved && choices.streaming,
                "every other page keeps Yes");
    }
    // Yes saves it; the picture plays lit at the default and after Yes, and dims only
    // once a Yes is turned back to No.
    {
        Run run(profiles, localization, {"0.20.0"});
        const int page = run.page_of(I::shortcuts_feature);
        run.to_page(page, 0);
        run.wait(1.5);
        const long untouched = run.picture_light();
        run.click(A::Choice, 0);
        run.wait(.6);
        const long yes = run.picture_light();
        run.click(A::Choice, 1);
        run.wait(.6);
        const long declined = run.picture_light();
        require(untouched > yes * 8 / 10 && untouched < yes * 12 / 10,
                "at its default No the picture plays as lit as after Yes");
        require(declined < yes * 7 / 10, "Yes turned back to No dims it");
        run.click(A::Choice, 0);
        require(run.finish(page).shortcuts, "Yes reaches Finish's choices");
    }
    {
        Run run(profiles, localization, {"0.20.0"});
        const int page = run.page_of(I::shortcuts_feature);
        run.to_page(page, 0);
        run.click(A::Choice, 0);
        run.click(A::Choice, 1);
        require(!run.finish(page).shortcuts, "Yes then No saves No");
    }
    // An older version chosen has no page for it, and Finish leaves it off.
    {
        Run run(profiles, localization, {"0.20.0", "0.19.0"}, 1);
        require(run.installer.wizard_pages() == 7 && run.page_of(I::shortcuts_feature) < 0 &&
                    run.installer.feature(6) == I::streaming_feature,
                "0.19.0 skips the shortcut bar's page");
        const auto choices = run.finish(0);
        require(!choices.shortcuts && !choices.shortcuts_asked,
                "and Finish saves nothing for it, leaving the setting as loaded");
    }
    // 0.18.x has Streaming support only; 0.17.x and 0.16.x no optional page.
    {
        Run run(profiles, localization, {"0.20.0", "0.18.0"}, 1);
        require(run.installer.wizard_pages() == 6 && run.installer.feature(5) == I::streaming_feature &&
                    run.page_of(I::curved_feature) < 0 && run.page_of(I::shortcuts_feature) < 0,
                "0.18.0 offers Streaming support's page alone");
        const auto choices = run.finish(0);
        require(!choices.curved && !choices.shortcuts_asked && choices.streaming,
                "and saves only the answers its pages asked");
    }
    for (const char* older : {"0.17.0", "0.16.0"}) {
        Run run(profiles, localization, {"0.20.0", older}, 1);
        require(run.installer.wizard_pages() == 5 && run.page_of(I::curved_feature) < 0 &&
                    run.page_of(I::shortcuts_feature) < 0 && run.page_of(I::streaming_feature) < 0,
                std::string(older) + " offers no optional page");
        const auto choices = run.finish(0);
        require(!choices.curved && !choices.shortcuts_asked && !choices.streaming,
                std::string(older) + " saves none of their answers");
    }
    // Streaming support still needs Replace native keyboard.
    {
        Run run(profiles, localization, {"0.20.0"});
        run.click(A::Choice, 1); // Replace native keyboard: No
        const int page = run.page_of(I::streaming_feature);
        run.to_page(page, 0);
        require(!run.installer.enabled(A::Choice, 0),
                "Streaming support can't be answered Yes without replacement");
        const auto choices = run.finish(page);
        require(!choices.replace_native && !choices.streaming && !choices.shortcuts,
                "and Finish saves it off");
    }
}

// R425: Finish without it, after Streaming support failed, saves the answers
// as Finish lists them: a 0.18.x install, which has no Curved keyboard page,
// leaves Curved keyboard off.
void finish_without(const fk::Profiles& profiles, const fk::Localization& localization) {
    Run run(profiles, localization, {"0.20.0", "0.18.0"}, 1);
    run.backend.fail_streaming = true;
    run.finish(0);
    run.wait(.5);
    run.click(A::FinishWithout);
    const auto& choices = *run.backend.finished;
    require(!choices.streaming && !choices.curved && !choices.shortcuts && !choices.shortcuts_asked &&
                choices.replace_native && choices.swipe,
            "Finish without it saves only the answers its pages showed");
}
// R424: Finish's eight answers stay inside the stage in every language, with
// Restart SteamOS's box unticked and ticked, whose note is the longest.
void finish_list_fits(const fk::Profiles& profiles) {
    for (const char* posix : {"cs", "de", "en", "es", "es_419", "fr", "it", "ja", "ko", "pl", "pt_BR",
                              "pt_PT", "ru", "tr", "uk", "zh_CN", "zh_TW"}) {
        fk::Localization localization;
        localization.load({}, {});
        localization.choose(fk::SystemLanguage{"", std::string(posix) + ".UTF-8"});
        Run run(profiles, localization, {"0.20.0"}, 0, true);
        run.to_page(run.installer.wizard_pages(), 0);
        run.wait(1);
        require(run.installer.finish_list_room() >= 0,
                std::string(posix) + ": Finish's answers end inside the stage");
        run.click(A::Restart);
        run.wait(.2);
        require(run.installer.finish_list_room() >= 0,
                std::string(posix) + ": and with Restart SteamOS ticked");
    }
}
// The picture's geometry, derived from the layout it shows, matches the
// embedded picture: its case ends where the derived case does, and a key the
// demos aim at is drawn where the derived rows and pitch put it.
void picture_geometry(const fk::Profiles& profiles) {
    struct Reader {
        const unsigned char* data;
        std::size_t size, at;
    } reader{in::keyboard_png, in::keyboard_png_size, 0};
    auto* picture = cairo_image_surface_create_from_png_stream(
        [](void* closure, unsigned char* out, unsigned int length) {
            auto& r = *static_cast<Reader*>(closure);
            if (r.size - r.at < length) {
                return CAIRO_STATUS_READ_ERROR;
            }
            std::copy_n(r.data + r.at, length, out);
            r.at += length;
            return CAIRO_STATUS_SUCCESS;
        },
        &reader);
    require(cairo_surface_status(picture) == CAIRO_STATUS_SUCCESS, "the embedded picture decodes");
    const auto* data = cairo_image_surface_get_data(picture);
    const int stride = cairo_image_surface_get_stride(picture);
    const int height = cairo_image_surface_get_height(picture);
    const auto alpha = [&](int x, int y) {
        return reinterpret_cast<const std::uint32_t*>(data + y * stride)[x] >> 24;
    };
    // The case's last opaque row, at its left, where no key or line lies.
    int last = 0;
    while (last + 1 < height && alpha(100, last + 1) > 128) {
        ++last;
    }
    const auto g =
        in::picture_geometry(profiles.layouts.at("en-us-full"), profiles.themes.at("graphite"));
    require(std::abs(g.case_bottom - (last + 1)) <= 1, "the case ends where the derived geometry says");
    // Each derived row's middle, in the column of Backquote and Tab, is a key
    // face, lighter than the case's gap between the first two rows.
    const auto light = [&](double x, double y) {
        const auto p = reinterpret_cast<const std::uint32_t*>(data + static_cast<int>(y) *
                                                                         stride)[static_cast<int>(x)];
        return ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255);
    };
    const double x = g.keys_left + .2 * g.key_pitch;
    const double gap = light(x, (g.row_middle[0] + g.row_middle[1]) / 2 + 3);
    for (const double y : g.row_middle) {
        require(light(x, y) > gap + 30, "a key lies where the derived rows put it");
    }
    cairo_surface_destroy(picture);
}
// What Finish saves: the bar's answer either way when its page was offered,
// as every other page's; without the page the setting stays as loaded (R423).
void saved_settings() {
    in::Choices choices;
    choices.shortcuts_asked = true;
    fk::Settings settings;
    in::apply_choices(settings, choices);
    require(!settings.shortcuts && settings.curved_keyboard && settings.swipe_typing,
            "the default choices leave the bar off and turn the others on");
    settings.shortcuts = true;
    in::apply_choices(settings, choices);
    require(!settings.shortcuts, "No turns off a bar a kept config had on");
    choices.shortcuts = true;
    in::apply_choices(settings, choices);
    require(settings.shortcuts, "Yes turns the bar on");
    // A version without the page asked nothing: neither on nor off is written.
    in::Choices unasked;
    for (const bool kept : {true, false}) {
        fk::Settings loaded;
        loaded.shortcuts = kept;
        in::apply_choices(loaded, unasked);
        require(loaded.shortcuts == kept, "without its page the bar stays as loaded");
    }
}
} // namespace

int main() {
    try {
        const auto profiles = fk::load_profiles({}, {});
        fk::Localization localization;
        localization.load({}, {});
        localization.choose(fk::SystemLanguage{"", "en_US.UTF-8"});
        saved_settings();
        picture_geometry(profiles);
        shortcuts_page(profiles, localization);
        finish_without(profiles, localization);
        finish_list_fits(profiles);
        std::cout << "Installer wizard pages and saved choices passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
