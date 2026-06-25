#include "Core/Events.h"
#include "Util/ColorGen.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::StandardAxis;
using ofs::test::PmFixture;

TEST_CASE("CreateRegion adds a region, auto-selects it, builds a default graph") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    REQUIRE(f.project().regions.size() == 1);
    const auto &r = f.project().regions[0];
    CHECK(f.project().procSelRegionId == r.id);
    CHECK(r.axisRoles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK(r.nodeGraph.nodes.size() >= 2); // one Input + one Output
}

TEST_CASE("CreateRegion auto-assigns a distinct golden-ratio band color") {
    PmFixture f;
    const auto seed = static_cast<size_t>(f.project().state.autoNameSeed);
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 20.0, .endTime = 30.0});
    REQUIRE(f.project().regions.size() == 2);
    // The color is derived from the generator at index = autoNameSeed + (region count before insert),
    // so the two regions get different, deterministic colors (regions stay sorted by startTime).
    CHECK(f.project().regions[0].color == ofs::util::goldenRatioColor(seed + 0));
    CHECK(f.project().regions[1].color == ofs::util::goldenRatioColor(seed + 1));
    CHECK(f.project().regions[0].color != f.project().regions[1].color);
}

TEST_CASE("ModifyRegion updates a region's band color") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    REQUIRE(f.project().regions.size() == 1);
    auto updated = f.project().regions[0];
    updated.color = IM_COL32(1, 2, 3, 200);
    f.send(ofs::ModifyRegionEvent{.regionId = updated.id, .updatedRegion = updated});
    CHECK(f.project().regions[0].color == IM_COL32(1, 2, 3, 200));
}

TEST_CASE("CreateRegion rejects spans shorter than 0.5 s") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 1.2});
    CHECK(f.project().regions.empty());
}

TEST_CASE("CreateRegion snaps a new region into the free slot after an overlapping one") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 5.0, .endTime = 15.0}); // anchor inside
    REQUIRE(f.project().regions.size() == 2);
    // Regions are sorted by startTime; the second begins where the first ends (no overlap) and
    // keeps its requested 10 s span since nothing follows it.
    const auto &second = f.project().regions[1];
    CHECK(second.startTime == doctest::Approx(10.0));
    CHECK(second.endTime == doctest::Approx(20.0));
}

TEST_CASE("CreateRegion snaps past a region on another axis (regions share one track)") {
    PmFixture f;
    f.project().axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::R0, .startTime = 5.0, .endTime = 15.0});
    REQUIRE(f.project().regions.size() == 2);
    CHECK(f.project().regions[1].startTime == doctest::Approx(10.0));
}

TEST_CASE("CreateRegion fits its span to the gap before the next region") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 5.0});
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 10.0, .endTime = 15.0});
    // Requested [6,14] overlaps the second region; the gap [5,10] caps the new region at 10.
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 6.0, .endTime = 14.0});
    REQUIRE(f.project().regions.size() == 3);
    const auto &mid = f.project().regions[1]; // sorted: [0,5], [6,10], [10,15]
    CHECK(mid.startTime == doctest::Approx(6.0));
    CHECK(mid.endTime == doctest::Approx(10.0));
}

TEST_CASE("CreateRegion clamps a snapped region to the timeline duration") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    // Anchor 5 is inside [0,10] and snaps to 10; the 5 s requested span would reach 15, but the
    // 12 s timeline caps it. Without a bound it would run past the end of the media.
    f.send(ofs::CreateRegionEvent{
        .axisRole = StandardAxis::L0, .startTime = 5.0, .endTime = 10.0, .timelineDuration = 12.0});
    REQUIRE(f.project().regions.size() == 2);
    const auto &second = f.project().regions[1];
    CHECK(second.startTime == doctest::Approx(10.0));
    CHECK(second.endTime == doctest::Approx(12.0));
}

TEST_CASE("CreateRegion emits a warning toast and creates nothing when no gap fits") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 10.2, .endTime = 20.0});
    // Anchor 5 snaps to 10, but the next region starts at 10.2 — only a 0.2 s gap (< 0.5 s min).
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 5.0, .endTime = 9.0});
    CHECK(f.project().regions.size() == 2);
    REQUIRE(f.notifications.size() == 1);
    CHECK(f.notifications[0].level == ofs::NotifyLevel::Warning);
}

TEST_CASE("AssignAxis allows removing the last axis, leaving a zero-axis region") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    const int id = f.project().regions[0].id;
    f.send(ofs::AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::L0, .assign = false});
    REQUIRE(f.project().regions.size() == 1);
    CHECK(f.project().regions[0].axisRoles.count() == 0);
}

TEST_CASE("DeleteRegion removes it and clears the selection id") {
    PmFixture f;
    f.send(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    const int id = f.project().regions[0].id;
    f.send(ofs::DeleteRegionEvent{.regionId = id});
    CHECK(f.project().regions.empty());
    CHECK(f.project().procSelRegionId == -1);
}
