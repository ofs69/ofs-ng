// Broad coverage of ProjectManager's synchronous event handlers. Each test pushes an event and
// asserts the resulting ScriptProject mutation. The coroutine flows that co_await a file dialog
// (open/save pickers, the unsaved-changes prompt) are exercised only along their dialog-free
// branches — there is no ModalManager wired up here to resume those suspended awaits. The one
// exception is the import picker: PMFixture auto-confirms it with the spec defaults (see below) so
// the project-creation flows, which now always raise it, run to completion.

#include "Core/Events.h"
#include "Core/GraphPresetEvents.h"
#include "Core/IntentEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/SceneViewEvents.h"
#include "Core/TranscodeEvents.h"
#include "Format/AppSettings.h"
#include "Format/BackupArchive.h"
#include "Format/Funscript.h"
#include "Services/EditIntentRouter.h"
#include "Services/EditModeRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/NavigatorRegistry.h"
#include "Services/NavigatorRouter.h"
#include "Services/PluginEvents.h"
#include "Services/ProjectManager.h"
#include "Services/ScriptNodeEvents.h"
#include "Services/SelectIntentRouter.h"
#include "Services/SelectionModeRegistry.h"
#include "Services/UndoSystem.h"
#include "UI/Modals.h"
#include "Util/PathUtil.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

using namespace ofs;
using ofs::test::EventCapture;
using ofs::test::TestProject;

namespace {

// Fixture wiring a ProjectManager (and an UndoSystem ahead of it, so undoable mutations snapshot
// correctly) over a fresh TestProject. Auto-backup is off so update() never schedules a write.
struct PMFixture {
    TestProject tp;
    AppSettings appSettings;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    UndoSystem undo; // constructed before pm: its snapshot handlers must register first
    ProjectManager pm;
    NavigatorRegistry navReg;
    NavigatorRouter nav; // sole StepRequestEvent subscriber: resolves a step into a SeekEvent
    EditModeRegistry editReg;
    EditIntentRouter edit; // sole EditRequestEvent subscriber: resolves an edit intent into mutation events
    SelectionModeRegistry selReg;
    SelectIntentRouter sel;        // sole SelectRequestEvent subscriber: resolves a selection gesture per-axis
    EventCapture<SeekEvent> seeks; // observes seeks (frame-step / next-action emit only a SeekEvent)
    EventCapture<AxisModifiedEvent> axisMods; // observes the re-eval signal a region edit raises

    PMFixture()
        : undo(tp.project, tp.eq), pm(tp.project, tp.eq, appSettings, jobSystem, effectReg),
          nav(tp.project, tp.eq, navReg), edit(tp.project, tp.eq, editReg), sel(tp.project, tp.eq, selReg) {
        appSettings.autoBackupEnabled = false;
        seeks.attach(tp.eq); // must register before freeze()
        axisMods.attach(tp.eq);
        // Stand in for the ModalManager on the import picker: there is no UI here to resume the
        // co_await AxisPick the project-creation flows now raise, so auto-confirm it with the spec's
        // defaults (the auto-detected axis per row). This keeps the auto-discovery assertions exact —
        // accepting the defaults is the same placement the old non-interactive flow produced. Other
        // modals (errors, plain confirms) stay unhandled, exactly as before.
        tp.eq.on<ShowModalEvent>([](const ShowModalEvent &e) {
            if (!e.axisPick || !e.resultAxisPick || !e.handle)
                return;
            AxisPickResult r;
            r.outcome = AxisPickResult::Outcome::Confirm;
            for (int i = 0; i < static_cast<int>(e.axisPick->rows.size()); ++i)
                r.rows.push_back(i);
            r.axis = e.axisPick->defaults;
            *e.resultAxisPick = std::move(r);
            e.handle.resume();
        });
        tp.eq.freeze();
        jobSystem.start();
    }

    ScriptProject &proj() { return tp.project; }
    EventQueue &eq() { return tp.eq; }
    template <class E> void push(E e) { tp.eq.push(std::move(e)); }
    // Mirror OfsApp's frame loop: drain, then flush the undo system so a discrete edit applied this
    // drain becomes undoable (and a no-op edit records nothing) — exactly as a real frame would.
    void drain() {
        tp.eq.drain();
        undo.endFrame();
    }

    // Pump drain() until `done`. The open/import/export flows run their file I/O on a JobSystem
    // worker and resume via ResumeFlowEvent, so a single drain() no longer applies the result.
    bool drainUntil(const std::function<bool()> &done, int timeoutMs = 5000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do {
            drain();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    AxisState &axis(StandardAxis ax) { return proj().axes[static_cast<size_t>(ax)]; }

    // Mark an axis present and seed it with `acts`.
    void showAxis(StandardAxis ax, std::initializer_list<ScriptAxisAction> acts = {}) {
        axis(ax).showInStrip = true;
        proj().mutate(
            ax,
            [&](AxisState &a) {
                for (const auto &x : acts)
                    a.actions.insert(x);
            },
            eq());
        drain();
    }
};

} // namespace

// ── Selection ────────────────────────────────────────────────────────────────

TEST_CASE("SelectAll selects every action; no-op on an absent axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {3.0, 30}});

    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).selection.size() == 3);

    // R0 is absent: handler returns early, selection stays empty.
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::R0});
    f.drain();
    CHECK(f.axis(StandardAxis::R0).selection.empty());
}

TEST_CASE("SetAxisSelection with empty clears one axis; a per-axis loop clears all") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(StandardAxis::R0, {{1.0, 10}});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::R0});
    f.drain();
    REQUIRE_FALSE(f.axis(StandardAxis::L0).selection.empty());

    f.push(SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = {}});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).selection.empty());
    CHECK_FALSE(f.axis(StandardAxis::R0).selection.empty());

    // "Clear all" is a per-axis loop now (ClearSelectionEvent was removed).
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (f.proj().axes[i].showInStrip)
            f.push(SetAxisSelectionEvent{.axis = static_cast<StandardAxis>(i), .selection = {}});
    f.drain();
    CHECK(f.axis(StandardAxis::R0).selection.empty());
}

TEST_CASE("SelectTimeRange: replace, additive toggle, and transposed range") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {3.0, 30}, {4.0, 40}});

    // Non-additive: replaces the selection with the actions in [1.5, 3.5].
    f.push(SelectRequestEvent{
        .gesture = SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 1.5, .endTime = 3.5, .additive = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).selection.size() == 2); // 2.0 and 3.0

    // Additive over [2.5, 4.5]: toggles 3.0 off and 4.0 on -> {2.0, 4.0}.
    f.push(SelectRequestEvent{
        .gesture = SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 2.5, .endTime = 4.5, .additive = true});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).selection.size() == 2);

    // Transposed range (start > end) is rejected without changing the selection.
    const auto before = f.axis(StandardAxis::L0).selection.size();
    f.push(SelectRequestEvent{
        .gesture = SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 5.0, .endTime = 1.0, .additive = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).selection.size() == before);
}

// ── Remove / add / move actions ────────────────────────────────────────────────

TEST_CASE("RemoveSelectedActions deletes the selection, else the action nearest the cursor") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {3.0, 30}});

    f.push(SelectRequestEvent{
        .gesture = SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 2.5, .additive = false});
    f.drain();
    f.push(RemoveSelectedActionsEvent{StandardAxis::L0});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.size() == 1); // only 3.0 remains

    // No selection: removes the action closest to the cursor.
    f.proj().playback.cursorPos = 3.0;
    f.push(RemoveSelectedActionsEvent{StandardAxis::L0});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty());
}

TEST_CASE("RemoveActionAtTime erases the action at the given time") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.push(RemoveActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(2.0));
}

TEST_CASE("AddPointAtPlayhead adds at the cursor on the active axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(AxisSelectedEvent{StandardAxis::L0});
    f.proj().playback.cursorPos = 2.5;
    f.drain();
    // The router resolves AddPointAtPlayhead → AddActionAtTimeEvent at the active axis + cursor.
    f.push(EditRequestEvent{.intent = {.kind = EditIntentKind::AddPointAtPlayhead, .pos = 60}});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(2.5));
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 60);
}

TEST_CASE("MoveActionToCurrentTime drags the nearest action onto the cursor") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {5.0, 50}});
    f.proj().playback.cursorPos = 2.0;
    f.push(MoveActionToCurrentTimeEvent{StandardAxis::L0});
    f.drain();
    // The action at 1.0 (nearest to cursor 2.0) moves to 2.0.
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 2);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(2.0));
}

TEST_CASE("MoveSelectionPosition shifts selected pos, else the nearest action's pos") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});

    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();
    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 15});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 25);
    CHECK(f.axis(StandardAxis::L0).actions[1].pos == 35);

    // No selection: nudges the action closest to the cursor, clamped into [0,100].
    f.push(SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = {}});
    f.proj().playback.cursorPos = 1.0;
    f.drain();
    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 1000});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 100); // clamped
}

TEST_CASE("MoveSelectionTime shifts selected actions in time, else the nearest action") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});

    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();
    const double before = f.axis(StandardAxis::L0).actions[0].at;
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .seekAfter = true});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at > before); // both shifted forward one step

    // No selection: moves the action nearest the cursor.
    f.push(SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = {}});
    f.proj().playback.cursorPos = f.axis(StandardAxis::L0).actions[1].at;
    f.drain();
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Backward, .seekAfter = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.size() == 2);
}

TEST_CASE("MoveSelectionTime reps moves N steps in one event") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();

    const double step = 1.0 / 30.0; // default overlay: Frame @ 30fps
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .reps = 3});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0 + 3 * step));
    CHECK(f.axis(StandardAxis::L0).actions[1].at == doctest::Approx(2.0 + 3 * step));
}

TEST_CASE("A held move coalesces into one undo step (time)") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();
    CHECK_FALSE(f.undo.canUndo()); // seeding/selection don't snapshot

    // Hold burst: only the first fire snapshots; the rest ride snapshot=false.
    const double step = 1.0 / 30.0;
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .snapshot = true});
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .snapshot = false});
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .snapshot = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0 + 3 * step));
    CHECK(f.undo.canUndo());

    // One undo restores the pre-hold state, and there is nothing left to undo (single step).
    f.push(UndoEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));
    CHECK(f.axis(StandardAxis::L0).actions[1].at == doctest::Approx(2.0));
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("A held move coalesces into one undo step (position)") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();

    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 5, .snapshot = true});
    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 5, .snapshot = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 20);
    CHECK(f.axis(StandardAxis::L0).actions[1].pos == 30);
    CHECK(f.undo.canUndo());

    f.push(UndoEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 10);
    CHECK(f.axis(StandardAxis::L0).actions[1].pos == 20);
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("A single tap is still one move and one undo step (defaults preserved)") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();

    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .seekAfter = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0 + 1.0 / 30.0));
    CHECK(f.undo.canUndo());

    f.push(UndoEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("MoveSelectionPosition no-ops on an empty axis and on a clamped non-change") {
    PMFixture f;
    f.showAxis(StandardAxis::L0); // present but no actions
    f.proj().playback.cursorPos = 1.0;
    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 10});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty()); // nothing to move

    // An action already at the ceiling can't move further up: the clamp yields the same pos, so the
    // handler records no edit.
    f.showAxis(StandardAxis::L0, {{1.0, 100}});
    f.push(MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 25});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions[0].pos == 100);
}

TEST_CASE("MoveSelectionTime skips a selected point that would collide with an unselected one") {
    PMFixture f;
    f.proj().overlay.overlay = ScriptingOverlay::Frame;
    f.proj().overlay.frameFps = 30.0f; // step = 1/30 s; 30 reps == exactly 1.0 s
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});

    // Select only the action at 1.0; moving it +1.0 s would land on the unselected 2.0 — skipped whole.
    VectorSet<ScriptAxisAction> sel;
    sel.insert({1.0, 10});
    f.push(SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = sel});
    f.drain();
    f.push(MoveSelectionTimeEvent{
        .axis = StandardAxis::L0, .direction = StepDirection::Forward, .seekAfter = false, .reps = 30});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 2);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0)); // unchanged
    CHECK(f.axis(StandardAxis::L0).actions[1].at == doctest::Approx(2.0));
}

TEST_CASE("MoveSelectionTime no-ops on an empty axis and when the nearest action would collide") {
    PMFixture f;
    f.proj().overlay.overlay = ScriptingOverlay::Frame;
    f.proj().overlay.frameFps = 30.0f;

    f.showAxis(StandardAxis::L0); // empty
    f.push(MoveSelectionTimeEvent{.axis = StandardAxis::L0, .direction = StepDirection::Forward, .seekAfter = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty());

    // No selection: the cursor-nearest action moving +1.0 s lands on the existing 2.0 — rejected.
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.proj().playback.cursorPos = 1.0;
    f.push(MoveSelectionTimeEvent{
        .axis = StandardAxis::L0, .direction = StepDirection::Forward, .seekAfter = false, .reps = 30});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 2);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0)); // collision rejected
}

TEST_CASE("MoveActionToCurrentTime no-ops when the cursor already sits on the nearest action") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.proj().playback.cursorPos = 2.0; // the nearest action is already at the cursor
    f.push(MoveActionToCurrentTimeEvent{StandardAxis::L0});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 2);
    CHECK(f.axis(StandardAxis::L0).actions[1].at == doctest::Approx(2.0));
}

TEST_CASE("CopySelection skips grouped axes that have no selection") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(StandardAxis::R0, {{1.0, 30}});
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();

    // Select on L0 only via a direct selection (SelectAll would fan across the group); R0 is in the
    // group but has no selection, so copy captures a single clip — the R0 branch is skipped.
    VectorSet<ScriptAxisAction> l0sel;
    l0sel.insert({1.0, 10});
    l0sel.insert({2.0, 20});
    f.push(SetAxisSelectionEvent{.axis = StandardAxis::L0, .selection = l0sel});
    f.drain();
    f.push(CopySelectionEvent{});
    f.drain();

    // A lone clip broadcasts across the group: R0 receives L0's two-point pattern verbatim (exact
    // paste over its [1,2] span), proving R0 contributed nothing of its own to the clipboard.
    f.push(PasteActionsEvent{.pasteTime = 1.0, .exact = true});
    f.drain();
    const auto &r0 = f.axis(StandardAxis::R0).actions;
    REQUIRE(r0.size() == 2);
    CHECK(r0[0].pos == 10); // L0's pattern, not R0's original 30
    CHECK(r0[1].at == doctest::Approx(2.0));
}

