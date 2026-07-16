#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Format/AppSettings.h"
#include "Format/Funscript.h"
#include "Format/Project.h"
#include "Services/JobSystem.h"
#include "Services/ProjectManager.h"
#include "Services/UndoSystem.h"
#include "UI/Modals.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "helpers/EventCapture.h"
#include "helpers/FixtureCompare.h"
#include "helpers/TestProject.h"
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <functional>
#include <map>
#include <thread>

using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::TestProject;

// Helper: spin-wait until ProjectManager is no longer dirty (pending write resolved).
static bool waitForSave(ofs::ProjectManager &pm, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (pm.isDirty()) {
        pm.update(0.0f);
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Helper: pump the queue until `done`. Open/import/export run their file I/O on a JobSystem worker
// and resume the flow via ResumeFlowEvent, so a single drain() no longer applies the result — keep
// draining until the worker has resumed and the predicate observes the applied state.
static bool drainUntil(ofs::EventQueue &eq, const std::function<bool()> &done, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        eq.drain();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST_CASE("ProjectManager: save to temp file clears the dirty flag") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    // Set up a present axis with one action so hasActiveProject() returns true.
    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_save.ofp";

    // Manually set the file path — avoids the save-as dialog.
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true; // mark dirty so isDirty() returns true

    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();

    CHECK(waitForSave(pm));
    CHECK_FALSE(pm.isDirty());
    CHECK(std::filesystem::exists(filePath));

    std::filesystem::remove(filePath);
}

// Closing a dirty project and choosing "Save" in the unsaved-changes prompt must end on a *closed*
// project (the welcome screen). guardUnsaved's Save branch schedules an ASYNC write and returns
// immediately, then proceed() closes the project (resetDocument clears filePath). When that write
// later completes, finalizePendingWrite must not stamp filePath back onto the now-closed project —
// doing so flips hasActiveProject() back to true and bounces the UI from the welcome screen into the
// editor. Regression test for that resurrection.
TEST_CASE("ProjectManager: saving via the close prompt lands on a closed (welcome-screen) project") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);

    // Stand in for the ModalManager: answer the unsaved-changes prompt with "Save" (button index 0)
    // and resume the suspended flow, exactly as the real UI would.
    tp.eq.on<ofs::ShowModalEvent>([](const ofs::ShowModalEvent &e) {
        if (!e.handle || !e.resultSlot)
            return;
        *e.resultSlot = 0; // Save
        e.handle.resume();
    });
    ofs::test::EventCapture<ofs::NotifyEvent> notes; // the save-success notify marks finalize done
    notes.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_close_then_save.ofp";
    std::filesystem::remove(filePath);
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true; // dirty so the close prompt fires
    REQUIRE(pm.isDirty());
    REQUIRE(pm.hasActiveProject());

    // Close with unsaved changes → prompt → "Save" → save scheduled (async) → proceed() closes.
    tp.eq.push(ofs::CloseProjectRequestEvent{});
    tp.eq.drain();
    // The project is closed synchronously once the save is scheduled.
    CHECK_FALSE(pm.hasActiveProject());

    // Pump until the async write finalizes (the success notify fires only after finalizePendingWrite).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (notes.received.empty() && std::chrono::steady_clock::now() < deadline) {
        pm.update(0.0f);
        tp.eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(std::filesystem::exists(filePath)); // the save really happened

    // The completed write must not bring the closed project back to life.
    CHECK_FALSE(pm.hasActiveProject());

    std::filesystem::remove(filePath);
}

// A negative timestamp is never valid — the timeline starts at t=0. Every path that places an action
// at a caller-supplied time must clamp it to 0 rather than create a point before the start of the script.
TEST_CASE("ProjectManager: AddActionAtTimeEvent clamps a negative time to 0") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = -1.5, .pos = 50});
    tp.eq.drain();

    const auto &axis = tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    REQUIRE(axis.actions.size() == 1);
    CHECK(axis.actions[0].at == doctest::Approx(0.0));
    CHECK(axis.actions[0].pos == 50);
}

TEST_CASE("ProjectManager: AddActionAtTimeEvent clamps an out-of-range position into [0,100]") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 150});
    tp.eq.push(ofs::AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 2.0, .pos = -30});
    tp.eq.drain();

    const auto &axis = tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    REQUIRE(axis.actions.size() == 2);
    CHECK(axis.actions[0].pos == 100); // 150 -> 100
    CHECK(axis.actions[1].pos == 0);   // -30 -> 0
}

TEST_CASE("ProjectManager: SplitRegionEvent splits at the playhead into two graph-copied halves") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::CreateRegionEvent{
        .axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0, .timelineDuration = 100.0});
    tp.eq.drain();
    REQUIRE(tp.project.regions.size() == 1);
    const int origId = tp.project.regions[0].id;
    const size_t graphNodes = tp.project.regions[0].nodeGraph.nodes.size();
    const ImU32 origColor = tp.project.regions[0].color;
    const std::string origName = tp.project.regions[0].name;

    tp.eq.push(ofs::SplitRegionEvent{.regionId = origId, .splitTime = 4.0});
    tp.eq.drain();

    REQUIRE(tp.project.regions.size() == 2); // regions stay sorted by startTime
    const auto &left = tp.project.regions[0];
    const auto &right = tp.project.regions[1];
    CHECK(left.startTime == doctest::Approx(0.0));
    CHECK(left.endTime == doctest::Approx(4.0));
    CHECK(right.startTime == doctest::Approx(4.0));
    CHECK(right.endTime == doctest::Approx(10.0));
    CHECK(left.id == origId);                          // the left half is the original, shrunk
    CHECK(right.id != origId);                         // the right half is a fresh region
    CHECK(right.nodeGraph.nodes.size() == graphNodes); // graph copied, not a fresh default
    CHECK(right.color != origColor);                   // new color so the halves read as distinct
    CHECK(right.name != origName);                     // new name for the split-off region
}

