#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Format/Funscript.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/PluginNodeIO.h"
#include "Services/ProcessingSystem.h"
#include "helpers/EventCapture.h"
#include "helpers/FixtureCompare.h"
#include "helpers/TestProject.h"
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

using ofs::GraphNodeType;
using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::EventCapture;
using ofs::test::TestProject;

// Helper: spin-drain until axis.pendingEval becomes nullptr (eval job complete).
static bool waitForEval(ofs::EventQueue &eq, const ofs::AxisState &axis, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (axis.pendingEval != nullptr) {
        eq.drain();
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

namespace {
// Fake plugin node functions (the host stores raw C function pointers; no .NET involved). Plugin nodes
// carry no scalar params (their state is a TState), so these bake the offset (+10) as a constant —
// the test exercises the eval pipeline and snapshot wiring, not a param channel.
constexpr float kPluginOffset = 10.0f;
// Functional modifier: output = input + 10.
void pluginFuncOffset(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/, float *outs,
                      int /*outCount*/, void * /*ud*/) {
    outs[0] = ins[0] + kPluginOffset;
}
// Discrete modifier: copy each input action, shifting position by +10. Exercises the
// OfsDiscreteInput/OfsDiscreteOutput buffer wiring the host builds and reads back.
void pluginDiscreteOffset(const ofs::OfsDiscreteInput *const *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/,
                          ofs::OfsDiscreteOutput *const *outs, int /*outCount*/, void * /*ud*/) {
    const ofs::OfsDiscreteInput *in = ins[0];
    ofs::OfsDiscreteOutput *out = outs[0];
    for (size_t i = 0; i < in->times.size(); ++i) {
        out->times.push_back(in->times[i]);
        out->positions.push_back(in->positions[i] + static_cast<int>(kPluginOffset));
    }
}

// A graph: Input(L0) → PluginNode("test.offset", +10) → Output(L0).
ofs::ProcessingNodeGraph offsetNodeGraph() {
    ofs::ProcessingNodeGraph graph;
    const int inId = graph.allocId();
    const int nodeId = graph.allocId();
    const int outId = graph.allocId();
    graph.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode pn;
    pn.id = nodeId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "test.offset";
    pn.role = StandardAxis::L0;
    graph.nodes.push_back(pn);
    graph.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = inId, .toNode = nodeId, .toPin = 0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});
    return graph;
}
} // namespace

TEST_CASE("ProcessingSystem: AxisModifiedEvent with no regions leaves resolved nullopt") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 50}); }, tp.eq);
    tp.eq.drain();

    // No regions — resolved should stay nullopt and no job should be submitted.
    CHECK(tp.project.axes[0].pendingEval == nullptr);
    CHECK_FALSE(tp.project.axes[0].resolved.has_value());
}

TEST_CASE("ProcessingSystem: pass-through graph resolves to source actions") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    // Add a region with a default (Input→Output pass-through) graph for L0.
    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = ofs::buildDefaultGraph(StandardAxis::L0);
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(
        StandardAxis::L0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 50});
            a.actions.insert({5.0, 75});
            a.actions.insert({9.0, 25});
        },
        tp.eq);
    tp.eq.drain();

    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 3);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 50);
    CHECK(resolved[1].at == doctest::Approx(5.0));
    CHECK(resolved[1].pos == 75);
    CHECK(resolved[2].at == doctest::Approx(9.0));
    CHECK(resolved[2].pos == 25);
}

TEST_CASE("ProcessingSystem: Constant-minus-input math graph flips action positions") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::registerNativeEffects(effectReg);
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    // 100 - pos inverts a stroke around 50 — what the (culled, now library-script) `invert` effect did.
    // Build it from kept native nodes: Constant(100) → Subtract pin0, Input → Subtract pin1 → Output.
    ofs::ProcessingNodeGraph graph;
    const int inputId = graph.allocId();
    const int constId = graph.allocId();
    const int subId = graph.allocId();
    const int outputId = graph.allocId();

    graph.nodes.push_back({.id = inputId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    graph.nodes.push_back(
        {.id = constId, .type = GraphNodeType::Constant, .constantValue = 100.0f, .role = StandardAxis::L0});
    graph.nodes.push_back({.id = subId, .type = GraphNodeType::Subtract, .role = StandardAxis::L0});
    graph.nodes.push_back({.id = outputId, .type = GraphNodeType::Output, .role = StandardAxis::L0});

    graph.links.push_back({.id = graph.allocId(), .fromNode = constId, .toNode = subId, .toPin = 0}); // A = 100
    graph.links.push_back({.id = graph.allocId(), .fromNode = inputId, .toNode = subId, .toPin = 1}); // B = input
    graph.links.push_back({.id = graph.allocId(), .fromNode = subId, .toNode = outputId, .toPin = 0});

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = graph;
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    // pos=25 → inverted around 50 → pos=75
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain();

    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));
    REQUIRE(tp.project.axes[0].resolved.has_value());

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 75);
}

TEST_CASE("ProcessingSystem: new AxisModifiedEvent cancels the in-flight job") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::registerNativeEffects(effectReg);
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = ofs::buildDefaultGraph(StandardAxis::L0);
    tp.project.regions.push_back(region);

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 10}); }, tp.eq);
    tp.eq.drain();

    // Immediately push a second mutation — should cancel the first job.
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({2.0, 20}); }, tp.eq);
    tp.eq.drain();

    // Wait for the final eval to complete. The result must reflect the latest state.
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));
    REQUIRE(tp.project.axes[0].resolved.has_value());

    const auto &resolved = tp.project.axes[0].resolved->actions;
    // Both actions should be in resolved (latest snapshot has both).
    bool hasBoth = false;
    for (const auto &a : resolved)
        if (a.at == doctest::Approx(1.0) || a.at == doctest::Approx(2.0))
            hasBoth = true;
    CHECK(hasBoth);
}

// Phase 7 — fixture-based regression: load a multi-axis funscript, evaluate a region that
// inverts L0 and passes R0 through, export the resolved actions, and diff against the expected
// fixture. Catches silent regressions in graph evaluation without a UI test.
TEST_CASE("ProcessingSystem: multi-axis invert region matches expected fixture") {
    auto fs = ofs::Funscript::load(ofs::test::fixturePath("multi_axis_regions.funscript"));
    REQUIRE(fs.has_value());
    auto allAxes = fs->toAllAxes(); // {"L0": ..., "R0": ...}
    REQUIRE(allAxes.contains("L0"));
    REQUIRE(allAxes.contains("R0"));

    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::registerNativeEffects(effectReg);
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    // Region [0,10] over both axes. L0 is inverted around 50 via Constant(100) - Input (the native
    // `invert` effect was culled to a library script); 100 - pos is identical to invert(center=50),
    // so the committed expected fixture still matches. R0 passes through.
    ofs::ProcessingNodeGraph graph;
    const int inL = graph.allocId();
    const int constL = graph.allocId();
    const int subL = graph.allocId();
    const int outL = graph.allocId();
    const int inR = graph.allocId();
    const int outR = graph.allocId();
    graph.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    graph.nodes.push_back(
        {.id = constL, .type = GraphNodeType::Constant, .constantValue = 100.0f, .role = StandardAxis::L0});
    graph.nodes.push_back({.id = subL, .type = GraphNodeType::Subtract, .role = StandardAxis::L0});
    graph.nodes.push_back({.id = outL, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    graph.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    graph.nodes.push_back({.id = outR, .type = GraphNodeType::Output, .role = StandardAxis::R0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = constL, .toNode = subL, .toPin = 0}); // A = 100
    graph.links.push_back({.id = graph.allocId(), .fromNode = inL, .toNode = subL, .toPin = 1});    // B = input
    graph.links.push_back({.id = graph.allocId(), .fromNode = subL, .toNode = outL, .toPin = 0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = inR, .toNode = outR, .toPin = 0});

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::R0));
    region.nodeGraph = graph;
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    const auto l0 = static_cast<size_t>(StandardAxis::L0);
    const auto r0 = static_cast<size_t>(StandardAxis::R0);
    tp.project.axes[l0].showInStrip = true;
    tp.project.axes[r0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [&](ofs::AxisState &a) { a.actions = allAxes["L0"]; }, tp.eq);
    tp.project.mutate(StandardAxis::R0, [&](ofs::AxisState &a) { a.actions = allAxes["R0"]; }, tp.eq);
    tp.eq.drain();

    REQUIRE(waitForEval(tp.eq, tp.project.axes[l0]));
    REQUIRE(waitForEval(tp.eq, tp.project.axes[r0]));
    REQUIRE(tp.project.axes[l0].resolved.has_value());
    REQUIRE(tp.project.axes[r0].resolved.has_value());

    auto out = ofs::Funscript::fromAxes11({
        {"L0", tp.project.axes[l0].resolved->actions},
        {"R0", tp.project.axes[r0].resolved->actions},
    });
    auto outPath = std::filesystem::temp_directory_path() / "ofs_fixture_multi_axis.out.funscript";
    REQUIRE(out.save(outPath));

    std::string diff;
    CHECK_MESSAGE(ofs::test::compareFunscriptFiles(
                      outPath, ofs::test::fixturePath("multi_axis_regions.expected.funscript"), &diff),
                  diff);

    std::filesystem::remove(outPath);
}

