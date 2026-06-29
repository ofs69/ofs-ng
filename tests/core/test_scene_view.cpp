#include "Core/BookmarkChapterState.h"
#include "Core/SceneViewTransition.h"
#include "Format/Project.h"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <numbers>

using namespace ofs;

TEST_CASE("chapterIndexAt uses half-open ranges and reports gaps") {
    BookmarkChapterState s;
    CHECK(s.chapterIndexAt(0.0) == -1); // empty

    s.chapters.push_back({.startTime = 0.0, .endTime = 5.0, .name = "a"});
    s.chapters.push_back({.startTime = 5.0, .endTime = 10.0, .name = "b"});

    CHECK(s.chapterIndexAt(-1.0) == -1);  // before first
    CHECK(s.chapterIndexAt(0.0) == 0);    // start is inclusive
    CHECK(s.chapterIndexAt(4.999) == 0);  // inside a
    CHECK(s.chapterIndexAt(5.0) == 1);    // end is exclusive → next chapter
    CHECK(s.chapterIndexAt(9.5) == 1);    // inside b
    CHECK(s.chapterIndexAt(10.0) == -1);  // end is exclusive → gap
    CHECK(s.chapterIndexAt(100.0) == -1); // after last
}

TEST_CASE("chapterIndexAt returns the first (earliest-starting) of overlapping chapters") {
    BookmarkChapterState s;
    s.chapters.push_back({.startTime = 0.0, .endTime = 8.0, .name = "a"});
    s.chapters.push_back({.startTime = 4.0, .endTime = 12.0, .name = "b"});
    CHECK(s.chapterIndexAt(5.0) == 0); // both contain 5.0; earliest-starting wins
    CHECK(s.chapterIndexAt(10.0) == 1);
}

TEST_CASE("SceneView round-trips through Project json; missing fields default") {
    Project p;
    p.defaultSceneView.framing.zoomFactor = 3.0f;
    p.defaultSceneView.framing.vrRotation = {0.3f, 0.7f};
    p.defaultSceneView.anchor.vrYaw = 1.25f;

    Chapter c{.startTime = 0.0, .endTime = 5.0, .name = "a"};
    SceneView sv;
    sv.anchor.center3dNorm = {0.2f, 0.8f};
    sv.anchor.p1Norm = {0.1f, 0.15f};
    sv.anchor.widthNorm = 0.2f;
    sv.anchor.vrBarP1 = {0.4f, 0.25f};
    sv.anchor.vrBarP2 = {-0.4f, -0.25f};
    sv.anchor.vrBarWidthAngle = 0.09f;
    sv.framing.vrZoom = 0.9f;
    sv.inverted = true;
    c.sceneView = sv;
    p.bookmarkChapters.chapters.push_back(c);

    const nlohmann::json j = p;
    const Project p2 = j.get<Project>();

    CHECK(p2.defaultSceneView.framing.zoomFactor == doctest::Approx(3.0f));
    CHECK(p2.defaultSceneView.framing.vrRotation.y == doctest::Approx(0.7f));
    CHECK(p2.defaultSceneView.anchor.vrYaw == doctest::Approx(1.25f));
    REQUIRE(p2.bookmarkChapters.chapters.size() == 1);
    REQUIRE(p2.bookmarkChapters.chapters[0].sceneView.has_value());
    const OverlayAnchor &a = p2.bookmarkChapters.chapters[0].sceneView->anchor;
    CHECK(a.center3dNorm.y == doctest::Approx(0.8f));
    CHECK(a.p1Norm.x == doctest::Approx(0.1f));
    CHECK(a.widthNorm == doctest::Approx(0.2f));
    CHECK(a.vrBarP1.x == doctest::Approx(0.4f));
    CHECK(a.vrBarP2.y == doctest::Approx(-0.25f));
    CHECK(a.vrBarWidthAngle == doctest::Approx(0.09f));
    CHECK(p2.bookmarkChapters.chapters[0].sceneView->framing.vrZoom == doctest::Approx(0.9f));
    CHECK(p2.bookmarkChapters.chapters[0].sceneView->inverted == true);
}

