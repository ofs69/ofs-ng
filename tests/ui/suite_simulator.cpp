#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/SceneView.h"
#include "Core/SceneViewEvents.h"
#include "Core/SimulatorSettings.h"
#include "helpers/TestState.h"
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow::Active — not in the public API
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

namespace {

constexpr float kEps = 1e-4f;

bool approx(float a, float b) {
    return std::fabs(a - b) <= kEps;
}
bool approxVec(const ImVec2 &a, const ImVec2 &b) {
    return approx(a.x, b.x) && approx(a.y, b.y);
}

// A fully-distinct SceneView, derived from `seed`, so a per-chapter restore is unambiguous: every
// serialized per-scene field (2D translation/zoom, VR rotation/zoom, 2D bar pos+scale, 3D rect
// pos+scale, VR pin+angular size, invert) differs from the struct defaults and from other seeds.
ofs::SceneView makeView(float seed) {
    ofs::SceneView v;
    v.framing.zoomFactor = 1.0f + seed;                                  // 2D zoom
    v.framing.translation = {0.10f * seed, 0.20f * seed};                // 2D pan
    v.framing.vrRotation = {0.30f + 0.05f * seed, 0.40f + 0.05f * seed}; // VR yaw/pitch (normalized)
    v.framing.vrZoom = 0.50f + 0.05f * seed;                             // VR zoom
    v.anchor.p1Norm = {0.10f * seed, 0.15f * seed};                      // 2D bar position
    v.anchor.p2Norm = {0.20f * seed, 0.25f * seed};
    v.anchor.widthNorm = 0.05f + 0.02f * seed;            // 2D bar scale (thickness)
    v.anchor.center3dNorm = {0.30f * seed, 0.35f * seed}; // 3D rect position
    v.anchor.size3dNorm = 0.20f + 0.03f * seed;           // 3D rect scale
    v.anchor.vrYaw = 0.10f * seed;                        // 3D rect VR pin
    v.anchor.vrPitch = 0.05f * seed;
    v.anchor.vrAngularSize = 0.40f + 0.02f * seed; // 3D rect VR scale
    v.inverted = (static_cast<int>(seed) % 2) == 1;
    return v;
}

// Push the three half-events the simulator/video windows would emit on user adjustment. Together
// they populate the SceneView for the chapter (or default) at the current cursor.
void captureView(ofs::EventQueue &eq, const ofs::SceneView &v) {
    eq.push(ofs::CaptureVideoFramingEvent{v.framing});
    eq.push(ofs::CaptureOverlayAnchorEvent{v.anchor});
    eq.push(ofs::CaptureSimInvertedEvent{v.inverted});
}

// Assert every restored per-scene field, void-returning so the IM_CHECK early-return is well-formed.
void checkViewEq(const ofs::SceneView &got, const ofs::SceneView &want) {
    IM_CHECK(approx(got.framing.zoomFactor, want.framing.zoomFactor));
    IM_CHECK(approxVec(got.framing.translation, want.framing.translation));
    IM_CHECK(approxVec(got.framing.vrRotation, want.framing.vrRotation));
    IM_CHECK(approx(got.framing.vrZoom, want.framing.vrZoom));
    IM_CHECK(approxVec(got.anchor.p1Norm, want.anchor.p1Norm));
    IM_CHECK(approxVec(got.anchor.p2Norm, want.anchor.p2Norm));
    IM_CHECK(approx(got.anchor.widthNorm, want.anchor.widthNorm));
    IM_CHECK(approxVec(got.anchor.center3dNorm, want.anchor.center3dNorm));
    IM_CHECK(approx(got.anchor.size3dNorm, want.anchor.size3dNorm));
    IM_CHECK(approx(got.anchor.vrYaw, want.anchor.vrYaw));
    IM_CHECK(approx(got.anchor.vrPitch, want.anchor.vrPitch));
    IM_CHECK(approx(got.anchor.vrAngularSize, want.anchor.vrAngularSize));
    IM_CHECK_EQ(got.inverted, want.inverted);
}

// Two non-overlapping chapters with a gap on either side, so cursor seeks can land inside each
// chapter and in "no chapter" territory.
void setTwoChapters(ofs::EventQueue &eq) {
    eq.push(ofs::ModifyBookmarkChapterEvent{.apply = [](ofs::BookmarkChapterState &s) {
        s.chapters.clear();
        s.chapters.push_back({.startTime = 10.0, .endTime = 20.0, .name = "A"});
        s.chapters.push_back({.startTime = 30.0, .endTime = 40.0, .name = "B"});
    }});
}

} // namespace

