// Real Roslyn compile test — boots the .NET runtime, compiles an inline C# script through
// Ofs.ScriptHost, and evaluates the resulting Script node end to end.
//
// Requires the .NET runtime plus the staged managed/ assemblies (Ofs.ScriptHost.dll + Roslyn)
// next to the binary (see tests/CMakeLists.txt). .NET is a hard requirement: if the host can't init,
// the test fails — it never silently skips.

#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptProject.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ProcessingSystem.h"
#include "Services/ScriptRegistry.h"
#include "Services/ScriptSystem.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace ofs;
using ofs::test::TestProject;

namespace {

bool drainUntil(EventQueue &eq, const std::function<bool()> &done, int timeoutMs = 20000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        eq.drain();
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    eq.drain();
    return true;
}

ProcessingNodeGraph scriptModGraph(const std::string &file) {
    ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 1;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Script(0 inputs) -> Output(L0). A generator ignores its (absent) input; its signal is grounded at
// the Output by the region Hz (functional) or emitted directly (discrete).
ProcessingNodeGraph scriptGenGraph(const std::string &file) {
    ProcessingNodeGraph g;
    const int sId = g.allocId();
    const int outId = g.allocId();
    ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 0;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0) on pin0, Input(R0) on pin1 -> Script(2 inputs) -> Output(L0). Mirrors the native
// twoInputCombGraph in test_processing_system; the region must carry both roles so the worker seeds
// both sources.
ProcessingNodeGraph scriptCombGraph(const std::string &file) {
    ProcessingNodeGraph g;
    const int inL = g.allocId();
    const int inR = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 2;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = sId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0)/Input(R0)/Input(V0) on pins 0/1/2 -> Script(3 inputs) -> Output(L0). Exercises the >2-input
// script shape end-to-end (the N-way wrapper, ins[2] injection, and the evaluator's N-way fold).
ProcessingNodeGraph script3InputGraph(const std::string &file) {
    ProcessingNodeGraph g;
    const int inL = g.allocId();
    const int inR = g.allocId();
    const int inV = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
    g.nodes.push_back({.id = inV, .type = GraphNodeType::Input, .role = StandardAxis::V0});
    ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 3;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = sId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = inV, .toNode = sId, .toPin = 2});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0) → Script(1 input, 2 outputs); out0 → Output(L0), out1 → Output(R0). Drives the multi-output
// script path end-to-end (named output locals injected, the wrapper's per-output flush, fromPin routing).
ProcessingNodeGraph scriptSplitGraph(const std::string &file) {
    ProcessingNodeGraph g;
    const int inL = g.allocId();
    const int sId = g.allocId();
    const int outL = g.allocId();
    const int outR = g.allocId();
    g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 1;
    s.scriptOutputCount = 2;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outL, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.nodes.push_back({.id = outR, .type = GraphNodeType::Output, .role = StandardAxis::R0});
    g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .fromPin = 0, .toNode = outL, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .fromPin = 1, .toNode = outR, .toPin = 0});
    return g;
}

// Stage a shipped library script as a user file under <pref>/scripts so compileFile() can resolve
// it by name. A file node only resolves user-folder files (the library is otherwise reached through
// embedded nodes), so these library-validation tests fork the shipped source onto disk first — the
// same path a real fork takes. Returns false if the packed source is unavailable in this build.
bool stageLibraryScript(const std::string &fileName) {
    auto source = ofs::res::readText("data/scripts/lib/" + fileName);
    if (!source)
        return false;
    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
    f << *source;
    return true;
}

} // namespace

TEST_CASE("ScriptSystem compiles an inline C# script via Roslyn and evaluates it") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    // Write a functional-modifier script into <prefPath>/scripts (a test temp dir).
    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    const std::string fileName = "e2e_mod.cs";
    {
        std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
        f << "// !ofs:signal functional\n// !ofs:input a\nreturn a + 10f;\n";
    }

    // Compile it.
    tp.eq.push(CompileScriptEvent{fileName});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find(fileName);
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    const CompiledScript *cs = tp.scriptReg.find(fileName);
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.valid());
    CHECK(cs->ref.signal == OfsSignalFunctional);
    CHECK(cs->ref.inputCount == 1);
    CHECK(cs->inputCount == 1);

    // Now evaluate a region whose graph uses the compiled script.
    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptModGraph(fileName);
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);

    bool evaluated = drainUntil(
        tp.eq, [&] { return tp.project.axes[0].pendingEval == nullptr && tp.project.axes[0].resolved.has_value(); });
    REQUIRE(evaluated);

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 35); // 25 + 10, computed by the Roslyn-compiled script
}

