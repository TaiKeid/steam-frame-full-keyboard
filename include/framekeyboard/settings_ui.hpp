#pragma once

#include "panel.hpp"

namespace framekeyboard {
struct SettingChoice {
    std::string id, name;
    // Disabled choices stay listed, dimmed, so the reason can be seen.
    bool enabled{true};
};
struct SettingField {
    std::string id, label, value;
    ControlStyle style{ControlStyle::Dropdown};
    bool checked{};
    std::vector<SettingChoice> choices{};
    // Every row puts its label at the left edge and its control at the right edge.
    bool enabled{true};
    // Dropdowns use this kind for choosing a value; their row opens generically.
    ControlAction action{};
    // A smaller second line under the label, dimmed with the field.
    std::string note{};
    // A dropdown whose choices are short, leaving its label more room.
    bool narrow{};
    // A Note row that describes rather than warns: the legend colour, not amber.
    bool plain{};
};
struct SettingsCard {
    std::string id, title;
    std::vector<SettingField> fields;
};
// Generic card/scroll state. App supplies the real fields; tests can supply extra
// cards and rows without adding fake preferences to production configuration.
class SettingsUi {
  public:
    static constexpr Rect viewport{18, 78, 1564, 476};
    void set_cards(std::vector<SettingsCard> cards);
    void append(PanelView& view) const;
    void reset();
    void close_popup();
    bool popup_open() const { return !open_field_.empty(); }
    // ID of the field whose dropdown is open, or empty.
    const std::string& open_field() const { return open_field_; }
    void toggle(const std::string& field);
    bool scroll(double x, double y, double dx, double dy);
    // Whether scroll() could move anything at this point: an open list longer
    // than its popup, an overflowing card, or cards wider than the panel.
    bool scrolls_at(double x, double y) const;
    // A press on a scrollbar captures it at once. A press elsewhere on the
    // cards becomes a touch-like drag once it moves past a small dead zone;
    // until then the control under it keeps its press.
    bool down(unsigned pointer, double x, double y);
    bool move(unsigned pointer, double x, double y);
    bool up(unsigned pointer);
    // Stick and wheel steps set where the cards are going; tick() eases
    // them there. True while anything moved.
    bool tick(double now);
    bool pressed(unsigned pointer) const { return capture_ && capture_->pointer == pointer; }
    void cancel_pointer(unsigned pointer);
    void cancel();

  private:
    Rect card_bounds(std::size_t index) const;
    Rect body(std::size_t index) const;
    Rect field_bounds(std::size_t card, std::size_t field) const;
    double vertical_limit(std::size_t card) const;
    double horizontal_limit() const;
    const SettingField* open_field_definition() const;
    std::optional<Rect> popup_bounds() const;
    struct Scrollbar {
        std::string id;
        Rect track, thumb;
        double limit{}, offset{};
        std::size_t card{};
        bool horizontal{};
    };
    std::vector<Scrollbar> scrollbars() const;
    std::vector<SettingsCard> cards_;
    // Shown offsets, and the targets that steps ease them toward. Drags and
    // scrollbars move both at once.
    std::map<std::string, double> vertical_, vertical_target_;
    double horizontal_{}, horizontal_target_{};
    double last_tick_{-1};
    struct Drag {
        unsigned pointer{};
        double x{}, y{}, horizontal{}, vertical{};
        std::optional<std::size_t> card; // the card under the press, if it scrolls
        bool active{}, sideways{};
    };
    std::optional<Drag> drag_;
    std::string open_field_;
    int first_choice_{};
    double choice_remainder_{};
    struct Capture {
        unsigned pointer;
        Scrollbar bar;
        double start{}, offset{};
    };
    std::optional<Capture> capture_;
};
} // namespace framekeyboard
