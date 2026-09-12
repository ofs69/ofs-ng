// Source actions live on the funscript millisecond grid. Two actions inside one grid slot are not
// representable — export rounds both to the same `at` and a reload merges them — and while they exist
// they share a single timeline dot, so an edit moves one of the pair and silently kinks the line at a
// stroke reversal. These pin the input boundaries that keep the grid invariant: add, drag, selection
// nudge, paste, and a plugin commit.

#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Format/AppSettings.h"
#include "Services/EditIntentRouter.h"
#include "Services/EditModeRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ProjectManager.h"
#include "Services/UndoSystem.h"
#include "helpers/TestProject.h"
#include <doctest/doctest.h>

#include <cmath>

using namespace ofs;
using ofs::test::TestProject;

namespace {

constexpr StandardAxis L0 = StandardAxis::L0;
constexpr size_t kL0 = static_cast<size_t>(StandardAxis::L0);

struct GridFixture {
    TestProject tp;
    AppSettings appSettings;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ExportMemory exportMemory;
    UndoSystem undo; // before pm: its snapshot handlers must register first
    ProjectManager pm;
    EditModeRegistry editReg;
    EditIntentRouter edit; // the path a keyboard move actually travels

    GridFixture()
        : undo(tp.project, tp.eq), pm(tp.project, tp.eq, appSettings, exportMemory, jobSystem, effectReg),
          edit(tp.project, tp.eq, editReg) {
        appSettings.autoBackupEnabled = false;
        tp.eq.freeze();
        jobSystem.start();
        tp.project.state.activeAxis = L0;
        axis().showInStrip = true;
    }

    AxisState &axis() { return tp.project.axes[kL0]; }
    const VectorSet<ScriptAxisAction> &actions() { return axis().actions; }

    template <class E> void send(E e) {
        tp.eq.push(std::move(e));
        tp.eq.drain();
    }

    void seed(std::initializer_list<ScriptAxisAction> acts) {
        tp.project.mutate(
            L0,
            [&](AxisState &a) {
                for (const auto &x : acts)
                    a.actions.insert(x);
            },
            tp.eq);
        tp.eq.drain();
    }

    // The action at `at`, or nullptr — by exact grid time, which is the point of the invariant.
    const ScriptAxisAction *at(double t) {
        auto it = actions().find(ScriptAxisAction{t, 0});
        return it == actions().end() ? nullptr : &*it;
    }
};

} // namespace

TEST_CASE("Adding a point inside an occupied grid slot updates it instead of creating a twin") {
    GridFixture f;
    f.seed({{1.0, 20}});

    // A click lands wherever the pixel maps to; at any zoom the mapped time is a full double. Without
    // snapping this inserts a second action 0.3 ms away that shares the first one's dot.
    f.send(AddActionAtTimeEvent{.axis = L0, .time = 1.0003, .pos = 30});

    REQUIRE(f.actions().size() == 1);
    REQUIRE(f.at(1.0) != nullptr);
    CHECK(f.at(1.0)->pos == 30);
}

TEST_CASE("Dragging a point onto its neighbour keeps the position change and refuses the time change") {
    GridFixture f;
    f.seed({{1.0, 20}, {1.001, 80}});

    // The reported incident's gesture: adjust a reversal point's height while its neighbour is one grid
    // slot away. erase-then-insert at an occupied time used to drop the dragged point entirely, because
    // VectorSet::insert refuses a duplicate rather than overwriting.
    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0, .toAt = 1.001, .toPos = 30, .snapshot = true});

    REQUIRE(f.actions().size() == 2);
    REQUIRE(f.at(1.0) != nullptr);
    CHECK(f.at(1.0)->pos == 30);   // the position edit still applies
    CHECK(f.at(1.001)->pos == 80); // the neighbour is untouched
}

TEST_CASE("Dragging a point to a free sub-millisecond time snaps it onto the grid") {
    GridFixture f;
    f.seed({{1.0, 20}, {2.0, 80}});

    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0, .toAt = 1.2297, .toPos = 40, .snapshot = true});

    REQUIRE(f.actions().size() == 2);
    REQUIRE(f.at(1.23) != nullptr);
    CHECK(f.at(1.23)->pos == 40);
    CHECK(f.at(1.0) == nullptr);
}

