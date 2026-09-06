#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "UI/Icons.h"
#include "UI/Notifications.h"
#include "Video/DummyVideoPlayer.h"
#include "Video/VideoPlayer.h"
#include "helpers/TestState.h"
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h> // ImRect
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <string>

// Map a bookmark-bar time to a screen pixel. Anchors to the addressable "##bookmarkbar" item (registered
// in VideoPlayerControls.cpp), so no bar geometry is hardcoded. The X mapping mirrors drawBookmarkBar's toX
// (linear over duration across the bar width). Used for a continuous *time* on the bar (e.g. a drag
// destination); an existing bookmark is a real item, addressed by its ##bookmark_<i> id instead.
static ImVec2 bookmarkPixel(ImGuiTestContext *ctx, double time) {
    const ImRect r = ctx->ItemInfo("Video Controls###video_controls/##controls/##bookmarkbar").RectFull;
    const double dur = getTestState().project->state.dummyDuration;
    const float x = r.Min.x + static_cast<float>(time / dur) * r.GetWidth();
    return ImVec2(x, r.GetCenter().y);
}

void RegisterVideoControlsTests(ImGuiTestEngine *e) {
    // Clicking the seek bar centre seeks to ~half the duration.
    IM_REGISTER_TEST(e, "videocontrols", "scrubber_click_seeks")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        const double dur = proj.state.dummyDuration;
        IM_CHECK_GT(dur, 0.0);

        ctx->ItemClick(
            "Video Controls###video_controls/##controls/###TimelineWidget"); // clicks item centre -> position 0.5
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(proj.playback.cursorPos - dur * 0.5), dur * 0.15);
    };

    // Dragging the seek bar moves the cursor.
    IM_REGISTER_TEST(e, "videocontrols", "scrubber_drag_seeks")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        const ImGuiTestItemInfo info = ctx->ItemInfo("Video Controls###video_controls/##controls/###TimelineWidget");
        const ImRect r = info.RectFull;

        ctx->MouseMoveToPos(ImVec2(r.Min.x + r.GetWidth() * 0.25f, r.GetCenter().y));
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(ImVec2(r.Min.x + r.GetWidth() * 0.80f, r.GetCenter().y));
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_GT(proj.playback.cursorPos, proj.state.dummyDuration * 0.5);
    };

    // Clicking a bookmark seeks the playhead to its time.
    IM_REGISTER_TEST(e, "videocontrols", "bookmark_click_seeks")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        const double bmTime = dur * 0.4;
        eq.push(ofs::ModifyBookmarkChapterEvent{
            .apply = [bmTime](ofs::BookmarkChapterState &s) { s.bookmarks.push_back({.time = bmTime, .name = "b"}); }});
        ctx->Yield(2);
        IM_CHECK_EQ(proj.bookmarks.bookmarks.size(), static_cast<size_t>(1));

        // The bookmark is a real item — click it by id (no pixel mapping).
        ctx->ItemClick("Video Controls###video_controls/##controls/##bookmark_0");
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(proj.playback.cursorPos - bmTime), dur * 0.05);
    };

    // Dragging a bookmark moves it to a new time.
    IM_REGISTER_TEST(e, "videocontrols", "bookmark_drag_moves_time")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        const double bmTime = dur * 0.4;
        eq.push(ofs::ModifyBookmarkChapterEvent{
            .apply = [bmTime](ofs::BookmarkChapterState &s) { s.bookmarks.push_back({.time = bmTime, .name = "b"}); }});
        ctx->Yield(2);

        // Grab the bookmark by its id, drag to the destination time (a continuous spot on the bar, so the
        // destination is the one place a time→pixel map is intrinsic).
        const double targetTime = dur * 0.6;
        ctx->MouseMove("Video Controls###video_controls/##controls/##bookmark_0");
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(bookmarkPixel(ctx, targetTime));
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(proj.bookmarks.bookmarks[0].time - targetTime), dur * 0.05);
    };

    // Clicking a chapter band walks the chapter: start, then end, then start again.
    IM_REGISTER_TEST(e, "videocontrols", "chapter_click_cycles_start_then_end")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        const double chStart = dur * 0.3;
        const double chEnd = dur * 0.6;
        eq.push(ofs::ModifyBookmarkChapterEvent{.apply = [chStart, chEnd](ofs::BookmarkChapterState &s) {
            s.chapters.push_back({.startTime = chStart, .endTime = chEnd, .name = "c"});
        }});
        ctx->Yield(2);
        IM_CHECK_EQ(proj.bookmarks.chapters.size(), static_cast<size_t>(1));

        // The band is drawn by BandBar's own hit testing, not as an addressable item, so the click
        // goes to a computed pixel inside the band (mid-span, clear of both resize edge zones).
        ctx->MouseMoveToPos(bookmarkPixel(ctx, (chStart + chEnd) * 0.5));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - chStart), dur * 0.05);

        // Playhead still inside the chapter the first click put it in → advance to its end.
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - chEnd), dur * 0.05);

        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - chStart), dur * 0.05);
    };

    // A chapter created after a middle one was deleted must not wear a surviving chapter's color:
    // the auto color is picked by probing for a free slot, not from the chapter count.
    IM_REGISTER_TEST(e, "videocontrols", "chapter_colors_stay_distinct_after_delete")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &proj = *getTestState().project;
            const double dur = proj.state.dummyDuration;

            auto addChapterAt = [ctx](ImVec2 pos) {
                ctx->MouseMoveToPos(pos);
                ctx->MouseClick(ImGuiMouseButton_Right);
                ctx->Yield(2);
                ctx->ItemClick("**/###add_chapter_here");
                ctx->Yield(2);
            };
            // Each chapter spans 10% of the duration from the clicked time, so these three don't collide.
            addChapterAt(bookmarkPixel(ctx, dur * 0.05));
            addChapterAt(bookmarkPixel(ctx, dur * 0.40));
            addChapterAt(bookmarkPixel(ctx, dur * 0.70));
            IM_CHECK_EQ(proj.bookmarks.chapters.size(), static_cast<size_t>(3));

            ctx->MouseMoveToPos(bookmarkPixel(ctx, dur * 0.45)); // inside the middle chapter
            ctx->MouseClick(ImGuiMouseButton_Right);
            ctx->Yield(2);
            ctx->ItemClick("**/###vpc_ch_delete");
            ctx->Yield(2);
            IM_CHECK_EQ(proj.bookmarks.chapters.size(), static_cast<size_t>(2));

            addChapterAt(bookmarkPixel(ctx, dur * 0.45)); // into the gap the delete left
            IM_CHECK_EQ(proj.bookmarks.chapters.size(), static_cast<size_t>(3));

            const auto &chs = proj.bookmarks.chapters;
            IM_CHECK(chs[0].color != chs[1].color);
            IM_CHECK(chs[1].color != chs[2].color);
            IM_CHECK(chs[0].color != chs[2].color);
        };

    // The transport play/pause button toggles the player's paused state. The dummy player starts
    // paused, so the first click plays and the second pauses; the button glyph flips with it.
    IM_REGISTER_TEST(e, "videocontrols", "play_pause_button_toggles")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto *player = getTestState().videoPlayer;
        IM_CHECK(player != nullptr);
        IM_CHECK(player->isPaused()); // dummy starts paused

        ctx->ItemClick((std::string("Video Controls###video_controls/**/") + ICON_PLAY).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(player->isPaused(), false);

        ctx->ItemClick((std::string("Video Controls###video_controls/**/") + ICON_PAUSE).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(player->isPaused(), true);
    };

    // The forward/backward transport buttons seek the playhead by a fixed 3 s step.
    IM_REGISTER_TEST(e, "videocontrols", "seek_step_buttons")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const double dur = proj.state.dummyDuration;
        IM_CHECK_GT(dur, 8.0); // need room for a 3 s step in each direction

        // Forward from 0 → ~3 s.
        ctx->ItemClick((std::string("Video Controls###video_controls/**/") + ICON_FORWARD).c_str());
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 3.0), 0.2);

        // From a known mid-point, backward → ~3 s earlier.
        eq.push(ofs::SeekEvent{6.0});
        ctx->Yield(2);
        ctx->ItemClick((std::string("Video Controls###video_controls/**/") + ICON_BACKWARD).c_str());
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(proj.playback.cursorPos - 3.0), 0.2);
    };

    // The speed slider drives the player's playback speed.
    IM_REGISTER_TEST(e, "videocontrols", "speed_slider_sets_speed")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto *player = getTestState().videoPlayer;
        IM_CHECK(player != nullptr);
        IM_CHECK_LT(std::abs(player->getPlaybackSpeed() - 1.0f), 0.01f); // default 1.0x

        // The speed slider stretches to the bottom-right corner, where toast notifications also
        // stack. A leaked error toast from an earlier suite would cover it and block the hover, so
        // clear any pending toasts first (pure test hygiene; doesn't touch what's under test).
        if (auto *notifications = getTestState().notifications)
            notifications->toasts.clear();
        ctx->Yield();

        ctx->ItemInputValue("Video Controls###video_controls/**/##Speed", 1.5f);
        ctx->Yield(2);

        IM_CHECK_LT(std::abs(player->getPlaybackSpeed() - 1.5f), 0.05f);
    };

    // Regression: the render-target size handed to the player must describe the pixels the image
    // actually covers. It is derived from live ImGui viewport state, and reading a viewport's
    // FramebufferScale bare yields (0,0) on the main viewport (ImGui fills it in only for secondary
    // platform viewports), which collapsed the request to a 1x1 target — the video became one flat
    // colour. Only a real ImGui context can catch that; the arithmetic alone unit-tests clean.
    IM_REGISTER_TEST(e, "videocontrols", "render_size_tracks_displayed_image")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;

        auto *dummy = static_cast<ofs::DummyVideoPlayer *>(getTestState().videoPlayer);
        // A source far larger than the panel, so the "never exceed the source" cap cannot bind and
        // the request is free to track the displayed size exactly.
        constexpr int kSrcW = 3840;
        constexpr int kSrcH = 2160;
        dummy->setFakeVideoForTesting(kSrcW, kSrcH, 1);
        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 60.0});
        eq.push(ofs::ModifyEvent<ofs::VideoPlayerState>{[](ofs::VideoPlayerState &v) {
            v.activeMode = ofs::VideoMode::Full;
            v.resolutionScale = 1.0f;
        }});
        ctx->Yield(2);

        // The Video Player shares its dock node with the Processing panel; bring it forward so it
        // renders the image and issues a render-size request.
        ctx->WindowFocus("Video Player###video_player");
        ctx->Yield(3);

        const int fullW = dummy->lastRenderWidth;
        const int fullH = dummy->lastRenderHeight;
        IM_CHECK(fullW > 1); // a collapsed request is the bug this guards
        IM_CHECK(fullH > 1);
        IM_CHECK(fullW <= kSrcW); // never asks for more than the source carries
        IM_CHECK(fullH <= kSrcH);
        // The image is fitted to the panel, so the request keeps the source's aspect.
        const float aspect = static_cast<float>(fullW) / static_cast<float>(fullH);
        IM_CHECK(std::fabs(aspect - static_cast<float>(kSrcW) / static_cast<float>(kSrcH)) < 0.05f);

        // Halving the user's quality setting halves the request: it tracks the settings rather than
        // being a constant that merely happens to look sane.
        eq.push(ofs::ModifyEvent<ofs::VideoPlayerState>{[](ofs::VideoPlayerState &v) { v.resolutionScale = 0.5f; }});
        ctx->Yield(3);
        IM_CHECK(std::abs(dummy->lastRenderWidth - fullW / 2) <= 2);
        IM_CHECK(std::abs(dummy->lastRenderHeight - fullH / 2) <= 2);

        // Restore the normal "no media" dummy. Media presence gates real commands — with ffmpeg on
        // PATH it is the only thing holding video.optimize-intra disabled — so a leaked fake frame
        // fails a later suite rather than this one.
        dummy->setFakeVideoForTesting(0, 0, 0);
        ctx->Yield(2);
    };
}
