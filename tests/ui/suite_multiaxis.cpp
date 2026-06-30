#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "helpers/TestState.h"
#include "helpers/TimelineCoords.h"
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h> // ImRect
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <initializer_list>

// UI tests for multi-axis editing (axis grouping). The strip mouse interaction state machine lives
// entirely in ScriptTimeline.cpp and has no service-level coverage, so it is driven here through real
// clicks/drags. The fan-out edits (move/add/box-select/region) are checked end-to-end through the
// script-line and region-bar UI to confirm the windows wire activeAxis-targeted events to the lead.
//
// Like suite_timeline, the live EventQueue is frozen after init, so tests assert on ScriptProject
// state rather than attaching new handlers. The fixture loads L0..A1 present and in the strip.
// Strip rows expose a stable per-row item id (##strip_row_<shortname>), so rows are referenced by
// id rather than by computed geometry.

namespace {

using ofs::StandardAxis;

size_t ix(StandardAxis a) {
    return static_cast<size_t>(a);
}

// Stable per-row item id (the InvisibleButton the strip hit-tests, registered in ScriptTimeline.cpp).
// Returns a static buffer — use one call per expression.
const char *rowRef(StandardAxis role) {
    static char buf[64];
    std::snprintf(buf, sizeof(buf), "Timeline###timeline/##strip_row_%s", ofs::standardAxisShortName(role).data());
    return buf;
}

ImRect stripRowRect(ImGuiTestContext *ctx, StandardAxis role) {
    return ctx->ItemInfo(rowRef(role)).RectFull;
}

// Point inside a strip row's right-edge eye/visibility zone (>= width-20px). Used for the eye toggle,
// which is a sub-region of the row item that a center ItemClick wouldn't hit.
ImVec2 stripRowEyePoint(ImGuiTestContext *ctx, StandardAxis role) {
    const ImRect r = stripRowRect(ctx, role);
    return ImVec2(r.Max.x - 10.0f, r.GetCenter().y);
}

// The fixture only puts L0 in the left strip; bring the rows a strip test needs into the panel so they
// render (and so their per-row item ids exist). present is already true for all device axes.
void showInStrip(ImGuiTestContext *ctx, std::initializer_list<StandardAxis> roles) {
    for (auto r : roles)
        getTestState().eventQueue->push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = r, .inPanel = true});
    ctx->Yield(2);
}

void seedAxis(ImGuiTestContext *ctx, StandardAxis role, std::initializer_list<ofs::ScriptAxisAction> pts) {
    ofs::VectorSet<ofs::ScriptAxisAction> actions;
    for (const auto &p : pts)
        actions.insert(p);
    getTestState().eventQueue->push(ofs::CommitAxisActionsEvent{.axis = role, .actions = actions});
    ctx->Yield(2);
}

// Form an explicit edit group via the grouping event (the reliable setup path); the UI interaction
// under test is then the single click/drag the test exercises. Grouping is restricted to panel-visible
// axes, so bring them into the strip first.
void groupAxes(ImGuiTestContext *ctx, std::initializer_list<StandardAxis> axes, StandardAxis lead) {
    showInStrip(ctx, axes);
    ofs::AxisRoles roles;
    for (auto a : axes)
        roles.set(ix(a));
    getTestState().eventQueue->push(ofs::SetAxisGroupingEvent{.roles = roles, .lead = lead});
    ctx->Yield(2);
}

} // namespace

