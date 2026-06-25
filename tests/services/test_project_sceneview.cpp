#include "Core/SceneViewEvents.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::test::PmFixture;

// ProjectManager merges the two capture half-events into the SceneView for the chapter at the
// cursor (or the project-level fallback), and resolveActiveSceneView() restores the right one when
// the cursor's chapter changes.

TEST_CASE("Overlay-anchor capture writes the chapter containing the cursor") {
    PmFixture f;
    f.project().bookmarks.chapters.push_back({.startTime = 0.0, .endTime = 5.0, .name = "a"});
    f.project().playback.cursorPos = 2.0;

    ofs::OverlayAnchor anc;
    anc.center3dNorm = {0.25f, 0.75f};
    f.send(ofs::CaptureOverlayAnchorEvent{anc});

    REQUIRE(f.project().bookmarks.chapters[0].sceneView.has_value());
    CHECK(f.project().bookmarks.chapters[0].sceneView->anchor.center3dNorm.x == doctest::Approx(0.25f));
    CHECK(f.project().activeSceneView.anchor.center3dNorm.y == doctest::Approx(0.75f));
}

TEST_CASE("Capture outside any chapter writes the project-level fallback") {
    PmFixture f;
    f.project().playback.cursorPos = 9.0; // no chapters

    ofs::OverlayAnchor anc;
    anc.vrYaw = 1.0f;
    f.send(ofs::CaptureOverlayAnchorEvent{anc});

    CHECK(f.project().defaultSceneView.anchor.vrYaw == doctest::Approx(1.0f));
    CHECK(f.project().bookmarks.chapters.empty());
}

TEST_CASE("Framing and anchor captures update independent halves of the same SceneView") {
    PmFixture f;
    f.project().playback.cursorPos = 1.0; // no chapter → default

    ofs::OverlayAnchor anc;
    anc.size3dNorm = 0.42f;
    f.send(ofs::CaptureOverlayAnchorEvent{anc});

    ofs::VideoFraming fr;
    fr.zoomFactor = 2.5f;
    f.send(ofs::CaptureVideoFramingEvent{fr});

    // The framing capture must not have clobbered the anchor half, nor vice versa.
    CHECK(f.project().defaultSceneView.anchor.size3dNorm == doctest::Approx(0.42f));
    CHECK(f.project().defaultSceneView.framing.zoomFactor == doctest::Approx(2.5f));
}

TEST_CASE("resolveActiveSceneView restores the chapter view, then the fallback, as the cursor moves") {
    PmFixture f;
    f.project().bookmarks.chapters.push_back({.startTime = 0.0, .endTime = 5.0, .name = "a"});

    // Seed chapter a (cursor inside) and the project default (cursor outside) with distinct views.
    f.project().playback.cursorPos = 2.0;
    ofs::OverlayAnchor inChapter;
    inChapter.center3dNorm = {0.1f, 0.2f};
    f.send(ofs::CaptureOverlayAnchorEvent{inChapter});

    f.project().playback.cursorPos = 9.0;
    ofs::OverlayAnchor outside;
    outside.center3dNorm = {0.9f, 0.9f};
    f.send(ofs::CaptureOverlayAnchorEvent{outside});

    // Crossing back into chapter a resolves to its remembered view.
    f.project().playback.cursorPos = 2.0;
    f.pm.update(0.0f);
    CHECK(f.project().activeSceneView.anchor.center3dNorm.x == doctest::Approx(0.1f));

    // Crossing back out resolves to the project default.
    f.project().playback.cursorPos = 9.0;
    f.pm.update(0.0f);
    CHECK(f.project().activeSceneView.anchor.center3dNorm.x == doctest::Approx(0.9f));
}
