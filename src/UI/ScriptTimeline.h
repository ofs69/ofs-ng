#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Format/AppSettings.h"
#include "UI/BandBar.h"
#include "imgui.h"
#include <cstdint>
#include <memory>

namespace ofs {

class VideoPlayer;
struct ScriptProject;
class EventQueue;
class WaveformRenderer;

class ScriptTimelineWindow {
  public:
    ScriptTimelineWindow();

    void render(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer, WaveformRenderer &waveform);
    [[nodiscard]] bool wasRegionClickedThisFrame() const { return m_regionClickedThisFrame; }

  private:
    struct ViewState {
        double visibleTime = 10.0;
        double targetVisibleTime = 10.0;
        double previousVisibleTime = 10.0;
        uint32_t zoomUpdateTime = 0;
    };

    struct EditState {
        // Phase 1: mouse pressed on a point — waiting for drag threshold or release
        bool hasDragCandidate = false;
        StandardAxis candidateAxis = StandardAxis::Count;
        double candidateAt = 0.0;
        int candidatePos = 0;

        // Phase 2: active drag
        bool isDragging = false;
        StandardAxis draggingAxis = StandardAxis::Count;
        double dragFromAt = 0.0;     // tracks current "from" time, updated each frame
        double originalDragAt = 0.0; // time at drag start, for alt snap-back
        int dragPos = 0;
        bool dragMoved = false;

        // Empty-area click — resolves to seek (release) or box-select (drag)
        bool emptyClickPending = false;
        double emptyClickTime = 0.0;
    };

    struct SelectionState {
        bool isSelecting = false;
        double absSelStart = 0.0;
        double relSelEnd = 0.0;
    };

    // Drag across left-strip rows to build a multi-axis editing group. anchorRow is the row the drag
    // started on (-1 = no drag in progress); the spanned run becomes the group each frame.
    struct StripDragState {
        int anchorRow = -1;
    };

    // One axis row's render inputs, resolved once by renderTimeline and threaded into renderStrip /
    // renderCurves. `isActive` is the lead axis; `inEditSet` is membership in the active edit group.
    struct AxisEntry {
        StandardAxis role = StandardAxis::Count;
        ImU32 color = 0;
        bool isActive = false;
        bool inEditSet = false;
    };

    void renderTimeline(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                        WaveformRenderer &waveform, const ImVec2 &pos, const ImVec2 &size);
    // Left strip: per-axis rows (label, lock/eye, group chips) plus their click/Ctrl-click/double-click/
    // drag interaction. Opens the shared ##timeline_ctx popup (latching ctxAxis) on a right-click.
    void renderStrip(const ScriptProject &project, EventQueue &eq, ImDrawList *drawList, const ImVec2 &pos,
                     const ImVec2 &size, const ImVec2 &stripPos, const ImVec2 &stripSize, const ImVec2 &curvePos,
                     const AxisEntry *stripEntries, int numRows, bool hasGroup);
    // Curve area: background gradient, waveform, grid, the per-axis polylines + active-axis dots, the
    // in-progress selection box, and the outer border. Read-only; all interaction stays in renderTimeline.
    void renderCurves(const ScriptProject &project, ImDrawList *drawList, WaveformRenderer &waveform,
                      const ImVec2 &pos, const ImVec2 &size, const ImVec2 &curvePos, const ImVec2 &curveSize,
                      double offsetTime, const AxisEntry *curveEntries, int curveCount, bool windowHovered);
    // The ##timeline_ctx popup body (axis lock/visibility/delete/add-region, view toggles, overlay
    // submenu). Begun and ended here; the matching OpenPopup calls live in the strip/curve interaction.
    void renderContextMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer);
    // The Scripting-overlay (Frame/Tempo) settings submenu, nested inside renderContextMenu.
    void renderOverlayMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) const;
    void renderOverlay(const ScriptProject &project, const ImVec2 &pos, const ImVec2 &size, double offsetTime) const;
    void renderPlayhead(VideoPlayer &videoPlayer, const ImVec2 &pos, const ImVec2 &size, double offsetTime) const;
    void renderRegionBar(VideoPlayer &videoPlayer, const ScriptProject &project, EventQueue &eq, const ImVec2 &barMin,
                         const ImVec2 &barMax, const ImVec2 &curvePos, const ImVec2 &curveSize, double offsetTime);

    static constexpr float kMinPanelH = 40.f;

    ViewState viewState;
    EditState editState;
    SelectionState selectionState;
    StripDragState stripDrag;
    ofs::ui::BandBarDragState regionDragState;
    // Axis the timeline context menu (##timeline_ctx) acts on, latched when the menu opens — from a
    // right-click on a strip row/curve or from the corner settings gear. Count == no axis section.
    StandardAxis ctxAxis = StandardAxis::Count;
    // Set when the context menu was opened from the corner gear (vs. a right-click). The gear sits at
    // the window's bottom edge, so its menu is anchored to grow upward; gearMenuAnchor is the screen
    // point the popup's bottom-left pins to (the gear's top-left).
    bool ctxFromGear = false;
    ImVec2 gearMenuAnchor{};
    int ctxRegionId = -1;
    double ctxRegionClickTime = 0.0;
    bool m_regionClickedThisFrame = false;
    // Region whose band color is mid-edit in the context menu. Only the first change of a picker
    // gesture snapshots undo, so dragging through the picker collapses into one undo step.
    int m_colorEditRegionId = -1;
};

} // namespace ofs