void RegisterMultiAxisTests(ImGuiTestEngine *e) {
    // Ctrl-clicking a second strip row groups it with the active axis (forms the edit group via the UI).
    IM_REGISTER_TEST(e, "multiaxis", "strip_ctrl_click_groups")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        getTestState().eventQueue->push(ofs::AxisSelectedEvent{StandardAxis::L0});
        ctx->Yield(2);
        showInStrip(ctx, {StandardAxis::L1});
        IM_CHECK(proj.state.axesGrouping.none());

        ctx->KeyDown(ImGuiMod_Ctrl);
        ctx->ItemClick(rowRef(StandardAxis::L1));
        ctx->KeyUp(ImGuiMod_Ctrl);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(2));
        IM_CHECK(proj.state.axesGrouping.test(ix(StandardAxis::L0)));
        IM_CHECK(proj.state.axesGrouping.test(ix(StandardAxis::L1)));
        IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::L0); // lead unchanged
    };

    // Ctrl-clicking the lead out of a 3-axis group removes it and promotes a new lead.
    IM_REGISTER_TEST(e, "multiaxis", "strip_ctrl_click_removes_lead_and_promotes")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &proj = *getTestState().project;
            groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1, StandardAxis::L2}, StandardAxis::L0);
            IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(3));

            // Ctrl-click L0 (the lead) → it leaves the group; lead promotes to the first remaining member.
            ctx->KeyDown(ImGuiMod_Ctrl);
            ctx->ItemClick(rowRef(StandardAxis::L0));
            ctx->KeyUp(ImGuiMod_Ctrl);
            ctx->Yield(2);

            IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(2));
            IM_CHECK(!proj.state.axesGrouping.test(ix(StandardAxis::L0)));
            IM_CHECK(proj.state.axesGrouping.test(ix(StandardAxis::L1)));
            IM_CHECK(proj.state.axesGrouping.test(ix(StandardAxis::L2)));
            IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::L1); // promoted lead
        };

    // Plain-clicking a member of an existing group re-leads to it without dissolving the group.
    IM_REGISTER_TEST(e, "multiaxis", "strip_click_member_releads")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1, StandardAxis::L2}, StandardAxis::L0);

        ctx->ItemClick(rowRef(StandardAxis::L1));
        ctx->Yield(2);

        IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(3)); // group intact
        IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::L1);                 // new lead
    };

    // Plain-clicking an axis outside the group dissolves it and selects that axis.
    IM_REGISTER_TEST(e, "multiaxis", "strip_click_nonmember_dissolves")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);
        showInStrip(ctx, {StandardAxis::R0}); // a panel-visible axis outside the group

        ctx->ItemClick(rowRef(StandardAxis::R0)); // not in the group
        ctx->Yield(2);

        IM_CHECK(proj.state.axesGrouping.none());
        IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::R0);
    };

    // Double-clicking a row always dissolves the group back to single-axis editing.
    IM_REGISTER_TEST(e, "multiaxis", "strip_double_click_dissolves")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);
        IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(2));

        ctx->ItemDoubleClick(rowRef(StandardAxis::L1));
        ctx->Yield(2);

        IM_CHECK(proj.state.axesGrouping.none());
        IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::L1); // double-click also selects the row
    };

    // Dragging across strip rows groups the whole spanned run, with the drag-start row as the lead.
    IM_REGISTER_TEST(e, "multiaxis", "strip_drag_groups_span")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        getTestState().eventQueue->push(ofs::AxisSelectedEvent{StandardAxis::L0});
        ctx->Yield(2);
        showInStrip(ctx, {StandardAxis::L1, StandardAxis::L2, StandardAxis::R0});

        ctx->MouseMoveToPos(stripRowRect(ctx, StandardAxis::L0).GetCenter());
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(stripRowRect(ctx, StandardAxis::R0).GetCenter()); // span L0..R0 (L0,L1,L2,R0)
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.state.axesGrouping.count(), static_cast<size_t>(4));
        for (auto a : {StandardAxis::L0, StandardAxis::L1, StandardAxis::L2, StandardAxis::R0})
            IM_CHECK(proj.state.axesGrouping.test(ix(a)));
        IM_CHECK_EQ(proj.state.activeAxis, StandardAxis::L0); // lead = drag start
    };

    // Clicking the right-edge eye zone toggles visibility and must NOT form an edit group.
    IM_REGISTER_TEST(e, "multiaxis", "strip_eye_click_does_not_group")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        getTestState().eventQueue->push(ofs::AxisSelectedEvent{StandardAxis::L0});
        ctx->Yield(2);
        showInStrip(ctx, {StandardAxis::L1});
        const bool visBefore = proj.axes[ix(StandardAxis::L1)].isVisible;

        ctx->MouseMoveToPos(stripRowEyePoint(ctx, StandardAxis::L1));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK(proj.state.axesGrouping.none());                          // no grouping from the eye zone
        IM_CHECK_NE(proj.axes[ix(StandardAxis::L1)].isVisible, visBefore); // visibility toggled
    };

    // A script-line drag of the lead's point fans out to a grouped member: absolute on the lead,
    // delta-mirrored on the member.
    IM_REGISTER_TEST(e, "multiaxis", "group_line_drag_mirrors_to_member")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedAxis(ctx, StandardAxis::L0, {{2.0, 50}});
        seedAxis(ctx, StandardAxis::L1, {{2.0, 50}});
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);

        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 50));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 2.0, 90)); // straight up: Δt≈0, Δpos≈+40
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_GT(proj.axes[ix(StandardAxis::L0)].actions.size(), static_cast<size_t>(0));
        IM_CHECK_GT(proj.axes[ix(StandardAxis::L1)].actions.size(), static_cast<size_t>(0));
        const auto &lead = proj.axes[ix(StandardAxis::L0)].actions[0];
        const auto &member = proj.axes[ix(StandardAxis::L1)].actions[0];
        IM_CHECK_GT(lead.pos, 50);                        // lead moved up
        IM_CHECK_EQ(member.pos, lead.pos);                // member mirrored the same delta from an equal start
        IM_CHECK_LT(std::abs(member.at - lead.at), 0.05); // same Δt
    };

    // Box-select on the script line fans the selection out to every member of the group.
    IM_REGISTER_TEST(e, "multiaxis", "group_box_select_fans_out")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        seedAxis(ctx, StandardAxis::L0, {{2.0, 50}, {3.0, 50}});
        seedAxis(ctx, StandardAxis::L1, {{2.0, 50}, {3.0, 50}});
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);

        ctx->MouseMoveToPos(timelinePixel(ctx, 1.5, 90));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(timelinePixel(ctx, 3.5, 10));
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_GT(proj.axes[ix(StandardAxis::L0)].selection.size(), static_cast<size_t>(0));
        IM_CHECK_GT(proj.axes[ix(StandardAxis::L1)].selection.size(), static_cast<size_t>(0)); // fanned out
    };

    // Shift-click on the empty script line adds the same action on every member of the group.
    IM_REGISTER_TEST(e, "multiaxis", "group_shift_click_add_fans_out")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);
        const size_t l0Before = proj.axes[ix(StandardAxis::L0)].actions.size();
        const size_t l1Before = proj.axes[ix(StandardAxis::L1)].actions.size();

        ctx->KeyDown(ImGuiMod_Shift);
        ctx->MouseMoveToPos(timelinePixel(ctx, 3.0, 60));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->KeyUp(ImGuiMod_Shift);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.axes[ix(StandardAxis::L0)].actions.size(), l0Before + 1);
        IM_CHECK_EQ(proj.axes[ix(StandardAxis::L1)].actions.size(), l1Before + 1); // fanned out
    };

    // Showing/hiding an axis in the strip panel is undoable (showInStrip is captured in the snapshot).
    IM_REGISTER_TEST(e, "multiaxis", "panel_visibility_toggle_is_undoable")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        IM_CHECK(!proj.axes[ix(StandardAxis::L1)].showInStrip); // fixture: only L0 in the panel

        eq.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = StandardAxis::L1, .inPanel = true});
        ctx->Yield(2);
        IM_CHECK(proj.axes[ix(StandardAxis::L1)].showInStrip);

        eq.push(ofs::UndoEvent{});
        ctx->Yield(2);
        IM_CHECK(!proj.axes[ix(StandardAxis::L1)].showInStrip); // restored out of the panel
    };

    // Creating a region from the band-bar menu while a group is active spans every grouped axis.
    IM_REGISTER_TEST(e, "multiaxis", "group_create_region_spans_members")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        getTestState().eventQueue->push(ofs::SeekEvent{5.0});
        ctx->Yield(2);
        groupAxes(ctx, {StandardAxis::L0, StandardAxis::L1}, StandardAxis::L0);
        const size_t regionsBefore = proj.regions.size();

        // Right-click empty band-bar area → the create-region menu (ctxRegionId == -1 branch).
        ctx->MouseMoveToPos(regionBarPixel(ctx, 5.0));
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        ctx->ItemClick("**/###add_region_playhead");
        ctx->Yield(2);

        IM_CHECK_EQ(proj.regions.size(), regionsBefore + 1);
        const auto &region = proj.regions.back();
        IM_CHECK(region.axisRoles.test(ix(StandardAxis::L0)));
        IM_CHECK(region.axisRoles.test(ix(StandardAxis::L1))); // group spanned into the region
        ctx->PopupCloseAll();
        ctx->Yield();
    };
}
