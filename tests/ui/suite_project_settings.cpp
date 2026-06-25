#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/OverlaySettings.h"
#include "Core/ScriptProject.h"
#include "Video/VideoPlayerSettings.h"
#include "helpers/TestState.h"
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Drives the Project window's Settings tab (src/UI/ProjectConfigWindow.cpp::renderSettingsTab). The
// Metadata tab is covered by suite_metadata.cpp. Settings-tab controls write project state through
// purpose events — OverlaySettingsChangedEvent, ModifyEvent<VideoPlayerState>, ChangeDummyDurationEvent
// — read back through getTestState().project. The fixture (basic.ofp) ships overlay=Frame/fps=30,
// videoPlayer=Full/100%, dummyDuration=60, no media, so the dummy-player branch of the Media section is
// the one rendered.
//
// Combos vs drags: a DragFloat with a "##id" label is findable by the **/ wildcard, but ImGui::Combo
// items register with an *empty* debug label, so **/ can't match them. Combos are therefore addressed
// by an exact, hashed path relative to the resolved Settings-tab child window (the same technique
// suite_shortcuts uses for its ##mode combo); drags keep the simpler **/ form.

namespace {
constexpr const char *kWin = "//Project###project_config";
constexpr const char *kMenu = "//##MainMenuBar/###menu_edit/###menu_project_config";

// The Settings tab body is a BeginChild whose window name carries a runtime hash suffix, so WindowInfo
// can't seed it (see reference_ui_test_engine_refs); resolve it by its structural "##settings_scroll"
// id. Match on that id alone — never on the "<parent>" portion of the name, which is the window's
// translated title and shifts per language.
ImGuiWindow *settingsChild() {
    for (ImGuiWindow *w : ImGui::GetCurrentContext()->Windows)
        if (std::strstr(w->Name, "##settings_scroll") != nullptr)
            return w;
    return nullptr;
}

// loadFixture → open the window (toggle-guarded) → grow it so the whole form is unclipped → Settings tab.
void openSettingsTab(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
        ctx->MenuClick(kMenu);
        ctx->Yield(2);
    }
    ctx->WindowResize(kWin, ImVec2(560.f, 900.f));
    ctx->SetRef(kWin);
    ctx->ItemClick("**/settings_tab");
    ctx->Yield(2);
}

void closeWindow(ImGuiTestContext *ctx) {
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window != nullptr)
        ctx->WindowClose(kWin);
    ctx->Yield(2);
}

// Pick `option` from the ImGui::Combo at `comboPath` (an exact path relative to the Settings-tab child,
// e.g. "##vp_form/##mode"). Opens the combo, clicks the option in the focused popup, restores the ref.
void comboPick(ImGuiTestContext *ctx, const char *comboPath, const char *option) {
    ImGuiWindow *child = settingsChild();
    IM_CHECK_SILENT(child != nullptr);
    ctx->SetRef(child);
    ctx->ItemClick(comboPath);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    // `option` is each item's stable ###id (e.g. "**/mode_vr"), not its visible label, so the click
    // survives translation. Resolution options stay percentages — numeric, not translatable.
    ctx->ItemClick(option);
    ctx->Yield(2);
    ctx->SetRef(kWin);
}
} // namespace