// ── EvalCompleteEvent payload + staleness guard ──────────────────────────────

TEST_CASE("ProcessingSystem: EvalCompleteEvent carries the resolved payload") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    EventCapture<ofs::EvalCompleteEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = ofs::buildDefaultGraph(StandardAxis::L0);
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(
        StandardAxis::L0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 50});
            a.actions.insert({5.0, 75});
        },
        tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    REQUIRE(cap.received.size() == 1);
    const auto &e = cap.received.back();
    CHECK(e.role == StandardAxis::L0);
    CHECK(e.hasResult);
    CHECK(e.job != nullptr);
    CHECK(e.evalMs >= 0.0);
    REQUIRE(e.resolvedActions.size() == 2);
    CHECK(e.resolvedActions[0].pos == 50);
    CHECK(e.resolvedActions[1].pos == 75);
}

TEST_CASE("ProcessingSystem: a stale EvalCompleteEvent from a superseded job is discarded") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    auto &axis = tp.project.axes[0];
    axis.showInStrip = true;
    auto current = std::make_shared<ofs::EvalJob>();
    axis.pendingEval = current;
    ofs::VectorSet<ScriptAxisAction> prior;
    prior.insert({1.0, 50});
    axis.resolved = ofs::ResolvedActions{.actions = prior, .evalMs = 1.0};

    // A result tagged with a different (superseded) job must not land.
    auto stale = std::make_shared<ofs::EvalJob>();
    ofs::VectorSet<ScriptAxisAction> bogus;
    bogus.insert({9.0, 99});
    tp.eq.push(ofs::EvalCompleteEvent{
        .role = StandardAxis::L0, .job = stale, .resolvedActions = bogus, .evalMs = 5.0, .hasResult = true});
    tp.eq.drain();

    REQUIRE(axis.resolved.has_value());
    REQUIRE(axis.resolved->actions.size() == 1);
    CHECK(axis.resolved->actions[0].pos == 50); // unchanged
    CHECK(axis.pendingEval == current);         // the in-flight job is left intact
}

TEST_CASE("ProcessingSystem: a matching EvalCompleteEvent applies; hasResult=false clears resolved") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    auto &axis = tp.project.axes[0];
    axis.showInStrip = true;

    auto job = std::make_shared<ofs::EvalJob>();
    axis.pendingEval = job;
    ofs::VectorSet<ScriptAxisAction> acts;
    acts.insert({2.0, 70});
    tp.eq.push(ofs::EvalCompleteEvent{
        .role = StandardAxis::L0, .job = job, .resolvedActions = acts, .evalMs = 3.0, .hasResult = true});
    tp.eq.drain();

    REQUIRE(axis.resolved.has_value());
    REQUIRE(axis.resolved->actions.size() == 1);
    CHECK(axis.resolved->actions[0].pos == 70);
    CHECK(axis.resolved->evalMs == doctest::Approx(3.0));
    CHECK(axis.pendingEval == nullptr); // applied → cleared

    // A hasResult=false completion (no regions) clears resolved.
    auto job2 = std::make_shared<ofs::EvalJob>();
    axis.pendingEval = job2;
    tp.eq.push(ofs::EvalCompleteEvent{
        .role = StandardAxis::L0, .job = job2, .resolvedActions = {}, .evalMs = 0.0, .hasResult = false});
    tp.eq.drain();

    CHECK_FALSE(axis.resolved.has_value());
    CHECK(axis.pendingEval == nullptr);
}

// ── Plugin node evaluation (the host's worker-thread evaluation machinery) ────

TEST_CASE("ProcessingSystem: a plugin functional-modifier node is evaluated") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;

    ofs::PluginNodeEntry entry;
    entry.id = "test.offset";
    entry.displayName = "Offset";
    entry.category = "test";
    entry.signal = ofs::OfsSignalFunctional;
    entry.inputCount = 1;
    entry.fn = reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFuncOffset);
    effectReg.pluginNodes["test.offset"] = entry;
    effectReg.pluginNodeKeys.push_back("test.offset");

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = offsetNodeGraph();
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0)); // timestamp preserved
    CHECK(resolved[0].pos == 35);                  // 25 + offset(10)
}

// Regression for the unload-mid-eval race (phase 4a): plugin nodes now
// resolve into the snapshot (nodeRefs) on the main thread, so a worker never reads
// effectReg.pluginNodes. Disabling a plugin AFTER the eval is dispatched must not affect the
// in-flight job — it reads its own NodeCallRef copy. Before the fix, the worker resolved
// effectReg.pluginNodes itself, so this clear could tear the entry out from under the running job.
TEST_CASE("ProcessingSystem: plugin resolution is snapshot-owned — disabling a plugin after dispatch is safe") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;

    ofs::PluginNodeEntry entry;
    entry.id = "test.offset";
    entry.displayName = "Offset";
    entry.category = "test";
    entry.signal = ofs::OfsSignalFunctional;
    entry.inputCount = 1;
    entry.fn = reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFuncOffset);
    effectReg.pluginNodes["test.offset"] = entry;
    effectReg.pluginNodeKeys.push_back("test.offset");

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = offsetNodeGraph();
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain(); // builds the snapshot (resolving the plugin into nodeRefs) and dispatches the worker

    // Disable the plugin AFTER dispatch. The in-flight worker reads its snapshot-owned ref, never the
    // registry, so the result must still be correct.
    effectReg.pluginNodes.clear();
    effectReg.pluginNodeKeys.clear();

    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));
    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 35); // resolved from the snapshot, not the cleared registry
}

TEST_CASE("ProcessingSystem: a plugin discrete-modifier node is evaluated") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;

    ofs::PluginNodeEntry entry;
    entry.id = "test.offset";
    entry.displayName = "Offset";
    entry.category = "test";
    entry.signal = ofs::OfsSignalDiscrete;
    entry.inputCount = 1;
    entry.fn = reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginDiscreteOffset);
    effectReg.pluginNodes["test.offset"] = entry;
    effectReg.pluginNodeKeys.push_back("test.offset");

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = offsetNodeGraph();
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(
        StandardAxis::L0,
        [](ofs::AxisState &a) {
            a.actions.insert({1.0, 25});
            a.actions.insert({5.0, 40});
        },
        tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 2);
    CHECK(resolved[0].pos == 35); // 25 + 10
    CHECK(resolved[1].pos == 50); // 40 + 10
}

// ── Plugin-node TState capture plumbing (phase 4c) ────────────────────────────
// No .NET: a native fake stands in for the managed codec. It "decodes" a node's nodeState JSON into an
// offset (held by an int handle), the host carries that handle in OfsEvalCtx.stateHandle to the worker,
// and an eval fn reads the offset back through the handle — proving the handle reaches the worker and is
// released on completion / supersede. Mirrors the function-pointer seam scripts use.
namespace {
struct FakeCodec {
    std::mutex mu;
    int nextHandle = 0;
    std::unordered_map<int, int> handleValue;             // handle -> decoded offset
    std::unordered_map<int, std::vector<int>> genHandles; // generation -> its handles
    int totalCaptures = 0;
    int totalReleasedHandles = 0;
    std::set<int> generationsReleased;

