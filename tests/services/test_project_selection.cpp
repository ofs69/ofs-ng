#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Services/SelectionModeRegistry.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::StandardAxis;
using ofs::test::PmFixture;
// C-ABI selection resolver types (defined inside namespace ofs in PluginApi.h).
using ofs::OfsEmitSelect;
using ofs::OfsSelectDrop;
using ofs::OfsSelectFn;
using ofs::OfsSelectPass;
using ofs::OfsSelectReplace;
using ofs::OfsSelectRequest;

// ── Select ────────────────────────────────────────────────────────────────────
TEST_CASE("SelectAll selects every action on the axis") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    CHECK(f.project().axes[0].selection.size() == 3);
}

TEST_CASE("SelectTimeRange (non-additive) selects actions within [start, end]") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 1.5,
                                   .endTime = 3.5,
                                   .additive = false});
    CHECK(f.project().axes[0].selection.size() == 2); // 2.0 and 3.0
}

TEST_CASE("SelectTimeRange (non-additive) replaces a prior selection") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0}); // {1,2,3}
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 0.5,
                                   .endTime = 1.5,
                                   .additive = false}); // -> {1}
    auto &sel = f.project().axes[0].selection;
    REQUIRE(sel.size() == 1);
    CHECK(sel.contains(ofs::ScriptAxisAction{1.0, 0}));
}

// ── Add / subtract (additive toggle) ───────────────────────────────────────────
TEST_CASE("Additive selection accumulates disjoint ranges") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 0.5,
                                   .endTime = 1.5,
                                   .additive = true}); // +{1}
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 2.5,
                                   .endTime = 3.5,
                                   .additive = true}); // +{3}
    auto &sel = f.project().axes[0].selection;
    REQUIRE(sel.size() == 2);
    CHECK(sel.contains(ofs::ScriptAxisAction{1.0, 0}));
    CHECK(sel.contains(ofs::ScriptAxisAction{3.0, 0}));
}

TEST_CASE("Additive selection over an already-selected range subtracts it") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0}); // {1,2,3}
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 1.5,
                                   .endTime = 3.5,
                                   .additive = true}); // toggle 2,3 off
    auto &sel = f.project().axes[0].selection;
    REQUIRE(sel.size() == 1);
    CHECK(sel.contains(ofs::ScriptAxisAction{1.0, 0}));
}

TEST_CASE("Additive selection toggles a single point off then on") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 1.9,
                                   .endTime = 2.1,
                                   .additive = true}); // +2
    CHECK(f.project().axes[0].selection.size() == 1);
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 1.9,
                                   .endTime = 2.1,
                                   .additive = true}); // -2
    CHECK(f.project().axes[0].selection.empty());
}

// ── Delete selected ─────────────────────────────────────────────────────────────
TEST_CASE("RemoveSelectedActions deletes the selected actions") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 1.5,
                                   .endTime = 3.5,
                                   .additive = false});
    f.send(ofs::RemoveSelectedActionsEvent{.axis = StandardAxis::L0});
    auto &acts = f.project().axes[0].actions;
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].at == doctest::Approx(1.0));
    CHECK(f.project().axes[0].selection.empty()); // selection cleared after delete
}

TEST_CASE("RemoveSelectedActions with no selection removes the action nearest the cursor") {
    PmFixture f;
    f.seedL0_3();
    f.project().playback.cursorPos = 2.1; // nearest is 2.0
    f.send(ofs::RemoveSelectedActionsEvent{.axis = StandardAxis::L0});
    auto &acts = f.project().axes[0].actions;
    REQUIRE(acts.size() == 2);
    CHECK_FALSE(acts.contains(ofs::ScriptAxisAction{2.0, 0}));
}

// ── Move selected (position / time) ─────────────────────────────────────────────
TEST_CASE("MoveSelectionPosition shifts every selected action's pos by delta") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    f.send(ofs::MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 10});
    auto &acts = f.project().axes[0].actions;
    CHECK(acts[0].pos == 20);
    CHECK(acts[1].pos == 30);
    CHECK(acts[2].pos == 40);
}

