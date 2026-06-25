#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "UI/Heatmap.h"
#include "helpers/TestState.h"
#include <cmath>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Render-without-crash smoke tests for windows that previously had zero coverage.
// They guard against /WX-clean code that still asserts or dereferences null at
// runtime. The pattern is: render a few frames, then assert the window exists.
// (Window ### titles resolve correctly because ImHashStr resets at "###", so the
// engine's GetID and ImGui's window ID agree.)
static void expectWindow(ImGuiTestContext *ctx, const char *name) {
    ctx->Yield(3);
    IM_CHECK(ctx->WindowInfo(name).Window != nullptr);
}

void RegisterWindowTests(ImGuiTestEngine *e) {
    // ── Always-on docked windows ──────────────────────────────────────────────
    IM_REGISTER_TEST(e, "windows", "statistics_renders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        expectWindow(ctx, "Statistics###statistics");
    };

    IM_REGISTER_TEST(e, "windows", "simulator_renders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        expectWindow(ctx, "Simulator###Simulator");
    };

    IM_REGISTER_TEST(e, "windows", "video_controls_render")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        expectWindow(ctx, "Video Controls###video_controls");
    };

    // ── Toggle windows opened from the main menu ──────────────────────────────
    IM_REGISTER_TEST(e, "windows", "preferences_opens_and_renders")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->MenuClick("//##MainMenuBar/###menu_edit/###menu_preferences"); // sets appState.showConfigWindow = true
        expectWindow(ctx, "Preferences###preferences");
        ctx->WindowClose("Preferences###preferences"); // close again — don't leak the open window into later suites
    };

    IM_REGISTER_TEST(e, "windows", "shortcut_window_renders_bindings")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_shortcuts"); // toggles appState.showShortcutWindow
        expectWindow(ctx, "Shortcut Bindings###shortcut_bindings");
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_shortcuts"); // close again — don't leak the open
                                                                          // (NoDocking) window into later suites
    };

    // ── Bookmark bar (drawn inside Video Controls) reflects an added bookmark ──
    IM_REGISTER_TEST(e, "windows", "bandbar_renders_after_add_bookmark")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const size_t before = proj.bookmarks.bookmarks.size();

        eq.push(ofs::ModifyBookmarkChapterEvent{
            .apply = [](ofs::BookmarkChapterState &s) { s.bookmarks.push_back({.time = 1.5, .name = "test"}); }});
        ctx->Yield(2);

        IM_CHECK_EQ(proj.bookmarks.bookmarks.size(), before + 1);
        expectWindow(ctx, "Video Controls###video_controls"); // bookmark bar drawn here; assert no crash
    };

    // ── Heatmap (drawn inside Video Controls) with real action data ───────────
    IM_REGISTER_TEST(e, "windows", "heatmap_renders_with_actions")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});

        ofs::VectorSet<ofs::ScriptAxisAction> actions;
        actions.insert({1.0, 10});
        actions.insert({2.0, 90});
        actions.insert({3.0, 20});
        eq.push(ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::L0, .actions = actions});

        expectWindow(ctx, "Video Controls###video_controls"); // heatmap strip recomputes from L0 actions
    };

    // ── Heatmap speed-binning math (pure; no GL) ──────────────────────────────
    // computeSpeedBuffer is the data path behind the heatmap texture: it bins each
    // stroke's speed (|Δpos|/Δt) into kSpeedResolution time slots, averages overlaps,
    // normalizes by maxSpeed, and clamps to [0,1]. These checks pin that math directly.
    IM_REGISTER_TEST(e, "windows", "heatmap_compute_speed_buffer")->TestFunc = [](ImGuiTestContext *) {
        using ofs::Heatmap;
        using ofs::ScriptAxisAction;
        using ofs::VectorSet;
        constexpr int N = Heatmap::kSpeedResolution;
        constexpr float kEps = 1e-4f;
        const VectorSet<ScriptAxisAction> empty;

        // Non-positive duration => empty buffer (update() then skips the GL upload).
        IM_CHECK(Heatmap::computeSpeedBuffer(0.0, empty, 100.f).empty());
        IM_CHECK(Heatmap::computeSpeedBuffer(-5.0, empty, 100.f).empty());

        // Valid duration, no strokes => full zero buffer of kSpeedResolution samples.
        {
            const auto buf = Heatmap::computeSpeedBuffer(static_cast<double>(N), empty, 100.f);
            IM_CHECK(static_cast<int>(buf.size()) == N);
            IM_CHECK(buf.front() == 0.f);
            IM_CHECK(buf.back() == 0.f);
        }

        // totalDuration == N makes timeStep == 1.0, so action.at floors straight to a bin index.
        // Single stroke confined to bin 0: speed = 50 / 0.5 = 100; /maxSpeed(200) = 0.5.
        {
            VectorSet<ScriptAxisAction> a;
            a.insert({0.0, 0});
            a.insert({0.5, 50});
            const auto buf = Heatmap::computeSpeedBuffer(static_cast<double>(N), a, 200.f);
            IM_CHECK(static_cast<int>(buf.size()) == N);
            IM_CHECK(std::abs(buf[0] - 0.5f) < kEps);
            IM_CHECK(buf[1] == 0.f);
        }

        // Stroke spanning bins 0..10: speed = 100/10 = 10; /maxSpeed(100) = 0.1 across [0,10], 0 after.
        {
            VectorSet<ScriptAxisAction> a;
            a.insert({0.0, 0});
            a.insert({10.0, 100});
            const auto buf = Heatmap::computeSpeedBuffer(static_cast<double>(N), a, 100.f);
            for (int i = 0; i <= 10; ++i)
                IM_CHECK(std::abs(buf[i] - 0.1f) < kEps);
            IM_CHECK(buf[11] == 0.f);
        }

        // Two strokes sharing bin 0 are averaged: speeds 100 and 200 => mean 150; /maxSpeed(300) = 0.5.
        {
            VectorSet<ScriptAxisAction> a;
            a.insert({0.0, 0});
            a.insert({0.3, 30}); // stroke0: 30 / 0.3 = 100
            a.insert({0.6, 90}); // stroke1: 60 / 0.3 = 200
            const auto buf = Heatmap::computeSpeedBuffer(static_cast<double>(N), a, 300.f);
            IM_CHECK(std::abs(buf[0] - 0.5f) < kEps);
        }

        // Speeds above maxSpeed clamp to 1.0: 100 / 0.1 = 1000; /maxSpeed(100) = 10 => clamped.
        {
            VectorSet<ScriptAxisAction> a;
            a.insert({0.0, 0});
            a.insert({0.1, 100});
            const auto buf = Heatmap::computeSpeedBuffer(static_cast<double>(N), a, 100.f);
            IM_CHECK(buf[0] == 1.0f);
        }
    };
}
