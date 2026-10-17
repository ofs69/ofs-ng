#include "TitleBarHitTest.h"

namespace ofs {

SDL_HitTestResult classifyTitleBarHit(const TitleBarHitInfo &info, SDL_WindowFlags flags, int width, int height,
                                      SDL_Point point) {
    // A fullscreen window covers its display edge to edge. A caption drag would pull it off the display
    // while it stays flagged fullscreen, and there is nothing to resize to.
    if (!info.enabled || (flags & SDL_WINDOW_FULLSCREEN) != 0)
        return SDL_HITTEST_NORMAL;

    constexpr int kBorder = 6; // edge-resize thickness, window points
    const int x = point.x;
    const int y = point.y;

    if ((flags & SDL_WINDOW_MAXIMIZED) == 0) {
        const bool left = x < kBorder;
        const bool right = x >= width - kBorder;
        const bool top = y < kBorder;
        const bool bottom = y >= height - kBorder;
        if (top && left)
            return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right)
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left)
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right)
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left)
            return SDL_HITTEST_RESIZE_LEFT;
        if (right)
            return SDL_HITTEST_RESIZE_RIGHT;
        if (top)
            return SDL_HITTEST_RESIZE_TOP;
        if (bottom)
            return SDL_HITTEST_RESIZE_BOTTOM;
    }

    // Interactive carve-out (e.g. the command-palette search box): stays normal client area so ImGui
    // receives the click instead of the OS starting a window drag.
    if (y < static_cast<int>(info.height) && info.searchRightX > info.searchLeftX &&
        x >= static_cast<int>(info.searchLeftX) && x < static_cast<int>(info.searchRightX))
        return SDL_HITTEST_NORMAL;

    // Title bar strip, left of the caption buttons, is the drag handle. The OS maps this to a caption,
    // giving move + double-click-maximize + snap for free.
    if (y < static_cast<int>(info.height) && x < static_cast<int>(info.buttonsLeftX))
        return SDL_HITTEST_DRAGGABLE;

    return SDL_HITTEST_NORMAL;
}

} // namespace ofs
