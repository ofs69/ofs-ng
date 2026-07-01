#include "ScriptTimeline.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/OverlaySettings.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Localization/AxisNames.h"
#include "Localization/Translator.h"
#include "UI/AxisColors.h"
#include "UI/BandBar.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/OverlayControls.h"
#include "UI/Theme.h"
#include "UI/TimelineLayout.h"
#include "UI/WaveformRenderer.h"
#include "Util/FrameAllocator.h"
#include "Util/TimeUtil.h"
#include "Video/VideoPlayer.h"
#include "imgui_internal.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <vector>

namespace ofs {

// Left gutter holding each axis's short-name label + eye toggle. Font-relative (not a fixed 56 px)
// so the two-char axis symbol and the eye icon both fit and scale at any font size / DPI; the same
// value feeds render() and renderTimeline() within a frame, so they always agree.
static float stripWidth() {
    return ImGui::GetFontSize() * 3.5f;
}
// Source-dot radius and the click hit-radius around it, both font-relative so the dots and their
// grab zones stay the same physical size at any font scale / DPI (≈8 px and ≈12 px at the 18 px
// default). Call within a frame.
static float dotRadius() {
    return ImGui::GetFontSize() * 0.45f;
}
static float dotHitRadius() {
    return ImGui::GetFontSize() * 0.67f;
}
static constexpr float kLodThreshSq = 32.0f;
static constexpr double kDotFadeStart = 10.0;
static constexpr double kDotFadeEnd = 40.0;
// Spring tuning for the script-line emphasis ease. zeta < 1 overshoots ("bounce back"). The emphasis weight
// glides the line's opacity and turns its overshoot into a transient width pulse on activation (steady
// width unchanged); the source dots get a poppier easeOutBack scale-in.
static constexpr float kEmphasisTau = 0.09f;      // slow enough that the opacity glide reads as motion
static constexpr float kEmphasisDamping = 0.5f;   // overshoot drives the activation width pulse
static constexpr float kEmphasisWidthBump = 9.0f; // px of transient line-width pulse per unit overshoot
static constexpr float kDotPopTau = 0.045f;
static constexpr float kDotPopDamping = 0.5f; // poppier dot-scale overshoot

static float easeOutExpo(float x) noexcept {
    return x >= 1.f ? 1.f : 1.f - powf(2, -10 * x);
}

// Breathing room kept above pos=100 and below pos=0 so the end-point dots aren't clipped at the band
// edge. kScriptLineVMargin for a normal full-height band, but capped at a quarter of the band so a thin
// Lanes row (many visible axes) still leaves the script line at least half the lane — without this cap a
// fixed 8px-per-side margin exceeds a <16px lane, inverting posToScreenY and pinning screenYToPos to 0.
static float scriptLineVMargin(float bandHeight) {
    return std::min(ofs::ui::kScriptLineVMargin, bandHeight * 0.25f);
}

static float posToScreenY(int posVal, const ImVec2 &pos, const ImVec2 &size) {
    const float margin = scriptLineVMargin(size.y);
    return pos.y + margin + (1.0f - (float)posVal / 100.0f) * (size.y - 2.0f * margin);
}

static int screenYToPos(float y, const ImVec2 &pos, const ImVec2 &size) {
    const float margin = scriptLineVMargin(size.y);
    float effectiveHeight = size.y - 2.0f * margin;
    if (effectiveHeight <= 0)
        return 0;
    float val = (1.0f - (y - pos.y - margin) / effectiveHeight) * 100.0f;
    return std::clamp((int)val, 0, 100);
}

static float timeToScreenX(double time, double visibleTime, double offsetTime, const ImVec2 &pos, const ImVec2 &size) {
    return pos.x + (float)((time - offsetTime) / visibleTime) * size.x;
}

static double screenXToTime(float x, double visibleTime, double offsetTime, const ImVec2 &pos, const ImVec2 &size) {
    return offsetTime + (double)((x - pos.x) / size.x) * visibleTime;
}

// ── Lane geometry (Lanes layout) ─────────────────────────────────────────────────────────────────
// In Lanes layout the script-line area is split into one row per strip axis, so each axis gets its own
// 0-100 band. Lane index = the axis's ordinal among showInStrip axes (StandardAxis order), matching
// renderStrip's row order so a lane lines up with its strip label. posToScreenY/screenYToPos already
// take a (pos,size) rect, so a lane is just the right sub-rect fed to those same two functions.
//
// Lanes share the band evenly until that would push a lane below kMinLaneH; past that the lane height
// is pinned to the minimum and the band scrolls (LaneLayout::scroll/maxScroll), so a dense many-axis
// view stays editable instead of collapsing into unusable slivers.
struct LaneRect {
    ImVec2 pos;
    ImVec2 size;
};

// Usability floor for one lane, font-relative so it scales with DPI/font. At the default font this is
// ~32 px, which keeps the 0-100 band plus its label comfortably legible. The default script-timeline
// dock height (DockLayout.cpp) is sized so the default 6-axis strip still shares it evenly without
// scrolling at this floor; the seventh axis is the first that overflows. The relationship is pinned by
// the ui-test `lanes_six_axes_fit_without_scrollbar`, so a change to either side that breaks it fails the build.
static float minLaneHeight() {
    return ImGui::GetFontSize() * 1.8f;
}

static int laneRowCount(const ScriptProject &project) {
    int n = 0;
    for (const auto &ax : project.axes)
        if (ax.showInStrip)
            ++n;
    return n;
}

static int laneIndexOf(const ScriptProject &project, StandardAxis role) {
    int n = 0;
    for (const auto &ax : project.axes) {
        if (ax.role == role)
            return ax.showInStrip ? n : -1;
        if (ax.showInStrip)
            ++n;
    }
    return -1;
}

// Resolve lane height + scroll bounds for the band. `scrollIn` is the persisted scroll, returned
// clamped into `.scroll`. Lanes fill the band evenly while each stays >= minLaneHeight(); below that the
// height pins to the minimum and the remainder becomes scrollable overflow.
static LaneLayout makeLaneLayout(const ScriptProject &project, bool lanes, const ImVec2 &scriptLinePos,
                                 const ImVec2 &scriptLineSize, float scrollIn) {
    LaneLayout l;
    l.lanes = lanes;
    l.scriptLinePos = scriptLinePos;
    l.scriptLineSize = scriptLineSize;
    l.count = laneRowCount(project);
    l.laneH = scriptLineSize.y;
    if (!lanes || l.count <= 0)
        return l;
    float evenH = scriptLineSize.y / static_cast<float>(l.count);
    float minH = minLaneHeight();
    if (evenH >= minH) {
        l.laneH = evenH; // fits — share the band evenly, no scroll
    } else {
        l.laneH = minH;
        l.maxScroll = static_cast<float>(l.count) * minH - scriptLineSize.y;
        l.scroll = std::clamp(scrollIn, 0.f, l.maxScroll);
    }
    return l;
}

// Sub-rect of the `index`-th lane within the band. Overlay, an out-of-range index, or a single lane all
// collapse to the full band — so a non-lanes caller and the degenerate one-axis case reuse the shared rect.
static LaneRect laneRectAt(const LaneLayout &l, int index) {
    if (!l.lanes || l.count <= 1 || index < 0 || index >= l.count)
        return {.pos = l.scriptLinePos, .size = l.scriptLineSize};
    float y = l.scriptLinePos.y - l.scroll + l.laneH * static_cast<float>(index);
    return {.pos = {l.scriptLinePos.x, y}, .size = {l.scriptLineSize.x, l.laneH}};
}

static LaneRect laneRectForAxis(const ScriptProject &project, const LaneLayout &l, StandardAxis role) {
    return laneRectAt(l, laneIndexOf(project, role));
}

// Time width of one dot-decimation bucket: dots closer than ~2*dotRadius() px are collapsed to one.
// The bucket grid is anchored to absolute time 0 (`it->at / bucket`), so panning never re-shuffles
// which dot in a cluster wins — that is what keeps decimation steady during playback. To also stay
// steady across *zoom*, the bucket size is snapped to a fixed power-of-two ladder rather than varying
// continuously with visibleTime. Within a ladder step the drawn set is bit-for-bit identical; at a
// step boundary the buckets cleanly halve/double, so the visible set is a strict superset/subset of
// its neighbor — dots reveal or merge in place instead of a different cluster member popping in.
// Both the renderer and the hit-test call this so a clickable dot is exactly a drawn dot.
static double dotBucketDuration(double visibleTime, float width) {
    double minBucket = static_cast<double>(dotRadius()) * 2.0 * visibleTime / static_cast<double>(width);
    if (!(minBucket > 0.0))
        return 1.0; // degenerate (zero-width view); any positive value keeps the bucket math finite
    constexpr double kLadderUnit = 0.001; // 1 ms — the absolute-time anchor the ladder is built on
    double step = std::ceil(std::log2(minBucket / kLadderUnit));
    return kLadderUnit * std::exp2(step);
}

// Enumerate the *visible* source dots of `role`'s axis — exactly the set the renderer draws, so a
// clickable dot is always a drawn dot. Applies, in order: the zoom-out fade gate (nothing once the dots
// have faded away), the zoom/pan-stable bucket decimation (one dot per bucket, see dotBucketDuration),
// and suppression inside regions that hide source points. `fn(action)` runs for each surviving dot in
// time order. Both the dot renderer (Pass 4) and findNearestAction iterate this, which is what keeps the
// hit-test from matching a dot the renderer culled (a hidden-region point) — the two used to drift.
template <class Fn>
static void forEachVisibleDot(const ScriptProject &project, StandardAxis role,
                              const VectorSet<ScriptAxisAction> &actions, double visibleTime, double offsetTime,
                              float width, Fn &&fn) {
    auto fadeT =
        static_cast<float>(std::clamp((visibleTime - kDotFadeStart) / (kDotFadeEnd - kDotFadeStart), 0.0, 1.0));
    if (1.0f - fadeT * fadeT <= 0.1f) // faded out entirely — no dots are drawn, so none are clickable
        return;

    // Precompute this axis's hidden source-point intervals (regions that hide points) overlapping the
    // visible window once, instead of rescanning every region per dot. Frame arena (main-thread only).
    const double winEnd = offsetTime + visibleTime;
    struct HiddenIv {
        double start, end;
    };
    auto *hidden =
        ofs::FrameAllocator::instance().allocArray<HiddenIv>(project.regions.empty() ? 1 : project.regions.size());
    size_t hiddenCount = 0;
    for (const auto &reg : project.regions)
        if (reg.axisRoles.test(static_cast<size_t>(role)) && !reg.showSourceActions && reg.endTime >= offsetTime &&
            reg.startTime <= winEnd)
            hidden[hiddenCount++] = {.start = reg.startTime, .end = reg.endTime};

    auto itStart = actions.lowerBound(ScriptAxisAction{offsetTime, 0});
    if (itStart != actions.begin())
        --itStart;
    auto itEnd = actions.upperBound(ScriptAxisAction{offsetTime + visibleTime, 0});
    if (itEnd != actions.end())
        ++itEnd;
    const double timeBucketDuration = dotBucketDuration(visibleTime, width);
    int lastBucket = -1;
    if (itStart != actions.begin())
        lastBucket = static_cast<int>(std::prev(itStart)->at / timeBucketDuration);
    for (auto it = itStart; it != itEnd; ++it) {
        int bucket = static_cast<int>(it->at / timeBucketDuration);
        if (bucket == lastBucket)
            continue;
        lastBucket = bucket;
        bool inHiddenRegion = false;
        for (size_t h = 0; h < hiddenCount; ++h)
            if (it->at >= hidden[h].start && it->at <= hidden[h].end) {
                inHiddenRegion = true;
                break;
            }
        if (inHiddenRegion)
            continue;
        fn(*it);
    }
}

static const ScriptAxisAction *findNearestAction(const ScriptProject &project, StandardAxis role,
                                                 const VectorSet<ScriptAxisAction> &actions, float mouseX, float mouseY,
                                                 double visibleTime, double offsetTime, const ImVec2 &pos,
                                                 const ImVec2 &size, float radius = dotHitRadius()) {
    const ScriptAxisAction *closest = nullptr;
    float closestDist2 = radius * radius;
    forEachVisibleDot(project, role, actions, visibleTime, offsetTime, size.x, [&](const ScriptAxisAction &a) {
        float sx = timeToScreenX(a.at, visibleTime, offsetTime, pos, size);
        float sy = posToScreenY(a.pos, pos, size);
        float dx = mouseX - sx;
        float dy = mouseY - sy;
        float dist2 = dx * dx + dy * dy;
        if (dist2 < closestDist2) {
            closestDist2 = dist2;
            closest = &a;
        }
    });
    return closest;
}

ScriptTimelineWindow::ScriptTimelineWindow() = default;

void ScriptTimelineWindow::renderStrip(const ScriptProject &project, EventQueue &eq, ImDrawList *drawList,
                                       const ImVec2 &pos, const ImVec2 &size, const ImVec2 &stripPos,
                                       const ImVec2 &stripSize, const ImVec2 &scriptLinePos,
                                       const AxisEntry *stripEntries, int numRows, bool hasGroup) {
    drawList->AddRectFilled(stripPos, stripPos + stripSize, ofs::theme::GetColorU32(AppCol_StripBg));

    // The separated view shows every axis in its own lane at all times, so the per-axis visibility toggle
    // is meaningless there: hide the eye icon and disable its hit-area (a click anywhere on the row then
    // selects/groups instead).
    const bool lanes = project.timelineView.layout == TimelineLayout::Lanes;

    // In Lanes the rows mirror the script-line lanes exactly (height + scroll), so a label always sits beside
    // its lane; Overlay shares the band evenly. When the lanes overflow (scrollbar shown) the rows are
    // clipped to the band and offset by the scroll, just like the script-line side.
    const float rowH = lanes ? laneLayout_.laneH : (numRows > 0 ? size.y / static_cast<float>(numRows) : size.y);
    const float scroll = lanes ? laneLayout_.scroll : 0.f;
    const bool clipRows = lanes && laneLayout_.maxScroll > 0.f;
    float fontH = ImGui::GetFontSize();

    // ImGui::PushClipRect (not the draw-list overload) so the clip bounds the per-row InvisibleButtons'
    // hit-testing, not just their pixels: an overflowing row's button would otherwise keep its full rect
    // and, when scrolled toward the top, reach below the band into the corner gear/layout buttons that sit
    // in the same strip column — stealing their clicks (the rows are submitted first and don't allow overlap).
    if (clipRows)
        ImGui::PushClipRect(stripPos, stripPos + stripSize, true);

    // The row the mouse is over this frame, resolved through the per-row buttons below (-1 = none).
    int hoveredRow = -1;

    for (int i = 0; i < numRows; ++i) {
        const auto &entry = stripEntries[i];
        const auto &ax = project.axes[static_cast<size_t>(entry.role)];
        ImVec2 rowMin = {stripPos.x, stripPos.y - scroll + rowH * static_cast<float>(i)};
        ImVec2 rowMax = {stripPos.x + stripSize.x, rowMin.y + rowH};

        // Each row is a real interactive item: the strip hit-tests and reacts through these per-row
        // buttons (which also give ui-tests a stable id, ##strip_row_<axis>, to target). Right button
        // is enabled so the context menu opens from the same item. The drag-to-group span below still
        // uses row geometry because it tracks the mouse across rows while the anchor button is held.
        ImGui::SetCursorScreenPos(rowMin);
        ImGui::InvisibleButton(fmtScratch("##strip_row_{}", standardAxisShortName(entry.role)), {stripSize.x, rowH},
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool rowHovered = ImGui::IsItemHovered();
        if (rowHovered)
            hoveredRow = i;

        if (entry.isActive)
            drawList->AddRectFilled(rowMin, rowMax, ofs::theme::GetColorU32(AppCol_StripActiveBg));
        else if (entry.inEditSet)
            // Faint axis-colored wash marks a grouped (non-lead) row; the lead keeps AppCol_StripActiveBg.
            drawList->AddRectFilled(rowMin, rowMax, (entry.color & 0x00FFFFFFu) | (40u << 24));

        if (rowHovered && !entry.isActive)
            drawList->AddRectFilled(rowMin, rowMax, ofs::theme::GetColorU32(AppCol_StripHoverBg));

        // Left-edge chip in the axis color marks every axis in the edit set — the active (lead) axis on
        // its own, and every member when a group is active. So the indicator is present even single-axis.
        if (entry.inEditSet)
            drawList->AddRectFilled(rowMin, {rowMin.x + 3.f, rowMax.y}, entry.color);

        if (i < numRows - 1)
            drawList->AddLine({rowMin.x, rowMax.y}, {rowMax.x, rowMax.y},
                              ofs::theme::GetColorU32(AppCol_StripSeparator), 1.f);

        const char *labelStr = standardAxisShortName(ax.role).data();
        ImU32 textColor = entry.isActive ? entry.color : ((entry.color & 0x00FFFFFF) | (0x99u << 24));
        float rowCy = rowMin.y + rowH * 0.5f;
        float textY = rowMin.y + (rowH - fontH) * 0.5f;

        constexpr float kLockSz = 4.f;
        constexpr float kLockPad = 3.f;
        constexpr float kNameX0 = kLockPad + kLockSz + 3.f;
        if (ax.isLocked) {
            ImU32 lockCol = (ofs::theme::GetColorU32(AppCol_LockIndicator) & 0x00FFFFFFU) |
                            (static_cast<ImU32>(entry.isActive ? 220u : 110u) << 24);
            drawList->AddRectFilled({rowMin.x + kLockPad, rowCy - kLockSz * 0.5f},
                                    {rowMin.x + kLockPad + kLockSz, rowCy + kLockSz * 0.5f}, lockCol);
        }

        const float nameX = rowMin.x + kNameX0;
        if (lanes) {
            // No eye icon in the separated view — the label runs to the row's right edge.
            ofs::ui::addTextShadow(drawList, {nameX, textY}, textColor,
                                   ofs::ui::elide(labelStr, rowMax.x - 6.f - nameX));
        } else {
            // Resolve the eye-icon slot first so the label can be elided to stop short of it: in a narrow
            // strip (small window / large font) the name would otherwise run under the icon.
            const char *eyeIcon = ax.isVisible ? ICON_EYE : ICON_EYE_OFF;
            ImVec2 iconSz = ImGui::CalcTextSize(eyeIcon);
            float iconX = rowMax.x - 6.f - iconSz.x;
            float iconY = rowMin.y + (rowH - iconSz.y) * 0.5f;
            ofs::ui::addTextShadow(drawList, {nameX, textY}, textColor, ofs::ui::elide(labelStr, iconX - nameX - 2.f));
            // Hidden axes keep their hue but read muted; visible ones use the row's full text color.
            ImU32 eyeCol = ax.isVisible ? textColor : ((textColor & 0x00FFFFFFu) | (0x66u << 24));
            drawList->AddText({iconX, iconY}, eyeCol, eyeIcon);
        }
    }

    if (clipRows)
        ImGui::PopClipRect();

    drawList->AddLine({scriptLinePos.x, pos.y}, {scriptLinePos.x, pos.y + size.y},
                      ofs::theme::GetColorU32(AppCol_StripDivider), 1.f);

    // Strip interaction, driven by the per-row buttons above (hoveredRow). Left-click selects
    // (dissolving any group); Ctrl-click toggles an axis in/out of the editing group; clicking a member
    // of an existing group makes it the lead without dissolving; double-click always dissolves; dragging
    // across rows builds a group from the spanned run.
    if (hoveredRow >= 0) {
        const int row = hoveredRow;
        const float mX = ImGui::GetMousePos().x;
        const StandardAxis clickedRole = stripEntries[row].role;
        const auto &rowAx = project.axes[static_cast<size_t>(clickedRole)];
        // Match the eye-icon slot drawn above (rowMax.x - 6 - iconWidth) so the click target tracks the
        // icon at any font size, instead of a fixed pixel band that drifts off it. The separated view has
        // no eye icon, so its hit-area is disabled and the whole row selects/groups.
        const char *eyeIcon = rowAx.isVisible ? ICON_EYE : ICON_EYE_OFF;
        const bool eyeArea = !lanes && mX >= stripPos.x + stripSize.x - 6.f - ImGui::CalcTextSize(eyeIcon).x;
        const bool ctrl = ImGui::GetIO().KeyCtrl;

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !eyeArea) {
            eq.push(AxisSelectedEvent{.role = clickedRole}); // dissolve the group + select
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (eyeArea) {
                eq.push(ToggleAxisVisibilityEvent{.axisRole = clickedRole, .visible = !rowAx.isVisible});
            } else if (ctrl) {
                // Toggle membership, operating on the current edit set so the first Ctrl-click groups the
                // active axis with the clicked one. Promote a new lead if the current lead was removed.
                AxisRoles roles = project.effectiveEditSet();
                roles.flip(static_cast<size_t>(clickedRole));
                StandardAxis lead = project.state.activeAxis;
                if (lead >= StandardAxis::Count || !roles.test(static_cast<size_t>(lead))) {
                    lead = clickedRole;
                    for (size_t k = 0; k < kStandardAxisCount; ++k)
                        if (roles.test(k)) {
                            lead = static_cast<StandardAxis>(k);
                            break;
                        }
                }
                eq.push(SetAxisGroupingEvent{.roles = roles, .lead = lead});
                stripDrag.anchorRow = row;
            } else if (hasGroup && project.state.axesGrouping.test(static_cast<size_t>(clickedRole))) {
                eq.push(SetAxisGroupingEvent{.roles = project.state.axesGrouping, .lead = clickedRole}); // re-lead
                stripDrag.anchorRow = row;
            } else {
                eq.push(AxisSelectedEvent{.role = clickedRole}); // dissolve + select
                stripDrag.anchorRow = row;
            }
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            // Open on release, matching the region band's context menu (BandBar) so right-click feels
            // identical across adjacent timeline surfaces.
            ctxAxis = clickedRole;
            ImGui::OpenPopup("##timeline_ctx");
        }
    }

    // Drag across strip rows → group the spanned run (lead = the row the drag started on).
    if (stripDrag.anchorRow >= 0 && stripDrag.anchorRow < numRows) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float mY = ImGui::GetMousePos().y;
            int row = std::clamp(static_cast<int>((mY - stripPos.y + scroll) / rowH), 0, numRows - 1);
            int lo = std::min(stripDrag.anchorRow, row);
            int hi = std::max(stripDrag.anchorRow, row);
            // IsMouseDragging is true every frame, so only push when the spanned run actually changes —
            // re-pushing an identical group each frame re-applies it and re-fires its observers needlessly.
            if (hi > lo && (lo != stripDrag.spanLo || hi != stripDrag.spanHi)) { // a real span of ≥2 rows
                stripDrag.spanLo = lo;
                stripDrag.spanHi = hi;
                AxisRoles roles;
                for (int r = lo; r <= hi; ++r)
                    roles.set(static_cast<size_t>(stripEntries[r].role));
                eq.push(SetAxisGroupingEvent{.roles = roles, .lead = stripEntries[stripDrag.anchorRow].role});
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        stripDrag.anchorRow = -1;
        stripDrag.spanLo = stripDrag.spanHi = -1;
    }
}

void ScriptTimelineWindow::renderScriptLines(const ScriptProject &project, ImDrawList *drawList,
                                             WaveformRenderer &waveform, const ImVec2 &pos, const ImVec2 &size,
                                             const ImVec2 &scriptLinePos, const ImVec2 &scriptLineSize,
                                             double offsetTime, const AxisEntry *scriptLineEntries, int scriptLineCount,
                                             bool lanes, bool windowHovered) {
    {
        ImU32 bgTop = ofs::theme::GetColorU32(AppCol_ScriptLineBgTop);
        ImU32 bgBottom = ofs::theme::GetColorU32(AppCol_ScriptLineBgBottom);
        drawList->AddRectFilledMultiColor(scriptLinePos, scriptLinePos + scriptLineSize, bgTop, bgTop, bgBottom,
                                          bgBottom);
    }

    // Clip everything that follows to the script-line band. In Lanes the wash/separators/grid below are placed
    // per-lane, and a scrolled lane's rect runs past the band edge — without this clip those fills/lines
    // bleed over the strip rows above and the region bar below. (Overlay lanes tile the band exactly, so
    // it is a harmless no-op there.) The per-axis loop nests its own per-lane clips inside this one.
    drawList->PushClipRect(scriptLinePos, scriptLinePos + scriptLineSize, true);

    // Active-lane wash (Lanes only): the active lane carries the same highlight as its active strip row so
    // the two read as one continuous band. Part of the background — drawn before the waveform so it tints
    // behind it, not over it (same layer as the background gradient).
    if (lanes)
        for (int li = 0; li < scriptLineCount; ++li)
            if (scriptLineEntries[li].isActive) {
                LaneRect lr = laneRectAt(laneLayout_, li);
                drawList->AddRectFilled(lr.pos, lr.pos + lr.size, ofs::theme::GetColorU32(AppCol_StripActiveBg));
            }

    // Audio waveform: drawn on top of the background gradient but before the grid lines and script lines, so it
    // sits behind everything the user edits while staying legible.
    if (project.timelineView.showAudioWaveform)
        waveform.drawBackground(drawList, scriptLinePos, scriptLineSize, offsetTime, viewState.visibleTime);

    if (windowHovered && ImGui::IsMouseHoveringRect(scriptLinePos, scriptLinePos + scriptLineSize))
        drawList->AddRectFilled(scriptLinePos, scriptLinePos + scriptLineSize,
                                ofs::theme::GetColorU32(AppCol_ScriptLineHoverBg));

    // Lane separators (Lanes only): a line between adjacent lanes makes each lane's bounds legible. Drawn
    // with the grid layer (above the waveform) so the boundary stays crisp over a loud waveform.
    if (lanes) {
        const ImU32 sepCol = ofs::theme::GetColorU32(AppCol_StripSeparator);
        for (int li = 1; li < scriptLineCount; ++li) {
            LaneRect lr = laneRectAt(laneLayout_, li);
            drawList->AddLine({lr.pos.x, lr.pos.y}, {lr.pos.x + lr.size.x, lr.pos.y}, sepCol, 1.0f);
        }
    }

    // Grid: the nine 0-100 reference lines (mid/50 emphasized). One band in Overlay; in Lanes each row
    // gets its own band so every lane reads against its own 0/50/100.
    auto drawGrid = [&](const ImVec2 &gp, const ImVec2 &gs) {
        for (int i = 0; i < 9; i++) {
            float y = posToScreenY((i + 1) * 10, gp, gs);
            ImU32 gridCol =
                (i == 4) ? ofs::theme::GetColorU32(AppCol_GridLineMid) : ofs::theme::GetColorU32(AppCol_GridLine);
            float lineW = (i == 4) ? ofs::theme::GetStyleVar(AppVar_GridLineMidWidth) : 1.0f;
            drawList->AddLine({gp.x, y}, {gp.x + gs.x, y}, gridCol, lineW);
        }
    };
    if (lanes)
        for (int li = 0; li < scriptLineCount; ++li) {
            LaneRect lr = laneRectAt(laneLayout_, li);
            drawGrid(lr.pos, lr.size);
        }
    else
        drawGrid(scriptLinePos, scriptLineSize);

    // The active lane's sub-rect, set per iteration in Lanes mode; the shared full rect in Overlay. Every
    // per-axis script-line/dot/selection mapping below routes through these instead of scriptLinePos/scriptLineSize.
    ImVec2 lanePos = scriptLinePos;
    ImVec2 laneSize = scriptLineSize;

    const auto &lineColors = ofs::theme::getActive().heatmapColors;

    auto segmentHeatColor = [&lineColors](const ScriptAxisAction *prev, const ScriptAxisAction *curr) -> ImU32 {
        double dt = curr->at - prev->at;
        float relSpeed = 0.0f;
        if (dt > 0.0) {
            auto speed = static_cast<float>(std::abs(curr->pos - prev->pos) / dt);
            relSpeed = std::clamp(speed / ofs::theme::getActive().heatmapMaxSpeed, 0.0f, 1.0f);
        }
        // Quantize to a fixed number of speed buckets so consecutive segments of
        // similar speed resolve to an identical color and can merge into one
        // flush AddPolyline (the run breaks only at bucket boundaries). 32 levels
        // keeps the gradient banding imperceptible.
        constexpr float kHeatBuckets = 32.0f;
        relSpeed = std::round(relSpeed * kHeatBuckets) / kHeatBuckets;
        float color[4];
        lineColors.getColorAt(relSpeed, color);
        return ImColor(color[0], color[1], color[2], 1.0f);
    };

    // Stroke a run of points as AddPolyline calls with flush miter joints — far
    // cheaper than per-segment AddLine (it shares join vertices instead of
    // duplicating an end-cap per segment) and gap-free. The color is queried per
    // segment; a run is flushed and restarted at the shared vertex whenever the
    // color changes (so the per-segment heat gradient still works — adjacent
    // same-bucket segments stay in one run) or the turn exceeds the miter limit.
    // With a constant color the run only ever breaks on the miter limit.
    //
    // Miter rationale: ImGui's miters are unbounded, so at a near-reversal the
    // join length blows up into a long spike. Zoomed far out the script line
    // compresses into a sawtooth of reversals, surfacing those spikes as black
    // needles. We break the run whenever the turn exceeds the limit, ending both
    // sides with a flat butt cap — no spike, no extra geometry. A tighter limit
    // butt-caps more turns, trading a few sub-pixel seams for fewer sharp caps.
    // cos(turn) < kMiterCosLimit ⇔ miter factor > ~1.4. Points are buffered into
    // the per-frame arena (no heap churn).
    constexpr float kMiterCosLimit = 0.0f; // break joins on turns sharper than 90°
    auto strokePolyline = [&](const auto &itBegin, const auto &itEnd, double skipAt, float width, auto colorOf) {
        const auto cap = static_cast<size_t>(std::distance(itBegin, itEnd));
        if (cap < 2)
            return;
        auto *pts = ofs::FrameAllocator::instance().allocArray<ImVec2>(cap);
        int n = 0;
        ImVec2 lastPt;
        ImVec2 lastDir;
        ImU32 runCol = 0;
        const ScriptAxisAction *prev = nullptr;
        bool have = false;
        bool haveDir = false;
        auto flush = [&] {
            if (n >= 2)
                drawList->AddPolyline(pts, n, runCol, width); // open polyline (flags default 0)
            n = 0;
        };
        for (auto it = itBegin; it != itEnd; ++it) {
            if (it->at == skipAt) { // gap marker: end the current run, start a new one
                flush();
                have = false;
                haveDir = false;
                prev = nullptr;
                continue;
            }
            ImVec2 p(timeToScreenX(it->at, viewState.visibleTime, offsetTime, lanePos, laneSize),
                     posToScreenY(it->pos, lanePos, laneSize));
            if (have) {
                float dx = p.x - lastPt.x;
                float dy = p.y - lastPt.y;
                float len2 = dx * dx + dy * dy;
                if (len2 < kLodThreshSq) // LOD: drop sub-threshold segments
                    continue;
                float invLen = 1.0f / std::sqrt(len2);
                ImVec2 dir(dx * invLen, dy * invLen);
                ImU32 segCol = colorOf(prev, &(*it));
                bool sharp = haveDir && lastDir.x * dir.x + lastDir.y * dir.y < kMiterCosLimit;
                if (n == 0) { // first segment of a run: seed start vertex + color
                    pts[n++] = lastPt;
                    runCol = segCol;
                } else if (sharp || segCol != runCol) {
                    flush();
                    pts[n++] = lastPt; // restart at the shared vertex (butt cap)
                    runCol = segCol;
                }
                pts[n++] = p;
                lastDir = dir;
                haveDir = true;
            }
            lastPt = p;
            prev = &(*it);
            have = true;
        }
        flush();
    };

    for (int ci = 0; ci < scriptLineCount; ++ci) {
        const auto &entry = scriptLineEntries[ci];
        const auto &ax = project.axes[static_cast<size_t>(entry.role)];
        const auto &displayActions = ax.resolved ? ax.resolved->actions : ax.actions;
        bool isActive = entry.isActive;

        if (lanes) {
            LaneRect lr = laneRectAt(laneLayout_, ci);
            lanePos = lr.pos;
            laneSize = lr.size;
            drawList->PushClipRect(lr.pos, lr.pos + lr.size, true);
        }
        // Eased emphasis weight (0 = background, 1 = active/grouped), stepped per frame in
        // stepAxisEmphasis. It fades the line's opacity/width/outline when the active axis or edit
        // group changes; the heat-gradient and dot passes below still switch discretely on isActive,
        // so a grouped member's emphasized line weight is the sole cue it belongs to the edit group.
        const float e = axisEmphasis_[static_cast<size_t>(entry.role)].value();
        const float aw = axisActive_[static_cast<size_t>(entry.role)].value();

        // Emphasized axes (active or grouped) reach full opacity and the themed base width; a plain
        // background reference fades toward backgroundAxisOpacity and 1px thinner. The contrast outline
        // is a fixed 2px halo each side, so it tracks the line width automatically. The settle width is
        // the e∈[0,1] interpolation; the spring's overshoot (e > 1, only on activation) becomes a
        // transient width pulse on top, so the line visibly swells then relaxes back to base — the
        // motion is in the transition, the resting width is unchanged. Floored at 1px so an undershoot
        // on the way out can't invert the stroke.
        // Lanes: every axis owns its row, so none is a faded background reference — full opacity, no
        // background-opacity dip. Overlay keeps the dip so stacked background axes recede behind the lead.
        const float bgAxisOpacity = ofs::theme::getActive().backgroundAxisOpacity;
        float lineOpacity = lanes ? 1.0f : std::clamp(bgAxisOpacity + (1.0f - bgAxisOpacity) * e, 0.0f, 1.0f);
        const float baseLineW = ofs::theme::GetStyleVar(AppVar_TimelineLineWidth);
        const float bgLineW = std::max(1.0f, baseLineW - 1.0f);
        const float settleW = bgLineW + (baseLineW - bgLineW) * std::clamp(e, 0.0f, 1.0f);
        const float pulseW = std::max(0.0f, e - 1.0f) * kEmphasisWidthBump;
        float lineW = std::max(1.0f, settleW + pulseW);
        float outlineW = lineW + 4.0f;
        auto outlineAlpha = static_cast<ImU32>(255.f * lineOpacity);

        constexpr double skipAt = -1.0;

        if (!displayActions.empty()) {
            auto itStart = displayActions.lowerBound(ScriptAxisAction{offsetTime, 0});
            if (itStart != displayActions.begin())
                --itStart;
            auto itEnd = displayActions.upperBound(ScriptAxisAction{offsetTime + viewState.visibleTime, 0});
            if (itEnd != displayActions.end())
                ++itEnd;

            // Pass 1: contrast outline
            const ImU32 outlineCol =
                (ofs::theme::GetColorU32(AppCol_TimelineOutline) & 0x00FFFFFFU) | (outlineAlpha << 24);
            strokePolyline(itStart, itEnd, skipAt, outlineW,
                           [outlineCol](const ScriptAxisAction *, const ScriptAxisAction *) { return outlineCol; });

            // Pass 2: colored lines. The active axis uses the per-segment heat gradient (bucketed so
            // same-speed runs still join flush). In Lanes every axis is a first-class row, so all lanes get
            // the heat gradient too; in Overlay the stacked background axes stay a single flat axis color so
            // they read as references behind the lead.
            if (isActive || lanes) {
                strokePolyline(itStart, itEnd, skipAt, lineW, segmentHeatColor);
            } else {
                ImU32 axColor = (entry.color & 0x00FFFFFF) | (static_cast<ImU32>(255.0f * lineOpacity) << 24);
                strokePolyline(itStart, itEnd, skipAt, lineW,
                               [axColor](const ScriptAxisAction *, const ScriptAxisAction *) { return axColor; });
            }

            // Pass 3: selection overlay (every axis in the edit set, so group selections are visible)
            if (entry.inEditSet && !ax.selection.empty()) {
                auto selStart = ax.selection.lowerBound(ScriptAxisAction{offsetTime, 0});
                if (selStart != ax.selection.begin())
                    --selStart;
                auto selEnd = ax.selection.upperBound(ScriptAxisAction{offsetTime + viewState.visibleTime, 0});
                if (selEnd != ax.selection.end())
                    ++selEnd;
                const ImU32 selLineCol = ofs::theme::GetColorU32(AppCol_SelectedLine);
                strokePolyline(selStart, selEnd, skipAt, lineW,
                               [selLineCol](const ScriptAxisAction *, const ScriptAxisAction *) { return selLineCol; });
            }

            // Pass 4: dots from source actions (active axis only; grouped members read as their
            // emphasized line, not dots). Driven by the eased active weight (a spring): the dot radius
            // scales with it, so when the active axis changes the dots pop in with an easeOutBack
            // overshoot — briefly larger than full size, then settling — instead of appearing instantly.
            // Alpha is a plain clamped fade (no overshoot past opaque). Hidden when points are toggled off.
            const float dotScale = std::max(0.0f, aw);
            if (project.timelineView.showPoints && dotScale > 0.01f && !ax.actions.empty()) {
                auto fadeT = static_cast<float>(
                    std::clamp((viewState.visibleTime - kDotFadeStart) / (kDotFadeEnd - kDotFadeStart), 0.0, 1.0));
                float dotAlpha = (1.0f - fadeT * fadeT) * std::clamp(aw, 0.0f, 1.0f);
                if (dotAlpha > 0.1f) {
                    auto alpha = static_cast<ImU32>(255.0f * dotAlpha);
                    const float rOuter = dotRadius() * dotScale;
                    const float rInner = dotRadius() * 0.7f * dotScale;
                    forEachVisibleDot(
                        project, ax.role, ax.actions, viewState.visibleTime, offsetTime, laneSize.x,
                        [&](const ScriptAxisAction &a) {
                            ImVec2 p(timeToScreenX(a.at, viewState.visibleTime, offsetTime, lanePos, laneSize),
                                     posToScreenY(a.pos, lanePos, laneSize));
                            bool selected = ax.selection.contains(a);
                            const ImU32 outlineCol =
                                (ofs::theme::GetColorU32(AppCol_TimelineOutline) & 0x00FFFFFFU) | (alpha << 24);
                            const ImU32 innerCol = (ofs::theme::GetColorU32(selected ? AppCol_TimelinePointSelected
                                                                                     : AppCol_TimelinePoint) &
                                                    0x00FFFFFFU) |
                                                   (alpha << 24);
                            drawList->AddCircleFilled(p, rOuter, outlineCol, 4);
                            drawList->AddCircleFilled(p, rInner, innerCol, 4);
                        });
                }
            }

            // Pass 5: selected-point dots on non-active axes. Pass 4 draws every dot for the active axis
            // (selected ones highlighted), but a grouped or background axis otherwise shows only its line —
            // so a multi-axis selection would be invisible on those axes. Draw just the selected points
            // there, so the selection reads on every axis it covers. Routed through the same
            // forEachVisibleDot as Pass 4, so it inherits identical culling — the zoom-fade gate,
            // zoom/pan-stable decimation, and hidden-region suppression — and only differs in the
            // selected-point filter and that its alpha isn't gated by the active-axis weight.
            if (project.timelineView.showPoints && !isActive && !ax.selection.empty()) {
                auto fadeT = static_cast<float>(
                    std::clamp((viewState.visibleTime - kDotFadeStart) / (kDotFadeEnd - kDotFadeStart), 0.0, 1.0));
                auto alpha = static_cast<ImU32>(255.0f * (1.0f - fadeT * fadeT));
                const float rOuter = dotRadius();
                const float rInner = dotRadius() * 0.7f;
                const ImU32 outlineCol =
                    (ofs::theme::GetColorU32(AppCol_TimelineOutline) & 0x00FFFFFFU) | (alpha << 24);
                const ImU32 innerCol =
                    (ofs::theme::GetColorU32(AppCol_TimelinePointSelected) & 0x00FFFFFFU) | (alpha << 24);
                forEachVisibleDot(project, ax.role, ax.actions, viewState.visibleTime, offsetTime, laneSize.x,
                                  [&](const ScriptAxisAction &a) {
                                      if (!ax.selection.contains(a))
                                          return;
                                      ImVec2 p(
                                          timeToScreenX(a.at, viewState.visibleTime, offsetTime, lanePos, laneSize),
                                          posToScreenY(a.pos, lanePos, laneSize));
                                      drawList->AddCircleFilled(p, rOuter, outlineCol, 4);
                                      drawList->AddCircleFilled(p, rInner, innerCol, 4);
                                  });
            }
        }

        if (lanes)
            drawList->PopClipRect();
    }

    // Selection box (a time range, drawn after the per-axis loop). The selection fans across the edit
    // group, so the marquee shows where it lands: in Overlay the stacked axes share one band, so it spans
    // the full height; in Lanes it is drawn over each active/grouped lane only — never the background lanes
    // it won't touch.
    if (selectionState.isSelecting) {
        auto relSel1 = (float)((selectionState.absSelStart - offsetTime) / viewState.visibleTime);
        auto rSel2 = (float)selectionState.relSelEnd;
        float minRel = std::min(relSel1, rSel2);
        float maxRel = std::max(relSel1, rSel2);
        const ImU32 selectColor = ofs::theme::GetColorU32(AppCol_SelectionBox);
        const ImU32 selectBg = ofs::theme::GetColorU32(AppCol_SelectionBoxFill);
        const float x0 = scriptLinePos.x + scriptLineSize.x * minRel;
        const float x1 = scriptLinePos.x + scriptLineSize.x * maxRel;
        auto drawBox = [&](const ImVec2 &bp, const ImVec2 &bs) {
            ImVec2 selMin(x0, bp.y);
            ImVec2 selMax(x1, bp.y + bs.y);
            drawList->AddRectFilled(selMin, selMax, selectBg);
            drawList->AddLine(selMin, {selMin.x, selMax.y}, selectColor, 3.0f);
            drawList->AddLine({selMax.x, selMin.y}, selMax, selectColor, 3.0f);
        };
        if (lanes) {
            for (int li = 0; li < scriptLineCount; ++li)
                if (scriptLineEntries[li].inEditSet) {
                    LaneRect lr = laneRectAt(laneLayout_, li);
                    drawBox(lr.pos, lr.size);
                }
        } else {
            drawBox(scriptLinePos, scriptLineSize);
        }
    }

    drawList->PopClipRect();

    // Outer border
    ImU32 borderColor = ofs::theme::GetColorU32(ImGuiCol_Border);
    if (project.state.activeAxis < StandardAxis::Count) {
        const auto &activeAx = project.axes[static_cast<size_t>(project.state.activeAxis)];
        if (!activeAx.selection.empty())
            borderColor = ofs::theme::GetColorU32(ImGuiCol_SliderGrabActive);
    }
    drawList->AddRect(pos, pos + size, borderColor, 0.0f, 1.0f, 0);
}

bool ScriptTimelineWindow::renderLaneScrollbar(ImDrawList *drawList, const ImVec2 &scriptLinePos,
                                               const ImVec2 &scriptLineSize, float mouseY) {
    if (laneLayout_.maxScroll <= 0.f)
        return false;

    // Standard proportional scrollbar: thumb height is the visible fraction of the content, and it
    // travels the leftover track. The thumb is positioned from the scroll the lanes were drawn with
    // (laneLayout_.scroll); drag/jump input feeds laneScroll_, which makeLaneLayout consumes next frame.
    const float sbW = ImGui::GetStyle().ScrollbarSize;
    const ImVec2 trackMin = {scriptLinePos.x + scriptLineSize.x - sbW, scriptLinePos.y};
    const ImVec2 trackMax = {scriptLinePos.x + scriptLineSize.x, scriptLinePos.y + scriptLineSize.y};
    const float contentH = static_cast<float>(laneLayout_.count) * laneLayout_.laneH;
    const float thumbH = std::max(scriptLineSize.y * scriptLineSize.y / contentH, ImGui::GetFontSize());
    const float travel = scriptLineSize.y - thumbH;
    const float thumbY = trackMin.y + (laneLayout_.scroll / laneLayout_.maxScroll) * std::max(travel, 0.f);

    // A real ImGui item owns the track, so its hover/active state is id-tracked (ui-tests drag it by
    // ##lane_scrollbar rather than synthesizing track coordinates). It is submitted after the lane buttons,
    // which set AllowOverlap for this overflow case, so the track wins hover where they overlap.
    ImGui::SetCursorScreenPos(trackMin);
    ImGui::InvisibleButton("##lane_scrollbar", {sbW, scriptLineSize.y});
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // On press off the thumb, jump so the thumb centers under the cursor; while held, track the mouse.
    if (ImGui::IsItemActivated() && (mouseY < thumbY || mouseY > thumbY + thumbH) && travel > 0.f)
        laneScroll_ = std::clamp((mouseY - trackMin.y - thumbH * 0.5f) / travel, 0.f, 1.f) * laneLayout_.maxScroll;
    if (active && travel > 0.f)
        laneScroll_ = std::clamp(laneScroll_ + ImGui::GetIO().MouseDelta.y * laneLayout_.maxScroll / travel, 0.f,
                                 laneLayout_.maxScroll);

    const ImGuiCol grabCol = active    ? ImGuiCol_ScrollbarGrabActive
                             : hovered ? ImGuiCol_ScrollbarGrabHovered
                                       : ImGuiCol_ScrollbarGrab;
    drawList->AddRectFilled(trackMin, trackMax, ofs::theme::GetColorU32(ImGuiCol_ScrollbarBg));
    drawList->AddRectFilled({trackMin.x, thumbY}, {trackMax.x, thumbY + thumbH}, ofs::theme::GetColorU32(grabCol),
                            ImGui::GetStyle().ScrollbarRounding);
    return hovered || active;
}

void ScriptTimelineWindow::stepAxisEmphasis(const ScriptProject &project) {
    const AxisRoles editSet = project.effectiveEditSet();
    const float dt = ImGui::GetIO().DeltaTime;
    for (size_t r = 0; r < kStandardAxisCount; ++r) {
        const bool isActive = static_cast<StandardAxis>(r) == project.state.activeAxis;
        const float emph = (isActive || editSet.test(r)) ? 1.0f : 0.0f;
        const float act = isActive ? 1.0f : 0.0f;
        if (!emphasisPrimed_) {
            axisEmphasis_[r].snap(emph);
            axisActive_[r].snap(act);
        }
        axisEmphasis_[r].setTarget(emph);
        axisEmphasis_[r].advance(dt, kEmphasisTau, kEmphasisDamping);
        axisActive_[r].setTarget(act);
        axisActive_[r].advance(dt, kDotPopTau, kDotPopDamping);
    }
    emphasisPrimed_ = true;
}

void ScriptTimelineWindow::renderTimeline(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                                          WaveformRenderer &waveform, const ImVec2 &pos, const ImVec2 &size) {
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    double offsetTime = project.playback.cursorPos - viewState.visibleTime / 2.0;

    // IsMouseHoveringRect only clips to this window's rect; it ignores windows stacked on top.
    // Gate the visual-only hover highlights on IsWindowHovered so they don't leak through a popup
    // or another window covering the timeline. (Click paths already use IsItemHovered, which is safe.)
    const bool windowHovered = ImGui::IsWindowHovered();

    const float stripW = stripWidth();
    const ImVec2 stripPos = pos;
    // Trim the strip's right edge by one item-spacing and let the script line fill the freed column, so the
    // strip and script line sit flush with no bare seam. The same trimmed edge is the left of the region bar
    // (render()), keeping the script line, overlay grid, and bands aligned. The gear keeps a margin of its own.
    const float stripMargin = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 stripSize = {stripW - stripMargin, size.y};
    const ImVec2 scriptLinePos = {pos.x + stripW - stripMargin, pos.y};
    const ImVec2 scriptLineSize = {size.x - stripW + stripMargin, size.y};

    // The multi-axis edit set (active group, or just {activeAxis}). Members beyond the lead render
    // their selection + dots and stay in the script-line list even when hidden, so a fanned-out edit is
    // visible while it happens. hasGroup gates the strip's group bracket/fill.
    const AxisRoles editSet = project.effectiveEditSet();
    const bool hasGroup = project.state.axesGrouping.count() > 1;
    const bool lanes = project.timelineView.layout == TimelineLayout::Lanes;

    // Resolve lane height + scroll for this frame, then write the clamped scroll back so it stays bounded
    // as the axis count or band height changes. Every lane-aware surface below reads this one struct.
    laneLayout_ = makeLaneLayout(project, lanes, scriptLinePos, scriptLineSize, laneScroll_);
    laneScroll_ = laneLayout_.scroll;

    stepAxisEmphasis(project); // advance the script-line emphasis ease once per frame, before any script line reads it

    std::array<AxisEntry, kStandardAxisCount> stripEntries{};
    int stripCount = 0;
    for (const auto &ax : project.axes) {
        if (!ax.showInStrip)
            continue;
        ImU32 color = standardAxisColor(ax.role);
        stripEntries[stripCount++] = {.role = ax.role,
                                      .color = color,
                                      .isActive = project.state.activeAxis == ax.role,
                                      .inEditSet = editSet.test(static_cast<size_t>(ax.role))};
    }

    std::array<AxisEntry, kStandardAxisCount> scriptLineEntries{};
    int scriptLineCount = 0;
    for (int si = 0; si < stripCount; ++si) {
        const auto &e = stripEntries[si];
        const auto &ax = project.axes[static_cast<size_t>(e.role)];
        if (e.isActive || ax.isVisible || e.inEditSet)
            scriptLineEntries[scriptLineCount++] = e;
    }
    std::stable_sort(scriptLineEntries.begin(), scriptLineEntries.begin() + scriptLineCount,
                     [](const AxisEntry &a, const AxisEntry &b) { return !a.isActive && b.isActive; });

    // ── Left strip ──────────────────────────────────────────────────────────
    renderStrip(project, eq, drawList, pos, size, stripPos, stripSize, scriptLinePos, stripEntries.data(), stripCount,
                hasGroup);

    // ── Script-line area ──────────────────────────────────────────────────────────
    // Lanes mode draws one lane per strip row (so script lines line up with their labels and hidden rows keep an
    // empty lane), in stable strip order; Overlay draws the filtered, active-last-sorted z-stack.
    const AxisEntry *scriptLineList = lanes ? stripEntries.data() : scriptLineEntries.data();
    const int scriptLineListCount = lanes ? stripCount : scriptLineCount;
    renderScriptLines(project, drawList, waveform, pos, size, scriptLinePos, scriptLineSize, offsetTime, scriptLineList,
                      scriptLineListCount, lanes, windowHovered);

    // ── Script-line area interaction ───────────────────────────────────────────────
    // Each lane is its own InvisibleButton, so ImGui reports which lane the cursor is over (no manual
    // Y→lane hit-test) and the lanes are real, queryable widgets. Overlay is the degenerate single lane:
    // one ##timeline button spanning the band. The lane under the cursor is the focus-click target; the
    // active axis stays the edit target (its pos is read against its own lane band below).
    // anyLaneHovered: the cursor is over the interactable band. laneAxis: which axis's lane it is over (Count
    // in Overlay, where the single ##timeline button spans the band) — the target of a strip-mirroring focus
    // click; the gesture target stays the active axis.
    bool anyLaneHovered = false;
    StandardAxis laneAxis = StandardAxis::Count;
    if (lanes) {
        int ord = 0;
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (!project.axes[i].showInStrip)
                continue;
            const LaneRect lr = laneRectAt(laneLayout_, ord++);
            if (laneLayout_.maxScroll > 0.f) // the scrollbar (submitted below) overlaps the lane's right edge
                ImGui::SetNextItemAllowOverlap();
            ImGui::SetCursorScreenPos(lr.pos);
            ImGui::InvisibleButton(fmtScratch("##lane_{}", i), lr.size,
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (ImGui::IsItemHovered()) {
                anyLaneHovered = true;
                laneAxis = static_cast<StandardAxis>(i);
            }
        }
    } else {
        ImGui::SetCursorScreenPos(scriptLinePos);
        ImGui::InvisibleButton("##timeline", scriptLineSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        anyLaneHovered = ImGui::IsItemHovered();
    }

    float mouseX = ImGui::GetMousePos().x;
    float mouseY = ImGui::GetMousePos().y;
    bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    bool ctrlHeld = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

    // Lane scrollbar (Lanes overflow only): drawn on top of the script lines; sets overLaneScrollbar so the
    // script-line gestures below ignore a press that lands on it.
    const bool overLaneScrollbar = renderLaneScrollbar(drawList, scriptLinePos, scriptLineSize, mouseY);

    // Script-line gestures (seek, edit, box-select) always target the active axis and fan across the edit group,
    // exactly like Overlay — that is what keeps grouping working and selection group-aware in Lanes. The
    // only Lanes difference is geometry: pos is read against the active axis's own lane band. Choosing the
    // active axis / group is the strip's job; clicking a lane mirrors a strip-row click (handled below).
    const StandardAxis gestureAxis = project.state.activeAxis;
    const LaneRect gestureLane = lanes ? laneRectForAxis(project, laneLayout_, gestureAxis)
                                       : LaneRect{.pos = scriptLinePos, .size = scriptLineSize};
    const bool gestureLocked =
        gestureAxis < StandardAxis::Count && project.axes[static_cast<size_t>(gestureAxis)].isLocked;

    struct NearHit {
        StandardAxis axis = StandardAxis::Count;
        const ScriptAxisAction *action = nullptr;
    };
    auto findNearest = [&]() -> NearHit {
        NearHit best;
        if (!project.timelineView.showPoints) // points hidden: no hit-testing, clicks fall through to seek
            return best;
        if (gestureAxis >= StandardAxis::Count)
            return best;
        const auto &gestureAx = project.axes[static_cast<size_t>(gestureAxis)];
        const auto *near = findNearestAction(project, gestureAxis, gestureAx.actions, mouseX, mouseY,
                                             viewState.visibleTime, offsetTime, gestureLane.pos, gestureLane.size);
        if (near) {
            best.axis = gestureAxis;
            best.action = near;
        }
        return best;
    };

    if (anyLaneHovered && !overLaneScrollbar) {
        NearHit hit = findNearest();

        if (hit.action != nullptr) {
            // A locked axis no-ops every point gesture; signal that with the not-allowed cursor so the
            // dead click isn't mistaken for an unresponsive UI.
            ImGui::SetMouseCursor(gestureLocked ? ImGuiMouseCursor_NotAllowed : ImGuiMouseCursor_Hand);
            if (ImGui::BeginTooltip()) {
                ImGui::Text(ICON_CLOCK_3 " %s", TimeUtil::formatTimeShort(hit.action->at));
                ImGui::Text(ICON_MOVE_VERTICAL " %d", hit.action->pos);
                // The script line's modifier gestures have no on-screen affordance; surface them here so a new
                // user can discover add/value-move/select. On a locked axis they no-op, so name the lock
                // instead so the dead clicks read as intentional.
                ImGui::Separator();
                ImGui::TextDisabled("%s", gestureLocked ? Str::TlAxisLocked.c_str() : Str::TlPointGestures.c_str());
                ImGui::EndTooltip();
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Lanes: a click in a lane that isn't the active one is a focus click — it changes the active
            // axis / group exactly as a click on that strip row would, then consumes the click (no seek or
            // edit). This keeps grouping working from the script line: Ctrl-click toggles the lane in/out of the
            // group, clicking a current group member re-leads to it (keeping the group), and a plain click
            // on a non-member selects it and dissolves the group. The next click, now on the active lane,
            // interacts normally.
            const bool focusLane = lanes && laneAxis < StandardAxis::Count && laneAxis != project.state.activeAxis;
            if (focusLane) {
                if (ctrlHeld) {
                    AxisRoles roles = project.effectiveEditSet();
                    roles.flip(static_cast<size_t>(laneAxis));
                    StandardAxis lead = project.state.activeAxis;
                    if (lead >= StandardAxis::Count || !roles.test(static_cast<size_t>(lead))) {
                        lead = laneAxis;
                        for (size_t k = 0; k < kStandardAxisCount; ++k)
                            if (roles.test(k)) {
                                lead = static_cast<StandardAxis>(k);
                                break;
                            }
                    }
                    eq.push(SetAxisGroupingEvent{.roles = roles, .lead = lead});
                } else if (hasGroup && project.state.axesGrouping.test(static_cast<size_t>(laneAxis))) {
                    eq.push(SetAxisGroupingEvent{.roles = project.state.axesGrouping, .lead = laneAxis}); // re-lead
                } else {
                    eq.push(AxisSelectedEvent{.role = laneAxis}); // select + dissolve
                }
            } else if (hit.action != nullptr && !gestureLocked) {
                if (ctrlHeld) {
                    // Toggle selection, no seek, no drag — a Point gesture (degenerate single-time range)
                    // routed through the active selection mode.
                    eq.push(SelectRequestEvent{.gesture = SelectGesture::Point,
                                               .axis = hit.axis,
                                               .startTime = hit.action->at,
                                               .endTime = hit.action->at,
                                               .pos = hit.action->pos,
                                               .additive = true});
                } else {
                    // Start drag candidate — resolves to seek+select or drag on mouse up/move
                    editState.hasDragCandidate = true;
                    editState.candidateAxis = hit.axis;
                    editState.candidateAt = hit.action->at;
                    editState.candidatePos = hit.action->pos;
                }
            } else if (shiftHeld && project.timelineView.showPoints && !gestureLocked &&
                       gestureAxis < StandardAxis::Count) {
                // Add point at cursor position (pos read against the target lane's band).
                double at = screenXToTime(mouseX, viewState.visibleTime, offsetTime, scriptLinePos, scriptLineSize);
                int pVal = screenYToPos(mouseY, gestureLane.pos, gestureLane.size);
                eq.push(ofs::EditRequestEvent{
                    .intent = {.kind = ofs::EditIntentKind::AddPoint, .axis = gestureAxis, .time = at, .pos = pVal}});
            } else {
                // Empty area: will seek on release or start box-select on drag.
                editState.emptyClickPending = true;
                editState.emptyClickTime =
                    screenXToTime(mouseX, viewState.visibleTime, offsetTime, scriptLinePos, scriptLineSize);
            }
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            // Open on release, matching the region band's context menu (BandBar) so right-click feels
            // identical across adjacent timeline surfaces. Lanes targets the row right-clicked; Overlay the
            // active axis.
            StandardAxis ctxTarget = lanes ? laneAxis : project.state.activeAxis;
            ctxAxis = (ctxTarget < StandardAxis::Count) ? ctxTarget : StandardAxis::Count;
            ImGui::OpenPopup("##timeline_ctx");
        }
    }

    renderContextMenu(project, eq, videoPlayer);
}

void ScriptTimelineWindow::renderContextMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) {
    if (ImGui::BeginPopup("##timeline_ctx")) {
        if (ctxAxis < StandardAxis::Count) {
            const auto &ctxAx = project.axes[static_cast<size_t>(ctxAxis)];
            if (ctxAxis != StandardAxis::L0) {
                if (ImGui::MenuItem(Str::TlHideFromPanel.id("tl_hide_from_panel")))
                    eq.push(ToggleAxisPanelVisibilityEvent{.axisRole = ctxAxis, .inPanel = false});
                ImGui::Separator();
            }
            bool locked = ctxAx.isLocked;
            if (ImGui::MenuItem((locked ? Str::TlUnlock : Str::TlLock)
                                    .iconId(locked ? ICON_LOCK_OPEN : ICON_LOCK, "tl_lock_toggle")))
                eq.push(ToggleAxisLockEvent{.axisRole = ctxAxis, .locked = !locked});
            if (isScratchAxis(ctxAxis)) {
                ImGui::Separator();
                if (ImGui::MenuItem(Str::TlDelete.iconId(ICON_TRASH, "tl_delete_axis")))
                    eq.push(RemoveAxisEvent{.axisRole = ctxAxis});
            }
            if (!ctxAx.selection.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem(Str::TlAddProcessingRegion.iconId(ICON_PLUS, "add_processing_region"))) {
                    double minAt = ctxAx.selection.begin()->at;
                    double maxAt = std::prev(ctxAx.selection.end())->at;
                    eq.push(CreateRegionEvent{.axisRole = ctxAxis,
                                              .startTime = minAt,
                                              .endTime = maxAt,
                                              .timelineDuration = videoPlayer.getDuration()});
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        // View toggle: hide the source points (rendering + interaction).
        ImGui::Separator();
        bool showPoints = project.timelineView.showPoints;
        if (ImGui::MenuItem(Str::TlShowPoints.id("tl_show_points"), nullptr, &showPoints))
            eq.push(SetTimelineShowPointsEvent{showPoints});

        bool showWaveform = project.timelineView.showAudioWaveform;
        if (ImGui::MenuItem(Str::TlShowWaveform.id("tl_show_waveform"), nullptr, &showWaveform))
            eq.push(SetTimelineShowWaveformEvent{showWaveform});

        // Script-line layout: z-stacked Overlay vs one row per axis (Lanes).
        ImGui::Separator();
        const TimelineLayout layout = project.timelineView.layout;
        if (ImGui::MenuItem(Str::TlLayoutOverlay.id("tl_layout_overlay"), nullptr, layout == TimelineLayout::Overlay))
            eq.push(SetTimelineLayoutEvent{TimelineLayout::Overlay});
        if (ImGui::MenuItem(Str::TlLayoutLanes.id("tl_layout_lanes"), nullptr, layout == TimelineLayout::Lanes))
            eq.push(SetTimelineLayoutEvent{TimelineLayout::Lanes});

        // Scripting overlay settings (Frame/Tempo), folded into the timeline's own context menu.
        ImGui::Separator();
        renderOverlayMenu(project, eq, videoPlayer);
        ImGui::EndPopup();
    }
}

void ScriptTimelineWindow::renderOverlayMenu(const ScriptProject &project, EventQueue &eq,
                                             VideoPlayer &videoPlayer) const {
    if (ImGui::BeginMenu(Str::TlOverlay.id("tl_overlay_menu"))) {
        auto ov = project.overlay;

        // Font-relative field width (was a fixed 160 px) so a longer translated combo entry fits
        // and the column scales with font/DPI inside the auto-sizing popup.
        const float fieldW = ImGui::GetFontSize() * 10.f;
        bool changed = ofs::ui::renderOverlayControls(ov, Str::TlOverlay, [&](TrKey label) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(fieldW);
        });

        if (ov.overlay == ScriptingOverlay::Frame) {
            const char *fpsStr = fmtScratch("{:.3f}", videoPlayer.getFps());
            ImGui::TextUnformatted(Str::TlVideoFps.fmt(fpsStr));
        }

        if (changed)
            eq.push(OverlaySettingsChangedEvent{ov});

        ImGui::EndMenu();
    }
}

bool ScriptTimelineWindow::renderSettingsBody(const ScriptProject &project, EventQueue &eq,
                                              VideoPlayer &videoPlayer) const {
    // ── View ──────────────────────────────────────────────────────────────────
    if (ofs::ui::beginForm("##tl_view_form")) {
        // Script-line layout: z-stacked Overlay vs one row per axis (Lanes). Visible labels are localized but
        // each carries a stable ###id; the stored value is the enum index, so translating never changes
        // what's persisted.
        ofs::ui::formRow(Str::TlLayout);
        const char *layoutNames[] = {Str::TlLayoutOverlay.id("layout_overlay"), Str::TlLayoutLanes.id("layout_lanes")};
        int currentLayout = static_cast<int>(project.timelineView.layout);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("###tlset_layout", &currentLayout, layoutNames, IM_ARRAYSIZE(layoutNames)))
            eq.push(SetTimelineLayoutEvent{static_cast<TimelineLayout>(currentLayout)});

        ofs::ui::formRow(Str::TlShowPoints);
        bool showPoints = project.timelineView.showPoints;
        if (ImGui::Checkbox("###tlset_show_points", &showPoints))
            eq.push(SetTimelineShowPointsEvent{showPoints});

        ofs::ui::formRow(Str::TlShowWaveform);
        bool showWaveform = project.timelineView.showAudioWaveform;
        if (ImGui::Checkbox("###tlset_show_waveform", &showWaveform))
            eq.push(SetTimelineShowWaveformEvent{showWaveform});
        ofs::ui::endForm();
    }

    // ── Axes ──────────────────────────────────────────────────────────────────
    // Per-axis quick toggles for script-line visibility (isVisible) and strip presence (showInStrip) — the
    // same two switches the Axes menu and the strip eye-icon expose, gathered here so they can be
    // flipped without hunting through menus. Listing mirrors the Axes menu: standard axes always, a
    // scratch axis only once it exists(). L0 is pinned in the panel, so its panel toggle is disabled.
    ImGui::SeparatorText(Str::AppMenuAxes);
    if (ImGui::BeginTable("##tl_axes", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        const float toggleW = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.x * 2.f;
        ImGui::TableSetupColumn("##axis_name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##axis_vis", ImGuiTableColumnFlags_WidthFixed, toggleW);
        ImGui::TableSetupColumn("##axis_panel", ImGuiTableColumnFlags_WidthFixed, toggleW);

        // Icon header row (eye = shown in timeline, panel = shown in the left strip), with the menu's own
        // labels as tooltips so the two columns read unambiguously. Each glyph is centered over its
        // fixed toggle column so it sits above the checkbox beneath it.
        // Center over the checkbox square (GetFrameHeight() wide, left-aligned in the cell), not over the
        // whole content region — the cell is wider than the square, so centering on the region would drift right.
        auto centeredIcon = [](const char *icon) {
            const float off = (ImGui::GetFrameHeight() - ImGui::CalcTextSize(icon).x) * 0.5f;
            if (off > 0.f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
            ImGui::TextUnformatted(icon);
        };
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        centeredIcon(ICON_EYE);
        ImGui::SetItemTooltip("%s", Str::AppAxesShowTimeline.c_str());
        ImGui::TableNextColumn();
        centeredIcon(ICON_PANEL_LEFT);
        ImGui::SetItemTooltip("%s", Str::AppAxesShowPanel.c_str());

        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            const auto role = static_cast<StandardAxis>(i);
            const auto &axis = project.axes[i];
            if (isScratchAxis(role) && !axis.exists())
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ofs::loc::localizedAxisName(role));

            ImGui::TableNextColumn();
            bool vis = axis.isVisible;
            if (ImGui::Checkbox(fmtScratch("###tlset_vis_{}", i), &vis))
                eq.push(ToggleAxisVisibilityEvent{.axisRole = role, .visible = vis});

            ImGui::TableNextColumn();
            bool inPanel = axis.showInStrip;
            const bool isL0 = role == StandardAxis::L0;
            ImGui::BeginDisabled(isL0);
            if (ImGui::Checkbox(fmtScratch("###tlset_panel_{}", i), &inPanel) && !isL0)
                eq.push(ToggleAxisPanelVisibilityEvent{.axisRole = role, .inPanel = inPanel});
            ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }

    // ── Scripting overlay (Frame/Tempo) ────────────────────────────────────────
    auto ov = project.overlay;
    bool changed = false;

    ImGui::SeparatorText(Str::TlOverlay);
    if (ofs::ui::beginForm("##tl_overlay_form")) {
        changed = ofs::ui::renderOverlayControls(ov, Str::PcfType, [](TrKey label) {
            ofs::ui::formRow(label);
            ImGui::SetNextItemWidth(-FLT_MIN);
        });
        ofs::ui::endForm();
    }

    if (ov.overlay == ScriptingOverlay::Frame) {
        const char *fpsStr = fmtScratch("{:.3f}", videoPlayer.getFps());
        ImGui::TextDisabled("%s", Str::TlVideoFps.fmt(fpsStr));
    }

    if (changed)
        eq.push(OverlaySettingsChangedEvent{ov});

    // Rendered as a click-away flyout (dismissOnClickAway): ImGui closes it on a click outside or
    // Escape, so the body never self-closes — return false.
    return false;
}

void ScriptTimelineWindow::render(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                                  WaveformRenderer &waveform) {
    m_regionClickedThisFrame = false;

    // NoNavInputs: this panel owns the unmodified arrow/Space editor shortcuts (frame-step, play/pause).
    // Without it, focusing the timeline would make ImGui keyboard nav claim those keys (see Application.cpp).
    ImGui::Begin(Str::TlTitle.id("timeline"), nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);

    ImVec2 outerPos = ImGui::GetCursorScreenPos();
    ImVec2 outerSize = ImGui::GetContentRegionAvail();

    // Region bar height tracks the font so its band labels are never clipped.
    const float regionBarH = ofs::ui::bandBarHeight();
    // Breathing room between the script-line area and the region bar (anchored at the bottom).
    const float regionBarGap = ImGui::GetStyle().ItemSpacing.y;

    // Floor the panel so the script-line band keeps at least one usable lane's height in either layout:
    // Separate-lanes already pins each lane to minLaneHeight() and scrolls, but the Stacked-lines overlay
    // is a single band with no such floor, so without this it collapses well below a lane. The band is the
    // area minus the region bar, so the panel minimum is that floor plus the region bar's reserved height.
    const float minPanelH = regionBarH + regionBarGap + minLaneHeight();
    if (outerSize.y < minPanelH)
        outerSize.y = minPanelH;

    const float stripW = stripWidth();
    // Mirror renderTimeline: the strip is trimmed by one item-spacing and the script line (with its overlay
    // grid and the region bar below) fills the freed column, flush with the strip.
    const float stripMargin = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 scriptLinePos = {outerPos.x + stripW - stripMargin, outerPos.y};
    const ImVec2 scriptLineSize = {outerSize.x - stripW + stripMargin, outerSize.y - regionBarH - regionBarGap};
    const bool lanes = project.timelineView.layout == TimelineLayout::Lanes;

    // Smooth zoom
    auto ticks = static_cast<uint32_t>(SDL_GetTicks());
    float zoomProgress = std::clamp(static_cast<float>(ticks - viewState.zoomUpdateTime) / 150.0f, 0.0f, 1.0f);
    zoomProgress = easeOutExpo(zoomProgress);
    viewState.visibleTime =
        viewState.previousVisibleTime +
        (viewState.targetVisibleTime - viewState.previousVisibleTime) * static_cast<double>(zoomProgress);

    double offsetTime = project.playback.cursorPos - viewState.visibleTime / 2.0;

    eq.push(UpdateTimelineViewEvent{.visibleTime = viewState.visibleTime, .offsetTime = offsetTime});

    // Plain IsWindowHovered (no ChildWindows): the default popup hierarchy counts a context menu
    // opened from this window as a child, so ChildWindows would let a scroll over that popup zoom
    // the timeline behind it. The window owns no real child windows, so the flag only causes that leak.
    // The lanes overflow the band (laneLayout_.maxScroll, resolved last frame) when even the minimum lane
    // height won't fit them all; then Shift+wheel scrolls the lanes vertically while plain wheel still
    // zooms. makeLaneLayout reclamps laneScroll_ next frame, so this only needs to stay non-negative.
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && laneLayout_.maxScroll > 0.f && ImGui::GetIO().KeyShift) {
            laneScroll_ = std::max(0.f, laneScroll_ - wheel * laneLayout_.laneH); // ~one lane per notch
        } else if (wheel != 0) {
            viewState.previousVisibleTime = viewState.visibleTime;
            viewState.targetVisibleTime *= (wheel > 0) ? 0.8 : 1.25;
            viewState.targetVisibleTime = std::clamp(viewState.targetVisibleTime, 0.1, 300.0);
            viewState.zoomUpdateTime = ticks;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            double timeDelta =
                static_cast<double>(-ImGui::GetIO().MouseDelta.x / scriptLineSize.x) * viewState.visibleTime;
            eq.push(SeekEvent{project.playback.cursorPos + timeDelta});
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
            // Clear every axis's selection (SetAxisSelectionEvent is per-axis by design).
            for (size_t i = 0; i < kStandardAxisCount; ++i)
                if (!project.axes[i].selection.empty())
                    eq.push(SetAxisSelectionEvent{.axis = static_cast<StandardAxis>(i), .selection = {}});
        }
    }

    if (selectionState.isSelecting) {
        float mouseX = ImGui::GetMousePos().x;
        selectionState.relSelEnd = std::clamp((mouseX - scriptLinePos.x) / scriptLineSize.x, 0.0f, 1.0f);
    }

    renderTimeline(project, eq, videoPlayer, waveform, outerPos,
                   {outerSize.x, outerSize.y - regionBarH - regionBarGap});
    renderOverlay(project, scriptLinePos, scriptLineSize, offsetTime);
    renderPlayhead(videoPlayer, scriptLinePos, scriptLineSize, offsetTime);

    // The region bar's track keeps the full strip width on its left (edge at stripW), so it stays inset
    // by the strip margin from the script line above — that inset is the small gap the corner gear sits in.
    // Bands still map through the script-line geometry, so they line up with the script line; bands are clamped to
    // the track's left edge (drawBandBar), so only the visible track rect is inset.
    const ImVec2 regionBarMin = {outerPos.x + stripW, outerPos.y + outerSize.y - regionBarH};
    const ImVec2 regionBarMax = {scriptLinePos.x + scriptLineSize.x, outerPos.y + outerSize.y};
    renderRegionBar(videoPlayer, project, eq, regionBarMin, regionBarMax, scriptLinePos, scriptLineSize, offsetTime);

    // Two corner buttons in the dead space below the left strip, beside the region bar: the settings gear
    // and a layout toggle. The gear opens the timeline settings modal (a discoverable entry point that
    // doesn't depend on knowing the right-click gesture); the toggle flips stacked↔lanes in one click,
    // the most-reached-for view setting. The pair splits the corner width with one item-spacing between
    // them, and the whole run stops one spacing short of the region bar's left edge so it keeps a margin.
    const ImVec2 cornerMin = {outerPos.x, regionBarMin.y};
    const float cornerMargin = ImGui::GetStyle().ItemSpacing.x;
    const float cornerW = regionBarMin.x - cornerMargin - cornerMin.x;
    const float btnW = (cornerW - cornerMargin) * 0.5f;

    // Empty-label button for the frame, with the icon drawn centered on top. ImGui::Button centers by the
    // glyph's advance width, but an icon glyph's visible ink isn't centered within its advance, so it reads
    // off-center; measuring and placing it ourselves centers the ink. This also sidesteps the half-width
    // clipping that the default frame padding caused.
    const auto iconButton = [](const char *id, const char *icon, ImVec2 pos, ImVec2 size) {
        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::Button(id, size);
        const ImVec2 textSize = ImGui::CalcTextSize(icon);
        ImGui::GetWindowDrawList()->AddText(
            {pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f},
            ImGui::GetColorU32(ImGuiCol_Text), icon);
        return clicked;
    };

    // Raise the settings modal through the shared ModalManager (chrome, centering, FIFO serialization,
    // shutdown teardown). Built here inside the live frame so the width is font-relative, like every
    // other showCustomModal call. The body captures the app-lifetime project/eq/videoPlayer (the
    // ModalManager is torn down before them), reads them fresh each frame, and returns true to close.
    if (iconButton("###tl_settings_gear", ICON_SETTINGS, cornerMin, {btnW, regionBarH}))
        showCustomModal(eq, {.title = Str::TlSettingsTitle.c_str(),
                             .width = ImGui::GetMainViewport()->Size.x * 0.45f, // match the Preferences window scale
                             .body = [this, p = &project, eqp = &eq,
                                      vp = &videoPlayer]() { return renderSettingsBody(*p, *eqp, *vp); },
                             .dismissOnClickAway = true}); // click-away / Escape closes it (no Close button)

    // Layout toggle: the icon shows the current layout (layers = stacked, rows = lanes); clicking switches
    // to the other. Mirrors the right-click menu / settings-combo writes — both push SetTimelineLayoutEvent.
    const bool lanesLayout = project.timelineView.layout == TimelineLayout::Lanes;
    if (iconButton("###tl_layout_toggle", lanesLayout ? ICON_ROWS_2 : ICON_LAYERS_2,
                   {cornerMin.x + btnW + cornerMargin, cornerMin.y}, {btnW, regionBarH}))
        eq.push(SetTimelineLayoutEvent{lanesLayout ? TimelineLayout::Overlay : TimelineLayout::Lanes});
    ImGui::SetItemTooltip("%s", Str::TlLayoutToggleTip.c_str());

    // ── Edge scroll during box selection or region resize/move ──────────────
    {
        bool scrollActive = selectionState.isSelecting || regionDragState.mode != ofs::ui::BandBarDragState::Mode::None;
        if (scrollActive) {
            float mouseX = ImGui::GetMousePos().x;
            const float kEdgeZone = ImGui::GetFontSize() * 3.3f; // ≈60 px at the 18 px default, DPI-relative
            double maxSpeed = viewState.visibleTime * 2.0;
            auto dt = static_cast<double>(ImGui::GetIO().DeltaTime);

            double seekDelta = 0.0;
            if (mouseX < scriptLinePos.x + kEdgeZone) {
                float t = std::clamp(1.0f - (mouseX - scriptLinePos.x) / kEdgeZone, 0.0f, 1.0f);
                seekDelta = -maxSpeed * static_cast<double>(t) * dt;
            } else if (mouseX > scriptLinePos.x + scriptLineSize.x - kEdgeZone) {
                float t = std::clamp(1.0f - (scriptLinePos.x + scriptLineSize.x - mouseX) / kEdgeZone, 0.0f, 1.0f);
                seekDelta = maxSpeed * static_cast<double>(t) * dt;
            }
            if (seekDelta != 0.0)
                eq.push(SeekEvent{project.playback.cursorPos + seekDelta});
        }
    }

    // ── Drag candidate: promote to drag or fire seek+select on click ────────
    if (editState.hasDragCandidate) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            editState.isDragging = true;
            editState.draggingAxis = editState.candidateAxis;
            editState.dragFromAt = editState.candidateAt;
            editState.originalDragAt = editState.candidateAt;
            editState.dragPos = editState.candidatePos;
            editState.hasDragCandidate = false;
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Click: seek to the point and select only it (a Point gesture, replacing the selection).
            eq.push(SeekEvent{editState.candidateAt});
            eq.push(SelectRequestEvent{.gesture = SelectGesture::Point,
                                       .axis = editState.candidateAxis,
                                       .startTime = editState.candidateAt,
                                       .endTime = editState.candidateAt,
                                       .pos = editState.candidatePos,
                                       .additive = false});
            editState.hasDragCandidate = false;
        }
    }

    // ── Empty-area click: seek on release, box-select on drag ────────────────
    if (editState.emptyClickPending) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            // Box-select stays available with points hidden — it is range
            // selection, not per-point interaction. Targets the active axis and fans across the edit group
            // (the same in Overlay and Lanes), so a marquee selects the group, never one lane.
            if (project.state.activeAxis < StandardAxis::Count) {
                const auto &activeAx = project.axes[static_cast<size_t>(project.state.activeAxis)];
                if (!activeAx.isLocked) {
                    selectionState.isSelecting = true;
                    selectionState.absSelStart = editState.emptyClickTime;
                }
            }
            editState.emptyClickPending = false;
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            eq.push(SeekEvent{editState.emptyClickTime});
            editState.emptyClickPending = false;
        }
    }

    // ── Active drag ──────────────────────────────────────────────────────────
    if (editState.isDragging && editState.draggingAxis < StandardAxis::Count) {
        const auto &dragAx = project.axes[static_cast<size_t>(editState.draggingAxis)];
        if (!dragAx.isLocked) {
            float mouseX = ImGui::GetMousePos().x;
            float mouseY = ImGui::GetMousePos().y;
            bool altHeld = ImGui::GetIO().KeyAlt;

            double newDragTime = altHeld ? editState.originalDragAt
                                         : std::max(0.0, screenXToTime(mouseX, viewState.visibleTime, offsetTime,
                                                                       scriptLinePos, scriptLineSize));
            // Map the vertical drag through the dragged axis's lane band so pos tracks the cursor 1:1
            // (a full-rect mapping would scale wrong once the lane is a fraction of the height).
            LaneRect dragLane = lanes ? laneRectForAxis(project, laneLayout_, editState.draggingAxis)
                                      : LaneRect{.pos = scriptLinePos, .size = scriptLineSize};
            int newDragPos = screenYToPos(mouseY, dragLane.pos, dragLane.size);

            bool timeChanged = (newDragTime != editState.dragFromAt);
            if (timeChanged || newDragPos != editState.dragPos) {
                bool hasConflict = timeChanged && dragAx.actions.contains(ScriptAxisAction{newDragTime, 0});
                if (!hasConflict) {
                    // First move of the drag is the gesture boundary (snapshot); the rest continue it.
                    eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MovePoint,
                                                        .axis = editState.draggingAxis,
                                                        .time = newDragTime,
                                                        .fromTime = editState.dragFromAt,
                                                        .pos = newDragPos},
                                             .gesture =
                                                 editState.dragMoved ? GesturePhase::Continue : GesturePhase::Begin});
                    editState.dragMoved = true;
                    editState.dragFromAt = newDragTime;
                    editState.dragPos = newDragPos;
                }
            }

            ImGui::SetMouseCursor(altHeld ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            editState.isDragging = false;
            editState.draggingAxis = StandardAxis::Count;
            editState.dragMoved = false;
        }
    }

    // Box selection commit
    if (selectionState.isSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        selectionState.isSelecting = false;
        double relSel1 = (selectionState.absSelStart - offsetTime) / viewState.visibleTime;
        double minRel = std::min(relSel1, selectionState.relSelEnd);
        double maxRel = std::max(relSel1, selectionState.relSelEnd);
        double startTime = offsetTime + viewState.visibleTime * minRel;
        double endTime = offsetTime + viewState.visibleTime * maxRel;
        if (endTime - startTime > 0.008) {
            bool additive = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            if (project.state.activeAxis < StandardAxis::Count) {
                const auto &activeAx = project.axes[static_cast<size_t>(project.state.activeAxis)];
                if (!activeAx.isLocked)
                    eq.push(SelectRequestEvent{.gesture = SelectGesture::Box,
                                               .axis = project.state.activeAxis,
                                               .startTime = startTime,
                                               .endTime = endTime,
                                               .additive = additive});
            }
        }
    }

    ImGui::End();
}

