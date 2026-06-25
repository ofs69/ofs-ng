#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Services/UndoSystem.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"
#include <doctest/doctest.h>

using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::EventCapture;
using ofs::test::TestProject;

namespace {
// A region spanning [0,5] targeting a single axis, with the default Input→Output graph.
ofs::ProcessingRegion makeRegion(int id, StandardAxis axis) {
    ofs::ProcessingRegion r;
    r.id = id;
    r.startTime = 0.0;
    r.endTime = 5.0;
    r.axisRoles.set(static_cast<size_t>(axis));
    r.nodeGraph = ofs::buildDefaultGraph(axis);
    return r;
}
} // namespace

TEST_CASE("UndoSystem: mutate snapshots state and undo reverts it") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    // UndoSystem snapshots before AddActionAtTimeEvent is processed by ProjectManager,
    // but here we drive it manually: push the undoable event then mutate directly.
    // The simpler route: trigger snapshot via AddActionAtTimeEvent (registered in UndoSystem).
    tp.project.axes[0].showInStrip = true;

    // Prime a snapshot by firing an event UndoSystem watches.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain(); // UndoSystem::push() snapshots (empty state), no ProjectManager present

    // Now manually mutate (simulating what ProjectManager would have done).
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].actions.size() == 1);

    // Undo should restore to the snapshot taken before the mutation.
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    CHECK(tp.project.axes[0].actions.empty());
}

TEST_CASE("UndoSystem: redo re-applies after undo") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.axes[0].showInStrip = true;

    // Snapshot before mutation.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();

    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    // Undo.
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.axes[0].actions.empty());

    // Redo — but undo moved the snapshot to redoStack; we need another snapshot for redo.
    // Actually: after undo, state = snapshot (empty). Redo pushes current (empty) to undo
    // and restores from redo. But redo was pushed when we called undo...
    // Let's verify canRedo is true after undo.
    CHECK(undo.canRedo());
    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();

    CHECK(tp.project.axes[0].actions.size() == 1);
}

TEST_CASE("UndoSystem: canUndo is false when stack is empty") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    CHECK_FALSE(undo.canUndo());
    CHECK_FALSE(undo.canRedo());
}

TEST_CASE("UndoSystem: consecutive undos walk the stack") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.axes[0].showInStrip = true;

    // Step 1: snapshot empty state.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    // Step 2: snapshot one-action state.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 2.0, .pos = 60});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({2.0, 60}); }, tp.eq);
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].actions.size() == 2);

    // First undo → back to one action.
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    CHECK(tp.project.axes[0].actions.size() == 1);

    // Second undo → back to empty.
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    CHECK(tp.project.axes[0].actions.empty());
}

TEST_CASE("UndoSystem: LoadProjectEvent clears the undo stack") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.axes[0].showInStrip = true;
    // Capture the pre-edit snapshot, apply the edit (as ProjectManager would), then flush the frame so
    // the document-changed snapshot is committed and there is something for LoadProjectEvent to clear.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();
    undo.endFrame();

    CHECK(undo.canUndo());

    tp.eq.push(ofs::LoadProjectEvent{});
    tp.eq.drain();

    CHECK_FALSE(undo.canUndo());
}

