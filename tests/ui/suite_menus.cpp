#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Format/AppSettings.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Drives OfsApp's main-menu-bar action wiring (src/App/OfsApp.cpp). The Layout menu is covered by
// suite_layouts; Edit ▸ Preferences / Project by suite_config / suite_project_settings; View ▸ Keyboard
// Shortcuts and File ▸ Export by suite_windows / suite_modals. This suite picks up the remaining
// observable actions: the View visibility toggles (AppSettings) and the Axes menu (ScriptProject).

void RegisterMenusTests(ImGuiTestEngine *e) {
    // View ▸ Simulator toggles AppSettings::showSimulator. AppSettings is app-level (persists across
    // tests), so restore it.
    IM_REGISTER_TEST(e, "menus", "view_toggle_simulator")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const bool before = getTestState().appSettings->showSimulator;
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_view_simulator/###menu_view_simulator_show");
        ctx->Yield();
        IM_CHECK_EQ(getTestState().appSettings->showSimulator, !before);
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_view_simulator/###menu_view_simulator_show"); // restore
        ctx->Yield();
        IM_CHECK_EQ(getTestState().appSettings->showSimulator, before);
    };

    IM_REGISTER_TEST(e, "menus", "view_toggle_statistics")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const bool before = getTestState().appSettings->showStatistics;
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_view_statistics");
        ctx->Yield();
        IM_CHECK_EQ(getTestState().appSettings->showStatistics, !before);
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_view_statistics"); // restore
        ctx->Yield();
        IM_CHECK_EQ(getTestState().appSettings->showStatistics, before);
    };

    // Axes ▸ Add Scratch Axis pushes AddScratchAxisEvent → the first scratch slot (S0) becomes present.
    // Project state resets on the next loadFixture, so no cleanup is needed. (The command-palette route
    // to the same action is covered by suite_command_palette; this is the menu route.)
    IM_REGISTER_TEST(e, "menus", "axes_add_scratch_axis")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip);

        ctx->MenuClick("//##MainMenuBar/###menu_axes/###menu_add_scratch_axis");
        ctx->Yield(2);

        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip);
    };

    // Axes ▸ L0 (Stroke) ▸ Show in Timeline pushes ToggleAxisVisibilityEvent → axis.isVisible flips.
    IM_REGISTER_TEST(e, "menus", "axes_toggle_show_in_timeline")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &l0 = proj.axes[static_cast<size_t>(ofs::StandardAxis::L0)];
        IM_CHECK(l0.showInStrip); // the fixture's active axis
        const bool before = l0.isVisible;

        ctx->MenuClick("//##MainMenuBar/###menu_axes/###menu_axis_0/###menu_axis_show_timeline");
        ctx->Yield(2);

        IM_CHECK_EQ(l0.isVisible, !before);
    };

    // Axes ▸ Show L0 Only pushes ShowL0OnlyEvent → every other axis leaves the strip and L0 stays active.
    // A scratch axis that holds actions is hidden from the strip too, but its data isn't cleared, so it
    // still exists() and remains listed in the Axes menu (re-showable via "Show in Panel").
    IM_REGISTER_TEST(e, "menus", "axes_show_l0_only")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        getTestState().eventQueue->push(
            ofs::ToggleAxisPanelVisibilityEvent{.axisRole = ofs::StandardAxis::R0, .inPanel = true});
        getTestState().eventQueue->push(ofs::AddScratchAxisEvent{}); // creates S0 in the strip
        ofs::VectorSet<ofs::ScriptAxisAction> s0Actions;
        s0Actions.insert({1.0, 42});
        getTestState().eventQueue->push(
            ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::S0, .actions = s0Actions}); // give S0 data
        getTestState().eventQueue->push(ofs::AxisSelectedEvent{ofs::StandardAxis::R0});
        ctx->Yield(2);
        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::R0)].showInStrip);
        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip);

        ctx->MenuClick("//##MainMenuBar/###menu_axes/###menu_l0_only");
        ctx->Yield(2);

        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::L0)].showInStrip);
        IM_CHECK(!proj.axes[static_cast<size_t>(ofs::StandardAxis::R0)].showInStrip);
        IM_CHECK(!proj.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip); // hidden from the strip
        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::S0)].exists());     // but data kept → still in menu
        IM_CHECK_EQ(proj.state.activeAxis, ofs::StandardAxis::L0);                    // active fell back to L0
    };
}
