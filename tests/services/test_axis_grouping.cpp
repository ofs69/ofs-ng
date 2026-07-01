// Multi-axis editing via axis grouping: a single activeAxis-targeted edit fans out across the active
// group (ScriptProject::axesGrouping), as one undo step, with delta-mirrored moves and a multi-axis
// clipboard. The fixture wires an UndoSystem ahead of ProjectManager so fan-out atomicity is testable.

#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Format/AppSettings.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ProjectManager.h"
#include "Services/SelectIntentRouter.h"
#include "Services/SelectionModeRegistry.h"
#include "Services/UndoSystem.h"
#include "helpers/TestProject.h"
#include <doctest/doctest.h>

using namespace ofs;
using ofs::test::TestProject;

namespace {

constexpr StandardAxis L0 = StandardAxis::L0;
constexpr StandardAxis R0 = StandardAxis::R0;
constexpr StandardAxis R1 = StandardAxis::R1;
size_t ix(StandardAxis a) {
    return static_cast<size_t>(a);
}

struct GFixture {
    TestProject tp;
    AppSettings appSettings;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    UndoSystem undo; // before pm: its snapshot handlers must register first
    ProjectManager pm;
    SelectionModeRegistry selReg;
    SelectIntentRouter sel; // sole SelectRequestEvent subscriber: resolves a selection gesture per-axis

    // Audio-cue outcome events, counted to assert what a grouping request drives.
    int groupingChanged = 0;
    int activeChanged = 0;

    GFixture()
        : undo(tp.project, tp.eq), pm(tp.project, tp.eq, appSettings, jobSystem, effectReg),
          sel(tp.project, tp.eq, selReg) {
        appSettings.autoBackupEnabled = false;
        tp.eq.on<AxisGroupingChangedEvent>([this](const AxisGroupingChangedEvent &) { ++groupingChanged; });
        tp.eq.on<ActiveAxisChangedEvent>([this](const ActiveAxisChangedEvent &) { ++activeChanged; });
        tp.eq.freeze();
        jobSystem.start();
    }

    ScriptProject &proj() { return tp.project; }
    AxisState &axis(StandardAxis a) { return proj().axes[ix(a)]; }
    template <class E> void send(E e) {
        tp.eq.push(std::move(e));
        tp.eq.drain();
    }

    void showAxis(StandardAxis a, std::initializer_list<ScriptAxisAction> acts = {}) {
        axis(a).showInStrip = true;
        proj().mutate(
            a,
            [&](AxisState &s) {
                for (const auto &x : acts)
                    s.actions.insert(x);
            },
            tp.eq);
        tp.eq.drain();
    }

    void group(std::initializer_list<StandardAxis> axes, StandardAxis lead) {
        AxisRoles r;
        for (auto a : axes)
            r.set(ix(a));
        send(SetAxisGroupingEvent{.roles = r, .lead = lead});
    }
};

} // namespace

TEST_CASE("effectiveEditSet is a singleton without a group, the whole group with one") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.showAxis(R0, {{1.0, 10}});

    f.send(AxisSelectedEvent{L0});
    CHECK(f.proj().effectiveEditSet().count() == 1);
    CHECK(f.proj().effectiveEditSet().test(ix(L0)));

    f.group({L0, R0}, L0);
    CHECK(f.proj().effectiveEditSet().count() == 2);
    CHECK(f.proj().state.axesGrouping.test(ix(L0)));
    CHECK(f.proj().state.axesGrouping.test(ix(R0)));
}

TEST_CASE("Grouping and activation skip axes not shown in the panel") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.showAxis(R0, {{1.0, 10}});
    f.axis(R0).showInStrip = false; // present but hidden from the strip

    // A hidden axis can't be activated.
    f.send(AxisSelectedEvent{R0});
    CHECK(f.proj().state.activeAxis == L0);

    // Nor grouped: R0 is filtered out, collapsing back to single-axis editing on L0.
    f.group({L0, R0}, L0);
    CHECK(f.proj().state.axesGrouping.none());
    CHECK(f.proj().effectiveEditSet().count() == 1);
    CHECK(f.proj().state.activeAxis == L0);

    // Once shown in the panel it groups normally.
    f.axis(R0).showInStrip = true;
    f.group({L0, R0}, L0);
    CHECK(f.proj().state.axesGrouping.count() == 2);
}