void RegisterSimulatorTests(ImGuiTestEngine *e) {
    // The Simulator window exists only to host the 3D simulator's two extra orthographic views: it is
    // hidden in 2D mode (where the bar lives entirely on the video overlay) and shown in 3D mode.
    IM_REGISTER_TEST(e, "simulator", "window_hidden_in_2d_shown_in_3d")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        // Force 2D (simulator mode is app-global and may leak in from a prior test). FindWindowByName
        // (not ctx->WindowInfo) so a missing/inactive window is just a null pointer, not a logged error.
        eq.push(ofs::ModifyEvent<ofs::SimulatorState>{[](ofs::SimulatorState &s) { s.use3dSimulator = false; }});
        ctx->Yield(3);
        IM_CHECK(!proj.simulator.use3dSimulator);
        ImGuiWindow *win = ImGui::FindWindowByName("Simulator###Simulator");
        IM_CHECK(win == nullptr || !win->Active); // hidden in 2D mode

        // Switching to 3D shows the window (the 3D scene + extra views render, no crash).
        eq.push(ofs::ModifyEvent<ofs::SimulatorState>{[](ofs::SimulatorState &s) { s.use3dSimulator = true; }});
        ctx->Yield(3);
        IM_CHECK(proj.simulator.use3dSimulator);
        win = ImGui::FindWindowByName("Simulator###Simulator");
        IM_CHECK(win != nullptr);
        IM_CHECK(win->Active);

        // Back to 2D hides it again.
        eq.push(ofs::ModifyEvent<ofs::SimulatorState>{[](ofs::SimulatorState &s) { s.use3dSimulator = false; }});
        ctx->Yield(3);
        IM_CHECK(!proj.simulator.use3dSimulator);
        win = ImGui::FindWindowByName("Simulator###Simulator");
        IM_CHECK(win == nullptr || !win->Active);
    };

    IM_REGISTER_TEST(e, "simulator", "toggle_invert")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const bool before = proj.activeSceneView.inverted;
        // Invert moved to the overlay's right-click menu (needs a live video to open), so drive the
        // underlying half-event directly — the same event the menu item pushes.
        eq.push(ofs::CaptureSimInvertedEvent{!before});
        ctx->Yield(3);
        IM_CHECK_EQ(proj.activeSceneView.inverted, !before);
    };

    // The core per-chapter scene-memory contract: each chapter remembers its own video framing
    // (2D pan/zoom, VR rotation/zoom) and simulator-overlay placement (2D bar pos+scale, 3D rect
    // pos+scale, VR pin+angular size, invert), and the active scene resolves to the chapter under
    // the cursor as it crosses boundaries. Chapter A is a Full-mode view, chapter B a VR-mode view,
    // so both framing field sets are exercised in one round-trip.
    IM_REGISTER_TEST(e, "simulator", "scene_view_restored_per_chapter")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 120.0}); // give the cursor room to seek
        ctx->Yield(2);
        setTwoChapters(eq);
        ctx->Yield(2);

        const ofs::SceneView vA = makeView(1.0f);
        const ofs::SceneView vB = makeView(2.0f);

        // Adjust the scene while the cursor sits inside chapter A, then inside chapter B.
        eq.push(ofs::SeekEvent{15.0});
        ctx->Yield(2);
        captureView(eq, vA);
        ctx->Yield(2);

        eq.push(ofs::SeekEvent{35.0});
        ctx->Yield(2);
        captureView(eq, vB);
        ctx->Yield(2);

        // Cross back into A: the active scene must restore A's full view, not B's.
        eq.push(ofs::SeekEvent{15.0});
        ctx->Yield(3);
        checkViewEq(proj.activeSceneView, vA);

        // ...and into B.
        eq.push(ofs::SeekEvent{35.0});
        ctx->Yield(3);
        checkViewEq(proj.activeSceneView, vB);

        // Re-entering A a second time must still yield A (restore is stable, not one-shot).
        eq.push(ofs::SeekEvent{15.0});
        ctx->Yield(3);
        checkViewEq(proj.activeSceneView, vA);

        // The chapter's stored override persists on the chapter struct, not just the active copy.
        const int idxA = proj.bookmarks.chapterIndexAt(15.0);
        IM_CHECK(idxA >= 0);
        IM_CHECK(proj.bookmarks.chapters[idxA].sceneView.has_value());
        checkViewEq(*proj.bookmarks.chapters[idxA].sceneView, vA);
    };

    // Capturing inside one chapter must not bleed into the project-level default (used outside any
    // chapter) or into a sibling chapter that was never adjusted.
    IM_REGISTER_TEST(e, "simulator", "scene_view_capture_is_isolated")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 120.0});
        ctx->Yield(2);
        setTwoChapters(eq);
        ctx->Yield(2);

        // Snapshot the default scene resolved in the gap before chapter A (cursor at 5s).
        eq.push(ofs::SeekEvent{5.0});
        ctx->Yield(3);
        const ofs::SceneView def0 = proj.activeSceneView;

        // Adjust only chapter A.
        eq.push(ofs::SeekEvent{15.0});
        ctx->Yield(2);
        captureView(eq, makeView(3.0f));
        ctx->Yield(2);

        // Back in the gap: the default fallback is unchanged.
        eq.push(ofs::SeekEvent{5.0});
        ctx->Yield(3);
        checkViewEq(proj.activeSceneView, def0);

        // Chapter B, never adjusted, also still resolves to the default (it has no override).
        eq.push(ofs::SeekEvent{35.0});
        ctx->Yield(3);
        checkViewEq(proj.activeSceneView, def0);
        const int idxB = proj.bookmarks.chapterIndexAt(35.0);
        IM_CHECK(idxB >= 0);
        IM_CHECK(!proj.bookmarks.chapters[idxB].sceneView.has_value());
    };
}
