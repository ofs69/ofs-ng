#include "Core/BookmarkChapterState.h"
#include "Format/Project.h"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

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