    void reset() {
        std::lock_guard lk(mu);
        nextHandle = 0;
        handleValue.clear();
        genHandles.clear();
        totalCaptures = 0;
        totalReleasedHandles = 0;
        generationsReleased.clear();
    }
    int liveHandles() {
        std::lock_guard lk(mu);
        return static_cast<int>(handleValue.size());
    }
};
FakeCodec g_codec;

// Crude offset extractor for a fixture JSON like {"Offset":7}; 0 if absent.
int decodeOffset(const char *json) {
    std::string s = json ? json : "";
    auto p = s.find("Offset");
    if (p == std::string::npos)
        return 0;
    p = s.find(':', p);
    return p == std::string::npos ? 0 : std::atoi(s.c_str() + p + 1);
}

int fakeCapture(int /*slot*/, const char *json, int generation) {
    std::lock_guard lk(g_codec.mu);
    const int h = g_codec.nextHandle++;
    g_codec.handleValue[h] = decodeOffset(json);
    g_codec.genHandles[generation].push_back(h);
    ++g_codec.totalCaptures;
    return h;
}
void fakeRelease(int generation) {
    std::lock_guard lk(g_codec.mu);
    g_codec.generationsReleased.insert(generation);
    auto it = g_codec.genHandles.find(generation);
    if (it == g_codec.genHandles.end())
        return;
    for (int h : it->second) {
        g_codec.handleValue.erase(h);
        ++g_codec.totalReleasedHandles;
    }
    g_codec.genHandles.erase(it);
}

// Functional modifier that reads its captured TState (the offset) via the state handle.
void pluginStateOffset(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx *ctx, float *outs,
                       int /*outCount*/, void * /*ud*/) {
    int off = 0;
    if (ctx->stateHandle >= 0) {
        std::lock_guard lk(g_codec.mu);
        auto it = g_codec.handleValue.find(ctx->stateHandle);
        if (it != g_codec.handleValue.end())
            off = it->second;
    }
    outs[0] = ins[0] + static_cast<float>(off);
}

// A graph Input(L0) → state PluginNode("test.stateoffset", nodeState={"Offset":N}) → Output(L0).
ofs::ProcessingNodeGraph stateOffsetGraph(const char *nodeStateJson) {
    ofs::ProcessingNodeGraph graph;
    const int inId = graph.allocId();
    const int nodeId = graph.allocId();
    const int outId = graph.allocId();
    graph.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode pn;
    pn.id = nodeId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "test.stateoffset";
    pn.nodeState = nodeStateJson;
    pn.role = StandardAxis::L0;
    graph.nodes.push_back(pn);
    graph.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = inId, .toNode = nodeId, .toPin = 0});
    graph.links.push_back({.id = graph.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});
    return graph;
}

// A state-backed plugin entry wired to the fake codec.
ofs::PluginNodeEntry stateOffsetEntry() {
    ofs::PluginNodeEntry entry;
    entry.id = "test.stateoffset";
    entry.displayName = "State Offset";
    entry.category = "test";
    entry.signal = ofs::OfsSignalFunctional;
    entry.inputCount = 1;
    entry.fn = reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginStateOffset);
    entry.hasState = true;
    entry.slot = 0;
    return entry;
}
} // namespace

TEST_CASE("ProcessingSystem: a plugin node's TState handle reaches the worker and is released") {
    g_codec.reset();
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    effectReg.pluginNodes["test.stateoffset"] = stateOffsetEntry();
    effectReg.pluginNodeKeys.push_back("test.stateoffset");
    effectReg.nodeStateCodec = {.capture = &fakeCapture, .release = &fakeRelease};

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = stateOffsetGraph(R"({"Offset":7})");
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 32); // 25 + offset(7), read through the state handle on the worker

    CHECK(g_codec.totalCaptures == 1);
    CHECK(g_codec.liveHandles() == 0); // released on completion — no leak
}

// A node that declares state but whose params are still at their defaults has an EMPTY nodeState (the UI
// only writes nodeState on a detected edit). It must still be captured: the worker needs a real
// stateHandle for a `prepare` factory to memoize its artifact once per region eval instead of rebuilding
// it per output sample. Empty JSON decodes to a default TState, so the capture is well-defined.
TEST_CASE("ProcessingSystem: a hasState node with empty nodeState is still captured") {
    g_codec.reset();
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    effectReg.pluginNodes["test.stateoffset"] = stateOffsetEntry();
    effectReg.pluginNodeKeys.push_back("test.stateoffset");
    effectReg.nodeStateCodec = {.capture = &fakeCapture, .release = &fakeRelease};

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = stateOffsetGraph(""); // default params → empty nodeState
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    CHECK(g_codec.totalCaptures == 1); // captured despite empty nodeState — the bug left this at 0
    CHECK(g_codec.liveHandles() == 0); // and released on completion
}

TEST_CASE("ProcessingSystem: TState captures do not leak across many evals") {
    g_codec.reset();
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    effectReg.pluginNodes["test.stateoffset"] = stateOffsetEntry();
    effectReg.pluginNodeKeys.push_back("test.stateoffset");
    effectReg.nodeStateCodec = {.capture = &fakeCapture, .release = &fakeRelease};

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = stateOffsetGraph(R"({"Offset":3})");
    tp.project.regions.push_back(region);
    tp.project.sortRegions();
    tp.project.axes[0].showInStrip = true;

    constexpr int kEvals = 5;
    for (int i = 0; i < kEvals; ++i) {
        // Each edit dirties the axis → one fresh eval that captures and (on completion) releases.
        tp.project.mutate(StandardAxis::L0, [i](ofs::AxisState &a) { a.actions.insert({1.0 + i, 25}); }, tp.eq);
        tp.eq.drain();
        REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));
    }

    CHECK(g_codec.totalCaptures == kEvals);
    CHECK(g_codec.totalReleasedHandles == kEvals);
    CHECK(g_codec.liveHandles() == 0);
}

// A job superseded before it completes never pushes EvalCompleteEvent, so its captures must be dropped
// on the abandon path. Two evals are queued and drained together: the first is superseded by the second
// in the same drain, so its generation is released by abandonEval, not by completion.
TEST_CASE("ProcessingSystem: superseding an eval releases its TState captures") {
    g_codec.reset();
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    effectReg.pluginNodes["test.stateoffset"] = stateOffsetEntry();
    effectReg.pluginNodeKeys.push_back("test.stateoffset");
    effectReg.nodeStateCodec = {.capture = &fakeCapture, .release = &fakeRelease};

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = stateOffsetGraph(R"({"Offset":5})");
    tp.project.regions.push_back(region);
    tp.project.sortRegions();
    tp.project.axes[0].showInStrip = true;

    // Queue two evals before draining: mutate enqueues AxisModifiedEvent (#1); the explicit request is
    // (#2). Both are main-thread-enqueued before any worker result, so #2 supersedes #1 within one drain.
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.push(ofs::RequestAxisEvalEvent{.role = StandardAxis::L0});
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));

    // Two generations were captured (one per eval); both released — the superseded one via abandonEval,
    // the surviving one via completion — so nothing leaks.
    CHECK(g_codec.totalCaptures == 2);
    CHECK(g_codec.totalReleasedHandles == 2);
    CHECK(g_codec.generationsReleased.size() == 2);
    CHECK(g_codec.liveHandles() == 0);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 30); // 25 + offset(5) from the surviving eval
}

// ── Graph-evaluation branch coverage (math ops, interpolation, effect kinds, guards) ──
// These exercise the worker-thread graph evaluator (evaluateGraphNode / applyMathOp /
// graphSampleToFn / nodeSignalToActions) directly through real eval jobs, using built graphs
// and a couple of in-test EffectDefinitions — no .NET, no UI.
namespace {

using ofs::GraphNodeType;
using ofs::ProcessingNodeGraph;

// A small fixture cutting the construct/freeze/start boilerplate the cases above repeat.
struct PsFixture {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps;
    PsFixture() : ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem) {}

    void start() {
        tp.eq.freeze();
        jobSystem.start();
    }

    void addRegion(const ProcessingNodeGraph &g, std::initializer_list<StandardAxis> roles, double s = 0.0,
                   double e = 10.0) {
        ofs::ProcessingRegion r;
        r.id = static_cast<int>(tp.project.regions.size()) + 1;
        r.startTime = s;
        r.endTime = e;
        for (auto role : roles)
            r.axisRoles.set(static_cast<size_t>(role));
        r.nodeGraph = g;
        tp.project.regions.push_back(r);
        tp.project.sortRegions();
    }

    void seed(StandardAxis role, ofs::VectorSet<ScriptAxisAction> acts) {
        tp.project.axes[static_cast<size_t>(role)].showInStrip = true;
        tp.project.mutate(role, [&](ofs::AxisState &a) { a.actions = std::move(acts); }, tp.eq);
    }

    // Drain, block until `role`'s eval lands, and return its resolved actions.
    const ofs::VectorSet<ScriptAxisAction> &eval(StandardAxis role) {
        tp.eq.drain();
        auto &axis = tp.project.axes[static_cast<size_t>(role)];
        REQUIRE(waitForEval(tp.eq, axis));
        REQUIRE(axis.resolved.has_value());
        return axis.resolved->actions;
    }
};