TEST_CASE("Grouping with a hidden lead falls back to the first valid member") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.showAxis(R0, {{1.0, 10}});
    f.axis(R1).showInStrip = false; // R1 stays out of the panel

    // Request R1 as the lead though it isn't selectable: the requested lead is filtered out and the
    // group's first in-panel member (L0) becomes the lead instead.
    f.group({L0, R0, R1}, R1);
    CHECK(f.proj().state.activeAxis == L0);
    CHECK(f.proj().state.axesGrouping.count() == 2); // L0 + R0; R1 dropped
    CHECK(f.proj().state.axesGrouping.test(ix(L0)));
    CHECK(f.proj().state.axesGrouping.test(ix(R0)));
    CHECK_FALSE(f.proj().state.axesGrouping.test(ix(R1)));
}

TEST_CASE("Grouping with no in-panel members leaves the active axis unchanged") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.send(AxisSelectedEvent{L0});
    f.axis(R0).showInStrip = false;
    f.axis(R1).showInStrip = false;

    // Every requested role is hidden: the handler bails before touching activeAxis or the grouping.
    f.group({R0, R1}, R0);
    CHECK(f.proj().state.activeAxis == L0);
    CHECK(f.proj().state.axesGrouping.none());
}

TEST_CASE("Group select-all + remove fans across the group as one undo step") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(R0, {{1.0, 15}, {2.0, 25}});
    f.group({L0, R0}, L0);

    f.send(SelectRequestEvent{.gesture = SelectGesture::All, .axis = L0});
    CHECK(f.axis(L0).selection.size() == 2);
    CHECK(f.axis(R0).selection.size() == 2); // selection fanned out

    f.send(RemoveSelectedActionsEvent{L0});
    CHECK(f.axis(L0).actions.empty());
    CHECK(f.axis(R0).actions.empty());

    f.send(UndoEvent{}); // a single undo restores BOTH axes
    CHECK(f.axis(L0).actions.size() == 2);
    CHECK(f.axis(R0).actions.size() == 2);
}

TEST_CASE("Group move is absolute on the lead and delta-mirrored on members") {
    GFixture f;
    f.showAxis(L0, {{2.0, 20}});
    f.showAxis(R0, {{2.0, 80}});
    f.group({L0, R0}, L0);

    // Drag the lead's point at t=2 to t=2.5, pos 30 (Δt=+0.5, Δpos=+10).
    f.send(MoveActionEvent{.axis = L0, .fromAt = 2.0, .toAt = 2.5, .toPos = 30, .snapshot = true});

    REQUIRE(f.axis(L0).actions.size() == 1);
    CHECK(f.axis(L0).actions[0].at == doctest::Approx(2.5));
    CHECK(f.axis(L0).actions[0].pos == 30); // lead: absolute toPos

    REQUIRE(f.axis(R0).actions.size() == 1);
    CHECK(f.axis(R0).actions[0].at == doctest::Approx(2.5)); // same Δt
    CHECK(f.axis(R0).actions[0].pos == 90);                  // 80 + Δpos
}

TEST_CASE("Group move skips members with no point at the dragged time and locked members") {
    GFixture f;
    f.showAxis(L0, {{2.0, 20}});
    f.showAxis(R0, {{5.0, 80}}); // no point at t=2
    f.showAxis(R1, {{2.0, 40}});
    f.axis(R1).isLocked = true;
    f.group({L0, R0, R1}, L0);

    f.send(MoveActionEvent{.axis = L0, .fromAt = 2.0, .toAt = 2.5, .toPos = 30, .snapshot = true});

    CHECK(f.axis(R0).actions[0].at == doctest::Approx(5.0)); // untouched (no matching point)
    CHECK(f.axis(R1).actions[0].at == doctest::Approx(2.0)); // untouched (locked)
    CHECK(f.axis(R1).actions[0].pos == 40);
}

TEST_CASE("Group add inserts the same absolute point on every member") {
    GFixture f;
    f.showAxis(L0);
    f.showAxis(R0);
    f.group({L0, R0}, L0);

    f.send(AddActionAtTimeEvent{.axis = L0, .time = 4.0, .pos = 55});
    REQUIRE(f.axis(L0).actions.size() == 1);
    REQUIRE(f.axis(R0).actions.size() == 1);
    CHECK(f.axis(L0).actions[0].pos == 55);
    CHECK(f.axis(R0).actions[0].pos == 55);
}

