#pragma once
#include "imgui.h"
#include <algorithm>

namespace ofs::ui {

// Pixel dimensions of the video render target.
struct RenderPixelSize {
    int w = 1;
    int h = 1;
};

// Size the video render target for an on-screen image of `displaySize` display units.
//
// The image is an ImGui quad, so its size is in display units, but the target is allocated in
// framebuffer pixels — and the two differ by the viewport's FramebufferScale wherever the desktop
// scales fractionally (Wayland at 150%) or the display is Retina. Sizing the target in display units
// hands the GPU fewer texels than the quad covers pixels and it magnifies the difference, so the video
// is softest on exactly the displays that could resolve the most detail. `resolutionScale` is the
// user's deliberate quality reduction and multiplies on top of that; the result is capped at the source
// resolution, above which the extra texels carry no picture.
inline RenderPixelSize videoRenderPixels(const ImVec2 &displaySize, float resolutionScale, const ImVec2 &fbScale,
                                         int sourceWidth, int sourceHeight) {
    const auto axis = [](float units, float scale, int source) {
        return std::max(1, std::min(static_cast<int>(units * scale), source));
    };
    return {axis(displaySize.x * resolutionScale, fbScale.x, sourceWidth),
            axis(displaySize.y * resolutionScale, fbScale.y, sourceHeight)};
}

} // namespace ofs::ui
