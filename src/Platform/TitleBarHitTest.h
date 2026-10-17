#pragma once

#include <SDL3/SDL_video.h>

namespace ofs {

// The custom title bar's interactive geometry, in window points, published by the UI each frame.
struct TitleBarHitInfo {
    float height = 0.0f;       // title bar strip height
    float buttonsLeftX = 0.0f; // caption buttons start here; drag region is to the left
    float searchLeftX = 0.0f;  // interactive carve-out (search box): left edge
    float searchRightX = 0.0f; // interactive carve-out (search box): right edge
    bool enabled = false;      // true once the borderless custom title bar is active
};

// Classifies `point` (window points) for the borderless window's OS hit test: an edge-resize zone, the
// title-strip drag handle, or normal client area. `flags` is the window's live SDL state, read at hit-test
// time rather than cached per frame so a maximize/fullscreen transition takes effect immediately.
SDL_HitTestResult classifyTitleBarHit(const TitleBarHitInfo &info, SDL_WindowFlags flags, int width, int height,
                                      SDL_Point point);

} // namespace ofs
