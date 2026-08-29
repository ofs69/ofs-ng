#pragma once
#include "imgui.h"
#include <algorithm>

namespace ofs::ui {

// A glViewport/glScissor rect: framebuffer pixels, origin at the bottom-left.
struct GlRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Convert a screen-space rect (ImGui display units, y down) into glViewport/glScissor arguments
// (framebuffer pixels, y up).
//
// The two spaces differ by ImDrawData::FramebufferScale, which is 1 only when the window's logical
// size equals its pixel size. Under fractional desktop scaling it is not: on Wayland at 150% SDL
// reports a logical window size and a 1.5× pixel size, so display units are points. ImGui's GL backend
// scales its own scissor rects by the same factor (imgui_impl_opengl3.cpp) but leaves the viewport and
// scissor untouched across a user draw callback, so a callback issuing raw GL must convert here. Skip
// the factor and the output lands at 1/scale of its intended offset from the framebuffer's bottom-left
// corner — an error that grows with distance from that corner, and vanishes at exactly 1×.
inline GlRect glRectFromScreen(const ImVec2 &min, const ImVec2 &size, const ImDrawData &dd) {
    const ImVec2 fb = dd.FramebufferScale;
    // A user callback is dispatched before the backend's empty-clip-rect skip, so a fully clipped
    // window can reach us with a degenerate rect; a negative extent is GL_INVALID_VALUE.
    const float w = std::max(0.f, size.x);
    const float h = std::max(0.f, size.y);
    return {static_cast<int>((min.x - dd.DisplayPos.x) * fb.x),
            static_cast<int>((dd.DisplaySize.y - (min.y - dd.DisplayPos.y) - h) * fb.y), static_cast<int>(w * fb.x),
            static_cast<int>(h * fb.y)};
}

} // namespace ofs::ui
