#include "installer.hpp"
#include "framekeyboard/paint.hpp"
#include "framekeyboard/panel.hpp"
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
// The keyboard picture on Home: the case, 1600 x panel_height, at 56% and
// centred. In the picture the case ends 588 pixels down, where its
// reflection starts here. Setup slides it up.
constexpr double picture_scale = .56, case_bottom = 588;
constexpr Rect picture{(Installer::width - panel_width * picture_scale) / 2, 48,
                       panel_width * picture_scale, panel_height * picture_scale};
constexpr double slide_distance = 300, slide_seconds = .7;
// How far a pressed button moves down: the keys' own travel.
constexpr double press_travel = 3;
// The same picture in the wizard: whole, as wide as a page's stage allows,
// and at half size for a closer look at its right end. Its numpad starts
// 1306 pixels across; without it the case ends 20 further.
constexpr double mini_scale = .2625, half_scale = .5, numpad_left = 1306, tenkeyless_width = 1326;
// Where keys lie in the picture: the main block's left edge and key pitch,
// and the middle of each row from the digits down.
constexpr double keys_left = 146, key_pitch = 63.6;
constexpr std::array<double, 5> row_middle{232, 307, 381, 456, 531};
// Home: Install and its version list's chevron, one control, centred.
constexpr Rect install_main{430, 554, 340, 76}, install_more{770, 554, 80, 76};
// Setup's card: each page's picture on a stage to the left, its text to the
// right, and Back, the page dots and Next along the bottom.
constexpr Rect card{140, 184, 1000, 592};
constexpr double card_pad = 40, footer_y = card.y + card.height - 36 - 60;
constexpr Rect stage{card.x + card_pad, card.y + card_pad, 460, footer_y - 28 - card.y - card_pad};
constexpr double column_x = stage.x + stage.width + 40,
                 column_width = card.x + card.width - card_pad - column_x;
// A page turn slides the old page out and the new one in, fading both.
constexpr double page_seconds = .45;
// The swipe and suggestion pictures: three rows of letters.
constexpr std::array<const char*, 3> letter_rows{"qwertyuiop", "asdfghjkl", "zxcvbnm"};
constexpr std::array<const char*, 3> demo_words{"swipe", "frame", "keys"};
constexpr double demo_step = .32, demo_hold = 1.1, demo_fade = .35;
double ease_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
double ease_in_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < .5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}
// 0..1 across [start, start + length].
double phase(double t, double start, double length) {
    return std::clamp((t - start) / length, 0.0, 1.0);
}

// Three rows of letter keys centred in `area`, staggered as on a keyboard.
struct Letters {
    double left, top, key, pitch;
    Letters(const Rect& area, double size, double spacing) : key(size), pitch(spacing) {
        left = area.x + (area.width - (9.5 * pitch + key)) / 2;
        top = area.y + (area.height - (2 * pitch + key)) / 2;
    }
    Rect at(int row, int column) const {
        const double stagger[] = {0, .25, .75};
        return {left + (stagger[row] + column) * pitch, top + row * pitch, key, key};
    }
    Rect of(char letter) const {
        for (int row = 0; row < 3; ++row) {
            if (const char* at_letter = std::strchr(letter_rows[row], letter)) {
                return at(row, static_cast<int>(at_letter - letter_rows[row]));
            }
        }
        return {};
    }
};

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
    {"installer.page.use-numpad", "installer.page.use-numpad.text"},
    {"installer.page.streaming", "installer.page.streaming.text"},
}};
// Finish's summary names each answer as Settings does, not by its question.
constexpr std::array<const char*, Installer::pages> summary_keys{
    "setting.replace-native-keyboard", "setting.swipe-typing",
    "setting.typing-suggestions",      "setting.key-flicks",
    "installer.summary.numpad",        "setting.streaming-support"};

// Whether a dotted version is at least another.
bool at_least(std::string_view version, std::string_view minimum) {
    const auto next = [](std::string_view& text) {
        long value = 0;
        std::size_t used = 0;
        while (used < text.size() && text[used] >= '0' && text[used] <= '9') {
            value = value * 10 + (text[used++] - '0');
        }
        text.remove_prefix(std::min(text.size(), used + 1)); // the number and its dot
        return value;
    };
    while (!version.empty() || !minimum.empty()) {
        const long have = next(version), want = next(minimum);
        if (have != want) {
            return have > want;
        }
    }
    return true;
}

void set_rgba(cairo_t* cr, Color color, double alpha) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
}

// A copy of `source` at `scale`, filtered well once.
cairo_surface_t* scaled(cairo_surface_t* source, double scale) {
    auto* copy = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(cairo_image_surface_get_width(source) * scale)),
        static_cast<int>(std::ceil(cairo_image_surface_get_height(source) * scale)));
    auto* cr = cairo_create(copy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    return copy;
}

} // namespace

Installer::Installer(Backend& backend, Theme theme, const Localization& localization)
    : backend_(backend), theme_(std::move(theme)), localization_(localization),
      font_(localization.fonts()),
      canvas_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy),
      hero_(nullptr, cairo_surface_destroy), mini_(nullptr, cairo_surface_destroy),
      half_(nullptr, cairo_surface_destroy), backdrop_(nullptr, cairo_surface_destroy),
      grounded_(nullptr, cairo_surface_destroy), card_(nullptr, cairo_surface_destroy),
      plate_(nullptr, cairo_surface_destroy) {
    PngReader reader{keyboard_png, keyboard_png_size};
    Surface image(cairo_image_surface_create_from_png_stream(read_png, &reader), cairo_surface_destroy);
    if (cairo_surface_status(image.get()) == CAIRO_STATUS_SUCCESS) {
        // Only the case: the README picture also has the movement line below
        // it, and its x would read as the installer's own (App's close
        // control: body.x + body.width - 78, 12, 60 x 42).
        Surface keyboard(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, panel_height),
                         cairo_surface_destroy);
        auto* cr = cairo_create(keyboard.get());
        cairo_set_source_surface(cr, image.get(), 0, 0);
        cairo_paint(cr);
        paint::source(cr, theme_.surface);
        cairo_rectangle(cr, panel_width - 84, 6, 72, 54);
        cairo_fill(cr);
        cairo_destroy(cr);
        hero_.reset(scaled(keyboard.get(), picture_scale));
        mini_.reset(scaled(keyboard.get(), mini_scale));
        half_.reset(scaled(keyboard.get(), half_scale));
    }
    paint_backdrop();
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
double Installer::choice_shown(int page) const {
    const double target = choice(page) ? 1 : 0;
    if (page != page_) {
        return target;
    }
    const double turned = ease_in_out((now_ - choice_changed_) / .4);
    return 1 - target + (2 * target - 1) * turned;
}
bool Installer::choice(int page) const {
    switch (page) {
    case 0:
        return choices_.replace_native;
    case 1:
        return choices_.swipe;
    case 2:
        return choices_.suggestions;
    case 3:
        return choices_.pull_for_shift;
    case 4:
        return choices_.numpad;
    default:
        return streaming_page() && choices_.streaming && streaming_blocked().key.empty();
    }
}
bool Installer::streaming_page() const {
    // An older release cannot take the answer, so it is not asked.
    return installing_.empty() || at_least(installing_, streaming_release);
}
UiText Installer::streaming_blocked() const {
    // It types through replacement's helper.
    return choices_.replace_native ? UiText{} : ui("installer.page.streaming.needs-replace");
}
bool Installer::restart_offered() const {
    return first_install_ && choice(pages - 1) && status_.streaming_restart;
}
bool Installer::streaming_failed() const {
    return status_.task == Task::Finish && status_.state == State::Failed &&
           status_.error.key == "installer.error.streaming";
}
double Installer::progress() const {
    if (status_.task != Task::Install || install_done_) {
        return status_.task == Task::None ? 0 : 1;
    }
    // Download takes most of the bar; checking and installing the rest.
    switch (status_.stage) {
    case Stage::Download:
        return .8 * status_.download;
    case Stage::Verify:
        return .84;
    case Stage::Stopping:
        return .88;
    case Stage::Install:
        return .92;
    }
    return 0;
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
    if (!live_) {
        return false;
    }
    const auto* hit = hit_at(pointer_x_, pointer_y_);
    return hit && hit->enabled && hit->action == action && hit->index == index &&
           (!pressed_ || (pressed_->action == action && pressed_->index == index));
}
bool Installer::held(Action action, int index) const {
    return live_ && pressed_ && pressed_->action == action && pressed_->index == index &&
           hovered(action, index);
}
void Installer::resize(int canvas_width, int canvas_height) {
    canvas_width = std::max(canvas_width, width);
    canvas_height = std::max(canvas_height, height);
    if (canvas_width == canvas_width_ && canvas_height == canvas_height_) {
        return;
    }
    canvas_width_ = canvas_width;
    canvas_height_ = canvas_height;
    offset_x_ = std::floor((canvas_width - width) / 2.0);
    offset_y_ = std::floor((canvas_height - height) / 2.0);
    canvas_.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_width, canvas_height));
    paint_backdrop();
    dirty_ = true;
}
void Installer::pointer_move(double x, double y) {
    move(x - offset_x_, y - offset_y_);
}
void Installer::move(double x, double y) {
    // Only a change of the control under the laser changes the picture.
    const auto under = [&](double at_x, double at_y) {
        const auto* hit = hit_at(at_x, at_y);
        return hit && hit->enabled ? std::pair{static_cast<int>(hit->action), hit->index}
                                   : std::pair{-1, 0};
    };
    dirty_ = dirty_ || under(x, y) != under(pointer_x_, pointer_y_);
    pointer_x_ = x;
    pointer_y_ = y;
}
void Installer::pointer_leave() {
    pointer_x_ = pointer_y_ = -1;
    pressed_.reset();
    dirty_ = true;
}
void Installer::pointer_down(double x, double y) {
    x -= offset_x_;
    y -= offset_y_;
    dirty_ = true;
    move(x, y);
    const auto* hit = hit_at(x, y);
    if (hit && hit->enabled) {
        pressed_ = *hit;
    } else {
        pressed_.reset();
        versions_open_ = false; // A press outside the list closes it.
    }
}
void Installer::pointer_up(double x, double y) {
    x -= offset_x_;
    y -= offset_y_;
    dirty_ = true;
    move(x, y);
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
        confirm_opened_ = now_;
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
        if (choice(page_) != yes) {
            choice_changed_ = now_; // The page's picture follows the choice.
        }
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
            choices_.numpad = yes;
            break;
        case 5:
            choices_.streaming = yes;
            break;
        }
        break;
    }
    case Action::Restart:
        // Once Finish is pressed its choice is sent: the box can no longer change it.
        if (!finish_pressed_) {
            restart_ = !restart_;
        }
        break;
    case Action::FinishWithout: {
        // The rest of the choices as they were; this one becomes No.
        choices_.streaming = false;
        finish_pressed_ = true;
        backend_.finish(choices_, false);
        break;
    }
    case Action::Back:
        turn_page(std::max(0, page_ - 1));
        break;
    case Action::Next:
        turn_page(std::min(wizard_pages(), page_ + 1));
        break;
    case Action::Finish:
        finish_pressed_ = true;
        if (first_install_) {
            // The answer as shown: No where the page could not be answered.
            auto choices = choices_;
            choices.streaming = choice(pages - 1);
            backend_.finish(choices, restart_ && restart_offered());
        } else {
            quit_ = true;
        }
        break;
    case Action::Retry:
        if (screen_ == Screen::Setup) {
            shown_progress_ = 0;
            backend_.install(releases_[selected_]);
        } else {
            backend_.releases();
        }
        break;
    case Action::None:
        break;
    }
}

