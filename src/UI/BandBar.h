#pragma once

#include "imgui.h"
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace ofs::ui {

// Width (px) of the left/right resize-grab zones at each band's edges. Shared so UI
// tests can target the zone instead of duplicating the literal.
inline constexpr float kBandBarEdgeW = 6.0f;

// Vertical padding (px) above and below the band fill inside the bar.
inline constexpr float kBandBarPad = 2.0f;

// Snap distance (px): while dragging a band edge/body — or a bookmark — within this many pixels of the
// playhead, the dragged time snaps to the exact playhead time. Shared so the chapter band and the
// bookmark drag snap identically.
inline constexpr float kBandBarSnapPx = 8.0f;

// Height (px) a band bar must be to vertically fit its centered text label at the
// current font size. Both the timeline region bar and the video chapter/bookmark bar
// derive their height from this so labels are never clipped when the font scales.
// Must be called within a frame (reads ImGui::GetFontSize()).
inline float bandBarHeight() {
    return ImGui::GetFontSize() + 2.0f * kBandBarPad + 4.0f;
}

struct BandItem {
    double startTime;
    double endTime;
    ImU32 color;
    std::string_view name; // displayed centered if it fits; empty = no label
    bool selected = false; // when true, draws a bright highlight outline (e.g. the selected region)
    bool hatched = false;  // when true, fills with diagonal stripes instead of solid (region bands,
                           // so they read as visually distinct from solid chapter bands)
};

struct BandBarDragState {
    enum class Mode { None, ResizeLeft, ResizeRight, Move };

    // Active drag
    Mode mode = Mode::None;
    int dragIdx = -1;
    double previewStart = 0.0;
    double previewEnd = 0.0;
    double originalStart = 0.0; // position at drag start, used for alt snap-back
    double originalEnd = 0.0;
    double dragTimeAtStart = 0.0; // time under cursor when drag began (Move mode)
    bool endedThisFrame = false;  // true on the one frame the drag is released

    // Click-vs-drag candidate (set on mouse-down, promoted on drag threshold)
    bool hasDragCandidate = false;
    int candidateIdx = -1;
    Mode candidateMode = Mode::None;
    double candidateStart = 0.0;
    double candidateEnd = 0.0;
    double candidateDragTime = 0.0;
};

struct BandBarCallbacks {
    // Called every frame while dragging (for live preview side-effects).
    // idx = band index, newStart/newEnd = constrained times. Null = no-op.
    std::function<void(int, double, double)> onDragUpdate;
    // Called once on mouse release with final constrained times. Null = no-op.
    std::function<void(int, double, double)> onDragEnd;
    // Left-click (no shift, drag did not just end) on a band. Null = no-op.
    std::function<void(int)> onClick;
    // Right-click on a band. Second arg = click time. Null = no-op.
    std::function<void(int, double)> onRightClick;
    // Right-click on empty bar area. Null = no-op.
    std::function<void(double)> onEmptyRightClick;
};

// Draws and handles interaction for a horizontal bar of colored time-range bands.
//
// bands        — sorted by startTime; read-only (drag preview is kept in dragState)
// dragState    — persisted across frames by caller
// playheadTime — used for snap-to-playhead on edge resize
// minTime/maxTime — global clamp range (typically 0 / duration)
// minDur       — minimum band width in seconds
// toX / toTime — coordinate conversion provided by caller
// id: label for the bar's addressable ImGui item (registered via ItemAdd so UI tests
// can anchor to its exact rect). Non-interactive — it does not consume input, so the
// manual hit-testing below is unaffected. Must be unique within the host window.
// suppressNewInteraction: when true, Phase 3 (hover/click) is skipped so the caller
// can give a higher-priority overlay (e.g. bookmarks) exclusive access to clicks.
void drawBandBar(ImDrawList *dl, ImVec2 barMin, ImVec2 barMax, std::span<const BandItem> bands,
                 BandBarDragState &dragState, double playheadTime, double minTime, double maxTime, double minDur,
                 const std::function<float(double)> &toX, const std::function<double(float)> &toTime,
                 const BandBarCallbacks &callbacks, const char *id, bool suppressNewInteraction = false);

} // namespace ofs::ui
