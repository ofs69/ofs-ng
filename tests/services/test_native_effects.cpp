// Numeric-correctness unit tests for the built-in (native) effects and node ops registered by
// registerNativeEffects, plus the math/constant graph nodes. Each case builds a minimal graph over a
// single region, pushes a known input through ProcessingSystem, and asserts the resolved output against
// hand-computed expectations. After the cull only the two genuine algorithms remain native (smooth,
// rdp — both EffectKind::Discrete); this exercises that branch plus the Add/Subtract/Multiply/Divide/
// Constant switch arms in the graph evaluator.
#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ProcessingSystem.h"
#include "helpers/TestProject.h"
#include <chrono>
#include <doctest/doctest.h>
#include <thread>

using ofs::GraphNodeType;
using ofs::ProcessingNodeGraph;
using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::TestProject;

namespace {

// Pump the frame loop until the axis settles (mirrors test_processing_system.cpp). ProcessingSystem
// submits from update() and can defer an axis across frames while a prior eval is in flight (the
// throttle), so update() must run every iteration; a null pendingEval after update() means settled.
bool waitForEval(ofs::ProcessingSystem &ps, ofs::EventQueue &eq, const ofs::AxisState &axis, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        ps.update();
        if (axis.pendingEval == nullptr)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        eq.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// One-region eval harness with the native effects registered. run() evaluates `graph` over the
// region [0, dur] for `role`, feeding `input` as the source actions, and returns the resolved set.
struct EvalHarness {
    TestProject tp;
    ofs::EffectRegistryState reg;
    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps;

    EvalHarness() : ps(tp.project, reg, tp.scriptReg, tp.eq, jobSystem) {
        ofs::registerNativeEffects(reg);
        tp.eq.freeze();
        jobSystem.start();
    }

    ofs::VectorSet<ScriptAxisAction> run(const ProcessingNodeGraph &graph,
                                         std::initializer_list<ScriptAxisAction> input,
                                         StandardAxis role = StandardAxis::L0, int hz = 30, double dur = 10.0) {
        ofs::ProcessingRegion region;
        region.id = 1;
        region.startTime = 0.0;
        region.endTime = dur;
        region.hz = hz;
        region.axisRoles.set(static_cast<size_t>(role));
        region.nodeGraph = graph;
        tp.project.regions.push_back(region);
        tp.project.sortRegions();

        auto &axis = tp.project.axes[static_cast<size_t>(role)];
        axis.showInStrip = true;
        tp.project.mutate(
            role,
            [&](ofs::AxisState &a) {
                for (const auto &act : input)
                    a.actions.insert(act);
            },
            tp.eq);
        tp.eq.drain();
        REQUIRE(waitForEval(ps, tp.eq, axis));
        REQUIRE(axis.resolved.has_value());
        return axis.resolved->actions;
    }
};

// Input → Effect(type, params) → Output, all on `role`.
ProcessingNodeGraph singleEffectGraph(const char *type, std::vector<float> params,
                                      StandardAxis role = StandardAxis::L0) {
    ProcessingNodeGraph g;
    const int in = g.allocId();
    const int eff = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = role});
    ofs::ProcessingGraphNode e;
    e.id = eff;
    e.type = GraphNodeType::Effect;
    e.effect.type = type;
    e.effect.params = std::move(params);
    e.role = role;
    g.nodes.push_back(e);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = role});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = eff, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eff, .toNode = out, .toPin = 0});
    return g;
}

// Input(A) and Constant(B) → math op → Output. Input is on pin 0, Constant on pin 1.
ProcessingNodeGraph mathGraph(GraphNodeType op, float constValue, StandardAxis role = StandardAxis::L0) {
    ProcessingNodeGraph g;
    const int in = g.allocId();
    const int con = g.allocId();
    const int m = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = role});
    g.nodes.push_back({.id = con, .type = GraphNodeType::Constant, .constantValue = constValue, .role = role});
    g.nodes.push_back({.id = m, .type = op, .role = role});
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = role});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = m, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = con, .toNode = m, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = m, .toNode = out, .toPin = 0});
    return g;
}

// Constant → Output (no input; the constant fills the whole region after discretization).
ProcessingNodeGraph constantGraph(float value, StandardAxis role = StandardAxis::L0) {
    ProcessingNodeGraph g;
    const int con = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = con, .type = GraphNodeType::Constant, .constantValue = value, .role = role});
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = role});
    g.links.push_back({.id = g.allocId(), .fromNode = con, .toNode = out, .toPin = 0});
    return g;
}

int posAt(const ofs::VectorSet<ScriptAxisAction> &acts, double at) {
    for (const auto &a : acts)
        if (a.at == doctest::Approx(at))
            return a.pos;
    return -1;
}

} // namespace

// ── Discrete effects ──────────────────────────────────────────────────────────

// The trivial effects (invert/scale/mix/noise/ramp/sine/triangle) were culled to library scripts;
// their math is covered by the .NET-gated library-eval tests in
// test_script_compile.cpp. Only the two kept native algorithms (smooth, rdp) are tested here.

