#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptProject.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"
#include <doctest/doctest.h>

using ofs::AxisModifiedEvent;
using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::EventCapture;
using ofs::test::TestProject;

TEST_CASE("mutate sets axis.dirty") {
    TestProject tp;
    EventCapture<AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    CHECK_FALSE(tp.project.axes[0].dirty);
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    CHECK(tp.project.axes[0].dirty);
}

TEST_CASE("mutate pushes exactly one AxisModifiedEvent per call") {
    TestProject tp;
    EventCapture<AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

TEST_CASE("mutate pushes AxisModifiedEvent with correct role for each call") {
    TestProject tp;
    EventCapture<AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &) {}, tp.eq);
    tp.project.mutate(StandardAxis::R0, [](ofs::AxisState &) {}, tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 2);
    CHECK(cap.received[0].role == StandardAxis::L0);
    CHECK(cap.received[1].role == StandardAxis::R0);
}

TEST_CASE("mutate prunes selection entries whose actions were removed") {
    TestProject tp;
    EventCapture<AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.mutate(
        StandardAxis::L0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 50});
            a.actions.insert({2.0, 60});
            a.selection.insert({1.0, 50});
            a.selection.insert({2.0, 60});
        },
        tp.eq);
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].selection.size() == 2);

    // Remove one action — selection should lose it too.
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.erase({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].selection.size() == 1);
    CHECK(tp.project.axes[0].selection[0].at == doctest::Approx(2.0));
}

// Action invariants live at the input boundaries, not in mutate(); clampedAction() is the O(1) helper.
TEST_CASE("clampedAction clamps a negative time to 0 and a position into [0,100]") {
    CHECK(ofs::clampedAction(-1.5, 50).at == doctest::Approx(0.0));
    CHECK(ofs::clampedAction(2.0, 50).at == doctest::Approx(2.0));
    CHECK(ofs::clampedAction(1.0, 150).pos == 100);
    CHECK(ofs::clampedAction(1.0, -30).pos == 0);
    CHECK(ofs::clampedAction(1.0, 42).pos == 42);
}

TEST_CASE("sortRegions keeps regions sorted by startTime after out-of-order inserts") {
    TestProject tp;
    tp.eq.freeze();

    ofs::ProcessingRegion a;
    a.id = 1;
    a.startTime = 3.0;
    a.endTime = 4.0;

    ofs::ProcessingRegion b;
    b.id = 2;
    b.startTime = 1.0;
    b.endTime = 2.0;

    ofs::ProcessingRegion c;
    c.id = 3;
    c.startTime = 2.0;
    c.endTime = 3.0;

    tp.project.regions.push_back(a);
    tp.project.regions.push_back(b);
    tp.project.regions.push_back(c);
    tp.project.sortRegions();

    REQUIRE(tp.project.regions.size() == 3);
    CHECK(tp.project.regions[0].startTime == doctest::Approx(1.0));
    CHECK(tp.project.regions[1].startTime == doctest::Approx(2.0));
    CHECK(tp.project.regions[2].startTime == doctest::Approx(3.0));
}

TEST_CASE("sortRegions places multi-axis region before single-axis on equal startTime") {
    TestProject tp;
    tp.eq.freeze();

    ofs::ProcessingRegion single;
    single.id = 1;
    single.startTime = 1.0;
    single.endTime = 2.0;
    single.axisRoles.reset();
    single.axisRoles.set(0);

    ofs::ProcessingRegion multi;
    multi.id = 2;
    multi.startTime = 1.0;
    multi.endTime = 2.0;
    multi.axisRoles.reset();
    multi.axisRoles.set(0);
    multi.axisRoles.set(1);

    tp.project.regions.push_back(single);
    tp.project.regions.push_back(multi);
    tp.project.sortRegions();

    REQUIRE(tp.project.regions.size() == 2);
    CHECK(tp.project.regions[0].id == multi.id);
    CHECK(tp.project.regions[1].id == single.id);
}
