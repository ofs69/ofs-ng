#include "Core/Events.h"
#include "Core/StandardAxis.h"
#include "helpers/TestState.h"
#include <imgui_te_engine.h>

void RegisterDialogsTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "dialogs", "open_project_by_path_loads_project")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(proj.metadata.title == "Fixture Project");                          // the fixture loaded
        IM_CHECK(proj.axes[static_cast<size_t>(ofs::StandardAxis::L0)].showInStrip); // L0 shown in the panel
    };

    IM_REGISTER_TEST(e, "dialogs", "fixture_activates_dummy_player")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK_GT(getTestState().project->state.dummyDuration, 0.0);
    };
}
