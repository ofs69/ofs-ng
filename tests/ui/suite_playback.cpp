#include "Core/Events.h"
#include "helpers/TestState.h"
#include <imgui_te_engine.h>

static void yieldFrames(ImGuiTestContext *ctx, int n) {
    for (int i = 0; i < n; ++i)
        ctx->Yield();
}

void RegisterPlaybackTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "playback", "play_advances_cursor")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        IM_CHECK_EQ(proj.playback.cursorPos, 0.0);

        eq.push(ofs::PlayPauseEvent{});
        yieldFrames(ctx, 5);

        IM_CHECK_GT(proj.playback.cursorPos, 0.0);
    };

    IM_REGISTER_TEST(e, "playback", "pause_stops_cursor")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::PlayPauseEvent{});
        yieldFrames(ctx, 5);

        eq.push(ofs::PlayPauseEvent{});
        ctx->Yield();

        const double stoppedAt = proj.playback.cursorPos;
        IM_CHECK_GT(stoppedAt, 0.0);

        yieldFrames(ctx, 5);
        IM_CHECK_EQ(proj.playback.cursorPos, stoppedAt);
    };

    // A remote controller (the WebSocket API) asks for a state, not a flip. Two identical requests
    // drained in the same frame must leave that state applied once — as toggles they cancelled out.
    IM_REGISTER_TEST(e, "playback", "set_playing_is_absolute")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::SetPlayingEvent{true});
        eq.push(ofs::SetPlayingEvent{true});
        yieldFrames(ctx, 5);
        IM_CHECK_GT(proj.playback.cursorPos, 0.0);

        eq.push(ofs::SetPlayingEvent{false});
        eq.push(ofs::SetPlayingEvent{false});
        ctx->Yield();

        const double stoppedAt = proj.playback.cursorPos;
        yieldFrames(ctx, 5);
        IM_CHECK_EQ(proj.playback.cursorPos, stoppedAt);
    };
}
