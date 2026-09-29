#pragma once

#include "instance.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include <csignal>

namespace framekeyboard {
struct Options {
    fs::path data_dir, config_dir, ei_socket;
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
    bool enabled{false};

  private:
    KeySink& target_;
};
class App {
  public:
    App(const Options& options, KeySink& sink);
    PanelView view() const;
    void down(unsigned pointer, double x, double y, double now);
    void move(unsigned pointer, double x, double y);
    void up(unsigned pointer, double x, double y);
    void cancel();
    bool tick(double now);
    void paint(double now);
    void show_settings();
    void show_placement();
    void summon();
    void set_dragging(bool dragging);
    void report_status(const std::string& message);
    std::vector<PlacementAction> take_placement_actions();
    void apply(Selection selection);
    void reload();
    bool quitting() const { return quit_; }
    bool take_recenter();
    bool dirty{true};
    PanelRenderer renderer;
    const Profiles& profiles() const { return profiles_; }
    const Selection& selection() const { return settings_.active; }

  private:
    std::vector<Control> controls() const;
    void action(const std::string& id);
    Options options_;
    Profiles profiles_;
    Settings settings_;
    Selection pending_;
    std::unique_ptr<LanguageMap> keymap_;
    InputGate gate_;
    KeyboardState keyboard_;
    std::map<unsigned, std::string> hovered_, pressed_controls_;
    std::string status_;
    bool settings_open_{false}, placement_open_{false}, quit_{false}, recenter_{false};
    std::vector<PlacementAction> placement_actions_;
    std::size_t favorite_index_{};
    bool dragging_{};
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, double duration);
} // namespace framekeyboard
