#pragma once
#include <imgui.h>
#include <imgui_internal.h> // GetStyleVarInfo

namespace ofs::style {

// Rebuild the unscaled (1×) themed base after theme::apply(), for DPI re-scaling.
//
// `applied` is the live ImGui style theme::apply() just wrote: its Colors[] and every ImGuiStyleVar_*
// field hold the themed values at 1×, but theme::apply() writes *only* those. The remaining size fields
// ImGuiStyle::ScaleAllSizes() touches — notably the internal ImGuiStyle::_MainScale — are left untouched,
// so they still carry the DPI scale baked in by the previous frame. Capturing `applied` verbatim as the
// DPI base (then ScaleAllSizes()-ing it again every theme apply) re-scales those fields each time and
// compounds them: (int)_MainScale grows by one per apply, which thickens the FrameBorderSize==0
// color-button border and the FIXME-DPI caret 1px at a time at any content scale > 1× (at exactly 1× the
// multiplier is 1.0, so nothing visibly drifts — which is why the bug only shows above 100% display
// scaling).
//
// Fix: start from `pristine` — the app's style at 1× DPI, captured once before any ScaleAllSizes() or
// theme apply — so all non-theme fields (including _MainScale == 1) reset to their true 1× value, then
// overlay the themed Colors[] and style-vars from `applied`. The result is a clean 1× themed base that
// ScaleAllSizes() can be applied to without drift, and it preserves any app-level (non-theme) style the
// pristine baseline carries.
inline ImGuiStyle unscaledThemeBase(const ImGuiStyle &applied, const ImGuiStyle &pristine) {
    ImGuiStyle base = pristine;
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        base.Colors[i] = applied.Colors[i];
    for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(i);
        auto *dst = static_cast<float *>(info->GetVarPtr(&base));
        const auto *src = static_cast<const float *>(info->GetVarPtr(const_cast<ImGuiStyle *>(&applied)));
        dst[0] = src[0];
        if (info->Count == 2)
            dst[1] = src[1];
    }
    return base;
}

} // namespace ofs::style
