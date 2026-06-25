#pragma once

#include "imgui.h"
#include <cmath>
#include <cstddef>

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

} // namespace ofs::util
