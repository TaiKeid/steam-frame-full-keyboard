#pragma once

#include "app_names.hpp"
#include "cjk.hpp"
#include "clipboard.hpp"
#include "delivered_context.hpp"
#include "dictation.hpp"
#include "dictation_editor.hpp"
#include "dictation_mode.hpp"
#include "dictation_send.hpp"
#include "emoji_glyphs.hpp"
#include "feedback.hpp"
#include "instance.hpp"
#include "japanese.hpp"
#include "japanese_worker.hpp"
#include "localization.hpp"
#include "next_words.hpp"
#include "output.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include "settings_ui.hpp"
#include "settings_writer.hpp"
#include "shell_protocol.hpp"
#include "shell_sink.hpp"
#include "shortcut_editor.hpp"
#include "shortcut_launch.hpp"
#include "shortcut_runner.hpp"
#include "shortcuts.hpp"
#include "stream_app.hpp"
#include "stream_support.hpp"
#include "swipe.hpp"
#include "theme_previews.hpp"
#include "word_suggestions.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace framekeyboard {
class NativeReplacement;
class Updates;
enum class Mode {
    Help,
    Preview,
    Vr,
    Render,
    RenderSettings,
    RenderEmoji,
    RenderDictation,
    Check,
    Describe
};
enum class InputBackend { None, Ei, Uinput };
// How long typing pauses before live candidates come: 0, after every kana,
// as on a phone; 0.15 s, after a pause, as before, to compare the two. An
// out-of-date list is never dimmed either way.
constexpr double default_live_candidate_pause = 0;
struct Options {
    fs::path data_dir, config_dir, ei_socket, text_socket;
    Mode mode{Mode::Help};
    InputBackend input{InputBackend::None};
    std::string output, target_language;
    double duration{};
    bool start_enabled{false};
    bool autostart{};
    // Started by the update helper, after an update the user started.
    bool update_launch{};
    // Launched by a shell with `--hosted FD` (docs/integration.md): the
    // inherited control connection, the process's number on it, and the
    // failure code of an update that rolled back to this process.
    int hosted_fd{-1};
    std::uint64_t hosted_session{1};
    std::string last_update;
    // The text sink between the output queue and the input backend; set
    // only for a hosted launch.
    ShellRouter* shell_router{};
    // Tests replace the optional swipe engine module; empty uses the installed one.
    SwipeEncoderFactory swipe_encoder;
    // Tests capture typed text here instead of the output queue; null uses the queue.
    TextOutput* text_output{};
    // Tests replace Japanese conversion; empty uses Mozc where installed,
    // the system's Anthy otherwise and meanwhile.
    JapaneseComposer::Factory japanese_converter;
    // The Mozc server to convert Japanese with, Anthy only meanwhile; the
    // keyboard and the simulator give the installed one (mozc_engine()).
    // Empty is Anthy alone.
    fs::path mozc_server;
    // For testing conversion, as in the simulator: a conversion waits this
    // long for a Mozc server that isn't answering before Anthy converts it.
    // The keyboard waits for nothing.
    std::chrono::milliseconds mozc_patience{};
    // Tests: after each request to the conversion worker, wait for its
    // answer, as when conversion ran on the VR thread. The keyboard never
    // waits for the worker.
    bool wait_for_japanese{};
    double live_candidate_pause = default_live_candidate_pause;
    // Debug mode: Settings has a Debug card, whose Mark Anthy suggestions
    // gives candidates Anthy converted an orange border. Only builds with
    // FRAMEKEYBOARD_DEBUG_MODE have the card; the others ignore this.
    bool debug{};
    // The system's interface language: main reads Steam's own setting and
    // LANG. Left empty, as in tests and previews, the interface is English.
    std::function<SystemLanguage()> system_language;
    // Streaming support's state on this system, read when Settings opens.
    // Left empty, as in tests and previews, the setting cannot be turned on.
    std::function<StreamSupportStatus()> stream_support;
    // Where Anthy keeps what it learns. Empty is the user's home with the
    // default config folder, and any other config folder itself.
    fs::path anthy_home;
    // Tests replace the dictation helper; empty uses the installed one.
    fs::path dictation_helper;
    // Where on-device models are kept; empty is ~/.local/share/framekeyboard/models.
    fs::path models_dir;
    // Where the theme picker keeps its previews; empty is
    // ~/.cache/framekeyboard/theme-previews ($XDG_CACHE_HOME if set).
    fs::path theme_previews_dir;
    std::vector<std::string> dictation_arguments;
    // Tests replace the clipboard writer; null writes the session clipboards.
    Clipboard* clipboard{};
    // Where the shortcut bar's app names come from (app_names.hpp): main
    // gives the headset's folders, the simulator its own names. Left empty,
    // as in tests and previews, no file is read and the names are those the
    // identities give.
    AppNameSource app_names;
    // Where the editor's Run list reads the scripts folder: tests and the
    // tour give their own names; empty makes and lists the real folder.
    ScriptListSource script_list;
    // The kept record of app names found (app_names.hpp, R362): main gives
    // app_name_record_path(); empty, as in tests and previews, keeps none.
    fs::path app_name_record;
    // Where a stream's app comes from (stream_app.hpp): main reads Steam's
    // streaming client; left empty, every stream is the PC's.
    StreamClientSource stream_clients;
};

// What a Haptic effect or Haptic volume click plays, by the pending choices.
struct SoundPreview {
    bool press{}, release{};
    std::string effect;
    int volume{};
};
class App {
  public:
    App(const Options& options, KeySink& sink, SettingsWriter::Write write_preferences = {},
        Updates* updates = nullptr, SettingsWriter::Write reconcile_preferences = {});
    // Saves emoji typed and a tone chosen while the picker is still open, as
    // when SteamVR quits.
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    PanelView view() const { return build_view(true); }
    // Everything but the emoji picker's cells and tabs, for hit tests and
    // input areas: building those for every laser event is the costly part.
    PanelView input_view() const { return build_view(false); }
    // Only what key placement depends on, for converting pointer coordinates
    // without building controls or copying the swipe trail.
    PanelView geometry() const;
    // True only for a newly accepted keyboard key, so VR can provide haptics.
    bool down(unsigned pointer, double x, double y, double now);
    // True when an unpressed pointer enters a different keyboard key.
    bool move(unsigned pointer, double x, double y);
    // True when this pointer releases a captured keyboard key, even outside it.
    bool up(unsigned pointer, double x, double y);
    // Includes a swipe, which owns its pointer after giving up the key press,
    // and an emoji press, drag or tone popup, so lost tracking still cancels them.
    bool pointer_pressed(unsigned pointer) const {
        return keyboard_.pointer_pressed(pointer) || pressed_controls_.contains(pointer) ||
               (bar_press_ && bar_press_->pointer == pointer) || settings_ui_.pressed(pointer) ||
               (swipe_track_ && swipe_track_->pointer == pointer) || emoji_.pressed(pointer) ||
               candidates_.pressed(pointer) ||
               (languages_ && languages_->drag && languages_->drag->pointer == pointer);
    }
    // Swipe typing is on and its word list has loaded; until then letters type on press.
    bool swipe_ready() { return swipe_enabled() && swipe_->ready(); }
    // What a pointer leaving the keyboard discards (cancel_pointer) because
    // the field's cursor may move: a decoding word, suggestions, the next
    // word's space, what the keys typed while completions show, acknowledged
    // or pending table text, and after a Japanese commit ↶, the follow-up
    // offers and the pair the next commit may learn (a pair only while
    // nothing was sent since). While any of it exists a laser that points
    // away with its trigger at rest is watched rather than canceled, so only
    // a pull, the other hand or hiding ends it: under Background Disabled the
    // laser crosses the open strip between the candidate band and the keys.
    // This also enables the existing off-key/caret watch for Chinese, without
    // a new timer. Pointing away at rest keeps adjacency until an observed edit.
    bool holds_typing_state() const {
        return table_context_.active() || swipe_waiting_ || !swipe_candidates_.empty() || swipe_space_ ||
               ((!typing_suggestions_.empty() || word_open()) && suggesting()) || reopen_ || follow_up_ ||
               (last_commit_ && last_commit_->queued == output_.queued());
    }
    // clicked: the pointer pulled its trigger off the keyboard, so it may
    // have moved the field's cursor: the word ends whichever hand typed it.
    void cancel_pointer(unsigned pointer, bool clicked = false);
    // The pointer holds a repeating key, such as Backspace, in the keyboard,
    // the dictation editor or the address popup. It stays held after the
    // laser leaves the panel, until the trigger's release
    // (docs/architecture.md, Input and focus).
    // Tests: whether the conversion worker's thread was ever started.
    bool japanese_worker_started() const { return japanese_worker_.started(); }
    bool holds_repeat(unsigned pointer) const {
        // A held While held macro keeps running off the panel, as a key repeats.
        if (bar_press_ && bar_press_->pointer == pointer && bar_press_->held &&
            shortcut_runner_.running()) {
            return true;
        }
        return keyboard_.repeats(pointer) || (editor_repeat_ && editor_repeat_->pointer == pointer) ||
               (address_repeat_ && address_repeat_->pointer == pointer) ||
               (reading_repeat_ && reading_repeat_->pointer == pointer);
    }
    void cancel(bool discard_composition = true);
    // As cancel(), for a change of pointer, view or placement that keeps the
    // target: presses end, but text and keys already accepted still arrive,
    // and a held tap or flick types as if let go.
    void release_input(bool discard_composition = true);
    // Sleep ends active delivery and capture, but keeps editor/configuration
    // drafts. No Send or queued key may continue merely because input resumes.
    // `asleep`: the system slept; otherwise the headset was taken off.
    void suspend_input(bool asleep = true);
    bool tick(double now);
    // Whether the image changed and needs uploading.
    bool paint(double now);
    void show_settings();
    void back();
    bool scroll(unsigned pointer, double dx, double dy);
    bool pinned() const { return settings_.pinned; }
    // Keep open holds the panel after its replacement request ends; only a
    // resident replacement has requests that close it.
    bool keep_open() const { return settings_.keep_open && resident_replacement() && !hosted(); }
    // A Keep open panel without a request types into the focused window, and
    // that window is out of sight: the Keep open icon turns amber.
    void set_hidden_focus(bool hidden) {
        dirty |= hidden != hidden_focus_;
        hidden_focus_ = hidden;
    }
    // The title of that hidden window, for the focus tooltip; empty when unknown.
    void set_hidden_window(std::string title) {
        dirty |= title != hidden_window_;
        hidden_window_ = std::move(title);
    }
    // Debugging only (vr.cpp's show_input_target): shown on the status line
    // when no notice is.
    void set_debug_status(std::string text) {
        dirty |= text != debug_status_;
        debug_status_ = std::move(text);
    }
    // Stay open over apps: the panel stays open while the dashboard is closed,
    // and VR reads the controllers itself so the focused app keeps its input.
    // Either the user's toggle or dictation's own, which keeps the panel open
    // while it listens with the dashboard closed.
    bool passthrough() const { return passthrough_ || dictation_passthrough_; }
    // Turning passthrough off also ends dictation's.
    void set_passthrough(bool on);
    // VR reports the dashboard each frame before deciding whether the panel
    // may show. Closing it while dictation listens turns on dictation's
    // passthrough, which lasts until dictation mode ends
    // (docs/dictation.md, Dashboard, passthrough and focus). True when it did.
    bool dashboard_visibility(bool visible);
    // Dictation has something going (listening, phrases on the way, sending
    // or text), so replacement mode keeps the panel after Steam's field closes.
    bool dictation_holds_panel() const;
    // While Settings is open the chosen theme shows at once; Back returns to
    // the saved one, since it never changed.
    const Theme& shown_theme() const {
        return profiles_.themes.at(settings_open_ ? pending_.theme : settings_.active.theme);
    }
    // Shown without a Steam field to type into: the editor keeps working,
    // but nothing is sent until the user picks a field.
    void set_field_needed(bool needed);
    // Steam's dashboard has Gamescope's keys (DashboardFocus) and no Steam
    // field is ours: whatever app Gamescope falls back to is not the one the
    // user is looking at, so nothing is typed until a field or window is chosen.
    // `known`: the probe has answered. Before it has, typing waits as for the
    // dashboard, but the shortcut bar keeps the focus it had (R343).
    void set_dashboard_focus(bool focused, bool known = true);
    // The panel left the view; listening pauses. A pause in interaction,
    // such as Steam handing over a new field, does not.
    void panel_hidden();
    // Nobody is wearing the headset, or it slept: the microphone closes at
    // once, with the message `key` names shown. Editor text stays; Type as I
    // speak stops.
    void stop_listening(std::string_view key);
    // A prepared update discards dictation and stops its helper, which has
    // exited by the time App is destroyed and the handoff starts.
    void end_dictation_for_update();
    bool dictation_helper_running() const { return dictation_ && dictation_->running(); }
    bool use_drag_to_move() const { return settings_.use_drag_to_move; }
    bool curved_keyboard() const { return settings_.curved_keyboard; }
    bool auto_leveling() const { return settings_.auto_leveling; }
    // Standalone-only: a hosted keyboard never replaces Steam's or starts at boot.
    bool replace_native_keyboard() const { return settings_.replace_native_keyboard && !hosted(); }
    bool start_with_steamos() const { return replace_native_keyboard() && settings_.start_with_steamos; }
    bool streaming_support() const { return replace_native_keyboard() && settings_.streaming_support; }
    // A hosted keyboard starts hidden too: only its shell shows it.
    // An update with replacement on starts hidden too, and then shows itself
    // (reveal_after_update), even if Steam's handed-back keyboard cannot be adopted.
    bool background_start() const { return hosted() || options_.autostart || reveal_after_update(); }
    bool reveal_after_update() const {
        return !hosted() && options_.update_launch && settings_.replace_native_keyboard;
    }
    bool replacement_ready() const;
    // Fixed diagnostic text for why replacement_ready() is false.
    const char* replacement_block() const;
    bool take_native_fallback();
    // X hides instead of exiting: with replacement, or under a shell.
    bool resident_replacement() const {
        return options_.mode == Mode::Vr && (settings_.replace_native_keyboard || hosted());
    }
    void close_panel();
    // X: discards dictation without asking, then closes the panel.
    void close_with_x();
    bool take_dismissal();
    void set_replacement_available(bool available, UiText reason = {}) {
        if (available) {
            reason = {};
        }
        dirty |= replacement_available_ != available || replacement_unavailable_reason_ != reason;
        replacement_available_ = available;
        replacement_unavailable_reason_ = std::move(reason);
    }
    void set_native_session(bool open) {
        dirty |= native_session_ != open;
        native_session_ = open;
    }
    // The Steam keyboard request the panel serves, 0 for none. Type as I
    // speak stops when it changes: its phrases belong to the old field.
    void set_native_request(std::uint64_t request);
    bool allows_movement(MoveSource source) const {
        // Corner resizers and curve bars work in either movement mode; only
        // the pin locks them, or a shell's lock or forwarded pointer. A bend
        // changes the panel's shape, so a shell's resize lock holds it too.
        const bool shape = source == MoveSource::Resize || source == MoveSource::Curve;
        if (shape ? locked_resize() : locked_move()) {
            return false;
        }
        if (source == MoveSource::Curve && !curved_keyboard()) {
            return false;
        }
        return !pinned() && !forwarded() &&
               (shape || use_drag_to_move() == (source == MoveSource::Grip));
    }
    void summon();
    // A corner resize holds the same input gate as movement.
    void set_dragging(bool dragging, bool resizing = false);
    bool dragging() const { return dragging_; }
    // The microphone is open for dictation or Type as I speak.
    bool listening() const { return capturing_; }
    // Pointers whose hold or double tap just locked a modifier, for VR's
    // haptic pulse in that hand (KeyboardState::take_new_locks).
    std::vector<unsigned> take_new_locks() { return keyboard_.take_new_locks(); }
    // Settings previews use the pending choice, but mute and listening always
    // keep the stream off, just as they do for ordinary keys.
    bool haptic_sounds() const {
        const auto& sounds = settings_open_ ? pending_haptic_sounds_ : settings_.haptic_sounds;
        return !capturing_ && settings_.haptic_sounds_switch && sounds != "off";
    }
    const std::string& haptic_effect() const { return settings_.haptic_effect; }
    int haptic_volume() const { return settings_.haptic_volume; }
    // Key vibration (config.hpp): VR reads this to keep or drop its pulses.
    bool vibration() const { return settings_.vibration; }
    std::optional<SoundPreview> take_sound_preview() { return std::exchange(sound_preview_, {}); }
    // Whether this press or release pops. Silent while the microphone is
    // listening: the speakers sit close enough for it to hear them.
    bool pops(KeyFeedback kind) const {
        const auto& sounds = settings_.haptic_sounds;
        return !capturing_ && settings_.haptic_sounds_switch && kind != KeyFeedback::Hover &&
               (sounds == "both" || sounds == (kind == KeyFeedback::Press ? "press" : "release"));
    }
    // Hiding the VR context releases keys before closing the input gate.
    void set_interaction_active(bool active);
    // Shows a message on the status line in the interface language; an
    // empty key clears it.
    void report_status(const UiText& message);
    void apply(Selection selection);
    // Reload immediately when storage is idle, otherwise coalesce the request
    // and complete it from tick() after the newest pending snapshot is saved.
    void reload();
    void flush_settings();
    // Only explicit profile/storage work gets extra time. The deadline stays
    // fixed until a complete VR frame, even across nested or repeated work.
    double slow_work_deadline() const { return slow_work_deadline_.load(); }
    void frame_completed() { slow_work_deadline_.store(0); }
    const fs::path& config_dir() const { return options_.config_dir; }
    // The interface language's tag (docs/localization-design.md).
    const std::string& interface_language() const { return localization_.tag(); }
    bool quitting() const { return quit_; }
    // ↶ could still bring back the phrase committed last (the simulator's trace).
    bool reopen_pending() const { return reopen_.has_value(); }
    bool updating() const;
    bool take_recenter();
    bool emoji_open() const { return emoji_open_; }
    bool dictation_open() const { return dictation_open_; }
    // Something under a hand ray may scroll with the stick: the emoji picker,
    // dictation, the shortcut bar (its menu included), the shortcut editor,
    // or a candidate row with more than fits, with or without the bar
    // (R345). Outside the dashboard SteamVR sends no scroll events, so VR
    // reads the stick only then (R338); scroll_room() says what is under the ray.
    bool stick_scrolls() const {
        return emoji_open_ || dictation_open_ || bar_shown() || shortcut_editor_.has_value() ||
               (candidates_.shown() && candidates_.scroll_limit() > 0);
    }
    // For image exports: dictation mode with sample text, whatever the settings.
    void show_dictation();
    // Which ways the stick could scroll what this laser points at: the emoji
    // grid, overflowing tabs, or an overflowing Settings card or list. None
    // elsewhere. SteamVR's dashboard ticks the controller for every scroll
    // step it sends, so VR asks for steps only where they would move something.
    std::optional<ScrollRoom> scroll_room(unsigned pointer) const;
    // For image exports: open the picker, and whether its images are all drawn.
    void show_emoji() { open_emoji(); }
    bool emoji_settled() const {
        return emoji_glyphs_ && emoji_checked_ && std::get<0>(emoji_requested_) > 0 &&
               emoji_glyphs_->idle();
    }
    bool dirty{true};
    PanelRenderer renderer;
    const Selection& selection() const { return settings_.active; }

