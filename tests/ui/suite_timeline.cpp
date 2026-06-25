#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "helpers/TestState.h"
#include "helpers/TimelineCoords.h"
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h> // ImRect
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <initializer_list>

// The live UI EventQueue is frozen after init, so these tests assert on
// ScriptProject state (the same pattern as suite_undo / suite_playback) rather
// than attaching new event handlers via EventCapture.

static void selectL0(ImGuiTestContext *ctx) {
    getTestState().eventQueue->push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
    ctx->Yield();
}

// Replace L0's actions with a known set, then yield so the timeline renders them
// (and the one-frame-latency UpdateTimelineViewEvent has settled).
static void seedL0(ImGuiTestContext *ctx, std::initializer_list<ofs::ScriptAxisAction> pts) {
    ofs::VectorSet<ofs::ScriptAxisAction> actions;
    for (const auto &p : pts)
        actions.insert(p);
    getTestState().eventQueue->push(ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::L0, .actions = actions});
    ctx->Yield(2);
}

void RegisterTimelineTests(ImGuiTestEngine *e) {
    // Smoke test — window renders.
    IM_REGISTER_TEST(e, "timeline", "window_renders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield();
        IM_CHECK(ctx->WindowInfo("Timeline###timeline").Window != nullptr);
    };

    // Click on empty curve area seeks the playhead to that time.
    IM_REGISTER_TEST(e, "timeline", "click_empty_seeks")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        ctx->Yield(); // ensure timelineView (visibleTime/offsetTime) is populated

        // L0 has no actions after loadFixture, so this lands on empty area -> seek.
        ctx->MouseMoveToPos(timelinePixel(ctx, /*time=*/2.0, /*pos=*/50));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 2.0), 0.25); // within a quarter second
    };

    // Shift+click on empty curve area adds an action at the cursor.
    IM_REGISTER_TEST(e, "timeline", "shift_click_adds_action")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        ctx->Yield();
        const size_t before = proj.axes[0].actions.size();

        ctx->KeyDown(ImGuiMod_Shift);
        ctx->MouseMoveToPos(timelinePixel(ctx, 3.0, 60));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Shift);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.axes[0].actions.size(), before + 1);
    };

    // Left-drag across empty area box-selects the enclosed points.
    IM_REGISTER_TEST(e, "timeline", "box_select_selects_range")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}, {3.0, 50}});

        // Drag a box that encloses both points (kept clear of the edge-scroll zones).
        ctx->MouseMoveToPos(timelinePixel(ctx, 1.5, 90));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 3.5, 10));
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_GT(proj.axes[0].selection.size(), static_cast<size_t>(0));
    };

    // Left-drag a point straight up changes its position.
    IM_REGISTER_TEST(e, "timeline", "drag_point_moves_it")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}});
        const int posBefore = proj.axes[0].actions[0].pos;

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 90)); // drag straight up
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_NE(proj.axes[0].actions[0].pos, posBefore);
    };

    // Holding Alt during a drag locks the X axis (time stays put, only Y moves).
    IM_REGISTER_TEST(e, "timeline", "alt_drag_locks_x")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}});
        const double atBefore = proj.axes[0].actions[0].at;

        ctx->KeyDown(ImGuiMod_Alt);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 3.5, 90)); // move in X and Y
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Alt);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(proj.axes[0].actions[0].at - atBefore), 0.05); // X snapped back
    };

    // Left-click directly on a point seeks the playhead to it and selects only that point.
    IM_REGISTER_TEST(e, "timeline", "click_point_seeks_and_selects")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}, {6.0, 50}});

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.axes[0].selection.size(), static_cast<size_t>(1));
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 2.0), 0.1); // seeked to the point
    };

    // Ctrl+click a point toggles its selection without seeking (no playhead move).
    IM_REGISTER_TEST(e, "timeline", "ctrl_click_point_toggles_no_seek")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}});
        const double cursorBefore = proj.playback.cursorPos;

        ctx->KeyDown(ImGuiMod_Ctrl);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Ctrl);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.axes[0].selection.size(), static_cast<size_t>(1));  // point selected
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - cursorBefore), 0.05); // no seek
    };

    // Scrolling the wheel over the timeline zooms in (shrinks the visible time window).
    IM_REGISTER_TEST(e, "timeline", "scroll_zooms_in")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        ctx->Yield();
        const double visibleBefore = proj.timelineView.visibleTime;
        IM_CHECK_GT(visibleBefore, 0.0);

        const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        ctx->MouseMoveToPos(r.GetCenter());
        ctx->MouseWheelY(1.0f); // +1 = zoom in
        ctx->Yield(20);         // let the 150 ms smooth-zoom settle

        IM_CHECK_LT(proj.timelineView.visibleTime, visibleBefore - 0.01);
    };

    // Middle-drag pans the view, which seeks the playhead.
    IM_REGISTER_TEST(e, "timeline", "middle_drag_pans")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        selectL0(ctx);
        // Seek into the middle so a pan in either direction is unclamped.
        eq.push(ofs::SeekEvent{5.0});
        ctx->Yield(2);
        const double cursorBefore = proj.playback.cursorPos;

        const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        const ImVec2 c = r.GetCenter();
        ctx->MouseMoveToPos(c);
        ctx->MouseDown(ImGuiMouseButton_Middle);
        ctx->MouseMoveToPos(ImVec2(c.x - 120.0f, c.y)); // drag left → cursor advances
        ctx->MouseUp(ImGuiMouseButton_Middle);
        ctx->Yield(2);

        IM_CHECK_GT(std::abs(proj.playback.cursorPos - cursorBefore), 0.05);
    };

    // Middle double-click clears the current selection.
    IM_REGISTER_TEST(e, "timeline", "middle_double_click_clears_selection")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}});

        // Select the point first (ctrl+click), then confirm it's selected.
        ctx->KeyDown(ImGuiMod_Ctrl);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Ctrl);
        ctx->Yield(2);
        IM_CHECK_GT(proj.axes[0].selection.size(), static_cast<size_t>(0));

        const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        ctx->MouseMoveToPos(r.GetCenter());
        ctx->MouseDoubleClick(ImGuiMouseButton_Middle);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.axes[0].selection.size(), static_cast<size_t>(0));
    };

    // Right-click opens the timeline context menu (which carries the "Show points" toggle).
    IM_REGISTER_TEST(e, "timeline", "right_click_opens_context_menu")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        selectL0(ctx);
        ctx->Yield();

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);

        IM_CHECK(ctx->ItemExists("**/###tl_show_points"));
        ctx->PopupCloseAll(); // dismiss the context menu so it doesn't intercept later tests' clicks
        ctx->Yield();
    };
}
