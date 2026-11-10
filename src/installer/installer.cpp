#include "installer.hpp"
#include "framekeyboard/paint.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <pango/pangocairo.h>

namespace framekeyboard::installer {
// docs/images/keyboard-graphite.png, embedded at build time.
extern const unsigned char keyboard_png[];
extern const std::size_t keyboard_png_size;

namespace {

constexpr double pi = 3.141592653589793;
// The keyboard picture on Home, 1600 x 672 scaled to 65%; Setup slides it up.
constexpr Rect picture{120, 56, 1040, 436.8};
constexpr double slide_distance = 300, slide_seconds = .7;
// Setup's card holds the wizard pages and Finish.
constexpr Rect card{200, 250, 880, 500};
constexpr double card_margin = 48;

double ease_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

struct PngReader {
    const unsigned char* data;
    std::size_t size, at{};
};
cairo_status_t read_png(void* closure, unsigned char* out, unsigned int length) {
    auto& reader = *static_cast<PngReader*>(closure);
    if (reader.size - reader.at < length) {
        return CAIRO_STATUS_READ_ERROR;
    }
    std::memcpy(out, reader.data + reader.at, length);
    reader.at += length;
    return CAIRO_STATUS_SUCCESS;
}

// Each wizard page's title and text keys.
constexpr std::array<std::pair<const char*, const char*>, Installer::pages> page_keys{{
    {"installer.page.replace", "installer.page.replace.text"},
    {"installer.page.swipe", "installer.page.swipe.text"},
    {"installer.page.suggestions", "installer.page.suggestions.text"},
    {"installer.page.pull", "installer.page.pull.text"},
    {"installer.page.numpad", "installer.page.numpad.text"},
}};

} // namespace

Installer::Installer(Backend& backend, Theme theme, const Localization& localization)
    : backend_(backend), theme_(std::move(theme)), localization_(localization),
      font_(localization.fonts()),
      canvas_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy),
      keyboard_(nullptr, cairo_surface_destroy) {
    PngReader reader{keyboard_png, keyboard_png_size};
    keyboard_.reset(cairo_image_surface_create_from_png_stream(read_png, &reader));
    backend_.releases();
}

std::string Installer::tr(std::string_view key, const UiArgs& args) const {
    return localization_.text(key, args);
}
Color Installer::dim() const {
    return paint::mix(theme_.legend, theme_.surface, .4);
}
bool Installer::busy() const {
    return status_.state == State::Running && status_.task != Task::Releases;
}

// ---- input

const Installer::Hit* Installer::hit_at(double x, double y) const {
    // Later controls lie above earlier ones (the version list, the dialog).
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const auto& r = it->bounds;
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) {
            return &*it;
        }
    }
    return nullptr;
}
bool Installer::hovered(Action action, int index) const {
    const auto* hit = hit_at(pointer_x_, pointer_y_);
    return hit && hit->enabled && hit->action == action && hit->index == index &&
           (!pressed_ || (pressed_->action == action && pressed_->index == index));
}
void Installer::pointer_move(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
}
void Installer::pointer_leave() {
    pointer_x_ = pointer_y_ = -1;
    pressed_.reset();
}
void Installer::pointer_down(double x, double y) {
    pointer_move(x, y);
    const auto* hit = hit_at(x, y);
    if (hit && hit->enabled) {
        pressed_ = *hit;
    } else {
        pressed_.reset();
        versions_open_ = false; // A press outside the list closes it.
    }
}
void Installer::pointer_up(double x, double y) {
    pointer_move(x, y);
    const auto* hit = hit_at(x, y);
    // Activated on release over the control it was pressed on, as Settings does.
    if (pressed_ && hit && hit->action == pressed_->action && hit->index == pressed_->index) {
        const Hit chosen = *pressed_;
        pressed_.reset();
        activate(chosen);
    }
    pressed_.reset();
}

