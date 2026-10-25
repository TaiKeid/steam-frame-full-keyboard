#pragma once

#include "cjk.hpp"
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
#include <optional>
#include <utility>

namespace framekeyboard {
class NativeReplacement;
class Updates;
enum class Mode { Help, Preview, Vr, Render, RenderSettings, RenderEmoji, Check, Describe };
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
    PanelView view() const;
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
    void paint(double now);
    void show_settings();
    void back();
    bool scroll(unsigned pointer, double dx, double dy);
    bool pinned() const { return settings_.pinned; }
    // Experimental, not saved: the panel stays open while the dashboard is
    // closed, and VR reads the controllers itself so the focused app keeps its input.
    bool passthrough() const { return passthrough_; }
    void set_passthrough(bool on);
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
    std::tuple<int, int, std::uint64_t, int, bool> emoji_requested_{};
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration);
} // namespace framekeyboard
