#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "UI/BandBar.h"
#include "helpers/TestState.h"
#include "helpers/TimelineCoords.h"
#include <cmath>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Drag interactions on the region BandBar (the strip below the timeline curve).
// Same approach as suite_timeline / suite_processing: seed state via events, drive
// the mouse over computed pixels, and assert on ScriptProject (the event queue is
// frozen after init, so we read project.regions rather than attaching handlers).
//
// The view is centered on the playhead. We seek to 8 s (view [3, 13] at the default
// 10 s zoom) and create a 7–9 s region, so both band edges land near the middle of
// the view — clear of the 60 px edge-scroll zones and >8 px from the playhead (which
// avoids the resize snap-to-playhead at the region edges we drag).

namespace {

constexpr double kRegStart = 7.0;
constexpr double kRegEnd = 9.0;
// Center of BandBar's (now font-relative) edge zone. A function, not a constant — must be read within
// a frame. Call sites already run inside the test context's frame loop.
inline float edgeGrab() {
    return ofs::ui::bandBarEdgeW() * 0.5f;
}

// Load fixture, park the playhead mid-timeline, create a 7–9 s region on L0.
void setupRegion(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    getTestState().eventQueue->push(ofs::SeekEvent{8.0});
    getTestState().eventQueue->push(ofs::CreateRegionEvent{
        .axisRole = ofs::StandardAxis::L0,
        .startTime = kRegStart,
        .endTime = kRegEnd,
    });
    ctx->Yield(2); // region created + timelineView (offset/visible) settled for pixel math
}

const ofs::ProcessingRegion &onlyRegion() {
    return getTestState().project->regions[0];
}

} // namespace

void RegisterBandBarTests(ImGuiTestEngine *e) {
    // Dragging the band body moves the whole region, preserving its duration.
    IM_REGISTER_TEST(e, "bandbar", "drag_body_moves_region")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        IM_CHECK_EQ(getTestState().project->regions.size(), static_cast<size_t>(1));

        const double mid = (kRegStart + kRegEnd) * 0.5;
        ctx->MouseMoveToPos(regionBarPixel(ctx, mid));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(regionBarPixel(ctx, mid + 1.0)); // drag right by ~1 s
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(onlyRegion().startTime - (kRegStart + 1.0)), 0.2);
        IM_CHECK_LT(std::abs(onlyRegion().endTime - (kRegEnd + 1.0)), 0.2);
    };

    // Dragging the left edge moves startTime only; endTime stays put.
    IM_REGISTER_TEST(e, "bandbar", "drag_left_edge_resizes_start")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);

        ImVec2 leftEdge = regionBarPixel(ctx, kRegStart);
        leftEdge.x += edgeGrab(); // land inside the left resize zone
        ctx->MouseMoveToPos(leftEdge);
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(regionBarPixel(ctx, kRegStart - 0.5)); // drag left
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(onlyRegion().startTime - (kRegStart - 0.5)), 0.2);
        IM_CHECK_LT(std::abs(onlyRegion().endTime - kRegEnd), 0.05); // right edge unchanged
    };

    // Dragging the right edge moves endTime only; startTime stays put.
    IM_REGISTER_TEST(e, "bandbar", "drag_right_edge_resizes_end")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);

        ImVec2 rightEdge = regionBarPixel(ctx, kRegEnd);
        rightEdge.x -= edgeGrab(); // land inside the right resize zone
        ctx->MouseMoveToPos(rightEdge);
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(regionBarPixel(ctx, kRegEnd + 0.5)); // drag right
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(onlyRegion().endTime - (kRegEnd + 0.5)), 0.2);
        IM_CHECK_LT(std::abs(onlyRegion().startTime - kRegStart), 0.05); // left edge unchanged
    };

    // Holding Alt during a body drag snaps the region back to its original position.
    IM_REGISTER_TEST(e, "bandbar", "alt_drag_snaps_back")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);

        const double mid = (kRegStart + kRegEnd) * 0.5;
        ctx->KeyDown(ImGuiMod_Alt);
        ctx->MouseMoveToPos(regionBarPixel(ctx, mid));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(regionBarPixel(ctx, mid + 1.0)); // move, but Alt locks it
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Alt);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(onlyRegion().startTime - kRegStart), 0.05);
        IM_CHECK_LT(std::abs(onlyRegion().endTime - kRegEnd), 0.05);
    };
}