TEST_CASE("StepRequest reps advances N frames in one seek") {
    PMFixture f;
    f.proj().overlay.overlay = ScriptingOverlay::Frame;
    f.proj().overlay.frameFps = 30.0f;
    f.proj().playback.cursorPos = 1.0;

    f.push(StepRequestEvent{.direction = StepDirection::Forward, .reps = 4});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(1.0 + 4.0 / 30.0));
}

TEST_CASE("StepRequest Action granularity steps to the adjacent action on the active axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {5.0, 30}});
    f.push(AxisSelectedEvent{StandardAxis::L0});
    f.proj().playback.cursorPos = 2.0;
    f.drain();

    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Action});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(5.0)); // next action after the cursor

    f.proj().playback.cursorPos = 2.0;
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::Action});
    f.drain();
    CHECK(f.seeks.received.back().time == doctest::Approx(1.0)); // previous action before the cursor
}

TEST_CASE("StepRequest Action granularity honors reps and no-ops past the last action") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {3.0, 30}});
    f.push(AxisSelectedEvent{StandardAxis::L0});
    f.proj().playback.cursorPos = 0.0;
    f.drain();

    f.push(StepRequestEvent{.direction = StepDirection::Forward, .reps = 2, .granularity = StepGranularity::Action});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(2.0)); // skipped the first action

    f.proj().playback.cursorPos = 3.0; // on the last action → no next action, no seek
    const size_t before = f.seeks.received.size();
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Action});
    f.drain();
    CHECK(f.seeks.received.size() == before);
}

TEST_CASE("StepRequest ActionAllAxes granularity steps to the nearest action across all axes") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{4.0, 10}});
    f.showAxis(StandardAxis::R0, {{2.5, 20}});
    f.proj().playback.cursorPos = 0.0;
    f.drain();

    // The nearest next action across both axes is R0's 2.5, independent of which axis is active.
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(2.5));
    // Landing on R0's action activates R0 so the user edits the script they stepped onto.
    CHECK(f.proj().state.activeAxis == StandardAxis::R0);
}

TEST_CASE("StepRequest ActionAllAxes activates the axis it lands on") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {9.0, 10}});
    f.showAxis(StandardAxis::R0, {{5.0, 20}});
    REQUIRE(f.proj().state.activeAxis == StandardAxis::L0);

    // Forward from 3.0: nearest next is R0's 5.0 → R0 becomes active.
    f.proj().playback.cursorPos = 3.0;
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    CHECK(f.seeks.received.back().time == doctest::Approx(5.0));
    CHECK(f.proj().state.activeAxis == StandardAxis::R0);

    // Backward from 8.0: nearest prev is R0's 5.0 again → stays R0.
    f.proj().playback.cursorPos = 8.0;
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    CHECK(f.seeks.received.back().time == doctest::Approx(5.0));
    CHECK(f.proj().state.activeAxis == StandardAxis::R0);

    // Forward from 6.0: nearest next is L0's 9.0 → active switches back to L0.
    f.proj().playback.cursorPos = 6.0;
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    CHECK(f.seeks.received.back().time == doctest::Approx(9.0));
    CHECK(f.proj().state.activeAxis == StandardAxis::L0);
}

TEST_CASE("PasteActions: offset paste and exact paste") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{0.0, 10}, {1.0, 90}});
    // Clipboard is now ProjectManager-owned: select the source and CopySelectionEvent populates it.
    f.push(AxisSelectedEvent{StandardAxis::L0});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();
    f.push(CopySelectionEvent{});
    f.drain();

    // Offset paste: clipboard origin (0.0) lands at pasteTime 5.0 → copies at 5.0 and 6.0.
    f.push(PasteActionsEvent{.pasteTime = 5.0, .exact = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{5.0, 0}));
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{6.0, 0}));

    // Exact paste: clipboard times used verbatim.
    f.push(PasteActionsEvent{.pasteTime = 0.0, .exact = true});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{1.0, 0}));
}

TEST_CASE("CommitAxisActions replaces actions and clamps out-of-range values") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}});

    VectorSet<ScriptAxisAction> incoming;
    incoming.insert({-2.0, 150}); // time and pos both out of range
    incoming.insert({3.0, -10});
    f.push(CommitAxisActionsEvent{.axis = StandardAxis::L0, .actions = incoming});
    f.drain();

    const auto &acts = f.axis(StandardAxis::L0).actions;
    REQUIRE(acts.size() == 2);
    CHECK(acts[0].at == doctest::Approx(0.0)); // -2.0 clamped to 0
    CHECK(acts[0].pos == 100);                 // 150 clamped to 100
    CHECK(acts[1].pos == 0);                   // -10 clamped to 0
}

// ── Simulator state ────────────────────────────────────────────────────────────

TEST_CASE("SimulatorPositionChanged stores the simulator state and marks dirty") {
    PMFixture f;
    f.showAxis(StandardAxis::L0); // hasActiveProject() so setDirty takes effect
    f.proj().clearDirtyFlags();

    SimulatorState s;
    s.p1 = {123.f, 456.f};
    f.push(SimulatorPositionChangedEvent{s});
    f.drain();
    CHECK(f.proj().simulator.p1.x == doctest::Approx(123.f));
    CHECK(f.pm.isDirty());
}

// ── Overlay ────────────────────────────────────────────────────────────────────

TEST_CASE("StepRequest (Frame and Tempo) emits a forward seek") {
    TestProject tp;
    NavigatorRegistry navReg;
    NavigatorRouter nav(tp.project, tp.eq, navReg);
    EventCapture<SeekEvent> seek;
    seek.attach(tp.eq);
    tp.eq.freeze();

    tp.project.playback.cursorPos = 1.0;
    tp.project.overlay.overlay = ScriptingOverlay::Frame;
    tp.project.overlay.frameFps = 30.0f;
    tp.eq.push(StepRequestEvent{.direction = StepDirection::Forward});
    tp.eq.drain();
    REQUIRE(seek.received.size() == 1);
    CHECK(seek.received[0].time > 1.0);

    tp.project.overlay.overlay = ScriptingOverlay::Tempo;
    tp.project.overlay.tempoBpm = 120.0f;
    tp.eq.push(StepRequestEvent{.direction = StepDirection::Forward});
    tp.eq.drain();
    REQUIRE(seek.received.size() == 2);
    CHECK(seek.received[1].time > 1.0);
}

TEST_CASE("NavigatorRouter falls back to follow-overlay for an unknown navigator on load") {
    TestProject tp;
    NavigatorRegistry navReg;
    NavigatorRouter nav(tp.project, tp.eq, navReg);
    tp.eq.freeze();

    // An unknown stored id (plugin uninstalled / foreign file) leaves the effective navigator at
    // follow-overlay; the authored (stored) id is preserved so a re-save keeps it. loadFromProject seeds
    // both fields equal — mirror that here.
    tp.project.storedNavigator = "some.uninstalled.plugin";
    tp.project.activeNavigator = "some.uninstalled.plugin";
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();
    CHECK(tp.project.activeNavigator == kFollowOverlayNavigatorId);
    CHECK(tp.project.storedNavigator == "some.uninstalled.plugin"); // authored id not clobbered

    // A registered stored id is restored as the effective navigator on load.
    tp.project.storedNavigator = kFollowOverlayNavigatorId;
    tp.project.activeNavigator = "stale";
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();
    CHECK(tp.project.activeNavigator == kFollowOverlayNavigatorId);
}

TEST_CASE("EditIntentRouter resolves an intent into its mutation event") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<AddActionAtTimeEvent> adds;
    adds.attach(tp.eq);
    tp.eq.freeze();

    // AddPoint passes axis/time/pos straight through to the AddActionAtTimeEvent mutation.
    tp.eq.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::R0, .time = 3.0, .pos = 42}});
    tp.eq.drain();
    REQUIRE(adds.received.size() == 1);
    CHECK(adds.received[0].axis == StandardAxis::R0);
    CHECK(adds.received[0].time == doctest::Approx(3.0));
    CHECK(adds.received[0].pos == 42);

    // AddPointAtPlayhead decomposes inside resolve: cursor time + active axis, emitted as AddActionAtTimeEvent.
    tp.project.state.activeAxis = StandardAxis::L0;
    tp.project.playback.cursorPos = 5.5;
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::AddPointAtPlayhead, .pos = 10}});
    tp.eq.drain();
    REQUIRE(adds.received.size() == 2);
    CHECK(adds.received[1].axis == StandardAxis::L0);
    CHECK(adds.received[1].time == doctest::Approx(5.5));
    CHECK(adds.received[1].pos == 10);
}

TEST_CASE("EditIntentRouter snapshot latch coalesces a gesture into one undo step") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveActionEvent> moves;
    moves.attach(tp.eq);
    tp.eq.freeze();

    auto move = [&](GesturePhase phase, double toAt) {
        tp.eq.push(EditRequestEvent{
            .intent =
                {.kind = EditIntentKind::MovePoint, .axis = StandardAxis::L0, .time = toAt, .fromTime = 1.0, .pos = 50},
            .gesture = phase});
        tp.eq.drain();
    };

    // A drag: the Begin frame opens the undo step (snapshot=true); every Continue frame folds in (false).
    move(GesturePhase::Begin, 1.1);
    move(GesturePhase::Continue, 1.2);
    move(GesturePhase::Continue, 1.3);
    REQUIRE(moves.received.size() == 3);
    CHECK(moves.received[0].snapshot == true);
    CHECK(moves.received[1].snapshot == false);
    CHECK(moves.received[2].snapshot == false);

    // A following discrete edit (OneShot) re-arms the latch — its own undo step.
    move(GesturePhase::OneShot, 2.0);
    REQUIRE(moves.received.size() == 4);
    CHECK(moves.received[3].snapshot == true);
}

TEST_CASE("EditIntentRouter falls back to native for an unknown edit mode on load") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    tp.eq.freeze();

    // An unknown stored id (plugin uninstalled / foreign file) leaves the effective mode at native; the
    // authored (stored) id is preserved so a re-save keeps it. loadFromProject seeds both fields equal.
    tp.project.storedEditMode = "some.uninstalled.mode";
    tp.project.activeEditMode = "some.uninstalled.mode";
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();
    CHECK(tp.project.activeEditMode == kNativeEditModeId);
    CHECK(tp.project.storedEditMode == "some.uninstalled.mode"); // authored id not clobbered

    // A registered stored id is restored as the effective mode on load.
    tp.project.storedEditMode = kNativeEditModeId;
    tp.project.activeEditMode = "stale";
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();
    CHECK(tp.project.activeEditMode == kNativeEditModeId);
}

// ── Plugin-mode / navigator dispatch (fake C callbacks — no .NET) ─────────────
// The router dispatches to OfsEditIntentFn / OfsNavStepFn exactly as it would a managed trampoline, so
// a plain C function pointer exercises the Pass/Drop/Replace and Seek/None paths without booting CoreCLR
// (the real managed bridge is covered end-to-end by the probe-plugin suite). These pin the router
// mechanics the probe can't isolate: multi-emit coalescing, group fan-out below the seam, Seek vs None.

namespace {
// Drop a RemovePoint, Replace an AddPoint with the same point shifted +1 in position, Pass everything
// else — a minimal mode touching all three dispositions.
int fakeTransformMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    if (in->kind == OfsEditRemovePoint)
        return OfsEditDrop;
    if (in->kind == OfsEditAddPoint) {
        OfsEditIntent out = *in;
        out.pos += 1;
        emit(sink, &out);
        return OfsEditReplace;
    }
    return OfsEditPass;
}

// Map one intent into THREE emitted copies — a multi-emit Replace, to prove the host snapshot latch
// coalesces them into a single undo step however many mutations one Replace produced.
int fakeFanThriceMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    for (int k = 0; k < 3; ++k) {
        OfsEditIntent out = *in;
        out.time = in->time + 0.1 * k; // distinct targets so each is its own mutation
        emit(sink, &out);
    }
    return OfsEditReplace;
}

// ReplacePerAxis: emit one AddPoint whose pos is computed from the (retargeted) axis — L0→80, R0→20,
// else its index. Because the router re-consults the mode once per editable axis with that axis
// substituted, each axis sees its own `in->axis` and yields a distinct pos — a projecting Replace would
// instead fan the lead's pos to every follower. Emits at the request's time, so the per-axis points are
// distinguishable only by pos.
int fakePerAxisMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    if (in->kind != OfsEditAddPoint)
        return OfsEditPass;
    OfsEditIntent out = *in;
    out.pos = (in->axis == static_cast<int>(StandardAxis::L0))   ? 80
              : (in->axis == static_cast<int>(StandardAxis::R0)) ? 20
                                                                 : in->axis;
    emit(sink, &out);
    return OfsEditReplacePerAxis;
}

// ReplacePerAxis on the lead, Pass on every follower. The lead (L0) emits a transformed point (pos 80)
// and returns ReplacePerAxis; a re-consulted follower returns Pass and emits nothing. The router must
// resolve that follower's retargeted intent NATIVELY (its original pos) rather than dropping it — the
// regression this drives: the follower loop used to ignore the disposition and apply only the (empty)
// emit list, silently losing a Pass follower's edit.
int fakePerAxisLeadPassFollower(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    if (in->kind != OfsEditAddPoint)
        return OfsEditPass;
    if (in->axis == static_cast<int>(StandardAxis::L0)) {
        OfsEditIntent out = *in;
        out.pos = 80;
        emit(sink, &out);
        return OfsEditReplacePerAxis;
    }
    return OfsEditPass; // follower defers to native resolution
}

// MovePoint variant of fakePerAxisMode: echoes the move unchanged (ReplacePerAxis) so the router
// re-consults the mode for every editable axis. Used to prove the host drops a follower that has no
// action at the lead's source time BEFORE the mode is consulted for it — a mode that always emits
// can't be why the follower goes untouched.
int fakePerAxisMoveMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    if (in->kind != OfsEditMovePoint)
        return OfsEditPass;
    OfsEditIntent out = *in;
    emit(sink, &out);
    return OfsEditReplacePerAxis;
}

// next → Seek(42); prev → None (swallow the step).
int fakeFixedNav(void *, const OfsNavIntent *step, OfsNavIntent *out) {
    if (step->direction > 0) {
        out->time = 42.0;
        return 1;
    }
    return 0;
}

// Replace every intent with an unchanged copy of itself: the intent round-trips host → toAbi →
// onEditIntent → fromAbi → host, so the kind-specific `flags` packing (MoveSelection, Paste) is
// exercised in both directions and must survive the round trip.
int fakeEchoMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    OfsEditIntent out = *in;
    emit(sink, &out);
    return OfsEditReplace;
}

