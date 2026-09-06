#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>

namespace ofs::util {

// Golden-ratio HSV color generator — visually distinct, pleasant, deterministic per index.
// The hue advances by φ per index, so consecutive items land on widely separated, non-repeating
// hues; deriving the hue from the index (not session-static state) means the Nth chapter/region of
// any project always gets the same color. s=0.65, v=0.70, a=220 matches the palette from ofs_old.
// Stateless math (no ImGui frame required) — safe to call from a main-thread service handler.
inline ImU32 goldenRatioColor(std::size_t index) {
    constexpr double kGolden = 0.618033988749895;
    constexpr double kBaseHue = 0.1;
    const double h = kBaseHue + kGolden * static_cast<double>(index);
    const auto hue = static_cast<float>(h - std::floor(h));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 0.70f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32({r, g, b, 220.0f / 255.0f});
}

// Lowest golden-ratio color (from `seed`) not already worn by an item in `existing`, where `colorOf`
// reads an item's color. Deriving the index from the item *count* instead breaks after a delete —
// removing the 2nd of 3 chapters frees index 1 while index 2 stays live, so the next chapter re-picks
// index 2 and duplicates its sibling. Probing for a free color keeps siblings distinct and reuses the
// index a delete freed. N items wear at most N colors, so one of the N+1 candidates is always free.
template <typename Range, typename ColorOf>
[[nodiscard]] inline ImU32 nextDistinctColor(std::size_t seed, const Range &existing, ColorOf colorOf) {
    const auto count = static_cast<std::size_t>(std::ranges::distance(existing));
    for (std::size_t i = 0; i < count; ++i) {
        const ImU32 candidate = goldenRatioColor(seed + i);
        if (std::ranges::none_of(existing, [&](const auto &item) { return colorOf(item) == candidate; }))
            return candidate;
    }
    return goldenRatioColor(seed + count);
}

} // namespace ofs::util