TEST_CASE("ScriptSystem injects a // !ofs:param as a named local readable in the body") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    const std::string fileName = "e2e_param.cs";
    {
        // The param 'Boost' is declared in the header and used by name in the body — only possible
        // if the host injected `float Boost = ctx.Param(0, 7f);` ahead of the body.
        std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
        f << "// !ofs:signal functional\n// !ofs:input a\n// !ofs:param Boost 7 0 100\nreturn a + Boost;\n";
    }

    tp.eq.push(CompileScriptEvent{fileName});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find(fileName);
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    const CompiledScript *cs = tp.scriptReg.find(fileName);
    REQUIRE(cs != nullptr);
    REQUIRE(cs->params.size() == 1); // the parsed def reached the registry
    CHECK(cs->params[0].name == "Boost");

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptModGraph(fileName);
    // Override the declared default (7) with a node value of 20 to prove the value is delivered.
    for (auto &n : region.nodeGraph.nodes)
        if (n.type == GraphNodeType::Script)
            n.effect.params = {20.0f};
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);

    bool evaluated = drainUntil(
        tp.eq, [&] { return tp.project.axes[0].pendingEval == nullptr && tp.project.axes[0].resolved.has_value(); });
    REQUIRE(evaluated);

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 45); // 25 + Boost(20), Boost delivered via the node param value
}

TEST_CASE("ScriptSystem injects bool/enum // !ofs:param as typed locals (bool + int index)") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    const std::string fileName = "e2e_boolenum.cs";
    {
        // The body only compiles if `Enabled` is injected as a `bool` (used in a ternary condition) and
        // `Mode` as an `int` (used in integer arithmetic). A float injection for either would be a hard
        // compile error — so a valid artifact + the expected output proves both typed injections.
        std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
        f << "// !ofs:signal functional\n// !ofs:input a\n"
             "// !ofs:param Enabled 1 bool\n"
             "// !ofs:param Mode 0 enum:Half,Double,Triple\n"
             "return Enabled ? a * (Mode + 1) : a;\n";
    }

    tp.eq.push(CompileScriptEvent{fileName});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find(fileName);
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled); // proves the bool/int injections compiled

    const CompiledScript *cs = tp.scriptReg.find(fileName);
    REQUIRE(cs != nullptr);
    REQUIRE(cs->params.size() == 2);
    CHECK(cs->params[0].type == ofs::OfsParamBool);
    CHECK(cs->params[1].type == ofs::OfsParamEnum);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptModGraph(fileName);
    // Enabled = true (1), Mode = "Triple" (index 2) ⇒ a * (2 + 1) = a * 3.
    for (auto &n : region.nodeGraph.nodes)
        if (n.type == GraphNodeType::Script)
            n.effect.params = {1.0f, 2.0f};
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);

    bool evaluated = drainUntil(
        tp.eq, [&] { return tp.project.axes[0].pendingEval == nullptr && tp.project.axes[0].resolved.has_value(); });
    REQUIRE(evaluated);

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 75); // 25 * 3 — Enabled (bool true) gated the int-indexed Mode arithmetic
}

// The trivial native effects were culled to shipped library scripts. These
// compile a library file by name — resolved through ofs::res from data.pak (source-tree fallback in
// tests) — and evaluate it, proving the .cs files are valid C# and reproduce the old effect math.
TEST_CASE("Library script Scale.cs compiles from data/scripts/lib and applies gain + offset") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    // Fork the shipped Scale.cs onto disk: a file node resolves user-folder files only.
    REQUIRE(stageLibraryScript("Scale.cs"));
    tp.eq.push(CompileScriptEvent{"Scale.cs"});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find("Scale.cs");
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptModGraph("Scale.cs");
    for (auto &n : region.nodeGraph.nodes) // gain=2, offset=0
        if (n.type == GraphNodeType::Script)
            n.effect.params = {2.0f, 0.0f};
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 20}); }, tp.eq);

    bool evaluated = drainUntil(
        tp.eq, [&] { return tp.project.axes[0].pendingEval == nullptr && tp.project.axes[0].resolved.has_value(); });
    REQUIRE(evaluated);

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 40); // 20 * 2 + 0
}

// Invert.cs is the one shipped library script with a discrete body, so it's the compile path most
// likely to regress on a stray edit. We assert it compiles and resolves as a discrete modifier; we do
// NOT drive its eval here. (Discrete eval reads the input through the host's discrete-I/O accessors via
// a process-global pointer that is only stable for one long-lived host — the real app has exactly one,
// but this suite spins up many ScriptSystems in sequence, so a second discrete eval would dereference a
// freed pointer. The discrete eval *mechanism* is covered by the native seam in test_native_effects /
// test_processing_system, and Invert's 100-pos math by the Constant-minus-input case there.)
TEST_CASE("Library script Invert.cs compiles from data/scripts/lib as a discrete modifier") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    // Fork the shipped Invert.cs onto disk: a file node resolves user-folder files only.
    REQUIRE(stageLibraryScript("Invert.cs"));
    tp.eq.push(CompileScriptEvent{"Invert.cs"});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find("Invert.cs");
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    const CompiledScript *cs = tp.scriptReg.find("Invert.cs");
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.signal == OfsSignalDiscrete); // Invert.cs declares // !ofs:signal discrete
    CHECK(cs->ref.inputCount == 1);             // // !ofs:input a
    CHECK(cs->params.size() == 1);              // // !ofs:param center
    CHECK(cs->params[0].name == "center");
}