// Deleting a scratch axis is destructive (drops actions + any regions referencing it).
// RemoveAxisEvent is registered in UndoSystem (before ProjectManager), so the snapshot
// captures the axis while still present and undo brings it back.
TEST_CASE("UndoSystem: deleting a scratch axis is undoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    const auto s0 = StandardAxis::S0;
    const auto s0i = static_cast<size_t>(s0);
    tp.project.axes[s0i].showInStrip = true;
    tp.project.mutate(s0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();
    tp.project.regions.push_back(makeRegion(1, s0));

    // Snapshot the present axis, then delete it (as ProjectManager::onRemoveAxis would).
    tp.eq.push(ofs::RemoveAxisEvent{.axisRole = s0});
    tp.eq.drain();
    std::erase_if(tp.project.regions, [s0i](const ofs::ProcessingRegion &r) { return r.axisRoles.test(s0i); });
    tp.project.mutate(
        s0,
        [](ofs::AxisState &a) {
            a.showInStrip = false;
            a.actions = {};
            a.selection = {};
        },
        tp.eq);
    tp.eq.drain();

    REQUIRE_FALSE(tp.project.axes[s0i].showInStrip);
    REQUIRE(tp.project.regions.empty());

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    CHECK(tp.project.axes[s0i].showInStrip);
    CHECK(tp.project.axes[s0i].actions.size() == 1);
    CHECK(tp.project.regions.size() == 1);
}

// Symmetric to deletion: adding a scratch axis snapshots the absent state first, so undo
// removes the freshly-added axis.
TEST_CASE("UndoSystem: adding a scratch axis is undoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    const auto s0 = StandardAxis::S0;
    const auto s0i = static_cast<size_t>(s0);
    REQUIRE_FALSE(tp.project.axes[s0i].showInStrip);

    // Snapshot the absent state, then add the axis (as ProjectManager::onAddScratchAxis would).
    tp.eq.push(ofs::AddScratchAxisEvent{});
    tp.eq.drain();
    tp.project.mutate(s0, [](ofs::AxisState &a) { a.showInStrip = true; }, tp.eq);
    tp.eq.drain();

    REQUIRE(tp.project.axes[s0i].showInStrip);

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    CHECK_FALSE(tp.project.axes[s0i].showInStrip);
}

// ── Event re-firing on undo/redo ──────────────────────────────────────────────
// Plugins and ProcessingSystem learn about axis changes only via AxisModifiedEvent.
// Undo/redo restore axis state directly (restoreSnapshot), so they MUST re-fire the
// event or downstream consumers (plugin onAxisModified, processing re-eval) silently
// go stale. These tests pin that contract.

TEST_CASE("UndoSystem: undo re-fires AxisModifiedEvent for the reverted axis") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq); // must attach before freeze(); eq.on() asserts afterwards
    tp.eq.freeze();

    tp.project.axes[0].showInStrip = true;

    // Snapshot empty state, then add an action.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    cap.received.clear(); // discard the mutate's own event; only watch the undo

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].actions.empty());
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

TEST_CASE("UndoSystem: redo re-fires AxisModifiedEvent for the re-applied axis") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[0].showInStrip = true;

    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.axes[0].actions.empty());

    cap.received.clear(); // watch only the redo

    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();

    REQUIRE(tp.project.axes[0].actions.size() == 1);
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

TEST_CASE("UndoSystem: undo re-fires AxisModifiedEvent only for axes that changed") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    tp.project.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;

    // Snapshot, then mutate only L0. R0 is untouched between snapshot and now.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    cap.received.clear();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    // Exactly one event, for L0 — R0 was identical across the snapshot so it must stay quiet.
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

// restoreSnapshot restores project.state.activeAxis. When that changes, it MUST push
// AxisSelectedEvent so consumers (plugin onAxisSelected, UI) re-sync the active axis after
// an undo/redo. Conversely it must stay silent when the active axis is unchanged.
TEST_CASE("UndoSystem: undo re-fires AxisSelectedEvent when the active axis changes") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisSelectedEvent> selCap;
    selCap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    tp.project.state.activeAxis = StandardAxis::R0;

    // Snapshot captures activeAxis = R0.
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    // User switches the active axis after the snapshot.
    tp.project.state.activeAxis = StandardAxis::L0;
    selCap.received.clear();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    // activeAxis was restored to R0, and an AxisSelectedEvent announced it.
    CHECK(tp.project.state.activeAxis == StandardAxis::R0);
    REQUIRE(selCap.received.size() == 1);
    CHECK(selCap.received[0].role == StandardAxis::R0);
}

TEST_CASE("UndoSystem: undo does NOT re-fire AxisSelectedEvent when the active axis is unchanged") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisSelectedEvent> selCap;
    selCap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    tp.project.state.activeAxis = StandardAxis::L0; // same before and after the snapshot

    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    selCap.received.clear();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    CHECK(selCap.received.empty());
}

