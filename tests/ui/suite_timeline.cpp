#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "UI/DockLayout.h" // ofs::ui::applyDefaultLayout
#include "helpers/TestState.h"
#include "helpers/TimelineCoords.h"
#include <cmath>
#include <cstdio>
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

    // Click on empty script-line area seeks the playhead to that time.
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

    // Shift+click on empty script-line area adds an action at the cursor.
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

    // The context menu carries the layout toggle, and choosing "Separate lanes" switches the project's
    // script-line layout to Lanes.
    IM_REGISTER_TEST(e, "timeline", "context_menu_switches_layout")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        ctx->Yield();
        IM_CHECK(proj.timelineView.layout == ofs::TimelineLayout::Overlay); // default

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(ctx->ItemExists("**/###tl_layout_overlay"));
        IM_CHECK(ctx->ItemExists("**/###tl_layout_lanes"));
        ctx->ItemClick("**/###tl_layout_lanes");
        ctx->Yield(2);

        IM_CHECK(proj.timelineView.layout == ofs::TimelineLayout::Lanes);
        ctx->PopupCloseAll();
        ctx->Yield();
    };

    // The corner gear opens the timeline settings modal (raised through the shared ModalManager, so it
    // carries the legacy ###ofsmodal id). Its tabular form drives the same view settings as the
    // right-click menu — here the audio waveform toggle and the stacked/separated layout picker.
    IM_REGISTER_TEST(e, "timeline", "gear_opens_settings_modal")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        ctx->Yield();
        IM_CHECK(proj.timelineView.layout == ofs::TimelineLayout::Overlay); // default

        ctx->ItemClick("**/###tl_settings_gear");
        ctx->Yield(3); // event drain + ModalManager open latch
        IM_CHECK(ctx->ItemExists("**/###tlset_show_waveform"));

        // Toggle the audio waveform from the modal. (Checkboxes are wildcard-addressable by their
        // ###id; the layout/overlay Combos are exercised elsewhere — context_menu_switches_layout —
        // since a Combo isn't **/-resolvable and opening one fights the click-away flyout in a test.)
        const bool wfBefore = proj.timelineView.showAudioWaveform;
        ctx->ItemClick("**/###tlset_show_waveform");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.timelineView.showAudioWaveform, !wfBefore);

        // Per-axis toggles: flip R0's strip presence and script-line visibility from the Axes table. (R0 is
        // index 3.)
        constexpr int r0 = static_cast<int>(ofs::StandardAxis::R0);
        char ref[48];
        const bool panelBefore = proj.axes[r0].showInStrip;
        std::snprintf(ref, sizeof(ref), "**/###tlset_panel_%d", r0);
        ctx->ItemClick(ref);
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[r0].showInStrip, !panelBefore);

        const bool visBefore = proj.axes[r0].isVisible;
        std::snprintf(ref, sizeof(ref), "**/###tlset_vis_%d", r0);
        ctx->ItemClick(ref);
        ctx->Yield(2);
        IM_CHECK_EQ(proj.axes[r0].isVisible, !visBefore);

        // Escape (or a click outside) dismisses the click-away flyout — there is no Close button.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists("**/###tlset_show_waveform"));

        // Restore defaults so later tests start clean.
        getTestState().eventQueue->push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
        if (proj.timelineView.showAudioWaveform != wfBefore)
            getTestState().eventQueue->push(ofs::SetTimelineShowWaveformEvent{.show = wfBefore});
        getTestState().eventQueue->push(
            ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ofs::StandardAxis::R0, .inPanel = panelBefore});
        getTestState().eventQueue->push(
            ofs::ToggleAxisVisibilityEvent{.axisRole = ofs::StandardAxis::R0, .visible = visBefore});
        ctx->Yield();
    };

    // In Lanes layout a drag in an axis's lane still moves the point (the single visible lane spans the
    // full height, so the lane-aware pos mapping reduces to the overlay mapping) and the lane becomes
    // the active axis. Exercises the lane render + interaction path end-to-end.
    IM_REGISTER_TEST(e, "timeline", "lanes_drag_moves_point")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        selectL0(ctx);
        seedL0(ctx, {{2.0, 50}});
        getTestState().eventQueue->push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Lanes});
        ctx->Yield(2);
        const int posBefore = proj.axes[0].actions[0].pos;

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 90)); // drag straight up within the lane
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_NE(proj.axes[0].actions[0].pos, posBefore);
        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L0);

        getTestState().eventQueue->push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
        ctx->Yield();
    };

    // Clicking an inactive lane focuses that axis without seeking; only a click in the already-active lane
    // scrubs the playhead. Guards the "seek only on the active lane" rule.
    IM_REGISTER_TEST(e, "timeline", "lanes_inactive_lane_click_focuses_not_seek")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &proj = *getTestState().project;
            // Two lanes: L0 (active) plus R0, so there is an inactive lane to click into.
            getTestState().eventQueue->push(
                ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ofs::StandardAxis::R0, .inPanel = true});
            getTestState().eventQueue->push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Lanes});
            selectL0(ctx);
            ctx->Yield(2);
            IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::R0)].showInStrip);
            IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L0);

            // Resolve R0's lane center from the showInStrip ordering (robust to how many axes the strip shows).
            int total = 0, r0Lane = -1;
            for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
                if (proj.axes[i].showInStrip) {
                    if (static_cast<ofs::StandardAxis>(i) == ofs::StandardAxis::R0)
                        r0Lane = total;
                    ++total;
                }
            IM_CHECK_GT(total, 1);
            IM_CHECK_GE(r0Lane, 0);
            const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
            const float laneH = r.GetHeight() / static_cast<float>(total);
            // Kept clear of the script line's left/right edge-scroll zones.
            const ImVec2 r0Empty = {r.Min.x + r.GetWidth() * 0.6f,
                                    r.Min.y + laneH * (static_cast<float>(r0Lane) + 0.5f)};

            const double cursorBefore = proj.playback.cursorPos;
            ctx->MouseMoveToPos(r0Empty);
            ctx->MouseClick(ImGuiMouseButton_Left);
            ctx->Yield(2);
            IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::R0);            // focused
            IM_CHECK_LT(std::abs(proj.playback.cursorPos - cursorBefore), 0.01); // did NOT seek

            // A second click — R0 is now the active lane — does seek.
            ctx->MouseClick(ImGuiMouseButton_Left);
            ctx->Yield(2);
            IM_CHECK_GT(std::abs(proj.playback.cursorPos - cursorBefore), 0.1); // seeked this time

            getTestState().eventQueue->push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
            ctx->Yield();
        };

    // Axis grouping functions in the separated view: a box-select on the active lane fans across the edit
    // group exactly as in the stacked view, selecting in every grouped axis (not just one lane).
    IM_REGISTER_TEST(e, "timeline", "lanes_box_select_fans_across_group")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ofs::StandardAxis::R0, .inPanel = true});
        auto seed = [&](ofs::StandardAxis ax, std::initializer_list<ofs::ScriptAxisAction> pts) {
            ofs::VectorSet<ofs::ScriptAxisAction> a;
            for (const auto &p : pts)
                a.insert(p);
            eq.push(ofs::CommitAxisActionsEvent{.axis = ax, .actions = a});
        };
        seed(ofs::StandardAxis::L0, {{2.0, 50}, {3.0, 50}});
        seed(ofs::StandardAxis::R0, {{2.0, 50}, {3.0, 50}});
        ofs::AxisRoles roles;
        roles.set(static_cast<size_t>(ofs::StandardAxis::L0));
        roles.set(static_cast<size_t>(ofs::StandardAxis::R0));
        eq.push(ofs::SetAxisGroupingEvent{.roles = roles, .lead = ofs::StandardAxis::L0});
        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Lanes});
        ctx->Yield(3);
        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L0);

        // Box-select across [1.5, 3.5] on L0's lane (lane 0). The vertical position is irrelevant — a box
        // selects a time range — so any Y inside the active lane works.
        int total = 0;
        for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
            if (proj.axes[i].showInStrip)
                ++total;
        IM_CHECK_GT(total, 1);
        const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        const float y = r.Min.y + (r.GetHeight() / static_cast<float>(total)) * 0.5f; // lane 0 (L0) center
        ctx->MouseMoveToPos({timelinePixel(ctx, 1.5, 50).x, y});
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos({timelinePixel(ctx, 3.5, 50).x, y});
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_GT(proj.axes[static_cast<size_t>(ofs::StandardAxis::L0)].selection.size(), static_cast<size_t>(0));
        IM_CHECK_GT(proj.axes[static_cast<size_t>(ofs::StandardAxis::R0)].selection.size(),
                    static_cast<size_t>(0)); // grouping fanned the selection to R0

        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
        ctx->Yield();
    };

    // Acceptance: the default six-axis strip in Lanes layout fits the timeline's default docked height
    // without a scrollbar — every lane stays >= minLaneHeight (font * 1.8), so the band shares evenly and
    // makeLaneLayout reports no overflow. Guards the "6 axes, no scrollbar" requirement against future
    // dock-ratio or min-lane-height drift.
    IM_REGISTER_TEST(e, "timeline", "lanes_six_axes_fit_without_scrollbar")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ofs::ui::applyDefaultLayout(); // the real default dock; the harness window is the authored 1920x1080
        ctx->Yield(3);
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;

        // Six strip axes — L0 is always shown; add five more for the canonical multi-axis set.
        for (ofs::StandardAxis ax : {ofs::StandardAxis::L1, ofs::StandardAxis::L2, ofs::StandardAxis::R0,
                                     ofs::StandardAxis::R1, ofs::StandardAxis::R2})
            eq.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ax, .inPanel = true});
        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Lanes});
        ctx->Yield(3);

        int shown = 0;
        for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
            if (proj.axes[i].showInStrip)
                ++shown;
        IM_CHECK_EQ(shown, 6);

        // No scrollbar ⇔ the script-line band is tall enough to give all six lanes the minimum height.
        const float bandH = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull.GetHeight();
        const float minLaneH = ImGui::GetFontSize() * 1.8f; // mirror minLaneHeight() in ScriptTimeline.cpp
        ctx->LogInfo("bandH=%.1f  6*minLaneH=%.1f", bandH, 6.0f * minLaneH);
        IM_CHECK_GE(bandH, 6.0f * minLaneH);

        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
        ctx->Yield();
    };

    // Past the fitting count the lanes overflow and the band scrolls: Shift+wheel moves the lanes, and the
    // focus-click hit-test honors the scroll offset, so the lane at the top of the band after scrolling is
    // a later axis than before. Exercises makeLaneLayout overflow + scrolled laneAxisAt end-to-end.
    IM_REGISTER_TEST(e, "timeline", "lanes_overflow_scrolls")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ofs::ui::applyDefaultLayout();
        ctx->Yield(3);
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;

        // Ten strip axes — comfortably past the ~6 that fit, so the band must scroll.
        for (ofs::StandardAxis ax : {ofs::StandardAxis::L1, ofs::StandardAxis::L2, ofs::StandardAxis::R0,
                                     ofs::StandardAxis::R1, ofs::StandardAxis::R2, ofs::StandardAxis::V0,
                                     ofs::StandardAxis::V1, ofs::StandardAxis::A0, ofs::StandardAxis::A1})
            eq.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ax, .inPanel = true});
        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Lanes});
        selectL0(ctx);
        ctx->Yield(3);
        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L0);

        const ImRect r = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        const float laneH = ImGui::GetFontSize() * 1.8f;       // == minLaneHeight() once scrolling
        const ImVec2 topLane = {r.Min.x + r.GetWidth() * 0.5f, // mid-X: clear of edge-scroll + scrollbar
                                r.Min.y + laneH * 0.5f};       // center of the top visible lane

        // Before scrolling the top lane is L0 (lane 0). Clicking it would only seek (already active), so to
        // prove scrolling we scroll first, then click. The wheel step is exactly minLaneHeight, so 2 notches
        // == 2 lanes, bringing L2 (the 3rd axis) to the top. Shift+wheel only scrolls over the timeline.
        ctx->MouseMoveToPos(topLane);
        ctx->KeyDown(ImGuiMod_Shift);
        ctx->MouseWheelY(-2.0f); // scroll down 2 lanes
        ctx->KeyUp(ImGuiMod_Shift);
        ctx->Yield(3);

        ctx->MouseMoveToPos(topLane);
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        // The top lane is no longer L0 — scrolling moved a later axis under the cursor and the focus-click
        // selected it. (L2 is the 3rd showInStrip axis: L0, L1, L2, …)
        IM_CHECK(proj.state.activeAxis != ofs::StandardAxis::L0);
        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L2);

        eq.push(ofs::SetTimelineLayoutEvent{.layout = ofs::TimelineLayout::Overlay});
        ctx->Yield();
    };
}
