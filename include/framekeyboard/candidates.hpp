#pragma once

#include "glide.hpp"
#include "layout.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace framekeyboard {
// What the panel draws of the candidate strip.
struct CandidateStripView {
    bool operator==(const CandidateStripView&) const = default;
    struct Chip {
        bool operator==(const Chip&) const = default;
        std::string text;
        Rect bounds; // Panel coordinates, scrolled; drawn within the strip's area.
        bool selected{}, hovered{};
        bool listed{}; // In the open list under the row, and drawn within it.
    };
    Rect area;
    std::optional<Rect> list;  // While the list is open.
    std::optional<Rect> thumb; // The open list's scrollbar, while it scrolls.
    std::vector<Chip> chips;   // Only those at least partly in view.
    // For a reading typed since: dimmed, and a release picks nothing.
    bool stale{};
};

// Japanese candidates in one row of chips (docs/japanese.md). A laser drag
// scrolls the row sideways and a quick one glides on, like the emoji grid; a
// chip picks on release over the chip it was pressed on, if the laser hardly
// moved. A press on a gliding row only catches it. Panel coordinates.
//
// With a list area the strip is open, as a phone's ∨ opens it: the row holds
// the chips that fit from the start and stops scrolling, and the rest wrap
// into lines below it, which scroll up and down instead.
class CandidateStrip {
  public:
    // A chip label's width in pixels.
    using Measure = std::function<double(const std::string&)>;
    struct Event {
        bool redraw{};
        std::optional<int> chosen;
    };
    static constexpr double chip_height = 38, chip_padding = 16, chip_spacing = 8, min_chip = 50;
    // How wide the row of these chips is, end to end; for the texts shown,
    // from the widths already measured.
    static double row_width(const std::vector<std::string>& texts, const Measure& measure);
    double row_width_of(const std::vector<std::string>& texts, const Measure& measure) const {
        return texts == texts_ ? width_ : row_width(texts, measure);
    }
    // A new list starts at its beginning; the same one keeps its place and
    // brings a newly selected chip into view. Opening or closing starts again.
    // `list` is the most room the open list may take; with `fit` it ends
    // after its last line when that comes sooner.
    void set(Rect area, std::vector<std::string> texts, int selected, bool stale, const Measure& measure,
             std::optional<Rect> list = {}, bool fit = false);
    void clear();
    bool shown() const { return !texts_.empty(); }
    bool contains(double x, double y) const {
        return shown() && (area_.contains(x, y) || (list_ && list_->contains(x, y)));
    }
    bool open() const { return shown() && list_.has_value(); }
    Event down(unsigned pointer, double x, double y, double now);
    Event move(unsigned pointer, double x, double y, double now);
    Event up(unsigned pointer, double x, double y, double now);
    Event tick(double now);
    // Stick or wheel notches; positive scrolls toward later candidates.
    bool scroll(double x, double y, double steps);
    std::optional<ScrollRoom> scroll_room(double x, double y) const;
    void cancel_pointer(unsigned pointer);
    void cancel();
    bool pressed(unsigned pointer) const { return press_ && press_->pointer == pointer; }
    bool gliding() const { return glide_.active(); }
    CandidateStripView view() const;
    double scroll_offset() const { return scroll_; }
    double scroll_limit() const;

  private:
    struct Press {
        unsigned pointer{};
        double at{}, scroll{}; // `at`: along the scroll axis, y while open.
        int chip{-1};          // -1: the gap between chips.
        bool dragging{}, caught{};
    };
    int chip_at(double x, double y) const;
    Rect chip_bounds(std::size_t index) const;
    // A still row ends on whole chips, so none is cut before ∨; while it is
    // dragged or glides, chips slide in cut at its edges. One wider than
    // the row shows cut either way.
    bool moving() const { return (press_ && press_->dragging) || glide_.active(); }
    bool shows(std::size_t index) const;
    void reveal(int index);
    void place();
    // The row's chip tops; the open list's lines follow at this pitch.
    double row_top() const { return area_.y + (area_.height - chip_height) / 2; }
    static constexpr double line_pitch = chip_height + chip_spacing;
    // A line's chip top before scrolling: the list's first line sits as far
    // below its top as it would right under the row, wherever the list is.
    double line_top(int line) const {
        return line == 0 ? row_top()
                         : list_->y + row_top() + line_pitch - (area_.y + area_.height) +
                               (line - 1) * line_pitch;
    }
    Rect area_{};
    std::optional<Rect> list_, requested_list_;
    bool fit_{};
    // Each chip's place while open: its line (0, the row) and x in that line.
    std::vector<int> lines_;
    std::vector<double> line_starts_;
    int line_count_{};
    std::vector<std::string> texts_;
    std::vector<double> starts_, widths_; // Strip x of each chip.
    double width_{};                      // The whole row.
    int selected_{-1};
    bool stale_{};
    double scroll_{};
    std::optional<Press> press_;
    Glide glide_;
    std::map<unsigned, int> hovered_;
    std::map<unsigned, std::pair<double, double>> positions_;
};
} // namespace framekeyboard
