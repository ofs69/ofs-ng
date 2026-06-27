#pragma once

#include "Core/StandardAxis.h"
#include "UI/Theme.h"
#include "imgui.h"

// Axis → theme-color lookups. These live in the UI layer (not Core/StandardAxis.h) because they reach
// into the themed AppCol_Axis* palette; Core must not depend on UI. The arithmetic maps a StandardAxis
// index onto the AppCol_AxisL0 / AppCol_AxisDimL0 blocks, which are laid out in StandardAxis order.

namespace ofs {

namespace detail {
// Valid axis index in [0, Count) — the shared bounds guard for the lookups below. Returns -1 for an
// out-of-range axis (e.g. StandardAxis::Count), so each accessor falls back to its neutral color.
inline int axisColorIndex(StandardAxis a) noexcept {
    const int idx = static_cast<int>(a);
    return (idx >= 0 && idx < static_cast<int>(StandardAxis::Count)) ? idx : -1;
}
} // namespace detail

inline ImU32 standardAxisColor(StandardAxis a) noexcept {
    const int idx = detail::axisColorIndex(a);
    return idx < 0 ? IM_COL32(128, 128, 128, 255) : ofs::theme::GetColorU32(static_cast<AppCol>(AppCol_AxisL0 + idx));
}

inline ImVec4 standardAxisColorVec4(StandardAxis a) noexcept {
    const int idx = detail::axisColorIndex(a);
    return idx < 0 ? ImVec4{0.5, 0.5, 0.5, 1.0}
                   : ofs::theme::GetStyleColorVec4(static_cast<AppCol>(AppCol_AxisL0 + idx));
}

inline ImU32 standardAxisColorDim(StandardAxis a) noexcept {
    const int idx = detail::axisColorIndex(a);
    return idx < 0 ? IM_COL32(70, 70, 70, 255) : ofs::theme::GetColorU32(static_cast<AppCol>(AppCol_AxisDimL0 + idx));
}

inline ImVec4 standardAxisColorDimVec4(StandardAxis a) noexcept {
    const int idx = detail::axisColorIndex(a);
    return idx < 0 ? ImVec4{0.27f, 0.27f, 0.27f, 1.0f}
                   : ofs::theme::GetStyleColorVec4(static_cast<AppCol>(AppCol_AxisDimL0 + idx));
}

} // namespace ofs