void Installer::turn_page(int page) {
    if (page == page_) {
        return;
    }
    page_from_ = page_;
    page_from_turned_ = page_turned_;
    page_ = page;
    page_turned_ = now_;
}

void Installer::begin_install() {
    if (releases_.empty()) {
        return;
    }
    screen_ = Screen::Setup;
    first_install_ = installed_.empty();
    installing_ = releases_[selected_].version;
    // Reinstalls and other versions don't show the wizard (owner's design).
    page_ = page_from_ = first_install_ ? 0 : wizard_pages();
    page_turned_ = now_;
    shown_progress_ = 0;
    setup_started_ = -1; // set on the next tick
    backend_.install(releases_[selected_]);
}

// ---- backend

void Installer::poll() {
    const auto stage_before = status_.stage;
    status_ = backend_.status();
    const bool changed = status_.task != seen_task_ || status_.state != seen_state_;
    seen_task_ = status_.task;
    seen_state_ = status_.state;
    dirty_ = dirty_ || changed || status_.stage != stage_before;
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
        done_at_ = now_;
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
            // A restart that failed is not tried again: everything else is set
            // up, so the next Finish opens the keyboard, and the message says
            // to restart from the power menu.
            if (status_.error.key == "installer.error.restart") {
                restart_ = false;
            }
        }
    }
}

double Installer::frame_interval() const {
    constexpr double every_frame = 0, spinner = 1.0 / 30, never = 1e9;
    const bool working = status_.state == State::Running;
    if (screen_ == Screen::Home) {
        // The dialog's entrance; otherwise only a spinner moves.
        return confirm_open_ && now_ - confirm_opened_ < .3 ? every_frame : working ? spinner : never;
    }
    // The slide into Setup, a page turn and the tick that draws itself when
    // the install is done.
    if (now_ - setup_started_ < 1 || now_ - page_turned_ < page_seconds + .05 ||
        (install_done_ && now_ - done_at_ < .8)) {
        return every_frame;
    }
    // A wizard page's picture loops for as long as the page shows, while the
    // download and install run: thirty frames a second are enough for it.
    if (page_ < wizard_pages()) {
        return spinner;
    }
    // Finish: the bar and the ring while the install runs, then nothing.
    return working || std::abs(progress() - shown_progress_) > .001 ? spinner : never;
}

bool Installer::tick(double now) {
    now_ = now;
    poll();
    if (screen_ == Screen::Setup && setup_started_ < 0) {
        setup_started_ = now;
    }
    const double interval = frame_interval();
    const bool settling = moving_ && interval > 1; // one last frame after motion ends
    if (!dirty_ && !settling && now - painted_at_ < interval) {
        return false;
    }
    // The bar eases toward the backend's progress and never runs backward
    // except when an install starts over.
    const double target = progress();
    const double step = std::clamp((now - last_tick_) * 6, 0.0, 1.0);
    shown_progress_ =
        target < shown_progress_ ? target : shown_progress_ + (target - shown_progress_) * step;
    last_tick_ = now;
    paint(now);
    dirty_ = false;
    moving_ = interval <= 1;
    painted_at_ = now;
    return true;
}

template <typename Draw> void Installer::cached(Surface& cache, const Rect& bounds, Draw&& draw) {
    if (!cache) {
        cache.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               static_cast<int>(std::ceil(bounds.width)),
                                               static_cast<int>(std::ceil(bounds.height))));
        auto* frame = cr_;
        cr_ = cairo_create(cache.get());
        cairo_translate(cr_, -bounds.x, -bounds.y);
        draw();
        cairo_destroy(cr_);
        cr_ = frame;
    }
    cairo_set_source_surface(cr_, cache.get(), bounds.x, bounds.y);
    cairo_paint(cr_);
}

// ---- painting helpers

void Installer::text(const std::string& value, const Rect& r, double size, Color color,
                     PangoAlignment alignment, bool wrap) {
    paint::label(cr_, value, r, size, font_, color, alignment, wrap);
}

double Installer::block(const std::string& value, const Rect& r, double size, Color color,
                        PangoWeight weight, bool draw, PangoAlignment alignment, double spacing) {
    auto* layout = pango_cairo_create_layout(cr_);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, font_.c_str());
    pango_font_description_set_weight(font, weight);
    pango_layout_set_auto_dir(layout, FALSE);
    pango_layout_set_alignment(layout, alignment);
    pango_layout_set_width(layout, static_cast<int>((r.width - 8) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    // Headings sit tight; paragraphs get room between their lines.
    pango_layout_set_line_spacing(layout, spacing > 0                    ? static_cast<float>(spacing)
                                          : weight > PANGO_WEIGHT_NORMAL ? 1.05f
                                                                         : 1.3f);
    pango_layout_set_text(layout, value.c_str(), static_cast<int>(value.size()));
    // Longer translations shrink a little rather than run into the controls.
    int width{}, height{};
    for (;; size *= .95) {
        pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_layout_get_pixel_size(layout, &width, &height);
        if (height <= r.height || size < 14) {
            break;
        }
    }
    if (draw) {
        paint::source(cr_, color);
        cairo_move_to(cr_, r.x + 4, r.y); // the same inset as paint::label
        pango_cairo_show_layout(cr_, layout);
    }
    pango_font_description_free(font);
    g_object_unref(layout);
    return height;
}

double Installer::text_width(const std::string& value, double size) const {
    auto* layout = pango_cairo_create_layout(cr_);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, font_.c_str());
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM); // as paint::label
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, value.c_str(), static_cast<int>(value.size()));
    int width{}, height{};
    pango_layout_get_pixel_size(layout, &width, &height);
    pango_font_description_free(font);
    g_object_unref(layout);
    return width;
}

void Installer::button(const Rect& bounds, Action action, int index, const std::string& label,
                       bool primary, bool enabled, double size) {
    if (live_) {
        hits_.push_back({bounds, action, index, enabled});
    }
    const bool hover = enabled && hovered(action, index);
    // Pressed, it moves down without changing size, as the keys do.
    const Rect r{bounds.x, bounds.y + (enabled && held(action, index) ? press_travel : 0), bounds.width,
                 bounds.height};
    if (primary && enabled) {
        paint::latched(cr_, theme_, r, theme_.radius);
        if (hover) {
            // The latched fill has no hover of its own: lift it a little.
            paint::rounded(cr_, r, theme_.radius);
            cairo_set_source_rgba(cr_, 1, 1, 1, .08);
            cairo_fill(cr_);
        }
    } else if (primary) {
        // The main action while it can't be taken: its own fill, far back.
        cairo_push_group(cr_);
        paint::latched(cr_, theme_, r, theme_.radius);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, .3);
    } else {
        paint::cap(cr_, theme_, r, hover, false, enabled);
    }
    text(label, r, size, !enabled ? dim() : primary ? theme_.latched_legend : theme_.legend);
}

void Installer::split_button(const Rect& main_bounds, const Rect& more_bounds, bool ready) {
    hits_.push_back({main_bounds, Action::Install, 0, ready});
    hits_.push_back({more_bounds, Action::Versions, 0, ready});
    // Both halves go down together under a press on either.
    const double down = ready && (held(Action::Install) || held(Action::Versions)) ? press_travel : 0;
    const Rect main{main_bounds.x, main_bounds.y + down, main_bounds.width, main_bounds.height};
    const Rect more{more_bounds.x, more_bounds.y + down, more_bounds.width, more_bounds.height};
    const Rect whole{main.x, main.y, more.x + more.width - main.x, main.height};
    if (ready) {
        paint::latched(cr_, theme_, whole, theme_.radius);
        // Each half lifts on its own under the laser, and the chevron's
        // while its list is open.
        for (const auto& [part, action, lift] :
             {std::tuple{main, Action::Install, hovered(Action::Install) ? .08 : 0.0},
              std::tuple{more, Action::Versions,
                         hovered(Action::Versions) ? .08
                         : versions_open_          ? .05
                                                   : 0.0}}) {
            if (lift > 0) {
                cairo_save(cr_);
                cairo_rectangle(cr_, part.x, part.y, part.width, part.height);
                cairo_clip(cr_);
                paint::rounded(cr_, whole, theme_.radius);
                cairo_set_source_rgba(cr_, 1, 1, 1, lift);
                cairo_fill(cr_);
                cairo_restore(cr_);
            }
        }
        // A groove between the halves, in the case's colour.
        cairo_rectangle(cr_, more.x - 1, whole.y + 16, 2, whole.height - 32);
        set_rgba(cr_, theme_.surface, .45);
        cairo_fill(cr_);
    } else {
        cairo_push_group(cr_);
        paint::latched(cr_, theme_, whole, theme_.radius);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, .3);
    }
    const Color ink = ready ? theme_.latched_legend : dim();
    text(tr(installed_.empty() ? "installer.install" : "installer.reinstall"), main, 28, ink);
    // The list's chevron: down, and up while the list is open above it.
    const double x = more.x + more.width / 2, y = more.y + more.height / 2;
    const double turn = versions_open_ ? -1 : 1;
    cairo_move_to(cr_, x - 9, y - 4 * turn);
    cairo_line_to(cr_, x, y + 5 * turn);
    cairo_line_to(cr_, x + 9, y - 4 * turn);
    set_rgba(cr_, ink, .9);
    cairo_set_line_width(cr_, 2.5);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr_);
}