// A heterogeneous Replace: one gesture → an add of a new point at 2.0 followed by a remove of the old
// point at 1.0 (the shape of a "stamp-new, clean-up-old" mode). The add is emitted FIRST so it takes
// the gesture's one snapshot; the remove follows SECOND. Both kinds must fold into a single undo step —
// the regression this drives: a Remove emitted after a prior mutation used to snapshot unconditionally,
// committing the first mutation as its own step and splitting the batch in two.
int fakeAddThenRemoveMode(void *, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    OfsEditIntent add{};
    add.kind = OfsEditAddPoint;
    add.axis = in->axis;
    add.time = 2.0;
    add.pos = 60;
    emit(sink, &add);
    OfsEditIntent rem{};
    rem.kind = OfsEditRemovePoint;
    rem.axis = in->axis;
    rem.time = 1.0;
    emit(sink, &rem);
    return OfsEditReplace;
}

// Always Pass — defers the step's granularity to the native resolution for every channel.
int fakePassNav(void *, const OfsNavIntent *, OfsNavIntent *) {
    return 2; /* OfsNavResultPass */
}

// Records the intent the router consulted the mode with (via userData), then echoes it back unchanged
// (Replace). The lens for asserting WHAT the router presented — used to prove a single-action
// MoveSelection nudge crosses the seam as a synthesized MovePoint, while a multi/absent nudge does not.
struct CapturedEdit {
    bool seen = false;
    int kind = -1, axis = -1, pos = 0;
    double fromTime = 0, time = 0;
};
int fakeCaptureEchoMode(void *ud, const OfsEditIntent *in, OfsEmitEdit emit, void *sink) {
    if (ud != nullptr) {
        auto *c = static_cast<CapturedEdit *>(ud);
        c->seen = true;
        c->kind = in->kind;
        c->axis = in->axis;
        c->pos = in->pos;
        c->fromTime = in->fromTime;
        c->time = in->time;
    }
    OfsEditIntent out = *in;
    emit(sink, &out);
    return OfsEditReplace;
}
} // namespace

TEST_CASE("EditIntentRouter dispatches Pass / Drop / Replace through an active plugin mode") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<AddActionAtTimeEvent> adds;
    EventCapture<RemoveActionAtTimeEvent> removes;
    EventCapture<MoveActionEvent> moves;
    adds.attach(tp.eq);
    removes.attach(tp.eq);
    moves.attach(tp.eq);
    tp.eq.freeze();

    EditModeEntry mode;
    mode.id = "test.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeTransformMode;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "test.mode"});
    tp.eq.drain();
    REQUIRE(tp.project.activeEditMode == "test.mode");

    // Replace: AddPoint pos 50 → the mode emits pos 51.
    tp.eq.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    tp.eq.drain();
    REQUIRE(adds.received.size() == 1);
    CHECK(adds.received[0].pos == 51);

    // Drop: RemovePoint emits nothing.
    tp.eq.push(
        EditRequestEvent{.intent = {.kind = EditIntentKind::RemovePoint, .axis = StandardAxis::L0, .time = 1.0}});
    tp.eq.drain();
    CHECK(removes.received.empty());

    // Pass: a MovePoint the mode doesn't transform resolves verbatim.
    tp.eq.push(EditRequestEvent{
        .intent = {
            .kind = EditIntentKind::MovePoint, .axis = StandardAxis::L0, .time = 3.0, .fromTime = 1.0, .pos = 70}});
    tp.eq.drain();
    REQUIRE(moves.received.size() == 1);
    CHECK(moves.received[0].toAt == doctest::Approx(3.0));
    CHECK(moves.received[0].toPos == 70);
}

TEST_CASE("EditIntentRouter presents a single selected-action time nudge to the mode as a MovePoint") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveActionEvent> moves;
    moves.attach(tp.eq);
    tp.eq.freeze();

    // Seed L0 with two actions and select exactly one (at 1.0). A single-action selection makes the
    // MoveSelection nudge a single-action nudge, which the router synthesizes into a MovePoint for the mode.
    tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 50});
            a.actions.insert({5.0, 60});
            a.selection.insert({1.0, 50});
        },
        tp.eq);
    tp.eq.drain();

    CapturedEdit cap;
    EditModeEntry mode;
    mode.id = "cap.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeCaptureEchoMode;
    mode.userData = &cap;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "cap.mode"});
    tp.eq.drain();

    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::L0,
                                           .direction = StepDirection::Forward,
                                           .reps = 1}});
    tp.eq.drain();

    // The mode saw a MovePoint synthesized from the lone selected action — not the raw MoveSelection.
    REQUIRE(cap.seen);
    CHECK(cap.kind == OfsEditMovePoint);
    CHECK(cap.fromTime == doctest::Approx(1.0));
    CHECK(cap.pos == 50);
    CHECK(cap.time == doctest::Approx(1.0 + stepTime(tp.project.overlay)));

    // The echoed MovePoint resolves to a MoveActionEvent at the nudged time.
    REQUIRE(moves.received.size() == 1);
    CHECK(moves.received[0].fromAt == doctest::Approx(1.0));
    CHECK(moves.received[0].toAt == doctest::Approx(1.0 + stepTime(tp.project.overlay)));
    CHECK(moves.received[0].toPos == 50);
}

TEST_CASE("EditIntentRouter resolves a no-selection position nudge to the nearest action as a MovePoint") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveActionEvent> moves;
    moves.attach(tp.eq);
    tp.eq.freeze();

    // No selection: a nudge falls back to the action nearest the playhead. Two actions straddle the cursor
    // (1.4) so closestAction must prefer 1.0 over 3.0 — exercising both the lower-bound and the prev branch.
    tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 30});
            a.actions.insert({3.0, 70});
        },
        tp.eq);
    tp.project.playback.cursorPos = 1.4;
    tp.eq.drain();

    CapturedEdit cap;
    EditModeEntry mode;
    mode.id = "cap.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeCaptureEchoMode;
    mode.userData = &cap;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "cap.mode"});
    tp.eq.drain();

    // A position nudge (direction == 0) carries its delta in pos: +15 on the nearest action's pos.
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::L0,
                                           .pos = 15,
                                           .direction = StepDirection::None}});
    tp.eq.drain();

    REQUIRE(cap.seen);
    CHECK(cap.kind == OfsEditMovePoint);
    CHECK(cap.fromTime == doctest::Approx(1.0)); // nearest to 1.4, not 3.0
    CHECK(cap.time == doctest::Approx(1.0));     // a position nudge leaves time unchanged
    CHECK(cap.pos == 45);                        // 30 + 15, clamped within [0,100]

    REQUIRE(moves.received.size() == 1);
    CHECK(moves.received[0].fromAt == doctest::Approx(1.0));
    CHECK(moves.received[0].toAt == doctest::Approx(1.0));
    CHECK(moves.received[0].toPos == 45);
}

TEST_CASE("EditIntentRouter does not synthesize a MovePoint for a MoveSelection with an out-of-range axis") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveSelectionPositionEvent> moves;
    moves.attach(tp.eq);
    tp.eq.freeze();

    CapturedEdit cap;
    EditModeEntry mode;
    mode.id = "cap.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeCaptureEchoMode;
    mode.userData = &cap;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "cap.mode"});
    tp.eq.drain();

    // axis == Count is out of range: isSingleActionNudge returns false (no axis to inspect), so the raw
    // MoveSelection crosses to the mode unchanged rather than being synthesized into a MovePoint.
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::Count,
                                           .pos = 5,
                                           .direction = StepDirection::None}});
    tp.eq.drain();

    REQUIRE(cap.seen);
    CHECK(cap.kind == OfsEditMoveSelection); // not synthesized
    REQUIRE(moves.received.size() == 1);
    CHECK(moves.received[0].delta == 5);
}

TEST_CASE("EditIntentRouter coalesces a multi-emit Replace into one undo step (snapshot latch)") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveActionEvent> moves;
    moves.attach(tp.eq);
    tp.eq.freeze();

    EditModeEntry mode;
    mode.id = "fan.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeFanThriceMode;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "fan.mode"});
    tp.eq.drain();

    // One Begin-phase MovePoint → three emitted MoveActionEvents; only the first opens the undo step, so
    // the whole frame's Replace collapses into a single coalesced gesture.
    tp.eq.push(EditRequestEvent{
        .intent =
            {.kind = EditIntentKind::MovePoint, .axis = StandardAxis::L0, .time = 1.0, .fromTime = 1.0, .pos = 50},
        .gesture = GesturePhase::Begin});
    tp.eq.drain();
    REQUIRE(moves.received.size() == 3);
    CHECK(moves.received[0].snapshot == true);
    CHECK(moves.received[1].snapshot == false);
    CHECK(moves.received[2].snapshot == false);
}

TEST_CASE("EditIntentRouter coalesces a multi-add Replace (Shaped Approach) into one undo step") {
    // Regression: an edit mode that resolves one add gesture into several injected points (Ofs.Core's
    // Shaped Approach) must produce a single undo step, not one per point. The latch stamps snapshot=true
    // on the first emitted add only; UndoSystem captures once and the batch coalesces.
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    CHECK_FALSE(f.undo.canUndo()); // seeding/showing doesn't snapshot

    EditModeEntry mode;
    mode.id = "fan.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeFanThriceMode; // one AddPoint → three adds at distinct times
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "fan.mode"});
    f.drain();

    f.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 3); // all three injected points landed
    CHECK(f.undo.canUndo());

    // One undo removes the whole batch and leaves nothing to undo — proof it was a single coalesced step.
    f.push(UndoEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty());
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("EditIntentRouter coalesces a heterogeneous Replace (add + remove) into one undo step") {
    // Regression: a Replace that mixes kinds — a mode that adds a new point and removes an old one — must
    // yield ONE undo step even when the unconditionally-snapshotting kind is emitted SECOND.
    // RemoveActionAtTimeEvent / PasteActionsEvent used to snapshot unconditionally, so a remove after a
    // prior add committed the add as its own step and split the batch in two (with an exposed intermediate
    // state); they now honor the same per-gesture snapshot latch as Add/Move (router stamps only the first
    // emit true).
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}});
    CHECK_FALSE(f.undo.canUndo()); // seeding doesn't snapshot

    EditModeEntry mode;
    mode.id = "snap.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeAddThenRemoveMode; // one gesture → {AddPoint @2.0, RemovePoint @1.0}
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "snap.mode"});
    f.drain();

    f.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 5.0, .pos = 50}});
    f.drain();
    // The old point at 1.0 is gone, the new one at 2.0 landed — the batch applied in full.
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions.begin()->at == doctest::Approx(2.0));
    REQUIRE(f.undo.undoStepCount() == 1); // the remove + add are ONE step, not two (the regression)

    // A single undo restores the pre-batch state (old point back, new one gone) and exhausts the stack —
    // proof the heterogeneous batch coalesced into one step rather than leaving a dangling intermediate.
    f.push(UndoEvent{});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions.begin()->at == doctest::Approx(1.0));
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("EditIntentRouter fans a plugin Replace across the active edit group") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();

    EditModeEntry mode;
    mode.id = "test.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeTransformMode; // AddPoint pos → pos + 1
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "test.mode"});
    f.drain();

    // The mode Replaces the lead-axis AddPoint (50 → 51); the host then fans the resolved mutation across
    // the group below the seam, so both the lead L0 and the follower R0 receive the transformed point.
    f.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    f.drain();

    const auto &l0 = f.axis(StandardAxis::L0).actions;
    const auto &r0 = f.axis(StandardAxis::R0).actions;
    REQUIRE(l0.size() == 1);
    REQUIRE(r0.size() == 1);
    CHECK(l0[0].pos == 51); // the mode's transform
    CHECK(r0[0].pos == 51); // fanned to the group follower
}

TEST_CASE("EditIntentRouter ReplacePerAxis re-consults the mode per follower axis (no projection)") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();

    EditModeEntry mode;
    mode.id = "peraxis.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakePerAxisMode;
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "peraxis.mode"});
    f.drain();

    // One AddPoint at the active lead (L0). ReplacePerAxis applies the lead's own result (pos 80) to L0,
    // then re-consults the mode for follower R0, which returns 20 — NOT the lead's 80 a projecting Replace
    // would have fanned. Same time on both axes, so each is told apart only by its axis-computed pos.
    f.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    f.drain();

    const auto &l0 = f.axis(StandardAxis::L0).actions;
    const auto &r0 = f.axis(StandardAxis::R0).actions;
    REQUIRE(l0.size() == 1);
    REQUIRE(r0.size() == 1);
    CHECK(l0[0].pos == 80); // lead's own per-axis result
    CHECK(r0[0].pos == 20); // follower re-consulted → its own result, not the projected 80
}

TEST_CASE("EditIntentRouter ReplacePerAxis resolves a Pass follower natively, not as a drop") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();

    EditModeEntry mode;
    mode.id = "peraxis.passfollower";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakePerAxisLeadPassFollower;
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "peraxis.passfollower"});
    f.drain();

    // AddPoint at the active lead (L0). The lead's per-axis result is pos 80; follower R0 returns Pass,
    // so it gets the native fallback — the original request (pos 50) retargeted to R0 — rather than nothing.
    f.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    f.drain();

    const auto &l0 = f.axis(StandardAxis::L0).actions;
    const auto &r0 = f.axis(StandardAxis::R0).actions;
    REQUIRE(l0.size() == 1);
    REQUIRE(r0.size() == 1); // the Pass follower is NOT dropped
    CHECK(l0[0].pos == 80);  // lead's per-axis Replace result
    CHECK(r0[0].pos == 50);  // native fallback: original pos, retargeted to R0
}

TEST_CASE("EditIntentRouter ReplacePerAxis drops a follower with no action at the lead's source time") {
    PMFixture f;
    // L0 holds the action the gesture moves (at 1.0). R0 has its own action at 1.0 (lines up with the
    // lead). R1 has an action only at 5.0 — nothing at the lead's source time 1.0.
    f.showAxis(StandardAxis::L0, {{1.0, 50}});
    f.showAxis(StandardAxis::R0, {{1.0, 40}});
    f.showAxis(StandardAxis::R1, {{5.0, 30}});
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    roles.set(static_cast<size_t>(StandardAxis::R1));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();

    EditModeEntry mode;
    mode.id = "peraxis.move";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakePerAxisMoveMode;
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "peraxis.move"});
    f.drain();

    // Move L0's action from 1.0 to 3.0. The lead and R0 (which has an action at 1.0) move; R1 has no
    // action at the lead's source time, so the host drops it rather than re-consulting the mode — which
    // would erase nothing on R1 then insert a phantom point at 3.0.
    f.push(EditRequestEvent{
        .intent = {
            .kind = EditIntentKind::MovePoint, .axis = StandardAxis::L0, .time = 3.0, .fromTime = 1.0, .pos = 70}});
    f.drain();

    const auto &l0 = f.axis(StandardAxis::L0).actions;
    const auto &r0 = f.axis(StandardAxis::R0).actions;
    const auto &r1 = f.axis(StandardAxis::R1).actions;
    REQUIRE(l0.size() == 1);
    REQUIRE(r0.size() == 1);
    CHECK(l0[0].at == doctest::Approx(3.0)); // lead moved
    CHECK(r0[0].at == doctest::Approx(3.0)); // follower with the point moved
    REQUIRE(r1.size() == 1);                 // dropped: untouched, no phantom point
    CHECK(r1[0].at == doctest::Approx(5.0));
}