void ScriptTimelineWindow::renderPlayhead(VideoPlayer &videoPlayer, const ImVec2 &pos, const ImVec2 &size,
                                          double offsetTime) const {
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float kPlayCursorWidth = ofs::theme::GetStyleVar(AppVar_ScriptPlayCursorWidth);
    const float kSeekCursorWidth = ofs::theme::GetStyleVar(AppVar_ScriptSeekCursorWidth);

    float actualX =
        ImFloor(timeToScreenX(videoPlayer.getActualPosition(), viewState.visibleTime, offsetTime, pos, size)) + 0.5f;
    if (actualX >= pos.x && actualX <= pos.x + size.x) {
        const ImU32 playCol = ofs::theme::GetColorU32(AppCol_ScriptPlayCursor);
        constexpr float triOfs = 0.5f;
        dl->AddLine({actualX, pos.y}, {actualX, pos.y + size.y}, playCol, kPlayCursorWidth);
        float capH = ImGui::GetFontSize() * 0.4f;
        float capW = capH * 0.6f;
        dl->AddTriangleFilled({actualX - capW + triOfs, pos.y}, {actualX + capW + triOfs, pos.y},
                              {actualX + triOfs, pos.y + capH}, playCol);
    }

    float curX = ImFloor(pos.x + size.x * 0.5f) + 0.5f;
    const ImU32 seekCol = ofs::theme::GetColorU32(AppCol_ScriptSeekCursor);
    constexpr float triOfs = 0.5f;
    dl->AddLine({curX, pos.y}, {curX, pos.y + size.y}, seekCol, kSeekCursorWidth);
    float capH = ImGui::GetFontSize() * 0.55f;
    float capW = capH * 0.65f;
    dl->AddTriangleFilled({curX - capW + triOfs, pos.y}, {curX + capW + triOfs, pos.y}, {curX + triOfs, pos.y + capH},
                          seekCol);
}