TEST_CASE("ProjectManager: SplitRegionEvent no-ops when a half would fall below the minimum") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::CreateRegionEvent{
        .axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0, .timelineDuration = 100.0});
    tp.eq.drain();
    const int origId = tp.project.regions[0].id;

    tp.eq.push(ofs::SplitRegionEvent{.regionId = origId, .splitTime = 0.3}); // left half 0.3 s < 0.5 s
    tp.eq.drain();

    REQUIRE(tp.project.regions.size() == 1); // unchanged
    CHECK(tp.project.regions[0].endTime == doctest::Approx(10.0));
}

TEST_CASE("ProjectManager: SplitRegionEvent no-ops on an unknown region id") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::CreateRegionEvent{
        .axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0, .timelineDuration = 100.0});
    tp.eq.drain();
    const int origId = tp.project.regions[0].id;

    tp.eq.push(ofs::SplitRegionEvent{.regionId = origId + 999, .splitTime = 4.0}); // no such region
    tp.eq.drain();

    REQUIRE(tp.project.regions.size() == 1); // unchanged
    CHECK(tp.project.regions[0].endTime == doctest::Approx(10.0));
}

// A split is one undo step: UndoSystem snapshots on SplitRegionEvent (it registers before
// ProjectManager), so undo restores the single pre-split region and redo re-applies the split.
TEST_CASE("ProjectManager: SplitRegionEvent is one undo step (undo merges, redo re-splits)") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::UndoSystem undo(tp.project, tp.eq); // before pm so its snapshot captures the pre-split state
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.eq.push(ofs::CreateRegionEvent{
        .axisRole = StandardAxis::L0, .startTime = 0.0, .endTime = 10.0, .timelineDuration = 100.0});
    tp.eq.drain();
    undo.endFrame(); // commit the create as its own step, separate from the split below
    REQUIRE(tp.project.regions.size() == 1);
    const int origId = tp.project.regions[0].id;

    tp.eq.push(ofs::SplitRegionEvent{.regionId = origId, .splitTime = 4.0});
    tp.eq.drain();
    undo.endFrame();
    REQUIRE(tp.project.regions.size() == 2);
    REQUIRE(undo.canUndo());

    // Undo merges the two halves back into the single original region [0,10].
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.regions.size() == 1);
    CHECK(tp.project.regions[0].id == origId);
    CHECK(tp.project.regions[0].startTime == doctest::Approx(0.0));
    CHECK(tp.project.regions[0].endTime == doctest::Approx(10.0));

    // Redo re-applies the split.
    REQUIRE(undo.canRedo());
    tp.eq.push(ofs::RedoEvent{});
    tp.eq.drain();
    REQUIRE(tp.project.regions.size() == 2);
    CHECK(tp.project.regions[0].endTime == doctest::Approx(4.0));
    CHECK(tp.project.regions[1].startTime == doctest::Approx(4.0));
}

TEST_CASE("ProjectManager: MoveActionEvent clamps a negative target time to 0") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({2.0, 40}); }, tp.eq);
    tp.eq.drain();

    // Drag the point left of t=0 and off the top of the range at once — both clamp.
    tp.eq.push(
        ofs::MoveActionEvent{.axis = StandardAxis::L0, .fromAt = 2.0, .toAt = -0.75, .toPos = 150, .snapshot = true});
    tp.eq.drain();

    const auto &axis = tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    REQUIRE(axis.actions.size() == 1);
    CHECK(axis.actions[0].at == doctest::Approx(0.0));
    CHECK(axis.actions[0].pos == 100);
}

TEST_CASE("ProjectManager: save then reload round-trips axis actions") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(
        StandardAxis::L0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 50});
            a.actions.insert({2.0, 75});
            a.actions.insert({3.0, 25});
        },
        tp.eq);
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_roundtrip.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true;

    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    // Reload by requesting open with the saved path.
    // closeProject() will call checkUnsavedChanges() — not dirty, so no dialog.
    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(
        drainUntil(tp.eq, [&] { return tp.project.axes[static_cast<size_t>(StandardAxis::L0)].actions.size() == 3; }));

    const auto &axis = tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    REQUIRE(axis.actions.size() == 3);
    CHECK(axis.actions[0].at == doctest::Approx(1.0));
    CHECK(axis.actions[0].pos == 50);
    CHECK(axis.actions[1].at == doctest::Approx(2.0));
    CHECK(axis.actions[1].pos == 75);
    CHECK(axis.actions[2].at == doctest::Approx(3.0));
    CHECK(axis.actions[2].pos == 25);

    std::filesystem::remove(filePath);
}