TEST_CASE("EditIntentRouter ReplacePerAxis coalesces all per-axis mutations into one undo step") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(SetAxisGroupingEvent{.roles = roles, .lead = StandardAxis::L0});
    f.drain();
    CHECK_FALSE(f.undo.canUndo());

    EditModeEntry mode;
    mode.id = "peraxis.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakePerAxisMode;
    f.push(RegisterEditModeEvent{mode});
    f.push(SetActiveEditModeEvent{.id = "peraxis.mode"});
    f.drain();

    // The snapshot latch is host-stamped below the seam: the lead mutation opens the undo step and the
    // follower's per-axis mutation coalesces into it, so a two-axis ReplacePerAxis is ONE undo step.
    f.push(
        EditRequestEvent{.intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50},
                         .gesture = GesturePhase::Begin});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    REQUIRE(f.axis(StandardAxis::R0).actions.size() == 1);
    CHECK(f.undo.canUndo());

    f.push(UndoEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty());
    CHECK(f.axis(StandardAxis::R0).actions.empty()); // both axes reverted by a single undo
    CHECK_FALSE(f.undo.canUndo());
}

TEST_CASE("NavigatorRouter dispatches Seek / None through an active plugin navigator") {
    TestProject tp;
    NavigatorRegistry navReg;
    NavigatorRouter nav(tp.project, tp.eq, navReg);
    EventCapture<SeekEvent> seeks;
    seeks.attach(tp.eq);
    tp.eq.freeze();

    NavigatorEntry navi;
    navi.id = "test.nav";
    navi.owningPlugin = "test";
    navi.onStep = &fakeFixedNav;
    tp.eq.push(RegisterNavigatorEvent{navi});
    tp.eq.push(SetActiveNavigatorEvent{.id = "test.nav"});
    tp.eq.drain();

    // next → Seek(42): exactly one SeekEvent at the navigator's target.
    tp.eq.push(StepRequestEvent{.direction = StepDirection::Forward, .reps = 1});
    tp.eq.drain();
    REQUIRE(seeks.received.size() == 1);
    CHECK(seeks.received[0].time == doctest::Approx(42.0));

    // prev → None: the step is swallowed, no further SeekEvent.
    tp.eq.push(StepRequestEvent{.direction = StepDirection::Backward, .reps = 1});
    tp.eq.drain();
    CHECK(seeks.received.size() == 1);
}

TEST_CASE("NavigatorRouter Pass defers the step to the native resolution") {
    PMFixture f;
    f.proj().overlay.overlay = ScriptingOverlay::Frame;
    f.proj().overlay.frameFps = 25.0f;
    f.proj().playback.cursorPos = 1.0;

    NavigatorEntry navi;
    navi.id = "pass.nav";
    navi.owningPlugin = "test";
    navi.onStep = &fakePassNav;
    f.push(RegisterNavigatorEvent{navi});
    f.push(SetActiveNavigatorEvent{.id = "pass.nav"});
    f.drain();
    REQUIRE(f.proj().activeNavigator == "pass.nav");

    // The navigator Passes its granularity → the host resolves the frame step natively (1 frame @ 25fps).
    f.push(StepRequestEvent{.direction = StepDirection::Forward});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(1.0 + 1.0 / 25.0));
}

TEST_CASE("NavigatorRouter native action stepping: backward reps, the first-action boundary, empty/none") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}, {3.0, 30}});
    f.proj().state.activeAxis = StandardAxis::L0;

    // Backward with reps: from 3.0, two steps back lands on 1.0.
    f.proj().playback.cursorPos = 3.0;
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .reps = 2, .granularity = StepGranularity::Action});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(1.0));

    // At the first action, a backward step has no previous action → no seek.
    f.proj().playback.cursorPos = 1.0;
    size_t before = f.seeks.received.size();
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::Action});
    f.drain();
    CHECK(f.seeks.received.size() == before);

    // Active axis empty → no adjacent action → no seek.
    f.proj().state.activeAxis = StandardAxis::R0; // present default axis, no actions
    before = f.seeks.received.size();
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Action});
    f.drain();
    CHECK(f.seeks.received.size() == before);

    // No active axis (the >= Count "none" sentinel) → no seek.
    f.proj().state.activeAxis = StandardAxis::Count;
    before = f.seeks.received.size();
    f.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Action});
    f.drain();
    CHECK(f.seeks.received.size() == before);
}

TEST_CASE("NavigatorRouter native ActionAllAxes steps backward across axes") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{4.0, 10}});
    f.showAxis(StandardAxis::R0, {{2.5, 20}});

    // From 5.0, the nearest previous action across both axes is L0's 4.0.
    f.proj().playback.cursorPos = 5.0;
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    REQUIRE(f.seeks.received.size() == 1);
    CHECK(f.seeks.received.back().time == doctest::Approx(4.0));

    // From 0.0, no axis has an earlier action → no seek.
    f.proj().playback.cursorPos = 0.0;
    const size_t before = f.seeks.received.size();
    f.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::ActionAllAxes});
    f.drain();
    CHECK(f.seeks.received.size() == before);
}

TEST_CASE("EditIntentRouter native resolution maps every intent kind to its mutation event") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg); // no plugin mode → native resolve()
    EventCapture<RemoveActionAtTimeEvent> removes;
    EventCapture<RemoveSelectedActionsEvent> removeSel;
    EventCapture<MoveSelectionPositionEvent> movePos;
    EventCapture<MoveSelectionTimeEvent> moveTime;
    EventCapture<PasteActionsEvent> pastes;
    removes.attach(tp.eq);
    removeSel.attach(tp.eq);
    movePos.attach(tp.eq);
    moveTime.attach(tp.eq);
    pastes.attach(tp.eq);
    tp.eq.freeze();
    REQUIRE(tp.project.activeEditMode == kNativeEditModeId);

    tp.eq.push(
        EditRequestEvent{.intent = {.kind = EditIntentKind::RemovePoint, .axis = StandardAxis::R0, .time = 2.0}});
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::RemoveSelected, .axis = StandardAxis::R0}});
    // MoveSelection resolves natively to a position event (direction 0) or a time event (direction != 0).
    tp.eq.push(
        EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection, .axis = StandardAxis::R0, .pos = -5}});
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::R0,
                                           .direction = StepDirection::Backward,
                                           .reps = 3,
                                           .seekAfter = true}});
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::Paste, .time = 4.0, .exact = true}});
    tp.eq.drain();

    REQUIRE(removes.received.size() == 1);
    CHECK(removes.received[0].axis == StandardAxis::R0);
    CHECK(removes.received[0].time == doctest::Approx(2.0));
    CHECK(removes.received[0].fanToGroup == true); // native path projects across the group below the seam

    REQUIRE(removeSel.received.size() == 1);
    CHECK(removeSel.received[0].axis == StandardAxis::R0);

    REQUIRE(movePos.received.size() == 1);
    CHECK(movePos.received[0].delta == -5);

    REQUIRE(moveTime.received.size() == 1);
    CHECK(moveTime.received[0].direction == StepDirection::Backward);
    CHECK(moveTime.received[0].reps == 3);
    CHECK(moveTime.received[0].seekAfter == true);

    REQUIRE(pastes.received.size() == 1);
    CHECK(pastes.received[0].pasteTime == doctest::Approx(4.0));
    CHECK(pastes.received[0].exact == true);
}

TEST_CASE("EditIntentRouter round-trips MoveSelection and Paste flags through a plugin mode") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveSelectionTimeEvent> moveTime;
    EventCapture<PasteActionsEvent> pastes;
    moveTime.attach(tp.eq);
    pastes.attach(tp.eq);
    tp.eq.freeze();

    EditModeEntry mode;
    mode.id = "echo.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeEchoMode; // Replace with an unchanged copy → host marshals out (toAbi) and back (fromAbi)
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "echo.mode"});
    tp.eq.drain();

    // direction(-1) / seekAfter / reps survive the OfsEditIntent round-trip out (toAbi) and back (fromAbi). L0 is
    // empty, so this multi/no-selection nudge stays a MoveSelection (not downgraded to a single MovePoint).
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::L0,
                                           .direction = StepDirection::Backward,
                                           .reps = 5,
                                           .seekAfter = true}});
    tp.eq.drain();
    REQUIRE(moveTime.received.size() == 1);
    CHECK(moveTime.received[0].direction == StepDirection::Backward);
    CHECK(moveTime.received[0].reps == 5);
    CHECK(moveTime.received[0].seekAfter == true);

    // Paste packs `exact` into flags bit0.
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::Paste, .time = 2.0, .exact = true}});
    tp.eq.drain();
    REQUIRE(pastes.received.size() == 1);
    CHECK(pastes.received[0].exact == true);
    CHECK(pastes.received[0].pasteTime == doctest::Approx(2.0));
}

TEST_CASE("EditIntentRouter presents a single-action MoveSelection to the mode as a MovePoint") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<MoveActionEvent> moves;          // a MovePoint resolves here
    EventCapture<MoveSelectionTimeEvent> moveSel; // a MoveSelection resolves here
    moves.attach(tp.eq);
    moveSel.attach(tp.eq);
    tp.eq.freeze();

    EditModeEntry mode;
    mode.id = "echo.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakeEchoMode; // echoes whatever kind it is consulted with back as a Replace
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "echo.mode"});
    tp.eq.drain();

    // One selected action on the active axis → the nudge is a single-action move. The mode is consulted with
    // a MovePoint (the action shifted by the host-applied step), which the echo resolves to a MoveActionEvent
    // — proving the mode saw a MovePoint, not a MoveSelection. No MoveSelectionTimeEvent is emitted.
    tp.project.state.activeAxis = StandardAxis::L0;
    tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 50});
            a.selection.insert({1.0, 50});
        },
        tp.eq);

    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::L0,
                                           .direction = StepDirection::Forward,
                                           .reps = 1}});
    tp.eq.drain();

    REQUIRE(moves.received.size() == 1);
    CHECK(moves.received[0].axis == StandardAxis::L0);
    CHECK(moves.received[0].fromAt == doctest::Approx(1.0));
    CHECK(moves.received[0].toAt == doctest::Approx(1.0 + ofs::stepTime(tp.project.overlay))); // host owns the step
    CHECK(moves.received[0].toPos == 50);
    CHECK(moveSel.received.empty());

    // A second selected action makes the same nudge a multi-action move → the mode is consulted with the
    // MoveSelection itself (echoed back to the native selection-time event), and no new MovePoint is emitted.
    tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({2.0, 60});
            a.selection.insert({2.0, 60});
        },
        tp.eq);

    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                           .axis = StandardAxis::L0,
                                           .direction = StepDirection::Forward,
                                           .reps = 1}});
    tp.eq.drain();

    REQUIRE(moveSel.received.size() == 1);
    CHECK(moveSel.received[0].direction == StepDirection::Forward);
    CHECK(moves.received.size() == 1); // unchanged — the multi-action nudge produced no per-point MovePoint
}

TEST_CASE("EditIntentRouter ReplacePerAxis on a non-lead axis stays single-axis") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<AddActionAtTimeEvent> adds;
    adds.attach(tp.eq);
    tp.eq.freeze();

    EditModeEntry mode;
    mode.id = "peraxis.mode";
    mode.owningPlugin = "test";
    mode.onEditIntent = &fakePerAxisMode;
    tp.eq.push(RegisterEditModeEvent{mode});
    tp.eq.push(SetActiveEditModeEvent{.id = "peraxis.mode"});
    tp.eq.drain();

    // Lead R0 is not the active axis (default L0), so the editable set is just {R0}: the mode is consulted
    // once, no follower re-consult, and the single mutation carries fanToGroup=false.
    tp.eq.push(EditRequestEvent{
        .intent = {.kind = EditIntentKind::AddPoint, .axis = StandardAxis::R0, .time = 1.0, .pos = 0}});
    tp.eq.drain();
    REQUIRE(adds.received.size() == 1);
    CHECK(adds.received[0].axis == StandardAxis::R0);
    CHECK(adds.received[0].pos == 20); // fakePerAxisMode maps R0 → 20
    CHECK(adds.received[0].fanToGroup == false);
}

TEST_CASE("EditIntentRouter AddPointAtPlayhead no-ops when there is no active axis") {
    TestProject tp;
    EditModeRegistry editReg;
    EditIntentRouter edit(tp.project, tp.eq, editReg);
    EventCapture<AddActionAtTimeEvent> adds;
    adds.attach(tp.eq);
    tp.eq.freeze();

    tp.project.state.activeAxis = StandardAxis::Count; // the >= Count "none" sentinel
    tp.eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::AddPointAtPlayhead, .pos = 50}});
    tp.eq.drain();
    CHECK(adds.received.empty()); // no active axis to add to → nothing emitted
}

TEST_CASE("SetActive{EditMode,Navigator,SelectionMode} ignore an unknown id (no dangling selection)") {
    PMFixture f;
    REQUIRE(f.proj().activeEditMode == kNativeEditModeId);
    REQUIRE(f.proj().activeNavigator == kFollowOverlayNavigatorId);
    REQUIRE(f.proj().activeSelectionMode == kNativeSelectionModeId);

    // A stale footer option naming a since-unloaded plugin must not take effect: each router rejects an
    // id its registry doesn't hold, so the active selection can never dangle.
    f.push(SetActiveEditModeEvent{.id = "ghost.mode"});
    f.push(SetActiveNavigatorEvent{.id = "ghost.nav"});
    f.push(SetActiveSelectionModeEvent{.id = "ghost.sel"});
    f.drain();

    CHECK(f.proj().activeEditMode == kNativeEditModeId);
    CHECK(f.proj().activeNavigator == kFollowOverlayNavigatorId);
    CHECK(f.proj().activeSelectionMode == kNativeSelectionModeId);
}