    // Shell integration (docs/integration.md), in app_host.cpp. A hosted
    // keyboard takes these from its shell; a standalone one never does.
    bool hosted() const { return options_.mode == Mode::Vr && options_.hosted_fd >= 0; }
    const Options& options() const { return options_; }
    bool dictation_model_ready() const { return model_ready(settings_.dictation_device_model); }
    Updates* updates() const { return updates_; }
    struct HostHints {
        std::string purpose{"text"}, enter{"enter"}, autocapitalize{"sentences"}, language;
    };
    struct HostChrome {
        bool close{true}, settings{true}, move{true}, resize{true}, favorites{true};
    };
    struct HostEvent {
        std::string name;
        JsonPtr fields{nullptr, json_object_put};
        bool telemetry{}; // the latest value replaces an unsent one
    };
    // Checks hints without applying them: an error code and the field.
    std::pair<std::string, std::string> check_hints(const HostHints& hints) const;
    // Ends everything addressed to the old context (held keys, queued and
    // retried output, composition, swipe results, Type as I speak phrases),
    // then the new context and its hints take effect together.
    void set_context(std::optional<std::string> context, const HostHints& hints);
    void set_context(std::optional<std::string> context) {
        set_context(std::move(context), HostHints{});
    }
    const std::optional<std::string>& context() const { return host_.context; }
    bool sink_on() const { return options_.shell_router && options_.shell_router->on(); }
    // Cancels through the old destination first. `client` is shown in the
    // case while the sink is on.
    void set_sink(bool on, const std::string& client = {});
    // The shown view: keyboard, emoji, dictation or settings.
    std::string view_name() const;
    // Empty on success, else an error code: `composing` when the change
    // would commit unfinished input, `unavailable` when the view cannot open.
    std::string show_view(const std::string& view);
    void set_chrome(const HostChrome& chrome);
    void set_locks(bool move, bool resize);
    bool locked_move() const { return host_.lock_move || !host_.chrome.move; }
    bool locked_resize() const { return host_.lock_resize || !host_.chrome.resize; }
    // A shell forwards its own pointer; native sources and the move line are off.
    void set_forwarded(bool on);
    bool forwarded() const { return host_.forwarded; }
    JsonPtr host_settings() const;
    // All or nothing: an error code and the key, or empty on success.
    std::pair<std::string, std::string> host_settings_set(json_object* values, std::int64_t request);
    std::string host_select(const std::string& layout, const std::string& language,
                            const std::string& favorite, std::int64_t request);
    JsonPtr host_profiles() const;
    std::string host_theme_select(const std::string& id, std::int64_t request);
    // An error code and the field, or empty on success.
    std::pair<std::string, std::string> host_theme_set(const std::string& json);
    void host_theme_reset();
    JsonPtr host_theme() const;
    // The shell's theme as it sent it, for previews; empty without one.
    const std::string& host_theme_json() const { return host_.theme_json; }
    JsonPtr host_dictation() const;
    // dictation.start, dictation.stop, dictation.test_server, dictation.model.
    std::pair<std::string, JsonPtr> host_dictation_request(const std::string& op, json_object* body);
    JsonPtr host_state() const;
    // Nothing unfinished that an update could lose (docs/integration.md, Idle).
    bool idle_for_update() const;
    // A hosted update is prepared and waits for the shell's handover.
    bool update_waiting() const { return hosted() && updating() && !quit_; }
    // Activation: unsent dictation is dropped, never committed, then the loop ends.
    void activate_update();
    // Settings → Quit, `quit-v1` and the shell's connection closing.
    void request_quit() { quit_ = true; }
    std::vector<HostEvent> take_host_events() { return std::exchange(host_.events, {}); }
    // X in hosted mode: the shell decides whether to show the panel again.
    bool take_user_hide() { return std::exchange(host_.user_hide, false); }