// Hiding an edited standard axis from the strip is not deletion: it keeps its data, so exists() stays
// true and the save filter must persist it. It reloads still hidden, with its actions intact. Guards
// against exists() narrowing back to showInStrip alone, which would silently drop the data on save.
TEST_CASE("ProjectManager: a standard axis hidden from the strip keeps its data through save/reload") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    const auto r0i = static_cast<size_t>(StandardAxis::R0);
    tp.project.axes[0].showInStrip = true; // L0 shown → project is active
    tp.project.axes[r0i].showInStrip = true;
    tp.project.mutate(
        StandardAxis::R0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 20});
            a.actions.insert({2.0, 80});
        },
        tp.eq);
    tp.eq.drain();

    // Hide R0 from the strip. Its actions remain, so exists() stays true.
    tp.eq.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = StandardAxis::R0, .inPanel = false});
    tp.eq.drain();
    REQUIRE_FALSE(tp.project.axes[r0i].showInStrip);
    REQUIRE(tp.project.axes[r0i].exists());

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_hidden_axis_roundtrip.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[r0i].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.axes[r0i].actions.size() == 2; }));

    const auto &r0 = tp.project.axes[r0i];
    CHECK_FALSE(r0.showInStrip);     // reloads still hidden from the strip
    REQUIRE(r0.actions.size() == 2); // data survived the hide + round-trip
    CHECK(r0.actions[0].pos == 20);
    CHECK(r0.actions[1].pos == 80);
    CHECK(r0.exists());

    std::filesystem::remove(filePath);
}

// Quick Export's silent re-export: an ExportFunscriptRequestEvent carrying a targetPath skips the
// file picker, writes straight to that path, and records the parameters as the project's lastExport
// (which Quick Export later replays). Marking dirty is what gets the config persisted on next save.
TEST_CASE("ProjectManager: export with a target path writes files and records the Quick Export config") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();
    tp.project.clearDirtyFlags(); // isolate the dirty assertion below to the export itself

    auto outDir = std::filesystem::temp_directory_path() / "ofs_test_quick_export";
    std::filesystem::remove_all(outDir);

    tp.eq.push(
        ofs::ExportFunscriptRequestEvent{.axes = {StandardAxis::L0}, .format = 0, .targetPath = outDir.string()});
    // Wait for lastExport, set after the worker resumes — by then the file is fully written and closed.
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.state.lastExport.has_value(); }));
    // Format 0 (1.0, one file per axis) writes "<stem>.<tag>.funscript"; stem is "script" with no media path.
    CHECK(std::filesystem::exists(outDir / "script.L0.funscript"));

    REQUIRE(tp.project.state.lastExport.has_value());
    CHECK(tp.project.state.lastExport->format == 0);
    REQUIRE(tp.project.state.lastExport->axes.size() == 1);
    CHECK(tp.project.state.lastExport->axes[0] == StandardAxis::L0);
    CHECK(tp.project.state.lastExport->outputPath == outDir.string());
    CHECK(pm.isDirty()); // recording the config marks dirty so the next save captures it

    std::filesystem::remove_all(outDir);
}

// A project's bookmarks/chapters are global; on export they are written into every funscript file's
// metadata (the standard OFS convention) so other tools — and a re-import — can read them back.
TEST_CASE("ProjectManager: export writes the project's bookmarks and chapters into the funscript") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.project.bookmarks.bookmarks.push_back({.time = 5.5, .name = "mark"});
    tp.project.bookmarks.chapters.push_back({.startTime = 0.0, .endTime = 10.0, .name = "scene"});
    tp.eq.drain();

    auto outDir = std::filesystem::temp_directory_path() / "ofs_test_marker_export";
    std::filesystem::remove_all(outDir);

    tp.eq.push(
        ofs::ExportFunscriptRequestEvent{.axes = {StandardAxis::L0}, .format = 0, .targetPath = outDir.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.state.lastExport.has_value(); }));

    auto loaded = ofs::Funscript::load(outDir / "script.L0.funscript");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->bookmarks.size() == 1);
    CHECK(loaded->bookmarks[0].name == "mark");
    CHECK(loaded->bookmarks[0].time == doctest::Approx(5.5));
    REQUIRE(loaded->chapters.size() == 1);
    CHECK(loaded->chapters[0].name == "scene");
    CHECK(loaded->chapters[0].startTime == doctest::Approx(0.0));
    CHECK(loaded->chapters[0].endTime == doctest::Approx(10.0));

    std::filesystem::remove_all(outDir);
}

// The remembered export config is per-project state and must survive a .ofp save/reload, or Quick
// Export would forget the target after the project is reopened.
TEST_CASE("ProjectManager: Quick Export config round-trips through project save and reload") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    tp.project.state.lastExport = ofs::ExportConfig{
        .format = 2, .axes = {StandardAxis::L0, StandardAxis::R0}, .outputPath = "D:/scripts/foo.funscript"};

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_export_config.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.state.lastExport.has_value(); }));

    REQUIRE(tp.project.state.lastExport.has_value());
    CHECK(tp.project.state.lastExport->format == 2);
    REQUIRE(tp.project.state.lastExport->axes.size() == 2);
    CHECK(tp.project.state.lastExport->axes[0] == StandardAxis::L0);
    CHECK(tp.project.state.lastExport->axes[1] == StandardAxis::R0);
    CHECK(tp.project.state.lastExport->outputPath == "D:/scripts/foo.funscript");

    std::filesystem::remove(filePath);
}

TEST_CASE("Project::load returns nullopt for a missing file") {
    auto missingPath = std::filesystem::temp_directory_path() / "does_not_exist_xyz123.ofp";
    auto result = ofs::Project::load(missingPath);
    CHECK_FALSE(result.has_value());
}

