#pragma once

#include "layout.hpp"

#include <cstddef>
#include <vector>

namespace framekeyboard {

// Extended key collision (docs/configuration.md): where a key takes the
// laser, its own bounds grown into the gaps it shares with its neighbours,
// which each take half. A gap is shared when it is at most half again the
// layout's usual gap that way, across or down; a wider one separates blocks
// of keys, such as the function row from the number row, and stays empty.
// A wider gap closed off on all four sides, as beside a tall ISO or JIS
// Enter, is split between the two keys facing across it. Areas never
// overlap. Layout coordinates.
struct KeyArea {
    bool operator==(const KeyArea&) const = default;
    std::size_t key{}; // Index into Layout::keys.
    Rect bounds;
};
// One or more areas per key: a key beside such a closed-off gap can take an
// L shape. `absent` marks keys left out, such as hidden ones, by index into
// Layout::keys; it may be empty.
std::vector<KeyArea> key_areas(const Layout& layout, const std::vector<bool>& absent = {});
// A page of flick keys, which keeps its gaps. The English page of the flick
// layout is not one: its only flick key is 「あA1」.
bool flick_page(const Layout& layout);

} // namespace framekeyboard
