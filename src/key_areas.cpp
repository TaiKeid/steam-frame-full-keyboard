#include "framekeyboard/key_areas.hpp"

#include "framekeyboard/config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace framekeyboard {
namespace {
constexpr double none = std::numeric_limits<double>::infinity();
// Layout coordinates carry decimals such as 54.4: edges this close meet.
constexpr double slack = 1e-6;

double right(const Rect& r) {
    return r.x + r.width;
}
double bottom(const Rect& r) {
    return r.y + r.height;
}
bool overlap(double a0, double a1, double b0, double b1) {
    return a0 < b1 - slack && b0 < a1 - slack;
}
bool intersects(const Rect& a, const Rect& b) {
    return overlap(a.x, right(a), b.x, right(b)) && overlap(a.y, bottom(a), b.y, bottom(b));
}
// Down as across: the pocket pass runs on keys with x and y swapped.
Rect swapped(const Rect& r) {
    return {r.y, r.x, r.height, r.width};
}
std::vector<Rect> swapped(const std::vector<Rect>& rects) {
    std::vector<Rect> result;
    result.reserve(rects.size());
    for (const auto& r : rects) {
        result.push_back(swapped(r));
    }
    return result;
}

// The gap from each side of a key to the nearest key facing that side.
struct Gaps {
    double left{none}, right{none}, top{none}, bottom{none};
};
std::vector<Gaps> facing_gaps(const std::vector<Rect>& keys) {
    std::vector<Gaps> result(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& r = keys[i];
        auto& gaps = result[i];
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const auto& o = keys[j];
            if (i == j) {
                continue;
            }
            if (overlap(r.y, bottom(r), o.y, bottom(o))) { // In its row.
                if (right(o) <= r.x + slack) {
                    gaps.left = std::min(gaps.left, std::max(0.0, r.x - right(o)));
                } else if (o.x >= right(r) - slack) {
                    gaps.right = std::min(gaps.right, std::max(0.0, o.x - right(r)));
                }
            }
            if (overlap(r.x, right(r), o.x, right(o))) { // Above or below it.
                if (bottom(o) <= r.y + slack) {
                    gaps.top = std::min(gaps.top, std::max(0.0, r.y - bottom(o)));
                } else if (o.y >= bottom(r) - slack) {
                    gaps.bottom = std::min(gaps.bottom, std::max(0.0, o.y - bottom(r)));
                }
            }
        }
    }
    return result;
}
// The most common of the gaps, to a tenth of a unit; 0 with none. Only
// gaps narrower than half their key count: with few keys, as in the emoji
// picker, the gaps across its grid would outnumber those between keys.
double usual_gap(const std::vector<Gaps>& gaps, const std::vector<Rect>& keys, double Gaps::* side,
                 double Rect::* size) {
    std::map<long long, int> counts;
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        if (const double gap = gaps[i].*side; gap < keys[i].*size / 2) {
            ++counts[std::llround(gap * 10)];
        }
    }
    long long usual = 0;
    int most = 0;
    for (const auto& [gap, count] : counts) {
        if (count > most) {
            usual = gap;
            most = count;
        }
    }
    return static_cast<double>(usual) / 10;
}