// Phase 7 — fixture-based regression: import a funscript, push it through the full project
// save/reload path (CBOR byte-string axis blob), export it again, and confirm the actions
// survive the round-trip unchanged against the expected fixture.
TEST_CASE("ProjectManager: funscript import → project save/reload → export matches fixture") {
    auto fs = ofs::Funscript::load(ofs::test::fixturePath("single_point.funscript"));
    REQUIRE(fs.has_value());
    const auto srcActions = fs->toActions();

    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [&](ofs::AxisState &a) { a.actions = srcActions; }, tp.eq);
    tp.eq.drain();

    auto ofpPath = std::filesystem::temp_directory_path() / "ofs_fixture_single_point.ofp";
    tp.project.state.filePath = ofpPath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    // Reload from disk, then export the round-tripped axis back to a funscript file.
    tp.eq.push(ofs::OpenProjectRequestEvent{ofpPath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return !tp.project.axes[0].actions.empty(); }));

    auto out = ofs::Funscript::fromActions(tp.project.axes[0].actions);
    auto outPath = std::filesystem::temp_directory_path() / "ofs_fixture_single_point.out.funscript";
    REQUIRE(out.save(outPath));

    std::string diff;
    CHECK_MESSAGE(
        ofs::test::compareFunscriptFiles(outPath, ofs::test::fixturePath("single_point.expected.funscript"), &diff),
        diff);

    std::filesystem::remove(ofpPath);
    std::filesystem::remove(outPath);
}

// One imported axis with a single point, for the ImportFunscriptDataEvent apply tests below.
static ofs::ImportFunscriptDataEvent::Axis importedAxis(std::optional<StandardAxis> role, double at, int pos) {
    ofs::VectorSet<ScriptAxisAction> acts;
    acts.insert({at, pos});
    return {.role = role, .actions = std::move(acts)};
}

// A chosen role places the actions there and makes the axis present — the picker offers axes that
// aren't present yet, so the apply must create them. A successful import reports how many it placed.
TEST_CASE("ProjectManager: ImportFunscriptDataEvent places a chosen role and makes it present") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    ofs::test::EventCapture<ofs::NotifyEvent> notes;
    notes.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    constexpr auto kR0 = static_cast<size_t>(StandardAxis::R0);
    REQUIRE_FALSE(tp.project.axes[kR0].showInStrip); // absent target — the apply must create it

    ofs::ImportFunscriptDataEvent data;
    data.axes.push_back(importedAxis(StandardAxis::R0, 1.0, 60));
    tp.eq.push(std::move(data));
    tp.eq.drain();

    CHECK(tp.project.axes[kR0].showInStrip);
    REQUIRE(tp.project.axes[kR0].actions.size() == 1);
    CHECK(tp.project.axes[kR0].actions[0].pos == 60);
    REQUIRE(notes.received.size() == 1);
    CHECK(notes.received[0].level == ofs::NotifyLevel::Success);
}

// A roleless entry (single-axis import, or a tag that didn't match a non-scratch axis) lands in the
// first free scratch slot, which becomes present.
TEST_CASE("ProjectManager: ImportFunscriptDataEvent routes a roleless axis to the first free scratch slot") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    ofs::test::EventCapture<ofs::NotifyEvent> notes;
    notes.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    constexpr auto kS0 = static_cast<size_t>(StandardAxis::S0);
    REQUIRE_FALSE(tp.project.axes[kS0].showInStrip);

    ofs::ImportFunscriptDataEvent data;
    data.axes.push_back(importedAxis(std::nullopt, 1.0, 42));
    tp.eq.push(std::move(data));
    tp.eq.drain();

    CHECK(tp.project.axes[kS0].showInStrip);
    REQUIRE(tp.project.axes[kS0].actions.size() == 1);
    CHECK(tp.project.axes[kS0].actions[0].pos == 42);
    REQUIRE(notes.received.size() == 1);
    CHECK(notes.received[0].level == ofs::NotifyLevel::Success);
}

// With every scratch slot already taken, a roleless axis has nowhere to go. It used to be dropped
// silently; now the user gets an error telling them how many axes were lost.
TEST_CASE("ProjectManager: ImportFunscriptDataEvent reports an error when scratch slots are exhausted") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    ofs::test::EventCapture<ofs::NotifyEvent> notes;
    notes.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    // Occupy all ten scratch slots (S0–S9).
    for (auto i = static_cast<size_t>(StandardAxis::S0); i < ofs::kStandardAxisCount; ++i)
        tp.project.axes[i].showInStrip = true;

    ofs::ImportFunscriptDataEvent data;
    data.axes.push_back(importedAxis(std::nullopt, 1.0, 30));
    tp.eq.push(std::move(data));
    tp.eq.drain();

    REQUIRE(notes.received.size() == 1);
    CHECK(notes.received[0].level == ofs::NotifyLevel::Error);
    CHECK(notes.received[0].message.find("dropped") != std::string::npos);
}

// A whole import — however many axes it touches — is one undo step: UndoSystem snapshots once on the
// event (it registers before ProjectManager), so a single undo reverts every placed axis at once.
TEST_CASE("ProjectManager: ImportFunscriptDataEvent applies as a single undo step across all axes") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    // UndoSystem before ProjectManager so its snapshot captures the pre-import state.
    ofs::UndoSystem undo(tp.project, tp.eq);
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    constexpr auto kR0 = static_cast<size_t>(StandardAxis::R0);
    constexpr auto kS0 = static_cast<size_t>(StandardAxis::S0);

    // R0 has a prior keyframe (so undo has something to restore); S0 starts empty/absent.
    tp.project.axes[kR0].showInStrip = true;
    tp.project.mutate(StandardAxis::R0, [](ofs::AxisState &a) { a.actions.insert({1.0, 10}); }, tp.eq);
    tp.eq.drain();

    ofs::ImportFunscriptDataEvent data;
    data.axes.push_back(importedAxis(StandardAxis::R0, 1.0, 88)); // overwrite R0
    data.axes.push_back(importedAxis(std::nullopt, 2.0, 55));     // new scratch S0
    tp.eq.push(std::move(data));
    tp.eq.drain();
    undo.endFrame(); // mirror the frame loop: commit the import's pre-edit snapshot

    CHECK(tp.project.axes[kR0].actions[0].pos == 88);
    CHECK(tp.project.axes[kS0].showInStrip);
    REQUIRE(tp.project.axes[kS0].actions.size() == 1);
    REQUIRE(undo.canUndo());

    // One undo reverts both axes: R0 back to its prior value, S0 gone entirely.
    tp.eq.push(ofs::UndoEvent{});
    tp.eq.drain();
    CHECK(tp.project.axes[kR0].actions[0].pos == 10);
    CHECK_FALSE(tp.project.axes[kS0].showInStrip);
    CHECK_FALSE(undo.canUndo());
}