void Installer::activate(const Hit& hit) {
    switch (hit.action) {
    case Action::Close:
        quit_ = true;
        break;
    case Action::Install:
        versions_open_ = false;
        begin_install();
        break;
    case Action::Versions:
        versions_open_ = !versions_open_;
        break;
    case Action::Version:
        selected_ = static_cast<std::size_t>(hit.index);
        versions_open_ = false;
        break;
    case Action::Uninstall:
        confirm_open_ = true;
        keep_settings_ = true;
        break;
    case Action::KeepSettings:
        keep_settings_ = hit.index == 0;
        break;
    case Action::CancelUninstall:
        confirm_open_ = false;
        break;
    case Action::ConfirmUninstall:
        backend_.uninstall(keep_settings_);
        break;
    case Action::Choice: {
        const bool yes = hit.index == 0;
        switch (page_) {
        case 0:
            choices_.replace_native = yes;
            break;
        case 1:
            choices_.swipe = yes;
            break;
        case 2:
            choices_.suggestions = yes;
            break;
        case 3:
            choices_.pull_for_shift = yes;
            break;
        case 4:
            choices_.hide_numpad = yes;
            break;
        }
        break;
    }
    case Action::Back:
        page_ = std::max(0, page_ - 1);
        break;
    case Action::Next:
        page_ = std::min(pages, page_ + 1);
        break;
    case Action::Finish:
        finish_pressed_ = true;
        if (first_install_) {
            backend_.finish(choices_);
        } else {
            quit_ = true;
        }
        break;
    case Action::Retry:
        if (screen_ == Screen::Setup) {
            backend_.install(releases_[selected_]);
        } else {
            backend_.releases();
        }
        break;
    case Action::None:
        break;
    }
}

void Installer::begin_install() {
    if (releases_.empty()) {
        return;
    }
    screen_ = Screen::Setup;
    first_install_ = installed_.empty();
    installing_ = releases_[selected_].version;
    // Reinstalls and other versions don't show the wizard (owner's design).
    page_ = first_install_ ? 0 : pages;
    setup_started_ = -1; // set on the next tick
    backend_.install(releases_[selected_]);
}

// ---- backend

void Installer::poll() {
    status_ = backend_.status();
    const bool changed = status_.task != seen_task_ || status_.state != seen_state_;
    seen_task_ = status_.task;
    seen_state_ = status_.state;
    if (!changed) {
        return;
    }
    if (status_.task == Task::Releases) {
        installed_ = status_.installed;
        if (status_.state == State::Done) {
            releases_ = status_.releases;
            selected_ = 0;
        }
    } else if (status_.task == Task::Install && status_.state == State::Done) {
        install_done_ = true;
        installed_ = status_.installed;
    } else if (status_.task == Task::Uninstall && status_.state != State::Running) {
        confirm_open_ = false;
        if (status_.state == State::Done) {
            installed_.clear();
            home_message_ = ui("installer.uninstalled");
        } else {
            home_message_ = status_.error;
        }
    } else if (status_.task == Task::Finish) {
        if (status_.state == State::Done) {
            quit_ = true;
        } else if (status_.state == State::Failed) {
            finish_pressed_ = false; // Finish again tries again; Back stays gone.
        }
    }
}

bool Installer::tick(double now) {
    poll();
    if (screen_ == Screen::Setup && setup_started_ < 0) {
        setup_started_ = now;
    }
    paint(now);
    return true;
}

// ---- painting helpers

void Installer::text(const std::string& value, const Rect& r, double size, Color color,
                     PangoAlignment alignment, bool wrap) {
    paint::label(cr_, value, r, size, font_, color, alignment, wrap);
}