TEST_CASE("OverlaySettingsChanged stores the overlay state and marks dirty") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.proj().clearDirtyFlags();

    OverlayState s;
    s.overlay = ScriptingOverlay::Tempo;
    s.tempoBpm = 90.0f;
    f.push(OverlaySettingsChangedEvent{s});
    f.drain();
    CHECK(f.proj().overlay.overlay == ScriptingOverlay::Tempo);
    CHECK(f.proj().overlay.tempoBpm == doctest::Approx(90.0f));
    CHECK(f.pm.isDirty());
}

// ── Axis management ────────────────────────────────────────────────────────────

TEST_CASE("AddScratchAxis creates the first free scratch axis and selects it") {
    PMFixture f;
    f.push(AddScratchAxisEvent{});
    f.drain();
    CHECK(f.axis(StandardAxis::S0).showInStrip);
    CHECK(f.proj().state.activeAxis == StandardAxis::S0);
}

TEST_CASE("RemoveAxis removes a scratch axis and reselects another present axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}});
    f.push(AddScratchAxisEvent{}); // creates + selects S0
    f.drain();
    REQUIRE(f.axis(StandardAxis::S0).showInStrip);

    f.push(RemoveAxisEvent{StandardAxis::S0});
    f.drain();
    CHECK_FALSE(f.axis(StandardAxis::S0).showInStrip);
    CHECK(f.proj().state.activeAxis == StandardAxis::L0); // reselected the remaining axis

    // A non-scratch (device) axis cannot be removed.
    f.push(RemoveAxisEvent{StandardAxis::L0});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).showInStrip);
}

// A scratch axis that holds actions persists like a standard axis even while hidden, so Add Scratch
// Axis must claim the next free slot rather than reuse (and silently wipe) it.
TEST_CASE("AddScratchAxis skips a hidden scratch axis that still holds data") {
    PMFixture f;
    f.showAxis(StandardAxis::S0, {{1.0, 42}});
    f.axis(StandardAxis::S0).showInStrip = false; // hidden, but its data keeps it in existence
    REQUIRE(f.axis(StandardAxis::S0).exists());

    f.push(AddScratchAxisEvent{});
    f.drain();

    CHECK(f.axis(StandardAxis::S0).actions.size() == 1); // untouched
    CHECK_FALSE(f.axis(StandardAxis::S0).showInStrip);
    CHECK(f.axis(StandardAxis::S1).showInStrip); // claimed the next free slot instead
    CHECK(f.proj().state.activeAxis == StandardAxis::S1);
}

// A scratch axis with actions behaves like a standard axis: Remove is a no-op (it must be emptied first),
// so removal can never clear script data.
TEST_CASE("RemoveAxis refuses a scratch axis that holds actions") {
    PMFixture f;
    f.showAxis(StandardAxis::S0, {{1.0, 42}});
    REQUIRE(f.axis(StandardAxis::S0).showInStrip);

    f.push(RemoveAxisEvent{StandardAxis::S0});
    f.drain();

    CHECK(f.axis(StandardAxis::S0).showInStrip);         // still present
    CHECK(f.axis(StandardAxis::S0).actions.size() == 1); // data intact, not cleared
}

TEST_CASE("ToggleAxisVisibility / Lock / PanelVisibility") {
    PMFixture f;
    f.showAxis(StandardAxis::R0);

    // Display-only flag writes must not raise AxisModifiedEvent — that signal drives a processing
    // re-eval / plugin notify, and a visibility/lock/panel toggle leaves the action data untouched.
    f.axisMods.received.clear();
    const std::uint64_t revBefore = f.proj().editRevision;
    f.push(ToggleAxisVisibilityEvent{.axisRole = StandardAxis::R0, .visible = false});
    f.push(ToggleAxisLockEvent{.axisRole = StandardAxis::R0, .locked = true});
    f.push(ToggleAxisPanelVisibilityEvent{.axisRole = StandardAxis::R0, .inPanel = false});
    f.drain();
    CHECK_FALSE(f.axis(StandardAxis::R0).isVisible);
    CHECK(f.axis(StandardAxis::R0).isLocked);
    CHECK_FALSE(f.axis(StandardAxis::R0).showInStrip);
    CHECK(f.axisMods.received.empty());
    // The change still persists: editRevision advances (so an unchanged-project backup skip won't drop it).
    CHECK(f.proj().editRevision > revBefore);

    // L0's panel visibility is pinned on — the toggle is a no-op.
    f.showAxis(StandardAxis::L0);
    f.push(ToggleAxisPanelVisibilityEvent{.axisRole = StandardAxis::L0, .inPanel = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).showInStrip);
}

// ── Regions ────────────────────────────────────────────────────────────────────

TEST_CASE("CreateRegion adds a region; a too-short range is rejected") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);

    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    REQUIRE(f.proj().regions.size() == 1);
    CHECK(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::L0)));

    // A < 0.5s span never becomes a region.
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 6.0, .endTime = 6.1});
    f.drain();
    CHECK(f.proj().regions.size() == 1);
}

// A region create that the handler rejects (no room / too-short span) must not touch the undo
// history: it records no undo step and, crucially, does not wipe a pending redo.
TEST_CASE("A rejected region create records no undo step and preserves redo") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);

    // Make a real region, then undo it so a redo is pending.
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    REQUIRE(f.proj().regions.size() == 1);
    REQUIRE(f.undo.canUndo());

    f.push(UndoEvent{});
    f.drain();
    REQUIRE(f.proj().regions.empty());
    REQUIRE_FALSE(f.undo.canUndo());
    REQUIRE(f.undo.canRedo());

    // Now attempt a create that is rejected (sub-0.5s span). Nothing changes, so no undo step is
    // recorded and the redo stays available.
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 6.0, .endTime = 6.1});
    f.drain();
    CHECK(f.proj().regions.empty());
    CHECK_FALSE(f.undo.canUndo());
    CHECK(f.undo.canRedo());

    // The preserved redo still re-creates the region.
    f.push(RedoEvent{});
    f.drain();
    CHECK(f.proj().regions.size() == 1);
}

TEST_CASE("DeleteRegion removes the region by id") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    f.push(DeleteRegionEvent{id});
    f.drain();
    CHECK(f.proj().regions.empty());

    f.push(DeleteRegionEvent{999}); // unknown id: no-op
    f.drain();
}

TEST_CASE("ModifyRegion applies a real change but skips an identical update") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // Identical region (gesture-start snapshot): no change.
    ProcessingRegion same = f.proj().regions[0];
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = same, .snapshot = true});
    f.drain();
    CHECK(f.proj().regions[0].endTime == doctest::Approx(5.0));

    // Real change: new name + end time.
    ProcessingRegion changed = f.proj().regions[0];
    changed.endTime = 8.0;
    changed.name = "renamed";
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = changed, .snapshot = false});
    f.drain();
    CHECK(f.proj().regions[0].endTime == doctest::Approx(8.0));
    CHECK(f.proj().regions[0].name == "renamed");

    // Hz must be applied like any other field (regression: it was silently dropped, so the Hz drag
    // appeared to always reset to its prior value).
    REQUIRE(f.proj().regions[0].hz != 45);
    ProcessingRegion hzChange = f.proj().regions[0];
    hzChange.hz = 45;
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = hzChange, .snapshot = false});
    f.drain();
    CHECK(f.proj().regions[0].hz == 45);
}

// A plugin node's TState lives in nodeState on a node inside the region's nodeGraph, so a state-only edit
// is part of the region diff (operator== includes nodeState). It must NOT be swallowed by the identical-
// region guard, and applying it must announce the region's axes for re-evaluation.
TEST_CASE("ModifyRegion applies a nodeState-only edit and re-fires AxisModifiedEvent") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // Give the first node some initial state, then push it as a gesture-start snapshot (no diff → skipped).
    REQUIRE_FALSE(f.proj().regions[0].nodeGraph.nodes.empty());
    const int nodeId = f.proj().regions[0].nodeGraph.nodes[0].id;
    ProcessingRegion seeded = f.proj().regions[0];
    seeded.nodeGraph.nodes[0].nodeState = R"({"Mix":0.0})";
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = seeded, .snapshot = false});
    f.drain();
    REQUIRE(f.proj().regions[0].nodeGraph.findNode(nodeId)->nodeState == R"({"Mix":0.0})");

    f.axisMods.received.clear();

    // An update differing ONLY in nodeState must apply (operator== sees the diff) and re-eval the axis.
    ProcessingRegion edited = f.proj().regions[0];
    edited.nodeGraph.nodes[0].nodeState = R"({"Mix":0.75})";
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = edited, .snapshot = false});
    f.drain();
    CHECK(f.proj().regions[0].nodeGraph.findNode(nodeId)->nodeState == R"({"Mix":0.75})");
    REQUIRE(f.axisMods.received.size() == 1);
    CHECK(f.axisMods.received[0].role == StandardAxis::L0);

    // Re-pushing the identical region (state included) is a no-op — guarded by *region == u.
    f.axisMods.received.clear();
    f.push(ModifyRegionEvent{.regionId = id, .updatedRegion = f.proj().regions[0], .snapshot = false});
    f.drain();
    CHECK(f.axisMods.received.empty());
}

TEST_CASE("MoveRegionNodes copies node positions and marks dirty") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;
    f.proj().clearDirtyFlags();

    ProcessingRegion moved = f.proj().regions[0];
    REQUIRE_FALSE(moved.nodeGraph.nodes.empty());
    const int nodeId = moved.nodeGraph.nodes[0].id;
    moved.nodeGraph.nodes[0].posX = 999.0f;
    f.push(MoveRegionNodesEvent{.regionId = id, .updatedRegion = moved});
    f.drain();

    CHECK(f.proj().regions[0].nodeGraph.findNode(nodeId)->posX == doctest::Approx(999.0f));
    CHECK(f.pm.isDirty());
}

TEST_CASE("AssignAxis adds and removes axes, and may drop to zero (region still exists)") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::R0, .assign = true});
    f.drain();
    CHECK(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::R0)));

    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::R0, .assign = false});
    f.drain();
    CHECK_FALSE(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::R0)));

    // Removing the last remaining axis is allowed: the region keeps its time slot with zero axes.
    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::L0, .assign = false});
    f.drain();
    REQUIRE(f.proj().regions.size() == 1);
    CHECK(f.proj().regions[0].axisRoles.none());
}

TEST_CASE("BakeRegion writes the resolved slice into the axis and deletes the region") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{0.0, 0}, {10.0, 100}});
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 2.0, .endTime = 6.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // Simulate ProcessingSystem having produced resolved output for L0.
    ResolvedActions resolved;
    resolved.actions.insert({3.0, 33});
    resolved.actions.insert({5.0, 55});
    f.axis(StandardAxis::L0).resolved = resolved;

    f.push(BakeRegionEvent{id});
    f.drain();
    CHECK(f.proj().regions.empty());
    // The baked keyframes now live in the axis within the region's [2,6] span.
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{3.0, 0}));
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{5.0, 0}));
}

TEST_CASE("Region handlers ignore an out-of-range axis and unknown region ids") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // CreateRegion with an out-of-range lead axis is rejected outright.
    f.push(CreateRegionEvent{.axisRole = StandardAxis::Count, .startTime = 6.0, .endTime = 9.0});
    f.drain();
    CHECK(f.proj().regions.size() == 1);

    // ModifyRegion / MoveRegionNodes / AssignAxis / Bake on an unknown id are no-ops.
    ProcessingRegion ghost = f.proj().regions[0];
    ghost.endTime = 99.0;
    f.push(ModifyRegionEvent{.regionId = id + 100, .updatedRegion = ghost, .snapshot = false});
    f.push(MoveRegionNodesEvent{.regionId = id + 100, .updatedRegion = ghost});
    f.push(AssignAxisToRegionEvent{.regionId = id + 100, .axis = StandardAxis::R0, .assign = true});
    f.push(BakeRegionEvent{id + 100});
    f.drain();
    REQUIRE(f.proj().regions.size() == 1);
    CHECK(f.proj().regions[0].endTime == doctest::Approx(5.0)); // unchanged
}

TEST_CASE("CreateRegion drops a grouped axis bit that is hidden from the strip") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    // R0 exists but is not shown in the strip; a stale grouping bit must not seed the region onto it.
    AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::R0));
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .axisRoles = roles, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    REQUIRE(f.proj().regions.size() == 1);
    CHECK(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK_FALSE(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::R0))); // hidden bit dropped
}

TEST_CASE("AssignAxis no-ops when assigning an already-assigned or removing an unassigned axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;
    const size_t nodeCount = f.proj().regions[0].nodeGraph.nodes.size();

    // L0 is already assigned: re-assigning adds no I/O nodes.
    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::L0, .assign = true});
    f.drain();
    CHECK(f.proj().regions[0].nodeGraph.nodes.size() == nodeCount);

    // R0 was never assigned: removing it changes nothing.
    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::R0, .assign = false});
    f.drain();
    CHECK(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK(f.proj().regions[0].nodeGraph.nodes.size() == nodeCount);
}

TEST_CASE("BakeRegion with no Output node falls back to the region's first resolved member axis") {
    PMFixture f;
    f.showAxis(StandardAxis::R0, {{0.0, 0}, {10.0, 100}});
    f.push(CreateRegionEvent{.axisRole = StandardAxis::R0, .startTime = 2.0, .endTime = 6.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // Strip the graph of its Output node so the bake takes the axisRoles fallback path instead.
    auto &nodes = f.proj().regions[0].nodeGraph.nodes;
    std::erase_if(nodes, [](const ProcessingGraphNode &n) { return n.type == GraphNodeType::Output; });

    ResolvedActions resolved;
    resolved.actions.insert({3.0, 33});
    resolved.actions.insert({5.0, 55});
    f.axis(StandardAxis::R0).resolved = resolved;

    f.push(BakeRegionEvent{id});
    f.drain();
    CHECK(f.proj().regions.empty());
    CHECK(f.axis(StandardAxis::R0).actions.contains(ScriptAxisAction{3.0, 0}));
    CHECK(f.axis(StandardAxis::R0).actions.contains(ScriptAxisAction{5.0, 0}));
}

TEST_CASE("SelectRegion / ClearRegionSelection set the selected region id") {
    PMFixture f;
    f.push(SelectRegionEvent{42});
    f.drain();
    CHECK(f.proj().procSelRegionId == 42);
    f.push(ClearRegionSelectionEvent{});
    f.drain();
    CHECK(f.proj().procSelRegionId == -1);
}

// ── Video / timeline ───────────────────────────────────────────────────────────

TEST_CASE("VideoModeChanged and VideoResolutionChanged update the video player state") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.proj().clearDirtyFlags();

    f.push(VideoModeChangedEvent{VideoMode::Full});
    f.push(VideoResolutionChangedEvent{0.75f});
    f.drain();
    CHECK(f.proj().videoPlayer.activeMode == VideoMode::Full);
    CHECK(f.proj().videoPlayer.resolutionScale == doctest::Approx(0.75f));
    CHECK(f.pm.isDirty());
}

TEST_CASE("UpdateTimelineView and SetTimelineShowPoints update timeline view state") {
    PMFixture f;
    f.push(UpdateTimelineViewEvent{.visibleTime = 12.0, .offsetTime = 3.0});
    f.push(SetTimelineShowPointsEvent{.show = false});
    f.drain();
    CHECK(f.proj().timelineView.visibleTime == doctest::Approx(12.0));
    CHECK(f.proj().timelineView.offsetTime == doctest::Approx(3.0));
    CHECK_FALSE(f.proj().timelineView.showPoints);
}

// ── Scene capture ──────────────────────────────────────────────────────────────

TEST_CASE("Capture events write the project-level scene view when outside any chapter") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.proj().playback.cursorPos = 1.0; // no chapters -> defaultSceneView

    OverlayAnchor anchor;
    anchor.widthNorm = 0.42f;
    f.push(CaptureOverlayAnchorEvent{anchor});
    VideoFraming framing;
    framing.zoomFactor = 2.0f;
    f.push(CaptureVideoFramingEvent{framing});
    f.push(CaptureSimInvertedEvent{true});
    f.drain();

    CHECK(f.proj().defaultSceneView.anchor.widthNorm == doctest::Approx(0.42f));
    CHECK(f.proj().defaultSceneView.framing.zoomFactor == doctest::Approx(2.0f));
    CHECK(f.proj().defaultSceneView.inverted);
}