// Metadata is non-axis project state written directly in the service handlers. It must survive the
// CBOR save/reload like axis data does, or reopening a project would silently drop the script's
// title/creator/tags/performers/license.
TEST_CASE("ProjectManager: metadata round-trips through project save and reload") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true; // hasActiveProject() so setDirty() in the handlers takes effect

    using Meta = ofs::FunscriptMetadata;
    tp.eq.push(ofs::ModifyEvent<Meta>{[](Meta &m) {
        m.title = "Saved Title";
        m.creator = "Saver";
        m.scriptUrl = "scr://x";
        m.videoUrl = "vid://y";
        m.license = "Paid";
        m.tags = {"t1", "t2"};
        m.performers = {"perf"};
    }});
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_metadata.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.metadata.title == "Saved Title"; }));

    const auto &m = tp.project.metadata;
    CHECK(m.title == "Saved Title");
    CHECK(m.creator == "Saver");
    CHECK(m.scriptUrl == "scr://x");
    CHECK(m.videoUrl == "vid://y");
    CHECK(m.license == "Paid");
    REQUIRE(m.tags.size() == 2);
    CHECK(m.tags[0] == "t1");
    CHECK(m.tags[1] == "t2");
    REQUIRE(m.performers.size() == 1);
    CHECK(m.performers[0] == "perf");

    std::filesystem::remove(filePath);
}

// Per-plugin data lives on ScriptProject (not on an axis), so its persistence rides the
// saveToProject/loadFromProject copy rather than the axis path. Pin that the copy fires in both
// directions through a real save→reload (the format-level round-trip is covered in test_project.cpp).
TEST_CASE("ProjectManager: per-plugin project data round-trips through save and reload") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true; // hasActiveProject() so the data write marks the project dirty

    // Two plugins' namespaced stores, written through the same event the host ABI pushes.
    tp.eq.push(ofs::SetPluginProjectDataEvent{
        .pluginName = "Ofs.Core", .key = "settings", .value = nlohmann::json{{"Mode", 1}, {"FixedTop", 90}}});
    tp.eq.push(ofs::SetPluginProjectDataEvent{.pluginName = "Other", .key = "flag", .value = true});
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_plugin_data.ofp";
    tp.project.state.filePath = filePath.string();
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    // Clear in memory so the reload, not a stale value, is what we observe.
    tp.project.pluginData = nlohmann::json::object();
    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.pluginData.contains("Ofs.Core"); }));

    const auto &pd = tp.project.pluginData;
    CHECK(pd["Ofs.Core"]["settings"]["Mode"] == 1);
    CHECK(pd["Ofs.Core"]["settings"]["FixedTop"] == 90);
    CHECK(pd["Other"]["flag"] == true);

    std::filesystem::remove(filePath);
}

// The playback cursor is saved with the project and restored on reopen. The seek is deferred until the
// freshly opened media reports its real length (a load fires LoadVideoEvent asynchronously, so the
// player's duration is 0 at load time). This pins: (1) the position rides save→reload, (2) the
// duration=0 reset that precedes the real length does not consume the armed seek, and (3) the seek
// target clamps to the reported duration.
TEST_CASE("ProjectManager: playback position is saved and restored as a deferred, clamped seek") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    ofs::test::EventCapture<ofs::SeekEvent> seeks;
    seeks.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true; // hasActiveProject() so the save marks the project dirty
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.project.playback.cursorPos = 12.5; // the resume point, mirrored each frame from the player
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_resume.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.axes[0].actions.size() == 1; }));
    // resetDocument zeroed the cursor; nothing has seeked yet (media duration is still unknown).
    CHECK(tp.project.playback.cursorPos == doctest::Approx(0.0));
    CHECK(seeks.received.empty());

    // The duration=0 the player emits to clear the previous file must not consume the armed seek.
    tp.eq.push(ofs::DurationChangedEvent{0.0});
    tp.eq.drain();
    CHECK(seeks.received.empty());

    // The real length arrives: the resume seek fires once, clamped into range (12.5 < 20).
    tp.eq.push(ofs::DurationChangedEvent{20.0});
    tp.eq.drain();
    REQUIRE(seeks.received.size() == 1);
    CHECK(seeks.received[0].time == doctest::Approx(12.5));

    // A later duration change (e.g. swapping media) must not re-fire the already-consumed seek.
    tp.eq.push(ofs::DurationChangedEvent{30.0});
    tp.eq.drain();
    CHECK(seeks.received.size() == 1);

    std::filesystem::remove(filePath);
}