// Blend3.cs is the shipped library script that demonstrates the new >2-input shape (three named
// !ofs:input pins). Compile and evaluate it end-to-end: it proves the wrapper injects ins[2], the three
// weight params wire through, and the evaluator's N-way fold sums all three inputs.
TEST_CASE("Library script Blend3.cs compiles and applies a weighted 3-input blend") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    REQUIRE(stageLibraryScript("Blend3.cs"));
    tp.eq.push(CompileScriptEvent{"Blend3.cs"});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find("Blend3.cs");
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    const CompiledScript *cs = tp.scriptReg.find("Blend3.cs");
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.signal == OfsSignalFunctional);
    CHECK(cs->ref.inputCount == 3); // three named !ofs:input pins — the new >2-input shape
    CHECK(cs->params.size() == 3);  // weightA / weightB / weightC

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::R0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::V0));
    region.nodeGraph = script3InputGraph("Blend3.cs");
    for (auto &n : region.nodeGraph.nodes) // weightA=2, weightB=1, weightC=1 → wsum=4
        if (n.type == GraphNodeType::Script)
            n.effect.params = {2.0f, 1.0f, 1.0f};
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 10}); }, tp.eq);
    tp.project.mutate(StandardAxis::R0, [](AxisState &a) { a.actions.insert({1.0, 40}); }, tp.eq);
    tp.project.mutate(StandardAxis::V0, [](AxisState &a) { a.actions.insert({1.0, 40}); }, tp.eq);

    bool evaluated = drainUntil(
        tp.eq, [&] { return tp.project.axes[0].pendingEval == nullptr && tp.project.axes[0].resolved.has_value(); });
    REQUIRE(evaluated);

    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 25); // (10*2 + 40 + 40) / 4
}

// Mirror.cs is the shipped library script demonstrating the new multi-OUTPUT shape: 1 input, 2 named
// outputs (main / mirror). It compiles via the multi-output wrapper (named output locals + per-output
// flush) and, wired to two axes, drives them from a single input — main passes the stroke through,
// mirror reflects it about Center.
TEST_CASE("Library script Mirror.cs compiles and splits one input across two output axes") {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;

    ScriptSystem scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem);
    ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();

    REQUIRE(scriptSystem.init());
    jobSystem.start();

    REQUIRE(stageLibraryScript("Mirror.cs"));
    tp.eq.push(CompileScriptEvent{"Mirror.cs"});
    bool compiled = drainUntil(tp.eq, [&] {
        const CompiledScript *cs = tp.scriptReg.find("Mirror.cs");
        return cs != nullptr && cs->ref.valid();
    });
    REQUIRE(compiled);

    const CompiledScript *cs = tp.scriptReg.find("Mirror.cs");
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.signal == OfsSignalFunctional);
    CHECK(cs->ref.inputCount == 1);
    CHECK(cs->ref.outputCount == 2); // two named !ofs:output pins
    REQUIRE(cs->outputNames.size() == 2);
    CHECK(cs->outputNames[0] == "main");
    CHECK(cs->outputNames[1] == "mirror");

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::R0));
    region.nodeGraph = scriptSplitGraph("Mirror.cs"); // Center defaults to 50
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
    tp.project.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 30}); }, tp.eq);
    tp.project.mutate(StandardAxis::R0, [](AxisState &a) {}, tp.eq); // trigger R0's eval

    bool lDone = drainUntil(tp.eq, [&] {
        auto &ax = tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
        return ax.pendingEval == nullptr && ax.resolved.has_value();
    });
    REQUIRE(lDone);
    bool rDone = drainUntil(tp.eq, [&] {
        auto &ax = tp.project.axes[static_cast<size_t>(StandardAxis::R0)];
        return ax.pendingEval == nullptr && ax.resolved.has_value();
    });
    REQUIRE(rDone);

    const auto &l = tp.project.axes[static_cast<size_t>(StandardAxis::L0)].resolved->actions;
    const auto &r = tp.project.axes[static_cast<size_t>(StandardAxis::R0)].resolved->actions;
    REQUIRE(l.size() == 1);
    REQUIRE(r.size() == 1);
    CHECK(l[0].pos == 30); // main = stroke
    CHECK(r[0].pos == 70); // mirror = Center*2 - stroke = 100 - 30
}