// Gaps across wider than `limit`, but closed off above and below by keys at
// most `limit_down` away, whose areas meet all along them: such a pocket is
// split between the two keys facing across it, each up to its middle and
// down to the areas above and below. Adds those halves to `extra` as
// (key, rect), in the same swapped or unswapped frame as `keys`.
void split_pockets(const std::vector<Rect>& keys, const std::vector<Rect>& grown, double limit,
                   double limit_down, std::vector<std::pair<std::size_t, Rect>>& extra) {
    // How far the areas on one side of a strip reach toward it, if keys
    // there close it off: they cover its width but for usual gaps.
    auto closed = [&](const Rect& strip, bool above) -> std::optional<double> {
        std::vector<std::pair<double, double>> spans;
        double edge = above ? -none : none;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto& o = keys[k];
            if (!overlap(o.x, right(o), strip.x, right(strip))) {
                continue;
            }
            const double distance = above ? strip.y - bottom(o) : o.y - bottom(strip);
            if (distance < -slack || distance > limit_down + slack) {
                continue;
            }
            spans.emplace_back(o.x, right(o));
            edge = above ? std::max(edge, bottom(grown[k])) : std::min(edge, grown[k].y);
        }
        std::sort(spans.begin(), spans.end());
        double reach = strip.x;
        for (const auto& [from, to] : spans) {
            if (from > reach + limit + slack) {
                return {};
            }
            reach = std::max(reach, to);
        }
        if (spans.empty() || reach < right(strip) - limit - slack) {
            return {};
        }
        return edge;
    };
    for (std::size_t a = 0; a < keys.size(); ++a) {
        for (std::size_t b = 0; b < keys.size(); ++b) {
            const auto &left = keys[a], &right_key = keys[b];
            const double gap = right_key.x - right(left);
            // No wider than the narrower key: a hole in a custom layout's
            // middle stays empty.
            if (a == b || gap <= limit + slack || gap > std::min(left.width, right_key.width)) {
                continue;
            }
            const double top = std::max(left.y, right_key.y);
            const double end = std::min(bottom(left), bottom(right_key));
            if (end - top <= slack) {
                continue;
            }
            const Rect strip{right(left), top, gap, end - top};
            if (std::any_of(keys.begin(), keys.end(),
                            [&](const Rect& o) { return intersects(o, strip); })) {
                continue; // Not facing each other: a key is between them.
            }
            const auto upper = closed(strip, true), lower = closed(strip, false);
            if (!upper || !lower) {
                continue;
            }
            const double middle = right(left) + gap / 2;
            const double from = std::min(*upper, strip.y), to = std::max(*lower, bottom(strip));
            if (const double start = right(grown[a]); middle - start > slack) {
                extra.emplace_back(a, Rect{start, from, middle - start, to - from});
            }
            if (const double stop = grown[b].x; stop - middle > slack) {
                extra.emplace_back(b, Rect{middle, from, stop - middle, to - from});
            }
        }
    }
}
// Joins an extra area to its key's main one when together they make one
// rectangle, so its outline stays a single square.
bool join(Rect& main, const Rect& extra) {
    const auto near = [](double a, double b) { return std::abs(a - b) <= slack; };
    if (near(extra.y, main.y) && near(extra.height, main.height)) {
        if (near(extra.x, right(main))) {
            main.width += extra.width;
            return true;
        }
        if (near(right(extra), main.x)) {
            main.x = extra.x;
            main.width += extra.width;
            return true;
        }
    }
    if (near(extra.x, main.x) && near(extra.width, main.width)) {
        if (near(extra.y, bottom(main))) {
            main.height += extra.height;
            return true;
        }
        if (near(bottom(extra), main.y)) {
            main.y = extra.y;
            main.height += extra.height;
            return true;
        }
    }
    return false;
}
} // namespace

std::vector<KeyArea> key_areas(const Layout& layout, const std::vector<bool>& absent) {
    std::vector<std::size_t> index;
    std::vector<Rect> keys;
    for (std::size_t i = 0; i < layout.keys.size(); ++i) {
        if (i >= absent.size() || !absent[i]) {
            index.push_back(i);
            keys.push_back(layout.keys[i].bounds);
        }
    }
    const auto gaps = facing_gaps(keys);
    const double across = 1.5 * usual_gap(gaps, keys, &Gaps::right, &Rect::width);
    const double down = 1.5 * usual_gap(gaps, keys, &Gaps::bottom, &Rect::height);
    auto half = [](double gap, double limit) { return gap <= limit + slack ? gap / 2 : 0; };
    std::vector<Rect> grown;
    grown.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& r = keys[i];
        const auto& g = gaps[i];
        const double left = half(g.left, across), right_side = half(g.right, across);
        const double top = half(g.top, down), bottom_side = half(g.bottom, down);
        grown.push_back(
            {r.x - left, r.y - top, r.width + left + right_side, r.height + top + bottom_side});
    }
    // Pockets across, then down in the swapped frame, both measured against
    // the areas grown so far.
    std::vector<std::pair<std::size_t, Rect>> extra;
    split_pockets(keys, grown, across, down, extra);
    std::vector<std::pair<std::size_t, Rect>> extra_down;
    split_pockets(swapped(keys), swapped(grown), down, across, extra_down);
    for (const auto& [key, rect] : extra_down) {
        extra.emplace_back(key, swapped(rect));
    }
    std::vector<KeyArea> result;
    result.reserve(keys.size() + extra.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        result.push_back({index[i], grown[i]});
    }
    for (const auto& [key, rect] : extra) {
        // A custom layout's odd geometry must not make two keys share a point.
        if (std::any_of(result.begin(), result.end(),
                        [&](const KeyArea& area) { return intersects(area.bounds, rect); })) {
            continue;
        }
        if (!join(result[key].bounds, rect)) {
            result.push_back({index[key], rect});
        }
    }
    return result;
}

bool flick_page(const Layout& layout) {
    return std::any_of(layout.keys.begin(), layout.keys.end(), [](const Key& key) {
        return key.action_kind == ActionKind::Flick && key.action != flick_mode_slot;
    });
}

} // namespace framekeyboard
