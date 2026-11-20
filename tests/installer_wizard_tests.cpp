// The installer's first-install wizard, driven through its own controls with
// a backend that only records what Finish would save. Nothing is installed,
// downloaded or started, and no settings file is written.
#include "../src/installer/installer.hpp"
#include "framekeyboard/config.hpp"
#include <cairo.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
    void releases() override {
        state = {};
        state.task = in::Task::Releases;
        state.state = in::State::Done;
        for (const auto& version : versions) {
            state.releases.push_back({"v" + version, version});
        }
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
        state.state = in::State::Done;
    }
    in::Status status() override { return state; }
};

// One installer from Home to its first wizard page, on a clock of its own.
struct Run {
    RecordingBackend backend;
    in::Installer installer;
    double now = 0;
    Run(const fk::Profiles& profiles, const fk::Localization& localization,
        std::vector<std::string> versions, int version = 0)
        : backend(), installer(backend, profiles.themes.at("graphite"),
                               profiles.layouts.at("en-us-full"), localization) {
        backend.versions = std::move(versions);
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
        require(!choices.shortcuts, "the shortcut bar's page starts at No");
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
        require(!run.finish(0).shortcuts, "and Finish saves no Yes for it");
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

// What Finish saves: Yes turns the bar on, No leaves it as it was.
void saved_settings() {
    in::Choices choices;
    fk::Settings settings;
    in::apply_choices(settings, choices);
    require(!settings.shortcuts && settings.curved_keyboard && settings.swipe_typing,
            "the default choices leave the bar off and turn the others on");
    settings.shortcuts = true;
    in::apply_choices(settings, choices);
    require(settings.shortcuts, "No leaves a bar turned on before as it was");
    fk::Settings fresh;
    choices.shortcuts = true;
    in::apply_choices(fresh, choices);
    require(fresh.shortcuts, "Yes turns the bar on");
}
} // namespace

int main() {
    try {
        const auto profiles = fk::load_profiles({}, {});
        fk::Localization localization;
        localization.load({}, {});
        localization.choose(fk::SystemLanguage{"", "en_US.UTF-8"});
        saved_settings();
        shortcuts_page(profiles, localization);
        std::cout << "Installer wizard pages and saved choices passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