TEST_CASE("Capture events write a chapter's scene view when the cursor is inside it") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    Chapter ch;
    ch.startTime = 0.0;
    ch.endTime = 10.0;
    ch.name = "scene";
    f.proj().bookmarks.chapters.push_back(ch);
    f.proj().playback.cursorPos = 5.0;

    VideoFraming framing;
    framing.zoomFactor = 3.0f;
    f.push(CaptureVideoFramingEvent{framing});
    f.drain();

    REQUIRE(f.proj().bookmarks.chapters[0].sceneView.has_value());
    CHECK(f.proj().bookmarks.chapters[0].sceneView->framing.zoomFactor == doctest::Approx(3.0f));
}

TEST_CASE("update() resolves the active scene view and emits RestoreSceneViewEvent") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<RestoreSceneViewEvent> restore;
    restore.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    Chapter ch;
    ch.startTime = 0.0;
    ch.endTime = 10.0;
    SceneView sv;
    sv.framing.zoomFactor = 4.0f;
    ch.sceneView = sv;
    tp.project.bookmarks.chapters.push_back(ch);
    tp.project.playback.cursorPos = 5.0;

    pm.update(0.016f);
    tp.eq.drain();

    REQUIRE_FALSE(restore.received.empty());
    CHECK(tp.project.activeSceneView.framing.zoomFactor == doctest::Approx(4.0f));
}

// ── Media / dummy duration ─────────────────────────────────────────────────────

TEST_CASE("ChangeDummyDuration sets the dummy duration on an active project") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(ChangeDummyDurationEvent{42.0});
    f.drain();
    CHECK(f.proj().state.dummyDuration == doctest::Approx(42.0));
}

TEST_CASE("ChangeDummyDuration rejects a non-positive span") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(ChangeDummyDurationEvent{120.0});
    f.drain();
    REQUIRE(f.proj().state.dummyDuration == doctest::Approx(120.0));

    // A zero/negative span has no editable timeline; the handler must leave the previous value intact.
    f.push(ChangeDummyDurationEvent{0.0});
    f.push(ChangeDummyDurationEvent{-5.0});
    f.drain();
    CHECK(f.proj().state.dummyDuration == doctest::Approx(120.0));
}

TEST_CASE("ChangeMediaPath sets/clears the media path and emits load/close video") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<LoadVideoEvent> load;
    EventCapture<CloseVideoEvent> close;
    EventCapture<ChangeDummyDurationEvent> dur;
    load.attach(tp.eq);
    close.attach(tp.eq);
    dur.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;  // active project
    tp.project.state.dummyDuration = 900.0; // user chose a 15-minute no-media canvas

    tp.eq.push(ChangeMediaPathEvent{"C:/videos/clip.mp4"});
    tp.eq.drain();
    CHECK(tp.project.state.mediaPath == "C:/videos/clip.mp4");
    CHECK(load.received.size() == 1);
    // Loading a video must NOT discard the chosen no-media length (this was the bug).
    CHECK(tp.project.state.dummyDuration == doctest::Approx(900.0));

    tp.eq.push(ChangeMediaPathEvent{""}); // empty = unload
    tp.eq.drain();
    CHECK(tp.project.state.mediaPath.empty());
    CHECK(close.received.size() == 1);
    // Unloading reuses the preserved length (not a 5-minute default) and restarts the dummy player.
    CHECK(tp.project.state.dummyDuration == doctest::Approx(900.0));
    REQUIRE(dur.received.size() == 1);
    CHECK(dur.received[0].durationSeconds == doctest::Approx(900.0));
}

TEST_CASE("DeclineOptimize sets a sticky per-project flag that a new original clears") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true; // active project

    tp.eq.push(ChangeMediaPathEvent{"C:/videos/a.mp4"});
    tp.eq.drain();
    tp.project.state.settingsDirty = false; // isolate the decline's own effect below

    tp.eq.push(DeclineOptimizeEvent{});
    tp.eq.drain();
    CHECK(tp.project.state.intraOptimizeDeclined);
    CHECK(tp.project.state.settingsDirty); // the decline must persist with the project

    // Re-selecting the same original keeps the decline — a video already dismissed isn't re-offered.
    tp.eq.push(ChangeMediaPathEvent{"C:/videos/a.mp4"});
    tp.eq.drain();
    CHECK(tp.project.state.intraOptimizeDeclined);

    // A genuinely different original resets it, so the new video is offered afresh.
    tp.eq.push(ChangeMediaPathEvent{"C:/videos/b.mp4"});
    tp.eq.drain();
    CHECK_FALSE(tp.project.state.intraOptimizeDeclined);
}

TEST_CASE("Unloading a video with no prior dummy span falls back to a non-zero length") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<ChangeDummyDurationEvent> dur;
    dur.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true; // active project, dummyDuration left at its 0 default (legacy)
    tp.eq.push(ChangeMediaPathEvent{""});
    tp.eq.drain();

    CHECK(tp.project.state.dummyDuration >= kDefaultDummyDuration); // never a zero-length timeline
    REQUIRE(dur.received.size() == 1);
    CHECK(dur.received[0].durationSeconds == doctest::Approx(tp.project.state.dummyDuration));
}

// ── Project lifecycle ──────────────────────────────────────────────────────────

TEST_CASE("getProjectTitle reflects no-project / untitled / named states") {
    PMFixture f;
    CHECK(std::string(f.pm.getProjectTitle()) == "No Project");

    f.showAxis(StandardAxis::L0);
    CHECK(std::string(f.pm.getProjectTitle()) == "Untitled Project");

    f.proj().state.filePath = "C:/projects/My Script.ofp";
    CHECK(std::string(f.pm.getProjectTitle()) == "My Script.ofp");
    CHECK(f.pm.getCurrentProjectPath() == std::filesystem::path("C:/projects/My Script.ofp"));
}

TEST_CASE("CloseProjectRequest on a clean project clears it and emits LoadProject/CloseVideo") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<LoadProjectEvent> load;
    EventCapture<CloseVideoEvent> close;
    load.attach(tp.eq);
    close.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();
    tp.project.clearDirtyFlags(); // clean -> guardUnsaved proceeds without a dialog

    tp.eq.push(CloseProjectRequestEvent{});
    tp.eq.drain();
    CHECK_FALSE(tp.project.axes[0].showInStrip);
    CHECK_FALSE(load.received.empty());
    CHECK_FALSE(close.received.empty());
}

TEST_CASE("Closing a project resets document settings so none bleed into the next") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    // Dirty up a spread of per-project document settings (the VR/video-mode case the user hit, plus
    // overlay, metadata, view and dummy span) — none of these may survive into the next project.
    tp.project.videoPlayer.activeMode = VideoMode::VrMode;
    tp.project.videoPlayer.resolutionScale = 0.5f;
    tp.project.overlay.overlay = ScriptingOverlay::Tempo;
    tp.project.overlay.tempoBpm = 99.0f;
    tp.project.metadata.title = "bleed";
    tp.project.timelineView.visibleTime = 123.0;
    tp.project.state.dummyDuration = 777.0;
    // App-level simulator settings (3D ranges, toggles) live in AppSettings and MUST survive a close.
    tp.project.simulator.use3dSimulator = true;
    tp.project.simulator.swayRange = 5.0f;
    tp.project.clearDirtyFlags(); // clean -> guardUnsaved proceeds straight to doClose

    tp.eq.push(CloseProjectRequestEvent{});
    tp.eq.drain();

    CHECK(tp.project.videoPlayer.activeMode == VideoMode::Full);
    CHECK(tp.project.videoPlayer.resolutionScale == doctest::Approx(1.0f));
    CHECK(tp.project.overlay.overlay == ScriptingOverlay::Frame);
    CHECK(tp.project.overlay.tempoBpm == doctest::Approx(120.0f));
    CHECK(tp.project.metadata.title.empty());
    CHECK(tp.project.timelineView.visibleTime == doctest::Approx(10.0));
    CHECK(tp.project.state.dummyDuration == doctest::Approx(0.0));
    // App-level simulator state preserved across the close.
    CHECK(tp.project.simulator.use3dSimulator);
    CHECK(tp.project.simulator.swayRange == doctest::Approx(5.0f));
}

TEST_CASE("Saving and opening a project promote it onto the recent list") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<RememberRecentProjectEvent> remember;
    remember.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    auto path = std::filesystem::temp_directory_path() / "ofs_recent_proj.ofp";
    std::filesystem::remove(path);
    tp.project.state.filePath = path.string();

    // A user save (filePath set, not Save-As) writes on a worker; update() finalizes and, on success,
    // pushes RememberRecentProjectEvent — the live signal the welcome screen's recent list listens for.
    tp.eq.push(SaveProjectEvent{false});
    tp.eq.drain();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (remember.received.empty() && std::chrono::steady_clock::now() < deadline) {
        pm.update(0.0f);
        tp.eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(remember.received.empty());
    CHECK(remember.received[0].path == ofs::util::toUtf8(path));

    // Re-opening that saved file promotes it again (the primary recent-list trigger).
    remember.received.clear();
    tp.project.clearDirtyFlags();
    tp.eq.push(OpenProjectRequestEvent{ofs::util::toUtf8(path)});
    auto openDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (remember.received.empty() && std::chrono::steady_clock::now() < openDeadline) {
        tp.eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(remember.received.empty());
    CHECK(remember.received[0].path == ofs::util::toUtf8(path));

    std::filesystem::remove(path);
}

TEST_CASE("An explicit close emits ProjectClosedEvent to suppress the next launch's reopen") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<ProjectClosedEvent> closed;
    EventCapture<RememberRecentProjectEvent> remember;
    closed.attach(tp.eq);
    remember.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();
    tp.project.clearDirtyFlags(); // clean -> guardUnsaved proceeds straight to doClose

    // The explicit user close fires ProjectClosedEvent (OfsApp clears reopenLastProject on it).
    tp.eq.push(CloseProjectRequestEvent{});
    tp.eq.drain();
    CHECK(closed.received.size() == 1);

    // Re-opening a project re-arms the reopen: RememberRecentProjectEvent fires and no further close.
    auto path = std::filesystem::temp_directory_path() / "ofs_reopen_proj.ofp";
    std::filesystem::remove(path);
    tp.project.state.filePath = path.string();
    tp.eq.push(SaveProjectEvent{false});
    tp.eq.drain();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (remember.received.empty() && std::chrono::steady_clock::now() < deadline) {
        pm.update(0.0f);
        tp.eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(remember.received.empty());
    CHECK(closed.received.size() == 1); // a save/open does not push a close
    std::filesystem::remove(path);
}

TEST_CASE("RequestExit on a clean project confirms the exit") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<ExitConfirmedEvent> exit;
    exit.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    // No active project => not dirty => guardUnsaved proceeds straight to ExitConfirmed.
    tp.eq.push(RequestExitEvent{});
    tp.eq.drain();
    CHECK(exit.received.size() == 1);
}

TEST_CASE("OpenProjectRequest with an unreadable file fails the load and clears the project") {
    // Standalone wiring (not PMFixture) so a ShowModalEvent capture can attach before freeze() — the
    // load now decodes on a worker, so the failure (an error modal) arrives only after it resumes.
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    EventCapture<ShowModalEvent> modals;
    modals.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();
    tp.project.clearDirtyFlags();

    auto garbage = std::filesystem::temp_directory_path() / "ofs_corrupt_project.ofp";
    std::ofstream(garbage, std::ios::binary) << "this is not a valid ofp file";

    tp.eq.push(OpenProjectRequestEvent{garbage.string()});
    // Pump until the failed load resumes and reports — guards against a late, erroneous restore.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (modals.received.empty() && std::chrono::steady_clock::now() < deadline) {
        tp.eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(modals.received.empty()); // an error modal was raised
    // doClose ran (project cleared) and loadProjectInternal failed, so no axis was restored.
    CHECK_FALSE(tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip);

    std::filesystem::remove(garbage);
}

// ── Export (dialog-free target-path branches) ───────────────────────────────────

TEST_CASE("Export format 1 and 2 with a target path write multi-axis files dialog-free") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 50}});
    f.showAxis(StandardAxis::R0, {{1.0, 25}});

    auto out11 = std::filesystem::temp_directory_path() / "ofs_export11.funscript";
    std::filesystem::remove(out11);
    f.push(ExportFunscriptRequestEvent{
        .axes = {StandardAxis::L0, StandardAxis::R0}, .format = 1, .targetPath = out11.string()});
    // lastExport is recorded after the write worker resumes, so it also signals the file is closed.
    REQUIRE(f.drainUntil([&] { return f.proj().state.lastExport && f.proj().state.lastExport->format == 1; }));
    CHECK(std::filesystem::exists(out11));

    auto out20 = std::filesystem::temp_directory_path() / "ofs_export20.funscript";
    std::filesystem::remove(out20);
    f.push(ExportFunscriptRequestEvent{
        .axes = {StandardAxis::L0, StandardAxis::R0}, .format = 2, .targetPath = out20.string()});
    REQUIRE(f.drainUntil([&] { return f.proj().state.lastExport && f.proj().state.lastExport->format == 2; }));
    CHECK(std::filesystem::exists(out20));

    std::filesystem::remove(out11);
    std::filesystem::remove(out20);
}