TEST_CASE("lerp interpolates the linear scene-view fields") {
    SceneView a;
    a.framing.zoomFactor = 1.0f;
    a.anchor.center3dNorm = {0.0f, 0.0f};
    SceneView b;
    b.framing.zoomFactor = 3.0f;
    b.anchor.center3dNorm = {1.0f, 0.5f};

    const SceneView m = lerp(a, b, 0.5f);
    CHECK(m.framing.zoomFactor == doctest::Approx(2.0f));
    CHECK(m.anchor.center3dNorm.x == doctest::Approx(0.5f));
    CHECK(m.anchor.center3dNorm.y == doctest::Approx(0.25f));

    const SceneView ends0 = lerp(a, b, 0.0f);
    const SceneView ends1 = lerp(a, b, 1.0f);
    CHECK(ends0.framing.zoomFactor == doctest::Approx(1.0f));
    CHECK(ends1.framing.zoomFactor == doctest::Approx(3.0f));
}

TEST_CASE("lerp takes the shortest arc across a wrapping yaw") {
    // Normalized yaw wraps with period 1.0: 0.9 → 0.1 is +0.2 the short way, not -0.8 the long way.
    SceneView a, b;
    a.framing.vrRotation.x = 0.9f;
    b.framing.vrRotation.x = 0.1f;
    const float mid = lerp(a, b, 0.5f).framing.vrRotation.x;
    CHECK(mid == doctest::Approx(1.0f)); // halfway along +0.2 from 0.9 == 1.0 (≡ 0.0)

    // Radian yaw (overlay) wraps with period 2π: just below 2π → just above 0 stays near the seam.
    constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
    SceneView c, d;
    c.anchor.vrYaw = twoPi - 0.1f;
    d.anchor.vrYaw = 0.1f;
    const float y = lerp(c, d, 0.5f).anchor.vrYaw;
    CHECK(y == doctest::Approx(twoPi)); // crosses 2π, not back through π
}

TEST_CASE("lerp snaps the discrete inverted flag at the midpoint") {
    SceneView a, b;
    a.inverted = false;
    b.inverted = true;
    CHECK(lerp(a, b, 0.49f).inverted == false);
    CHECK(lerp(a, b, 0.5f).inverted == true);
}

TEST_CASE("SceneViewTransition eases from start to target and then settles") {
    SceneView a, b;
    a.framing.zoomFactor = 1.0f;
    b.framing.zoomFactor = 2.0f;

    SceneViewTransition t(0.25f);
    CHECK_FALSE(t.active()); // idle until started

    t.start(a, b);
    CHECK(t.active());
    const float mid = t.advance(0.1f).framing.zoomFactor;
    CHECK(mid > 1.0f);
    CHECK(mid < 2.0f);
    CHECK(t.active());

    t.advance(0.1f);
    const float end = t.advance(0.1f).framing.zoomFactor; // total dt 0.3 > 0.25 duration
    CHECK(end == doctest::Approx(2.0f));
    CHECK_FALSE(t.active());
}

TEST_CASE("SceneViewTransition snap and cancel skip the glide") {
    SceneView a, b;
    a.framing.zoomFactor = 1.0f;
    b.framing.zoomFactor = 2.0f;

    SceneViewTransition snapper;
    snapper.snap(b);
    CHECK_FALSE(snapper.active());
    CHECK(snapper.target().framing.zoomFactor == doctest::Approx(2.0f));

    SceneViewTransition canceller;
    canceller.start(a, b);
    CHECK(canceller.active());
    canceller.cancel();
    CHECK_FALSE(canceller.active());
}

TEST_CASE("An old project file without scene-view fields loads with defaults") {
    nlohmann::json pj = Project{};
    // A chapter object as written before per-chapter scene memory existed (no "sceneView" key).
    pj["bookmarkChapters"]["chapters"] = nlohmann::json::array({{{"startTime", 0.0}, {"endTime", 1.0}, {"name", "x"}}});
    // ...and no "defaultSceneView" key at all.
    pj.erase("defaultSceneView");

    const Project p = pj.get<Project>();
    REQUIRE(p.bookmarkChapters.chapters.size() == 1);
    CHECK_FALSE(p.bookmarkChapters.chapters[0].sceneView.has_value());
    CHECK(p.defaultSceneView.framing.zoomFactor == doctest::Approx(1.0f)); // struct default
}
