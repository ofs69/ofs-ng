#include "Core/Events.h"
#include "Services/PluginApi.h"
#include "Services/PluginManager.h"
#include "helpers/PluginManagerTestAccess.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

using namespace ofs;

namespace {

// Records what the fake plugin's onBuildUI sees. PluginApi callbacks carry no context,
// so this is file-static; the test resets it before driving.
struct UiRec {
    int buildUiCalls = 0;
    int clicks = 0;
    const HostApi *host = nullptr;
};
UiRec g_ui;

// The fake plugin's onBuildUI draws host widgets through the HostApi it was given. The
// host fns read the live PluginCtx (currentPluginName / uiIdCounter) that renderUI sets
// per plugin per frame, so this exercises the real ImGui path, not a stub.
void fakeBuildUi() {
    ++g_ui.buildUiCalls;
    const HostApi *h = g_ui.host;
    h->uiLabel(h->ctx, "Fake plugin label");
    if (h->uiButton(h->ctx, "Click me"))
        ++g_ui.clicks;
}
const char *fakeName() {
    return "Fake UI Plugin";
}
PluginApi makeUiApi() {
    PluginApi a{};
    a.version = OFS_ABI_VERSION;
    a.onBuildUI = &fakeBuildUi;
    a.getName = &fakeName;
    return a;
}

} // namespace

void RegisterPluginsTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "plugins", "no_crash_with_no_plugins")->TestFunc = [](ImGuiTestContext *ctx) {
        for (int i = 0; i < 5; ++i)
            ctx->Yield();
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // onBuildUI is invoked each frame and host widgets work in the live ImGui context: a real click on
    // a plugin-drawn button round-trips back through hostUiButton. Drives the app's own PluginManager
    // (the single one in the process) via the addTestPlugin seam — no second manager.
    IM_REGISTER_TEST(e, "plugins", "onbuildui_invokes_host_widgets")->TestFunc = [](ImGuiTestContext *ctx) {
        auto *pm = getTestState().pluginManager;
        IM_CHECK(pm != nullptr);
        g_ui = UiRec{};
        g_ui.host = &PluginManagerTestAccess::hostApi(*pm);
        PluginManagerTestAccess::addTestPlugin(*pm, "uitest", makeUiApi());

        // OfsApp's frame loop calls pluginManager->renderUI() every frame. One yield lets renderUI create
        // the window object; the injected window can land tabbed/behind in the shared dock layout, where
        // ImGui::Begin returns false and onBuildUI never runs, so bring it to the front (as the other
        // plugin-UI suites do) to make its tab active and draw its body.
        ctx->Yield();
        ctx->WindowFocus("Fake UI Plugin###uitest");
        ctx->SetRef("Fake UI Plugin###uitest");
        ctx->Yield(2);
        IM_CHECK_GT(g_ui.buildUiCalls, 0);

        // Click the plugin-drawn button; next frame onBuildUI sees uiButton() return true.
        const int clicksBefore = g_ui.clicks;
        ctx->ItemClick("**/Click me");
        ctx->Yield();
        IM_CHECK_GT(g_ui.clicks, clicksBefore);

        // Disable the injected plugin so its window doesn't linger into later suites.
        getTestState().eventQueue->push(ofs::SetPluginEnabledEvent{.name = "uitest", .enabled = false});
        ctx->Yield(2);
    };
}