// End-to-end action serialization + timestamp rounding: the roundtrip.ofp fixture (written by
// tools/gen_test_fixtures.py) carries L0 actions whose timestamps exercise the seconds<->ms
// conversion. We load it (Python-written CBOR blob → C++), re-save/reload (C++ writer → C++ reader),
// then export to funscript and confirm sub-millisecond timestamps round to the nearest whole ms.
TEST_CASE("ProjectManager: roundtrip.ofp actions survive ofp/funscript round-trip with rounded ms") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    constexpr auto l0 = static_cast<size_t>(StandardAxis::L0);

    tp.eq.push(ofs::OpenProjectRequestEvent{ofs::test::fixturePath("roundtrip.ofp").string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.axes[l0].actions.size() == 5; }));

    REQUIRE(tp.project.axes[l0].actions.size() == 5);
    // The ofp blob stores at*1000 as a full-precision double, so the seconds survive intact.
    CHECK(tp.project.axes[l0].actions[0].at == doctest::Approx(0.5));
    CHECK(tp.project.axes[l0].actions[3].at == doctest::Approx(3.4567));
    CHECK(tp.project.axes[l0].actions[3].pos == 30);

    // Re-save with the C++ writer and reload, proving the sub-ms doubles round-trip both directions.
    auto ofpPath = std::filesystem::temp_directory_path() / "ofs_roundtrip_resave.ofp";
    tp.project.state.filePath = ofpPath.string();
    tp.project.axes[l0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));
    tp.eq.push(ofs::OpenProjectRequestEvent{ofpPath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return tp.project.axes[l0].actions.size() == 5; }));
    REQUIRE(tp.project.axes[l0].actions.size() == 5);
    CHECK(tp.project.axes[l0].actions[3].at == doctest::Approx(3.4567));

    // Export to funscript: at→ms rounds to nearest. Pin it directly and against the expected fixture.
    auto out = ofs::Funscript::fromActions(tp.project.axes[l0].actions);
    REQUIRE(out.actions.size() == 5);
    CHECK(out.actions[3].at == 3457); // 3.4567 s → 3457 ms, rounded (not truncated to 3456)

    auto outPath = std::filesystem::temp_directory_path() / "ofs_roundtrip.out.funscript";
    REQUIRE(out.save(outPath));
    std::string diff;
    CHECK_MESSAGE(
        ofs::test::compareFunscriptFiles(outPath, ofs::test::fixturePath("roundtrip.expected.funscript"), &diff), diff);

    std::filesystem::remove(ofpPath);
    std::filesystem::remove(outPath);
}

// Plugins are notified of a project load via LoadProjectEvent (-> onProjectChange) and learn
// the active axis via AxisSelectedEvent (-> onAxisSelected). Both are produced by the load
// path (clearProject + loadProjectInternal). This test pins that those events actually fire
// on reload, so a plugin caching project/axis state can't silently miss a project switch.
TEST_CASE("ProjectManager: reloading a project emits LoadProjectEvent and AxisSelectedEvent") {
    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);

    ofs::test::EventCapture<ofs::LoadProjectEvent> loadCap;
    ofs::test::EventCapture<ofs::AxisSelectedEvent> selCap;
    loadCap.attach(tp.eq); // must attach before freeze(); eq.on() asserts afterwards
    selCap.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    auto filePath = std::filesystem::temp_directory_path() / "ofs_test_load_events.ofp";
    tp.project.state.filePath = filePath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    loadCap.received.clear(); // watch only the events produced by the reload
    selCap.received.clear();

    tp.eq.push(ofs::OpenProjectRequestEvent{filePath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return selCap.received.size() == 1; }));

    // onProjectChange fires (close clears the old project, load brings in the new one).
    CHECK(loadCap.received.size() >= 1);
    // onAxisSelected fires exactly once, naming the restored active axis.
    REQUIRE(selCap.received.size() == 1);
    CHECK(selCap.received[0].role == StandardAxis::L0);

    std::filesystem::remove(filePath);
}

// A region's axisRoles bitset is serialized as string tags (saveToProject) and rebuilt from those
// tags on load (loadFromProject). The reload also re-validates each region's node graph and replaces
// a structurally invalid one with a default. Both paths run only when a project carries regions.
TEST_CASE("ProjectManager: a region's axis roles and node graph survive save/reload") {
    namespace fs = std::filesystem;
    const fs::path projPath = fs::temp_directory_path() / "ofs_test_region_roundtrip.ofp";

    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    ofs::AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    roles.set(static_cast<size_t>(StandardAxis::R1));
    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 1.0;
    region.endTime = 5.0;
    region.name = "verse";
    region.hz = 24;
    region.axisRoles = roles;
    region.nodeGraph = ofs::buildDefaultGraph(roles); // valid: one Input→Output pair per role
    tp.project.regions = {region};

    tp.project.state.filePath = projPath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{projPath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return !tp.project.regions.empty(); }));

    REQUIRE(tp.project.regions.size() == 1);
    const auto &r = tp.project.regions[0];
    CHECK(r.name == "verse");
    CHECK(r.hz == 24);
    CHECK(r.axisRoles.test(static_cast<size_t>(StandardAxis::L0)));
    CHECK(r.axisRoles.test(static_cast<size_t>(StandardAxis::R1)));
    CHECK_FALSE(r.axisRoles.test(static_cast<size_t>(StandardAxis::R0)));
    CHECK(r.nodeGraph.nodes.size() == region.nodeGraph.nodes.size());

    fs::remove(projPath);
}