// ── Generator / combiner / discrete compile+eval shapes ────────────────────────────────────────
// The existing tests above cover the functional-modifier shape. These drive the remaining five
// (signal, inputCount) method shapes through CompileScriptNative's wrapper/delegate switch and the
// matching Ofs.Api eval trampolines (Gen/Comb/DiscreteGen/DiscreteComb). A short fixture removes the
// per-test boilerplate; construction order matches the tests above (systems, freeze, init, start).
namespace {

struct RoslynFixture {
    TestProject tp;
    JobSystem jobSystem;
    EffectRegistryState effectReg;
    ScriptSystem scriptSystem;
    ProcessingSystem ps;
    bool ready = false;
    RoslynFixture()
        : scriptSystem(tp.project, tp.scriptReg, tp.eq, jobSystem),
          ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem) {
        tp.eq.freeze();
        ready = scriptSystem.init();
        if (ready)
            jobSystem.start();
    }
};

void writeScript(const std::string &fileName, const std::string &body) {
    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
    f << body;
}

// Compile a user-folder script and wait for a valid artifact; returns the registry entry (or null).
const CompiledScript *compileScript(RoslynFixture &f, const std::string &fileName) {
    f.tp.eq.push(CompileScriptEvent{fileName});
    drainUntil(f.tp.eq, [&] {
        const CompiledScript *cs = f.tp.scriptReg.find(fileName);
        return cs != nullptr && cs->ref.valid();
    });
    return f.tp.scriptReg.find(fileName);
}

bool evalDone(RoslynFixture &f, StandardAxis role) {
    auto &axis = f.tp.project.axes[static_cast<size_t>(role)];
    return drainUntil(f.tp.eq, [&] { return axis.pendingEval == nullptr && axis.resolved.has_value(); });
}

} // namespace

TEST_CASE("ScriptSystem compiles and evaluates a functional generator (0 inputs)") {
    RoslynFixture f;
    REQUIRE(f.ready);
    const std::string fileName = "e2e_fgen.cs";
    writeScript(fileName, "// !ofs:signal functional\n// !ofs:inputs 0\nreturn 60f;\n");
    const CompiledScript *cs = compileScript(f, fileName);
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.inputCount == 0);
    CHECK(cs->ref.signal == OfsSignalFunctional);
    CHECK(cs->inputCount == 0);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptGenGraph(fileName);
    f.tp.project.regions.push_back(region);
    f.tp.project.sortRegions();

    f.tp.project.axes[0].showInStrip = true;
    f.tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 25}); }, f.tp.eq);

    REQUIRE(evalDone(f, StandardAxis::L0));
    const auto &resolved = f.tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() > 1); // functional generator discretized across [0,10] at the region Hz
    for (const auto &a : resolved)
        CHECK(a.pos == 60); // constant output regardless of t — the seed action is replaced
}

TEST_CASE("ScriptSystem compiles and evaluates a functional combiner (2 inputs)") {
    RoslynFixture f;
    REQUIRE(f.ready);
    const std::string fileName = "e2e_fcomb.cs";
    writeScript(fileName, "// !ofs:signal functional\n// !ofs:input a\n// !ofs:input b\nreturn (a + b) / 2f;\n");
    const CompiledScript *cs = compileScript(f, fileName);
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.inputCount == 2);
    CHECK(cs->ref.signal == OfsSignalFunctional);
    CHECK(cs->inputCount == 2);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::R0));
    region.nodeGraph = scriptCombGraph(fileName);
    f.tp.project.regions.push_back(region);
    f.tp.project.sortRegions();

    // Seed R0 (pin1) before L0 so its action is in the snapshot when L0's eval is dispatched.
    f.tp.project.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;
    f.tp.project.mutate(StandardAxis::R0, [](AxisState &a) { a.actions.insert({1.0, 60}); }, f.tp.eq);
    f.tp.project.axes[0].showInStrip = true;
    f.tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 40}); }, f.tp.eq);

    REQUIRE(evalDone(f, StandardAxis::L0));
    const auto &resolved = f.tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 50); // (a=40 + b=60) / 2, both inputs discrete and aligned at t=1.0
}