// ── Graph load / trust ──────────────────────────────────────────────────────────

TEST_CASE("ApplyGraphRemap retargets a pending graph onto a present axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // A pending graph saved against R0; remap it onto the region's L0.
    PendingGraphLoad pending;
    pending.regionId = id;
    pending.graph = buildDefaultGraph(StandardAxis::R0);
    pending.savedAxes = {StandardAxis::R0};
    pending.name = "remapped";
    pending.hz = 60;
    f.proj().pendingGraphLoad = std::move(pending);

    f.push(ApplyGraphRemapEvent{.regionId = id, .mapping = {{.from = StandardAxis::R0, .to = StandardAxis::L0}}});
    f.drain();

    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
    CHECK(f.proj().regions[0].axisRoles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK(f.proj().regions[0].name == "remapped");
    CHECK(f.proj().regions[0].hz == 60);
}

TEST_CASE("ApplyGraphRemap drops a pending load whose region was deleted while the dialog was open") {
    PMFixture f;
    // The pending load points at a region id that no longer exists (closed/deleted mid-dialog).
    PendingGraphLoad pending;
    pending.regionId = 555;
    pending.graph = buildDefaultGraph(StandardAxis::R0);
    pending.savedAxes = {StandardAxis::R0};
    f.proj().pendingGraphLoad = std::move(pending);

    f.push(ApplyGraphRemapEvent{.regionId = 555, .mapping = {{.from = StandardAxis::R0, .to = StandardAxis::L0}}});
    f.drain();

    CHECK_FALSE(f.proj().pendingGraphLoad.has_value()); // resolved rather than left dangling
}

TEST_CASE("ApplyGraphRemap for a region that doesn't match the pending load is a no-op") {
    PMFixture f;
    PendingGraphLoad pending;
    pending.regionId = 7;
    pending.graph = buildDefaultGraph(StandardAxis::R0);
    pending.savedAxes = {StandardAxis::R0};
    f.proj().pendingGraphLoad = std::move(pending);

    // The event targets a different region than the pending load — the handler bails and leaves it.
    f.push(ApplyGraphRemapEvent{.regionId = 8, .mapping = {{.from = StandardAxis::R0, .to = StandardAxis::L0}}});
    f.drain();
    REQUIRE(f.proj().pendingGraphLoad.has_value());
    CHECK(f.proj().pendingGraphLoad->regionId == 7);
}

TEST_CASE("ApplyGraphRemap skips non-IO nodes while retargeting Input/Output roles") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // A graph with a Constant node mixed in: only the Input/Output roles are rewritten; the Constant
    // is passed over.
    ProcessingNodeGraph g;
    const int in = g.allocId(), c = g.allocId(), out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    g.nodes.push_back({.id = c, .type = GraphNodeType::Constant, .constantValue = 50.f, .role = StandardAxis::R0});
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = StandardAxis::R0});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = out, .toPin = 0});

    PendingGraphLoad pending;
    pending.regionId = id;
    pending.graph = g;
    pending.savedAxes = {StandardAxis::R0};
    pending.name = "mixed";
    f.proj().pendingGraphLoad = std::move(pending);

    f.push(ApplyGraphRemapEvent{.regionId = id, .mapping = {{.from = StandardAxis::R0, .to = StandardAxis::L0}}});
    f.drain();

    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
    const auto &nodes = f.proj().regions[0].nodeGraph.nodes;
    for (const auto &n : nodes) {
        if (n.type == GraphNodeType::Input || n.type == GraphNodeType::Output)
            CHECK(n.role == StandardAxis::L0); // remapped
        else
            CHECK(n.role == StandardAxis::R0); // Constant untouched
    }
}

TEST_CASE("RemapCurrentGraph stashes a pending load mirroring the region's live graph") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;
    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::R0, .assign = true});
    f.proj().regions[0].name = "live";
    f.proj().regions[0].hz = 45;
    f.drain();

    const ProcessingNodeGraph before = f.proj().regions[0].nodeGraph;

    f.push(RemapCurrentGraphEvent{id});
    f.drain();

    REQUIRE(f.proj().pendingGraphLoad.has_value());
    const auto &pgl = *f.proj().pendingGraphLoad;
    CHECK(pgl.regionId == id);
    CHECK_FALSE(pgl.needsTrust); // the live graph already ran — no trust gate
    CHECK(pgl.name == "live");
    CHECK(pgl.hz == 45);
    // savedAxes are the distinct Input/Output roles in encounter order (L0 created first, R0 appended).
    CHECK(pgl.savedAxes == std::vector<StandardAxis>{StandardAxis::L0, StandardAxis::R0});
    CHECK(pgl.graph == before);                     // a copy of the live graph
    CHECK(f.proj().regions[0].nodeGraph == before); // original untouched until the user confirms
}

TEST_CASE("RemapCurrentGraph then ApplyGraphRemap retargets the live graph onto a new axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    f.push(RemapCurrentGraphEvent{id});
    f.drain();
    REQUIRE(f.proj().pendingGraphLoad.has_value());

    f.push(ApplyGraphRemapEvent{.regionId = id, .mapping = {{.from = StandardAxis::L0, .to = StandardAxis::R0}}});
    f.drain();

    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
    const auto &roles = f.proj().regions[0].axisRoles;
    CHECK_FALSE(roles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK(roles.test(static_cast<size_t>(StandardAxis::R0)));
    for (const auto &n : f.proj().regions[0].nodeGraph.nodes)
        if (n.type == GraphNodeType::Input || n.type == GraphNodeType::Output)
            CHECK(n.role == StandardAxis::R0);
}

TEST_CASE("RemapCurrentGraph no-ops on an unknown region") {
    PMFixture f;
    f.push(RemapCurrentGraphEvent{999});
    f.drain();
    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("RemapCurrentGraph no-ops on a region whose graph has no Input/Output axes") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;
    // Strip the region down to a graph with no Input/Output nodes — nothing to remap.
    f.proj().regions[0].nodeGraph = ProcessingNodeGraph{};

    f.push(RemapCurrentGraphEvent{id});
    f.drain();
    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("ConfirmGraphTrust is a no-op for a mismatched region or an already-trusted load") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    PendingGraphLoad pending;
    pending.regionId = id;
    pending.graph = buildDefaultGraph(StandardAxis::L0);
    pending.savedAxes = {StandardAxis::L0};
    pending.needsTrust = false; // already trusted: confirm must not re-apply
    f.proj().pendingGraphLoad = pending;

    // Wrong region — guard returns, pending stays.
    f.push(ConfirmGraphTrustEvent{id + 1});
    f.drain();
    REQUIRE(f.proj().pendingGraphLoad.has_value());

    // Right region but needsTrust already false — second guard returns, pending still untouched.
    f.push(ConfirmGraphTrustEvent{id});
    f.drain();
    REQUIRE(f.proj().pendingGraphLoad.has_value());
    CHECK_FALSE(f.proj().pendingGraphLoad->needsTrust);
}

TEST_CASE("ConfirmGraphTrust drops a pending load whose region was deleted while the dialog was open") {
    PMFixture f;
    PendingGraphLoad pending;
    pending.regionId = 556;
    pending.graph = buildDefaultGraph(StandardAxis::L0);
    pending.savedAxes = {StandardAxis::L0};
    pending.needsTrust = true; // gets past the needsTrust guard to the region lookup
    f.proj().pendingGraphLoad = std::move(pending);

    f.push(ConfirmGraphTrustEvent{556});
    f.drain();

    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("CancelGraphLoad discards the pending graph load") {
    PMFixture f;
    PendingGraphLoad pending;
    pending.regionId = 7;
    f.proj().pendingGraphLoad = std::move(pending);
    f.push(CancelGraphLoadEvent{7});
    f.drain();
    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("ConfirmGraphTrust accepts embedded scripts and records the trust hash") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    // Build an L0 graph that carries an embedded Script node (so it needs trust).
    ProcessingNodeGraph graph = buildDefaultGraph(StandardAxis::L0);
    ProcessingGraphNode script;
    script.id = graph.allocId();
    script.type = GraphNodeType::Script;
    script.scriptEmbeddedSource = "// embedded code";
    script.role = StandardAxis::L0;
    graph.nodes.push_back(script);

    PendingGraphLoad pending;
    pending.regionId = id;
    pending.graph = graph;
    pending.savedAxes = {StandardAxis::L0};
    pending.needsTrust = true;
    f.proj().pendingGraphLoad = std::move(pending);

    f.push(ConfirmGraphTrustEvent{id});
    f.drain();

    // Trust accepted: the matching-axes graph applied immediately and the pending load cleared.
    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
    // The trust record persisted to disk.
    CHECK(std::filesystem::exists(ofs::util::getPrefPath() / "trusted_graphs.json"));
}

TEST_CASE("SaveEmbeddedScript promotes an embedded node to a file node") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    auto &region = f.proj().regions[0];
    const int id = region.id;

    ProcessingGraphNode script;
    script.id = region.nodeGraph.allocId();
    script.type = GraphNodeType::Script;
    script.scriptEmbeddedSource = "// promote me";
    region.nodeGraph.nodes.push_back(script);
    const int nodeId = script.id;

    const std::string fileName = "ofs_test_promoted_script.cs";
    auto scriptPath = ofs::util::getPrefPath() / "scripts" / ofs::util::fromUtf8(fileName);
    std::filesystem::remove(scriptPath);

    f.push(SaveEmbeddedScriptEvent{.regionId = id, .nodeId = nodeId, .fileName = fileName});
    f.drain();

    auto *node = f.proj().regions[0].nodeGraph.findNode(nodeId);
    REQUIRE(node != nullptr);
    CHECK(node->scriptFile == fileName);
    CHECK(node->scriptEmbeddedSource.empty());
    CHECK(std::filesystem::exists(scriptPath));

    std::filesystem::remove(scriptPath);
}

// ── ModifyEvent<T> passive-state handlers ───────────────────────────────────────

// ── Auto-backup timer (update) ──────────────────────────────────────────────────

TEST_CASE("update() writes a dated auto-backup once the interval elapses on a changed project") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = true; // arm the backup timer
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    auto filePath = std::filesystem::temp_directory_path() / "ofs_backup_proj.ofp";
    tp.project.state.filePath = filePath.string();
    // A real edit event marks the project changed-since-backup; the dated backup is dirty-gated, so a
    // clean project never produces one.
    tp.eq.push(AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
    tp.eq.drain();

    const auto backupDir = ofs::backup::dirForProject(filePath);
    std::error_code ec;
    std::filesystem::remove_all(backupDir, ec);

    auto countBackups = [&]() {
        int n = 0;
        std::error_code it;
        for (const auto &e : std::filesystem::directory_iterator(backupDir, it))
            if (e.path().extension() == ".ofp" && ofs::util::toUtf8(e.path().filename()).starts_with("backup-"))
                ++n;
        return n;
    };

    pm.update(61.0f); // crosses the 60s interval -> schedules the backup write

    // The write runs on a worker; update() finalizes it once the future is ready.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (countBackups() == 0 && std::chrono::steady_clock::now() < deadline) {
        pm.update(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(countBackups() == 1);

    // A second interval with no edit in between must not add another (duplicate) backup — the dirty-gate
    // keeps the rolling window full of distinct snapshots, not idle copies.
    pm.update(61.0f);
    for (int i = 0; i < 50; ++i) {
        pm.update(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(countBackups() == 1);

    std::filesystem::remove_all(backupDir, ec);
}

TEST_CASE("update() skips the auto-backup when the project is unchanged") {
    TestProject tp;
    AppSettings appSettings;
    appSettings.autoBackupEnabled = true;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    auto filePath = std::filesystem::temp_directory_path() / "ofs_backup_clean.ofp";
    tp.project.state.filePath = filePath.string();
    tp.eq.drain();

    const auto backupDir = ofs::backup::dirForProject(filePath);
    std::error_code ec;
    std::filesystem::remove_all(backupDir, ec);

    pm.update(61.0f); // interval elapses, but nothing changed (editRevision == backupRevision == 0)
    for (int i = 0; i < 50; ++i) {
        pm.update(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // No backup directory/files were created for an untouched project.
    int n = 0;
    std::error_code it;
    for (const auto &e : std::filesystem::directory_iterator(backupDir, it))
        if (e.path().extension() == ".ofp")
            ++n;
    CHECK(n == 0);

    std::filesystem::remove_all(backupDir, ec);
}

// ── Graph dialog-flow early returns and remap error branches ────────────────────

TEST_CASE("SaveGraph/LoadGraph for an unknown region return before any dialog") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    // No region with this id: both coroutines co_return before awaiting a file dialog.
    f.push(SaveGraphEvent{999});
    f.push(LoadGraphEvent{999});
    f.drain();
    CHECK_FALSE(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("Regions share one timeline track: overlap snaps forward globally, assignment never gates it") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    // Anchor 2 falls inside the first region; creation snaps the new one past it (global, not
    // per-axis) rather than failing, so it lands in the free slot at [5,9].
    f.push(CreateRegionEvent{.axisRole = StandardAxis::R0, .startTime = 2.0, .endTime = 6.0});
    f.drain();
    REQUIRE(f.proj().regions.size() == 2);
    CHECK(f.proj().regions[1].startTime == doctest::Approx(5.0));

    // Assigning a second axis only adds graph I/O; with no possible overlap it always succeeds.
    const int id = f.proj().regions[0].id;
    f.push(AssignAxisToRegionEvent{.regionId = id, .axis = StandardAxis::R0, .assign = true});
    f.drain();
    auto *region = f.proj().findRegion(id);
    REQUIRE(region != nullptr);
    CHECK(region->axisRoles.test(static_cast<size_t>(StandardAxis::R0)));
}

TEST_CASE("ApplyGraphRemap rejects duplicate target axes") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
    f.drain();
    const int id = f.proj().regions[0].id;

    auto setPending = [&] {
        PendingGraphLoad p;
        p.regionId = id;
        p.graph = buildDefaultGraph(StandardAxis::L0);
        p.savedAxes = {StandardAxis::L0};
        f.proj().pendingGraphLoad = std::move(p);
    };

    // Two source axes mapped onto the same target → error, pending load left in place.
    setPending();
    f.push(ApplyGraphRemapEvent{.regionId = id,
                                .mapping = {{.from = StandardAxis::L0, .to = StandardAxis::R0},
                                            {.from = StandardAxis::R0, .to = StandardAxis::R0}}});
    f.drain();
    CHECK(f.proj().pendingGraphLoad.has_value());
}

TEST_CASE("ModifyEvent<SimulatorState> and ModifyEvent<VideoPlayerState>") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.proj().clearDirtyFlags();

    // SimulatorState changes do NOT mark the project dirty (app-level pref).
    f.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.use3dSimulator = true; }});
    f.drain();
    CHECK(f.proj().simulator.use3dSimulator);
    CHECK_FALSE(f.pm.isDirty());

    // VideoPlayerState changes DO mark dirty.
    f.push(ModifyEvent<VideoPlayerState>{[](VideoPlayerState &v) { v.resolutionScale = 0.5f; }});
    f.drain();
    CHECK(f.proj().videoPlayer.resolutionScale == doctest::Approx(0.5f));
    CHECK(f.pm.isDirty());
}

TEST_CASE("SetPluginProjectDataEvent stores per-plugin data, marks dirty, and stays out of undo") {
    PMFixture f;
    f.showAxis(StandardAxis::L0); // an active project, so setDirty() isn't a no-op
    f.proj().clearDirtyFlags();

    // A write lands under pluginName→key, marks the project dirty, but is NOT undoable (metadata-like).
    f.push(SetPluginProjectDataEvent{.pluginName = "P", .key = "a", .value = nlohmann::json{{"foo", 42}}});
    f.drain();
    CHECK(f.proj().pluginData["P"]["a"] == (nlohmann::json{{"foo", 42}}));
    CHECK(f.pm.isDirty());
    CHECK_FALSE(f.undo.canUndo());

    // A second plugin's data coexists independently — namespacing keeps the two stores isolated.
    f.push(SetPluginProjectDataEvent{.pluginName = "Q", .key = "a", .value = nlohmann::json("q")});
    f.drain();
    CHECK(f.proj().pluginData["P"]["a"] == (nlohmann::json{{"foo", 42}}));
    CHECK(f.proj().pluginData["Q"]["a"] == "q");

    // A null value erases just that key; emptying a plugin's last key drops the whole plugin object.
    f.push(SetPluginProjectDataEvent{.pluginName = "P", .key = "a", .value = nlohmann::json(nullptr)});
    f.drain();
    CHECK_FALSE(f.proj().pluginData.contains("P"));
    CHECK(f.proj().pluginData.contains("Q"));

    // Empty plugin name or key is ignored.
    f.proj().clearDirtyFlags();
    f.push(SetPluginProjectDataEvent{.pluginName = "", .key = "a", .value = 1});
    f.push(SetPluginProjectDataEvent{.pluginName = "P", .key = "", .value = 1});
    f.drain();
    CHECK_FALSE(f.pm.isDirty());
}

// ── Axis strip visibility presets ───────────────────────────────────────────────

TEST_CASE("ShowMultiAxis reveals every device axis L0..R2 in the strip") {
    PMFixture f;
    f.showAxis(StandardAxis::L0); // only L0 visible to start
    f.proj().clearDirtyFlags();

    f.push(ShowMultiAxisEvent{});
    f.drain();

    for (size_t i = 0; i <= static_cast<size_t>(StandardAxis::R2); ++i) {
        CAPTURE(i);
        CHECK(f.proj().axes[i].showInStrip);
        CHECK(f.proj().axes[i].isVisible);
    }
    // Scratch axes are untouched, and the reveal marks the project dirty.
    CHECK_FALSE(f.axis(StandardAxis::S0).showInStrip);
    CHECK(f.pm.isDirty());

    // Re-issuing it when everything is already shown changes nothing (no dirty re-arm).
    f.proj().clearDirtyFlags();
    f.push(ShowMultiAxisEvent{});
    f.drain();
    CHECK_FALSE(f.pm.isDirty());
}

TEST_CASE("ShowL0Only hides every other axis and reselects L0") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.showAxis(StandardAxis::S0, {{1.0, 42}}); // scratch with data: hidden but kept in existence
    f.push(AxisSelectedEvent{StandardAxis::R0});
    f.drain();
    REQUIRE(f.proj().state.activeAxis == StandardAxis::R0);

    f.push(ShowL0OnlyEvent{});
    f.drain();

    CHECK(f.axis(StandardAxis::L0).showInStrip);
    CHECK(f.axis(StandardAxis::L0).isVisible);
    CHECK_FALSE(f.axis(StandardAxis::R0).showInStrip);
    CHECK_FALSE(f.axis(StandardAxis::S0).showInStrip);
    CHECK(f.axis(StandardAxis::S0).actions.size() == 1);  // data never cleared
    CHECK(f.proj().state.activeAxis == StandardAxis::L0); // reselected after the prior active was hidden
}