ofs::VectorSet<ScriptAxisAction> acts(std::initializer_list<ScriptAxisAction> a) {
    return ofs::VectorSet<ScriptAxisAction>(a.begin(), a.end());
}

// Constant(constVal) on pin `constPin`, Input(L0) on the other pin, into `op`, then Output(L0).
ProcessingNodeGraph constOpInputGraph(GraphNodeType op, float constVal, int constPin) {
    ProcessingNodeGraph g;
    const int inId = g.allocId(), cId = g.allocId(), opId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back(
        {.id = cId, .type = GraphNodeType::Constant, .constantValue = constVal, .role = StandardAxis::L0});
    g.nodes.push_back({.id = opId, .type = op, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = opId, .toPin = constPin});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = opId, .toPin = constPin == 0 ? 1 : 0});
    g.links.push_back({.id = g.allocId(), .fromNode = opId, .toNode = outId, .toPin = 0});
    return g;
}

} // namespace

TEST_CASE("ProcessingSystem: applyMathOp covers add, multiply, divide, and divide-by-zero") {
    struct Case {
        GraphNodeType op;
        float constVal;
        int constPin;
        int input;
        int expected;
    };
    // Input(L0) seeded at pos=`input`; Constant on the named pin. Divide is A/B so pin order matters.
    const Case cases[] = {
        {GraphNodeType::Add, 30.f, 0, 20, 50},     // 30 + 20
        {GraphNodeType::Multiply, 2.f, 0, 20, 40}, // 2 * 20
        {GraphNodeType::Divide, 2.f, 1, 40, 20},   // 40 / 2  (input=A on pin0)
        {GraphNodeType::Divide, 0.f, 1, 40, 0},    // 40 / 0  -> guarded to 0
    };
    for (const auto &c : cases) {
        CAPTURE(static_cast<int>(c.op));
        CAPTURE(c.constVal);
        PsFixture f;
        f.addRegion(constOpInputGraph(c.op, c.constVal, c.constPin), {StandardAxis::L0});
        f.start();
        f.seed(StandardAxis::L0, acts({{1.0, c.input}}));
        const auto &r = f.eval(StandardAxis::L0);
        REQUIRE(r.size() == 1);
        CHECK(r[0].pos == c.expected);
    }
}

TEST_CASE("ProcessingSystem: math node interpolates two discrete inputs on the union of their timestamps") {
    // Add Input(L0) + Input(R0) over a 2-axis region. L0 and R0 have staggered timestamps, so each
    // input is sampled (graphSampleToFn: front-clamp, interpolate, back-clamp) at the union times.
    PsFixture f;
    ProcessingNodeGraph g;
    const int inL = g.allocId(), inR = g.allocId(), addId = g.allocId(), outL = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    g.nodes.push_back({.id = addId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outL, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = addId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = addId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = addId, .toNode = outL, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0, StandardAxis::R0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {2.0, 40}}));  // L0(t): 0->0, 1->20(interp), 2->40, 3->40(clamp)
    f.seed(StandardAxis::R0, acts({{1.0, 10}, {3.0, 30}})); // R0(t): 0->10(clamp),1->10,2->20(interp),3->30
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 4); // union {0,1,2,3}
    CHECK(r[0].pos == 10);  // 0 + 10
    CHECK(r[1].pos == 30);  // 20 + 10
    CHECK(r[2].pos == 60);  // 40 + 20
    CHECK(r[3].pos == 70);  // 40 + 30
}

TEST_CASE("ProcessingSystem: a math node with one input unconnected substitutes the 50 midpoint") {
    // Add with pin1 dangling: the missing operand reads as 50, so output = input + 50.
    PsFixture f;
    ProcessingNodeGraph g;
    const int inId = g.allocId(), addId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = addId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back(
        {.id = g.allocId(), .fromNode = inId, .toNode = addId, .toPin = 0}); // pin1 intentionally left unconnected
    g.links.push_back({.id = g.allocId(), .fromNode = addId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 10}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].pos == 60); // 10 + 50 (dangling pin → midpoint)
}

TEST_CASE("ProcessingSystem: a fully functional graph is discretized at the region Hz") {
    // Constant + Constant has no discrete timestamps, so the Add stays functional and the Output
    // discretizes it at the region's 30 Hz. Over [0,10] that's ~301 samples, all 20+30 = 50.
    PsFixture f;
    ProcessingNodeGraph g;
    const int cA = g.allocId(), cB = g.allocId(), addId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cA, .type = GraphNodeType::Constant, .constantValue = 20.f, .role = StandardAxis::L0});
    g.nodes.push_back({.id = cB, .type = GraphNodeType::Constant, .constantValue = 30.f, .role = StandardAxis::L0});
    g.nodes.push_back({.id = addId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cA, .toNode = addId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = cB, .toNode = addId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = addId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}})); // dirties the axis; replaced by the functional output in-region
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100); // Hz-driven discretization of the functional output
    for (const auto &a : r)
        CHECK(a.pos == 50);
}

TEST_CASE("ProcessingSystem: an Effect node with an unknown effect type passes input through") {
    PsFixture f;
    ProcessingNodeGraph g;
    const int inId = g.allocId(), eId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode effectNode;
    effectNode.id = eId;
    effectNode.type = GraphNodeType::Effect;
    effectNode.effect.type = "does.not.exist";
    effectNode.role = StandardAxis::L0;
    g.nodes.push_back(effectNode);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = eId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 75}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 25);
    CHECK(r[1].pos == 75);
}

TEST_CASE("ProcessingSystem: a discrete effect downstream of a functional node is discretized first") {
    // Constant(50) -> smooth -> Output. The Constant is functional, so the discrete `smooth` effect
    // discretizes it at the region Hz before running. A constant stays a constant through smoothing.
    PsFixture f;
    ofs::registerNativeEffects(f.effectReg);
    ProcessingNodeGraph g;
    const int cId = g.allocId(), eId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cId, .type = GraphNodeType::Constant, .constantValue = 50.f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode effectNode;
    effectNode.id = eId;
    effectNode.type = GraphNodeType::Effect;
    effectNode.effect.type = "smooth";
    effectNode.effect.params = {2.f}; // passes
    effectNode.role = StandardAxis::L0;
    g.nodes.push_back(effectNode);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = eId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 50); // smoothing a flat 50 leaves 50
}

// Register a functional EffectDefinition that adds params[0] to its input (or emits a constant
// params[0] when it ignores input). Drives the EffectKind::Functional branches.
static void registerFuncEffects(ofs::EffectRegistryState &reg) {
    {
        ofs::EffectDefinition def;
        def.kind = ofs::EffectKind::Functional;
        def.type = "test.funcadd";
        def.fn = ofs::FuncEffectFn([](const ofs::ActionFn &in, double, double, const float *p, int) -> ofs::ActionFn {
            const float k = p[0];
            return [in, k](double t) { return (in ? in(t) : 50.0f) + k; };
        });
        reg.orderedKeys.push_back(def.type);
        reg.effects.emplace(def.type, std::move(def));
    }
    {
        ofs::EffectDefinition def;
        def.kind = ofs::EffectKind::Functional;
        def.type = "test.funcconst";
        def.ignoresInput = true;
        def.fn = ofs::FuncEffectFn([](const ofs::ActionFn &, double, double, const float *p, int) -> ofs::ActionFn {
            const float v = p[0];
            return [v](double) { return v; };
        });
        reg.orderedKeys.push_back(def.type);
        reg.effects.emplace(def.type, std::move(def));
    }
}

// Input(L0)/Constant(srcConst) -> Effect(type, {param}) -> Output(L0). useConstSrc picks the upstream.
static ProcessingNodeGraph funcEffectGraph(const std::string &type, float param, bool useConstSrc, float srcConst) {
    ProcessingNodeGraph g;
    const int srcId = g.allocId(), eId = g.allocId(), outId = g.allocId();
    if (useConstSrc)
        g.nodes.push_back(
            {.id = srcId, .type = GraphNodeType::Constant, .constantValue = srcConst, .role = StandardAxis::L0});
    else
        g.nodes.push_back({.id = srcId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode effectNode;
    effectNode.id = eId;
    effectNode.type = GraphNodeType::Effect;
    effectNode.effect.type = type;
    effectNode.effect.params = {param};
    effectNode.role = StandardAxis::L0;
    g.nodes.push_back(effectNode);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = srcId, .toNode = eId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eId, .toNode = outId, .toPin = 0});
    return g;
}