void RegisterProjectSettingsTests(ImGuiTestEngine *e) {
    // ── Overlay type combo: Frame → Tempo pushes OverlaySettingsChangedEvent ──────────
    IM_REGISTER_TEST(e, "projectsettings", "overlay_type_switch")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(proj.overlay.overlay == ofs::ScriptingOverlay::Frame); // fixture default

        comboPick(ctx, "##overlay_form/##overlay_type", "**/overlay_tempo");
        IM_CHECK(proj.overlay.overlay == ofs::ScriptingOverlay::Tempo);

        comboPick(ctx, "##overlay_form/##overlay_type", "**/overlay_frame"); // restore
        IM_CHECK(proj.overlay.overlay == ofs::ScriptingOverlay::Frame);
        closeWindow(ctx);
    };

    // ── Overlay FPS drag (Frame mode) writes frameFps ─────────────────────────────────
    IM_REGISTER_TEST(e, "projectsettings", "overlay_fps_drag")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_LT(std::abs(proj.overlay.frameFps - 30.f), 0.01f); // fixture default

        ctx->ItemInputValue("**/##fps", 60.f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.overlay.frameFps - 60.f), 0.5f);
        closeWindow(ctx);
    };

    // ── Overlay BPM drag (Tempo mode) writes tempoBpm ─────────────────────────────────
    // Switching to Tempo reveals the BPM/Offset/Snap rows; the BPM drag is the Tempo branch's widget.
    IM_REGISTER_TEST(e, "projectsettings", "overlay_bpm_drag")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;

        comboPick(ctx, "##overlay_form/##overlay_type", "**/overlay_tempo");
        IM_CHECK(proj.overlay.overlay == ofs::ScriptingOverlay::Tempo);

        ctx->ItemInputValue("**/##bpm", 90.f);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.overlay.tempoBpm - 90.f), 0.5f);

        comboPick(ctx, "##overlay_form/##overlay_type", "**/overlay_frame"); // restore
        closeWindow(ctx);
    };

    // ── Video-player Mode combo: Full → VR writes activeMode ──────────────────────────
    IM_REGISTER_TEST(e, "projectsettings", "video_mode_switch")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(proj.videoPlayer.activeMode == ofs::VideoMode::Full); // fixture default

        comboPick(ctx, "##vp_form/##mode", "**/mode_vr");
        IM_CHECK(proj.videoPlayer.activeMode == ofs::VideoMode::VrMode);

        comboPick(ctx, "##vp_form/##mode", "**/mode_full"); // restore
        IM_CHECK(proj.videoPlayer.activeMode == ofs::VideoMode::Full);
        closeWindow(ctx);
    };

    // ── Video-player Resolution combo writes resolutionScale ──────────────────────────
    IM_REGISTER_TEST(e, "projectsettings", "video_resolution_switch")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_LT(std::abs(proj.videoPlayer.resolutionScale - 1.0f), 0.001f); // 100% default

        comboPick(ctx, "##vp_form/##res", "**/50%");
        IM_CHECK_LT(std::abs(proj.videoPlayer.resolutionScale - 0.5f), 0.001f);

        comboPick(ctx, "##vp_form/##res", "**/100%"); // restore
        closeWindow(ctx);
    };

    // ── Media section (no video → dummy player): the duration field + Apply parse path ──
    // The fixture has no media and an active dummy player, so the duration input is shown. Typing a
    // HH:MM:SS string and Apply pushes ChangeDummyDurationEvent with the parsed seconds.
    IM_REGISTER_TEST(e, "projectsettings", "dummy_duration_apply")->TestFunc = [](ImGuiTestContext *ctx) {
        openSettingsTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_LT(std::abs(proj.state.dummyDuration - 60.0), 0.01); // fixture default

        ctx->ItemInput("**/##dummy_dur");
        ctx->KeyCharsReplace("0:02:00"); // 2 minutes
        ctx->ItemClick("**/dur_apply");
        ctx->Yield(3);
        IM_CHECK_LT(std::abs(proj.state.dummyDuration - 120.0), 0.01);

        // Restore so suites that assume the fixture's 60 s duration aren't disturbed mid-run.
        ctx->ItemInput("**/##dummy_dur");
        ctx->KeyCharsReplace("60");
        ctx->ItemClick("**/dur_apply");
        ctx->Yield(3);
        IM_CHECK_LT(std::abs(proj.state.dummyDuration - 60.0), 0.01);
        closeWindow(ctx);
    };

    // ── Markers tab: the per-row seek button seeks the playhead ─────────────────────────
    // Seed one chapter and one bookmark via the document event, then click each row's seek button
    // (###chseek / ###bmseek) and read the cursor back through proj.playback.
    IM_REGISTER_TEST(e, "projectsettings", "markers_rows_seek")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        const double chStart = dur * 0.5;
        const double bmTime = dur * 0.3;
        eq.push(ofs::ModifyBookmarkChapterEvent{.apply = [chStart, dur](ofs::BookmarkChapterState &s) {
            s.chapters.push_back({.startTime = chStart, .endTime = dur * 0.7, .name = "c"});
        }});
        eq.push(ofs::ModifyBookmarkChapterEvent{
            .apply = [bmTime](ofs::BookmarkChapterState &s) { s.bookmarks.push_back({.time = bmTime, .name = "b"}); }});
        ctx->Yield(2);

        if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
            ctx->MenuClick(kMenu);
            ctx->Yield(2);
        }
        ctx->WindowResize(kWin, ImVec2(560.f, 900.f));
        ctx->SetRef(kWin);
        ctx->ItemClick("**/markers_tab");
        ctx->Yield(2);

        ctx->ItemClick("**/chseek");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - chStart), dur * 0.05);

        ctx->ItemClick("**/bmseek");
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - bmTime), dur * 0.05);
        closeWindow(ctx);
    };

    // ── Markers tab: editing a chapter/bookmark name writes it back to the document ──────
    // The name inputs (###chname / ###bmname) push ModifyBookmarkChapterEvent; read the result back
    // through proj.bookmarks. One undo step per gesture is exercised by the undo-system suite; here we
    // just confirm the edit path reaches the document.
    IM_REGISTER_TEST(e, "projectsettings", "markers_name_edit")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        eq.push(ofs::ModifyBookmarkChapterEvent{.apply = [dur](ofs::BookmarkChapterState &s) {
            s.chapters.push_back({.startTime = dur * 0.5, .endTime = dur * 0.7, .name = "c"});
        }});
        eq.push(ofs::ModifyBookmarkChapterEvent{
            .apply = [dur](ofs::BookmarkChapterState &s) { s.bookmarks.push_back({.time = dur * 0.3, .name = "b"}); }});
        ctx->Yield(2);

        if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
            ctx->MenuClick(kMenu);
            ctx->Yield(2);
        }
        ctx->WindowResize(kWin, ImVec2(560.f, 900.f));
        ctx->SetRef(kWin);
        ctx->ItemClick("**/markers_tab");
        ctx->Yield(2);

        ctx->ItemInput("**/##chname");
        ctx->KeyCharsReplaceEnter("Intro");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.bookmarks.chapters[0].name.c_str(), "Intro");

        ctx->ItemInput("**/##bmname");
        ctx->KeyCharsReplaceEnter("Cue");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.bookmarks.bookmarks[0].name.c_str(), "Cue");
        closeWindow(ctx);
    };
}
