#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

using namespace ofs;

namespace {
bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Mirror of ProjectManager::hasActiveProject() — the test can't reach the private method, so it
// checks the same observable state to confirm doClose() really left us with no project.
bool noProjectOpen(const ScriptProject &p) {
    if (!p.state.filePath.empty() || !p.state.mediaPath.empty())
        return false;
    for (const auto &a : p.axes)
        if (a.showInStrip)
            return false;
    return true;
}

// Close any open project and settle on a clean no-project state. clearDirtyFlags() first so the
// close runs synchronously (guardUnsaved sees nothing dirty) instead of raising the save prompt.
void closeToNoProject(ImGuiTestContext *ctx) {
    auto &proj = *getTestState().project;
    auto &eq = *getTestState().eventQueue;
    proj.clearDirtyFlags();
    eq.push(CloseProjectRequestEvent{});
    ctx->Yield(3);
    IM_CHECK(noProjectOpen(proj));
    IM_CHECK(!anyModalOpen());
}
} // namespace

// Coverage for the app running with NO project loaded — every other suite opens a fixture first, so
// this is the only place the no-project render path and its menu/dirty invariants are exercised.
void RegisterNoProjectTests(ImGuiTestEngine *e) {
    // Smoke test: with no project, the always-on chrome (menu bar, footer) plus the welcome screen
    // must render across several frames without dereferencing absent project state. The editor windows
    // (timeline, simulator, statistics, …) deliberately do not render here — that branch lives in the
    // welcome suite; this guards the no-project render path and the menu/dirty invariants.
    IM_REGISTER_TEST(e, "noproject", "renders_without_project")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx); // start from a known-good open project, then close it
        closeToNoProject(ctx);
        ctx->Yield(10); // let every window submit its no-project frame
        IM_CHECK(noProjectOpen(*getTestState().project));
    };

    // Regression: a non-existent project cannot have unsaved changes. A dirty-marking handler firing
    // while no project is open (e.g. the video view-mode toggle, which runs even on the "No Project"
    // screen) must mark nothing dirty at the source, so the next Close raises no "Unsaved Changes"
    // prompt. setDirty()'s hasActiveProject guard is what holds this invariant.
    IM_REGISTER_TEST(e, "noproject", "dirty_without_project_raises_no_prompt")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        closeToNoProject(ctx);

        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        eq.push(VideoModeChangedEvent{VideoMode::VrMode}); // real dirty-marking handler, no project open
        ctx->Yield(3);
        IM_CHECK(!proj.state.settingsDirty); // guard kept the flag clean at the source

        eq.push(CloseProjectRequestEvent{}); // would prompt if the toggle had marked us dirty
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };
}