// A region whose serialized node graph is structurally invalid (e.g. a foreign/corrupt file) must be
// reset to a default graph on load, not evaluated as-is.
TEST_CASE("ProjectManager: loading a project replaces a region's invalid node graph with a default") {
    namespace fs = std::filesystem;
    const fs::path projPath = fs::temp_directory_path() / "ofs_test_region_badgraph.ofp";

    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    ofs::AxisRoles roles;
    roles.set(static_cast<size_t>(StandardAxis::L0));
    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 2.0;
    region.axisRoles = roles;
    region.nodeGraph = ofs::buildDefaultGraph(roles);
    region.nodeGraph.nodes[0].id = 0; // non-positive id → validateGraph rejects on reload
    tp.project.regions = {region};

    tp.project.state.filePath = projPath.string();
    tp.project.axes[0].dirty = true;
    tp.eq.push(ofs::SaveProjectEvent{false});
    tp.eq.drain();
    REQUIRE(waitForSave(pm));

    tp.eq.push(ofs::OpenProjectRequestEvent{projPath.string()});
    REQUIRE(drainUntil(tp.eq, [&] { return !tp.project.regions.empty(); }));

    REQUIRE(tp.project.regions.size() == 1);
    // Reset to a default graph for the region's roles — and that default is itself valid.
    const auto &g = tp.project.regions[0].nodeGraph;
    for (const auto &n : g.nodes)
        CHECK(n.id > 0);
    std::string err;
    CHECK(ofs::sanitizeProjectGraph(tp.project.regions[0].nodeGraph, effectReg, err));

    fs::remove(projPath);
}

// Promoting a graph-embedded script node to a file in <pref>/scripts: writes the source under the
// chosen name, repoints the node at the file (clearing its inline source), and asks for a recompile —
// unless a *different* file already owns that name, in which case it refuses and leaves the node alone.
TEST_CASE("ProjectManager: SaveEmbeddedScriptEvent materializes or refuses an embedded script node") {
    namespace fs = std::filesystem;
    const fs::path sdir = ofs::util::getPrefPath() / "scripts";
    fs::create_directories(sdir);

    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    ofs::test::EventCapture<ofs::CompileScriptEvent> compiles;
    ofs::test::EventCapture<ofs::ShowModalEvent> modals;
    compiles.attach(tp.eq);
    modals.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 7;
    region.startTime = 0.0;
    region.endTime = 2.0;
    ofs::ProcessingGraphNode node;
    node.id = 3;
    node.type = ofs::GraphNodeType::Script;
    region.nodeGraph.nodes = {node};
    region.nodeGraph.nextId = 4;
    tp.project.regions = {region};

    SUBCASE("an embedded node is written to a new file and repointed at it") {
        const fs::path written = sdir / "ofs_test_promote_new.cs";
        fs::remove(written);
        tp.project.regions[0].nodeGraph.nodes[0].scriptEmbeddedSource = "// embedded body";

        tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "ofs_test_promote_new.cs"});
        tp.eq.drain();

        const auto &n = tp.project.regions[0].nodeGraph.nodes[0];
        CHECK(n.scriptFile == "ofs_test_promote_new.cs");
        CHECK(n.scriptEmbeddedSource.empty());
        auto onDisk = ofs::util::readFile(written);
        REQUIRE(onDisk.has_value());
        CHECK(*onDisk == "// embedded body");
        REQUIRE(compiles.received.size() == 1);
        CHECK(compiles.received[0].fileName == "ofs_test_promote_new.cs");
        CHECK(modals.received.empty());

        fs::remove(written);
    }

    SUBCASE("a name owned by a byte-identical file is reused without rewriting") {
        const fs::path existing = sdir / "ofs_test_promote_same.cs";
        REQUIRE(ofs::util::writeFile(existing, "// shared body"));
        tp.project.regions[0].nodeGraph.nodes[0].scriptEmbeddedSource = "// shared body";

        tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "ofs_test_promote_same.cs"});
        tp.eq.drain();

        const auto &n = tp.project.regions[0].nodeGraph.nodes[0];
        CHECK(n.scriptFile == "ofs_test_promote_same.cs"); // promoted, not refused
        CHECK(n.scriptEmbeddedSource.empty());
        CHECK(modals.received.empty());
        CHECK(compiles.received.size() == 1);

        fs::remove(existing);
    }

    SUBCASE("a name owned by a different file is refused, leaving the node untouched") {
        const fs::path existing = sdir / "ofs_test_promote_clash.cs";
        REQUIRE(ofs::util::writeFile(existing, "// a different local script"));
        tp.project.regions[0].nodeGraph.nodes[0].scriptEmbeddedSource = "// my body";

        tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "ofs_test_promote_clash.cs"});
        tp.eq.drain();

        const auto &n = tp.project.regions[0].nodeGraph.nodes[0];
        CHECK(n.scriptFile.empty()); // still an embedded node
        CHECK(n.scriptEmbeddedSource == "// my body");
        CHECK(ofs::util::readFile(existing) == "// a different local script"); // not clobbered
        CHECK(compiles.received.empty());
        CHECK(modals.received.size() == 1); // error surfaced

        fs::remove(existing);
    }

    SUBCASE("guards: a malformed request changes nothing and surfaces no file") {
        auto &n0 = tp.project.regions[0].nodeGraph.nodes[0];
        n0.scriptEmbeddedSource = "// body";

        SUBCASE("an unknown region id is ignored") {
            tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 999, .nodeId = 3, .fileName = "x.cs"});
        }
        SUBCASE("an empty file name is ignored") {
            tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = ""});
        }
        SUBCASE("a non-script node is ignored") {
            n0.type = ofs::GraphNodeType::Constant;
            tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "x.cs"});
        }
        SUBCASE("a node with neither inline source nor a file reference writes nothing") {
            n0.scriptEmbeddedSource.clear();
            n0.scriptFile.clear();
            tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "x.cs"});
        }
        tp.eq.drain();

        CHECK(compiles.received.empty());
        CHECK(modals.received.empty());
        CHECK(tp.project.regions[0].nodeGraph.nodes[0].scriptFile.empty());
    }

    SUBCASE("forking a missing source file surfaces an error and writes nothing") {
        auto &n0 = tp.project.regions[0].nodeGraph.nodes[0];
        n0.scriptEmbeddedSource.clear();
        n0.scriptFile = "ofs_test_no_such_source_zzz.cs"; // not on disk → fork read fails
        const fs::path dst = sdir / "ofs_test_fork_fail_dst.cs";
        fs::remove(dst);

        tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "ofs_test_fork_fail_dst.cs"});
        tp.eq.drain();

        CHECK(modals.received.size() == 1); // "could not read the source script to fork"
        CHECK(compiles.received.empty());
        CHECK_FALSE(fs::exists(dst));
        CHECK(tp.project.regions[0].nodeGraph.nodes[0].scriptFile == "ofs_test_no_such_source_zzz.cs"); // untouched
    }

    SUBCASE("a file node forks the referenced file's contents into the new name") {
        const fs::path orig = sdir / "ofs_test_fork_src.cs";
        const fs::path fork = sdir / "ofs_test_fork_dst.cs";
        REQUIRE(ofs::util::writeFile(orig, "// original file contents"));
        fs::remove(fork);
        auto &n0 = tp.project.regions[0].nodeGraph.nodes[0];
        n0.scriptFile = "ofs_test_fork_src.cs"; // a file node: no embedded source
        n0.scriptEmbeddedSource.clear();

        tp.eq.push(ofs::SaveEmbeddedScriptEvent{.regionId = 7, .nodeId = 3, .fileName = "ofs_test_fork_dst.cs"});
        tp.eq.drain();

        const auto &n = tp.project.regions[0].nodeGraph.nodes[0];
        CHECK(n.scriptFile == "ofs_test_fork_dst.cs");
        CHECK(ofs::util::readFile(fork) == "// original file contents");
        CHECK(modals.received.empty());

        fs::remove(orig);
        fs::remove(fork);
    }
}