TEST_CASE("ProcessingSystem: a functional effect on a discrete input runs at the input timestamps") {
    PsFixture f;
    registerFuncEffects(f.effectReg);
    f.addRegion(funcEffectGraph("test.funcadd", 10.f, /*useConstSrc=*/false, 0.f), {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2); // one output per input timestamp — no Hz grid
    CHECK(r[0].pos == 35);  // 25 + 10
    CHECK(r[1].pos == 50);  // 40 + 10
}

TEST_CASE("ProcessingSystem: a functional effect on a functional input stays functional until the output") {
    PsFixture f;
    registerFuncEffects(f.effectReg);
    f.addRegion(funcEffectGraph("test.funcadd", 5.f, /*useConstSrc=*/true, 40.f), {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100); // functional all the way → discretized at the output Hz
    for (const auto &a : r)
        CHECK(a.pos == 45); // 40 + 5
}

TEST_CASE("ProcessingSystem: a functional generator effect ignores its input") {
    PsFixture f;
    registerFuncEffects(f.effectReg);
    // Input is present but ignoresInput=true, so the output is the flat constant 77.
    f.addRegion(funcEffectGraph("test.funcconst", 77.f, /*useConstSrc=*/false, 0.f), {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 10}, {9.0, 90}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 77);
}

TEST_CASE("ProcessingSystem: a cyclic graph is broken by the in-progress guard and yields no output") {
    // A -> B -> A cycle feeding the Output: the cycle guard returns an empty signal, so the region's
    // in-range action is consumed as input and nothing is emitted back.
    PsFixture f;
    ProcessingNodeGraph g;
    const int aId = g.allocId(), bId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = aId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = bId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = bId, .toNode = aId, .toPin = 0}); // A.pin0 <- B
    g.links.push_back({.id = g.allocId(), .fromNode = aId, .toNode = bId, .toPin = 0}); // B.pin0 <- A  (cycle)
    g.links.push_back({.id = g.allocId(), .fromNode = aId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 50}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.empty()); // cycle produced no output for the region
}

TEST_CASE("ProcessingSystem: a link to a non-existent node resolves to an empty signal") {
    PsFixture f;
    ProcessingNodeGraph g;
    const int outId = g.allocId();
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back(
        {.id = g.allocId(), .fromNode = 9999, .toNode = outId, .toPin = 0}); // fromNode 9999 does not exist
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 50}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.empty()); // dangling link → no output
}

TEST_CASE("ProcessingSystem: LoadProjectEvent re-evaluates every region-driven axis") {
    PsFixture f;
    f.addRegion(constOpInputGraph(GraphNodeType::Add, 5.f, 0), {StandardAxis::L0});
    f.start();
    // Seed actions directly (not via the event path), then let LoadProjectEvent drive the eval: it
    // enqueues an AxisModifiedEvent for each region-driven axis.
    f.tp.project.axes[0].showInStrip = true;
    f.tp.project.axes[0].actions = acts({{1.0, 20}});
    f.tp.eq.push(ofs::LoadProjectEvent{});
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].pos == 25); // 20 + 5 → proves the load kicked off evaluation
}

// ── Plugin combiner-node evaluation (two-input nodes) ─────────────────────────
// Combiners are the one node kind taking two input pins, so they exercise the dual-input
// resolution in evalNodeCallback: the functional path (union-of-timestamps when any input is
// discrete; Hz fallback when both are functional) and the discrete path (sampleOnto grounding
// when one sibling is functional, native grids when both are discrete, Hz when both functional).
namespace {

// Functional combiner: a + b.
void pluginFuncComb(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/, float *outs,
                    int /*outCount*/, void * /*ud*/) {
    outs[0] = ins[0] + ins[1];
}

// Discrete combiner: sum the two index-aligned input grids (the host hands a/b aligned whenever one
// sibling is grounded onto the other, and same-grid discrete inputs line up too).
void pluginDiscreteComb(const ofs::OfsDiscreteInput *const *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/,
                        ofs::OfsDiscreteOutput *const *outs, int /*outCount*/, void * /*ud*/) {
    const ofs::OfsDiscreteInput *a = ins[0];
    const ofs::OfsDiscreteInput *b = ins[1];
    ofs::OfsDiscreteOutput *out = outs[0];
    for (size_t i = 0; i < a->times.size(); ++i) {
        out->times.push_back(a->times[i]);
        const float bv = i < b->positions.size() ? b->positions[i] : 0.0f;
        out->positions.push_back(a->positions[i] + bv);
    }
}

void registerComb(ofs::EffectRegistryState &reg, const char *id, ofs::OfsSignalKind sig) {
    ofs::PluginNodeEntry e;
    e.id = id;
    e.displayName = "Comb";
    e.category = "test";
    e.signal = sig;
    e.inputCount = 2;
    e.fn = sig == ofs::OfsSignalFunctional ? reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFuncComb)
                                           : reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginDiscreteComb);
    reg.pluginNodes[id] = e;
    reg.pluginNodeKeys.push_back(id);
}

ofs::ProcessingGraphNode combNode(int id, const char *nodeId) {
    ofs::ProcessingGraphNode pn;
    pn.id = id;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = nodeId;
    pn.role = StandardAxis::L0;
    return pn;
}

// pin0 = Input(L0), pin1 = Input(R0); plugin combiner → Output(L0).
ProcessingNodeGraph twoInputCombGraph(const char *nodeId) {
    ProcessingNodeGraph g;
    const int inL = g.allocId(), inR = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    g.nodes.push_back(combNode(cId, nodeId));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = cId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = cId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0) on `inputPin`, Constant(constVal) on the other pin; plugin combiner → Output(L0).
ProcessingNodeGraph inputConstCombGraph(const char *nodeId, int inputPin, float constVal) {
    ProcessingNodeGraph g;
    const int inL = g.allocId(), cstId = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back(
        {.id = cstId, .type = GraphNodeType::Constant, .constantValue = constVal, .role = StandardAxis::L0});
    g.nodes.push_back(combNode(cId, nodeId));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = cId, .toPin = inputPin});
    g.links.push_back({.id = g.allocId(), .fromNode = cstId, .toNode = cId, .toPin = inputPin == 0 ? 1 : 0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    return g;
}

// Functional 3-input mix: a + b + c. Exercises the N-way (>2) input fold, not just the old A/B pair.
void pluginFunc3(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/, float *outs,
                 int /*outCount*/, void * /*ud*/) {
    outs[0] = ins[0] + ins[1] + ins[2];
}

// Discrete 3-input mix: sum the three index-aligned input grids onto pin0's timestamps.
void pluginDiscrete3(const ofs::OfsDiscreteInput *const *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/,
                     ofs::OfsDiscreteOutput *const *outs, int /*outCount*/, void * /*ud*/) {
    const ofs::OfsDiscreteInput *a = ins[0];
    ofs::OfsDiscreteOutput *out = outs[0];
    for (size_t i = 0; i < a->times.size(); ++i) {
        const float bv = i < ins[1]->positions.size() ? ins[1]->positions[i] : 0.0f;
        const float cv = i < ins[2]->positions.size() ? ins[2]->positions[i] : 0.0f;
        out->times.push_back(a->times[i]);
        out->positions.push_back(a->positions[i] + bv + cv);
    }
}

void register3(ofs::EffectRegistryState &reg, const char *id, ofs::OfsSignalKind sig) {
    ofs::PluginNodeEntry e;
    e.id = id;
    e.displayName = "Mix3";
    e.category = "test";
    e.signal = sig;
    e.inputCount = 3;
    e.fn = sig == ofs::OfsSignalFunctional ? reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFunc3)
                                           : reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginDiscrete3);
    reg.pluginNodes[id] = e;
    reg.pluginNodeKeys.push_back(id);
}

// pin0 = Input(L0), pin1 = Input(R0), pin2 = Input(V0); plugin 3-input node → Output(L0).
ProcessingNodeGraph threeInputGraph(const char *nodeId) {
    ProcessingNodeGraph g;
    const int inL = g.allocId(), inR = g.allocId(), inV = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    g.nodes.push_back({.id = inV, .type = GraphNodeType::Input, .role = StandardAxis::V0});
    g.nodes.push_back(combNode(cId, nodeId));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = cId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = cId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = inV, .toNode = cId, .toPin = 2});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    return g;
}