TEST_CASE("A plugin commit is snapped onto the grid") {
    GridFixture f;

    // A plugin supplies raw doubles. Snapping at the commit boundary keeps the in-memory set equal to
    // what a save/reload round-trip yields, rather than deferring the merge to the next load.
    VectorSet<ScriptAxisAction> supplied;
    for (const auto &a : {ScriptAxisAction{1.0, 10}, ScriptAxisAction{1.0004, 90}, ScriptAxisAction{2.4999, 50}})
        supplied.insert(a);
    f.send(CommitAxisActionsEvent{.axis = L0, .actions = std::move(supplied)});

    REQUIRE(f.actions().size() == 2);
    CHECK(f.at(1.0) != nullptr);
    CHECK(f.at(2.5) != nullptr);
}

TEST_CASE("Nudging a selection by a frame-time delta lands the points on the grid") {
    GridFixture f;
    f.seed({{1.0, 20}, {1.1, 80}});
    f.tp.project.mutate(L0, [](AxisState &a) { a.selection.insert({1.0, 20}); }, f.tp.eq);
    f.tp.eq.drain();

    // stepTime is a video frame (1/29.97 s here), which is never a whole millisecond — so an unsnapped
    // nudge walks the point off the grid and back into the unrepresentable zone.
    f.tp.project.overlay.overlay = ScriptingOverlay::Frame;
    f.tp.project.overlay.frameFps = 29.97f;
    f.send(MoveSelectionTimeEvent{.axis = L0, .direction = StepDirection::Forward, .seekAfter = false, .reps = 1});

    // 1/29.97 s = 33.3667 ms, so the snapped landing slot is 1.033 s exactly.
    REQUIRE(f.actions().size() == 2);
    CHECK(f.at(1.033) != nullptr);
    CHECK(f.at(1.1) != nullptr);
}

TEST_CASE("Pasting at an off-grid playhead lands the clip on the grid") {
    GridFixture f;
    f.seed({{5.0, 10}, {5.25, 90}});
    f.tp.project.mutate(
        L0,
        [](AxisState &a) {
            a.selection.insert({5.0, 10});
            a.selection.insert({5.25, 90});
        },
        f.tp.eq);
    f.tp.eq.drain();
    f.send(CopySelectionEvent{});
    f.tp.project.mutate(
        L0,
        [](AxisState &a) {
            a.actions = {};
            a.selection = {};
        },
        f.tp.eq);
    f.tp.eq.drain();

    // The playhead is wherever the video happens to be — a full double, not a whole millisecond.
    f.send(PasteActionsEvent{.pasteTime = 10.0007123, .exact = false});

    REQUIRE(f.actions().size() == 2);
    CHECK(f.at(10.001) != nullptr); // anchor snapped
    CHECK(f.at(10.251) != nullptr); // the clip's internal spacing is preserved exactly
}

TEST_CASE("A move whose source time names no action does not create one") {
    GridFixture f;
    f.seed({{1.0, 20}});

    // The drag loop re-sends fromAt as the time it last asked for. If that time was snapped on the way
    // in, fromAt names nothing — and erase-then-insert would then leave the original behind and add a
    // second point, one per frame, for the length of the drag.
    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0007, .toAt = 1.2, .toPos = 40, .snapshot = true});

    REQUIRE(f.actions().size() == 1);
    CHECK(f.at(1.0) != nullptr);
}

TEST_CASE("Repeated keyboard time nudges move one point instead of multiplying it") {
    GridFixture f;
    f.seed({{1.0, 20}, {5.0, 80}});
    f.tp.project.mutate(L0, [](AxisState &a) { a.selection.insert({1.0, 20}); }, f.tp.eq);
    f.tp.eq.drain();
    f.tp.project.overlay.overlay = ScriptingOverlay::Frame;
    f.tp.project.overlay.frameFps = 29.97f;

    for (int i = 0; i < 5; ++i)
        f.send(MoveSelectionTimeEvent{.axis = L0, .direction = StepDirection::Forward, .seekAfter = false});

    CHECK(f.actions().size() == 2);
    CHECK(f.axis().selection.size() == 1);
}