void Installer::segments(const Rect& r, Action action, int chosen, bool enabled) {
    if (!enabled) {
        cairo_push_group(cr_); // The answer it is held at, far back.
    }
    paint::well(cr_, theme_, r);
    const char* labels[] = {"installer.yes", "installer.no"};
    for (int i = 0; i < 2; ++i) {
        const Rect segment{r.x + i * r.width / 2, r.y, r.width / 2, r.height};
        if (live_) {
            hits_.push_back({segment, action, i, enabled});
        }
        const Rect thumb{segment.x + 4, segment.y + 4, segment.width - 8, segment.height - 8};
        if (chosen == i) {
            paint::latched(cr_, theme_, thumb, theme_.radius);
        } else if (enabled && hovered(action, i)) {
            paint::rounded(cr_, thumb, theme_.radius);
            paint::source(cr_, theme_.hover);
            cairo_fill(cr_);
        }
        text(tr(labels[i]), segment, 22,
             chosen == i ? theme_.latched_legend : paint::mix(theme_.legend, theme_.surface, .2));
    }
    if (!enabled) {
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, .4);
    }
}

void Installer::checkbox(const Rect& r, Action action, const std::string& label, bool checked,
                         bool enabled) {
    if (live_) {
        hits_.push_back({r, action, 0, enabled});
    }
    // Pressed, the box and its label go down together, as a key does.
    const double down = held(action) ? press_travel : 0;
    const Rect box{r.x + 12, r.y + (r.height - 32) / 2 + down, 32, 32};
    if (checked) {
        paint::latched(cr_, theme_, box, 8);
        cairo_new_path(cr_);
        cairo_move_to(cr_, box.x + 8, box.y + 17);
        cairo_line_to(cr_, box.x + 14, box.y + 23);
        cairo_line_to(cr_, box.x + 25, box.y + 10);
        cairo_set_line_width(cr_, 3);
        cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
        paint::source(cr_, theme_.latched_legend);
        cairo_stroke(cr_);
    } else {
        paint::well(cr_, theme_, box);
        if (hovered(action)) {
            paint::rounded(cr_, {box.x + 3, box.y + 3, box.width - 6, box.height - 6}, 6);
            paint::source(cr_, theme_.hover);
            cairo_fill(cr_);
        }
    }
    text(label, {box.x + box.width + 14, r.y + down, r.width - box.width - 38, r.height}, 21,
         theme_.legend, PANGO_ALIGN_LEFT);
}

void Installer::spinner(double x, double y, double radius) {
    cairo_new_path(cr_);
    cairo_arc(cr_, x, y, radius, 0, 2 * pi);
    set_rgba(cr_, theme_.legend, .12);
    cairo_set_line_width(cr_, 3);
    cairo_stroke(cr_);
    const double start = std::fmod(now_ * 1.4, 1.0) * 2 * pi;
    cairo_arc(cr_, x, y, radius, start, start + .6 * pi);
    set_rgba(cr_, theme_.legend, .85);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr_);
}

void Installer::key(const Rect& r, bool lit, double radius) {
    // The side shows below the face, as on the keyboard's shallow keys.
    const double depth = std::max(2.0, std::round(r.height * .07));
    paint::rounded(cr_, {r.x, r.y + depth, r.width, r.height - depth}, radius);
    paint::source(cr_, theme_.side_bottom);
    cairo_fill(cr_);
    const Rect face{r.x, r.y, r.width, r.height - depth};
    if (lit) {
        paint::latched(cr_, theme_, face, radius);
        return;
    }
    paint::rounded(cr_, face, radius);
    auto* shade = cairo_pattern_create_linear(0, face.y, 0, face.y + face.height);
    for (const auto& [at, color] :
         {std::pair{0.0, theme_.top}, {.5, theme_.middle}, {1.0, theme_.bottom}}) {
        cairo_pattern_add_color_stop_rgb(shade, at, color.r, color.g, color.b);
    }
    cairo_set_source(cr_, shade);
    cairo_fill(cr_);
    cairo_pattern_destroy(shade);
}

void Installer::field(const Rect& r, const std::string& value, double size, double alpha, bool caret) {
    paint::well(cr_, theme_, r);
    const double x = r.x + 20;
    const bool shown = !value.empty() && alpha > 0;
    if (shown) {
        cairo_push_group(cr_);
        text(value, {x - 4, r.y, r.width - 40, r.height}, size, theme_.legend, PANGO_ALIGN_LEFT);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, alpha);
    }
    if (caret && std::fmod(now_, 1.0) < .6) {
        const double at = x + (shown ? text_width(value, size) + 2 : 0);
        cairo_rectangle(cr_, at, r.y + r.height / 2 - size * .6, 2.5, size * 1.2);
        set_rgba(cr_, theme_.legend, .85);
        cairo_fill(cr_);
    }
}

// ---- the keyboard and backdrop
void Installer::paint_backdrop() {
    // The whole canvas, in screen coordinates from the screens' corner.
    backdrop_.reset(cairo_image_surface_create(CAIRO_FORMAT_RGB24, canvas_width_, canvas_height_));
    auto* pixels = cairo_image_surface_get_data(backdrop_.get());
    const int stride = cairo_image_surface_get_stride(backdrop_.get());
    // Lighter than the keyboard's case above, so the case stands out, and
    // darker toward the bottom; a faint glow of the latched colour behind the
    // keyboard; and corners that fall off into the case's dark. Closing is
    // the window's own (KDE, Gamescope).
    const auto top = paint::mix(theme_.surface, theme_.middle, .55);
    const auto bottom = paint::mix(theme_.surface, theme_.bottom, .12);
    const double cx = width / 2.0, glow_y = picture.y + picture.height * .5, dark_y = height * .42;
    // Shades this dark band on a headset's panels: each pixel is rounded
    // with a little noise, the same for its three channels.
    std::uint32_t noise = 0x9e3779b9;
    const auto random = [&] {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return (noise >> 8) / 16777216.0;
    };
    for (int row_index = 0; row_index < canvas_height_; ++row_index) {
        auto* row = reinterpret_cast<std::uint32_t*>(pixels + row_index * stride);
        const double y = row_index - offset_y_;
        for (int column = 0; column < canvas_width_; ++column) {
            const double x = column - offset_x_;
            Color c = paint::mix(top, bottom, std::clamp(y / (height - 1.0), 0.0, 1.0));
            const double glow = std::clamp(1 - std::hypot(x - cx, y - glow_y) / 720, 0.0, 1.0);
            c = paint::mix(c, theme_.latched, .2 * glow * glow * (3 - 2 * glow));
            const double dark = std::clamp((std::hypot(x - cx, y - dark_y) - 380) / 600, 0.0, 1.0);
            c = paint::mix(c, theme_.surface, .75 * dark);
            const double dither = random() + random() - 1;
            const auto channel = [&](double value) {
                return static_cast<std::uint32_t>(
                    std::clamp(std::lround(value * 255 + dither), 0L, 255L));
            };
            row[column] = channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
        }
    }
    cairo_surface_mark_dirty(backdrop_.get());
}
void Installer::paint_keyboard(double slide) {
    if (!hero_) {
        return;
    }
    const double y = std::round(picture.y - slide_distance * slide);
    const double floor = y + case_bottom * picture_scale;
    // At rest it is one picture with its shadow and reflection.
    constexpr Rect resting{picture.x - 20, picture.y - 4, picture.width + 40, picture.height + 70};
    if (slide <= 0 && (!grounded_ || cairo_get_target(cr_) != grounded_.get())) {
        cached(grounded_, resting, [&] { paint_keyboard(0); });
        return;
    }
    // Its shadow and reflection go as soon as it leaves the floor.
    const double grounded = 1 - std::min(1.0, slide * 5);
    if (grounded > 0) {
        // A soft shadow lifts the case off the backdrop.
        const double radius = theme_.surface_radius * picture_scale;
        for (int i = 1; i <= 6; ++i) {
            const double spread = i * 3.0;
            paint::rounded(cr_, {picture.x - spread / 2, y + spread, picture.width + spread, floor - y},
                           radius + spread);
            cairo_set_source_rgba(cr_, 0, 0, 0, .07 * grounded);
            cairo_fill(cr_);
        }
        // Its reflection on a polished floor, mirrored at the case's lower
        // edge and gone within a row of keys.
        constexpr double depth = 56;
        auto* mirrored = cairo_pattern_create_for_surface(hero_.get());
        cairo_matrix_t flip;
        cairo_matrix_init(&flip, 1, 0, 0, -1, -picture.x, floor + case_bottom * picture_scale);
        cairo_pattern_set_matrix(mirrored, &flip);
        cairo_save(cr_);
        cairo_rectangle(cr_, picture.x, floor + 2, picture.width, depth);
        cairo_clip(cr_);
        cairo_set_source(cr_, mirrored);
        auto* fade = cairo_pattern_create_linear(0, floor, 0, floor + depth);
        cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, .14 * grounded);
        cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, 0);
        cairo_mask(cr_, fade);
        cairo_pattern_destroy(fade);
        cairo_restore(cr_);
        cairo_pattern_destroy(mirrored);
    }
    cairo_set_source_surface(cr_, hero_.get(), picture.x, y);
    if (slide <= 0) {
        cairo_paint(cr_);
        return;
    }
    // Fades downward: dim at the top, gone before the card.
    const double fade_end = height + (card.y - 60 - height) * slide;
    auto* mask = cairo_pattern_create_linear(0, 0, 0, fade_end);
    cairo_pattern_add_color_stop_rgba(mask, 0, 0, 0, 0, 1 - .86 * slide);
    cairo_pattern_add_color_stop_rgba(mask, 1, 0, 0, 0, 1 - slide);
    cairo_mask(cr_, mask);
    cairo_pattern_destroy(mask);
}