void ScriptTimelineWindow::renderOverlay(const ScriptProject &project, const ImVec2 &pos, const ImVec2 &size,
                                         double offsetTime) const {
    const auto &modeState = project.overlay;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(pos, pos + size, true);

    constexpr int kLineFadeStart = 60;
    constexpr int kLineFadeEnd = 100;
    const float coarseWidth = ofs::theme::GetStyleVar(AppVar_OverlayLineMajorWidth);

    auto lineAlpha = [](int count) -> float {
        if (count >= kLineFadeEnd)
            return 0.0f;
        if (count <= kLineFadeStart)
            return 1.0f;
        float t = static_cast<float>(count - kLineFadeStart) / static_cast<float>(kLineFadeEnd - kLineFadeStart);
        return 1.0f - easeOutExpo(t);
    };

    auto applyAlpha = [](ImU32 color, float alpha) -> ImU32 {
        auto a = static_cast<ImU32>(static_cast<float>((color >> 24) & 0xFF) * alpha);
        return (color & 0x00FFFFFF) | (a << 24);
    };

    if (modeState.overlay == ScriptingOverlay::Frame) {
        double frameTime = 1.0 / modeState.frameFps;
        int startFrame = static_cast<int>(std::floor(offsetTime / frameTime));
        int endFrame = static_cast<int>(std::ceil((offsetTime + viewState.visibleTime) / frameTime));
        int drawnLines = endFrame - startFrame + 1;

        int coarseCount = drawnLines / 10;
        float fineAlpha = lineAlpha(drawnLines);
        float coarseAlpha = lineAlpha(coarseCount);

        if (coarseAlpha > 0.0f) {
            for (int f = startFrame; f <= endFrame; ++f) {
                bool isCoarse = (f % 10 == 0);
                float alpha = isCoarse ? coarseAlpha : fineAlpha;
                if (alpha <= 0.0f)
                    continue;
                float x =
                    timeToScreenX(static_cast<double>(f) * frameTime, viewState.visibleTime, offsetTime, pos, size);
                ImU32 color = applyAlpha(isCoarse ? ofs::theme::GetColorU32(AppCol_OverlayLineMajor)
                                                  : ofs::theme::GetColorU32(AppCol_OverlayLineMinor),
                                         alpha);
                drawList->AddLine({x, pos.y}, {x, pos.y + size.y}, color, isCoarse ? coarseWidth : 1.0f);
            }
        }
    } else if (modeState.overlay == ScriptingOverlay::Tempo) {
        const int mi = std::clamp(modeState.tempoMeasureIndex, 0, kTempoSubdivisionCount - 1);
        const double beatDuration = 60.0 / modeState.tempoBpm;          // seconds per beat
        const double beatTime = beatDuration * kTempoBeatMultiples[mi]; // seconds per grid line
        const double offset = modeState.tempoOffsetSeconds;
        // A measure (downbeat) line falls every measurePeriod grid lines; >= 1 always.
        const int measurePeriod = std::max(1, static_cast<int>(std::lround(4.0 / kTempoBeatMultiples[mi])));

        const int startStep = static_cast<int>(std::floor((offsetTime - offset) / beatTime));
        const int endStep = static_cast<int>(std::ceil((offsetTime + viewState.visibleTime - offset) / beatTime));
        const int drawnLines = endStep - startStep + 1;

        const float subAlpha = lineAlpha(drawnLines);
        const float measureAlpha = lineAlpha(drawnLines / measurePeriod);

        if (measureAlpha > 0.0f) {
            const ImU32 measureColor = applyAlpha(ofs::theme::GetColorU32(AppCol_TempoMeasureLine), measureAlpha);
            const ImU32 subColor = applyAlpha(ofs::theme::GetColorU32(AppCol_OverlayLineMinor), subAlpha);
            const ImU32 labelColor =
                applyAlpha(ofs::theme::GetColorU32(static_cast<AppCol>(ImGuiCol_Text)), measureAlpha);

            for (int s = startStep; s <= endStep; ++s) {
                // floor-mod so measure boundaries stay aligned for negative steps (offset > 0).
                const bool isMeasure = (((s % measurePeriod) + measurePeriod) % measurePeriod) == 0;
                const float alpha = isMeasure ? measureAlpha : subAlpha;
                if (alpha <= 0.0f)
                    continue;
                const double t = offset + static_cast<double>(s) * beatTime;
                const float x = timeToScreenX(t, viewState.visibleTime, offsetTime, pos, size);
                drawList->AddLine({x, pos.y}, {x, pos.y + size.y}, isMeasure ? measureColor : subColor,
                                  isMeasure ? coarseWidth : 1.0f);

                if (isMeasure) {
                    const int measureIdx = static_cast<int>(std::floor(static_cast<double>(s) / measurePeriod));
                    drawList->AddText({x + 3.0f, pos.y}, labelColor, fmtScratch("{}", measureIdx));
                }
            }
        }
    }

    drawList->PopClipRect();
}