// ── Region undo/redo re-fires AxisModifiedEvent ──────────────────────────────
// ProcessingSystem re-evaluates an axis only on AxisModifiedEvent. A region edit
// (create/delete/node-graph change) changes a region's contribution to its axes
// without touching those axes' source actions, so restoreSnapshot must announce the
// region delta as AxisModifiedEvent for the affected axes — otherwise the resolved
// output goes stale after undo/redo. These pin that contract.

TEST_CASE("UndoSystem: undoing a region creation re-fires AxisModifiedEvent for its axes") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;

    // Snapshot the region-free state, then add a region targeting L0 (as ProjectManager would). The
    // tentative pre-create snapshot is committed at undo time once the document is seen to differ.
    tp.eq.push(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 5.0});
    tp.eq.drain();
    tp.project.regions.push_back(makeRegion(1, StandardAxis::L0));

    cap.received.clear();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    REQUIRE(tp.project.regions.empty());
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

TEST_CASE("UndoSystem: redoing a region creation re-fires AxisModifiedEvent for its axes") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;

    tp.eq.push(ofs::CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 5.0});
    tp.eq.drain();
    tp.project.regions.push_back(makeRegion(1, StandardAxis::L0));

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.regions.empty());

    cap.received.clear();

    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();

    REQUIRE(tp.project.regions.size() == 1);
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

TEST_CASE("UndoSystem: undoing a node-graph edit re-fires AxisModifiedEvent for the region's axes") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    tp.project.regions.push_back(makeRegion(1, StandardAxis::L0));

    // Snapshot the region, then edit its node graph (no axis actions change).
    tp.eq.push(ofs::ModifyRegionEvent{.regionId = 1, .updatedRegion = tp.project.regions[0], .snapshot = true});
    tp.eq.drain();
    tp.project.regions[0].nodeGraph.nodes.push_back(
        {.id = 99, .type = ofs::GraphNodeType::Constant, .constantValue = 25.0f, .role = StandardAxis::L0});

    cap.received.clear();

    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    // The graph edit was reverted, and the axis was announced for re-evaluation.
    REQUIRE(tp.project.regions.size() == 1);
    CHECK(tp.project.regions[0].nodeGraph.nodes.size() == 2); // back to the default Input→Output pair
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

// A plugin node's TState (nodeState) rides on the C++ region struct, which UndoSystem already snapshots —
// so a state edit is part of the region diff and undo/redo restore it through the same path as any other
// node-graph change, with no separate state-undo machinery.
TEST_CASE("UndoSystem: undo/redo restores a plugin node's nodeState") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    EventCapture<ofs::AxisModifiedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    auto region = makeRegion(1, StandardAxis::L0);
    region.nodeGraph.nodes[0].nodeState = R"({"Mix":0.0})";
    tp.project.regions.push_back(region);

    // Snapshot at gesture start (carries the pre-edit nodeState), then edit the state.
    tp.eq.push(ofs::ModifyRegionEvent{.regionId = 1, .updatedRegion = tp.project.regions[0], .snapshot = true});
    tp.eq.drain();
    tp.project.regions[0].nodeGraph.nodes[0].nodeState = R"({"Mix":0.9})";

    cap.received.clear();
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.regions.size() == 1);
    CHECK(tp.project.regions[0].nodeGraph.nodes[0].nodeState == R"({"Mix":0.0})"); // restored
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0); // re-eval announced

    cap.received.clear();
    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();
    CHECK(tp.project.regions[0].nodeGraph.nodes[0].nodeState == R"({"Mix":0.9})"); // re-applied
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == StandardAxis::L0);
}

// ── Chapter undo/redo ─────────────────────────────────────────────────────────
// Chapter add/remove/move/resize are undoable: ModifyBookmarkChapterEvent is registered in
// UndoSystem (before ProjectManager), so the tentative snapshot captures the pre-edit chapters and undo
// restores them. These tests have no ProjectManager, so the mutation that the queued event would normally
// apply is applied directly here; the empty mutator only exists to trigger UndoSystem's tentative capture.
namespace {
auto kNoopChapterEdit = [](ofs::BookmarkChapterState &) {};
}