void Installer::small_picture(cairo_surface_t* small, double s, double x, double y, double numpad) {
    if (!small) {
        return;
    }
    const double h = case_bottom * s;
    const Rect body{x, y, (tenkeyless_width + (panel_width - tenkeyless_width) * numpad) * s, h};
    const double radius = theme_.surface_radius * s;
    for (int i = 1; i <= 3; ++i) {
        paint::rounded(cr_, {body.x - i, body.y + i * 2, body.width + 2 * i, body.height}, radius + i);
        cairo_set_source_rgba(cr_, 0, 0, 0, .14);
        cairo_fill(cr_);
    }
    // The case at its width now, the keys up to the numpad on it, and the
    // numpad as far as it still shows.
    cairo_save(cr_);
    paint::rounded(cr_, body, radius);
    cairo_clip(cr_);
    paint::source(cr_, theme_.surface);
    cairo_paint(cr_);
    cairo_set_source_surface(cr_, small, x, y);
    cairo_rectangle(cr_, x, y, numpad_left * s, h);
    cairo_fill(cr_);
    if (numpad > 0) {
        cairo_rectangle(cr_, x + numpad_left * s, y, body.width - numpad_left * s, h);
        cairo_clip(cr_);
        cairo_paint_with_alpha(cr_, numpad * numpad);
    }
    cairo_restore(cr_);
}
void Installer::stage_plate(const Rect& r) {
    if (!plate_ || cairo_get_target(cr_) != plate_.get()) {
        cached(plate_, r, [&] { stage_plate(r); });
        return;
    }
    const double radius = theme_.radius + 4;
    cairo_save(cr_);
    paint::rounded(cr_, r, radius);
    cairo_clip(cr_);
    // Home's backdrop in small: lighter than the case, with the same glow.
    auto* fill = cairo_pattern_create_linear(0, r.y, 0, r.y + r.height);
    const auto top = paint::mix(theme_.surface, theme_.middle, .62);
    const auto bottom = paint::mix(theme_.surface, theme_.middle, .3);
    cairo_pattern_add_color_stop_rgb(fill, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(fill, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cr_, fill);
    cairo_paint(cr_);
    cairo_pattern_destroy(fill);
    const double cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    auto* glow = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r.width * .7);
    const auto& l = theme_.latched;
    cairo_pattern_add_color_stop_rgba(glow, 0, l.r, l.g, l.b, .16);
    cairo_pattern_add_color_stop_rgba(glow, 1, l.r, l.g, l.b, 0);
    cairo_set_source(cr_, glow);
    cairo_paint(cr_);
    cairo_pattern_destroy(glow);
    // Shaded under its top edge, as a well is.
    auto* shade = cairo_pattern_create_linear(0, r.y, 0, r.y + 14);
    cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, .35);
    cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
    cairo_set_source(cr_, shade);
    cairo_paint(cr_);
    cairo_pattern_destroy(shade);
    cairo_restore(cr_);
}

void Installer::case_plate(const Rect& r) {
    const double radius = theme_.radius + 6;
    for (int i = 1; i <= 4; ++i) {
        paint::rounded(cr_, {r.x - i, r.y + i * 2, r.width + 2 * i, r.height}, radius + i);
        cairo_set_source_rgba(cr_, 0, 0, 0, .1);
        cairo_fill(cr_);
    }
    paint::rounded(cr_, r, radius);
    paint::source(cr_, theme_.surface);
    cairo_fill(cr_);
}

void Installer::laser(const Rect& s, double x, double y, double alpha) {
    if (alpha <= 0) {
        return;
    }
    const Color ink = paint::mix(theme_.latched, theme_.legend, .45);
    auto* beam = cairo_pattern_create_linear(s.x + s.width, s.y + s.height, x, y);
    cairo_pattern_add_color_stop_rgba(beam, 0, ink.r, ink.g, ink.b, 0);
    cairo_pattern_add_color_stop_rgba(beam, 1, ink.r, ink.g, ink.b, .5 * alpha);
    cairo_move_to(cr_, s.x + s.width + 30, s.y + s.height + 60);
    cairo_line_to(cr_, x, y);
    cairo_set_source(cr_, beam);
    cairo_set_line_width(cr_, 3);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr_);
    cairo_pattern_destroy(beam);
    cairo_arc(cr_, x, y, 8, 0, 2 * pi);
    set_rgba(cr_, ink, .9 * alpha);
    cairo_fill(cr_);
    cairo_arc(cr_, x, y, 3.5, 0, 2 * pi);
    set_rgba(cr_, theme_.legend, alpha);
    cairo_fill(cr_);
}

// ---- screens

void Installer::paint(double now) {
    now_ = now;
    cr_ = cairo_create(canvas_.get());
    hits_.clear();
    cairo_set_source_surface(cr_, backdrop_.get(), 0, 0);
    cairo_paint(cr_);
    cairo_translate(cr_, offset_x_, offset_y_);
    if (screen_ == Screen::Home) {
        paint_home();
    } else {
        paint_setup();
    }
    cairo_destroy(cr_);
    cr_ = nullptr;
    cairo_surface_flush(canvas_.get());
}
void Installer::paint_home() {
    paint_keyboard(0);
    const bool loading = status_.task == Task::Releases && status_.state == State::Running;
    const bool list_failed = status_.task == Task::Releases && status_.state == State::Failed;
    const bool uninstalling = status_.task == Task::Uninstall && status_.state == State::Running;

    // The keyboard's name, the same in every language.
    block("Full Keyboard", {240, 432, 800, 56}, 40, theme_.legend, PANGO_WEIGHT_SEMIBOLD, true,
          PANGO_ALIGN_CENTER);
    // Under it: what is installed, in a chip, or what just happened.
    if (!home_message_.key.empty()) {
        text(localization_.text(home_message_), {200, 496, 880, 40}, 20, dim());
    } else if (!installed_.empty()) {
        const auto line = tr("installer.installed", {{"version", installed_}});
        const double w = std::min(760.0, text_width(line, 19) + 68);
        const Rect chip{(width - w) / 2, 496, w, 40};
        paint::rounded(cr_, chip, chip.height / 2);
        set_rgba(cr_, theme_.surface, .7);
        cairo_fill(cr_);
        cairo_arc(cr_, chip.x + 24, chip.y + chip.height / 2, 5, 0, 2 * pi);
        paint::source(cr_, paint::mix(theme_.latched, theme_.legend, .35));
        cairo_fill(cr_);
        text(line, {chip.x + 36, chip.y, chip.width - 48, chip.height}, 19,
             paint::mix(theme_.legend, theme_.surface, .15), PANGO_ALIGN_LEFT);
    }

    if (list_failed) {
        button({install_main.x, install_main.y, install_more.x + install_more.width - install_main.x,
                install_main.height},
               Action::Retry, 0, tr("installer.retry"), true, true, 26);
    } else {
        split_button(install_main, install_more, !releases_.empty() && !uninstalling);
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
    const Rect below{200, install_main.y + install_main.height + 10, 880, 36};
    if (loading) {
        // The spinner leads the line, which stays centred.
        const double w = std::min(below.width - 40, text_width(caption, 20));
        spinner(width / 2.0 - w / 2 - 18, below.y + below.height / 2, 9);
        text(caption, {below.x + 14, below.y, below.width, below.height}, 20, tone);
    } else {
        text(caption, below, 20, tone);
    }

    if (!installed_.empty()) {
        button({530, 706, 220, 56}, Action::Uninstall, 0, tr("installer.uninstall"), false,
               !uninstalling, 20); // Removing needs nothing from GitHub.
    }
    if (versions_open_) {
        paint_versions();
    }
    if (confirm_open_) {
        paint_confirm();
    }
}

void Installer::paint_versions() {
    constexpr double row = 60, pad = 8;
    // As many of the newest as fit above the button.
    const auto shown = std::min<std::size_t>(releases_.size(), 8);
    const double list_width = install_more.x + install_more.width - install_main.x;
    const double list_height = static_cast<double>(shown) * row + 2 * pad;
    const Rect list{install_main.x, install_main.y - 10 - list_height, list_width, list_height};
    paint::settings_card(cr_, theme_, list);
    hits_.push_back({list, Action::None, 0, false}); // presses between rows stay in the list
    for (std::size_t i = 0; i < shown; ++i) {
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
             {r.x + 16, r.y, r.width - 32, r.height}, 21,
             i == selected_ ? theme_.latched_legend : theme_.legend, PANGO_ALIGN_LEFT);
    }
}

void Installer::paint_confirm() {
    const double shown = ease_out((now_ - confirm_opened_) / .22);
    // Dims everything behind it and takes every press.
    cairo_set_source_rgba(cr_, 0, 0, 0, .6 * shown);
    cairo_paint(cr_);
    hits_.push_back({{0, 0, width, height}, Action::None, 0, false});
    const bool running = status_.task == Task::Uninstall && status_.state == State::Running;

    // The dialog grows with its text: title, explanation, the settings row
    // and the buttons.
    constexpr double dialog_width = 720, pad = 44, row = 64, buttons_height = 60;
    const double w = dialog_width - 2 * pad;
    const auto title = tr("installer.uninstall.title");
    const auto explanation = tr("installer.uninstall.text");
    const double title_height =
        block(title, {0, 0, w, 80}, 30, theme_.legend, PANGO_WEIGHT_SEMIBOLD, false);
    const double text_height = block(explanation, {0, 0, w, 110}, 21, dim(), PANGO_WEIGHT_NORMAL, false);
    const double dialog_height =
        pad + title_height + 14 + text_height + 32 + row + 36 + buttons_height + pad;
    const Rect dialog{(width - dialog_width) / 2, (height - dialog_height) / 2, dialog_width,
                      dialog_height};
    const double x = dialog.x + pad;

    cairo_push_group(cr_);
    cairo_translate(cr_, 0, 14 * (1 - shown));
    paint::settings_card(cr_, theme_, dialog);
    double y = dialog.y + pad;
    block(title, {x, y, w, 80}, 30, theme_.legend, PANGO_WEIGHT_SEMIBOLD);
    y += title_height + 14;
    block(explanation, {x, y, w, 110}, 21, dim());
    y += text_height + 32;
    // Keep my settings, as a Settings row: a line above it, the label and Yes/No.
    cairo_rectangle(cr_, x, y - 16, w, 1);
    set_rgba(cr_, theme_.legend, .07);
    cairo_fill(cr_);
    text(tr("installer.uninstall.keep"), {x, y, w - 260, row}, 22, theme_.legend, PANGO_ALIGN_LEFT);
    segments({x + w - 240, y, 240, row}, Action::KeepSettings, keep_settings_ ? 0 : 1);

    const double buttons = dialog.y + dialog.height - pad - buttons_height;
    const Rect confirm{x + w - 240, buttons, 240, buttons_height};
    button({confirm.x - 16 - 200, buttons, 200, buttons_height}, Action::CancelUninstall, 0,
           tr("installer.cancel"), false, !running);
    hits_.push_back({confirm, running ? Action::None : Action::ConfirmUninstall, 0, !running});
    if (running) {
        paint::cap(cr_, theme_, confirm, false, false, false);
        spinner(confirm.x + confirm.width / 2, confirm.y + confirm.height / 2, 13);
    } else {
        // Removing is the one action that undoes the keyboard: the error
        // colour, deep in the keys' own shading rather than bright.
        const bool hover = hovered(Action::ConfirmUninstall);
        paint::rounded(cr_, confirm, theme_.radius);
        auto* shade = cairo_pattern_create_linear(0, confirm.y, 0, confirm.y + confirm.height);
        const auto top = paint::mix(hover ? theme_.hover : theme_.top, theme_.error, .4);
        const auto bottom = paint::mix(theme_.bottom, theme_.error, .3);
        cairo_pattern_add_color_stop_rgb(shade, 0, top.r, top.g, top.b);
        cairo_pattern_add_color_stop_rgb(shade, 1, bottom.r, bottom.g, bottom.b);
        cairo_set_source(cr_, shade);
        cairo_fill(cr_);
        cairo_pattern_destroy(shade);
        text(tr("installer.uninstall"), confirm, 22, theme_.legend);
    }
    cairo_pop_group_to_source(cr_);
    cairo_paint_with_alpha(cr_, shown);
}