// pin0 = Constant(aVal), pin1 = Constant(bVal); plugin combiner → Output(L0).
ProcessingNodeGraph twoConstCombGraph(const char *nodeId, float aVal, float bVal) {
    ProcessingNodeGraph g;
    const int cA = g.allocId(), cB = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cA, .type = GraphNodeType::Constant, .constantValue = aVal, .role = StandardAxis::L0});
    g.nodes.push_back({.id = cB, .type = GraphNodeType::Constant, .constantValue = bVal, .role = StandardAxis::L0});
    g.nodes.push_back(combNode(cId, nodeId));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cA, .toNode = cId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = cB, .toNode = cId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    return g;
}

} // namespace

TEST_CASE("ProcessingSystem: a functional combiner with discrete inputs emits on the union of timestamps") {
    // funcComb(a,b)=a+b. L0/R0 are staggered, so each is graphSampleToFn-sampled at the union times.
    PsFixture f;
    registerComb(f.effectReg, "test.fcomb", ofs::OfsSignalFunctional);
    f.addRegion(twoInputCombGraph("test.fcomb"), {StandardAxis::L0, StandardAxis::R0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {2.0, 40}}));  // 0->0, 1->20(interp), 2->40, 3->40(clamp)
    f.seed(StandardAxis::R0, acts({{1.0, 10}, {3.0, 30}})); // 0->10(clamp), 1->10, 2->20(interp), 3->30
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 4); // union {0,1,2,3}
    CHECK(r[0].pos == 10);  // 0 + 10
    CHECK(r[1].pos == 30);  // 20 + 10
    CHECK(r[2].pos == 60);  // 40 + 20
    CHECK(r[3].pos == 70);  // 40 + 30
}

TEST_CASE("ProcessingSystem: a functional combiner with two functional inputs stays functional until the output") {
    // Constant + Constant → no discrete timestamps, so the combiner stays functional and the Output
    // discretizes it at the region Hz.
    PsFixture f;
    registerComb(f.effectReg, "test.fcomb", ofs::OfsSignalFunctional);
    f.addRegion(twoConstCombGraph("test.fcomb", 20.f, 30.f), {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 50); // 20 + 30
}

TEST_CASE("ProcessingSystem: a discrete combiner with two discrete inputs sums their native grids") {
    PsFixture f;
    registerComb(f.effectReg, "test.dcomb", ofs::OfsSignalDiscrete);
    f.addRegion(twoInputCombGraph("test.dcomb"), {StandardAxis::L0, StandardAxis::R0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    f.seed(StandardAxis::R0, acts({{1.0, 5}, {5.0, 10}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 30); // 25 + 5
    CHECK(r[1].pos == 50); // 40 + 10
}

TEST_CASE("ProcessingSystem: a discrete combiner grounds a functional pin1 onto the discrete pin0 grid") {
    // pin0 = discrete Input(L0), pin1 = functional Constant(10): A's timestamps ground B (sampleOnto).
    PsFixture f;
    registerComb(f.effectReg, "test.dcomb", ofs::OfsSignalDiscrete);
    f.addRegion(inputConstCombGraph("test.dcomb", /*inputPin=*/0, 10.f), {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 35); // 25 + 10
    CHECK(r[1].pos == 50); // 40 + 10
}

TEST_CASE("ProcessingSystem: a discrete combiner grounds a functional pin0 onto the discrete pin1 grid") {
    // Mirror of the above: pin0 = functional Constant(10), pin1 = discrete Input(L0). B's timestamps ground A.
    PsFixture f;
    registerComb(f.effectReg, "test.dcomb", ofs::OfsSignalDiscrete);
    f.addRegion(inputConstCombGraph("test.dcomb", /*inputPin=*/1, 10.f), {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 35); // 10 + 25
    CHECK(r[1].pos == 50); // 10 + 40
}

TEST_CASE("ProcessingSystem: a discrete combiner with two functional inputs falls back to the region Hz") {
    // Both pins functional (Constant+Constant): no discrete grid to borrow, so each is discretized at Hz.
    PsFixture f;
    registerComb(f.effectReg, "test.dcomb", ofs::OfsSignalDiscrete);
    f.addRegion(twoConstCombGraph("test.dcomb", 10.f, 20.f), {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100); // Hz fallback for both functional inputs
    for (const auto &a : r)
        CHECK(a.pos == 30); // 10 + 20
}

TEST_CASE("ProcessingSystem: a functional 3-input node emits on the N-way union of all input timestamps") {
    // func3(a,b,c)=a+b+c over three staggered discrete inputs — proves the >2-input fold and the N-way
    // timestamp union (not just the old A/B merge). Each input is graphSampleToFn-sampled at the union.
    PsFixture f;
    register3(f.effectReg, "test.f3", ofs::OfsSignalFunctional);
    f.addRegion(threeInputGraph("test.f3"), {StandardAxis::L0, StandardAxis::R0, StandardAxis::V0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {3.0, 30}}));  // 0->0,  1->10, 2->20, 3->30
    f.seed(StandardAxis::R0, acts({{1.0, 10}, {2.0, 20}})); // 0->10, 1->10, 2->20, 3->20 (clamped ends)
    f.seed(StandardAxis::V0, acts({{0.0, 5}, {2.0, 25}}));  // 0->5,  1->15, 2->25, 3->25 (clamped tail)
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 4); // union {0,1,2,3}
    CHECK(r[0].pos == 15);  // 0 + 10 + 5
    CHECK(r[1].pos == 35);  // 10 + 10 + 15
    CHECK(r[2].pos == 65);  // 20 + 20 + 25
    CHECK(r[3].pos == 75);  // 30 + 20 + 25
}

TEST_CASE("ProcessingSystem: a discrete 3-input node sums three native grids") {
    // Three discrete inputs on a shared grid: each keeps its own native grid (no Hz), summed index-aligned.
    PsFixture f;
    register3(f.effectReg, "test.d3", ofs::OfsSignalDiscrete);
    f.addRegion(threeInputGraph("test.d3"), {StandardAxis::L0, StandardAxis::R0, StandardAxis::V0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    f.seed(StandardAxis::R0, acts({{1.0, 5}, {5.0, 10}}));
    f.seed(StandardAxis::V0, acts({{1.0, 2}, {5.0, 3}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 32); // 25 + 5 + 2
    CHECK(r[1].pos == 53); // 40 + 10 + 3
}

TEST_CASE("ProcessingSystem: toggling auto-eval clears, halts, then resumes evaluation") {
    PsFixture f;
    f.addRegion(constOpInputGraph(GraphNodeType::Add, 5.f, 0), {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 20}}));
    {
        const auto &r = f.eval(StandardAxis::L0);
        REQUIRE(r.size() == 1);
        CHECK(r[0].pos == 25);
    }
    auto &axis = f.tp.project.axes[0];

    // Halt auto-eval: the processed result is dropped (timeline falls back to raw actions).
    f.tp.eq.push(ofs::SetAutoEvalEnabledEvent{.enabled = false});
    f.tp.eq.drain();
    CHECK_FALSE(f.tp.project.autoEvalEnabled);
    CHECK_FALSE(axis.resolved.has_value());

    // An edit while halted updates actions but does not recompute (onAxisModified early-returns).
    f.seed(StandardAxis::L0, acts({{1.0, 20}, {2.0, 40}}));
    f.tp.eq.drain();
    CHECK(axis.pendingEval == nullptr);
    CHECK_FALSE(axis.resolved.has_value());

    // Re-setting the same value is a no-op.
    f.tp.eq.push(ofs::SetAutoEvalEnabledEvent{.enabled = false});
    f.tp.eq.drain();
    CHECK_FALSE(axis.resolved.has_value());

    // Re-enable: the axis recomputes from the edits made while halted.
    f.tp.eq.push(ofs::SetAutoEvalEnabledEvent{.enabled = true});
    f.tp.eq.drain(); // process the toggle so it submits the eval job (sets pendingEval)
    REQUIRE(waitForEval(f.tp.eq, axis));
    REQUIRE(axis.resolved.has_value());
    const auto &r2 = axis.resolved->actions;
    REQUIRE(r2.size() == 2);
    CHECK(r2[0].pos == 25); // 20 + 5
    CHECK(r2[1].pos == 45); // 40 + 5
}

// ── Remaining evaluator branches: shared-node caching, functional plugin paths with a
// missing sibling, an Output-less graph, an unresolved plugin node, and out-of-region actions. ──
namespace {

// Register the functional offset modifier (input + 10) into an arbitrary EffectRegistryState.
void registerOffsetMod(ofs::EffectRegistryState &reg) {
    ofs::PluginNodeEntry e;
    e.id = "test.offset";
    e.displayName = "Offset";
    e.category = "test";
    e.signal = ofs::OfsSignalFunctional;
    e.inputCount = 1;
    e.fn = reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFuncOffset);
    reg.pluginNodes["test.offset"] = e;
    reg.pluginNodeKeys.push_back("test.offset");
}

} // namespace

TEST_CASE("ProcessingSystem: a node feeding two pins of one consumer is evaluated once and cached") {
    // Input(L0) wired to BOTH pins of an Add: resolving pin1 returns the pin0 result from the eval
    // cache rather than re-walking the Input — output = input + input.
    PsFixture f;
    ProcessingNodeGraph g;
    const int inId = g.allocId(), addId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = addId, .type = GraphNodeType::Add, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = addId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = addId, .toPin = 1}); // same source on both pins
    g.links.push_back({.id = g.allocId(), .fromNode = addId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 20}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].pos == 40); // 20 + 20
}

TEST_CASE("ProcessingSystem: a plugin functional modifier on a functional input stays functional to the output") {
    // Constant(40) → offset(+10) → Output. The input never grounds to a discrete grid, so the
    // modifier composes functionally and the Output discretizes the whole thing at the region Hz.
    PsFixture f;
    registerOffsetMod(f.effectReg);
    ProcessingNodeGraph g;
    const int cId = g.allocId(), pId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cId, .type = GraphNodeType::Constant, .constantValue = 40.f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode pn;
    pn.id = pId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "test.offset";
    pn.role = StandardAxis::L0;
    g.nodes.push_back(pn);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = pId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = pId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 50); // 40 + 10
}

TEST_CASE("ProcessingSystem: a functional combiner with both pins unconnected substitutes 50 for each") {
    // No links into either pin: both operands read as the 50 midpoint, so the combiner emits 50+50,
    // clamped to 100, discretized at the region Hz.
    PsFixture f;
    registerComb(f.effectReg, "test.fcomb", ofs::OfsSignalFunctional);
    ProcessingNodeGraph g;
    const int cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back(combNode(cId, "test.fcomb"));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back(
        {.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0}); // both combiner input pins left dangling
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}})); // dirties the axis; replaced by the functional output
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 100); // 50 + 50, clamped
}