  private:
    std::vector<Control> controls() const;
    std::string game_sound_message() const;
    void preview_sound();
    PanelView build_view(bool picker) const;
    // Hidden by the setting, unless a number hint shows it for the context.
    bool numpad_hidden() const { return settings_.hide_numpad && !host_.numpad_shown; }
    // The case's opacity for the saved Background (PanelView::case_opacity).
    double case_opacity() const {
        return settings_.background_surface == "visible" ? 1
               : settings_.background_surface == "semi-transparent"
                   ? semi_transparent_case_opacity(shown_theme())
                   : 0;
    }
    // A launch that cannot type: the desktop preview, or `--input none`
    // without the shell's text sink.
    bool preview_input() const { return options_.input == InputBackend::None && !sink_on(); }
    // Nothing leaves the keyboard, but the editor still takes text: it is local.
    bool preview_output() const { return preview_input() && !dictation_open_ && !field_typing(); }
    // Text goes to a local editor, not the target: dictation's, or a
    // shortcut editor field typed on the keyboard.
    bool local_text() const { return dictation_open_ || field_typing(); }
    // What the context's purpose rules out (docs/integration.md, Hints).
    bool hint_private() const {
        return host_.context && (host_.hints.purpose == "password" || host_.hints.purpose == "pin");
    }
    bool hint_no_autospace() const {
        return host_.context && (host_.hints.purpose == "email" || host_.hints.purpose == "url" ||
                                 host_.hints.purpose == "terminal");
    }
    bool hint_terminal() const { return host_.context && host_.hints.purpose == "terminal"; }
    bool hint_no_suggestions() const { return hint_private() || hint_terminal(); }
    // The Cut/Copy/Paste keys the target takes (terminal_keys.hpp): a
    // terminal the sink detected, else the shell's terminal hint. The text
    // sink keeps the keyboard's own Ctrl+X/C/V key IDs.
    EditKeys edit_keys() const {
        return sink_on() ? EditKeys::Standard : combine_edit_keys(output_.edit_keys(), hint_terminal());
    }
    // The kind the keys were last drawn for (tick() redraws on a change).
    EditKeys shown_edit_keys_{EditKeys::Standard};
    // The letter a chord presses: the target keymap's in Unicode mode.
    int chord_code(const EditChord& chord);
    // Paste's keys in press order, for the keyboard's own paste paths;
    // empty when the target has no paste key, as in xterm.
    std::vector<int> paste_keys();
    // Applies the context's autocapitalize hint to dictated or swiped text.
    std::string hint_case(std::string text) const;
    void host_event(const std::string& name, JsonPtr fields, bool telemetry = false);
    // Reports what a change of settings changed, with its source.
    void host_settings_changed(const Settings& before, const char* source);
    void route_desktop_text();
    void host_tick(double now);
    // Restores the selection a context's language hint replaced, unless the
    // user chose another meanwhile.
    void end_language_hint();
    // The hinted layout and language are still the active ones.
    bool language_hint_in_place() const;
    // What a save writes: a hint still in place is the field's, not the
    // user's, so the selection before it is saved instead. A selection the
    // user chose over the hint ends the hint.
    Selection persisted_selection();
    // The shell's theme, kept under this ID while it is active; never saved.
    static constexpr const char* shell_theme_id = "shell";
    struct HostState {
        std::optional<std::string> context;
        HostHints hints;
        // A hinted language replaced this selection until the context ends.
        std::optional<Selection> before_language;
        Selection hinted;               // the layout and language the hint chose
        std::optional<bool> num_before; // Num Lock before a number hint
        bool numpad_shown{};
        HostChrome chrome;
        bool lock_move{}, lock_resize{}, forwarded{};
        bool user_hide{};
        std::optional<Theme> theme; // the shell's own theme, never saved
        std::string theme_json;     // as the shell sent it
        std::string theme_before;   // the installed theme it replaced
        std::string sink_client;    // shown while the sink is on
        bool test_by_shell{};       // the Test in flight was the shell's
        std::string test_reported;
        double level_at{};
        std::vector<HostEvent> events;
        // Protocol requests whose settings writes are not yet reported. A
        // save result arrives only once nothing newer is pending, so it
        // covers every write queued before it, whatever their ids.
        std::vector<std::int64_t> unsaved_requests;
        // The last reported dictation state, so only changes are sent.
        std::string dictation_state;
        int dictation_level{-1};
        std::string dictation_error;
        std::uint64_t download_bytes{};
        bool download_reported{};
    } host_;
    void update_settings_ui();
    // `keep_reading`: the selection's language and layout are the ones
    // typing already, and an unfinished reading stays (Apply in Settings).
    void activate(Selection selection, bool keep_reading = false);
    void save();
    void update_save_status();
    void allow_slow_work();
    void rebuild_cycle_entries();
    void action(const ControlAction& action);
    // Saves the pending Settings and closes them, as Apply does.
    void apply_settings();
    void refresh_typing();
    bool japanese_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_japanese();
    void convert_japanese();
    // Conversion on the worker (docs/architecture.md, Japanese conversion).
    // A step of the composer's runs here when it converts nothing, as
    // cycling candidates does; one that converts goes to the worker, on a
    // copy of the composer as it was, and Japanese input waits for its
    // answer. `then` gets the step's result, here or once answered.
    void japanese_step(JapaneseWorker::Step step, std::function<void(bool)> then = {});
    // A copy of the composer for the worker, with what it reads of App's
    // own: the learning permission now, and a snapshot of the choices.
    JapaneseComposer japanese_copy();
    void learn_japanese();
    void drop_live_requests();
    // Whether every request to the worker has its answer taken, so the
    // composer's record of what was last converted is the worker's.
    bool japanese_settled() const { return japanese_worker_.outstanding() == 0; }
    // With Options::wait_for_japanese, waits for the worker's answers.
    void settle_japanese();
    // Japanese input waits, in the order it came, while a step waits for
    // its answer, or until every answer is in for one that reads what was
    // last converted: 変換 after a live request, as typed this quickly.
    // Typing alone never waits.
    struct JapaneseWait {
        std::uint64_t id{}; // 0: every answer.
        std::uint64_t edits{};
        std::function<void(bool)> then;
        bool convert{}; // The step is 変換.
    };
    std::optional<JapaneseWait> japanese_wait_;
    struct HeldInput {
        enum class Kind { Key, Pick, Down, Move, Up, Cancel, Control } kind{};
        Key key;
        std::set<int> modifiers;
        int index{};
        std::vector<std::string> row; // A pick's candidates, as tapped.
        ControlAction control;        // A toolbar button's, as pressed.
        unsigned pointer{};
        bool clicked{}; // A Cancel's: cancel_pointer(pointer, clicked).
        double x{}, y{}, now{};
        static HeldInput pointer_event(Kind kind, unsigned pointer, double x = 0, double y = 0,
                                       double now = 0) {
            HeldInput input;
            input.kind = kind;
            input.pointer = pointer;
            input.x = x;
            input.y = y;
            input.now = now;
            return input;
        }
        static HeldInput key_press(const Key& key, const std::set<int>& modifiers) {
            HeldInput input;
            input.key = key;
            input.modifiers = modifiers;
            return input;
        }
    };
    std::deque<HeldInput> held_input_;
    // Pointers pressed on keys while input waits, until their release.
    std::set<unsigned> held_pointers_;
    bool replaying_input_{};
    bool japanese_holding() const { return japanese_wait_ || !held_input_.empty(); }
    // Whether 変換 waits for its answer with nothing pressed after it. ⌫,
    // Esc and ↶ then leave it at once, as on a phone, and its answer is
    // given up; after other keys they wait their turn.
    bool conversion_pending_last() const;
    void leave_pending_conversion();
    // Holds what came while input waits; a held action replayed that has
    // to wait again goes back to the front.
    void hold_input(HeldInput input);
    // Whether a press waits, held: one on a key while input waits.
    bool hold_press(unsigned pointer, double x, double y, double now);
    void replay_input();
    void drop_held_input(bool discard_composition, bool cancel_delivery);
    // The candidates shown for Japanese, as a held pick checks them.
    std::vector<std::string> japanese_row() const;
    // The choices the worker's copies read, made again after each change.
    std::shared_ptr<const NextWords> choices_snapshot_, completions_snapshot_;
    // What live candidates were last asked of the worker for.
    std::string live_asked_;
    std::uint64_t live_asked_clears_{};
    bool japanese_lists_preloaded_{};
    bool japanese() const;
    bool cjk_key(const Key& key, bool execute, const std::set<int>& mods);
    // Two-set Hangul letters, which send each syllable as the next one starts.
    bool hangul_key(const Key& key, const std::set<int>& mods);
    // The Chinese punctuation a key types (Language::punctuation), or null:
    // also for the key's plain character after a digit, as in 3.5 and 10:30.
    const std::vector<std::string>* chinese_punctuation(const Key& key, const std::set<int>& mods) const;
    // The key types Chinese punctuation, which goes to the target as text.
    bool punctuation_key(const Key& key, const std::set<int>& mods) const;
    // `turn_key` names the key and its Shift state in punctuation_turns_.
    void type_chinese_punctuation(const std::string& turn_key, const std::vector<std::string>& texts);
    // - and = page the candidates, five at a time.
    void page_cjk(int direction);
    // Zhuyin's cursor, choosing and one-step Esc (McBopomofo); false for keys
    // that type or send as usual.
    bool zhuyin_key(const std::string& code, bool execute, const std::set<int>& mods);
    // Zhuyin punctuation into the reading at the cursor, to convert with it.
    bool insert_punctuation(const Key& key, const std::set<int>& mods, const std::string& text);
    bool latin_toggle(const Key& key) const;
    // What a local IME press tells the keyboard state: 한/영 stands in right
    // Alt's place but must not latch as a modifier.
    int ime_press_code(const Key& key) const;
    void commit_cjk();
    void send_finished_syllables();
    void choose_cjk(int index);
    // Either composer has a reading in progress.
    bool composing_text() const;
    // Commits any reading through text_output_; false when it could not.
    bool commit_composition();
    // A commit that could not go out: the target, or in dictation mode the full editor.
    void commit_failed();
    bool unicode_mode() const;
    bool external_jis() const;
    bool text_key(const Key& key, const std::set<int>& mods) const;
    // Swipe typing. Decoding runs on SwipeService's worker; App alone sends text.
    bool swipe_supported(const std::string& language) const;
    // The same answer without reading the disk on every frame.
    bool swipe_known(const std::string& language) const;
    // Settings' switch reaches every language of the cycle, not only the main one.
    bool pending_can_swipe() const;
    // Typing suggestions (docs/configuration.md, Typing suggestions): completions of the word
    // being typed, for the languages swipe typing is offered in.
    bool suggestions_supported(const std::string& language) const;
    bool pending_can_suggest() const;
    // Whether completions show now: on in Settings, for this language, and
    // not in a password, PIN or terminal field, dictation or a composer.
    bool suggesting() const;
    // The language's word list, loaded on a thread of its own on first need.
    void load_word_list(double now);
    // Whether a lexicon's language cases I as Turkish does.
    bool turkic_lexicon(const std::string& lexicon) const;
    LearnedWords& learned_words(const std::string& lexicon);
    // Follows the word being typed: text this keyboard typed, typed or
    // swiped, a Backspace, and anything that ends it or makes it unknown.
    void track_typed(const std::string& text);
    void track_backspace();
    // The word before the cursor, as the keys typed it; empty after a space.
    std::string current_word() const;
    // Whether one is being typed: what the keys typed ends in a letter.
    bool word_open() const;
    // The word ended with a space, or a pick: it is learned. Enter, Tab and
    // digits end it unlearned, as a password ends, and punctuation waits for
    // the space after it.
    void end_word(bool learn);
    // The current word without a closing quote.
    std::string finished_word() const;
    // A message about a moment, such as asking to wait for typing to finish:
    // it goes by itself a little after the output is out, or when a key is
    // pressed (tick_passing_status).
    void show_passing_status(const std::string& message);
    void clear_passing_status();
    void tick_passing_status();
    std::optional<std::pair<std::string, double>> passing_status_; // Its text, and when shown.
    // Learns `word`, which ended at `end` in typed_text_, unless it already
    // counted there.
    void learn_word(const std::string& word, std::size_t end);
    // The cursor may have moved: typing goes on at a word's start.
    void lose_word();
    void update_typing_suggestions();
    void choose_typing_suggestion(int index);
    // A swipe alternative chosen in the swiped word's place.
    void swap_swiped(const std::string& word);
    // The Clear learned words popup: a row for each language with something
    // learned, by name, all off at first.
    struct LearnedRow {
        std::string id; // A word list's ID, or "japanese" for its conversions.
        std::string title, note;
        bool on{};
    };
    std::vector<LearnedRow> learned_rows_;
    std::vector<LearnedRow> learned_rows();
    bool anything_learned();
    void clear_learned(const std::vector<LearnedRow>& rows);
    // A word list's language as people say it: German for every German
    // keyboard, English (US) and English (UK) apart.
    std::string word_list_name(const std::string& lexicon) const;
    Rect learned_popup_bounds() const;
    Rect learned_row_bounds(std::size_t row) const;
    mutable std::map<std::string, bool> swipe_support_, suggestion_support_;
    // Whether the main language or an additional one can swipe.
    bool swipe_in_cycle_{};
    bool swipe_enabled() const { return swipe_ && swipe_keyboard_ && !hint_private(); }
    void prepare_swipe();
    // Whether a press on a swipe letter starts a swipe now.
    bool swipe_starts(const std::set<int>& modifiers) const;
    // Whether a press of this key can start a swipe.
    bool swipes(const Key& key, const std::set<int>& modifiers) const;
    // Pull for Shift (Settings): a key that types a character types it on
    // release instead, never repeating, and pulled down by flick_distance
    // its Shift character. Space, the numpad
    // and keys that edit or move keep acting on press.
    struct KeyFlickTrack {
        unsigned pointer{};
        std::string key;
        double y{};               // Press point in layout coordinates.
        std::set<int> modifiers;  // At the press, for the tap.
        xkb_keysym_t symbol{};    // The tap's, before down() consumes a latched Shift.
        bool shifts{}, flicked{}; // A Shift character to flick to; pulled down far enough.
        double pull{};            // How far toward a flick, 0 to 1, for the legend.
        // The tap composes and the pull types, or the other way round: the
        // target's admission waits for the release (R143).
        bool admit_on_release{};
    };
    std::optional<KeyFlickTrack> key_flick_;
    bool key_flick_key(const Key& key, const std::set<int>& modifiers);
    // A JIS kana key typing its kana: not in Latin mode, where it types its
    // letter or symbol as on any keyboard.
    bool kana_key(const Key& key) const;
    // The key has a Shift character of its own to flick to: one its legend
    // shows, or a letter's capital while swipe doesn't take the letter.
    bool key_flick_shifts(const Key& key) const;
    // The key's Shift character as its legend shows it, for the flick.
    std::string key_flick_legend(const Key& key) const;
    bool key_flick_down(unsigned pointer, const Key& key, double x, double y, double now,
                        const std::set<int>& modifiers);
    // True when the pull crosses into a flick, for VR's haptic tick.
    bool key_flick_move(double x, double y);
    void finish_key_flick();
    // The key only composes with these modifiers: nothing reaches the target
    // yet. Chinese punctuation is text.
    bool key_flick_composes(const Key& key, const std::set<int>& modifiers);
    // Says why text went nowhere: a full editor or a missing target.
    void text_refused();
    // With `gap` the press is between keys, in the letter's reach: until it
    // moves it shows nothing and takes nothing, and a tap types nothing.
    bool swipe_down(unsigned pointer, const Key& key, double x, double y, double now,
                    const std::set<int>& mods, bool gap = false);
    // Such a press becomes a swipe, as one on its letter would. False if refused.
    bool begin_gap_swipe();
    void swipe_move(double x, double y);
    bool swipe_up(unsigned pointer, double x, double y);
    // A pending word must reach the target before any later key.
    void finish_swipe(bool wait);
    void apply_swipe(const SwipeService::Result& result);
    void choose_swipe(int index);
    void forget_failed_swipe();
    void discard_swipe();
    // 12-key flick input. A press only shows the key; release types the kana
    // for the flick direction, or the next toggle kana for a repeated tap.
    enum class FlickDirection { Tap, Left, Up, Right, Down };
    struct FlickTrack {
        unsigned pointer{};
        std::string key, slot;
        double x{}, y{}, pressed{}; // Press point in layout coordinates and press time.
        FlickDirection direction{FlickDirection::Tap};
    };
    // Japanese flick modes: kana composes; English shows the layout's
    // QWERTY page, and digits turn the grid into a number pad.
    enum class FlickMode { Kana, Latin, Digits };
    FlickMode flick_mode() const;
    // The flick keys' table for the mode.
    const std::map<std::string, FlickKey>& flick_keys() const;
    // The active layout, or its English page in English mode.
    const Layout& shown_layout() const;
    bool band_more() const;
    // The shown Japanese candidates came from Anthy while Mozc couldn't, and
    // the Debug card's Mark Anthy suggestions marks them.
    bool candidates_fell_back() const;
    // The layout has the 「あA1」 key, which replaces the toolbar's A / あ.
    bool has_mode_key() const;
    // The reading stays open across the modes, as on a phone.
    void set_flick_mode(FlickMode mode);
    // ↶, as on Gboard: the last kana steps back through its key's taps,
    // conversion returns to the reading, and with neither the phrase just
    // committed comes back as a reading.
    void undo_japanese(double now);
    // Another pointer than this one holds ↶, which acts on release.
    bool undo_held(unsigned pointer) const;
    void reopen_phrase();
    void finish_reopen();
    void expire_reopen(double now);
    // Candidates: Japanese for the converting segment or live for the reading
    // (docs/japanese.md), Chinese for the reading; in the toolbar, the flick
    // band or over the dictation editor's foot.
    std::optional<Rect> candidate_area(const PanelView& band) const;
    // A composing language's reading and candidates show over the main
    // keyboard (PanelView::ime_band).
    // The shortcut editor keeps it out of sight, as Settings does (R333).
    bool ime_band_shown() const {
        return (japanese() || cjk_) && !emoji_open_ && !dictation_open_ && !shortcut_editor_;
    }
    // The views' ime_room: kept while the band isn't shown, outside the emoji
    // picker and dictation mode, whose layouts already take the room.
    bool ime_room_kept() const { return !japanese() && !cjk_ && !emoji_open_ && !dictation_open_; }
    // The views' reading_room, from the main layout whichever layout a view
    // shows, so the emoji picker and dictation mode keep its case.
    double reading_room() const;
    // geometry() with the controls, which place the reading and candidates
    // of full layouts.
    PanelView band_geometry() const;
    // Where ∨ goes while the candidates have more than their row shows.
    std::optional<Rect> candidate_more_;
    // Kana compose: on the kana page, or the flick digits page, whose digits
    // and symbols join the reading as on a phone (3, じに → 3時に), or the
    // English page while a reading is open, whose letters join it as typed.
    bool japanese_typing() const;
    // Builds the strip from the composer or the follow-ups, every tick: a
    // tick with the same candidates and no frame since the last build, which
    // a change to the layout, panel or toolbar would need, does nothing.
    void sync_candidates();
    // Places `texts` in the band, with ∨ and the open list.
    void place_candidates(std::vector<std::string> texts, int selected);
    struct CandidateSync {
        int source{};                   // What texts came from: none, CJK, 変換, live, follow-ups.
        std::vector<std::string> texts; // As given, before placing.
        int selected{};
        bool list{}, shown{};
        std::uint64_t frames{}; // frames_ at the build.
        bool operator==(const CandidateSync&) const = default;
    };
    std::optional<CandidateSync> candidate_sync_;
    // Frames painted with `dirty` set: something on the panel changed.
    std::uint64_t frames_{};
    // Live candidates after a pause in typing.
    void tick_japanese(double now);
    void pick_candidate(int index);
    // A pick from live candidates for an earlier reading (split_live()).
    void pick_earlier_candidate(int index);
    bool flick_down(unsigned pointer, const Key& key, double x, double y, double now);
    void flick_move(double x, double y);
    bool flick_up(unsigned pointer, double x, double y);
    // Types the held flick now, as its release would.
    void finish_flick();
    // What releasing now would type, and whether it replaces the last toggle kana.
    std::string flick_text(const FlickTrack& track, bool* cycles = nullptr) const;
    void flick_type(const std::string& text);
    int native_code(const Key& key, const std::set<int>& mods);
    // Emoji picker. It opens over the keyboard's own case; its keys type as usual.
    bool emoji_available() const;
    bool emoji_key(const std::string& id) const;
    void open_emoji();
    void close_emoji();
    void update_emoji(double now);
    void update_emoji_content();
    void apply_emoji_event(const EmojiPicker::Event& event);
    bool type_emoji(const std::string& text);
    void save_emoji();
    void update_output_status();
    bool retry_output();
    Updates* updates_{};
    Options options_;
    Profiles profiles_;
    // Interface text; App resolves keys when it builds what the panel shows.
    Localization localization_;
    std::string tr(std::string_view key, const UiArgs& args = {}) const {
        return localization_.text(key, args);
    }
    std::string tr(const UiText& message) const { return localization_.text(message); }
    std::string tr(const ErrorText& error) const {
        return error.message.key.empty() ? error.english : tr(error.message);
    }
    // An exception's message as shown: a module's UiError in the interface
    // language, any other in its own text, which stays English.
    std::string tr(const std::exception& error) const { return tr(error_text(error)); }
    // Whether the status line shows the message `key` names. Compared as text:
    // the interface language changes only as Settings opens, applies or
    // reloads, and a message from before a change no longer matches.
    bool status_is(std::string_view key) const { return !status_.empty() && status_ == tr(key); }
    // The Debug card's choice, else the system's language; a change rebuilds
    // what holds interface text.
    void follow_interface_language();
    std::string composition_hint(const PanelView& v) const;
    std::string interface_shown_; // the language the cards were built in
    Settings settings_;
    SettingsWriter settings_writer_;
    std::string save_status_;
    bool save_status_is_startup_{};
    bool reload_requested_{};
    // default_ is the main language, which stays while cycling changes
    // settings_.active, the one being typed in. Settings edits main.
    Selection pending_, default_;
    Selection main_selection() const;
    // Only explicit Apply changes the saved choice. Startup fallback and
    // favorite cycling affect the runtime selection/default independently.
    Selection persisted_active_;
    std::string startup_status_;
    std::vector<Selection> cycle_entries_;
    std::vector<Favorite> pending_favorites_;
    bool pending_hide_numpad_{};
    std::string pending_background_surface_{"visible"};
    bool pending_hover_fade_{true};
    bool pending_curved_keyboard_{};
    std::string pending_haptic_sounds_{"off"};
    std::string pending_haptic_effect_{"e"};
    int pending_haptic_volume_{75};
    std::optional<SoundPreview> sound_preview_;
    bool pending_use_drag_to_move_{};
    bool pending_auto_leveling_{true};
    bool pending_vibration_{true};
    bool pending_replace_native_keyboard_{};
    bool pending_start_with_steamos_{};
    // Streaming support, as the card shows it: off whenever replacement is.
    bool pending_streaming_support_{};
    // This Settings visit changed it, or replacement, or answered its
    // question: a flag that comes back from a failed save leaves the card
    // alone then (R217).
    bool streaming_touched_{};
    // Read when Settings opens: whether it can be offered, and whether
    // turning it on has to wait for a SteamOS restart.
    StreamSupportStatus stream_status_;
    // The user ticked it in this visit to Settings and has not applied it.
    bool streaming_turned_on() const {
        return pending_streaming_support_ && pending_replace_native_keyboard_ &&
               !(settings_.streaming_support && settings_.replace_native_keyboard);
    }
    // Leaving Settings with ← asks about changed languages first, then about
    // streaming support: the first answer waits here for the second.
    std::optional<bool> leave_saves_languages_;
    bool pending_swipe_typing_{};
    bool pending_typing_suggestions_{true};
    bool pending_key_flicks_{};
    bool pending_emoji_close_on_select_{};
    bool pending_flick_only_{};
    bool pending_flick_katakana_{};
    bool pending_extended_key_collision_{true};
    // The Debug card (FRAMEKEYBOARD_DEBUG_MODE builds with Options::debug).
    // Its switches stay in memory, never in config.json, so a restart starts
    // from these defaults; Apply and save takes them and Back discards them,
    // as for every other card.
    bool debug_mark_anthy_{true}, pending_debug_mark_anthy_{true};
    bool debug_key_outlines_{}, pending_debug_key_outlines_{};
    // The interface language for this run, chosen on the Debug card; empty
    // follows the system.
    std::string debug_interface_language_, pending_debug_interface_language_;
    // Anthy restarts on the conversion worker for learning turned on or
    // off, or to clear what it learned (docs/japanese.md, Learning), and
    // Mozc's learning is cleared with it; live candidates wait.
    bool anthy_restarting_{};
    bool clearing_learning_{};
    bool mozc_clear_failed_{}; // Its profile stayed (clear_mozc_learning()).
    bool clear_queued_{};
    bool learned_{}; // Something to clear, as of Settings opening or a restart.
    void apply_learning();
    fs::path anthy_home() const;
    bool owns_anthy() const;
    // Japanese among the user's languages as saved, main or additional.
    bool japanese_added() const;
    // Mozc's server runs only while Japanese is added: started when it is,
    // kept running, and stopped when it is removed, not to use its memory.
    void sync_mozc();
    // Likewise the engines and tables of the Chinese languages added: a
    // composer for each keeps its dictionaries or table loaded while another
    // language is in use, and goes when the language is removed.
    void sync_chinese();
    std::map<std::string, std::unique_ptr<CjkComposer>> chinese_engines_; // By input method.
    // Where the Chinese engines keep what they learn; empty for an image
    // export, which learns nothing.
    fs::path chinese_learning_dir() const;
    bool chinese_learned() const;
    // Clear learned words, for Chinese: its composers go, then their files.
    void clear_chinese_learning();
    // The server's state last asked of the worker, and whether that start
    // or stop is still under way there.
    std::optional<bool> mozc_wanted_ = false;
    bool mozc_starting_{};
    fs::path mozc_server_; // Empty without Mozc: Anthy converts.
    // When tick() last made sure the server runs: one that died starts again
    // within seconds, not at the next conversion.
    double mozc_checked_{};
    // What Mozc learned is saved once the user pauses: 3 s after the last
    // phrase it learned, key press or laser aimed over the panel, as long
    // as nothing more happens, and as the panel hides.
    void tick_mozc_save(double now);
    // Saves on the worker what Mozc learned, once for each count of phrases
    // learned: a save that found the server gone isn't asked for again.
    void save_mozc();
    std::uint64_t mozc_save_asked_{};
    void note_aiming(unsigned pointer, double x, double y);
    double mozc_clock_{}; // tick()'s last time, for moves, which come without one
    double active_at_{};
    std::uint64_t mozc_unsaved_{};
    // Chinese learning waits for the same pause, or the panel hiding, and is
    // saved in one go rather than on each commit (R142).
    bool chinese_unsaved_ = false;
    void save_chinese();
    struct Aim {
        double x, y, at; // Where a laser was, and when, as its motion began.
    };
    std::map<unsigned, Aim> aims_;
    bool learning_allowed() const;
    void restart_learning(bool clear);
    void learning_restarted();
    bool restarting_anthy() const { return anthy_restarting_; }
    // Every call to Mozc and Anthy runs on the conversion worker, whose
    // answers tick() takes each frame (docs/architecture.md, Japanese
    // conversion). With Options::wait_for_japanese, a request waits for
    // its answer.
    JapaneseWorker japanese_worker_;
    void post_japanese_task(std::function<void()> work, std::function<void()> done = {});
    void take_japanese_answers();
    bool pending_focus_tooltip_{};
    bool pending_desktop_unicode_paste_{};
    int pending_dictation_send_gap_{20};
    bool pending_dictation_send_paste_{};
    bool pending_dictation_direct_{};
    bool pending_dictation_fallback_{};
    std::string pending_dictation_device_model_{"small-q8_0"};
    std::string pending_dictation_, pending_dictation_server_;
    // The address popup's own one-line field (docs/dictation.md, entering
    // the server address). It types only here, never into the target.
    struct AddressEntry {
        std::string text;
        std::size_t cursor{};
        std::string error; // its key (dictation.server.*), shown in the language then chosen
    };
    std::optional<AddressEntry> address_;
    // A held Backspace or arrow in the popup repeats like the editor's; it
    // lasts only while its pointer still holds that key's press.
    struct AddressRepeat {
        unsigned pointer{};
        std::string key;
        double next{};
    };
    std::optional<AddressRepeat> address_repeat_;
    // A question over Settings: clearing favorites, or downloading the
    // on-device model, either on its own or before applying the settings
    // that need it.
    // The last three ask before a Settings action ends dictation.
    enum class Confirm {
        None,
        ClearLanguages,
        ClearLearning,
        SaveLanguages,
        Download,
        DownloadAndApply,
        QuitDictation,
        SteamKeyboardDictation,
        UpdateDictation,
        EnableStreaming, // ticking Streaming support: what it does, Cancel or Accept
        LeaveStreaming   // ← with it ticked but not applied
    };
    Confirm confirm_{Confirm::None};
    bool dictation_question() const {
        return confirm_ == Confirm::QuitDictation || confirm_ == Confirm::SteamKeyboardDictation ||
               confirm_ == Confirm::UpdateDictation;
    }
    // Something a Settings action would end: listening, whether or not the
    // user is speaking; an editor with text or phrases on the way; a Type as I
    // speak session still typing.
    bool dictation_at_stake() const { return dictation_holds_panel() || direct_active(); }
    void quit_keyboard();
    void use_steam_keyboard();
    // The Additional languages popup (app_languages.cpp). Its lists glide to
    // stick steps as the cards do. The languages list scrolls under an up or
    // down laser drag; a sideways one, or any on an additional row, lifts the
    // row. A drag's release clicks nothing.
    struct LanguagesPopup {
        enum class Which { Layouts, Languages, Additional };
        struct List {
            double scroll{}, target{};
        };
        std::string layout; // Whose languages show.
        // Worked out once per layout: every pointer event builds a view, and
        // checking 77 languages against a layout isn't cheap.
        std::vector<std::string> layout_order, language_order;
        std::map<std::string, bool> usable; // Language -> this layout can type it.
        // Language -> swipe support, looked up once as it reads the disk.
        // Empty while swiping is off, so the rows show no icon at all.
        std::map<std::string, bool> swipe;
        List layouts, languages, additional;
        struct Drag {
            unsigned pointer{};
            Which source{Which::Languages};
            Selection pair;
            bool liftable{}, lifted{}, scrolling{}, over_main{};
            double press_x{}, press_y{}, x{}, y{}, scroll{};
            std::optional<std::size_t> insert; // Where it would land in the list.
        };
        std::optional<Drag> drag;
        double last_tick{-1};
    };
    std::optional<LanguagesPopup> languages_;
    // Popup edits prompt on leaving Settings; the dropdowns alone do not.
    bool languages_edited_{};
    Selection saved_main_;
    std::vector<Selection> saved_additional_;
    // Pending additional languages in cycle order, each once, without main.
    std::vector<Selection> additional_pairs() const;
    void set_additional_pairs(const std::vector<Selection>& pairs);
    void set_main_language(const Selection& pair, std::vector<Selection> additional);
    void remember_languages();
    bool languages_changed() const;
    void open_languages();
    void close_languages();
    void leave_settings();
    std::vector<std::string> layout_order() const;
    void order_languages();
    double languages_limit(LanguagesPopup::Which which) const;
    std::optional<bool> language_swipe(const std::string& language) const;
    PanelView::LanguagesView::Row language_row(const Selection& pair) const;
    void append_languages(PanelView& v) const;
    void languages_press(unsigned pointer, double x, double y);
    bool languages_move(unsigned pointer, double x, double y);
    void update_languages_drop();
    bool languages_release(unsigned pointer);
    void languages_action(const ControlAction& command);
    bool languages_scroll(unsigned pointer, double dy);
    LanguagesPopup::List* languages_list_under(double x, double y);
    std::optional<ScrollRoom> languages_scroll_room(double x, double y) const;
    void tick_languages(double now);
    // The theme picker (app_themes.cpp): every theme as a card, in rows that
    // glide to stick steps; an up or down laser drag scrolls them too, and its
    // release clicks nothing. A card chooses its theme at once, as the
    // dropdown did: Settings shows in it, Back from Settings undoes it.
    struct ThemesPopup {
        double scroll{}, target{};
        struct Drag {
            unsigned pointer{};
            double press_y{}, scroll{};
            bool scrolling{};
        };
        std::optional<Drag> drag;
        double last_tick{-1};
        // The cards' pictures: they live and go with the popup, and while
        // the panel is hidden they are freed and nothing loads.
        ThemePreviews previews;
        // Every theme has its picture: idle frames look at none of them
        // (R297). Cleared when the themes may have changed (every
        // update_settings_ui: a Reload, a shell's theme) and on hiding.
        bool filled{};
    };
    std::optional<ThemesPopup> themes_;
    ThemePreviewStore theme_store_; // the pictures on disk, for the whole process
    std::vector<const Theme*> theme_order() const;
    double themes_limit() const;
    void open_themes();
    void close_themes();
    void append_themes(PanelView& v) const;
    void themes_press(unsigned pointer, double x, double y);
    bool themes_move(unsigned pointer, double x, double y);
    bool themes_release(unsigned pointer);
    bool themes_scroll(unsigned pointer, double dy);
    std::optional<ScrollRoom> themes_scroll_room(double x, double y) const;
    void tick_themes(double now);
    // After anything that may change the themes (every update_settings_ui:
    // a Reload, a shell's theme): their pictures and the scroll follow.
    void themes_reloaded();
    void load_visible_pictures();
    void address_key(const std::string& key);
    // The Dictation card's connection line, as its key (setting.dictation-test.*,
    // which the shell protocol also maps): empty until Test runs.
    std::string dictation_test_;
    // The address the Test in flight checks: one at a time, and its answer
    // labels and remembers only that address.
    std::optional<std::string> testing_server_;
    void test_dictation_server();
    bool native_fallback_{};
    bool replacement_available_{};
    UiText replacement_unavailable_reason_;
    bool native_session_{};
    bool dismiss_requested_{};
    SettingsUi settings_ui_;
    std::unique_ptr<LanguageMap> keymap_;
    // The launch declaration is frozen; Reload cannot redefine the target.
    std::optional<Language> target_language_;
    OutputController output_;
    // Every text-producing path types through this: output_ unless replaced.
    TextOutput* text_output_;
    KeyboardState keyboard_;
    JapaneseComposer composition_;
    JapaneseWords japanese_words_; // What the composer completes readings to.
    JapaneseWords japanese_kaomoji_;
    // The usual conversions of common phrases (japanese/preferred.tsv), and
    // what was committed for those readings, which wins once it differs.
    JapaneseWords japanese_preferred_;
    NextWords japanese_choices_;
    // Every conversion committed, by its typed reading: live candidates offer
    // it back while its start is typed again (JapaneseComposer::set_learned).
    NextWords japanese_completions_;
    std::unique_ptr<CjkComposer> cjk_;
    DeliveredContext table_context_;
    std::uint32_t table_context_window_{};
    bool table_context_allowed() const;
    void clear_table_context();
    void observe_table_context();
    struct TableDelivery {
        DeliveredContext::Ticket receipt;
        std::uint32_t window{};
        std::optional<std::string> field;
        const CjkComposer* composer{};
    };
    std::optional<TableDelivery> begin_table_delivery();
    void finish_table_delivery(const std::optional<TableDelivery>& ticket, const std::string& text);
    // 한/영 switched two-set Korean to the layout's own letters. Never saved,
    // and a change of language or layout returns it to Hangul.
    bool hangul_latin_{};
    // The last key typed a digit, so Chinese , . and : type themselves.
    bool after_digit_{};
    // Punctuation with two texts that take turns, as “ and ”: by key action,
    // Shift ones with a +, the next text's index.
    std::map<std::string, std::size_t> punctuation_turns_;
    bool japanese_latin_{};
    bool flick_digits_{}; // With japanese_latin_: the flick digits.
    // The flick kana are katakana (the script key, Flick Katakana (JP)).
    // Never saved: it starts in hiragana, and a change of language or
    // layout returns it there.
    bool katakana_{};
    // A flick kana as the kana page types and shows it.
    std::string flick_kana(const std::string& text) const;
    // The kana phrase committed last, which ↶ can bring back until anything
    // else is pressed, a trigger is pulled off the keys or the keyboard hides
    // (a laser resting off them keeps it: holds_typing_state).
    struct Reopen {
        std::string text, reading;
        // The output's counts right after the commit: any change means
        // something was sent since, or the phrase may have been dropped.
        std::uint64_t queued{}, failures{}, cancels{};
    };
    std::optional<Reopen> reopen_;
    // The phrase a Japanese commit typed, while the strip offers what may
    // come next, as a phone does: punctuation (docs/japanese.md,
    // Prediction). A press anywhere but the candidates, a key after the
    // commit, a trigger pulled off the keys or the keyboard hiding ends it; a choice
    // types at once.
    std::optional<std::string> follow_up_;
    // Kept for follow_up_ until the next commit learns a pair.
    const std::vector<std::string>& follow_ups() const;
    struct FollowUps {
        std::string after;
        bool learning{};
        std::vector<std::string> texts;
    };
    mutable std::optional<FollowUps> follow_ups_;
    void type_follow_up(const std::string& text);
    // Phrases committed straight after one another, offered first after the
    // phrase before them (NextWords), and the last commit with the output's
    // count right after it: a commit with nothing sent since follows it. The
    // laser leaving with a pull or the input ending starts a new run.
    NextWords next_words_;
    struct LastCommit {
        std::string text;
        std::uint64_t queued{};
    };
    std::optional<LastCommit> last_commit_;
    // After a commit reached the field: learns that it followed the last.
    void follow_commit(const std::string& text, std::uint64_t queued_before);
    void tick_next_words();
    // ↶'s Backspaces on their way: the reading comes back once the queue has
    // delivered this many entries in all, with no failure since.
    struct Reopening {
        std::string reading;
        std::uint64_t completed{}, failures{};
        // The last tick that saw an entry delivered, and the count then; -1
        // before the first tick.
        double since{-1};
        std::uint64_t progress{};
    };
    std::optional<Reopening> reopening_;
    bool text_ready_{};
    bool text_transport_ready_{}; // The text connection, whatever the profile.
    bool typing_ready_{};
    struct HoverTarget {
        // Bar: a part of the shortcut bar or its menu (bar_hit names them).
        enum class Kind { None, Key, Control, Status, Bar } kind{Kind::None};
        std::string id;
        bool operator==(const HoverTarget&) const = default;
    };
    std::map<unsigned, HoverTarget> hovered_;
    struct PressedControl {
        std::string id;
        ControlAction action;
    };
    std::map<unsigned, PressedControl> pressed_controls_;
    std::string status_;
    bool settings_open_{false}, quit_{false}, recenter_{false};
    std::map<unsigned, std::pair<double, double>> pointer_positions_;
    bool dragging_{}, resizing_{};
    bool passthrough_{};
    bool dictation_passthrough_{}; // Tracked apart from the user's toggle.
    // Since when the message explaining it has shown; negative until the next
    // tick times it. Empty once it is gone or another message replaced it.
    std::optional<double> passthrough_message_since_;
    bool field_needed_{};
    bool hidden_focus_{}, dashboard_focus_{};
    bool dashboard_focus_known_{true}; // set_dashboard_focus's `known`
    double focus_shake_since_{-1};
    double swipe_shake_since_{-1};
    // Typing refused for want of a target (target_missing): the Focus mode
    // button's icon shakes instead of a status message, or, where that button
    // is absent, the status says to click the window. False with a target.
    bool refuse_without_target(double now);
    bool focus_mode_button() const { return resident_replacement() && !hosted(); }
    // No target to type into: a held panel without a field, or the dashboard has the keys.
    bool target_missing() const { return field_needed_ || dashboard_focus_; }
    std::string debug_status_, hidden_window_;
    bool interaction_active_{true};
    bool backend_ready_{true}, backend_can_resume_{};
    std::optional<bool> native_num_requested_;
    std::unique_ptr<SwipeService> swipe_;
    std::shared_ptr<const SwipeKeyboard> swipe_keyboard_; // Set only while swipe is active.
    // Where each shown key takes the laser with Extended key collision,
    // worked out again only when the keys shown change: another layout or
    // page, the picker or dictation mode, a key hidden or shown, or a
    // layout rebuilt in place, which bumps layouts_rebuilt_.
    struct KeyAreasFor {
        const Layout* layout{};
        std::uint64_t rebuilt{};
        bool numpad_hidden{}, mic_hidden{}, script_hidden{};
        std::vector<KeyArea> areas;
    };
    mutable std::optional<KeyAreasFor> key_areas_;
    std::uint64_t layouts_rebuilt_{};
    // Null without Extended key collision, and on a flick page.
    const std::vector<KeyArea>* key_areas(const PanelView& view) const;
    // The same areas whatever the setting says, and on flick pages: where
    // keys take the laser under Background Disabled (R380).
    const std::vector<KeyArea>& all_key_areas(const PanelView& view) const;
    // Pointers whose press began on the keyboard and goes on (R380).
    std::set<unsigned> laser_presses_;
    void hold_laser(unsigned pointer, bool held);
    std::string swipe_prepared_; // Layout, language and word list in swipe_.
    struct SwipeTrack {
        unsigned pointer{};
        std::string key; // The tapped key, for Pinyin composition.
        Rect bounds;     // Its layout bounds: the press is a swipe once it leaves them.
        std::set<int> modifiers;
        xkb_keysym_t symbol{}; // The tapped character if the press never becomes a swipe.
        bool shifted{}, swiping{};
        bool gap{}; // Pressed between keys (swipe_down).
        std::vector<SwipePoint> points;
        std::vector<std::pair<double, double>> trail;
        std::size_t painted{}; // Trail point the last repaint request ended at.
    };
    std::optional<SwipeTrack> swipe_track_;
    // Types a press that never became a swipe, as its release would.
    void type_swipe_tap(const SwipeTrack& track);
    std::optional<FlickTrack> flick_track_;
    // The kana the last tap typed, which a quick repeated tap replaces.
    struct FlickToggle {
        std::string slot;
        std::size_t index{};
        double at{};
    };
    std::optional<FlickToggle> flick_toggle_;
    CandidateStrip candidates_;
    // The band's ∨ opened the candidates over the keys; closes with ∧, a
    // choice, or once there are no candidates.
    bool candidate_list_{};
    // Live candidates wait for a pause: the reading last seen, and when it
    // will have paused long enough. Infinite once tried for that reading.
    std::string live_typed_;
    double live_due_{};
    std::uint64_t swipe_request_{};
    bool swipe_waiting_{}, swipe_shifted_{}, swipe_caps_{};
    // The laser left after releasing the pending swipe: the target's cursor
    // may have moved, so its word is discarded.
    bool swipe_detached_{};
    // What the keys typed since the cursor last may have moved, taken to
    // start at a word's start, so Backspace steps back into earlier words;
    // the word being typed ends it. Its completions show when no swipe
    // alternatives do.
    std::string typed_text_;
    // A word punctuation ended, waiting for a space; and the last word that
    // counted. Both with where they end in typed_text_.
    std::optional<std::pair<std::size_t, std::string>> pending_word_, counted_;
    std::map<unsigned, double> held_keys_; // Typing keys still held, by pointer: their press.
    std::optional<unsigned> word_pointer_; // The pointer that typed into the word last.
    std::uint32_t word_window_{};          // Where the word is typed.
    bool stream_paste_routed_{};           // The sink has the clipboard for a streaming client.
    std::vector<std::string> typing_suggestions_;
    WordListLoader word_list_loader_;
    std::shared_ptr<const WordList> word_list_;
    std::string word_list_id_;                          // The loaded or loading list's.
    std::map<std::string, LearnedWords> learned_words_; // By word list ID.
    // When the keys last typed, in tick() time: the list loads at the first
    // letter and goes after a while without one, as while the panel is hidden.
    bool typed_since_tick_{};
    double typed_at_{-1};
    // Alternatives for the last swiped word; any other input invalidates them.
    std::vector<std::string> swipe_candidates_;
    int swipe_selected_{};
    std::string swipe_committed_;
    std::uint64_t swipe_failures_{}; // Output failures when swipe_committed_ was sent.
    bool swipe_notice_{};            // status_ explains removed alternatives.
    bool swipe_space_{};             // The previous output was a swiped word.
    std::atomic<double> slow_work_deadline_{};
    bool emoji_open_{};
    std::optional<unsigned> emoji_key_pointer_; // Holds the emoji key; release toggles the picker.
    EmojiLayout emoji_layout_;
    double emoji_width_{};
    EmojiPicker emoji_;
    // Created on first open; it checks which emoji the font can draw, once.
    std::unique_ptr<EmojiGlyphs> emoji_glyphs_;
    EmojiCatalog emoji_catalog_; // Only what the font draws; set once checked.
    bool emoji_checked_{}, emoji_font_{true};
    EmojiPreferences emoji_preferences_, emoji_saved_;
    std::uint64_t emoji_content_{};
    // Size, content, tone, popup, lifted: lifting or putting down an emoji
    // asks again, so neither the carried one nor its restored cell goes blank.
    std::tuple<int, std::uint64_t, int, bool, bool> emoji_requested_{};
    int emoji_requested_column_{};
    // Dictation mode (docs/dictation.md). Its keys edit the editor locally;
    // none of them reaches the output queue or the target.
    enum class EditorCommand {
        None,
        Backspace,
        Delete,
        Newline,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown
    };
    // Typed text goes into the editor while dictation mode is open.
    class EditorOutput : public TextOutput {
      public:
        explicit EditorOutput(DictationEditor& editor) : editor_(editor) {}
        bool text_press(unsigned, const std::string& text, double) override {
            return editor_.type(text);
        }
        bool commit_text(const std::string& text) override {
            return fits(text, 0) && editor_.type(text);
        }
        bool replaces_text() override { return true; }
        bool replace_text(std::size_t delete_before, const std::string& text) override {
            if (!fits(text, delete_before)) {
                return false;
            }
            while (delete_before-- > 0) {
                editor_.backspace();
            }
            return editor_.type(text);
        }
        bool taps_then_text(int, std::size_t, const std::string&) override { return false; }