TEST_CASE("MoveSelectionPosition clamps to [0,100]") {
    PmFixture f;
    ofs::VectorSet<ofs::ScriptAxisAction> a;
    a.insert({1.0, 95});
    a.insert({2.0, 5});
    f.send(ofs::CommitAxisActionsEvent{.axis = StandardAxis::L0, .actions = a});
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    f.send(ofs::MoveSelectionPositionEvent{.axis = StandardAxis::L0, .delta = 10});
    auto &acts = f.project().axes[0].actions;
    CHECK(acts[0].pos == 100); // 95 + 10 clamped
    CHECK(acts[1].pos == 15);
}

TEST_CASE("MoveSelectionTime shifts selected times forward and the selection follows") {
    PmFixture f;
    f.seedL0_3();
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    const double firstBefore = f.project().axes[0].actions[0].at;
    f.send(ofs::MoveSelectionTimeEvent{
        .axis = StandardAxis::L0, .direction = ofs::StepDirection::Forward, .seekAfter = false});
    auto &ax = f.project().axes[0];
    CHECK(ax.actions.size() == 3);         // count preserved
    CHECK(ax.actions[0].at > firstBefore); // moved forward by one overlay step
    CHECK(ax.selection.size() == 3);       // selection re-points at the moved actions
}

// ── Selection-mode seam: native fan-out, plugin resolvers, fallback ──────────────
// These exercise SelectIntentRouter directly with plain C-ABI resolver function pointers — no CoreCLR, so
// they run unconditionally (the real-plugin path is covered in the plugins suite). They pin the router
// behaviour the Phase-8 selection seam depends on: per-axis group fan-out, Pass/Drop/Replace, the
// host-owned additive combine below the seam, and weak-reference fallback to native.
namespace {
constexpr size_t kL0 = static_cast<size_t>(StandardAxis::L0);
constexpr size_t kR0 = static_cast<size_t>(StandardAxis::R0);

// Plugin resolvers: a C function pointer the router invokes per editable axis. Each ignores its sink for
// Pass/Drop and emits kept actions for Replace, exactly like the managed trampoline would.
int selPass(void *, const OfsSelectRequest *, OfsEmitSelect, void *) {
    return OfsSelectPass;
}
int selDrop(void *, const OfsSelectRequest *, OfsEmitSelect, void *) {
    return OfsSelectDrop;
}
int selReplace2(void *, const OfsSelectRequest *, OfsEmitSelect emit, void *sink) {
    emit(sink, {2.0, 0});
    return OfsSelectReplace;
}
// Proves per-axis consultation: a different kept action per axis is only possible if the router consulted
// the mode once per axis with that axis substituted into the request. Both times exist on their axis
// (L0 has 2.0, R0 has 1.0), so each resolves to a real selected action.
int selReplacePerAxis(void *, const OfsSelectRequest *in, OfsEmitSelect emit, void *sink) {
    emit(sink, {in->axis == static_cast<int>(StandardAxis::L0) ? 2.0 : 1.0, 0});
    return OfsSelectReplace;
}
// Emits an action whose time names no point — the router must silently ignore it (a mode selects
// existing points, never invents them).
int selReplaceGhostTime(void *, const OfsSelectRequest *, OfsEmitSelect emit, void *sink) {
    emit(sink, {99.0, 0});
    return OfsSelectReplace;
}

void useMode(PmFixture &f, const char *id, OfsSelectFn fn, const char *owner = "FakePlugin") {
    ofs::SelectionModeEntry e;
    e.id = id;
    e.owningPlugin = owner;
    e.onSelect = fn;
    f.send(ofs::RegisterSelectionModeEvent{e});
    f.send(ofs::SetActiveSelectionModeEvent{.id = id});
}

// Seed L0 {1,2,3} + R0 {1,2} and group them with L0 active, so a request naming the active axis fans out.
void seedGroup(PmFixture &f) {
    f.seedL0_3();
    ofs::VectorSet<ofs::ScriptAxisAction> r;
    r.insert({1.0, 10});
    r.insert({2.0, 20});
    f.send(ofs::CommitAxisActionsEvent{.axis = StandardAxis::R0, .actions = r});
    f.project().axes[kR0].showInStrip = true;
    f.project().state.axesGrouping.set(kL0);
    f.project().state.axesGrouping.set(kR0);
}
} // namespace