TEST_CASE("ScriptSystem compiles and evaluates a discrete generator (0 inputs)") {
    RoslynFixture f;
    REQUIRE(f.ready);
    const std::string fileName = "e2e_dgen.cs";
    writeScript(fileName, "// !ofs:signal discrete\n// !ofs:inputs 0\noutp.Add(2.0, 30);\noutp.Add(5.0, 80);\n");
    const CompiledScript *cs = compileScript(f, fileName);
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.inputCount == 0);
    CHECK(cs->ref.signal == OfsSignalDiscrete);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = scriptGenGraph(fileName);
    f.tp.project.regions.push_back(region);
    f.tp.project.sortRegions();

    f.tp.project.axes[0].showInStrip = true;
    f.tp.project.mutate(StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 25}); }, f.tp.eq);

    REQUIRE(evalDone(f, StandardAxis::L0));
    const auto &resolved = f.tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 2); // exactly the two emitted actions; the seed is replaced
    CHECK(resolved[0].at == doctest::Approx(2.0));
    CHECK(resolved[0].pos == 30);
    CHECK(resolved[1].at == doctest::Approx(5.0));
    CHECK(resolved[1].pos == 80);
}

TEST_CASE("ScriptSystem compiles and evaluates a discrete combiner (2 inputs)") {
    RoslynFixture f;
    REQUIRE(f.ready);
    const std::string fileName = "e2e_dcomb.cs";
    // Reads both DiscreteReaders: iterates a (enumerator), and reads b via Count + indexer.
    writeScript(fileName, "// !ofs:signal discrete\n// !ofs:input a\n// !ofs:input b\n"
                          "int off = b.Count > 0 ? b[0].Pos : 0;\n"
                          "foreach (var x in a) outp.Add(x.At, x.Pos + off);\n");
    const CompiledScript *cs = compileScript(f, fileName);
    REQUIRE(cs != nullptr);
    CHECK(cs->ref.inputCount == 2);
    CHECK(cs->ref.signal == OfsSignalDiscrete);

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.axisRoles.set(static_cast<size_t>(StandardAxis::R0));
    region.nodeGraph = scriptCombGraph(fileName);
    f.tp.project.regions.push_back(region);
    f.tp.project.sortRegions();

    f.tp.project.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;
    f.tp.project.mutate(StandardAxis::R0, [](AxisState &a) { a.actions.insert({1.0, 10}); }, f.tp.eq);
    f.tp.project.axes[0].showInStrip = true;
    f.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 30});
            a.actions.insert({4.0, 70});
        },
        f.tp.eq);

    REQUIRE(evalDone(f, StandardAxis::L0));
    const auto &resolved = f.tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 2); // a's two actions, each lifted by b[0].Pos (10)
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 40);
    CHECK(resolved[1].at == doctest::Approx(4.0));
    CHECK(resolved[1].pos == 80);
}

TEST_CASE("ScriptSystem records a real Roslyn diagnostic for an invalid script") {
    RoslynFixture f;
    REQUIRE(f.ready);
    const std::string fileName = "e2e_bad.cs";
    // A genuine syntax error: the file exists, so it reaches the real Roslyn compile (not the
    // synchronous missing-file placeholder) and comes back with emit diagnostics.
    writeScript(fileName, "// !ofs:signal functional\n// !ofs:input a\nreturn a +;\n");
    f.tp.eq.push(CompileScriptEvent{fileName});
    bool recorded = drainUntil(f.tp.eq, [&] {
        const CompiledScript *cs = f.tp.scriptReg.find(fileName);
        return cs != nullptr && !cs->error.empty();
    });
    REQUIRE(recorded);
    const CompiledScript *cs = f.tp.scriptReg.find(fileName);
    REQUIRE(cs != nullptr);
    CHECK_FALSE(cs->ref.valid());
    CHECK(cs->error.find("CS") != std::string::npos); // a real CSxxxx compiler diagnostic
}

// Each successful Roslyn compile roots a collectible ALC + slot for the process lifetime; the leak
// sweep in refreshByHash releases any artifact no file/embedded node still references. Editing a
// file's content (a fresh hash) orphans the prior artifact, which drives ReleaseScriptNative ->
// Nodes.ReleaseScript (slot null + ALC unload) for both the functional and discrete slot tables.
TEST_CASE("ScriptSystem releases functional and discrete script artifacts orphaned by a content change") {
    RoslynFixture f;
    REQUIRE(f.ready);
    auto orphans = [&](const std::string &fileName, const std::string &v1, const std::string &v2) {
        writeScript(fileName, v1);
        const std::string hashV1 = scriptContentHash(v1);
        REQUIRE(compileScript(f, fileName) != nullptr);
        REQUIRE(f.tp.scriptReg.byHash.contains(hashV1));

        writeScript(fileName, v2);
        f.tp.eq.push(CompileScriptEvent{fileName});
        bool swept = drainUntil(f.tp.eq, [&] {
            const CompiledScript *cs = f.tp.scriptReg.find(fileName);
            // v2 valid AND v1's artifact reclaimed (no file maps to it now -> orphan -> released).
            return cs != nullptr && cs->ref.valid() && !f.tp.scriptReg.byHash.contains(hashV1);
        });
        REQUIRE(swept);
    };

    orphans("e2e_orphan_f.cs", "// !ofs:signal functional\n// !ofs:input a\nreturn a + 1f;\n",
            "// !ofs:signal functional\n// !ofs:input a\nreturn a + 2f;\n");
    orphans("e2e_orphan_d.cs",
            "// !ofs:signal discrete\n// !ofs:input a\nforeach (var x in a) outp.Add(x.At, x.Pos);\n",
            "// !ofs:signal discrete\n// !ofs:input a\nforeach (var x in a) outp.Add(x.At, 100 - x.Pos);\n");
}