      private:
        // Whole or not at all, where the editor by itself takes what has room:
        // a word's alternatives and a reading count on all of it being there.
        bool fits(const std::string& text, std::size_t freed) const;
        DictationEditor& editor_;
    };
    struct EditorRepeat {
        unsigned pointer{};
        EditorCommand command{EditorCommand::None};
        std::string text; // When the held key types text instead.
        double next{};
        bool extend{}; // Shift was on when it was pressed: a move extends the selection.
    };
    bool dictation_available() const;
    bool editor_editable() const;
    std::string test_address() const;
    void end_input(bool discard_composition, bool cancel_delivery);
    // Types what a pointer other than `keep` holds that types on release: a
    // kana flick, a key flick, or a tap that never became a swipe. A swipe
    // stays; so does a held ↶ unless `undo`, as its Backspaces need the reading.
    void finish_held_taps(std::optional<unsigned> keep, bool undo = true);
    void open_dictation();
    void close_dictation();
    // Rebuilds the mode's keys when the layout or the hidden numpad changed.
    void refresh_dictation_layout();
    void set_dictation_view(PanelView& view) const;
    bool dictation_down(unsigned pointer, const Key& key, double x, double y, double now,
                        bool gap = false);
    // A key pressed on the keyboard, or one of the Japanese band's arrows.
    // With `gap` the press is between keys in a letter's swipe reach, where
    // it can only start a swipe.
    bool press_key(unsigned pointer, const Key* key, double x, double y, double now, const PanelView& v,
                   bool gap = false);
    // Typing again clears an error shown over the editor.
    void clear_editor_error();
    void dictation_key_up(const Key& key);
    EditorCommand editor_command(const Key& key, xkb_keysym_t symbol) const;
    void run_editor_command(EditorCommand command, bool extend);
    // Keeps the cursor's line inside the editor after an edit or a move.
    void follow_cursor();
    // Send (docs/dictation.md, Send). Pieces go to the output queue one at a
    // time, each once the previous one is delivered, so a failure knows how
    // much arrived; delivered text leaves the editor as it goes.
    struct Sending {
        std::vector<SendPiece> pieces;
        std::size_t next{}; // The piece in flight, or the next one.
        std::size_t sent{}; // Editor bytes already delivered and erased.
        bool in_flight{};
        std::uint64_t completed{}, failures{}; // The queue's counters before the piece.
        double since{-1};                      // When the piece was first offered.
        std::size_t total{}, done{};           // Code points, for the counter.
        // A paste: 0 not begun, 1 taking the clipboard, 2 letting Gamescope
        // fetch it, 3 Ctrl+V sent, waiting for the application's request,
        // 4 a short wait for it to insert the text.
        int paste_stage{};
        std::uint64_t paste_write{}, paste_served{};
        double paste_at{};
        // Gamescope's focused window when Send was pressed: every piece is for it.
        std::uint32_t window{};
    };
    bool tick_paste(double now);
    // Erases a delivered piece from the editor and moves on; after the last,
    // the mode closes.
    void finish_piece(double now);
    Clipboard* clipboard();
    // Whether a paste of clipboard write `number` will get our text: in a
    // nested desktop, only its own clipboard counts. Empty while being set.
    std::optional<bool> paste_ready(std::uint64_t number);
    std::unique_ptr<Clipboard> clipboard_;
    bool can_send() const;
    void start_send();
    bool tick_send(double now);
    // Ends a send that has not finished. With `attempted`, the piece in flight
    // may have arrived, so the editor marks it. `moved`: another window took focus.
    void stop_send(bool attempted, bool moved = false);
    std::optional<Sending> sending_;
    std::uint32_t send_window_{}; // Focused when Send was pressed, for a Send that waits.
    // The status a stopped Send set; while it shows, it shows on the card
    // above the case (PanelView::status_card), whichever check stopped it.
    std::string send_status_;
    // Whether Gamescope's focus left `window`, the one output was accepted
    // for; never for 0, a target that names no window (KeySink::focused_window).
    bool window_moved(std::uint32_t window) const {
        return window && output_.focused_window() != window;
    }
    // Live dictation (docs/dictation.md, Capturing). The helper starts on the
    // first capture, Test or download, and exits once dictation is off and
    // none of them is under way.
    bool start_capture();
    void pause_capture();
    // Stops capture and drops every phrase not yet inserted.
    void cancel_capture();
    void discard_dictation();
    bool tick_dictation(double now);
    void dictation_event(const Dictation::Event& event, double now);
    // Phrases not yet inserted, including any the helper may still announce
    // before it confirms a pause.
    bool phrases_pending() const { return !pending_phrases_.empty() || awaiting_pause_; }
    bool awaiting_pause_{};
    // The phrase being spoken, so the recognition ring leaves it out.
    std::uint64_t speaking_id_{};
    // A phrase was spoken and its text is not back yet, in either mode.
    bool recognizing() const;
    // Type as I speak (docs/dictation.md): with the setting on, the mic key
    // captures on the normal keyboard and phrases go straight to the target
    // through the output queue, after keys already queued.
    bool direct_dictation() const { return settings_.dictation_direct && !dictation_open_; }
    // The capture session was started that way; its phrases go to the target.
    bool direct_{};
    // Capturing, or phrases not yet typed.
    // A session lasts until its last phrase is out of the output queue too,
    // so Esc and a field change can still drop it.
    // A failed phrase waiting for Enter's retry still belongs to the session,
    // so Esc can drop it.
    bool direct_active() const {
        return direct_ &&
               (capturing_ || phrases_pending() || !direct_waiting_.empty() ||
                direct_paste_.has_value() || output_.text_pending() || output_.retry_available());
    }
    // Characters waiting before listening pauses.
    static constexpr long max_direct_waiting = 1000;
    // Stops capture and drops every phrase not yet typed, with the status
    // `key` names, if any.
    void stop_direct(std::string_view key);
    void direct_phrase(const std::string& text);
    // Live words (docs/dictation.md, Type as I speak): the phrase being
    // spoken, its last partial guesses and the words typed from them. A word
    // is typed once three guesses in a row agree on it exactly.
    struct LiveWords {
        std::uint64_t id{};
        std::deque<std::vector<std::string>> guesses; // the last three; never logged
        std::vector<std::string> typed;
    };
    std::optional<LiveWords> live_;
    // Words, or in Japanese and Chinese characters.
    std::vector<std::string> live_tokens(const std::string& text) const;
    std::string live_join(const std::vector<std::string>& tokens, std::size_t from,
                          std::size_t to) const;
    void live_partial(std::uint64_t id, const std::string& text);
    // The final text's words not yet typed live.
    std::string live_rest(std::uint64_t id, const std::string& text);
    // Hands waiting phrases to the output queue as room appears.
    void flush_direct(double now);
    void tick_direct(double now);
    // Shows a dictation message over the editor, or in direct mode on the status line.
    void dictation_message(std::string text, bool error);
    void tick_passthrough_message(double now);
    void end_passthrough_message();
    // Recognized phrases waiting for room in the output queue, in order. The
    // front one may be partly typed already.
    std::deque<std::string> direct_waiting_;
    // With Send set to Paste, each phrase is pasted like Send's text: 1
    // taking the clipboard, 2 letting Gamescope fetch it, 3 Ctrl+V sent and
    // waiting for the application's request, 4 a short wait before the next.
    struct DirectPaste {
        std::string text; // never logged
        int stage{1};
        std::uint64_t write{}, served{}, failures{};
        double at{};
    };
    std::optional<DirectPaste> direct_paste_;
    bool direct_type_only_{}; // the clipboard failed this session; phrases are typed
    // Advances a paste; false while it waits.
    void tick_direct_paste(double now);
    // The phrases of this capture session as the prompt's context, at most
    // 200 code points; never logged.
    std::string direct_context_;
    bool direct_typed_{}; // the next phrase starts with a space
    // The target refused the front phrase outright, so the queue kept no
    // retry: it waits for Enter, which is not sent, or Esc.
    bool direct_blocked_{};
    // Type as I speak types at the Send pace; set while its text is going out.
    bool direct_gap_{};
    // The last reason waiting phrases could not go out, logged once per change.
    std::string direct_wait_reason_;
    std::uint64_t direct_request_{}, native_request_{};
    // The window the session types into: focused when its first text went
    // out, or when Enter continued after another window took focus. 0 until
    // then, so a mic tap before the window is chosen stops nothing.
    std::uint32_t direct_window_{};
    // Another window took focus before the front phrase went out: it waits
    // for Enter, as a refused phrase does.
    void block_direct_for_window();
    std::uint64_t direct_failures_{}; // output failures seen when the session started
    std::unique_ptr<Dictation> dictation_;
    // Starts the helper object on first use; the process starts on demand.
    Dictation& dictation();
    // On-device models (docs/dictation.md, On-device recognition).
    fs::path models_dir_;
    fs::path model_path(const std::string& model) const;
    bool model_ready(const std::string& model) const;
    // Settings that need the model: On device, or the server with fallback.
    static bool needs_model(const std::string& mode, bool fallback) {
        return mode == "device" || (mode == "server" && fallback);
    }
    // A model download in progress; the mic key shows it and waits.
    struct ModelDownload {
        std::string model;
        std::uint64_t done{}, total{};
        bool quantizing{};
    };
    std::optional<ModelDownload> download_;
    // 0–1 while downloading, below 0 while the model is prepared.
    double model_download_share() const;
    void start_download(const std::string& model);
    void delete_model(const std::string& model);
    // The last phrase came from the fallback on the Frame; the editor's
    // counter says so quietly until a server phrase arrives.
    bool device_fallback_{};
    // Where speech is recognized now: the headset, the user's computer, a
    // hosted provider, or none without dictation.
    Icon recognition_place() const;
    bool capturing_{};
    bool game_sound_{}; // This capture records around filters a play session runs.
    // A game is running and this capture hears its sound: the status line
    // says so while capturing. The helper reports it once per capture.
    double alive_until_{}; // The helper gets keep-alives until then.
    // How long Send waits for one phrase to be recognized, and the restart of
    // that wait when a phrase comes back.
    double recognition_limit() const;
    void send_progress(double now);
    // The mic key's last five levels, 0–8, oldest first; cut to nine steps
    // so the bars redraw only on visible changes, and not at all in silence.
    std::vector<int> mic_levels_; // Six, as PanelView::dictation_levels.
    // Levels arrive in bursts as audio is read; they enter the bars one
    // every dictation_level_period, so the glide keeps a steady pace.
    std::deque<int> mic_incoming_;
    double mic_level_since_{};
    // A phrase shown word by word, as if typed: editor bytes [start, end)
    // were inserted, and those before `shown` are visible.
    struct Reveal {
        std::size_t start{}, end{}, shown{};
        double next{};
        bool spaced{true}; // false: Japanese and Chinese appear a character at a time
    };
    std::optional<Reveal> reveal_;
    void finish_reveal();
    // A laser press in the editor: dragged, it scrolls the text like a touch
    // screen; released where it began, it places the cursor.
    struct EditorDrag {
        unsigned pointer{};
        double x{}, y{}, scroll{};
        bool scrolling{};
    };
    std::optional<EditorDrag> editor_drag_;
    double editor_scroll_limit() const;
    double editor_line_height() const;
    // Phrases announced and not yet inserted or dropped, with the time each
    // was handed to the recognizer (zero while still being spoken).
    std::map<std::uint64_t, double> pending_phrases_;
    bool waiting_{}; // A phrase has waited over a second for its text.
    std::string notice_;
    bool notice_error_{};
    bool confirm_discard_{};
    std::optional<double> send_wait_since_; // Send waits for pending phrases.
    bool dictation_open_{};
    DictationEditor editor_;
    EditorOutput editor_output_{editor_};
    DictationLayout dictation_layout_;
    // What dictation_layout_ was built from: its geometry also follows the
    // theme. Compared field by field, so the per-frame check builds no string.
    struct DictationLayoutFor {
        std::string layout, theme, language; // The language decides the Japanese band.
        bool numpad_hidden{};
    };
    std::optional<DictationLayoutFor> dictation_layout_for_; // Reset whenever profiles change.
    double editor_scroll_{};
    std::optional<double> editor_goal_x_;           // Column kept across Up and Down.
    std::optional<unsigned> dictation_key_pointer_; // Mic, Send and Undo act on release.
    std::optional<EditorRepeat> editor_repeat_;
    // A held Backspace on a local reading (Japanese, Chinese or Korean) repeats
    // until the reading is empty. Japanese and Chinese stop there. Korean's
    // finished syllables are already in the field, so as on a PC it goes on
    // deleting them: after a fresh delay, its press becomes a native one.
    struct ReadingRepeat {
        unsigned pointer{};
        Key key;
        std::set<int> modifiers;
        double next{};
        bool continues{}; // Emptied: at `next` it goes on into the field.
    };
    std::optional<ReadingRepeat> reading_repeat_;
    bool reading_open() const { return !composition_.empty() || (cjk_ && !cjk_->empty()); }
    void continue_held_backspace(double now);
    // In the editor, a reading that starts over a selection replaces it at
    // once, as a PC's input method does, rather than leave it lit until the
    // commit. A target's selection can't be known, so only the editor's.
    void reading_replaces_selection(bool was_open);
    void start_reading_repeat(unsigned pointer, const Key& key, const std::set<int>& modifiers,
                              double now);