void Installer::paragraph(const std::string& value, const Rect& r, double size, Color color) {
    auto* layout = pango_cairo_create_layout(cr_);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, font_.c_str());
    pango_layout_set_auto_dir(layout, FALSE);
    pango_layout_set_width(layout, static_cast<int>((r.width - 8) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, value.c_str(), static_cast<int>(value.size()));
    // Longer translations shrink a little rather than run into the controls.
    for (int width, height;; size *= .95) {
        pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_layout_set_spacing(layout, static_cast<int>(size * .2 * PANGO_SCALE));
        pango_layout_get_pixel_size(layout, &width, &height);
        if (height <= r.height || size < 14) {
            break;
        }
    }
    paint::source(cr_, color);
    cairo_move_to(cr_, r.x + 4, r.y); // the same inset as paint::label
    pango_cairo_show_layout(cr_, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

void Installer::button(const Rect& r, Action action, int index, const std::string& label, bool primary,
                       bool enabled, double size) {
    hits_.push_back({r, action, index, enabled});
    const bool hover = enabled && hovered(action, index);
    if (primary && enabled) {
        paint::latched(cr_, theme_, r, theme_.radius);
        if (hover) {
            // The latched fill has no hover of its own: lift it a little.
            paint::rounded(cr_, r, theme_.radius);
            cairo_set_source_rgba(cr_, 1, 1, 1, .08);
            cairo_fill(cr_);
        }
    } else {
        paint::cap(cr_, theme_, r, hover, false, enabled);
    }
    text(label, r, size, enabled ? theme_.legend : dim());
}

void Installer::segments(const Rect& r, Action action, int chosen) {
    paint::well(cr_, theme_, r);
    const char* labels[] = {"installer.yes", "installer.no"};
    for (int i = 0; i < 2; ++i) {
        const Rect segment{r.x + i * r.width / 2, r.y, r.width / 2, r.height};
        hits_.push_back({segment, action, i, true});
        const Rect thumb{segment.x + 4, segment.y + 4, segment.width - 8, segment.height - 8};
        if (chosen == i) {
            paint::latched(cr_, theme_, thumb, theme_.radius);
        } else if (hovered(action, i)) {
            paint::rounded(cr_, thumb, theme_.radius);
            paint::source(cr_, theme_.hover);
            cairo_fill(cr_);
        }
        text(tr(labels[i]), segment, 22, theme_.legend);
    }
}

void Installer::spinner(double x, double y, double radius, double now) {
    const double start = std::fmod(now * 1.4, 1.0) * 2 * pi;
    cairo_new_path(cr_);
    cairo_arc(cr_, x, y, radius, start, start + 1.4 * pi);
    cairo_set_source_rgba(cr_, theme_.legend.r, theme_.legend.g, theme_.legend.b, .8);
    cairo_set_line_width(cr_, 3);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr_);
}

void Installer::paint_keyboard(double slide) {
    if (!keyboard_ || cairo_surface_status(keyboard_.get()) != CAIRO_STATUS_SUCCESS) {
        return;
    }
    const double scale = picture.width / cairo_image_surface_get_width(keyboard_.get());
    const double y = picture.y - slide_distance * slide;
    // The pattern's own matrix places the picture, so the mask below stays
    // in canvas coordinates.
    auto* image = cairo_pattern_create_for_surface(keyboard_.get());
    cairo_matrix_t placed;
    cairo_matrix_init_scale(&placed, 1 / scale, 1 / scale);
    cairo_matrix_translate(&placed, -picture.x, -y);
    cairo_pattern_set_matrix(image, &placed);
    cairo_pattern_set_filter(image, CAIRO_FILTER_GOOD);
    cairo_set_source(cr_, image);
    cairo_pattern_destroy(image);
    if (slide <= 0) {
        cairo_paint(cr_);
        return;
    }
    // Fades into the case colour downward: dim at the top, gone before the card.
    const double fade_end = height + (card.y - 60 - height) * slide;
    auto* mask = cairo_pattern_create_linear(0, 0, 0, fade_end);
    cairo_pattern_add_color_stop_rgba(mask, 0, 0, 0, 0, 1 - .86 * slide);
    cairo_pattern_add_color_stop_rgba(mask, 1, 0, 0, 0, 1 - slide);
    cairo_mask(cr_, mask);
    cairo_pattern_destroy(mask);
}

// ---- screens

void Installer::paint(double now) {
    now_ = now;
    cr_ = cairo_create(canvas_.get());
    hits_.clear();
    paint::source(cr_, theme_.surface);
    cairo_paint(cr_);
    if (screen_ == Screen::Home) {
        paint_home();
    } else {
        paint_setup(now);
    }
    // The ×, as on the keyboard; it waits while something runs.
    button({width - 76, 16, 60, 48}, Action::Close, 0, "×", false, !busy(), 28);
    cairo_destroy(cr_);
    cr_ = nullptr;
    cairo_surface_flush(canvas_.get());
}

void Installer::paint_home() {
    paint_keyboard(0);
    const bool loading = status_.task == Task::Releases && status_.state == State::Running;
    const bool list_failed = status_.task == Task::Releases && status_.state == State::Failed;
    const bool uninstalling = status_.task == Task::Uninstall && status_.state == State::Running;

    // Above the button: what is installed, or what just happened.
    std::string line;
    if (!home_message_.key.empty()) {
        line = localization_.text(home_message_);
    } else if (!installed_.empty()) {
        line = tr("installer.installed", {{"version", installed_}});
    }
    text(line, {240, 506, 800, 36}, 21, dim());

    const Rect main{440, 560, 320, 76}, more{764, 560, 76, 76};
    if (list_failed) {
        button({main.x, main.y, main.width + 4 + more.width, main.height}, Action::Retry, 0,
               tr("installer.retry"), true);
    } else {
        const bool ready = !releases_.empty() && !uninstalling;
        button(main, Action::Install, 0,
               tr(installed_.empty() ? "installer.install" : "installer.reinstall"), true, ready, 28);
        button(more, Action::Versions, 0, "", true, ready);
        // The list's chevron: down, and up while the list is open above it.
        const double x = more.x + more.width / 2, y = more.y + more.height / 2;
        const double turn = versions_open_ ? -1 : 1;
        cairo_move_to(cr_, x - 9, y - 4 * turn);
        cairo_line_to(cr_, x, y + 5 * turn);
        cairo_line_to(cr_, x + 9, y - 4 * turn);
        const Color ink = ready ? theme_.legend : dim();
        cairo_set_source_rgba(cr_, ink.r, ink.g, ink.b, .85);
        cairo_set_line_width(cr_, 2.5);
        cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr_);
    }

    // Below it: the chosen version, or why there is none.
    std::string caption;
    Color tone = dim();
    if (loading) {
        caption = tr("installer.loading");
    } else if (list_failed) {
        caption = localization_.text(status_.error);
        tone = theme_.error;
    } else if (!releases_.empty()) {
        caption = tr(selected_ == 0 ? "installer.version.latest" : "installer.version",
                     {{"version", releases_[selected_].version}});
    }
    text(caption, {240, 644, 800, 32}, 20, tone);

    if (!installed_.empty()) {
        button({540, 700, 200, 52}, Action::Uninstall, 0, tr("installer.uninstall"), false,
               !uninstalling && !list_failed, 20);
    }
    if (versions_open_) {
        paint_versions();
    }
    if (confirm_open_) {
        paint_confirm();
    }
}