TEST_CASE("Native selection fans out per-axis across the active edit group") {
    PmFixture f;
    seedGroup(f);
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 1.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.size() == 2); // L0: 2,3 in range
    CHECK(f.project().axes[kR0].selection.size() == 1); // R0: only 2 (it has no point at 3)
}

TEST_CASE("Plugin selection mode Pass selects the native candidates per axis") {
    PmFixture f;
    seedGroup(f);
    useMode(f, "FakePlugin.pass", selPass);
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.size() == 3); // Pass ≡ native: all in range
    CHECK(f.project().axes[kR0].selection.size() == 2);
}

TEST_CASE("Plugin selection mode Drop selects nothing on every axis") {
    PmFixture f;
    seedGroup(f);
    useMode(f, "FakePlugin.drop", selDrop);
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.empty());
    CHECK(f.project().axes[kR0].selection.empty());
}

TEST_CASE("Plugin selection mode Replace is re-consulted per axis with that axis's result") {
    PmFixture f;
    seedGroup(f);
    useMode(f, "FakePlugin.peraxis", selReplacePerAxis);
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    auto &l0 = f.project().axes[kL0].selection;
    auto &r0 = f.project().axes[kR0].selection;
    REQUIRE(l0.size() == 1);
    REQUIRE(r0.size() == 1);
    CHECK(l0.contains(ofs::ScriptAxisAction{2.0, 0})); // L0 kept the time emitted for L0
    CHECK(r0.contains(ofs::ScriptAxisAction{1.0, 0})); // R0 kept the (different) time emitted for R0
}

TEST_CASE("Plugin Replace ignores an emitted time that names no action") {
    PmFixture f;
    f.seedL0_3();
    useMode(f, "FakePlugin.ghost", selReplaceGhostTime);
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.empty()); // 99.0 names no action → silently ignored
}

TEST_CASE("Additive combine is host-owned below a plugin Replace") {
    PmFixture f;
    f.seedL0_3();
    // Pre-select {1.0} natively, then a plugin Replace emitting {2.0} with additive toggles it in, not over.
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 1.5});
    REQUIRE(f.project().axes[kL0].selection.size() == 1);
    useMode(f, "FakePlugin.replace2", selReplace2);
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 0.5,
                                   .endTime = 3.5,
                                   .additive = true});
    CHECK(f.project().axes[kL0].selection.size() == 2); // {1.0} + toggled-in {2.0}
    // A second additive pass over the same {2.0} toggles it back off — the mode never sees the combine.
    f.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                   .axis = StandardAxis::L0,
                                   .startTime = 0.5,
                                   .endTime = 3.5,
                                   .additive = true});
    CHECK(f.project().axes[kL0].selection.size() == 1);
}

TEST_CASE("A dangling selection-mode id falls back to native on project load, preserving the authored id") {
    PmFixture f;
    f.seedL0_3();
    // Mimic loading a file authored with an absent plugin's mode: stored + effective both name it.
    f.project().storedSelectionMode = "ghost.selectmode";
    f.project().activeSelectionMode = "ghost.selectmode";
    f.send(ofs::LoadProjectEvent{});
    CHECK(f.project().activeSelectionMode == ofs::kNativeSelectionModeId); // effective fell back
    CHECK(f.project().storedSelectionMode == "ghost.selectmode");          // authored id preserved
    // The selection still resolves (natively): a box selects every in-range action.
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.size() == 3);
}