    // The shortcut bar (docs/shortcut-bar-design.md), in app_shortcuts.cpp.
    // App owns the bars, which one shows and the runner; the panel only
    // draws the view App builds, and only the runner sends a button's keys.
    ShortcutModel shortcuts_;
    // An invalid shortcuts.json loaded nothing: Settings says why (never
    // quoting its content), and the next save keeps it as shortcuts.json.bad.
    std::string shortcuts_invalid_;
    ShortcutProfileChoice profile_choice_;
    bool pending_shortcuts_{};
    // Letters go through the target's keymap only in Unicode mode, as Cut,
    // Copy and Paste do (chord_code). Declared after output_, which it uses.
    QueuedShortcutOutput shortcut_output_{output_, [this] { return unicode_mode(); }};
    ShortcutRunner shortcut_runner_{shortcut_output_};
    // The run under way: its profile and button, which stay lit while it runs.
    struct ShortcutRun {
        std::string profile;
        std::size_t button{};
    };
    std::optional<ShortcutRun> shortcut_run_;
    // Its progress in whole pixels of its button's border (R372), -1 for none.
    int run_progress_px_ = -1;
    double run_progress_painted_{}; // when run_progress_px_ last changed (R375)
    // A moving stroke repaints its button at most this often: about 30 times a second.
    static constexpr double progress_repaint_interval = .033;
    // The focus last given to profile_choice_: Gamescope's window and its
    // app's identity. A change of either is a change of focus, which stops
    // a run too; the identity is kept only while that app has the keys.
    FocusedApp bar_focus_;
    std::uint64_t bar_focus_serial_{}; // KeySink::focus_serial, beside it
    // The app with the keys, as the bar sees it: none while Steam's
    // dashboard has them (set_dashboard_focus); in a stream from a PC, the
    // streamed app once this session's answer is in.
    FocusedApp focused_app() const;
    // Streams from a PC (R359). A session is the native request a stream
    // holds (stream_session_target); its app is read off the VR thread when
    // it begins and dies with it, so a new session never shows the last
    // one's game. Until its answer is in, the bar keeps what it shows.
    StreamAppFinder stream_apps_{options_.stream_clients};
    std::optional<std::uint64_t> stream_session_; // its request
    std::uint64_t stream_token_{};                // the finder's number for it
    std::optional<AppIdentity> stream_app_;
    void track_stream_session();
    bool stream_app_pending() const { return stream_session_ && !stream_app_; }
    // The bar's focus is a stream's app (R364). After its session ends, as
    // when the panel hides and the route goes local, Gamescope reports no
    // window and no app for the stream: the focus is unknown then, and the
    // bar keeps that app with its pick or empty bar until a window, the
    // dashboard or another session's different app takes the keys.
    bool stream_focus_held_{};
    // App names (design, Apps): looked up off the VR thread when the menu
    // opens and on an assign, taken on a tick. `app_name_` is the last one
    // found for an app; `name_fixes_` are ties saved with the name the
    // identity gave, updated once when their lookup lands.
    AppNameLookup app_names_{options_.app_names};
    struct AppName {
        AppIdentity app;
        std::string name; // empty: none found, the fallback stands
    };
    std::optional<AppName> app_name_;
    std::vector<AppName> name_fixes_; // the identity and the name saved with it
    std::set<std::pair<int, std::string>> looked_up_names_; // apps looked up in this run (R376)
    // Names found before, for when the files that gave them are gone (R362);
    // read at start, written through settings_writer_.
    std::vector<RecordedName> name_record_;
    void keep_found_name(const AppIdentity& app, const std::string& name);
    // The focused app's name for the picker: its tie's, one found, else the
    // identity's own; empty without an identity.
    std::string focused_app_name() const;
    void look_up_focused_name();
    // R361: a program's identity once held its arguments where they run on
    // in one string. The focused program's old tie, whose ID is its name and
    // a space, is moved to its name alone and saved, as an assign is.
    void adopt_long_program_tie();
    // R362: a desktop stream once showed as Steam's Remote Play Client app; its tie moves to the PC's
    // stream identity when that has the keys, or goes if the stream identity has one already.
    void adopt_remote_play_tie();
    void take_app_names();
    // Why the focused app can't be assigned, as a tooltip; empty when it can.
    std::string assign_unavailable() const;
    // A press on an assign button: ties the focused app to that profile, or
    // unties it, saved at once.
    void assign_profile(std::size_t row);
    // Each bar's sideways scroll, kept while the keyboard runs, by bar ID;
    // stick steps set the target and tick() eases to it.
    struct BarScroll {
        double shown{}, target{}, remainder{};
    };
    std::map<std::string, BarScroll> bar_scroll_;
    BarScroll menu_scroll_;
    double bar_tick_{-1};
    // A press on the bar, the picker or the menu, which acts on release over
    // the same part. One on a button dragged past 12 px scrolls the bar
    // instead; one that stopped the button's own run does nothing more.
    struct BarPress {
        unsigned pointer{};
        std::string target; // as bar_hit names it
        double x{}, scroll{}, at{};
        bool dragging{}, stopped{};
        // The profile a button press began on: it never acts on another (R321).
        std::string profile;
        bool held{}; // runs a While held macro, from its press to its release
    };
    std::optional<BarPress> bar_press_;
    bool bar_menu_{};
    // Shortcuts and macros is on, and no shell runs the keyboard.
    bool shortcuts_enabled() const { return settings_.shortcuts && !hosted(); }
    // The bar or the empty bar shows: not in Settings or dictation mode.
    bool bar_shown() const;
    // The bar showing, or null for the empty bar and for none.
    const ShortcutProfile* shown_profile() const;
    // At startup, and again for Reload config: a reload that finds the file
    // invalid keeps the bars already loaded and reports it.
    void load_shortcuts(bool reloading = false);
    void configure_bar();
    // Saves the model on the preference worker.
    void save_shortcuts();
    // A finished save's result: a failure's status, or, for one that worked,
    // the end of the note that shortcuts.json can't be read.
    void take_shortcuts_result();
    void apply_bar_changes(const ShortcutProfileChoice::Changes& changes);
    void append_shortcut_bar(PanelView& view) const;
    // What of the bar `view` shows lies at (x, y): button-N, picker, plate,
    // and over an open menu row-N, assign-N, edit or menu.
    std::optional<std::string> bar_hit(const PanelView& view, double x, double y) const;
    ShortcutBarGeometry bar_geometry(std::size_t buttons) const;
    Rect bar_button_bounds(const ShortcutBarGeometry& bar, std::size_t count, std::size_t index,
                           double scroll) const;
    bool shortcut_button_enabled(const ShortcutButton& button) const;
    // A press on the bar; true for a button's, which feels like a key's.
    bool bar_down(unsigned pointer, const std::string& target, double x, double now);
    // A pressed button dragged sideways scrolls the bar; true while it does.
    bool bar_move(unsigned pointer, double x);
    bool bar_up(unsigned pointer);
    void bar_action(const std::string& target);
    // Whether the bar or its open menu moved; none when the pointer isn't
    // over them, so what lies under it may scroll.
    std::optional<bool> bar_scroll(unsigned pointer, double dx, double dy);
    std::optional<ScrollRoom> bar_scroll_room(double x, double y) const;
    void tick_shortcut_bar(double now);
    void press_shortcut_button(std::size_t index, double now);
    void stop_shortcut_run();
    // Run buttons (design, Run): a press starts one program off the VR
    // thread; more presses on that button wait until its start is
    // reported, taken on a tick. A failed start shows a status naming the
    // button; only the kind of failure is logged, as a number.
    ShortcutLauncher launcher_;
    struct PendingStart {
        std::uint64_t id{};
        std::string profile;
        std::size_t button{};
        std::string label; // for the status; never logged
    };
    std::vector<PendingStart> pending_starts_;
    std::uint64_t next_start_{};
    void start_run_button(const std::string& profile, std::size_t index, const ShortcutButton& button);
    void take_started();
    // The Debug card's sample bars, while there are none.
    void add_sample_shortcuts();