// Reviewing a pending graph's embedded scripts dumps each one to a throwaway temp dir under a safe
// filename: directory parts baked into scriptFile are stripped (no path traversal out of the review
// dir), a missing/extension-less name is normalized to "<...>.cs", and same-named scripts are
// disambiguated so every node's source is independently readable.
TEST_CASE("ProjectManager: ReviewGraphScriptsEvent writes embedded scripts under sanitized names") {
    namespace fs = std::filesystem;

    TestProject tp;
    ofs::AppSettings appSettings;
    appSettings.autoBackupEnabled = false;
    ofs::JobSystem jobSystem;
    ofs::EffectRegistryState effectReg;
    ofs::ProjectManager pm(tp.project, tp.eq, appSettings, jobSystem, effectReg);
    tp.eq.freeze();
    jobSystem.start();

    const auto script = [](int id, std::string file, std::string src) {
        ofs::ProcessingGraphNode n;
        n.id = id;
        n.type = ofs::GraphNodeType::Script;
        n.scriptFile = std::move(file);
        n.scriptEmbeddedSource = std::move(src);
        return n;
    };

    ofs::PendingGraphLoad pending;
    pending.regionId = 5;
    pending.needsTrust = true;
    // A traversal attempt, a same-name pair (disambiguated), an extension-less name, an empty name,
    // plus a file node (no embedded source) and a non-script node that must both be skipped.
    pending.graph.nodes = {
        script(1, "../../escape.cs", "// escape"),
        script(2, "dup.cs", "// dup A"),
        script(3, "dup.cs", "// dup B"),
        script(4, "noext", "// noext"),
        script(5, "", "// nameless"),
        script(6, "filenode.cs", ""), // file node: scriptEmbedded() == false → skipped
    };
    ofs::ProcessingGraphNode plain;
    plain.id = 7;
    plain.type = ofs::GraphNodeType::Constant; // non-script → skipped
    pending.graph.nodes.push_back(plain);
    tp.project.pendingGraphLoad = std::move(pending);

    // A review request that doesn't match the pending load is a no-op (no pending graph for that region).
    tp.eq.push(ofs::ReviewGraphScriptsEvent{.regionId = 999});
    tp.eq.drain();

    tp.eq.push(ofs::ReviewGraphScriptsEvent{.regionId = 5});
    tp.eq.drain();

    const fs::path reviewDir = fs::temp_directory_path() / "ofs-graph-review";
    REQUIRE(fs::is_directory(reviewDir));

    // Collect what landed in the review dir, keyed by filename.
    std::map<std::string, std::string> byName;
    for (const auto &e : fs::directory_iterator(reviewDir)) {
        auto body = ofs::util::readFile(e.path());
        REQUIRE(body.has_value());
        byName[ofs::util::toUtf8(e.path().filename())] = *body;
    }

    // Exactly the five embedded scripts, none of the two skipped nodes.
    CHECK(byName.size() == 5);
    // Traversal stripped to the bare filename component, written inside the review dir.
    CHECK(byName.count("escape.cs") == 1);
    CHECK(byName["escape.cs"] == "// escape");
    // The empty name and the extension-less name are normalized to ".cs".
    CHECK(byName.count("embedded_4.cs") == 1); // node index 4 (the nameless one)
    CHECK(byName["embedded_4.cs"] == "// nameless");
    CHECK(byName.count("noext.cs") == 1);
    CHECK(byName["noext.cs"] == "// noext");
    // The duplicate names resolve to two distinct files carrying their own sources.
    CHECK(byName.count("dup.cs") == 1);
    CHECK(byName.count("dup_2.cs") == 1);
    CHECK(byName["dup.cs"] == "// dup A");
    CHECK(byName["dup_2.cs"] == "// dup B");

    fs::remove_all(reviewDir);
}
