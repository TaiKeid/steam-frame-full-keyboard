#pragma once

#include "cjk.hpp"
#include "clipboard.hpp"
#include "dictation.hpp"
#include "dictation_editor.hpp"
#include "dictation_mode.hpp"
#include "dictation_send.hpp"
#include "emoji_glyphs.hpp"
#include "instance.hpp"
#include "japanese.hpp"
#include "output.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include "settings_ui.hpp"
#include "settings_writer.hpp"
#include "swipe.hpp"
#include <atomic>
#include <csignal>
#include <deque>
#include <optional>
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
struct Options {
    fs::path data_dir, config_dir, ei_socket, text_socket;
    Mode mode{Mode::Help};
    InputBackend input{InputBackend::None};
    std::string output, target_language;
    double duration{};
    bool start_enabled{false};
    bool autostart{};
    bool background_update{};
    // Tests replace the optional swipe engine module; empty uses the installed one.
    SwipeEncoderFactory swipe_encoder;
    // Tests capture typed text here instead of the output queue; null uses the queue.
    TextOutput* text_output{};
    // Tests replace the dictation helper; empty uses the installed one.
    fs::path dictation_helper;
    std::vector<std::string> dictation_arguments;
    // Tests replace the clipboard writer; null writes the X clipboard.
    Clipboard* clipboard{};
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
               settings_ui_.pressed(pointer) || (swipe_track_ && swipe_track_->pointer == pointer) ||
               emoji_.pressed(pointer);
    }
    // Swipe typing is on and its word list has loaded; until then letters type on press.
    bool swipe_ready() { return swipe_enabled() && swipe_->ready(); }
    void cancel_pointer(unsigned pointer);
    void cancel(bool discard_composition = true);
    bool tick(double now);
    // Whether the image changed and needs uploading.
    bool paint(double now);
    void show_settings();
    void back();
    bool scroll(unsigned pointer, double dx, double dy);
    bool pinned() const { return settings_.pinned; }
    // Experimental, not saved: the panel stays open while the dashboard is
    // closed, and VR reads the controllers itself so the focused app keeps its input.
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
    // The panel left the view; listening pauses. A pause in interaction,
    // such as Steam handing over a new field, does not.
    void panel_hidden();
    // A prepared update discards dictation and stops its helper, which has
    // exited by the time App is destroyed and the handoff starts.
    void end_dictation_for_update();
    bool dictation_helper_running() const { return dictation_ && dictation_->running(); }
    bool use_drag_to_move() const { return settings_.use_drag_to_move; }
    bool auto_leveling() const { return settings_.auto_leveling; }
    bool replace_native_keyboard() const { return settings_.replace_native_keyboard; }
    bool start_with_steamos() const {
        return settings_.replace_native_keyboard && settings_.start_with_steamos;
    }
    bool background_start() const {
        return options_.autostart || (options_.background_update && settings_.replace_native_keyboard);
    }
    bool replacement_ready() const;
    // Fixed diagnostic text for why replacement_ready() is false.
    const char* replacement_block() const;
    bool take_native_fallback();
    bool resident_replacement() const {
        return options_.mode == Mode::Vr && settings_.replace_native_keyboard;
    }
    void close_panel();
    bool take_dismissal();
    void set_replacement_available(bool available, std::string reason = {}) {
        if (available) {
            reason.clear();
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
        // Corner resizers work in either movement mode; only the pin locks them.
        return !pinned() &&
               (source == MoveSource::Resize || use_drag_to_move() == (source == MoveSource::Grip));
    }
    void summon();
    // A corner resize holds the same input gate as movement.
    void set_dragging(bool dragging, bool resizing = false);
    // Hiding the VR context releases keys before closing the input gate.
    void set_interaction_active(bool active);
    void report_status(const std::string& message);
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
    bool quitting() const { return quit_; }
    bool updating() const;
    bool take_recenter();
    bool emoji_open() const { return emoji_open_; }
    bool dictation_open() const { return dictation_open_; }
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

  private:
    std::vector<Control> controls() const;
    PanelView build_view(bool picker) const;
    void update_settings_ui();
    void activate(Selection selection);
    void save();
    void update_save_status();
    void allow_slow_work();
    void rebuild_cycle_entries();
    void action(const ControlAction& action);
    void refresh_typing();
    bool japanese_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_japanese();
    void convert_japanese();
    bool japanese() const;
    bool cjk_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_cjk();
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
    bool swipe_enabled() const { return swipe_ && swipe_keyboard_; }
    void prepare_swipe();
    bool swipe_down(unsigned pointer, const Key& key, double x, double y, double now,
                    const std::set<int>& mods);
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
    Settings settings_;
    SettingsWriter settings_writer_;
    std::string save_status_;
    bool save_status_is_startup_{};
    bool reload_requested_{};
    Selection pending_, default_;
    // Only explicit Apply changes the saved choice. Startup fallback and
    // favorite cycling affect the runtime selection/default independently.
    Selection persisted_active_;
    std::string startup_status_;
    std::vector<Selection> cycle_entries_;
    std::vector<Favorite> pending_favorites_;
    bool pending_hide_numpad_{};
    bool pending_use_drag_to_move_{};
    bool pending_auto_leveling_{true};
    bool pending_replace_native_keyboard_{};
    bool pending_start_with_steamos_{};
    bool pending_swipe_typing_{};
    bool pending_emoji_close_on_select_{};
    int pending_dictation_send_gap_{20};
    bool pending_dictation_send_paste_{};
    bool pending_dictation_direct_{};
    std::string pending_dictation_, pending_dictation_server_;
    // The address popup's own one-line field (docs/dictation.md, entering
    // the server address). It types only here, never into the target.
    struct AddressEntry {
        std::string text;
        std::size_t cursor{};
        std::string error;
    };
    std::optional<AddressEntry> address_;
    bool confirm_clear_favorites_{};
    void address_key(const std::string& key);
    // The Dictation card's connection line: empty until Test runs.
    std::string dictation_test_;
    void test_dictation_server();
    bool native_fallback_{};
    bool replacement_available_{};
    std::string replacement_unavailable_reason_;
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
    std::unique_ptr<CjkComposer> cjk_;
    bool cjk_latin_{};
    bool japanese_latin_{};
    bool text_ready_{};
    bool text_transport_ready_{}; // The text connection, whatever the profile.
    bool typing_ready_{};
    struct HoverTarget {
        enum class Kind { None, Key, Control } kind{Kind::None};
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
    bool field_needed_{};
    bool interaction_active_{true};
    bool backend_ready_{true}, backend_can_resume_{};
    std::optional<bool> native_num_requested_;
    std::unique_ptr<SwipeService> swipe_;
    std::shared_ptr<const SwipeKeyboard> swipe_keyboard_; // Set only while swipe is active.
    std::string swipe_prepared_;                          // Layout, language and word list in swipe_.
    struct SwipeTrack {
        unsigned pointer{};
        std::string key; // The tapped key, for Pinyin composition.
        std::set<int> modifiers;
        xkb_keysym_t symbol{}; // The tapped character if the press never becomes a swipe.
        bool shifted{}, swiping{};
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
    std::uint64_t swipe_request_{};
    bool swipe_waiting_{}, swipe_shifted_{}, swipe_caps_{};
    // The laser left after releasing the pending swipe: the target's cursor
    // may have moved, so its word is discarded.
    bool swipe_detached_{};
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
    std::tuple<int, std::uint64_t, int, bool> emoji_requested_{}; // Size, content, tone, popup.
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
        bool commit_text(const std::string& text) override { return editor_.type(text); }
        bool replaces_text() override { return false; }
        bool replace_text(std::size_t, const std::string&) override { return false; }
        bool taps_then_text(int, std::size_t, const std::string&) override { return false; }

      private:
        DictationEditor& editor_;
    };
    struct EditorRepeat {
        unsigned pointer{};
        EditorCommand command{EditorCommand::None};
        std::string text; // When the held key types text instead.
        double next{};
    };
    bool dictation_available() const;
    void open_dictation();
    void close_dictation();
    // Rebuilds the mode's keys when the layout or the hidden numpad changed.
    void refresh_dictation_layout();
    void set_dictation_view(PanelView& view) const;
    bool dictation_down(unsigned pointer, const Key& key, double x, double y, double now);
    // Typing again clears an error shown over the editor.
    void clear_editor_error();
    void dictation_key_up(const Key& key);
    EditorCommand editor_command(const Key& key, xkb_keysym_t symbol) const;
    void run_editor_command(EditorCommand command);
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
        // 4 a short wait for it to insert the text before Enter.
        int paste_stage{};
        std::uint64_t paste_write{}, paste_served{};
        double paste_at{};
    };
    bool tick_paste(double now);
    // Erases a delivered piece from the editor and moves on; after the last,
    // the mode closes.
    void finish_piece(double now);
    Clipboard* clipboard();
    std::unique_ptr<Clipboard> clipboard_;
    bool can_send() const;
    void start_send();
    bool tick_send(double now);
    // Ends a send that has not finished. With `attempted`, the piece in flight
    // may have arrived, so the editor marks it.
    void stop_send(bool attempted);
    std::optional<Sending> sending_;
    // Live dictation (docs/dictation.md, Capturing). The helper starts on the
    // first capture and stays until the keyboard exits.
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
    // Type as I speak (docs/dictation.md): with the setting on, the mic key
    // captures on the normal keyboard and phrases go straight to the target
    // through the output queue, after keys already queued.
    bool direct_dictation() const { return settings_.dictation_direct && !dictation_open_; }
    // The capture session was started that way; its phrases go to the target.
    bool direct_{};
    // Capturing, or phrases not yet typed.
    bool direct_active() const {
        return direct_ && (capturing_ || phrases_pending() || !direct_waiting_.empty() ||
                           direct_paste_.has_value());
    }
    // Characters waiting before listening pauses.
    static constexpr long max_direct_waiting = 1000;
    // Stops capture and drops every phrase not yet typed, with a status.
    void stop_direct(std::string status);
    void direct_phrase(const std::string& text);
    // Hands waiting phrases to the output queue as room appears.
    void flush_direct(double now);
    void tick_direct(double now);
    // Shows a dictation message over the editor, or in direct mode on the status line.
    void dictation_message(std::string text, bool error);
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
    // The last reason waiting phrases could not go out, logged once per change.
    std::string direct_wait_reason_;
    std::uint64_t direct_request_{}, native_request_{};
    std::uint64_t direct_failures_{}; // output failures seen when the session started
    std::unique_ptr<Dictation> dictation_;
    bool capturing_{};
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
    // Everything but the final Enter arrived; Send with an empty editor
    // presses only Enter, so the text is never sent twice.
    bool enter_pending_{};
    bool dictation_open_{};
    DictationEditor editor_;
    EditorOutput editor_output_{editor_};
    DictationLayout dictation_layout_;
    std::string dictation_layout_key_; // Layout ID and numpad state it was built for.
    double editor_scroll_{};
    std::optional<double> editor_goal_x_;           // Column kept across Up and Down.
    std::optional<unsigned> dictation_key_pointer_; // Mic, Send and Undo act on release.
    std::optional<EditorRepeat> editor_repeat_;
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration);
} // namespace framekeyboard