TEST_CASE("native smooth averages each interior point with its neighbors") {
    EvalHarness h;
    // One pass: middle = (0 + 90 + 0)/3 = 30; endpoints untouched.
    auto out = h.run(singleEffectGraph("smooth", {1.0f}), {{1.0, 0}, {2.0, 90}, {3.0, 0}});
    REQUIRE(out.size() == 3);
    CHECK(posAt(out, 1.0) == 0);
    CHECK(posAt(out, 2.0) == 30);
    CHECK(posAt(out, 3.0) == 0);
}

TEST_CASE("native smooth is a no-op with fewer than three points") {
    EvalHarness h;
    auto out = h.run(singleEffectGraph("smooth", {3.0f}), {{1.0, 10}, {2.0, 90}});
    REQUIRE(out.size() == 2);
    CHECK(posAt(out, 1.0) == 10);
    CHECK(posAt(out, 2.0) == 90);
}

TEST_CASE("native rdp drops a collinear interior point") {
    EvalHarness h;
    // (0,0)-(1,50)-(2,100) are collinear in (time, pos/100) space → midpoint removed.
    auto out = h.run(singleEffectGraph("rdp", {0.05f}), {{0.0, 0}, {1.0, 50}, {2.0, 100}});
    REQUIRE(out.size() == 2);
    CHECK(posAt(out, 0.0) == 0);
    CHECK(posAt(out, 2.0) == 100);
}

TEST_CASE("native rdp keeps a sharply deviating interior point") {
    EvalHarness h;
    // The spike at t=1 is far off the (0,0)-(2,0) baseline → retained.
    auto out = h.run(singleEffectGraph("rdp", {0.05f}), {{0.0, 0}, {1.0, 100}, {2.0, 0}});
    REQUIRE(out.size() == 3);
    CHECK(posAt(out, 1.0) == 100);
}

TEST_CASE("native rdp terminates on a negative epsilon (clamped to 0)") {
    EvalHarness h;
    // A negative epsilon used to make `maxDist > epsilon` always true; on a collinear span maxIdx never
    // advances, so the [lo, hi] range re-pushed itself forever and spun the worker. Clamping epsilon to 0
    // (plus the maxIdx-advanced guard) must make this finish and behave like a lossless simplification:
    // the collinear midpoint is dropped, endpoints kept.
    auto out = h.run(singleEffectGraph("rdp", {-1.0f}), {{0.0, 0}, {1.0, 50}, {2.0, 100}});
    REQUIRE(out.size() == 2);
    CHECK(posAt(out, 0.0) == 0);
    CHECK(posAt(out, 2.0) == 100);
}

TEST_CASE("native rdp returns the input unchanged for two or fewer points") {
    EvalHarness h;
    auto out = h.run(singleEffectGraph("rdp", {0.5f}), {{0.0, 0}, {2.0, 100}});
    REQUIRE(out.size() == 2);
    CHECK(posAt(out, 0.0) == 0);
    CHECK(posAt(out, 2.0) == 100);
}

// ── Math node ops + Constant ──────────────────────────────────────────────────

TEST_CASE("Add node sums the two inputs at the discrete timestamps") {
    EvalHarness h;
    auto out = h.run(mathGraph(GraphNodeType::Add, 10.0f), {{1.0, 40}});
    REQUIRE(out.size() == 1);
    CHECK(posAt(out, 1.0) == 50); // 40 + 10
}

TEST_CASE("Subtract node computes A minus B") {
    EvalHarness h;
    auto out = h.run(mathGraph(GraphNodeType::Subtract, 10.0f), {{1.0, 40}});
    REQUIRE(out.size() == 1);
    CHECK(posAt(out, 1.0) == 30); // 40 - 10
}

TEST_CASE("Multiply node multiplies A by B and clamps") {
    EvalHarness h;
    auto out = h.run(mathGraph(GraphNodeType::Multiply, 2.0f), {{1.0, 40}});
    REQUIRE(out.size() == 1);
    CHECK(posAt(out, 1.0) == 80); // 40 * 2
}

TEST_CASE("Divide node divides A by B") {
    EvalHarness h;
    auto out = h.run(mathGraph(GraphNodeType::Divide, 2.0f), {{1.0, 40}});
    REQUIRE(out.size() == 1);
    CHECK(posAt(out, 1.0) == 20); // 40 / 2
}

TEST_CASE("Divide node yields zero when the divisor is zero") {
    EvalHarness h;
    auto out = h.run(mathGraph(GraphNodeType::Divide, 0.0f), {{1.0, 40}});
    REQUIRE(out.size() == 1);
    CHECK(posAt(out, 1.0) == 0);
}

TEST_CASE("Constant node fills the region with a fixed value") {
    EvalHarness h;
    auto out = h.run(constantGraph(30.0f), {{1.0, 0}}, StandardAxis::L0, 30, 10.0);
    REQUIRE(out.size() > 2);
    for (const auto &a : out)
        CHECK(a.pos == 30);
}
