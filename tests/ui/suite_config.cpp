#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Core/SimulatorSettings.h"
#include "Format/AppSettings.h"
#include "helpers/TestState.h"
#include <cmath>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Drives the Preferences window (src/UI/ConfigurationWindow.cpp) — the Application and Simulator
// tabs. The Theme tab is covered separately in suite_theme.cpp. Application-tab controls push
// ModifyEvent<AppSettings> (read back through OfsAppTestAccess::appSettings → getTestState().appSettings);
// Simulator-tab controls push ModifyEvent<SimulatorState> (read back through project.simulator). Every
// control is addressed by its stable ## id so the visible label/icon can change without breaking tests.

namespace {
constexpr const char *kWin = "//Preferences###preferences"; // ###id, absolute so it ignores SetRef
constexpr const char *kMenu = "//##MainMenuBar/###menu_edit/###menu_preferences";

bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// loadFixture → open Preferences if a prior suite didn't leave it open → grow it so every form row is
// unclipped. The menu item is a toggle, so only click it when the window isn't already up.
void openPrefs(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
        ctx->MenuClick(kMenu);
        ctx->Yield(2);
    }
    ctx->WindowResize(kWin, ImVec2(760.f, 1000.f));
    ctx->SetRef(kWin);
}

void openAppTab(ImGuiTestContext *ctx) {
    openPrefs(ctx);
    ctx->ItemClick("**/app_tab"); // tab bar lives in the parent window; address by stable ###id
    ctx->Yield(2);
}

void openSimTab(ImGuiTestContext *ctx) {
    openPrefs(ctx);
    ctx->ItemClick("**/sim_tab");
    ctx->Yield(2);
}

// Close without leaving the window (or its restart-required info modal) open for the next suite.
void closePrefs(ImGuiTestContext *ctx) {
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window != nullptr)
        ctx->WindowClose(kWin);
    ctx->Yield(2);
}
} // namespace

void RegisterConfigTests(ImGuiTestEngine *e) {
    // ── Application tab: HW-decoding checkbox round-trips through AppSettings ──────────
    // Restored to its open-time value before closing so the "Restart Required" modal (which fires only
    // when hwdec/fontSize differ from the open snapshot) is not raised here — that branch has its own test.
    IM_REGISTER_TEST(e, "config", "app_hwdec_toggle")->TestFunc = [](ImGuiTestContext *ctx) {
        openAppTab(ctx);
        const auto *s = getTestState().appSettings;
        const bool before = s->hwdecEnabled;

        ctx->ItemClick("**/##hwdec");
        ctx->Yield(2);
        IM_CHECK_EQ(s->hwdecEnabled, !before);

        ctx->ItemClick("**/##hwdec"); // restore (also avoids the restart-required modal on close)
        ctx->Yield(2);
        IM_CHECK_EQ(s->hwdecEnabled, before);
        closePrefs(ctx);
    };

    // ── Application tab: Auto-Backup checkbox ─────────────────────────────────────────
    IM_REGISTER_TEST(e, "config", "app_autobackup_toggle")->TestFunc = [](ImGuiTestContext *ctx) {
        openAppTab(ctx);
        const auto *s = getTestState().appSettings;
        const bool before = s->autoBackupEnabled;

        ctx->ItemClick("**/###auto_backup");
        ctx->Yield(2);
        IM_CHECK_EQ(s->autoBackupEnabled, !before);

        ctx->ItemClick("**/###auto_backup"); // restore the global default for later suites
        ctx->Yield(2);
        IM_CHECK_EQ(s->autoBackupEnabled, before);
        closePrefs(ctx);
    };

    // ── Application tab: Live-Reload checkbox ─────────────────────────────────────────
    IM_REGISTER_TEST(e, "config", "app_live_reload_toggle")->TestFunc = [](ImGuiTestContext *ctx) {
        openAppTab(ctx);
        const auto *s = getTestState().appSettings;
        const bool before = s->liveReloadTranslations;

        ctx->ItemClick("**/liveReload");
        ctx->Yield(2);
        IM_CHECK_EQ(s->liveReloadTranslations, !before);

        ctx->ItemClick("**/liveReload"); // restore
        ctx->Yield(2);
        closePrefs(ctx);
    };

    // ── Application tab: changing a restart-gated setting raises the info modal on close ──
    // hwdec (like fontSize) is captured at open; closing with it changed fires showInfo("Restart
    // Required"). Dismiss the modal and restore the global value so later suites see a clean slate.
    IM_REGISTER_TEST(e, "config", "app_restart_modal_on_close")->TestFunc = [](ImGuiTestContext *ctx) {
        openAppTab(ctx);
        auto &eq = *getTestState().eventQueue;
        const bool before = getTestState().appSettings->hwdecEnabled;

        ctx->ItemClick("**/##hwdec"); // diverge from the open-time snapshot
        ctx->Yield(2);
        IM_CHECK_EQ(getTestState().appSettings->hwdecEnabled, !before);

        ctx->WindowClose(kWin);
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // "Restart Required"
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/###modalbtn0"); // the single "OK" button
        ctx->Yield(2);
        IM_CHECK(!anyModalOpen());

        // The window is closed, so restore the global through the event queue rather than the UI.
        eq.push(ofs::ModifyEvent<ofs::AppSettings>{[before](ofs::AppSettings &s) { s.hwdecEnabled = before; }});
        ctx->Yield(2);
        IM_CHECK_EQ(getTestState().appSettings->hwdecEnabled, before);
    };

    // ── Simulator tab: feature-toggle checkboxes write SimulatorState ─────────────────
    IM_REGISTER_TEST(e, "config", "sim_feature_toggles")->TestFunc = [](ImGuiTestContext *ctx) {
        openSimTab(ctx);
        auto &proj = *getTestState().project;
        const bool indBefore = proj.simulator.enableIndicators; // default true
        const bool posBefore = proj.simulator.enablePosition;   // default false

        ctx->ItemClick("**/##simindicators");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.simulator.enableIndicators, !indBefore);

        ctx->ItemClick("**/##simshowpos");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.simulator.enablePosition, !posBefore);

        closePrefs(ctx);
    };

    // ── Simulator tab: a 3D-mapping range slider/drag writes the float ────────────────
    IM_REGISTER_TEST(e, "config", "sim_stroke_range_drag")->TestFunc = [](ImGuiTestContext *ctx) {
        openSimTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_LT(std::abs(proj.simulator.strokeRange - 1.0f), 0.01f); // default 1.0

        ctx->ItemInputValue("**/##simstroke", 2.5f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.simulator.strokeRange - 2.5f), 0.05f);

        closePrefs(ctx);
    };

    // ── Simulator tab: the Extra-Lines DragInt writes the int count ───────────────────
    IM_REGISTER_TEST(e, "config", "sim_extra_lines_count")->TestFunc = [](ImGuiTestContext *ctx) {
        openSimTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.simulator.extraLinesCount, 0); // default

        ctx->ItemInputValue("**/##extracount", 5);
        ctx->Yield(2);
        IM_CHECK_EQ(proj.simulator.extraLinesCount, 5);

        closePrefs(ctx);
    };
}