TEST_CASE("Unregistering the active selection mode's plugin falls back to native, preserving the authored id") {
    PmFixture f;
    f.seedL0_3();
    useMode(f, "FakePlugin.drop", selDrop); // activate → SetActive also stamps storedSelectionMode
    REQUIRE(f.project().activeSelectionMode == "FakePlugin.drop");
    REQUIRE(f.project().storedSelectionMode == "FakePlugin.drop");

    f.send(ofs::UnregisterSelectionModesEvent{"FakePlugin"});
    CHECK(f.project().activeSelectionMode == ofs::kNativeSelectionModeId); // effective fell back
    CHECK(f.project().storedSelectionMode == "FakePlugin.drop");           // authored id preserved
    // Resolution is native again (the dropped plugin no longer swallows the selection).
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.size() == 3);
}

namespace {
// Register a selection mode without activating it — mimics a plugin loading *after* the project, the
// real startup order (LoadProjectEvent drains on frame 1; loadPlugins() runs in onStartupComplete()).
void registerMode(PmFixture &f, const char *id, OfsSelectFn fn, const char *owner = "FakePlugin") {
    ofs::SelectionModeEntry e;
    e.id = id;
    e.owningPlugin = owner;
    e.onSelect = fn;
    f.send(ofs::RegisterSelectionModeEvent{e});
}
} // namespace

TEST_CASE("Registering the authored selection mode after a load fallback restores it as effective") {
    PmFixture f;
    f.seedL0_3();
    // Project authored with a plugin mode, loaded before that plugin: onProjectLoaded falls back to native.
    f.project().storedSelectionMode = "FakePlugin.drop";
    f.project().activeSelectionMode = "FakePlugin.drop";
    f.send(ofs::LoadProjectEvent{});
    REQUIRE(f.project().activeSelectionMode == ofs::kNativeSelectionModeId);

    // The plugin now loads and registers the authored mode — the deferred restore re-activates it.
    registerMode(f, "FakePlugin.drop", selDrop);
    CHECK(f.project().activeSelectionMode == "FakePlugin.drop");
    CHECK(f.project().storedSelectionMode == "FakePlugin.drop");
    // Resolution now runs through the restored plugin mode (drop ⇒ nothing selected).
    f.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(f.project().axes[kL0].selection.empty());
}

TEST_CASE("Registering a non-authored selection mode after a fallback does not steal activation") {
    PmFixture f;
    f.seedL0_3();
    f.project().storedSelectionMode = "FakePlugin.drop";
    f.project().activeSelectionMode = "FakePlugin.drop";
    f.send(ofs::LoadProjectEvent{});
    REQUIRE(f.project().activeSelectionMode == ofs::kNativeSelectionModeId);

    // A different plugin mode registering must not activate — only the authored id restores.
    registerMode(f, "FakePlugin.other", selPass);
    CHECK(f.project().activeSelectionMode == ofs::kNativeSelectionModeId);
    CHECK(f.project().storedSelectionMode == "FakePlugin.drop"); // authored id still preserved
}

TEST_CASE("An in-session pick survives a late registration of the previously-authored mode") {
    PmFixture f;
    f.seedL0_3();
    f.project().storedSelectionMode = "FakePlugin.drop";
    f.project().activeSelectionMode = "FakePlugin.drop";
    f.send(ofs::LoadProjectEvent{}); // falls back to native (FakePlugin.drop absent)

    // User picks a different, present mode in-session — this moves the stored/authored id.
    useMode(f, "FakePlugin.pass", selPass);
    REQUIRE(f.project().storedSelectionMode == "FakePlugin.pass");

    // The originally-authored plugin loads late: it must NOT override the user's in-session pick.
    registerMode(f, "FakePlugin.drop", selDrop);
    CHECK(f.project().activeSelectionMode == "FakePlugin.pass");
    CHECK(f.project().storedSelectionMode == "FakePlugin.pass");
}