void ScriptTimelineWindow::renderRegionBar(VideoPlayer &videoPlayer, const ScriptProject &project, EventQueue &eq,
                                           const ImVec2 &barMin, const ImVec2 &barMax, const ImVec2 &scriptLinePos,
                                           const ImVec2 &scriptLineSize, double offsetTime) {
    const double duration = videoPlayer.getDuration();
    if (duration <= 0.0)
        return;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    // Recessed track fill + soft outline so the region bar is a visible band even with no
    // regions placed (ProcessingPanelBg alone blended into the surrounding panel).
    dl->AddRectFilled(barMin, barMax, ofs::theme::GetColorU32(AppCol_StripBg));
    dl->AddRect(barMin, barMax, ofs::theme::GetColorU32(ImGuiCol_Border));

    // With no regions placed the bar is just an empty track; the region/processing feature is otherwise
    // undiscoverable, so prompt the right-click that creates one (elided so a narrow bar can't overrun).
    if (project.regions.empty()) {
        const char *hint = ofs::ui::elide(Str::TlAddRegionHint.c_str(), (barMax.x - barMin.x) - ImGui::GetFontSize());
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({(barMin.x + barMax.x - ts.x) * 0.5f, (barMin.y + barMax.y - ts.y) * 0.5f},
                    ofs::theme::GetColorU32(ImGuiCol_TextDisabled), hint);
    }

    // Regions are axis-agnostic on the timeline: every region renders here regardless of the
    // active axis. Axis assignment governs only the node graph's I/O, not visibility.
    const int bandCount = static_cast<int>(project.regions.size());
    auto *bandData = ofs::FrameAllocator::instance().allocArray<ofs::ui::BandItem>(bandCount);
    auto *regionIds = ofs::FrameAllocator::instance().allocArray<int>(bandCount);
    int i = 0;
    for (const auto &reg : project.regions) {
        bandData[i] = {.startTime = reg.startTime,
                       .endTime = reg.endTime,
                       .color = reg.color,
                       .name = reg.name,
                       .selected = reg.id == project.procSelRegionId,
                       .hatched = true}; // region bands are hatched to read distinctly from chapter bands
        regionIds[i] = reg.id;
        ++i;
    }
    std::span<const ofs::ui::BandItem> bands(bandData, bandCount);

    const double playheadTime = project.playback.cursorPos;

    auto toX = [&](double t) -> float {
        return timeToScreenX(t, viewState.visibleTime, offsetTime, scriptLinePos, scriptLineSize);
    };
    auto toTime = [&](float x) -> double {
        return screenXToTime(x, viewState.visibleTime, offsetTime, scriptLinePos, scriptLineSize);
    };

    ofs::ui::BandBarCallbacks callbacks;

    callbacks.onDragEnd = [&](int idx, double finalStart, double finalEnd) {
        if (idx < 0 || idx >= bandCount)
            return;
        const auto *reg = project.findRegion(regionIds[idx]);
        if (!reg)
            return;
        // The band only previews locally during the drag, so reg still holds the pre-drag span. A drag
        // released in place (e.g. Alt snap-back) leaves it unchanged — emitting ModifyRegionEvent would
        // still create an undo step (snapshot defaults true) even though onModifyRegion then discards the
        // identical region. Skip the no-op so it costs neither an undo entry nor a re-eval.
        if (finalStart == reg->startTime && finalEnd == reg->endTime)
            return;
        ProcessingRegion updated = *reg;
        updated.startTime = finalStart;
        updated.endTime = finalEnd;
        eq.push(ModifyRegionEvent{.regionId = regionIds[idx], .updatedRegion = updated});
    };

    callbacks.onClick = [&](int idx) {
        if (idx >= 0 && idx < bandCount)
            eq.push(SelectRegionEvent{.regionId = regionIds[idx]});
    };

    callbacks.onRightClick = [&](int idx, double t) {
        if (idx >= 0 && idx < bandCount) {
            ctxRegionId = regionIds[idx];
            ctxRegionClickTime = t;
            m_colorEditRegionId = -1; // each menu-open starts a fresh color gesture (one undo step)
            ImGui::OpenPopup("##region_ctx");
        }
    };

    callbacks.onEmptyRightClick = [&](double t) {
        ctxRegionId = -1;
        ctxRegionClickTime = t;
        ImGui::OpenPopup("##region_ctx");
    };

    ofs::ui::drawBandBar(dl, barMin, barMax, bands, regionDragState, playheadTime, 0.0, duration, 0.5, toX, toTime,
                         callbacks, "##regionbar");

    // A press anywhere on a region bar must not read as a click in empty space, or OfsApp's
    // click-outside check would clear the selection. The press is recorded as a drag candidate (and
    // promoted to an active drag if the cursor moves), both on the mouse-down frame the outside check
    // runs — so flag it here, not in onClick, which only fires on release after the check has passed.
    if (regionDragState.hasDragCandidate || regionDragState.mode != ofs::ui::BandBarDragState::Mode::None)
        m_regionClickedThisFrame = true;

    if (ImGui::BeginPopup("##region_ctx")) {
        // Tooltip for a BeginDisabled()'d menu item explaining why it's unavailable. Disabled items
        // don't hover by default, so allow-when-disabled is required.
        const auto disabledReq = [](const char *msg) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", msg);
        };
        const auto *ctxReg = project.findRegion(ctxRegionId);
        if (ctxReg != nullptr) {
            ImGui::TextDisabled("%s " GLYPH_EN_DASH " %s", TimeUtil::formatTime(ctxReg->startTime, true),
                                TimeUtil::formatTime(ctxReg->endTime, true));
            ImGui::Separator();
            {
                // Per-region band color. ColorEdit3's return value (true on every change) is the
                // reliable commit signal here: the swatch opens a nested picker popup, and
                // IsItemDeactivatedAfterEdit never fires when the user dismisses the picker by clicking
                // outside — which also closes this context menu. The picker fires every frame of a drag,
                // so only the first change snapshots undo (the rest ride snapshot=false): one undo step
                // per gesture, consistent with the region's other edits.
                ImColor colVec(ctxReg->color);
                std::array<float, 3> col = {colVec.Value.x, colVec.Value.y, colVec.Value.z};
                if (ImGui::ColorEdit3(Str::VpcColor.id("region_color"), col.data(), ImGuiColorEditFlags_NoInputs)) {
                    ProcessingRegion updated = *ctxReg;
                    updated.color = static_cast<ImU32>(ImColor(col[0], col[1], col[2], colVec.Value.w));
                    eq.push(ModifyRegionEvent{.regionId = ctxRegionId,
                                              .updatedRegion = updated,
                                              .snapshot = m_colorEditRegionId != ctxRegionId});
                    m_colorEditRegionId = ctxRegionId;
                }
                if (ImGui::IsItemDeactivated())
                    m_colorEditRegionId = -1; // gesture ended — next change starts a fresh undo step
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Str::TlDelete.iconId(ICON_TRASH, "tl_region_delete"))) {
                eq.push(DeleteRegionEvent{.regionId = ctxRegionId});
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem(Str::TlBake.iconId(ICON_BAKE, "tl_region_bake"))) {
                eq.push(BakeRegionEvent{.regionId = ctxRegionId});
                ImGui::CloseCurrentPopup();
            }

            // Split at the playhead into two regions — only when the playhead sits inside the region far
            // enough from each edge that both halves clear the 0.5 s minimum.
            constexpr double kMinRegionDur = 0.5;
            const bool canSplit =
                playheadTime > ctxReg->startTime + kMinRegionDur && playheadTime < ctxReg->endTime - kMinRegionDur;
            ImGui::BeginDisabled(!canSplit);
            if (ImGui::MenuItem(Str::TlSplitAtPlayhead.iconId(ICON_SPLIT, "tl_region_split")) && canSplit) {
                eq.push(SplitRegionEvent{.regionId = ctxRegionId, .splitTime = playheadTime});
                ImGui::CloseCurrentPopup();
            }
            if (!canSplit)
                disabledReq(Str::TlSplitReq.c_str());
            ImGui::EndDisabled();

            // Grow the region into the empty space up to its neighbors. Regions never overlap and are
            // kept sorted by startTime, so the nearest occupied edge on each side bounds the growth;
            // the timeline ends bound it where there is no neighbor.
            double leftBound = 0.0;
            double rightBound = duration;
            for (const auto &r : project.regions) {
                if (r.id == ctxRegionId)
                    continue;
                if (r.endTime <= ctxReg->startTime)
                    leftBound = std::max(leftBound, r.endTime);
                if (r.startTime >= ctxReg->endTime)
                    rightBound = std::min(rightBound, r.startTime);
            }
            const bool canGrowLeft = leftBound < ctxReg->startTime;
            const bool canGrowRight = rightBound > ctxReg->endTime;

            ImGui::Separator();
            ImGui::BeginDisabled(!canGrowLeft);
            if (ImGui::MenuItem(Str::TlGrowLeft.iconId(ICON_ARROW_LEFT_TO_LINE, "tl_region_grow_left")) &&
                canGrowLeft) {
                ProcessingRegion updated = *ctxReg;
                updated.startTime = leftBound;
                eq.push(ModifyRegionEvent{.regionId = ctxRegionId, .updatedRegion = updated});
                ImGui::CloseCurrentPopup();
            }
            if (!canGrowLeft)
                disabledReq(Str::TlGrowNoRoom.c_str());
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canGrowRight);
            if (ImGui::MenuItem(Str::TlGrowRight.iconId(ICON_ARROW_RIGHT_TO_LINE, "tl_region_grow_right")) &&
                canGrowRight) {
                ProcessingRegion updated = *ctxReg;
                updated.endTime = rightBound;
                eq.push(ModifyRegionEvent{.regionId = ctxRegionId, .updatedRegion = updated});
                ImGui::CloseCurrentPopup();
            }
            if (!canGrowRight)
                disabledReq(Str::TlGrowNoRoom.c_str());
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canGrowLeft && !canGrowRight);
            if (ImGui::MenuItem(Str::TlGrowBoth.iconId(ICON_UNFOLD_HORIZONTAL, "tl_region_grow_both")) &&
                (canGrowLeft || canGrowRight)) {
                ProcessingRegion updated = *ctxReg;
                updated.startTime = leftBound;
                updated.endTime = rightBound;
                eq.push(ModifyRegionEvent{.regionId = ctxRegionId, .updatedRegion = updated});
                ImGui::CloseCurrentPopup();
            }
            if (!canGrowLeft && !canGrowRight)
                disabledReq(Str::TlGrowNoRoom.c_str());
            ImGui::EndDisabled();
        } else {
            constexpr double kDefaultDur = 5.0;
            // A region is seeded with the active axis (its default graph I/O), so creation
            // still needs a valid active axis even though the bar itself is axis-agnostic.
            const bool hasActiveAxis = project.state.activeAxis < StandardAxis::Count;
            const AxisState *activeAx =
                hasActiveAxis ? &project.axes[static_cast<size_t>(project.state.activeAxis)] : nullptr;

            ImGui::TextDisabled("%s", TimeUtil::formatTime(ctxRegionClickTime, true));
            ImGui::Separator();
            ImGui::BeginDisabled(!hasActiveAxis);
            if (ImGui::MenuItem(Str::TlAddRegionPlayhead.iconId(ICON_PLUS, "add_region_playhead")) && hasActiveAxis) {
                double s = playheadTime;
                double e = std::min(duration, playheadTime + kDefaultDur);
                eq.push(CreateRegionEvent{.axisRole = project.state.activeAxis,
                                          .axisRoles = project.effectiveEditSet(),
                                          .startTime = s,
                                          .endTime = e,
                                          .timelineDuration = duration});
                ImGui::CloseCurrentPopup();
            }
            if (!hasActiveAxis)
                disabledReq(Str::TlNeedActiveAxis.c_str());
            if (ImGui::MenuItem(Str::TlAddRegionHere.iconId(ICON_PLUS, "add_region_here")) && hasActiveAxis) {
                double s = std::max(0.0, ctxRegionClickTime - kDefaultDur * 0.5);
                double e = std::min(duration, ctxRegionClickTime + kDefaultDur * 0.5);
                eq.push(CreateRegionEvent{.axisRole = project.state.activeAxis,
                                          .axisRoles = project.effectiveEditSet(),
                                          .startTime = s,
                                          .endTime = e,
                                          .timelineDuration = duration});
                ImGui::CloseCurrentPopup();
            }
            if (!hasActiveAxis)
                disabledReq(Str::TlNeedActiveAxis.c_str());
            ImGui::EndDisabled();

            ImGui::Separator();

            double selStart = 0.0, selEnd = 0.0;
            bool canAddOverSel = false;
            if (hasActiveAxis && !activeAx->selection.empty()) {
                selStart = activeAx->selection.begin()->at;
                selEnd = std::prev(activeAx->selection.end())->at;
                if (selEnd - selStart >= 0.5) {
                    canAddOverSel = true;
                    for (int j = 0; j < bandCount; ++j) {
                        const auto *reg = project.findRegion(regionIds[j]);
                        if (reg && reg->startTime < selEnd && reg->endTime > selStart) {
                            canAddOverSel = false;
                            break;
                        }
                    }
                }
            }
            ImGui::BeginDisabled(!canAddOverSel);
            if (ImGui::MenuItem(Str::TlAddRegionSelection.iconId(ICON_PLUS, "add_region_selection")) && canAddOverSel) {
                eq.push(CreateRegionEvent{.axisRole = project.state.activeAxis,
                                          .axisRoles = project.effectiveEditSet(),
                                          .startTime = selStart,
                                          .endTime = selEnd,
                                          .timelineDuration = duration});
                ImGui::CloseCurrentPopup();
            }
            if (!canAddOverSel)
                disabledReq(hasActiveAxis ? Str::TlAddOverSelReq.c_str() : Str::TlNeedActiveAxis.c_str());
            ImGui::EndDisabled();

            const bool canAddEntireDur = hasActiveAxis && (bandCount == 0);
            ImGui::BeginDisabled(!canAddEntireDur);
            if (ImGui::MenuItem(Str::TlAddRegionDuration.iconId(ICON_PLUS, "add_region_duration")) && canAddEntireDur) {
                eq.push(CreateRegionEvent{.axisRole = project.state.activeAxis,
                                          .axisRoles = project.effectiveEditSet(),
                                          .startTime = 0.0,
                                          .endTime = duration,
                                          .timelineDuration = duration});
                ImGui::CloseCurrentPopup();
            }
            if (!canAddEntireDur)
                disabledReq(hasActiveAxis ? Str::TlAddDurationReq.c_str() : Str::TlNeedActiveAxis.c_str());
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

} // namespace ofs