void Installer::paint_setup() {
    const double since = now_ - setup_started_;
    paint_keyboard(ease_out(since / slide_seconds));
    paint_progress();
    // The card rises into place as the keyboard slides away.
    const double entered = ease_out((since - .15) / .6);
    if (entered < 1) {
        cairo_push_group(cr_);
    }
    cairo_save(cr_);
    cairo_translate(cr_, 0, std::round(40 * (1 - entered)));
    cached(card_, {card.x - 24, card.y - 24, card.width + 48, card.height + 48},
           [&] { paint::settings_card(cr_, theme_, card); });
    // Pages slide through the card like a carousel, fading as they go:
    // Next moves them left, Back right.
    const double turned = ease_in_out((now_ - page_turned_) / page_seconds);
    cairo_save(cr_);
    paint::rounded(cr_, card, theme_.radius + 4);
    cairo_clip(cr_);
    if (turned < 1 && page_from_ != page_) {
        const double way = page_ > page_from_ ? 1 : -1, travel = card.width;
        live_ = false; // The leaving page takes no presses.
        cairo_push_group(cr_);
        paint_page(page_from_, -way * turned * travel);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, 1 - turned);
        live_ = true;
        cairo_push_group(cr_);
        paint_page(page_, way * (1 - turned) * travel);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, turned);
    } else {
        paint_page(page_, 0);
    }
    cairo_restore(cr_);
    paint_footer(turned);
    cairo_restore(cr_);
    if (entered < 1) {
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, entered);
    }
}

void Installer::paint_page(int page, double shift) {
    cairo_save(cr_);
    // Hits stay where the page comes to rest; only the drawing moves, by
    // whole pixels so text and the cached stage stay sharp.
    cairo_translate(cr_, std::round(shift), 0);
    if (page < wizard_pages()) {
        paint_wizard_page(page);
    } else {
        paint_finish_page();
    }
    cairo_restore(cr_);
}

void Installer::paint_progress() {
    const bool installing = status_.task == Task::Install;
    const bool running = installing && status_.state == State::Running;
    const bool failed = installing && status_.state == State::Failed;
    const std::string title = tr(install_done_    ? "installer.installed.title"
                                 : first_install_ ? "installer.installing"
                                                  : "installer.reinstalling",
                                 {{"version", installing_}});
    block(title, {card.x, 22, card.width, 50}, 30, theme_.legend, PANGO_WEIGHT_SEMIBOLD, true,
          PANGO_ALIGN_CENTER);

    const Rect bar{390, 86, 500, 14};
    paint::well(cr_, theme_, bar);
    if (shown_progress_ > 0) {
        const Rect fill{bar.x + 3, bar.y + 3, std::max(8.0, (bar.width - 6) * shown_progress_),
                        bar.height - 6};
        paint::latched(cr_, theme_, fill, fill.height / 2);
        if (running) {
            // A soft light runs along the fill while work goes on.
            cairo_save(cr_);
            paint::rounded(cr_, fill, fill.height / 2);
            cairo_clip(cr_);
            const double at = fill.x - 120 + std::fmod(now_ * .8, 1.0) * (fill.width + 240);
            auto* sheen = cairo_pattern_create_linear(at - 80, 0, at + 80, 0);
            cairo_pattern_add_color_stop_rgba(sheen, 0, 1, 1, 1, 0);
            cairo_pattern_add_color_stop_rgba(sheen, .5, 1, 1, 1, .22);
            cairo_pattern_add_color_stop_rgba(sheen, 1, 1, 1, 1, 0);
            cairo_set_source(cr_, sheen);
            cairo_paint(cr_);
            cairo_pattern_destroy(sheen);
            cairo_restore(cr_);
        }
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
        // The reason and Try again, centred together under the bar.
        constexpr double retry_width = 210, between = 24;
        const double reason = std::min(card.width - retry_width - between, text_width(line, 20) + 12);
        const double x = (width - reason - between - retry_width) / 2;
        text(line, {x, 106, reason, 68}, 20, tone, PANGO_ALIGN_CENTER, true);
        button({x + reason + between, 112, retry_width, 56}, Action::Retry, 0, tr("installer.retry"),
               false, true, 20);
    } else {
        text(line, {card.x, 110, card.width, 34}, 20, tone);
    }
}
void Installer::paint_wizard_page(int page) {
    const double t = std::max(0.0, now_ - (page == page_ ? page_turned_ : page_from_turned_));
    // A feature answered No plays on, dimmed. The numpad's picture shows
    // either answer instead.
    const double lit = page == 4 ? 1 : .3 + .7 * choice_shown(page);
    stage_plate(stage);
    cairo_save(cr_);
    paint::rounded(cr_, stage, theme_.radius + 4);
    cairo_clip(cr_);
    if (lit < 1) {
        cairo_push_group(cr_);
    }
    switch (page) {
    case 0:
        paint_replace_demo(stage, t);
        break;
    case 1:
        paint_swipe_demo(stage, t);
        break;
    case 2:
        paint_suggestions_demo(stage, t);
        break;
    case 3:
        paint_pull_demo(stage, t);
        break;
    case 4:
        paint_numpad_demo(stage, t);
        break;
    default:
        paint_stream_demo(stage, t);
        break;
    }
    if (lit < 1) {
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, lit);
    }
    cairo_restore(cr_);

    // The question, what it means and Yes/No, centred beside the stage.
    constexpr double choice_height = 62, choice_width = 300, under_title = 14, over_choice = 30;
    const auto title = tr(page_keys[page].first);
    const auto explanation = tr(page_keys[page].second);
    // Streaming support's page says why it stays at No when it has to.
    const auto blocked = page == pages - 1 ? streaming_blocked() : UiText{};
    const auto reason = blocked.key.empty() ? std::string{} : localization_.text(blocked);
    constexpr double over_reason = 12;
    const double reason_height = reason.empty()
                                     ? 0
                                     : over_reason + block(reason, {0, 0, column_width, 60}, 18, dim(),
                                                           PANGO_WEIGHT_NORMAL, false);
    const double title_height =
        block(title, {0, 0, column_width, 84}, 32, theme_.legend, PANGO_WEIGHT_SEMIBOLD, false);
    const double room =
        stage.height - title_height - under_title - over_choice - choice_height - reason_height;
    const double text_height =
        block(explanation, {0, 0, column_width, room}, 21, dim(), PANGO_WEIGHT_NORMAL, false);
    const double total =
        title_height + under_title + text_height + over_choice + choice_height + reason_height;
    double y = stage.y + std::max(0.0, (stage.height - total) * .45);
    block(title, {column_x, y, column_width, 84}, 32, theme_.legend, PANGO_WEIGHT_SEMIBOLD);
    y += title_height + under_title;
    block(explanation, {column_x, y, column_width, room}, 21, dim());
    y += text_height + over_choice;
    segments({column_x + 4, y, std::min(choice_width, column_width - 8), choice_height}, Action::Choice,
             choice(page) ? 0 : 1, reason.empty());
    if (!reason.empty()) {
        block(reason, {column_x, y + choice_height + over_reason, column_width, 60}, 18, dim());
    }
}
void Installer::paint_finish_page() {
    stage_plate(stage);
    // Restart SteamOS sits under the badge, which makes room for it.
    const bool restart = restart_offered();
    paint_finish_badge(stage.x + stage.width / 2, stage.y + stage.height / 2 - (restart ? 44 : 0), 84);
    if (restart) {
        const auto label = tr("installer.finish.restart");
        const double width = std::min(stage.width - 48, text_width(label, 21) + 84);
        const Rect row{stage.x + (stage.width - width) / 2, stage.y + stage.height - 116, width, 52};
        checkbox(row, Action::Restart, label, restart_, !finish_pressed_);
        block(tr("installer.finish.restart.note"),
              {stage.x + 24, row.y + row.height + 8, stage.width - 48, 48}, 16, dim(),
              PANGO_WEIGHT_NORMAL, true, PANGO_ALIGN_CENTER);
    }

    // With Restart SteamOS ticked, Finish restarts instead of opening the keyboard.
    const char* note = !first_install_           ? "installer.ready.reinstall"
                       : restart && restart_     ? "installer.ready.restart"
                       : choices_.replace_native ? "installer.ready.replace"
                                                 : "installer.ready.manual";
    const auto value = tr(note);
    const Color ink = paint::mix(theme_.legend, theme_.surface, .12);
    if (!first_install_) {
        const double h =
            block(value, {0, 0, column_width, stage.height}, 24, ink, PANGO_WEIGHT_NORMAL, false);
        block(value, {column_x, stage.y + (stage.height - h) / 2, column_width, stage.height}, 24, ink);
        return;
    }
    // On a first install, what Finish saves: each setting and its answer.
    // A long name takes two lines; all of them share the largest size at
    // which the list fits under the note.
    constexpr double gap = 20, chip_width = 96, chip_height = 36, row_pad = 14, pitch = 1.12;
    const double label_width = column_width - chip_width - 16;
    const double h = block(value, {0, 0, column_width, 130}, 22, ink, PANGO_WEIGHT_NORMAL, false);
    const Color label_ink = paint::mix(theme_.legend, theme_.surface, .2);
    std::array<double, pages> labels{};
    double size = 19, list_height = 0;
    for (;; size -= 1) {
        list_height = 0;
        for (std::size_t page = 0; page < static_cast<std::size_t>(wizard_pages()); ++page) {
            labels[page] = block(tr(summary_keys[page]), {0, 0, label_width, height}, size, label_ink,
                                 PANGO_WEIGHT_NORMAL, false, PANGO_ALIGN_LEFT, pitch);
            list_height += std::max(chip_height, labels[page]) + row_pad;
        }
        if (h + gap + list_height <= stage.height || size <= 16) {
            break;
        }
    }
    double y = stage.y + std::max(0.0, (stage.height - h - gap - list_height) * .45);
    block(value, {column_x, y, column_width, 130}, 22, ink);
    y += h + gap;
    const Color card_fill = paint::mix(theme_.surface, theme_.bottom, .45);
    for (int page = 0; page < wizard_pages(); ++page) {
        const double label = labels[static_cast<std::size_t>(page)];
        const double row = std::max(chip_height, label) + row_pad;
        cairo_rectangle(cr_, column_x + 4, y, column_width - 8, 1);
        set_rgba(cr_, theme_.legend, .07);
        cairo_fill(cr_);
        block(tr(summary_keys[page]), {column_x, y + (row - label) / 2, label_width, height}, size,
              label_ink, PANGO_WEIGHT_NORMAL, true, PANGO_ALIGN_LEFT, pitch);
        const bool yes = choice(page);
        const Rect chip{column_x + column_width - chip_width - 4, y + (row - chip_height) / 2,
                        chip_width, chip_height};
        if (yes) {
            paint::latched(cr_, theme_, chip, chip.height / 2);
        } else {
            paint::rounded(cr_, chip, chip.height / 2);
            paint::source(cr_, paint::mix(card_fill, theme_.surface, .7));
            cairo_fill(cr_);
        }
        text(tr(yes ? "installer.yes" : "installer.no"), chip, 18, yes ? theme_.latched_legend : dim());
        y += row;
    }
}