TEST_CASE("UndoSystem: adding a chapter is undoable and redoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.eq.push(ofs::ModifyBookmarkChapterEvent{.apply = kNoopChapterEdit}); // capture pre-add snapshot
    tp.eq.drain();
    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 3.0, .name = "c1"});
    undo.endFrame(); // commit the tentative now that chapters differ

    REQUIRE(undo.canUndo());
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    CHECK(tp.project.bookmarks.chapters.empty());

    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.bookmarks.chapters.size() == 1);
    CHECK(tp.project.bookmarks.chapters[0].name == "c1");
}

TEST_CASE("UndoSystem: deleting a chapter is undoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 3.0, .name = "c1"});

    tp.eq.push(ofs::ModifyBookmarkChapterEvent{.apply = kNoopChapterEdit}); // snapshot with the chapter present
    tp.eq.drain();
    tp.project.bookmarks.chapters.clear();
    undo.endFrame();

    REQUIRE(undo.canUndo());
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.bookmarks.chapters.size() == 1);
    CHECK(tp.project.bookmarks.chapters[0].name == "c1");
}

TEST_CASE("UndoSystem: moving/resizing a chapter is undoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 3.0, .name = "c1"});

    tp.eq.push(ofs::ModifyBookmarkChapterEvent{.apply = kNoopChapterEdit}); // snapshot the pre-drag span
    tp.eq.drain();
    tp.project.bookmarks.chapters[0].startTime = 4.0;
    tp.project.bookmarks.chapters[0].endTime = 8.0;
    undo.endFrame();

    REQUIRE(undo.canUndo());
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.bookmarks.chapters.size() == 1);
    CHECK(tp.project.bookmarks.chapters[0].startTime == doctest::Approx(1.0));
    CHECK(tp.project.bookmarks.chapters[0].endTime == doctest::Approx(3.0));
}

// A chapter's sceneView is captured-on-adjust (overlay/video framing), not through the undo path. Undo of
// an unrelated edit must restore size/position/name/color but leave the live framing intact — it is never
// snapshotted (SnapshotCodec serialize(Chapter)), and restoreSnapshot carries the live value across.
TEST_CASE("UndoSystem: undo preserves a chapter's live scene view") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 3.0, .name = "c1"});

    tp.eq.push(ofs::ModifyBookmarkChapterEvent{.apply = kNoopChapterEdit}); // snapshot before the rename
    tp.eq.drain();
    tp.project.bookmarks.chapters[0].name = "renamed";
    undo.endFrame();

    // The user then adjusts the scene framing (live, outside undo) after the snapshot was taken.
    ofs::SceneView sv;
    sv.framing.zoomFactor = 3.5f;
    tp.project.bookmarks.chapters[0].sceneView = sv;

    REQUIRE(undo.canUndo());
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();

    REQUIRE(tp.project.bookmarks.chapters.size() == 1);
    CHECK(tp.project.bookmarks.chapters[0].name == "c1");            // rename undone
    REQUIRE(tp.project.bookmarks.chapters[0].sceneView.has_value()); // framing preserved, not reverted
    CHECK(tp.project.bookmarks.chapters[0].sceneView->framing.zoomFactor == doctest::Approx(3.5f));
}

// Bookmarks are undoable too (they ride in the snapshot's BookmarkChapterState): adding one captures a
// tentative that flush commits, and undo restores the empty list.
TEST_CASE("UndoSystem: adding a bookmark is undoable") {
    TestProject tp;
    ofs::UndoSystem undo(tp.project, tp.eq);
    tp.eq.freeze();

    tp.eq.push(ofs::ModifyBookmarkChapterEvent{.apply = kNoopChapterEdit}); // capture the pre-add snapshot
    tp.eq.drain();
    tp.project.bookmarks.bookmarks.push_back({.time = 1.0, .name = "b"});
    undo.endFrame();

    REQUIRE(undo.canUndo());
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    CHECK(tp.project.bookmarks.bookmarks.empty());

    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.bookmarks.bookmarks.size() == 1);
    CHECK(tp.project.bookmarks.bookmarks[0].name == "b");
}