// ── onScriptCompiled / refreshByHash (no .NET needed; driven via ScriptCompiledEvent) ──────────
// These ScriptSystem handlers are not gated behind a live .NET host, so a ScriptCompiledEvent pushed
// onto the queue exercises the header-refresh, link-pruning, and re-eval paths without compiling
// anything. They live in the plugin-tests binary only because ScriptSystem.h transitively needs the
// NetHost headers at compile time; nothing here boots the runtime (no init()/drainUntil). The
// releaseScriptNative orphan sweep needs the host and stays null, so it's skipped.
namespace {

void dummyScriptFn() {} // a non-null fnPtr so NodeCallRef::valid() is true; refreshByHash never calls it

NodeCallRef validRef(OfsSignalKind signal, int inputCount) {
    return {.fnPtr = reinterpret_cast<OfsGenericFn>(&dummyScriptFn),
            .userData = nullptr,
            .signal = signal,
            .inputCount = inputCount};
}

// A ScriptSystem over a fresh TestProject, handlers registered but init() never called (no .NET).
struct SsFixture {
    TestProject tp;
    JobSystem jobSystem;
    ScriptSystem ss;
    SsFixture() : ss(tp.project, tp.scriptReg, tp.eq, jobSystem) {}
};

// Region [0,10] over L0 with one embedded Script node (id 1) at `src`/`inputCount`, plus an Input
// (id 2) feeding the script's pin `feedPin` so an arity shrink can prune that link.
ProcessingRegion embeddedScriptRegion(const std::string &src, uint8_t inputCount, int feedPin) {
    ProcessingRegion r;
    r.id = 1;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    ProcessingNodeGraph g;
    ProcessingGraphNode sn;
    sn.id = 1;
    sn.type = GraphNodeType::Script;
    sn.scriptEmbeddedSource = src;
    sn.scriptInputCount = inputCount;
    sn.scriptSignal = 1; // functional
    sn.role = StandardAxis::L0;
    g.nodes.push_back(sn);
    g.nodes.push_back({.id = 2, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.links.push_back({.id = 3, .fromNode = 2, .toNode = 1, .toPin = feedPin}); // Input(2) -> Script(1).pin[feedPin]
    r.nodeGraph = g;
    return r;
}

} // namespace

TEST_CASE("ScriptSystem: a successful compile refreshes the node header, prunes over-arity links, and re-evals") {
    SsFixture f;
    const std::string src = "// !ofs:signal functional\nfloat Eval(double t, NodeContext ctx) => 50;\n";
    f.tp.project.regions.push_back(embeddedScriptRegion(src, /*inputCount=*/2, /*feedPin=*/1));
    ofs::test::EventCapture<AxisModifiedEvent> mods;
    mods.attach(f.tp.eq);
    f.tp.eq.freeze();

    ScriptCompiledEvent e;
    e.ok = true;
    e.hash = scriptContentHash(src);
    e.inputCount = 1; // arity shrinks 2 -> 1
    e.ref = validRef(OfsSignalDiscrete, 1);
    f.tp.eq.push(e);
    f.tp.eq.drain();

    auto &node = f.tp.project.regions[0].nodeGraph.nodes[0];
    CHECK(node.scriptInputCount == 1);
    CHECK(node.scriptSignal == static_cast<uint8_t>(OfsSignalDiscrete));
    // The link feeding pin1 (>= the new arity 1) is pruned; nothing dangles to a hidden pin.
    CHECK(f.tp.project.regions[0].nodeGraph.links.empty());
    CHECK(f.tp.scriptReg.byHash.contains(e.hash));
    bool reEvaledL0 = false;
    for (const auto &m : mods.received)
        if (m.role == StandardAxis::L0)
            reEvaledL0 = true;
    CHECK(reEvaledL0);
}

TEST_CASE("ScriptSystem: an errored compile records the diagnostic but preserves the node's persisted header") {
    SsFixture f;
    const std::string src = "this is not valid C#";
    f.tp.project.regions.push_back(embeddedScriptRegion(src, /*inputCount=*/2, /*feedPin=*/1));
    f.tp.eq.freeze();

    ScriptCompiledEvent e;
    e.ok = false;
    e.error = "CS1002: ; expected";
    e.hash = scriptContentHash(src);
    e.inputCount = 1; // ignored: an invalid ref with no parsed params must not rewrite the header
    f.tp.eq.push(e);
    f.tp.eq.drain();

    auto &node = f.tp.project.regions[0].nodeGraph.nodes[0];
    CHECK(node.scriptInputCount == 2);                          // header preserved (a combiner stays a combiner)
    CHECK(f.tp.project.regions[0].nodeGraph.links.size() == 1); // pin1 link not pruned
    REQUIRE(f.tp.scriptReg.byHash.contains(e.hash));
    CHECK(f.tp.scriptReg.byHash[e.hash].error == "CS1002: ; expected");
    CHECK_FALSE(f.tp.scriptReg.byHash[e.hash].ref.valid());
}

TEST_CASE("ScriptSystem: a successful compile of a file node updates the header via fileToHash") {
    SsFixture f;
    ProcessingRegion r;
    r.id = 1;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    ProcessingGraphNode sn;
    sn.id = 1;
    sn.type = GraphNodeType::Script;
    sn.scriptFile = "lib.cs"; // file node: resolves through fileToHash, not an embedded source hash
    sn.scriptInputCount = 1;
    sn.role = StandardAxis::L0;
    r.nodeGraph.nodes.push_back(sn);
    f.tp.project.regions.push_back(r);
    f.tp.eq.freeze();

    ScriptCompiledEvent e;
    e.ok = true;
    e.fileName = "lib.cs";
    e.hash = "deadbeefdeadbeef";
    e.inputCount = 2;
    e.ref = validRef(OfsSignalFunctional, 2);
    f.tp.eq.push(e);
    f.tp.eq.drain();

    CHECK(f.tp.scriptReg.fileToHash["lib.cs"] == "deadbeefdeadbeef");
    auto &node = f.tp.project.regions[0].nodeGraph.nodes[0];
    CHECK(node.scriptInputCount == 2);
    CHECK(node.scriptSignal == static_cast<uint8_t>(OfsSignalFunctional));
}

TEST_CASE("ScriptSystem: a compile result for no live node records the artifact without a re-eval") {
    SsFixture f;
    const std::string src = "// !ofs:signal functional\n";
    f.tp.project.regions.push_back(embeddedScriptRegion(src, /*inputCount=*/1, /*feedPin=*/0));
    ofs::test::EventCapture<AxisModifiedEvent> mods;
    mods.attach(f.tp.eq);
    f.tp.eq.freeze();

    ScriptCompiledEvent e;
    e.ok = true;
    e.hash = "0000000000000000"; // matches no node's content hash
    e.inputCount = 1;
    e.ref = validRef(OfsSignalFunctional, 1);
    f.tp.eq.push(e);
    f.tp.eq.drain();

    CHECK(f.tp.scriptReg.byHash.contains("0000000000000000")); // artifact still recorded
    CHECK(mods.received.empty());                              // no node matched → no axis touched
}

TEST_CASE("ScriptSystem: notifyScriptFault coalesces repeat faults within the window") {
    SsFixture f;
    ofs::test::EventCapture<NotifyEvent> notes;
    notes.attach(f.tp.eq);
    f.tp.eq.freeze();

    f.ss.notifyScriptFault("node:gen"); // first of this ctx → one toast
    f.ss.notifyScriptFault("node:gen"); // same ctx within the 3s window → suppressed (counted)
    f.ss.notifyScriptFault("node:mod"); // distinct ctx → its own toast
    f.tp.eq.drain();

    CHECK(notes.received.size() == 2); // one toast per distinct fault context
    for (const auto &n : notes.received)
        CHECK(n.level == NotifyLevel::Error);
}

// The following two need a live host (init()) to clear the `ready` gate that fronts compileFile /
// compileEmbedded / onProjectLoaded; they skip cleanly when .NET is unavailable.

TEST_CASE("ScriptSystem: compiling an absent file records a 'script not found' placeholder") {
    TestProject tp;
    JobSystem jobSystem;
    ScriptSystem ss(tp.project, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    REQUIRE(ss.init());
    jobSystem.start();

    // A missing file resolves synchronously to an errored placeholder (no Roslyn job submitted), so the
    // node shows "script not found" rather than an indefinite "compiling…".
    tp.eq.push(CompileScriptEvent{"does_not_exist.cs"});
    tp.eq.drain();

    const std::string hash = "missing:does_not_exist.cs";
    REQUIRE(tp.scriptReg.fileToHash.contains("does_not_exist.cs"));
    CHECK(tp.scriptReg.fileToHash["does_not_exist.cs"] == hash);
    REQUIRE(tp.scriptReg.byHash.contains(hash));
    CHECK_FALSE(tp.scriptReg.byHash[hash].ref.valid());
    CHECK(tp.scriptReg.byHash[hash].error.find("not found") != std::string::npos);
}

TEST_CASE("ScriptSystem: LoadProjectEvent compiles every file and embedded script node in the project") {
    TestProject tp;
    JobSystem jobSystem;
    ScriptSystem ss(tp.project, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    REQUIRE(ss.init());
    jobSystem.start();

    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    const std::string fileName = "load_mod.cs";
    {
        std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
        f << "// !ofs:signal functional\n// !ofs:input a\nreturn a + 5f;\n";
    }
    const std::string embedded = "// !ofs:signal functional\n// !ofs:input a\nreturn a + 9f;\n";

    // One region carrying both a file node and an embedded node — onProjectLoaded must compile both.
    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    ProcessingGraphNode fileNode;
    fileNode.id = 1;
    fileNode.type = GraphNodeType::Script;
    fileNode.scriptFile = fileName;
    fileNode.role = StandardAxis::L0;
    ProcessingGraphNode embNode;
    embNode.id = 2;
    embNode.type = GraphNodeType::Script;
    embNode.scriptEmbeddedSource = embedded;
    embNode.role = StandardAxis::L0;
    region.nodeGraph.nodes.push_back(fileNode);
    region.nodeGraph.nodes.push_back(embNode);
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.eq.push(LoadProjectEvent{});
    const std::string embHash = scriptContentHash(embedded);
    bool done = drainUntil(tp.eq, [&] {
        const CompiledScript *file = tp.scriptReg.find(fileName);
        auto emb = tp.scriptReg.byHash.find(embHash);
        return file && file->ref.valid() && emb != tp.scriptReg.byHash.end() && emb->second.ref.valid();
    });
    REQUIRE(done);
    CHECK(tp.scriptReg.find(fileName)->ref.signal == OfsSignalFunctional);
    CHECK(tp.scriptReg.byHash[embHash].ref.valid());

    std::filesystem::remove(scriptsDir / ofs::util::fromUtf8(fileName));
}

// Regression: a project reopened on startup loads before the deferred scriptSystem->init()
// (onStartupComplete runs after the first frame), so its LoadProjectEvent and per-node compile
// requests are dropped while ready == false. init() must re-scan the already-loaded graph once the
// host is up, otherwise a saved script node never compiles until the user re-adds it.
TEST_CASE("ScriptSystem: init() compiles script nodes from a project loaded before the host was ready") {
    TestProject tp;
    JobSystem jobSystem;
    ScriptSystem ss(tp.project, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    // Mirror the real startup order: the worker pool is running (OfsApp::init) before the deferred
    // scriptSystem->init() (onStartupComplete) compiles the loaded graph.
    jobSystem.start();

    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(scriptsDir, ec);
    const std::string fileName = "startup_mod.cs";
    {
        std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
        f << "// !ofs:signal functional\n// !ofs:input a\nreturn a + 5f;\n";
    }
    const std::string embedded = "// !ofs:signal functional\n// !ofs:input a\nreturn a + 11f;\n";

    ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    ProcessingGraphNode fileNode;
    fileNode.id = 1;
    fileNode.type = GraphNodeType::Script;
    fileNode.scriptFile = fileName;
    fileNode.role = StandardAxis::L0;
    ProcessingGraphNode embNode;
    embNode.id = 2;
    embNode.type = GraphNodeType::Script;
    embNode.scriptEmbeddedSource = embedded;
    embNode.role = StandardAxis::L0;
    region.nodeGraph.nodes.push_back(fileNode);
    region.nodeGraph.nodes.push_back(embNode);
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    // The project's LoadProjectEvent fires before the host is up: it is dropped (ready == false), so this
    // alone leaves the nodes uncompiled — exactly the startup-reorder bug.
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();
    CHECK(tp.scriptReg.find(fileName) == nullptr); // dropped while not ready — nothing compiled yet

    // Now the deferred host init runs; it must pick up the already-loaded graph.
    REQUIRE(ss.init());

    const std::string embHash = scriptContentHash(embedded);
    bool done = drainUntil(tp.eq, [&] {
        const CompiledScript *file = tp.scriptReg.find(fileName);
        auto emb = tp.scriptReg.byHash.find(embHash);
        return file && file->ref.valid() && emb != tp.scriptReg.byHash.end() && emb->second.ref.valid();
    });
    REQUIRE(done);
    CHECK(tp.scriptReg.find(fileName)->ref.signal == OfsSignalFunctional);
    CHECK(tp.scriptReg.byHash[embHash].ref.valid());

    std::filesystem::remove(scriptsDir / ofs::util::fromUtf8(fileName));
}
