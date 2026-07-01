#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Format/AppSettings.h"
#include "UI/BandBar.h"
#include "Util/SmoothedFloat.h"
#include "imgui.h"
#include <array>
#include <cstdint>
#include <memory>

namespace ofs {

class VideoPlayer;
struct ScriptProject;
class EventQueue;
class WaveformRenderer;

// Resolved per-frame lane geometry for the script-line/strip band (Lanes layout). Built once at the top of
// renderTimeline and read by renderStrip, renderScriptLines, and the script-line interaction so every surface
// agrees on lane height/scroll. Overlay leaves it at its band-filling default (laneH == scriptLineSize.y,
// no scroll). See the lane-geometry helpers in ScriptTimeline.cpp.
struct LaneLayout {
    bool lanes = false;      // Lanes mode active
    int count = 0;           // visible lane count (= showInStrip axes)
    ImVec2 scriptLinePos{};  // full script-line band top-left
    ImVec2 scriptLineSize{}; // full script-line band size
    float laneH = 0.f;       // per-lane height (>= minLaneHeight() once scrolling)
    float scroll = 0.f;      // applied vertical scroll offset, clamped to [0, maxScroll]
    float maxScroll = 0.f;   // content overflow; > 0 means the lane scrollbar is shown
};

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
        // Last spanned run pushed as a group this drag, so an unchanged span isn't re-pushed every frame.
        // -1 = nothing pushed yet.
        int spanLo = -1;
        int spanHi = -1;
    };

    // One axis row's render inputs, resolved once by renderTimeline and threaded into renderStrip /
    // renderScriptLines. `isActive` is the lead axis; `inEditSet` is membership in the active edit group.
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
                     const ImVec2 &size, const ImVec2 &stripPos, const ImVec2 &stripSize, const ImVec2 &scriptLinePos,
                     const AxisEntry *stripEntries, int numRows, bool hasGroup);
    // Script-line area: background gradient, waveform, grid, the per-axis polylines + active-axis dots, the
    // in-progress selection box, and the outer border. Read-only; all interaction stays in renderTimeline.
    // `lanes` splits each axis into its own horizontal row (grid + script line + dots per lane); Overlay (false)
    // z-stacks every axis into the shared full-height 0-100 band.
    void renderScriptLines(const ScriptProject &project, ImDrawList *drawList, WaveformRenderer &waveform,
                           const ImVec2 &pos, const ImVec2 &size, const ImVec2 &scriptLinePos,
                           const ImVec2 &scriptLineSize, double offsetTime, const AxisEntry *scriptLineEntries,
                           int scriptLineCount, bool lanes, bool windowHovered);
    // Draw + drive the Lanes vertical scrollbar (a thumb on the script line's right edge). Backed by a real
    // ##lane_scrollbar InvisibleButton submitted after the lane buttons (which set AllowOverlap in the
    // overflow case), so its hover/active state is id-tracked and ui-tests can drag it by id. Returns true
    // while the cursor is over the track so the caller suppresses script-line gestures there; a no-op
    // returning false unless the lanes overflow (laneLayout_.maxScroll > 0).
    bool renderLaneScrollbar(ImDrawList *drawList, const ImVec2 &scriptLinePos, const ImVec2 &scriptLineSize,
                             float mouseY);
    // The ##timeline_ctx popup body (axis lock/visibility/delete/add-region, view toggles, overlay
    // submenu). Begun and ended here; the matching OpenPopup calls live in the strip/script-line interaction.
    void renderContextMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer);
    // The Scripting-overlay (Frame/Tempo) settings submenu, nested inside renderContextMenu.
    void renderOverlayMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) const;
    // Body of the timeline-settings modal raised from the corner gear via showCustomModal (the
    // ModalManager owns the chrome). A tabular form of the timeline's view settings (layout, source
    // points, audio waveform, scripting overlay) — the same settings the right-click context menu
    // exposes as quick toggles, gathered into one discoverable dialog. Returns true when the modal
    // should close (Close button / Escape).
    bool renderSettingsBody(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) const;
    void renderOverlay(const ScriptProject &project, const ImVec2 &pos, const ImVec2 &size, double offsetTime) const;
    void renderPlayhead(VideoPlayer &videoPlayer, const ImVec2 &pos, const ImVec2 &size, double offsetTime) const;
    void renderRegionBar(VideoPlayer &videoPlayer, const ScriptProject &project, EventQueue &eq, const ImVec2 &barMin,
                         const ImVec2 &barMax, const ImVec2 &scriptLinePos, const ImVec2 &scriptLineSize,
                         double offsetTime);
    // Advance the per-axis script-line emphasis ease once per frame, separate from rendering: each axis
    // targets 1 while it is the active lead or an edit-group member, 0 otherwise. renderScriptLines only
    // reads the eased weight (axisEmphasis_).
    void stepAxisEmphasis(const ScriptProject &project);

    ViewState viewState;
    EditState editState;
    SelectionState selectionState;
    StripDragState stripDrag;
    ofs::ui::BandBarDragState regionDragState;
    // Lanes-layout vertical scroll: persisted scroll offset and the resolved geometry for the current
    // frame. laneScroll_ survives between frames (clamped to the live overflow each frame); laneLayout_
    // is rebuilt at the top of renderTimeline. A scrollbar-thumb drag is tracked by the ##lane_scrollbar
    // item's own active state, so no separate latch is kept here.
    float laneScroll_ = 0.f;
    LaneLayout laneLayout_;
    // Axis the timeline context menu (##timeline_ctx) acts on, latched when the menu opens from a
    // right-click on a strip row/script line. Count == no axis section.
    StandardAxis ctxAxis = StandardAxis::Count;
    int ctxRegionId = -1;
    double ctxRegionClickTime = 0.0;
    bool m_regionClickedThisFrame = false;
    // Region whose band color is mid-edit in the context menu. Only the first change of a picker
    // gesture snapshots undo, so dragging through the picker collapses into one undo step.
    int m_colorEditRegionId = -1;
    // Per-axis eased highlight weights, advanced by stepAxisEmphasis, indexed by StandardAxis role.
    // axisEmphasis_ (1 = active or edit-group member) fades the line's opacity/width/outline; axisActive_
    // (1 = the single active lead only) fades the source dots — kept separate because grouped members are
    // emphasized but, by design, draw no dots. emphasisPrimed_ snaps the first frame to its target so the
    // timeline doesn't fade in on appearance.
    std::array<SmoothedFloat, kStandardAxisCount> axisEmphasis_;
    std::array<SmoothedFloat, kStandardAxisCount> axisActive_;
    bool emphasisPrimed_ = false;
};

} // namespace ofs