    // The shortcut editor (docs/shortcut-bar-design.md, Settings and the
    // editor), in app_shortcut_editor.cpp: a screen of its own over the
    // case, working on its own copy until ✓, with a popup over it for
    // typing labels and bar names and recording keys on a mini keyboard.
    std::optional<ShortcutEditor> shortcut_editor_;
    enum class EditorQuestion { None, Leave, DeleteProfile };
    EditorQuestion editor_question_{EditorQuestion::None};
    BarScroll editor_rows_scroll_, editor_strip_scroll_;
    double editor_tick_{-1};
    // A press on a bar row or a strip button, which a drag past the dead zone
    // lifts: a row moves up or down the list, a button along the strip, or
    // off it to be deleted. Its release then clicks nothing.
    struct ShortcutDrag {
        unsigned pointer{};
        bool row{};
        std::size_t index{}; // where it is now: it moves as it passes others
        double x{}, y{};     // the press
        double px{}, py{};   // the laser now
        bool lifted{}, outside{};
        bool step{}; // a macro step's row, moved up or down its list
        // A button over the profiles' rows: the row it is over, and whether
        // that profile takes it (another one, not full); it lights up.
        std::optional<std::size_t> over_row;
        bool takes{};
        // A script's row: the drag scrolls the list, from `scroll` at the press.
        bool scripts{};
        double scroll{};
        // A bar's button, moved along the strip: no other kind's (R352).
        bool button() const { return !row && !step && !scripts; }
    };
    // A button dropped on another profile's row: Move or Copy, in a popover
    // beside that row, until either or a press anywhere else.
    struct Carry {
        std::size_t button{}, profile{};
    };
    std::optional<Carry> carry_;
    std::optional<ShortcutDrag> shortcut_drag_;
    // The editor's screen shows: open, with no field over it, outside Settings.
    bool editor_screen() const { return shortcut_editor_ && !field_ && !settings_open_; }
    // Full width, or the keyboard's width without its numpad on a narrower case.
    double editor_width(const PanelView& keyboard) const;
    void open_shortcut_editor();
    void close_shortcut_editor(bool save);
    void append_editor_controls(std::vector<Control>& result, const Rect& body) const;
    void append_shortcut_editor(PanelView& view) const;
    void editor_action(const ControlAction& action);
    // Starts a drag candidate for a press on a row or strip button.
    void editor_press(unsigned pointer, const std::string& id, double x, double y);
    // True while a lifted drag takes the laser.
    bool editor_drag_move(unsigned pointer, double x, double y);
    bool editor_drag_up(unsigned pointer);
    // The selected button is a macro: the Macro view shows, not the Shortcut view.
    bool editing_macro() const;
    // The selected button is a shortcut: the Shortcut view shows.
    bool editing_keys() const;
    bool editor_scroll(unsigned pointer, double dx, double dy);
    std::optional<ScrollRoom> editor_scroll_room(double x, double y) const;
    void tick_shortcut_editor(double now);
    // The Macro view (design, Settings and the editor): its steps' list
    // scrolls; a held − or + of a wait or of Repeat's count steps on.
    BarScroll editor_steps_scroll_;
    struct ValueRepeat {
        unsigned pointer{};
        std::string id; // the control held
        ControlAction action;
        double next{};
    };
    std::optional<ValueRepeat> value_repeat_;
    void append_macro_controls(std::vector<Control>& result, const Rect& well,
                               const ShortcutButton& button) const;
    // What a press on the macro does, as the line beside Repeat says it.
    std::string macro_sends(const ShortcutAction& action) const;
    std::string wait_text(int ms) const;
    double editor_steps_limit() const;
    // The Run view (design, Settings and the editor): Script's list of the
    // scripts folder, read off the VR thread when the view shows it and on
    // Refresh, none until the first read lands; or Command's field.
    ScriptLister script_lister_{options_.script_list};
    std::optional<std::vector<std::string>> scripts_;
    // Scripts the editor's buttons chose that are in the folder past the
    // list's end, as the last read found (R354).
    std::vector<std::string> scripts_beyond_;
    bool scripts_reading_{};
    // Reads the folder again, with every script the editor's buttons chose.
    void read_scripts();
    BarScroll editor_scripts_scroll_;
    struct ScriptRow {
        std::string name;
        bool missing{}; // the chosen script, gone from the folder
    };
    // The list as it shows: the folder's scripts, and a chosen one that is
    // gone, in their order.
    std::vector<ScriptRow> script_rows() const;
    double editor_scripts_limit() const;
    // The selected button is Run with Script chosen: the list shows.
    bool editing_scripts() const;
    void append_run_controls(std::vector<Control>& result, const ShortcutButton& button) const;
    // The scripts folder as the user knows it: ~ for the home folder.
    std::string scripts_folder_text() const;