void Installer::paint_finish_badge(double x, double y, double radius) {
    const bool failed = status_.task == Task::Install && status_.state == State::Failed;
    cairo_new_path(cr_);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
    if (install_done_) {
        // The ring fills into a disc and a tick draws itself in.
        const double grown = ease_out((now_ - done_at_) / .35);
        const double drawn = ease_out((now_ - done_at_ - .15) / .4);
        cairo_arc(cr_, x, y, radius * (.92 + .08 * grown), 0, 2 * pi);
        cairo_push_group(cr_);
        paint::latched(cr_, theme_, {x - radius, y - radius, 2 * radius, 2 * radius}, radius);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, .35 + .65 * grown);
        cairo_new_path(cr_);
        const std::array<std::pair<double, double>, 3> tick{{{-.36, .02}, {-.1, .28}, {.4, -.26}}};
        const double first = std::hypot(tick[1].first - tick[0].first, tick[1].second - tick[0].second);
        const double second = std::hypot(tick[2].first - tick[1].first, tick[2].second - tick[1].second);
        const double along = drawn * (first + second);
        cairo_move_to(cr_, x + tick[0].first * radius, y + tick[0].second * radius);
        const double a = std::min(1.0, along / first);
        cairo_line_to(cr_, x + (tick[0].first + (tick[1].first - tick[0].first) * a) * radius,
                      y + (tick[0].second + (tick[1].second - tick[0].second) * a) * radius);
        if (along > first) {
            const double b = (along - first) / second;
            cairo_line_to(cr_, x + (tick[1].first + (tick[2].first - tick[1].first) * b) * radius,
                          y + (tick[1].second + (tick[2].second - tick[1].second) * b) * radius);
        }
        if (drawn > 0) {
            paint::source(cr_, theme_.latched_legend);
            cairo_set_line_width(cr_, radius * .13);
            cairo_stroke(cr_);
        }
        return;
    }
    // A recessed ring that fills as the install goes, the spinner within.
    cairo_arc(cr_, x, y, radius, 0, 2 * pi);
    set_rgba(cr_, theme_.surface, .9);
    cairo_set_line_width(cr_, 12);
    cairo_stroke(cr_);
    if (failed) {
        cairo_arc(cr_, x, y, radius, 0, 2 * pi);
        paint::source(cr_, paint::mix(theme_.surface, theme_.error, .5));
        cairo_set_line_width(cr_, 6);
        cairo_stroke(cr_);
        text("!", {x - radius, y - radius, 2 * radius, 2 * radius}, 64, theme_.error);
        return;
    }
    const double filled = 2 * pi * std::max(.02, shown_progress_);
    cairo_arc(cr_, x, y, radius, -pi / 2, -pi / 2 + filled);
    paint::source(cr_, theme_.latched);
    cairo_set_line_width(cr_, 8);
    cairo_stroke(cr_);
    // A light runs along the filled part while work goes on.
    const double head = -pi / 2 + filled * std::fmod(now_ * .7, 1.0);
    cairo_arc(cr_, x, y, radius, std::max(-pi / 2, head - .5), head);
    set_rgba(cr_, theme_.legend, .3);
    cairo_set_line_width(cr_, 8);
    cairo_stroke(cr_);
    // An arrow into a tray: it is being fetched.
    const double a = radius * .26;
    cairo_move_to(cr_, x, y - a * 1.2);
    cairo_line_to(cr_, x, y + a * .5);
    cairo_move_to(cr_, x - a * .7, y - a * .2);
    cairo_line_to(cr_, x, y + a * .5);
    cairo_line_to(cr_, x + a * .7, y - a * .2);
    cairo_move_to(cr_, x - a, y + a * 1.2);
    cairo_line_to(cr_, x + a, y + a * 1.2);
    set_rgba(cr_, theme_.legend, .55);
    cairo_set_line_width(cr_, 5);
    cairo_stroke(cr_);
}

void Installer::paint_footer(double turned) {
    const double left = card.x + card_pad, right = card.x + card.width - card_pad;
    const bool finish = page_ == wizard_pages();
    // Streaming support could not be set up: the choice is made here, without
    // going back to its page.
    const bool without = finish && streaming_failed();
    // Back returns through the wizard until Finish is pressed.
    if (without) {
        button({left, footer_y, 300, 60}, Action::FinishWithout, 0,
               tr("installer.finish.without-streaming"), false);
    } else if (first_install_ && page_ > 0 && !(finish && finish_pressed_)) {
        button({left, footer_y, 200, 60}, Action::Back, 0, tr("installer.back"), false);
    }
    if (!finish) {
        // Picking doesn't advance; Next does.
        button({right - 200, footer_y, 200, 60}, Action::Next, 0, tr("installer.next"), false);
    } else {
        const Rect r{right - 260, footer_y, 260, 60};
        const bool waiting = (status_.task == Task::Install || status_.task == Task::Finish) &&
                             status_.state == State::Running;
        const auto name = tr(without ? "installer.retry" : "installer.finish");
        button(r, Action::Finish, 0, waiting ? "" : name, true, install_done_ && !finish_pressed_, 24);
        if (waiting) {
            // The label, with a spinner before it while the work runs.
            const auto label = name;
            const double w = std::min(r.width - 70, text_width(label, 24));
            spinner(r.x + (r.width - w) / 2 - 8, r.y + r.height / 2, 10);
            text(label, {r.x + 22, r.y, r.width - 22, r.height}, 24, dim());
        }
    }
    if (!first_install_) {
        return;
    }
    // A dot for each page and Finish; the current one stretches into a bar
    // that travels with the turn.
    const double at = page_from_ + (page_ - page_from_) * std::clamp(turned, 0.0, 1.0);
    std::array<double, pages + 1> widths{};
    double total = 0;
    for (int i = 0; i <= wizard_pages(); ++i) {
        widths[i] = 10 + 22 * std::max(0.0, 1 - std::abs(at - i));
        total += widths[i] + (i ? 10 : 0);
    }
    const Color card_fill = paint::mix(theme_.surface, theme_.bottom, .45);
    double x = card.x + (card.width - total) / 2;
    for (int i = 0; i <= wizard_pages(); ++i) {
        const Rect dot{x, footer_y + 25, widths[i], 10};
        const double near = std::max(0.0, 1 - std::abs(at - i));
        const Color rest = i < at ? paint::mix(theme_.latched, card_fill, .45)
                                  : paint::mix(card_fill, theme_.legend, .14);
        paint::rounded(cr_, dot, 5);
        paint::source(cr_, paint::mix(rest, paint::mix(theme_.latched, theme_.legend, .2), near));
        cairo_fill(cr_);
        x += widths[i] + 10;
    }
}

// ---- the wizard's pictures

// An app's window with a text field: Full Keyboard rises under it, the laser
// types a few words into it and the keyboard goes again, as it does
// wherever Steam's would.
void Installer::paint_replace_demo(const Rect& s, double t) {
    constexpr double cycle = 5.8, typing = 1.5, step = .25;
    const double c = std::fmod(t, cycle);
    const Rect window{s.x + 50, s.y + 30, s.width - 100, 150};
    for (int i = 1; i <= 4; ++i) {
        paint::rounded(cr_, {window.x - i, window.y + i * 2, window.width + 2 * i, window.height},
                       theme_.radius + 2 + i);
        cairo_set_source_rgba(cr_, 0, 0, 0, .08);
        cairo_fill(cr_);
    }
    paint::rounded(cr_, window, theme_.radius + 2);
    paint::source(cr_, paint::mix(theme_.surface, theme_.bottom, .45));
    cairo_fill(cr_);
    // Its title strip, and a line standing in for its title.
    cairo_save(cr_);
    paint::rounded(cr_, window, theme_.radius + 2);
    cairo_clip(cr_);
    cairo_rectangle(cr_, window.x, window.y, window.width, 34);
    paint::source(cr_, theme_.middle);
    cairo_fill(cr_);
    cairo_restore(cr_);
    paint::rounded(cr_, {window.x + 18, window.y + 13, 96, 8}, 4);
    set_rgba(cr_, theme_.legend, .2);
    cairo_fill(cr_);

    // Words typed into the field: bars of text that grow as keys go down.
    const Rect input{window.x + 20, window.y + 56, window.width - 40, 68};
    paint::well(cr_, theme_, input);
    constexpr std::array<double, 5> words{52, 34, 66, 28, 44};
    const double typed = phase(c, typing, 8 * step);
    double x = input.x + 20, left = typed * (52 + 34 + 66 + 28 + 44);
    for (const double word : words) {
        const double w = std::min(word, left);
        if (w <= 0) {
            break;
        }
        paint::rounded(cr_, {x, input.y + input.height / 2 - 6, w, 12}, 6);
        set_rgba(cr_, theme_.legend, .55);
        cairo_fill(cr_);
        left -= word;
        x += w + (left > 0 ? 10 : 4);
    }
    if (c > .3 && c < 4.6 && std::fmod(now_, 1.0) < .6) {
        cairo_rectangle(cr_, x, input.y + input.height / 2 - 15, 2.5, 30);
        set_rgba(cr_, theme_.legend, .85);
        cairo_fill(cr_);
    }

    // The keyboard: up when the field is chosen, down when it's done.
    const double rise = c < 4.4 ? ease_out(phase(c, .5, .6)) : 1 - ease_in_out(phase(c, 4.4, .6));
    const double rest = s.y + s.height - 30 - case_bottom * mini_scale, hidden = s.y + s.height + 16;
    const double kx = s.x + (s.width - panel_width * mini_scale) / 2;
    const double ky = hidden + (rest - hidden) * rise;
    small_picture(mini_.get(), mini_scale, kx, ky, 1);
    // The laser goes from key to key: h e l l o, space, a t.
    constexpr std::array<std::pair<double, int>, 8> keys{
        {{6.25, 2}, {3.5, 1}, {9.25, 2}, {9.25, 2}, {9.5, 1}, {7, 4}, {1.25, 2}, {5.5, 1}}};
    const auto at = [&](std::size_t i) {
        const auto& [column, row] = keys[std::min(i, keys.size() - 1)];
        return std::pair{kx + (keys_left + column * key_pitch + key_pitch / 2) * mini_scale,
                         ky + row_middle[static_cast<std::size_t>(row)] * mini_scale};
    };
    const double hop = std::max(0.0, (c - typing) / step);
    const auto from = at(static_cast<std::size_t>(hop)), to = at(static_cast<std::size_t>(hop) + 1);
    const double f = ease_in_out((hop - std::floor(hop)) / .6);
    laser(s, from.first + (to.first - from.first) * f, from.second + (to.second - from.second) * f,
          phase(c, typing - .4, .3) * (1 - phase(c, typing + 8 * step + .2, .3)));
}

