#include "ScriptTimeline.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/OverlaySettings.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Localization/Translator.h"
#include "UI/AxisColors.h"
#include "UI/BandBar.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
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
static constexpr float kLodThreshSq = 32.0f;
static constexpr float kDotR = 8.0f;
static constexpr double kDotFadeStart = 10.0;
static constexpr double kDotFadeEnd = 40.0;

static float easeOutExpo(float x) noexcept {
    return x >= 1.f ? 1.f : 1.f - powf(2, -10 * x);
}

static float posToScreenY(int posVal, const ImVec2 &pos, const ImVec2 &size) {
    constexpr float margin = ofs::ui::kCurveVMargin;
    return pos.y + margin + (1.0f - (float)posVal / 100.0f) * (size.y - 2.0f * margin);
}

static int screenYToPos(float y, const ImVec2 &pos, const ImVec2 &size) {
    constexpr float margin = ofs::ui::kCurveVMargin;
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

// Time width of one dot-decimation bucket: dots closer than ~2*kDotR px are collapsed to one.
// The bucket grid is anchored to absolute time 0 (`it->at / bucket`), so panning never re-shuffles
// which dot in a cluster wins — that is what keeps decimation steady during playback. To also stay
// steady across *zoom*, the bucket size is snapped to a fixed power-of-two ladder rather than varying
// continuously with visibleTime. Within a ladder step the drawn set is bit-for-bit identical; at a
// step boundary the buckets cleanly halve/double, so the visible set is a strict superset/subset of
// its neighbor — dots reveal or merge in place instead of a different cluster member popping in.
// Both the renderer and the hit-test call this so a clickable dot is exactly a drawn dot.
static double dotBucketDuration(double visibleTime, float width) {
    double minBucket = static_cast<double>(kDotR) * 2.0 * visibleTime / static_cast<double>(width);
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
                                                 const ImVec2 &size, float radius = 12.0f) {
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
                                       const ImVec2 &stripSize, const ImVec2 &curvePos, const AxisEntry *stripEntries,
                                       int numRows, bool hasGroup) {
    drawList->AddRectFilled(stripPos, stripPos + stripSize, ofs::theme::GetColorU32(AppCol_StripBg));

    float rowH = numRows > 0 ? size.y / static_cast<float>(numRows) : size.y;
    float fontH = ImGui::GetFontSize();

    // The row the mouse is over this frame, resolved through the per-row buttons below (-1 = none).
    int hoveredRow = -1;

    for (int i = 0; i < numRows; ++i) {
        const auto &entry = stripEntries[i];
        const auto &ax = project.axes[static_cast<size_t>(entry.role)];
        ImVec2 rowMin = {stripPos.x, stripPos.y + rowH * static_cast<float>(i)};
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

        // Resolve the eye-icon slot first so the label can be elided to stop short of it: in a narrow
        // strip (small window / large font) the name would otherwise run under the icon.
        const char *eyeIcon = ax.isVisible ? ICON_EYE : ICON_EYE_OFF;
        ImVec2 iconSz = ImGui::CalcTextSize(eyeIcon);
        float iconX = rowMax.x - 6.f - iconSz.x;
        float iconY = rowMin.y + (rowH - iconSz.y) * 0.5f;

        const float nameX = rowMin.x + kNameX0;
        ofs::ui::addTextShadow(drawList, {nameX, textY}, textColor, ofs::ui::elide(labelStr, iconX - nameX - 2.f));

        // Hidden axes keep their hue but read muted; visible ones use the row's full text color.
        ImU32 eyeCol = ax.isVisible ? textColor : ((textColor & 0x00FFFFFFu) | (0x66u << 24));
        drawList->AddText({iconX, iconY}, eyeCol, eyeIcon);
    }

    drawList->AddLine({curvePos.x, pos.y}, {curvePos.x, pos.y + size.y}, ofs::theme::GetColorU32(AppCol_StripDivider),
                      1.f);

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
        // icon at any font size, instead of a fixed pixel band that drifts off it.
        const char *eyeIcon = rowAx.isVisible ? ICON_EYE : ICON_EYE_OFF;
        const bool eyeArea = mX >= stripPos.x + stripSize.x - 6.f - ImGui::CalcTextSize(eyeIcon).x;
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
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ctxAxis = clickedRole;
            ctxFromGear = false;
            ImGui::OpenPopup("##timeline_ctx");
        }
    }

    // Drag across strip rows → group the spanned run (lead = the row the drag started on).
    if (stripDrag.anchorRow >= 0 && stripDrag.anchorRow < numRows) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float mY = ImGui::GetMousePos().y;
            int row = std::clamp(static_cast<int>((mY - stripPos.y) / rowH), 0, numRows - 1);
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