void Installer::paint_versions() {
    constexpr double row = 56, pad = 8, list_width = 300;
    const double list_height = static_cast<double>(releases_.size()) * row + 2 * pad;
    const Rect list{840 - list_width, 552 - list_height, list_width, list_height};
    paint::settings_card(cr_, theme_, list);
    hits_.push_back({list, Action::None, 0, false}); // presses between rows stay in the list
    for (std::size_t i = 0; i < releases_.size(); ++i) {
        const Rect r{list.x + pad, list.y + pad + static_cast<double>(i) * row, list.width - 2 * pad,
                     row - 4};
        hits_.push_back({r, Action::Version, static_cast<int>(i), true});
        const bool hover = hovered(Action::Version, static_cast<int>(i));
        if (i == selected_) {
            paint::latched(cr_, theme_, r, theme_.radius);
        } else if (hover) {
            paint::rounded(cr_, r, theme_.radius);
            paint::source(cr_, theme_.hover);
            cairo_fill(cr_);
        }
        text(tr(i == 0 ? "installer.version.latest" : "installer.version",
                {{"version", releases_[i].version}}),
             {r.x + 14, r.y, r.width - 28, r.height}, 21, theme_.legend, PANGO_ALIGN_LEFT);
    }
}

void Installer::paint_confirm() {
    // Dims everything behind it and takes every press.
    cairo_set_source_rgba(cr_, 0, 0, 0, .55);
    cairo_paint(cr_);
    hits_.push_back({{0, 0, width, height}, Action::None, 0, false});
    const bool running = status_.task == Task::Uninstall && status_.state == State::Running;
    const Rect dialog{290, 190, 700, 420};
    paint::settings_card(cr_, theme_, dialog);
    const double x = dialog.x + card_margin, w = dialog.width - 2 * card_margin;
    text(tr("installer.uninstall.title"), {x, dialog.y + 36, w, 50}, 32, theme_.legend,
         PANGO_ALIGN_LEFT);
    paragraph(tr("installer.uninstall.text"), {x, dialog.y + 100, w, 90}, 21, dim());
    text(tr("installer.uninstall.keep"), {x, dialog.y + 214, w - 260, 56}, 22, theme_.legend,
         PANGO_ALIGN_LEFT);
    segments({x + w - 240, dialog.y + 210, 240, 60}, Action::KeepSettings, keep_settings_ ? 0 : 1);
    const double buttons = dialog.y + dialog.height - card_margin - 60;
    button({x, buttons, 200, 60}, Action::CancelUninstall, 0, tr("installer.cancel"), false, !running);
    if (running) {
        hits_.push_back({{x + w - 240, buttons, 240, 60}, Action::None, 0, false});
        paint::cap(cr_, theme_, {x + w - 240, buttons, 240, 60}, false, false, false);
        spinner(x + w - 120, buttons + 30, 13, now_);
    } else {
        button({x + w - 240, buttons, 240, 60}, Action::ConfirmUninstall, 0, tr("installer.uninstall"),
               true);
    }
}