    // The editor's popup (design, Typing and recording): a fixed US mini
    // keyboard whose keys are the editor's own controls, acting on press.
    // Nothing it does goes through the key state, the output queue, a
    // TextOutput or a typing language, so nothing reaches the app.
    struct EditorPopup {
        ShortcutPopupView::Kind kind{ShortcutPopupView::Kind::Label};
        std::string text;                  // Label, Name: never logged
        std::size_t cursor{};              // a byte offset into text
        std::vector<ShortcutChord> chords; // Recording, from nothing
        // Modifier keys latched by a tap, in the order tapped; a second tap
        // lets one go, a key takes them all.
        std::vector<std::string> latched;
        bool caps{}; // Caps Lock, for text
        // Recording a macro's Keys step, `step` or a new one added on Done,
        // rather than the shortcut.
        std::optional<std::size_t> step;
        bool new_step{};
    };
    std::optional<EditorPopup> popup_;
    // A held Backspace, Delete or arrow repeats, as on the keyboard.
    struct PopupRepeat {
        unsigned pointer{};
        std::string key; // the mini key's ID
        double next{};
    };
    std::optional<PopupRepeat> popup_repeat_;
    // Recorded combinations scroll in a strip of fixed width, in the editor
    // and in the popup, with the stick or a drag (R331); the popup's follows
    // the newest chip.
    BarScroll editor_chips_scroll_, popup_chips_scroll_;
    struct ChipsDrag {
        unsigned pointer{};
        bool popup{};
        double x{}, scroll{}; // where the press began
    };
    std::optional<ChipsDrag> chips_drag_;
    bool chips_press(unsigned pointer, double x, double y);
    bool chips_drag_move(unsigned pointer, double x);
    // How far each strip can scroll: its chips' width past its own.
    double editor_chips_limit() const;
    double popup_chips_limit() const;
    // The popup's strip to its newest chip.
    void follow_popup_chips();
    // Made from en-us-full as the popup opens, whatever layout and language
    // are in use; the keymap gives its keys their US characters and legends.
    Layout mini_layout_;
    std::unique_ptr<LanguageMap> mini_keymap_;
    void open_popup(ShortcutPopupView::Kind kind, std::optional<std::size_t> step = std::nullopt,
                    bool new_step = false);
    // How many combinations the recording takes: 4 for a shortcut, 32 for a step.
    std::size_t popup_limit() const;
    // Ends the popup, keeping what it holds when `save`, as Enter or Done.
    void end_popup(bool save);
    // A mini key pressed: it acts at once; Backspace, Delete and the arrows
    // repeat while held.
    void popup_key(const std::string& id, unsigned pointer, double now);
    void popup_text_key(const Key& key);
    // Records the latched modifiers with `key`, or alone when it is empty,
    // as one combination; the latch ends.
    void popup_record(const std::string& key);
    void popup_action(const std::string& value);
    void append_popup_controls(std::vector<Control>& result, const Rect& body) const;
    void append_popup_view(ShortcutEditorView& view, const Rect& body) const;
    void tick_popup(double now);