// Three rows of letters with a trail tracing one word after another, as a
// swipe does on the keyboard: the keys it crosses light up, and the word
// lands in the field when the trail ends.
void Installer::paint_swipe_demo(const Rect& s, double t) {
    constexpr double field_height = 68, gap = 50, size = 34, pitch = 39, pad = 15;
    const double top = s.y + (s.height - field_height - gap - 2 * pitch - size) / 2;
    const Rect input{s.x + 32, top, s.width - 64, field_height};
    const Letters letters({s.x, top + field_height + gap, s.width, 2 * pitch + size}, size, pitch);
    case_plate({letters.left - pad, letters.top - pad, 9.5 * pitch + size + 2 * pad,
                2 * pitch + size + 2 * pad});
    // Which word, and how far along it: trace, hold, fade.
    std::array<double, demo_words.size()> lengths{};
    double cycle = 0;
    for (std::size_t i = 0; i < demo_words.size(); ++i) {
        lengths[i] =
            static_cast<double>(std::strlen(demo_words[i]) - 1) * demo_step + demo_hold + demo_fade;
        cycle += lengths[i];
    }
    t = std::fmod(t, cycle);
    std::size_t word = 0;
    while (word + 1 < demo_words.size() && t >= lengths[word]) {
        t -= lengths[word++];
    }
    const std::string typed = demo_words[word];
    std::vector<std::pair<double, double>> points;
    for (char letter : typed) {
        const auto r = letters.of(letter);
        points.emplace_back(r.x + r.width / 2, r.y + r.height / 2);
    }
    const double last = static_cast<double>(points.size()) - 1;
    const double trace = last * demo_step;
    const double progress = std::min(t / demo_step, last);
    const double fade =
        t > trace + demo_hold ? std::max(0.0, 1 - (t - trace - demo_hold) / demo_fade) : 1;
    const double shown = t >= trace ? fade * std::min(1.0, (t - trace) / .15) : 0;

    field(input, typed, 26, shown, true);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; letter_rows[row][column]; ++column) {
            const char letter = letter_rows[row][column];
            const auto r = letters.at(row, column);
            // Latched once the trail has reached it, as a held key is.
            const auto found = typed.find(letter);
            const bool lit = found != std::string::npos && static_cast<double>(found) <= progress + .01;
            key(r, lit && fade > .5);
            text(std::string(1, letter), {r.x, r.y - 2, r.width, r.height}, 18,
                 lit && fade > .5 ? theme_.latched_legend : theme_.legend);
        }
    }
    // The trail as the keyboard draws it (panel.cpp), at this demo's scale:
    // the whole path as a quiet thin line, and over its last moments a comet,
    // thinner and fainter with age.
    constexpr double path_width = 2, comet_width = 4.5, glow_width = 11, comet_seconds = .42;
    const auto at = [&](double along) {
        const auto i = std::min(static_cast<std::size_t>(along), points.size() - 2);
        const double f = along - static_cast<double>(i);
        return std::pair{points[i].first + (points[i + 1].first - points[i].first) * f,
                         points[i].second + (points[i + 1].second - points[i].second) * f};
    };
    const Color path_ink = paint::mix(theme_.legend, theme_.latched, .22);
    const Color glow = paint::mix(theme_.latched, theme_.legend, .35);
    cairo_move_to(cr_, points[0].first, points[0].second);
    for (std::size_t i = 1; i <= static_cast<std::size_t>(progress); ++i) {
        cairo_line_to(cr_, points[i].first, points[i].second);
    }
    const auto head = at(progress);
    cairo_line_to(cr_, head.first, head.second);
    set_rgba(cr_, path_ink, .30 * fade);
    cairo_set_line_width(cr_, path_width);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr_);
    // The comet: short pieces of the path, each as old as the moment the
    // head passed it; after the trace it burns out where it stopped.
    // Pieces end on a fixed grid of tenths of the way between two letters,
    // so every letter is a piece's end and the comet turns sharply there
    // instead of cutting the corner by a different amount every frame.
    const double from = std::max(0.0, progress - comet_seconds / demo_step);
    std::vector<std::pair<std::pair<double, double>, double>> pieces; // end point, life left
    double start = from;
    for (int k = static_cast<int>(std::floor(from * 10)) + 1; start < progress - 1e-6; ++k) {
        const double end = std::min(k / 10.0, progress);
        const double age = t - std::min(end, last) * demo_step;
        const double left = std::clamp(1 - age / comet_seconds, 0.0, 1.0);
        pieces.emplace_back(at(end), left * left * (3 - 2 * left));
        start = end;
    }
    if (!pieces.empty()) {
        // Clipped to where it can reach, so the groups stay small.
        const auto tail = at(from);
        double x0 = tail.first, y0 = tail.second, x1 = tail.first, y1 = tail.second;
        for (const auto& [p, eased] : pieces) {
            x0 = std::min(x0, p.first);
            y0 = std::min(y0, p.second);
            x1 = std::max(x1, p.first);
            y1 = std::max(y1, p.second);
        }
        const double reach = glow_width / 2 + 2;
        cairo_save(cr_);
        cairo_rectangle(cr_, std::floor(x0 - reach), std::floor(y0 - reach),
                        std::ceil(x1 - x0 + 2 * reach) + 1, std::ceil(y1 - y0 + 2 * reach) + 1);
        cairo_clip(cr_);
        // Each piece replaces what lies under it, so joins don't darken.
        const auto strokes = [&](double width, double strength, Color colour) {
            cairo_push_group(cr_);
            cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
            auto previous = tail;
            for (const auto& [end, eased] : pieces) {
                cairo_move_to(cr_, previous.first, previous.second);
                cairo_line_to(cr_, end.first, end.second);
                set_rgba(cr_, colour, eased);
                cairo_set_line_width(cr_, width * (.25 + .75 * eased));
                cairo_stroke(cr_);
                previous = end;
            }
            cairo_set_operator(cr_, CAIRO_OPERATOR_OVER);
            cairo_pop_group_to_source(cr_);
            cairo_paint_with_alpha(cr_, strength * fade);
        };
        strokes(glow_width, .16, glow);
        strokes(comet_width, .9, path_ink);
        cairo_restore(cr_);
    }
    // The laser, as SteamVR draws it over the keyboard: on the comet's head
    // while tracing, fading soon after the word ends, as in the other demos.
    const double pointer =
        std::min(1.0, t / .15) * (t < trace ? 1 : std::max(0.0, 1 - (t - trace) / .3));
    laser(s, head.first, head.second, pointer);
}

// A word typed letter by letter: three ways to finish it show under the
// field, and the middle one completes it.
void Installer::paint_suggestions_demo(const Rect& s, double t) {
    constexpr double cycle = 5.2, first = .5, every = .55, chosen_at = first + 2 * every + .8;
    constexpr double field_height = 68, gap = 50, chip_height = 46, size = 34, pitch = 39, pad = 15;
    const double c = std::fmod(t, cycle);
    const std::string word = "keyboard";
    const double keys_height = chip_height + 14 + 2 * pitch + size;
    const double top = s.y + (s.height - field_height - gap - keys_height) / 2;
    const Rect input{s.x + 32, top, s.width - 64, field_height};
    // The case: suggestions along its top, as on the keyboard, and the letters.
    const Letters letters({s.x, top + field_height + gap + chip_height + 14, s.width, 2 * pitch + size},
                          size, pitch);
    const Rect plate{letters.left - pad, top + field_height + gap - pad, 9.5 * pitch + size + 2 * pad,
                     keys_height + 2 * pad};
    case_plate(plate);
    // The first three letters are typed; the suggestion does the rest.
    std::size_t typed = 0;
    while (typed < 3 && c >= first + static_cast<double>(typed) * every) {
        ++typed;
    }
    const bool completed = c >= chosen_at + .15;
    field(input, completed ? word : word.substr(0, typed), 26, 1 - phase(c, 4.3, .4), true);

    if (typed > 0) {
        static const std::array<std::array<const char*, 3>, 3> candidates{{
            {"key", "keep", "know"},
            {"keep", "key", "kept"},
            {"keys", "keyboard", "key"},
        }};
        const auto& row = candidates[typed - 1];
        const double w = (plate.width - 2 * pad - 2 * 8) / 3;
        cairo_push_group(cr_);
        for (int i = 0; i < 3; ++i) {
            const Rect chip{plate.x + pad + i * (w + 8), plate.y + pad, w, chip_height};
            const bool pressed = typed == 3 && i == 1 && c >= chosen_at && c < chosen_at + .4;
            key(chip, pressed, theme_.radius);
            text(row[i], {chip.x, chip.y - 2, chip.width, chip.height}, 18,
                 pressed ? theme_.latched_legend : theme_.legend);
        }
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, phase(c, first, .2) * (1 - phase(c, chosen_at + .4, .3)));
    }

    // The keys, each lit for a moment as it's typed.
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; letter_rows[row][column]; ++column) {
            const char letter = letter_rows[row][column];
            const auto r = letters.at(row, column);
            bool lit = false;
            for (std::size_t i = 0; i < 3; ++i) {
                const double down = first + static_cast<double>(i) * every;
                lit = lit || (word[i] == letter && c >= down && c < down + .25);
            }
            key(r, lit);
            text(std::string(1, letter), {r.x, r.y - 2, r.width, r.height}, 18,
                 lit ? theme_.latched_legend : theme_.legend);
        }
    }
}