void Installer::paint_setup(double now) {
    paint_keyboard(ease_out((now - setup_started_) / slide_seconds));
    paint_progress();
    paint::settings_card(cr_, theme_, card);
    if (page_ < pages) {
        paint_wizard_page();
    } else {
        paint_finish_page(now);
    }
}

void Installer::paint_progress() {
    const bool installing = status_.task == Task::Install;
    const bool failed = installing && status_.state == State::Failed;
    std::string title = tr(install_done_    ? "installer.installed.title"
                           : first_install_ ? "installer.installing"
                                            : "installer.reinstalling",
                           {{"version", installing_}});
    text(title, {200, 28, 880, 52}, 30, theme_.legend);

    // Download takes most of the bar; checking and installing the rest.
    double fraction = 1;
    if (installing && !install_done_) {
        switch (status_.stage) {
        case Stage::Download:
            fraction = .8 * status_.download;
            break;
        case Stage::Verify:
            fraction = .84;
            break;
        case Stage::Stopping:
            fraction = .88;
            break;
        case Stage::Install:
            fraction = .92;
            break;
        }
    }
    const Rect bar{290, 96, 700, 22};
    paint::well(cr_, theme_, bar);
    if (fraction > 0) {
        const Rect fill{bar.x + 4, bar.y + 4, std::max(14.0, (bar.width - 8) * fraction),
                        bar.height - 8};
        paint::latched(cr_, theme_, fill, fill.height / 2);
    }

    std::string line;
    Color tone = dim();
    if (failed) {
        line = localization_.text(status_.error);
        tone = theme_.error;
    } else if (install_done_) {
        line = tr("installer.stage.done");
    } else if (installing) {
        switch (status_.stage) {
        case Stage::Download:
            line = tr("installer.stage.download",
                      {{"percent", static_cast<long long>(std::lround(status_.download * 100))}});
            break;
        case Stage::Verify:
            line = tr("installer.stage.verify");
            break;
        case Stage::Stopping:
            line = tr("installer.stage.stopping");
            break;
        case Stage::Install:
            line = tr("installer.stage.install");
            break;
        }
    }
    if (status_.task == Task::Finish && status_.state == State::Failed) {
        line = localization_.text(status_.error);
        tone = theme_.error;
    }
    if (failed) {
        text(line, {200, 130, 660, 60}, 20, tone, PANGO_ALIGN_RIGHT, true);
        button({880, 136, 160, 48}, Action::Retry, 0, tr("installer.retry"), false, true, 20);
    } else {
        text(line, {200, 130, 880, 36}, 20, tone);
    }
}