TEST_CASE("ProcessingSystem: a functional combiner with a discrete pin and a dangling pin fills 50 for the gap") {
    // pin0 = discrete Input(L0), pin1 unconnected: the union timestamps come from pin0; the missing
    // pin1 reads as 50 at each, so output = input + 50.
    PsFixture f;
    registerComb(f.effectReg, "test.fcomb", ofs::OfsSignalFunctional);
    ProcessingNodeGraph g;
    const int inId = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back(combNode(cId, "test.fcomb"));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = cId, .toPin = 0}); // pin1 dangling
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 75); // 25 + 50
    CHECK(r[1].pos == 90); // 40 + 50
}

TEST_CASE("ProcessingSystem: a plugin functional modifier with no input substitutes the 50 midpoint") {
    // The modifier's lone pin is unconnected, so its operand reads as 50 and the output is the flat
    // 50 + 10, discretized at the region Hz.
    PsFixture f;
    registerOffsetMod(f.effectReg);
    ProcessingNodeGraph g;
    const int pId = g.allocId(), outId = g.allocId();
    ofs::ProcessingGraphNode pn;
    pn.id = pId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "test.offset";
    pn.role = StandardAxis::L0;
    g.nodes.push_back(pn);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back(
        {.id = g.allocId(), .fromNode = pId, .toNode = outId, .toPin = 0}); // modifier input pin left dangling
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.size() > 100);
    for (const auto &a : r)
        CHECK(a.pos == 60); // 50 + 10
}

TEST_CASE("ProcessingSystem: a functional combiner with a dangling pin and a discrete pin fills 50 for the gap") {
    // Mirror of the discrete-pin0 case: pin0 unconnected, pin1 = discrete Input(L0). The union
    // timestamps come from pin1; the missing pin0 reads as 50, so output = 50 + input.
    PsFixture f;
    registerComb(f.effectReg, "test.fcomb", ofs::OfsSignalFunctional);
    ProcessingNodeGraph g;
    const int inId = g.allocId(), cId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back(combNode(cId, "test.fcomb"));
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = cId, .toPin = 1}); // pin0 dangling
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 75); // 50 + 25
    CHECK(r[1].pos == 90); // 50 + 40
}

TEST_CASE("ProcessingSystem: a graph with no Output node leaves the source actions unchanged") {
    // An Input-only graph produces no Output result, so the evaluator falls back to the axis's own
    // in-region actions verbatim.
    PsFixture f;
    ProcessingNodeGraph g;
    const int inId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    f.addRegion(g, {StandardAxis::L0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 75}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 25);
    CHECK(r[1].pos == 75);
}

TEST_CASE("ProcessingSystem: a plugin node whose id is not registered produces no output") {
    // The snapshot builder skips an unresolved plugin id, so the worker finds no NodeCallRef and the
    // node yields an empty signal (a plugin node never passes its input through) — the in-region
    // action is consumed and nothing is emitted.
    PsFixture f; // effectReg has no "test.offset" registered
    ProcessingNodeGraph g;
    const int inId = g.allocId(), pId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode pn;
    pn.id = pId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "test.offset"; // never added to effectReg
    pn.role = StandardAxis::L0;
    g.nodes.push_back(pn);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = pId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = pId, .toNode = outId, .toPin = 0});
    f.addRegion(g, {StandardAxis::L0}, 0.0, 10.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{5.0, 50}}));
    const auto &r = f.eval(StandardAxis::L0);
    CHECK(r.empty());
}

TEST_CASE("ProcessingSystem: actions outside every driving region are preserved verbatim") {
    // Two regions: A[2,5] drives L0 (+5), B[6,9] drives R0 only. Evaluating L0 must skip region B (it
    // doesn't drive L0) and pass through the actions before A, between regions, and after B untouched.
    PsFixture f;
    f.addRegion(constOpInputGraph(GraphNodeType::Add, 5.f, 0), {StandardAxis::L0}, 2.0, 5.0);
    f.addRegion(ofs::buildDefaultGraph(StandardAxis::R0), {StandardAxis::R0}, 6.0, 9.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 10}, {3.0, 20}, {7.0, 30}, {10.0, 40}}));
    const auto &r = f.eval(StandardAxis::L0);
    REQUIRE(r.size() == 4);
    CHECK(r[0].pos == 10); // before region A — verbatim
    CHECK(r[1].pos == 25); // in region A — 20 + 5
    CHECK(r[2].pos == 30); // within region B's span but B drives R0, not L0 — verbatim
    CHECK(r[3].pos == 40); // after every region — verbatim
}

// Input(L0) → Discretize(keepActions, nodeHz) → Output(L0). params[0] = keep-actions, params[1] = Hz.
static ProcessingNodeGraph discretizeGraph(bool keepActions, int nodeHz) {
    ProcessingNodeGraph g;
    const int inId = g.allocId(), discId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode disc;
    disc.id = discId;
    disc.type = GraphNodeType::Discretize;
    disc.role = StandardAxis::L0;
    disc.effect.params = {keepActions ? 1.0f : 0.0f, static_cast<float>(nodeHz)};
    g.nodes.push_back(disc);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = discId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = discId, .toNode = outId, .toPin = 0});
    return g;
}

// A region [0,4] with the node sampling at a coarse 1 Hz resamples onto the grid t = 0,1,2,3,4. The
// input's 100 peak sits at t=1.5, squarely between two grid samples — the "keep actions" case.
TEST_CASE("ProcessingSystem: Discretize keeps off-grid input peaks when anti-aliasing is on") {
    PsFixture f;
    f.addRegion(discretizeGraph(/*keepActions=*/true, /*nodeHz=*/1), {StandardAxis::L0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {1.5, 100}, {4.0, 0}}));
    const auto &r = f.eval(StandardAxis::L0);

    // The peak survives at its exact original time, on top of the uniform grid.
    bool peakKept = false;
    for (const auto &a : r)
        if (a.at == doctest::Approx(1.5) && a.pos == 100)
            peakKept = true;
    CHECK(peakKept);
    CHECK(r.size() == 6); // grid {0,1,2,3,4} + the merged-in 1.5
}