    // Renaming in place (design, Renaming in place): a label typed on the
    // real keyboard as a field on the bar. Kept for that phase; nothing opens
    // it yet. The editor's screen waits behind it.
    struct ShortcutField {
        std::string text;     // never logged
        std::size_t cursor{}; // a byte offset into text
    };
    std::optional<ShortcutField> field_;
    // Typed text, composed, swiped or flicked, goes into the field: whole,
    // or not at all where the label would grow past its limit.
    class FieldOutput : public TextOutput {
      public:
        explicit FieldOutput(App& app) : app_(app) {}
        bool text_press(unsigned, const std::string& text, double) override {
            return app_.field_insert(text);
        }
        bool commit_text(const std::string& text) override { return app_.field_insert(text); }
        bool replaces_text() override { return true; }
        bool replace_text(std::size_t delete_before, const std::string& text) override {
            return app_.field_replace(delete_before, text);
        }
        bool taps_then_text(int, std::size_t, const std::string&) override { return false; }

      private:
        App& app_;
    };
    FieldOutput field_output_{*this};
    bool field_typing() const { return field_.has_value(); }
    // The selected button's label, typed on the keyboard.
    void open_field();
    // Ends the field, keeping what it holds when `save`, as Enter or Done.
    void end_field(bool save);
    bool field_insert(const std::string& text);
    bool field_replace(std::size_t delete_before, const std::string& text);
    bool field_down(unsigned pointer, const Key& key, double x, double y, double now, bool gap);
    void field_action(const ControlAction& action);
    void append_field_controls(std::vector<Control>& result, const Rect& body) const;
    void append_field_view(PanelView& view) const;
    void append_field_bar(PanelView& view) const;
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
// The shell's control descriptor, kept open past the VR loop for a hosted
// update's handover; -1 otherwise.
extern int hosted_handover_fd;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration);
} // namespace framekeyboard