void ScriptTimelineWindow::renderCurves(const ScriptProject &project, ImDrawList *drawList, WaveformRenderer &waveform,
                                        const ImVec2 &pos, const ImVec2 &size, const ImVec2 &curvePos,
                                        const ImVec2 &curveSize, double offsetTime, const AxisEntry *curveEntries,
                                        int curveCount, bool windowHovered) {
    {
        ImU32 bgTop = ofs::theme::GetColorU32(AppCol_CurveBgTop);
        ImU32 bgBottom = ofs::theme::GetColorU32(AppCol_CurveBgBottom);
        drawList->AddRectFilledMultiColor(curvePos, curvePos + curveSize, bgTop, bgTop, bgBottom, bgBottom);
    }

    // Audio waveform: drawn on top of the background gradient but before the grid lines and curves, so it
    // sits behind everything the user edits while staying legible.
    if (project.timelineView.showAudioWaveform)
        waveform.drawBackground(drawList, curvePos, curveSize, offsetTime, viewState.visibleTime);

    if (windowHovered && ImGui::IsMouseHoveringRect(curvePos, curvePos + curveSize))
        drawList->AddRectFilled(curvePos, curvePos + curveSize, ofs::theme::GetColorU32(AppCol_CurveHoverBg));

    for (int i = 0; i < 9; i++) {
        float y = posToScreenY((i + 1) * 10, curvePos, curveSize);
        ImU32 gridCol =
            (i == 4) ? ofs::theme::GetColorU32(AppCol_GridLineMid) : ofs::theme::GetColorU32(AppCol_GridLine);
        float lineW = (i == 4) ? ofs::theme::GetStyleVar(AppVar_GridLineMidWidth) : 1.0f;
        drawList->AddLine({curvePos.x, y}, {curvePos.x + curveSize.x, y}, gridCol, lineW);
    }

    drawList->PushClipRect(curvePos, curvePos + curveSize, true);

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
    // join length blows up into a long spike. Zoomed far out the curve
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
            ImVec2 p(timeToScreenX(it->at, viewState.visibleTime, offsetTime, curvePos, curveSize),
                     posToScreenY(it->pos, curvePos, curveSize));
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

    for (int ci = 0; ci < curveCount; ++ci) {
        const auto &entry = curveEntries[ci];
        const auto &ax = project.axes[static_cast<size_t>(entry.role)];
        const auto &displayActions = ax.resolved ? ax.resolved->actions : ax.actions;
        bool isActive = entry.isActive;
        // Grouped (non-lead) members get full opacity and a slightly thicker line than a plain
        // visible reference axis. Dots render only on the active axis, so this line weight is the
        // sole cue that a background curve belongs to the edit group.
        bool isGroupedMember = entry.inEditSet && !isActive;
        bool emphasized = isActive || isGroupedMember;

        const float bgAxisOpacity = ofs::theme::getActive().backgroundAxisOpacity;
        float lineOpacity = emphasized ? 1.0f : bgAxisOpacity;
        // Emphasized axes (active or grouped) stroke at the themed base width; a plain background
        // reference reads 1px thinner. The contrast outline is a fixed 2px halo each side, so it
        // tracks the line width automatically.
        float baseLineW = ofs::theme::GetStyleVar(AppVar_TimelineLineWidth);
        float lineW = emphasized ? baseLineW : std::max(1.0f, baseLineW - 1.0f);
        float outlineW = lineW + 4.0f;
        auto outlineAlpha = static_cast<ImU32>(emphasized ? 255 : static_cast<int>(255.f * bgAxisOpacity));

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

            // Pass 2: colored lines. The active axis uses the per-segment heat
            // gradient (bucketed so same-speed runs still join flush); background
            // axes are a single color.
            if (isActive) {
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
            // emphasized line, not dots). Hidden when points are toggled off.
            if (project.timelineView.showPoints && isActive && !ax.actions.empty()) {
                auto fadeT = static_cast<float>(
                    std::clamp((viewState.visibleTime - kDotFadeStart) / (kDotFadeEnd - kDotFadeStart), 0.0, 1.0));
                float dotAlpha = 1.0f - fadeT * fadeT;
                if (dotAlpha > 0.1f) {
                    auto alpha = static_cast<ImU32>(255.0f * dotAlpha);
                    forEachVisibleDot(
                        project, ax.role, ax.actions, viewState.visibleTime, offsetTime, curveSize.x,
                        [&](const ScriptAxisAction &a) {
                            ImVec2 p(timeToScreenX(a.at, viewState.visibleTime, offsetTime, curvePos, curveSize),
                                     posToScreenY(a.pos, curvePos, curveSize));
                            bool selected = ax.selection.contains(a);
                            const ImU32 outlineCol =
                                (ofs::theme::GetColorU32(AppCol_TimelineOutline) & 0x00FFFFFFU) | (alpha << 24);
                            const ImU32 innerCol = (ofs::theme::GetColorU32(selected ? AppCol_TimelinePointSelected
                                                                                     : AppCol_TimelinePoint) &
                                                    0x00FFFFFFU) |
                                                   (alpha << 24);
                            drawList->AddCircleFilled(p, kDotR, outlineCol, 4);
                            drawList->AddCircleFilled(p, kDotR * 0.7f, innerCol, 4);
                        });
                }
            }
        }

        // Selection box (active only)
        if (isActive && selectionState.isSelecting) {
            auto relSel1 = (float)((selectionState.absSelStart - offsetTime) / viewState.visibleTime);
            auto rSel2 = (float)selectionState.relSelEnd;
            float minRel = std::min(relSel1, rSel2);
            float maxRel = std::max(relSel1, rSel2);
            const ImU32 selectColor = ofs::theme::GetColorU32(AppCol_SelectionBox);
            const ImU32 selectBg = ofs::theme::GetColorU32(AppCol_SelectionBoxFill);
            ImVec2 selMin(curvePos.x + curveSize.x * minRel, curvePos.y);
            ImVec2 selMax(curvePos.x + curveSize.x * maxRel, curvePos.y + curveSize.y);
            drawList->AddRectFilled(selMin, selMax, selectBg);
            drawList->AddLine(selMin, {selMin.x, selMax.y}, selectColor, 3.0f);
            drawList->AddLine({selMax.x, selMin.y}, selMax, selectColor, 3.0f);
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
    // Trim the strip's right edge by one item-spacing and let the curve fill the freed column, so the
    // strip and curve sit flush with no bare seam. The same trimmed edge is the left of the region bar
    // (render()), keeping the curve, overlay grid, and bands aligned. The gear keeps a margin of its own.
    const float stripMargin = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 stripSize = {stripW - stripMargin, size.y};
    const ImVec2 curvePos = {pos.x + stripW - stripMargin, pos.y};
    const ImVec2 curveSize = {size.x - stripW + stripMargin, size.y};

    // The multi-axis edit set (active group, or just {activeAxis}). Members beyond the lead render
    // their selection + dots and stay in the curve list even when hidden, so a fanned-out edit is
    // visible while it happens. hasGroup gates the strip's group bracket/fill.
    const AxisRoles editSet = project.effectiveEditSet();
    const bool hasGroup = project.state.axesGrouping.count() > 1;

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

    std::array<AxisEntry, kStandardAxisCount> curveEntries{};
    int curveCount = 0;
    for (int si = 0; si < stripCount; ++si) {
        const auto &e = stripEntries[si];
        const auto &ax = project.axes[static_cast<size_t>(e.role)];
        if (e.isActive || ax.isVisible || e.inEditSet)
            curveEntries[curveCount++] = e;
    }
    std::stable_sort(curveEntries.begin(), curveEntries.begin() + curveCount,
                     [](const AxisEntry &a, const AxisEntry &b) { return !a.isActive && b.isActive; });

    // ── Left strip ──────────────────────────────────────────────────────────
    renderStrip(project, eq, drawList, pos, size, stripPos, stripSize, curvePos, stripEntries.data(), stripCount,
                hasGroup);

    // ── Curve area ──────────────────────────────────────────────────────────
    renderCurves(project, drawList, waveform, pos, size, curvePos, curveSize, offsetTime, curveEntries.data(),
                 curveCount, windowHovered);

    // ── Curve area interaction ───────────────────────────────────────────────
    ImGui::SetCursorScreenPos(curvePos);
    ImGui::InvisibleButton("##timeline", curveSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    float mouseX = ImGui::GetMousePos().x;
    float mouseY = ImGui::GetMousePos().y;
    bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    bool ctrlHeld = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

    struct NearHit {
        StandardAxis axis = StandardAxis::Count;
        const ScriptAxisAction *action = nullptr;
    };
    auto findNearest = [&]() -> NearHit {
        NearHit best;
        if (!project.timelineView.showPoints) // points hidden: no hit-testing, clicks fall through to seek
            return best;
        if (project.state.activeAxis >= StandardAxis::Count)
            return best;
        const auto &activeAx = project.axes[static_cast<size_t>(project.state.activeAxis)];
        const auto *near = findNearestAction(project, project.state.activeAxis, activeAx.actions, mouseX, mouseY,
                                             viewState.visibleTime, offsetTime, curvePos, curveSize);
        if (near) {
            best.axis = project.state.activeAxis;
            best.action = near;
        }
        return best;
    };

    if (ImGui::IsItemHovered()) {
        NearHit hit = findNearest();
        bool activeLocked = project.state.activeAxis < StandardAxis::Count &&
                            project.axes[static_cast<size_t>(project.state.activeAxis)].isLocked;

        if (hit.action != nullptr) {
            if (!activeLocked)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::BeginTooltip()) {
                ImGui::Text(ICON_CLOCK_3 " %s", TimeUtil::formatTimeShort(hit.action->at));
                ImGui::Text(ICON_MOVE_VERTICAL " %d", hit.action->pos);
                ImGui::EndTooltip();
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hit.action != nullptr && !activeLocked) {
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
            } else if (shiftHeld && project.timelineView.showPoints && !activeLocked &&
                       project.state.activeAxis < StandardAxis::Count) {
                // Add point at cursor position
                double at = screenXToTime(mouseX, viewState.visibleTime, offsetTime, curvePos, curveSize);
                int pVal = screenYToPos(mouseY, curvePos, curveSize);
                eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::AddPoint,
                                                         .axis = project.state.activeAxis,
                                                         .time = at,
                                                         .pos = pVal}});
            } else {
                // Empty area: will seek on release or start box-select on drag
                editState.emptyClickPending = true;
                editState.emptyClickTime =
                    screenXToTime(mouseX, viewState.visibleTime, offsetTime, curvePos, curveSize);
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ctxAxis = (project.state.activeAxis < StandardAxis::Count) ? project.state.activeAxis : StandardAxis::Count;
            ctxFromGear = false;
            ImGui::OpenPopup("##timeline_ctx");
        }
    }

    renderContextMenu(project, eq, videoPlayer);
}

