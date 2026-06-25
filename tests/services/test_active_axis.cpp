#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::StandardAxis;
using ofs::test::PmFixture;

static size_t ix(StandardAxis a) {
    return static_cast<size_t>(a);
}

// Make a standard axis present and give it the same three-point set as seedL0_3.
static void present3(PmFixture &f, StandardAxis ax) {
    f.project().axes[ix(ax)].showInStrip = true;
    ofs::VectorSet<ofs::ScriptAxisAction> a;
    a.insert({1.0, 10});
    a.insert({2.0, 20});
    a.insert({3.0, 30});
    f.send(ofs::CommitAxisActionsEvent{.axis = ax, .actions = a});
}

TEST_CASE("Default active axis is L0") {
    PmFixture f;
    CHECK(f.project().state.activeAxis == StandardAxis::L0);
}

TEST_CASE("AxisSelected sets the active axis and clears the region selection") {
    PmFixture f;
    f.project().axes[ix(StandardAxis::R0)].showInStrip = true; // only panel-visible axes can be activated
    f.project().procSelRegionId = 7;                           // pretend a region was selected
    f.send(ofs::AxisSelectedEvent{StandardAxis::R0});
    CHECK(f.project().state.activeAxis == StandardAxis::R0);
    CHECK(f.project().procSelRegionId == -1);
}

TEST_CASE("AxisSelected is a no-op for an axis not shown in the panel") {
    PmFixture f;                                             // only L0 shown in the panel
    f.send(ofs::AxisSelectedEvent{StandardAxis::R0});        // R0 hidden → rejected
    CHECK(f.project().state.activeAxis == StandardAxis::L0); // unchanged

    f.project().axes[ix(StandardAxis::R0)].showInStrip = false; // exists but hidden from the strip
    f.send(ofs::AxisSelectedEvent{StandardAxis::R0});
    CHECK(f.project().state.activeAxis == StandardAxis::L0); // still rejected
}

TEST_CASE("Selection is per-axis: selecting L0 leaves R0 untouched") {
    PmFixture f;
    present3(f, StandardAxis::R0);
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    CHECK(f.project().axes[ix(StandardAxis::L0)].selection.size() == 3);
    CHECK(f.project().axes[ix(StandardAxis::R0)].selection.empty());
}

TEST_CASE("SetAxisSelection(empty) clears only that axis") {
    PmFixture f;
    present3(f, StandardAxis::R0);
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::R0});
    f.send(ofs::SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = {}});
    CHECK(f.project().axes[ix(StandardAxis::L0)].selection.empty());
    CHECK(f.project().axes[ix(StandardAxis::R0)].selection.size() == 3);
}

TEST_CASE("A per-axis SetAxisSelection loop clears every axis") {
    PmFixture f;
    present3(f, StandardAxis::R0);
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::R0});
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        if (f.project().axes[i].showInStrip)
            f.send(ofs::SetAxisSelectionEvent{.axis = static_cast<StandardAxis>(i), .selection = {}});
    CHECK(f.project().axes[ix(StandardAxis::L0)].selection.empty());
    CHECK(f.project().axes[ix(StandardAxis::R0)].selection.empty());
}

TEST_CASE("AxisSelected dissolves an active group; SetAxisGrouping forces the lead in") {
    PmFixture f;
    present3(f, StandardAxis::R0);
    f.seedL0_3();
    // Group L0+R0 with R0 as lead.
    ofs::AxisRoles g;
    g.set(ix(StandardAxis::L0));
    g.set(ix(StandardAxis::R0));
    f.send(ofs::SetAxisGroupingEvent{.roles = g, .lead = StandardAxis::R0});
    CHECK(f.project().state.activeAxis == StandardAxis::R0);
    CHECK(f.project().state.axesGrouping.count() == 2);
    CHECK(f.project().effectiveEditSet().count() == 2);

    // Activating any axis dissolves the group.
    f.send(ofs::AxisSelectedEvent{StandardAxis::L0});
    CHECK(f.project().state.axesGrouping.none());
    CHECK(f.project().effectiveEditSet().count() == 1);
}

TEST_CASE("Edits target a valid axis even when it is hidden from the panel") {
    PmFixture f; // R0 exists (all axes do) but is hidden
    f.send(ofs::AddActionAtTimeEvent{.axis = StandardAxis::R0, .time = 1.0, .pos = 50});
    CHECK(f.project().axes[ix(StandardAxis::R0)].actions.size() == 1);
}

TEST_CASE("Edits target the event's axis regardless of which axis is active") {
    PmFixture f;
    present3(f, StandardAxis::R0);
    f.send(ofs::AxisSelectedEvent{StandardAxis::L0});                                    // active = L0
    f.send(ofs::AddActionAtTimeEvent{.axis = StandardAxis::R0, .time = 4.0, .pos = 50}); // edits R0
    CHECK(f.project().axes[ix(StandardAxis::R0)].actions.size() == 4);
    CHECK(f.project().axes[ix(StandardAxis::L0)].actions.empty());
}