// The laser presses 2 and pulls down. As on the keyboard, the key stays
// where it is: its Shift character slides from the corner into the middle
// and grows as the key's own shrinks away below; pulled far enough the key
// fills, and releasing it types @.
void Installer::paint_pull_demo(const Rect& s, double t) {
    constexpr double cycle = 4.6, pressed_at = .6, released_at = 2.0;
    constexpr double field_height = 68, gap = 50, size = 120, between = 14, pad = 16;
    constexpr double large = 44, small = 26;
    const double c = std::fmod(t, cycle);
    const bool held = c >= pressed_at && c < released_at;
    const double pull = held ? ease_in_out(phase(c, pressed_at + .2, .55)) : 0;
    const double top = s.y + (s.height - field_height - gap - size) / 2;
    field({s.x + 32, top, s.width - 64, field_height}, c >= released_at && c < 4.1 ? "name@" : "name",
          26, 1, true);

    const double cx = s.x + s.width / 2, keys_y = top + field_height + gap;
    case_plate({cx - 1.5 * size - between - pad, keys_y - pad, 3 * size + 2 * between + 2 * pad,
                size + 2 * pad});
    // A key's two legends at rest: the Shift character small in its upper
    // part, its own below.
    const auto corner = [](const Rect& face) {
        return Rect{face.x, face.y + 8, face.width, face.height * .4};
    };
    const auto middle = [](const Rect& face) {
        return Rect{face.x, face.y + face.height * .4, face.width, face.height * .55};
    };
    const std::array<std::array<const char*, 2>, 2> sides{{{"!", "1"}, {"#", "3"}}};
    for (int i = 0; i < 2; ++i) {
        const Rect r{cx + (i ? 1 : -1) * (size + between) - size / 2, keys_y, size, size};
        cairo_push_group(cr_);
        key(r, false, 10);
        text(sides[i][0], corner(r), small, theme_.legend);
        text(sides[i][1], middle(r), large * .85, theme_.legend);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, .4);
    }
    // A pressed key goes down by its travel and no further.
    const Rect face{cx - size / 2, keys_y + (held ? 4 : 0), size, size};
    const bool filled = pull >= 1;
    key(face, filled, 10);
    const Color ink = filled ? theme_.latched_legend : theme_.legend;
    const auto between_rects = [&](const Rect& a, const Rect& b) {
        return Rect{a.x + (b.x - a.x) * pull, a.y + (b.y - a.y) * pull,
                    a.width + (b.width - a.width) * pull, a.height + (b.height - a.height) * pull};
    };
    const Rect below{face.x, face.y + face.height * .62, face.width, face.height * .38};
    cairo_push_group(cr_);
    text("2", between_rects(middle(face), below), large * .85 + (small - large * .85) * pull, ink);
    cairo_pop_group_to_source(cr_);
    cairo_paint_with_alpha(cr_, 1 - pull);
    text("@", between_rects(corner(face), {face.x, face.y, face.width, face.height * .94}),
         small + (large - small) * pull, ink);

    // The laser: onto the key, down with the pull, and away after release.
    const double arrive = ease_out(phase(c, 0, .5)), leave = ease_in_out(phase(c, 2.6, .5));
    const double away = 1 - arrive + leave;
    laser(s, cx + 34 + 120 * away, keys_y + size * .34 + 90 * away + size * .46 * pull,
          std::min(1.0, 1.4 - away));
}

// The keyboard's right end, close up: the numpad fades and the case narrows
// to tenkeyless as the choice changes, while Insert to Page Down and the
// arrows stay. With the numpad, the laser types digits on it.
void Installer::paint_numpad_demo(const Rect& s, double t) {
    constexpr double scale = half_scale, margin = 30;
    const double numpad = choice_shown(4);
    // The case's right edge keeps its margin with the numpad and comes half
    // the numpad's width inward without it; the rest runs off to the left.
    const double narrowed = (panel_width - tenkeyless_width) * (1 - numpad) * scale;
    const double x = s.x + s.width - margin - panel_width * scale + narrowed / 2;
    const double y = s.y + (s.height - case_bottom * scale) / 2;
    cairo_push_group(cr_);
    small_picture(half_.get(), scale, x, y, numpad);
    cairo_pop_group_to_source(cr_);
    auto* run_off = cairo_pattern_create_linear(s.x, 0, s.x + 150, 0);
    cairo_pattern_add_color_stop_rgba(run_off, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(run_off, 1, 0, 0, 0, 1);
    cairo_mask(cr_, run_off);
    cairo_pattern_destroy(run_off);

    // 7 8 9, 4 5 6 and 1 2 3: the numpad's columns are 63 pixels apart from
    // 1362, its rows the main block's from the second down.
    constexpr double cycle = 5.2, typing = .8, step = .34;
    constexpr std::size_t digits = 9;
    const double c = std::fmod(std::max(0.0, std::min(t, now_ - choice_changed_ - .4)), cycle);
    const auto at = [&](std::size_t i) {
        i = std::min(i, digits - 1);
        return std::pair{x + (1362 + 63.0 * static_cast<double>(i % 3)) * scale,
                         y + row_middle[1 + i / 3] * scale};
    };
    const double hop = std::max(0.0, (c - typing) / step);
    const auto from = at(static_cast<std::size_t>(hop)), to = at(static_cast<std::size_t>(hop) + 1);
    const double f = ease_in_out((hop - std::floor(hop)) / .6);
    laser(s, from.first + (to.first - from.first) * f, from.second + (to.second - from.second) * f,
          numpad < 1 ? 0 : phase(c, typing - .4, .3) * (1 - phase(c, typing + digits * step, .3)));
}

// A game or application streamed from the PC with a text line open: the
// laser types on the keyboard below and every script arrives, not only
// Latin letters.
void Installer::paint_stream_demo(const Rect& s, double t) {
    // What is typed, one character at a time: each prefix of it.
    static const std::vector<std::string> typed = [] {
        const std::string line = "GG! Привет こんにちは";
        std::vector<std::string> prefixes{""};
        for (std::size_t i = 0; i < line.size();) {
            ++i;
            while (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
                ++i; // the rest of a UTF-8 character
            }
            prefixes.push_back(line.substr(0, i));
        }
        return prefixes;
    }();
    constexpr double typing = 1.1, step = .22, hold = 1.3;
    const auto count = static_cast<double>(typed.size() - 1);
    const double done = typing + count * step, cycle = done + hold + 1.2;
    const double c = std::fmod(t, cycle);

    // The stream's window, drawn as page 1 draws an app's.
    const Rect window{s.x + 40, s.y + 26, s.width - 80, 168};
    for (int i = 1; i <= 4; ++i) {
        paint::rounded(cr_, {window.x - i, window.y + i * 2, window.width + 2 * i, window.height},
                       theme_.radius + 2 + i);
        cairo_set_source_rgba(cr_, 0, 0, 0, .08);
        cairo_fill(cr_);
    }
    paint::rounded(cr_, window, theme_.radius + 2);
    paint::source(cr_, paint::mix(theme_.surface, theme_.bottom, .45));
    cairo_fill(cr_);
    cairo_save(cr_);
    paint::rounded(cr_, window, theme_.radius + 2);
    cairo_clip(cr_);
    cairo_rectangle(cr_, window.x, window.y, window.width, 34);
    paint::source(cr_, theme_.middle);
    cairo_fill(cr_);
    cairo_restore(cr_);
    // What the window shows, the same in every language.
    text("PC", {window.x + 18, window.y, window.width - 36, 34}, 15,
         paint::mix(theme_.legend, theme_.middle, .45), PANGO_ALIGN_LEFT);
    // Two lines of the game's chat above the line being typed.
    for (int i = 0; i < 2; ++i) {
        paint::rounded(cr_, {window.x + 24, window.y + 50 + i * 16, i ? 150.0 : 210.0, 8}, 4);
        set_rgba(cr_, theme_.legend, .14);
        cairo_fill(cr_);
    }
    const double progress = (c - typing) / step;
    const auto shown =
        c < typing ? std::size_t{0} : std::min(typed.size() - 1, static_cast<std::size_t>(progress) + 1);
    field({window.x + 20, window.y + 88, window.width - 40, 60}, typed[shown], 23,
          1 - phase(c, done + hold, .5), c > .4 && c < done + hold);

    // The keyboard: up while the line is typed, down when it is sent.
    const double rise =
        c < done + hold ? ease_out(phase(c, .3, .6)) : 1 - ease_in_out(phase(c, done + hold, .6));
    const double rest = s.y + s.height - 30 - case_bottom * mini_scale, hidden = s.y + s.height + 16;
    const double kx = s.x + (s.width - panel_width * mini_scale) / 2;
    const double ky = hidden + (rest - hidden) * rise;
    small_picture(mini_.get(), mini_scale, kx, ky, 1);
    // The laser walks the keys, one for each character.
    constexpr std::array<std::pair<double, int>, 7> keys{
        {{4.5, 2}, {7.5, 1}, {1.5, 3}, {6, 2}, {9, 1}, {3, 1}, {5.5, 3}}};
    const auto at = [&](std::size_t i) {
        const auto& [column, row] = keys[i % keys.size()];
        return std::pair{kx + (keys_left + column * key_pitch + key_pitch / 2) * mini_scale,
                         ky + row_middle[static_cast<std::size_t>(row)] * mini_scale};
    };
    const double hop = std::clamp(progress, 0.0, count - 1);
    const auto from = at(static_cast<std::size_t>(hop)), to = at(static_cast<std::size_t>(hop) + 1);
    const double f = ease_in_out((hop - std::floor(hop)) / .6);
    laser(s, from.first + (to.first - from.first) * f, from.second + (to.second - from.second) * f,
          phase(c, typing - .4, .3) * (1 - phase(c, done + .2, .3)));
}

std::optional<Rect> Installer::control(Action action, int index) const {
    for (const auto& hit : hits_) {
        if (hit.action == action && hit.index == index) {
            return Rect{hit.bounds.x + offset_x_, hit.bounds.y + offset_y_, hit.bounds.width,
                        hit.bounds.height};
        }
    }
    return std::nullopt;
}

bool Installer::enabled(Action action, int index) const {
    return std::any_of(hits_.begin(), hits_.end(), [&](const Hit& hit) {
        return hit.action == action && hit.index == index && hit.enabled;
    });
}

} // namespace framekeyboard::installer