TEST_CASE("Grouped copy captures each axis; paste restores each to its own role") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(R0, {{1.0, 70}, {2.0, 80}});
    f.group({L0, R0}, L0);

    f.send(SelectRequestEvent{.gesture = SelectGesture::All, .axis = L0}); // selects both
    f.send(CopySelectionEvent{});
    f.send(PasteActionsEvent{.pasteTime = 5.0, .exact = false}); // anchor 1.0 → offset +4

    CHECK(f.axis(L0).actions.contains(ScriptAxisAction{5.0, 0}));
    CHECK(f.axis(L0).actions.contains(ScriptAxisAction{6.0, 0}));
    CHECK(f.axis(R0).actions.contains(ScriptAxisAction{5.0, 0}));
    CHECK(f.axis(R0).actions.contains(ScriptAxisAction{6.0, 0}));
    // Each axis keeps its own positions — R0's clip didn't get L0's values.
    CHECK(f.axis(R0).actions.find(ScriptAxisAction{5.0, 0})->pos == 70);
    CHECK(f.axis(L0).actions.find(ScriptAxisAction{5.0, 0})->pos == 10);
}

TEST_CASE("A single-axis clipboard is broadcast across the active group on paste") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(R0); // empty

    // Copy L0 alone (no group), then group L0+R0 and paste → broadcast onto both.
    f.send(AxisSelectedEvent{L0});
    f.send(SelectRequestEvent{.gesture = SelectGesture::All, .axis = L0});
    f.send(CopySelectionEvent{});
    f.group({L0, R0}, L0);
    f.send(PasteActionsEvent{.pasteTime = 5.0, .exact = false});

    CHECK(f.axis(L0).actions.contains(ScriptAxisAction{5.0, 0}));
    CHECK(f.axis(R0).actions.contains(ScriptAxisAction{5.0, 0})); // broadcast
    CHECK(f.axis(R0).actions.contains(ScriptAxisAction{6.0, 0}));
}

TEST_CASE("Group cut is lossless: capture all, delete all, paste-exact restores all") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.showAxis(R0, {{1.0, 70}});
    f.group({L0, R0}, L0);
    f.send(SelectRequestEvent{.gesture = SelectGesture::All, .axis = L0});

    // Cut = CopySelection then RemoveSelected (what the edit.cut command pushes).
    f.send(CopySelectionEvent{});
    f.send(RemoveSelectedActionsEvent{L0});
    CHECK(f.axis(L0).actions.empty());
    CHECK(f.axis(R0).actions.empty());

    f.send(PasteActionsEvent{.pasteTime = 0.0, .exact = true});
    CHECK(f.axis(L0).actions.contains(ScriptAxisAction{1.0, 0}));
    CHECK(f.axis(R0).actions.find(ScriptAxisAction{1.0, 0})->pos == 70);
}

TEST_CASE("Switching only the group lead cues activation, not a re-grouping") {
    GFixture f;
    f.showAxis(L0, {{1.0, 10}});
    f.showAxis(R0, {{1.0, 10}});

    f.group({L0, R0}, L0); // forms the group → grouping cue
    CHECK(f.groupingChanged == 1);
    CHECK(f.activeChanged == 0);

    // Same group set, new lead: an active-axis change, so the activation cue fires and no second
    // grouping cue does.
    f.group({L0, R0}, R0);
    CHECK(f.proj().state.activeAxis == R0);
    CHECK(f.proj().state.axesGrouping.count() == 2);
    CHECK(f.groupingChanged == 1);
    CHECK(f.activeChanged == 1);

    // Re-issuing the identical group with the identical lead changes nothing → silent on both cues.
    f.group({L0, R0}, R0);
    CHECK(f.groupingChanged == 1);
    CHECK(f.activeChanged == 1);
}

TEST_CASE("CreateRegion spans every axis in axisRoles") {
    GFixture f;
    f.showAxis(L0);
    f.showAxis(R0);
    AxisRoles roles;
    roles.set(ix(L0));
    roles.set(ix(R0));
    f.send(CreateRegionEvent{.axisRole = L0, .axisRoles = roles, .startTime = 0.0, .endTime = 10.0});

    REQUIRE(f.proj().regions.size() == 1);
    CHECK(f.proj().regions[0].axisRoles.test(ix(L0)));
    CHECK(f.proj().regions[0].axisRoles.test(ix(R0)));
    CHECK(f.proj().regions[0].nodeGraph.nodes.size() == 4); // one Input→Output pair per axis
}