void ScriptTimelineWindow::renderContextMenu(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) {
    // The gear sits on the window's bottom edge: pin the popup's bottom-left to the gear's top-left
    // (pivot {0,1}) so it grows upward over the timeline instead of off the bottom. Right-click opens
    // are left to ImGui's default cursor placement.
    if (ctxFromGear)
        ImGui::SetNextWindowPos(gearMenuAnchor, ImGuiCond_Always, {0.0f, 1.0f});
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
        bool changed = false;

        // Font-relative field width (was a fixed 160 px) so a longer translated combo entry fits
        // and the column scales with font/DPI inside the auto-sizing popup.
        const float fieldW = ImGui::GetFontSize() * 10.f;

        const char *overlayNames[] = {Str::TlOverlayFrame, Str::TlOverlayTempo};
        int currentOverlay = static_cast<int>(ov.overlay);
        ImGui::TextUnformatted(Str::TlOverlay);
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::Combo("##overlay", &currentOverlay, overlayNames, IM_ARRAYSIZE(overlayNames))) {
            ov.overlay = static_cast<ScriptingOverlay>(currentOverlay);
            changed = true;
        }

        if (ov.overlay == ScriptingOverlay::Frame) {
            const char *fpsStr = fmtScratch("{:.3f}", videoPlayer.getFps());
            ImGui::TextUnformatted(Str::TlVideoFps.fmt(fpsStr));

            constexpr float kCommonFpss[] = {23.976f, 24.0f, 25.0f, 29.97f, 30.0f, 48.0f, 50.0f, 59.94f, 60.0f};
            const char *kCommonFpsNames[] = {"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"};
            int currentFpsIdx = -1;
            for (int i = 0; i < IM_ARRAYSIZE(kCommonFpss); ++i) {
                if (std::abs(ov.frameFps - kCommonFpss[i]) < 0.001f) {
                    currentFpsIdx = i;
                    break;
                }
            }

            ImGui::TextUnformatted(Str::TlScriptFps);
            ImGui::SetNextItemWidth(fieldW);
            if (ImGui::Combo("##fps_preset", &currentFpsIdx, kCommonFpsNames, IM_ARRAYSIZE(kCommonFpsNames))) {
                ov.frameFps = kCommonFpss[currentFpsIdx];
                changed = true;
            }

            ImGui::TextUnformatted(Str::TlCustomFps);
            ImGui::SetNextItemWidth(fieldW);
            changed |= ImGui::DragFloat("##custom_fps", &ov.frameFps, 0.1f, 1.0f, 240.0f);
        } else if (ov.overlay == ScriptingOverlay::Tempo) {
            ImGui::TextUnformatted(Str::TlBpm);
            ImGui::SetNextItemWidth(fieldW);
            changed |= ImGui::DragFloat("##bpm", &ov.tempoBpm, 0.1f, 10.0f, 500.0f);

            ImGui::TextUnformatted(Str::TlOffsetSeconds);
            ImGui::SetNextItemWidth(fieldW);
            changed |= ImGui::DragFloat("##tempo_offset", &ov.tempoOffsetSeconds, 0.001f, -10.0f, 10.0f, "%.3f",
                                        ImGuiSliderFlags_AlwaysClamp);

            // kTempoSubdivisionNames index 0 ("1/1 (measure)") carries the only translatable word;
            // the rest are numeric fractions. Localize just that entry into the frame arena.
            auto **snapNames = ofs::FrameAllocator::instance().allocArray<const char *>(kTempoSubdivisionCount);
            for (int i = 0; i < kTempoSubdivisionCount; ++i)
                snapNames[i] = kTempoSubdivisionNames[i];
            snapNames[0] = fmtScratch("1/1 ({})", Str::TlMeasure.c_str());

            ImGui::TextUnformatted(Str::TlSnap);
            ImGui::SetNextItemWidth(fieldW);
            changed |= ImGui::Combo("##tempo_snap", &ov.tempoMeasureIndex, snapNames, kTempoSubdivisionCount);
        }

        if (changed)
            eq.push(OverlaySettingsChangedEvent{ov});

        ImGui::EndMenu();
    }
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
    if (outerSize.y < kMinPanelH)
        outerSize.y = kMinPanelH;

    // Region bar height tracks the font so its band labels are never clipped.
    const float regionBarH = ofs::ui::bandBarHeight();
    // Breathing room between the curve area and the region bar (anchored at the bottom).
    const float regionBarGap = ImGui::GetStyle().ItemSpacing.y;

    const float stripW = stripWidth();
    // Mirror renderTimeline: the strip is trimmed by one item-spacing and the curve (with its overlay
    // grid and the region bar below) fills the freed column, flush with the strip.
    const float stripMargin = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 curvePos = {outerPos.x + stripW - stripMargin, outerPos.y};
    const ImVec2 curveSize = {outerSize.x - stripW + stripMargin, outerSize.y - regionBarH - regionBarGap};

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
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            viewState.previousVisibleTime = viewState.visibleTime;
            viewState.targetVisibleTime *= (wheel > 0) ? 0.8 : 1.25;
            viewState.targetVisibleTime = std::clamp(viewState.targetVisibleTime, 0.1, 300.0);
            viewState.zoomUpdateTime = ticks;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            double timeDelta = static_cast<double>(-ImGui::GetIO().MouseDelta.x / curveSize.x) * viewState.visibleTime;
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
        selectionState.relSelEnd = std::clamp((mouseX - curvePos.x) / curveSize.x, 0.0f, 1.0f);
    }

    renderTimeline(project, eq, videoPlayer, waveform, outerPos,
                   {outerSize.x, outerSize.y - regionBarH - regionBarGap});
    renderOverlay(project, curvePos, curveSize, offsetTime);
    renderPlayhead(videoPlayer, curvePos, curveSize, offsetTime);

    // The region bar's track keeps the full strip width on its left (edge at stripW), so it stays inset
    // by the strip margin from the curve above — that inset is the small gap the corner gear sits in.
    // Bands still map through the curve geometry, so they line up with the curve; bands are clamped to
    // the track's left edge (drawBandBar), so only the visible track rect is inset.
    const ImVec2 regionBarMin = {outerPos.x + stripW, outerPos.y + outerSize.y - regionBarH};
    const ImVec2 regionBarMax = {curvePos.x + curveSize.x, outerPos.y + outerSize.y};
    renderRegionBar(videoPlayer, project, eq, regionBarMin, regionBarMax, curvePos, curveSize, offsetTime);

    // Settings gear in the dead corner below the left strip, beside the region bar. It opens the same
    // context menu as a right-click on the timeline, giving the overlay/view settings a discoverable
    // entry point that doesn't depend on knowing the right-click gesture. Latch ctxAxis exactly as the
    // right-click path does so the menu's axis section targets the active axis. Width runs to one
    // item-spacing short of the region bar's left edge so the button keeps a margin from it.
    const ImVec2 gearMin = {outerPos.x, regionBarMin.y};
    const float gearMargin = ImGui::GetStyle().ItemSpacing.x;
    const float gearW = regionBarMin.x - gearMargin - gearMin.x;
    ImGui::SetCursorScreenPos(gearMin);
    if (ImGui::Button(ICON_SETTINGS "###tl_settings", {gearW, regionBarH})) {
        ctxAxis = (project.state.activeAxis < StandardAxis::Count) ? project.state.activeAxis : StandardAxis::Count;
        ctxFromGear = true;
        gearMenuAnchor = gearMin;
        ImGui::OpenPopup("##timeline_ctx");
    }

    // ── Edge scroll during box selection or region resize/move ──────────────
    {
        bool scrollActive = selectionState.isSelecting || regionDragState.mode != ofs::ui::BandBarDragState::Mode::None;
        if (scrollActive) {
            float mouseX = ImGui::GetMousePos().x;
            constexpr float kEdgeZone = 60.0f;
            double maxSpeed = viewState.visibleTime * 2.0;
            auto dt = static_cast<double>(ImGui::GetIO().DeltaTime);

            double seekDelta = 0.0;
            if (mouseX < curvePos.x + kEdgeZone) {
                float t = std::clamp(1.0f - (mouseX - curvePos.x) / kEdgeZone, 0.0f, 1.0f);
                seekDelta = -maxSpeed * static_cast<double>(t) * dt;
            } else if (mouseX > curvePos.x + curveSize.x - kEdgeZone) {
                float t = std::clamp(1.0f - (curvePos.x + curveSize.x - mouseX) / kEdgeZone, 0.0f, 1.0f);
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
            // selection, not per-point interaction.
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

            double newDragTime =
                altHeld ? editState.originalDragAt
                        : std::max(0.0, screenXToTime(mouseX, viewState.visibleTime, offsetTime, curvePos, curveSize));
            int newDragPos = screenYToPos(mouseY, curvePos, curveSize);

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
                                           const ImVec2 &barMin, const ImVec2 &barMax, const ImVec2 &curvePos,
                                           const ImVec2 &curveSize, double offsetTime) {
    const double duration = videoPlayer.getDuration();
    if (duration <= 0.0)
        return;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    // Recessed track fill + soft outline so the region bar is a visible band even with no
    // regions placed (ProcessingPanelBg alone blended into the surrounding panel).
    dl->AddRectFilled(barMin, barMax, ofs::theme::GetColorU32(AppCol_StripBg));
    dl->AddRect(barMin, barMax, ofs::theme::GetColorU32(ImGuiCol_Border));

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
        return timeToScreenX(t, viewState.visibleTime, offsetTime, curvePos, curveSize);
    };
    auto toTime = [&](float x) -> double {
        return screenXToTime(x, viewState.visibleTime, offsetTime, curvePos, curveSize);
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
        if (idx >= 0 && idx < bandCount) {
            eq.push(SelectRegionEvent{.regionId = regionIds[idx]});
            m_regionClickedThisFrame = true;
        }
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

    if (ImGui::BeginPopup("##region_ctx")) {
        const auto *ctxReg = project.findRegion(ctxRegionId);
        if (ctxReg != nullptr) {
            ImGui::TextDisabled("%s \xe2\x80\x93 %s", TimeUtil::formatTime(ctxReg->startTime, true),
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
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canGrowRight);
            if (ImGui::MenuItem(Str::TlGrowRight.iconId(ICON_ARROW_RIGHT_TO_LINE, "tl_region_grow_right")) &&
                canGrowRight) {
                ProcessingRegion updated = *ctxReg;
                updated.endTime = rightBound;
                eq.push(ModifyRegionEvent{.regionId = ctxRegionId, .updatedRegion = updated});
                ImGui::CloseCurrentPopup();
            }
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
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

} // namespace ofs
