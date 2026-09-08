#pragma once

#include "imgui.h"
#include <algorithm>

// Shared timeline layout geometry — single source of truth for the renderer and the
// UI tests, so test coordinate helpers reference the real value instead of a copy.
//
// Each metric comes in two forms: one taking an explicit font size (pure, unit-testable at a
// simulated DPI) and one reading the frame's own. Every build and test config runs at 1× content
// scale, so a DPI mistake here is invisible to the suite unless the pure form is exercised
// directly — see tests/ui/test_timeline_layout.cpp.

namespace ofs::ui {

// Radius of an action's source dot. Font-relative so a dot keeps the same physical size at any font
// scale / DPI (≈8 px at the 18 px default).
inline float dotRadius(float fontSize) {
    return fontSize * 0.45f;
}
inline float dotRadius() { // call within a frame
    return dotRadius(ImGui::GetFontSize());
}

// Breathing room kept above pos=100 and below pos=0 so the end-point dots aren't clipped at the band
// edge — hence exactly one dot radius, which is what keeps the clearance right when DPI or the base
// font size changes the dots it protects. Capped at a quarter of the band so a thin Lanes row (many
// visible axes) still leaves the script line at least half the lane — without this cap the margin
// exceeds a short lane, inverting posToScreenY and pinning screenYToPos to 0.
inline float scriptLineVMargin(float bandHeight, float fontSize) {
    return std::min(dotRadius(fontSize), bandHeight * 0.25f);
}
inline float scriptLineVMargin(float bandHeight) { // call within a frame
    return scriptLineVMargin(bandHeight, ImGui::GetFontSize());
}

} // namespace ofs::ui
