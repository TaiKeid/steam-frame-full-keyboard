#pragma once

#include "cjk.hpp"
#include "instance.hpp"
#include "japanese.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include "settings_ui.hpp"
#include <csignal>
#include <optional>

namespace framekeyboard {
struct Options {
    fs::path data_dir, config_dir, ei_socket, text_socket;
    std::string mode{"help"}, output, input{"none"}, target_language;
    double duration{};
    bool start_enabled{false};
};

// Development previews never send input. The installed typing launcher opts in.
class InputGate : public KeySink {
  public:
    explicit InputGate(KeySink& target) : target_(target) {}
    void send(int code, int value) override {
        if (enabled) {
            target_.send(code, value);
        }
    }
    bool pump() override { return target_.pump(); }
    bool take_input_reset() override { return target_.take_input_reset(); }
    bool can_resume() const override { return target_.can_resume(); }
    bool text_available() override { return target_.text_available(); }
    bool commit_text(const std::string& text) override { return enabled && target_.commit_text(text); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return target_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return target_.num_lock_state(); }
    bool enabled{false};

  private:
    KeySink& target_;
};
class App {
  public:
    App(const Options& options, KeySink& sink);
    PanelView view() const;
    // True only for a newly accepted keyboard key, so VR can provide haptics.
    bool down(unsigned pointer, double x, double y, double now);
    // True when an unpressed pointer enters a different keyboard key.
    bool move(unsigned pointer, double x, double y);
    // True when this pointer releases a captured keyboard key, even outside it.
    bool up(unsigned pointer, double x, double y);
    bool pointer_pressed(unsigned pointer) const {
        return keyboard_.pointer_pressed(pointer) || pressed_controls_.contains(pointer) ||
               settings_ui_.pressed(pointer);
    }
    void cancel_pointer(unsigned pointer);
    void cancel(bool discard_composition = true);
    bool tick(double now);
    void paint(double now);
    void show_settings();
    void back();
    bool scroll(unsigned pointer, double dx, double dy);
    bool pinned() const { return settings_.pinned; }
    void summon();
    void set_dragging(bool dragging);
    // Hiding the VR context releases keys before closing the input gate.
    void set_interaction_active(bool active);
    void report_status(const std::string& message);
    std::vector<PlacementAction> take_placement_actions();
    void apply(Selection selection);
    void reload();
    const fs::path& config_dir() const { return options_.config_dir; }
    bool quitting() const { return quit_; }
    bool take_recenter();
    bool dirty{true};
    PanelRenderer renderer;
    const Selection& selection() const { return settings_.active; }

  private:
    std::vector<Control> controls() const;
    void update_settings_ui();
    void activate(Selection selection);
    void save();
    void rebuild_cycle_entries();
    void action(const std::string& id);
    void refresh_typing();
    bool japanese_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_japanese();
    bool japanese() const;
    bool cjk_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_cjk();
    void choose_cjk(int index);
    bool unicode_mode() const;
    bool text_key(const Key& key, const std::set<int>& mods) const;
    int native_code(const Key& key, const std::set<int>& mods);
    bool flush_text();
    std::string pending_text_;
    struct TextRepeat {
        std::string text;
        double next;
    };
    std::map<unsigned, TextRepeat> text_repeats_;
    Options options_;
    Profiles profiles_;
    Settings settings_;
    Selection pending_, default_;
    std::vector<Selection> cycle_entries_;
    std::vector<Favorite> pending_favorites_;
    bool pending_hide_numpad_{};
    SettingsUi settings_ui_;
    std::unique_ptr<LanguageMap> keymap_;
    // The launch declaration is frozen; Reload cannot redefine the target.
    std::optional<Language> target_language_;
    InputGate gate_;
    KeyboardState keyboard_;
    JapaneseComposer composition_;
    std::unique_ptr<CjkComposer> cjk_;
    bool cjk_latin_{};
    bool japanese_latin_{};
    bool text_ready_{};
    std::map<unsigned, std::string> hovered_, pressed_controls_;
    std::string status_;
    bool settings_open_{false}, quit_{false}, recenter_{false};
    std::vector<PlacementAction> placement_actions_;
    std::map<unsigned, std::pair<double, double>> pointer_positions_;
    bool dragging_{};
    bool interaction_active_{true};
    bool backend_ready_{true}, backend_can_resume_{};
    std::optional<bool> native_num_requested_;
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, double duration);
} // namespace framekeyboard