void Installer::paint_wizard_page() {
    const double x = card.x + card_margin, w = card.width - 2 * card_margin;
    text(tr("installer.page-count",
            {{"page", static_cast<long long>(page_ + 1)}, {"count", static_cast<long long>(pages)}}),
         {card.x + card.width - card_margin - 120, card.y + 36, 120, 40}, 20, dim(), PANGO_ALIGN_RIGHT);
    text(tr(page_keys[page_].first), {x, card.y + 32, w - 130, 56}, 34, theme_.legend, PANGO_ALIGN_LEFT);
    paragraph(tr(page_keys[page_].second), {x, card.y + 108, w, 150}, 22, dim());

    std::optional<bool> value;
    switch (page_) {
    case 0:
        value = choices_.replace_native;
        break;
    case 1:
        value = choices_.swipe;
        break;
    case 2:
        value = choices_.suggestions;
        break;
    case 3:
        value = choices_.pull_for_shift;
        break;
    case 4:
        value = choices_.hide_numpad;
        break;
    }
    segments({x, card.y + 280, 360, 64}, Action::Choice, value ? (*value ? 0 : 1) : -1);

    const double buttons = card.y + card.height - card_margin - 60;
    if (page_ > 0) {
        button({x, buttons, 180, 60}, Action::Back, 0, tr("installer.back"), false);
    }
    // Picking doesn't advance; Next waits for the one choice without a default.
    button({x + w - 180, buttons, 180, 60}, Action::Next, 0, tr("installer.next"), false,
           value.has_value());
}

void Installer::paint_finish_page(double now) {
    const double x = card.x + card_margin, w = card.width - 2 * card_margin;
    const char* note = !first_install_           ? "installer.ready.reinstall"
                       : choices_.replace_native ? "installer.ready.replace"
                                                 : "installer.ready.manual";
    text(tr(note), {x, card.y + 56, w, 120}, 24, dim(), PANGO_ALIGN_CENTER, true);

    const bool installing = status_.task == Task::Install && status_.state == State::Running;
    const bool finishing = status_.task == Task::Finish && status_.state == State::Running;
    const Rect finish{card.x + (card.width - 360) / 2, card.y + 220, 360, 84};
    const bool enabled = install_done_ && !finish_pressed_;
    button(finish, Action::Finish, 0, "", true, enabled);
    // The label, with a spinner beside it while waiting.
    const std::string label = tr("installer.finish");
    if (installing || finishing) {
        spinner(finish.x + finish.width / 2 - 70, finish.y + finish.height / 2, 12, now);
        text(label, {finish.x + 40, finish.y, finish.width - 40, finish.height}, 28, dim());
    } else {
        text(label, finish, 28, enabled ? theme_.legend : dim());
    }
    // Back returns to the wizard until Finish is pressed.
    if (first_install_ && !finish_pressed_) {
        button({x, card.y + card.height - card_margin - 60, 180, 60}, Action::Back, 0,
               tr("installer.back"), false);
    }
}

std::optional<Rect> Installer::control(Action action, int index) const {
    for (const auto& hit : hits_) {
        if (hit.action == action && hit.index == index) {
            return hit.bounds;
        }
    }
    return std::nullopt;
}

} // namespace framekeyboard::installer