TEST_CASE("ProcessingSystem: Discretize aliases away off-grid peaks when anti-aliasing is off") {
    PsFixture f;
    f.addRegion(discretizeGraph(/*keepActions=*/false, /*nodeHz=*/1), {StandardAxis::L0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {1.5, 100}, {4.0, 0}}));
    const auto &r = f.eval(StandardAxis::L0);

    // Only the uniform grid remains; the 1.5 peak is gone and the grid never reaches 100.
    REQUIRE(r.size() == 5);
    int maxPos = 0;
    for (const auto &a : r) {
        CHECK_FALSE(a.at == doctest::Approx(1.5));
        maxPos = std::max(maxPos, a.pos);
    }
    CHECK(maxPos == 80); // interpolated grid sample at t=2, the closest the resample gets to the peak
}

// A purely functional input (Constant) has no discrete action times to keep, so Discretize falls back
// to a plain uniform resample regardless of the toggle — the grid is filled at the region's Hz.
TEST_CASE("ProcessingSystem: Discretize resamples a functional input onto the Hz grid") {
    PsFixture f;
    ProcessingNodeGraph g;
    const int cId = g.allocId(), discId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cId, .type = GraphNodeType::Constant, .constantValue = 60.0f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode disc;
    disc.id = discId;
    disc.type = GraphNodeType::Discretize;
    disc.role = StandardAxis::L0;
    disc.effect.params = {1.0f, 1.0f}; // keep-actions on, sample at 1 Hz
    g.nodes.push_back(disc);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = discId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = discId, .toNode = outId, .toPin = 0});

    f.addRegion(g, {StandardAxis::L0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{2.0, 99}})); // in-region; consumed as source, replaced by the resample
    const auto &r = f.eval(StandardAxis::L0);

    REQUIRE(r.size() == 5); // grid {0,1,2,3,4}, every sample the constant value
    for (const auto &a : r)
        CHECK(a.pos == 60);
}

// The node's own Hz drives sampling, not the region's. Region keeps its default 30 Hz; the node samples
// a Constant(60) over [0,4] at 2 Hz → grid t = 0,0.5,…,3.5 + endpoint 4 = 9 samples (not the ~120 the
// region rate would give).
TEST_CASE("ProcessingSystem: Discretize samples at its own Hz, overriding the region rate") {
    PsFixture f;
    ProcessingNodeGraph g;
    const int cId = g.allocId(), discId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = cId, .type = GraphNodeType::Constant, .constantValue = 60.0f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode disc;
    disc.id = discId;
    disc.type = GraphNodeType::Discretize;
    disc.role = StandardAxis::L0;
    disc.effect.params = {1.0f, 2.0f}; // 2 Hz, while the region stays at its default 30 Hz
    g.nodes.push_back(disc);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = cId, .toNode = discId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = discId, .toNode = outId, .toPin = 0});

    f.addRegion(g, {StandardAxis::L0}, 0.0, 4.0);
    REQUIRE(f.tp.project.regions[0].hz == 30); // region default left untouched
    f.start();
    f.seed(StandardAxis::L0, acts({{2.0, 99}}));
    const auto &r = f.eval(StandardAxis::L0);

    CHECK(r.size() == 9); // 2 Hz over [0,4], not the region's 30 Hz
    for (const auto &a : r)
        CHECK(a.pos == 60);
}

// Functionalize marks a discrete input continuous, so the Output's single discretization point
// re-samples it at the region Hz. A sparse discrete input {0:0, 2:100} would otherwise reach the
// Output verbatim (2 actions); routed through Functionalize over [0,2] at a region rate of 2 Hz it
// becomes the interpolated grid t = 0,0.5,1,1.5,2 → 0,25,50,75,100.
TEST_CASE("ProcessingSystem: Functionalize defers discretization to the region Hz") {
    PsFixture f;
    ProcessingNodeGraph g;
    const int inId = g.allocId(), funcId = g.allocId(), outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = funcId, .type = GraphNodeType::Functionalize, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = funcId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = funcId, .toNode = outId, .toPin = 0});

    f.addRegion(g, {StandardAxis::L0}, 0.0, 2.0);
    f.tp.project.regions[0].hz = 2; // coarse region rate so the resampled grid is easy to enumerate
    f.start();
    f.seed(StandardAxis::L0, acts({{0.0, 0}, {2.0, 100}}));
    const auto &r = f.eval(StandardAxis::L0);

    REQUIRE(r.size() == 5); // grid {0,0.5,1,1.5,2}, not the 2 sparse input actions
    CHECK(r[0].pos == 0);
    CHECK(r[1].pos == 25);
    CHECK(r[2].pos == 50);
    CHECK(r[3].pos == 75);
    CHECK(r[4].pos == 100);
}

// ── Multi-output nodes (1 → 2 outputs; fromPin selects the source slot) ────────────────────────
namespace {

// 1 input → 2 outputs: out0 passes the input through, out1 reflects it about 100 (an opposing axis).
void pluginFuncSplit(double, const float *ins, int, const ofs::OfsEvalCtx *, float *outs, int, void *) {
    outs[0] = ins[0];
    outs[1] = 100.0f - ins[0];
}
void pluginDiscreteSplit(const ofs::OfsDiscreteInput *const *ins, int, const ofs::OfsEvalCtx *,
                         ofs::OfsDiscreteOutput *const *outs, int, void *) {
    const ofs::OfsDiscreteInput *a = ins[0];
    for (size_t i = 0; i < a->times.size(); ++i) {
        outs[0]->times.push_back(a->times[i]);
        outs[0]->positions.push_back(a->positions[i]);
        outs[1]->times.push_back(a->times[i]);
        outs[1]->positions.push_back(100.0f - a->positions[i]);
    }
}
void registerSplit(ofs::EffectRegistryState &reg, const char *id, ofs::OfsSignalKind sig) {
    ofs::PluginNodeEntry e;
    e.id = id;
    e.displayName = "Split";
    e.category = "test";
    e.signal = sig;
    e.inputCount = 1;
    e.outputCount = 2;
    e.fn = sig == ofs::OfsSignalFunctional ? reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginFuncSplit)
                                           : reinterpret_cast<ofs::OfsNodeEvalFn>(&pluginDiscreteSplit);
    reg.pluginNodes[id] = e;
    reg.pluginNodeKeys.push_back(id);
}
// Input(L0) → Split; out0 → Output(L0), out1 → Output(R0). The two output links carry distinct fromPins,
// so a single node fans two independently-computed signals out to two axes.
ProcessingNodeGraph splitGraph(const char *nodeId) {
    ProcessingNodeGraph g;
    const int inL = g.allocId(), sId = g.allocId(), outL = g.allocId(), outR = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode sn = combNode(sId, nodeId);
    sn.pluginInputCount = 1;
    sn.pluginOutputCount = 2;
    g.nodes.push_back(sn);
    g.nodes.push_back({.id = outL, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outR, .type = GraphNodeType::Output, .role = StandardAxis::R0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .fromPin = 0, .toNode = outL, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .fromPin = 1, .toNode = outR, .toPin = 0});
    return g;
}

} // namespace

TEST_CASE("ProcessingSystem: a functional 1->2 splitter drives two axes from one input via fromPin") {
    PsFixture f;
    registerSplit(f.effectReg, "test.split", ofs::OfsSignalFunctional);
    f.addRegion(splitGraph("test.split"), {StandardAxis::L0, StandardAxis::R0}, 0.0, 4.0);
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 30}}));
    f.seed(StandardAxis::R0, acts({})); // trigger R0's eval; its value rides L0's input through out1
    const auto &l = f.eval(StandardAxis::L0);
    REQUIRE(l.size() == 1);
    CHECK(l[0].pos == 30); // out0 passes the input through
    const auto &r = f.eval(StandardAxis::R0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].pos == 70); // out1 = 100 - 30
}

TEST_CASE("ProcessingSystem: a discrete 1->2 splitter writes independent output grids per pin") {
    PsFixture f;
    registerSplit(f.effectReg, "test.dsplit", ofs::OfsSignalDiscrete);
    f.addRegion(splitGraph("test.dsplit"), {StandardAxis::L0, StandardAxis::R0});
    f.start();
    f.seed(StandardAxis::L0, acts({{1.0, 25}, {5.0, 40}}));
    f.seed(StandardAxis::R0, acts({}));
    const auto &l = f.eval(StandardAxis::L0);
    REQUIRE(l.size() == 2);
    CHECK(l[0].pos == 25);
    CHECK(l[1].pos == 40);
    const auto &r = f.eval(StandardAxis::R0);
    REQUIRE(r.size() == 2);
    CHECK(r[0].pos == 75); // 100 - 25
    CHECK(r[1].pos == 60); // 100 - 40
}
