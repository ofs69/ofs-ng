#pragma once

#include "Core/ScriptProject.h"
#include "UI/TimelineLayout.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_internal.h> // ImRect
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Maps a (time, pos) coordinate to a screen pixel inside the timeline curve area.
//
// Mirrors ScriptTimeline.cpp's timeToScreenX / posToScreenY exactly:
//   x = curve.x + ((time - offsetTime) / visibleTime) * curve.width
//   y = curve.y + margin + (1 - pos/100) * (curve.height - 2*margin)   (margin = 8)
//
// The curve area is the "##timeline" InvisibleButton; its RectFull is exactly
// {curvePos, curvePos + curveSize}. The view (visibleTime / offsetTime) is read
// from project.timelineView, which ProjectManager updates every frame from the
// timeline's UpdateTimelineViewEvent.
inline ImVec2 timelinePixel(ImGuiTestContext *ctx, double time, int pos) {
    const ImGuiTestItemInfo info = ctx->ItemInfo("Timeline###timeline/##timeline");
    const ImRect r = info.RectFull;

    const auto &tv = getTestState().project->timelineView;
    const double visibleTime = tv.visibleTime > 0.0 ? tv.visibleTime : 10.0;
    const double offsetTime = tv.offsetTime;

    constexpr float margin = ofs::ui::kCurveVMargin;
    const float x = r.Min.x + static_cast<float>((time - offsetTime) / visibleTime) * r.GetWidth();
    const float y = r.Min.y + margin + (1.0f - static_cast<float>(pos) / 100.0f) * (r.GetHeight() - 2.0f * margin);
    return ImVec2(x, y);
}

// Maps a time to a screen pixel at the vertical center of the region band bar.
//
// X is taken from the CURVE geometry, not the bar's own rect: renderRegionBar maps bands through
// curvePos/curveSize (its toX), while the "##regionbar" item is inset from the curve by one item-spacing
// on the left (ScriptTimeline.cpp), so its origin/width differ from where bands actually land. Using the
// bar rect would offset the computed X by stripMargin*(1-frac) — enough to push an edge grab into the
// band body. Read X from the same curve item timelinePixel uses, and only the Y center from the bar.
inline ImVec2 regionBarPixel(ImGuiTestContext *ctx, double time) {
    const ImRect curve = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
    const ImRect bar = ctx->ItemInfo("Timeline###timeline/##regionbar").RectFull;

    const auto &tv = getTestState().project->timelineView;
    const double visibleTime = tv.visibleTime > 0.0 ? tv.visibleTime : 10.0;
    const double offsetTime = tv.offsetTime;

    const float x = curve.Min.x + static_cast<float>((time - offsetTime) / visibleTime) * curve.GetWidth();
    const float y = bar.GetCenter().y;
    return ImVec2(x, y);
}