TEST_CASE("Repeated keyboard nudges with no selection move the nearest point only") {
    GridFixture f;
    f.seed({{1.0, 20}, {5.0, 80}});
    f.tp.project.playback.cursorPos = 1.0;
    f.tp.project.overlay.overlay = ScriptingOverlay::Frame;
    f.tp.project.overlay.frameFps = 29.97f;

    for (int i = 0; i < 5; ++i)
        f.send(MoveSelectionTimeEvent{.axis = L0, .direction = StepDirection::Forward, .seekAfter = true});

    CHECK(f.actions().size() == 2);
}

TEST_CASE("A held keyboard move through the intent router moves one point") {
    GridFixture f;
    f.seed({{1.0, 20}, {5.0, 80}});
    f.tp.project.playback.cursorPos = 1.0;
    f.tp.project.overlay.overlay = ScriptingOverlay::Frame;
    f.tp.project.overlay.frameFps = 29.97f;

    // The shape OfsAppCommands emits for a held Shift+Right: one Begin, then Continue per repeat.
    for (int i = 0; i < 6; ++i)
        f.send(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = L0,
                                           .direction = StepDirection::Forward,
                                           .reps = 1,
                                           .seekAfter = true},
                                .gesture = i == 0 ? GesturePhase::Begin : GesturePhase::Continue});

    CHECK(f.actions().size() == 2);
}

TEST_CASE("Repeatedly moving an action onto the playhead does not clone it") {
    GridFixture f;
    f.seed({{1.0, 20}, {5.0, 80}});
    f.tp.project.playback.cursorPos = 2.00049; // off-grid, as a real playhead is

    for (int i = 0; i < 4; ++i)
        f.send(MoveActionToCurrentTimeEvent{.axis = L0});

    CHECK(f.actions().size() == 2);
    CHECK(f.at(2.0) != nullptr); // landed on the grid, not at the raw playhead time
}

TEST_CASE("A mouse drag relocates one point instead of leaving a trail") {
    GridFixture f;
    f.seed({{1.0, 20}, {5.0, 80}});

    // The drag loop as ScriptTimeline drives it: each frame addresses the point by the time it last
    // wrote and asks for the cursor's time, snapped to the grid the store uses.
    double fromAt = 1.0;
    for (int frame = 1; frame <= 20; ++frame) {
        const double cursorTime = 1.0 + 0.0007123 * frame; // sub-grid cursor motion, as a real drag is
        const double target = snapAuthoredTime(cursorTime);
        if (target == fromAt)
            continue;
        f.send(MoveActionEvent{.axis = L0, .fromAt = fromAt, .toAt = target, .toPos = 20, .snapshot = frame == 1});
        fromAt = target;
    }

    CHECK(f.actions().size() == 2);
    CHECK(f.at(fromAt) != nullptr);
}

TEST_CASE("A drag cannot push a point through its neighbour") {
    GridFixture f;
    f.seed({{1.0, 20}, {1.001, 80}});

    // A fast cursor jumps clean past the neighbour in one frame. The point must not go with it: there
    // is no free slot between 1.0 and its neighbour, so the time holds and only the position tracks.
    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0, .toAt = 1.004, .toPos = 45, .snapshot = false});

    REQUIRE(f.actions().size() == 2);
    REQUIRE(f.at(1.0) != nullptr);
    CHECK(f.at(1.0)->pos == 45);   // the vertical edit lands
    CHECK(f.at(1.001)->pos == 80); // the neighbour is untouched, and still second
}

TEST_CASE("A move stops one slot short of a neighbour instead of hopping it") {
    GridFixture f;
    f.seed({{1.0, 20}, {1.010, 80}, {2.0, 30}});

    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0, .toAt = 1.5, .toPos = 20, .snapshot = true});

    REQUIRE(f.actions().size() == 3);
    CHECK(f.at(1.009) != nullptr); // clamped to the last free slot below the neighbour
    CHECK(f.at(1.010) != nullptr);
    CHECK(f.at(2.0) != nullptr);
}

TEST_CASE("A move is free to travel when nothing is in the way") {
    GridFixture f;
    f.seed({{1.0, 20}, {9.0, 80}});

    f.send(MoveActionEvent{.axis = L0, .fromAt = 1.0, .toAt = 5.5, .toPos = 60, .snapshot = true});

    REQUIRE(f.actions().size() == 2);
    REQUIRE(f.at(5.5) != nullptr);
    CHECK(f.at(5.5)->pos == 60);
}