TEST_CASE("Hiding the active axis from the panel reselects another strip axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0);
    f.showAxis(StandardAxis::R0);
    f.push(AxisSelectedEvent{StandardAxis::R0});
    f.drain();
    REQUIRE(f.proj().state.activeAxis == StandardAxis::R0);

    // Hiding the *active* axis triggers the reselect branch → first remaining strip axis (L0).
    f.push(ToggleAxisPanelVisibilityEvent{.axisRole = StandardAxis::R0, .inPanel = false});
    f.drain();
    CHECK_FALSE(f.axis(StandardAxis::R0).showInStrip);
    CHECK(f.proj().state.activeAxis == StandardAxis::L0);
}

// ── Bake fallbacks ──────────────────────────────────────────────────────────────

TEST_CASE("BakeRegion on an unknown region id is a no-op") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}});
    f.push(BakeRegionEvent{999});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.size() == 1); // nothing touched
}

// When the region's graph has no Output node to name a baked axis, baking falls back to the first of
// the region's assigned axes that has resolved output.
TEST_CASE("BakeRegion with no Output node falls back to the region's assigned axis") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{0.0, 0}, {10.0, 100}});

    // A region whose graph carries only an Input node (no Output) but is assigned to L0.
    ProcessingRegion region;
    region.id = 1;
    region.startTime = 2.0;
    region.endTime = 6.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    ProcessingNodeGraph g;
    g.nodes.push_back({.id = g.allocId(), .type = GraphNodeType::Input, .role = StandardAxis::L0});
    region.nodeGraph = g;
    f.proj().regions.push_back(region);
    f.proj().sortRegions();

    ResolvedActions resolved;
    resolved.actions.insert({3.0, 33});
    resolved.actions.insert({5.0, 55});
    f.axis(StandardAxis::L0).resolved = resolved;

    f.push(BakeRegionEvent{1});
    f.drain();
    CHECK(f.proj().regions.empty());
    // The fallback baked L0's resolved slice into the [2,6] span.
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{3.0, 0}));
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{5.0, 0}));
}

// ── Edit guards (no-op early returns) ────────────────────────────────────────────

TEST_CASE("MoveActionToCurrentTime no-ops on empty, on-cursor, and occupied-slot cases") {
    PMFixture f;
    f.showAxis(StandardAxis::L0); // empty axis
    f.proj().playback.cursorPos = 2.0;

    // Empty actions: nothing to move.
    f.push(MoveActionToCurrentTimeEvent{StandardAxis::L0});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.empty());

    // Cursor already sits on the only action: the nearest action == cursor → no move.
    f.showAxis(StandardAxis::L0, {{2.0, 50}});
    f.push(MoveActionToCurrentTimeEvent{StandardAxis::L0});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(2.0));

    // The cursor's slot is already occupied by another action: moving the nearest onto it is refused.
    f.showAxis(StandardAxis::L0, {{1.0, 10}}); // now {1.0,10} and {2.0,50}; cursor at 2.0 is occupied
    f.push(MoveActionToCurrentTimeEvent{StandardAxis::L0});
    f.drain();
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 2);
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{1.0, 0})); // 1.0 stayed put
}

TEST_CASE("CopySelection skips axes whose selection is empty") {
    PMFixture f;
    f.showAxis(StandardAxis::L0, {{1.0, 10}, {2.0, 20}});
    f.showAxis(StandardAxis::R0, {{1.0, 30}}); // present and in the group, but never selected
    f.push(AxisSelectedEvent{StandardAxis::L0});
    f.push(SelectRequestEvent{.gesture = SelectGesture::All, .axis = StandardAxis::L0});
    f.drain();

    f.push(CopySelectionEvent{});
    f.drain();
    // Only L0 was selected, so the empty-R0 branch is skipped: pasting reproduces L0 alone.
    f.push(PasteActionsEvent{.pasteTime = 5.0, .exact = false});
    f.drain();
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{5.0, 0}));
    CHECK(f.axis(StandardAxis::L0).actions.contains(ScriptAxisAction{6.0, 0}));
    // R0 received nothing — it had no selection to copy.
    CHECK(f.axis(StandardAxis::R0).actions.size() == 1);
}

// ── New-project media auto-discovery (initNewProject / initNewProjectFromFunscript) ──
//
// Dropping a media or funscript file is the public entry into the otherwise-private new-project
// flows. A fresh fixture is never dirty, so guardUnsaved proceeds without a (here unwired) modal,
// and openProjectVideo only pushes an unhandled Load/Dummy event — so these tests exercise the
// sibling-discovery and tag-mapping logic without any video player.

namespace {
// Write a one-action single-axis funscript ({atMs, pos}) to `path`, optionally with a title.
void writeFunscript(const std::filesystem::path &path, int atMs, int pos, const std::string &title = "") {
    ofs::Funscript fs;
    fs.actions = {{.at = atMs, .pos = pos}};
    fs.metadata.title = title;
    REQUIRE(fs.save(path));
}
} // namespace

TEST_CASE("Dropping media auto-discovers sibling funscripts by stem, axis suffix, and scratch fallback") {
    PMFixture f;

    const auto dir = std::filesystem::temp_directory_path() / "ofs_disc_map";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto video = dir / "clip.mp4";
    std::ofstream(video).put('x'); // existence only; the resulting Load event is unhandled here

    writeFunscript(dir / "clip.funscript", 1000, 50, "Primary"); // stem match -> primary L0, metadata adopted
    writeFunscript(dir / "clip.roll.funscript", 2000, 30);       // ".roll" suffix -> R1
    writeFunscript(dir / "clip.xyz.funscript", 3000, 40);        // unknown suffix -> first hidden scratch slot

    f.push(OpenDroppedFileEvent{ofs::util::toUtf8(video)});
    REQUIRE(f.drainUntil([&] { return f.axis(StandardAxis::S0).actions.size() == 1; }));

    CHECK(f.proj().state.mediaPath == ofs::util::toUtf8(video));
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));
    CHECK(f.proj().metadata.title == "Primary"); // the stem-matching file is the canonical metadata source

    REQUIRE(f.axis(StandardAxis::R1).actions.size() == 1);
    CHECK(f.axis(StandardAxis::R1).actions[0].at == doctest::Approx(2.0));

    // "xyz" maps to no standard axis, so it lands on S0 — the first scratch slot still hidden after setup.
    REQUIRE(f.axis(StandardAxis::S0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::S0).actions[0].at == doctest::Approx(3.0));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Dropping media adopts a multi-axis sibling funscript, fanning channels out by tag") {
    PMFixture f;

    const auto dir = std::filesystem::temp_directory_path() / "ofs_disc_multi";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto video = dir / "clip.mp4";
    std::ofstream(video).put('x');

    VectorSet<ScriptAxisAction> l0, r0;
    l0.insert({1.0, 50});
    r0.insert({2.0, 60});
    auto fs = ofs::Funscript::fromAxes11({{"L0", l0}, {"R0", r0}}); // root L0 + axes[] R0
    REQUIRE(fs.save(dir / "clip.funscript"));

    f.push(OpenDroppedFileEvent{ofs::util::toUtf8(video)});
    REQUIRE(f.drainUntil([&] { return f.axis(StandardAxis::R0).actions.size() == 1; }));

    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));
    REQUIRE(f.axis(StandardAxis::R0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::R0).actions[0].at == doctest::Approx(2.0));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Dropping a lone funscript opens a media-less project on its mapped axis") {
    PMFixture f;

    const auto dir = std::filesystem::temp_directory_path() / "ofs_disc_solo";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeFunscript(dir / "solo.funscript", 1500, 60, "Solo"); // no sibling media in this dir

    f.push(OpenDroppedFileEvent{ofs::util::toUtf8(dir / "solo.funscript")});
    REQUIRE(f.drainUntil([&] { return f.axis(StandardAxis::L0).actions.size() == 1; }));

    CHECK(f.proj().state.mediaPath.empty());   // stays media-less
    CHECK(f.proj().state.dummyDuration > 0.0); // dummy timeline span seeded
    CHECK(f.proj().metadata.title == "Solo");  // funscript metadata adopted
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.5));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Dropping a funscript that has sibling media defers to the media's auto-discovery") {
    PMFixture f;

    const auto dir = std::filesystem::temp_directory_path() / "ofs_disc_fssibling";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "clip.mp4").put('x');         // sibling media sharing the funscript stem
    writeFunscript(dir / "clip.funscript", 1000, 50); // dropping THIS resolves to the media

    f.push(OpenDroppedFileEvent{ofs::util::toUtf8(dir / "clip.funscript")});
    REQUIRE(f.drainUntil([&] { return f.axis(StandardAxis::L0).actions.size() == 1; }));

    // The funscript drop found "clip.mp4" and opened a media-backed project instead of a media-less one.
    CHECK(f.proj().state.mediaPath == ofs::util::toUtf8(dir / "clip.mp4"));
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Dropping a lone multi-axis funscript fans each tagged track onto its axis, media-less") {
    PMFixture f;

    const auto dir = std::filesystem::temp_directory_path() / "ofs_disc_multi";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    VectorSet<ScriptAxisAction> l0, r0;
    l0.insert({1.0, 50});
    r0.insert({2.0, 60});
    auto fs = ofs::Funscript::fromAxes11({{"L0", l0}, {"R0", r0}}); // root L0 + axes[] R0
    REQUIRE(fs.save(dir / "multi.funscript"));                      // no sibling media in this dir

    f.push(OpenDroppedFileEvent{ofs::util::toUtf8(dir / "multi.funscript")});
    REQUIRE(f.drainUntil([&] { return f.axis(StandardAxis::R0).actions.size() == 1; }));

    CHECK(f.proj().state.mediaPath.empty()); // media-less project
    REQUIRE(f.axis(StandardAxis::L0).actions.size() == 1);
    CHECK(f.axis(StandardAxis::L0).actions[0].at == doctest::Approx(1.0));
    REQUIRE(f.axis(StandardAxis::R0).actions.size() == 1); // tagged track fanned to R0
    CHECK(f.axis(StandardAxis::R0).actions[0].at == doctest::Approx(2.0));

    std::filesystem::remove_all(dir);
}
