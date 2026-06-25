#include <doctest/doctest.h>

#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/PluginNodeIO.h"
#include "Services/ProcessingSystem.h"
#include "Services/ScriptHeader.h"
#include "Services/ScriptRegistry.h"
#include "Services/ScriptWatch.h"
#include "helpers/TestProject.h"

#include <chrono>
#include <thread>

using ofs::GraphNodeType;
using ofs::ScriptAxisAction;
using ofs::StandardAxis;
using ofs::test::TestProject;

// ── Header parser (pure, no .NET) ─────────────────────────────────────────────

TEST_CASE("parseScriptHeader: reads signal and a named input directive") {
    auto h = ofs::parseScriptHeader("// !ofs:signal discrete\n// !ofs:input speed\nfloat acc = 0;\n");
    CHECK(h.hasSignal);
    CHECK(h.signal == ofs::OfsSignalDiscrete);
    CHECK(h.hasInputs);
    CHECK(h.inputCount() == 1);
    CHECK(h.inputNames[0] == "speed");
    CHECK(h.outputCount() == 1); // a single implicit output
    CHECK(h.outputNames[0] == "out");
}

TEST_CASE("parseScriptHeader: functional + 2 named inputs, leading whitespace and trailing comment") {
    auto h = ofs::parseScriptHeader("   // !ofs:signal functional   // a comment\n"
                                    "// !ofs:input a\n// !ofs:input b\n");
    CHECK(h.hasSignal);
    CHECK(h.signal == ofs::OfsSignalFunctional);
    CHECK(h.hasInputs);
    CHECK(h.inputCount() == 2);
    CHECK(h.inputNames[0] == "a");
    CHECK(h.inputNames[1] == "b");
}

TEST_CASE("parseScriptHeader: named outputs make a multi-output node") {
    auto h = ofs::parseScriptHeader("// !ofs:signal functional\n"
                                    "// !ofs:input stroke\n"
                                    "// !ofs:output main\n// !ofs:output mirror\n");
    CHECK(h.inputCount() == 1);
    CHECK(h.hasOutputs);
    CHECK(h.outputCount() == 2);
    CHECK(h.outputNames[0] == "main");
    CHECK(h.outputNames[1] == "mirror");
}

TEST_CASE("parseScriptHeader: generator (no input directive) and case-insensitive value") {
    auto h = ofs::parseScriptHeader("// !ofs:signal DISCRETE\n");
    CHECK(h.signal == ofs::OfsSignalDiscrete);
    CHECK_FALSE(h.hasInputs);
    CHECK(h.inputCount() == 0); // no !ofs:input line ⇒ a generator
}

TEST_CASE("parseScriptHeader: inputs/outputs accept more than two, up to the per-direction cap") {
    std::string src;
    for (int i = 0; i < 3; ++i)
        src += "// !ofs:input in" + std::to_string(i) + "\n";
    auto three = ofs::parseScriptHeader(src);
    CHECK(three.hasInputs);
    CHECK(three.inputCount() == 3);

    std::string capSrc, overSrc;
    for (int i = 0; i < 16; ++i)
        capSrc += "// !ofs:input p" + std::to_string(i) + "\n";
    overSrc = capSrc + "// !ofs:input p16\n"; // a 17th input over the 16-pin cap → dropped
    CHECK(ofs::parseScriptHeader(capSrc).inputCount() == 16);
    CHECK(ofs::parseScriptHeader(overSrc).inputCount() == 16);
}

TEST_CASE("parseScriptHeader: a duplicate pin name keeps the first") {
    auto h = ofs::parseScriptHeader("// !ofs:input a\n// !ofs:input a\n// !ofs:input b\n");
    CHECK(h.inputCount() == 2);
    CHECK(h.inputNames[0] == "a");
    CHECK(h.inputNames[1] == "b");
}

TEST_CASE("parseScriptHeader: missing directives leave defaults with hasX=false") {
    auto h = ofs::parseScriptHeader("float Eval(double t, NodeContext ctx) => 50;\n");
    CHECK_FALSE(h.hasSignal);
    CHECK_FALSE(h.hasInputs);
    CHECK_FALSE(h.hasOutputs);
    CHECK(h.signal == ofs::OfsSignalFunctional); // default
    CHECK(h.inputCount() == 0);                  // no input directive ⇒ generator
    CHECK(h.outputCount() == 1);                 // a single implicit output
}

TEST_CASE("parseScriptHeader: malformed values are ignored") {
    auto h = ofs::parseScriptHeader("// !ofs:signal sideways\n// !ofs:input 2bad\n// !ofs:input has-dash\n");
    CHECK_FALSE(h.hasSignal);      // "sideways" not recognized
    CHECK_FALSE(h.hasInputs);      // "2bad"/"has-dash" are not valid C# identifiers → dropped
    CHECK(h.warnings.size() == 3); // each recognized-but-dropped directive records author feedback
}

TEST_CASE("parseScriptHeader: a recognized-but-dropped directive records a warning, a clean one does not") {
    // A well-formed header produces no warnings.
    CHECK(ofs::parseScriptHeader("// !ofs:signal discrete\n// !ofs:input a\n// !ofs:param Speed 1 0 10\n")
              .warnings.empty());

    auto badDefault = ofs::parseScriptHeader("// !ofs:param X abc\n");
    CHECK(badDefault.params.empty());
    REQUIRE(badDefault.warnings.size() == 1);
    CHECK(badDefault.warnings[0].find("X") != std::string::npos);
    CHECK(badDefault.warnings[0].find("not a number") != std::string::npos);

    auto emptyEnum = ofs::parseScriptHeader("// !ofs:param Mode 0 enum:\n");
    CHECK(emptyEnum.params.empty());
    REQUIRE(emptyEnum.warnings.size() == 1);
    CHECK(emptyEnum.warnings[0].find("enum") != std::string::npos);

    // Duplicates are dropped (first wins) but flagged, not silent.
    auto dupParam = ofs::parseScriptHeader("// !ofs:param Gain 1\n// !ofs:param Gain 2\n");
    CHECK(dupParam.params.size() == 1);
    REQUIRE(dupParam.warnings.size() == 1);
    CHECK(dupParam.warnings[0].find("duplicate") != std::string::npos);
}

TEST_CASE("parseScriptHeader: name and description carry free text to end of line") {
    auto h = ofs::parseScriptHeader("// !ofs:name Sine Wave\n"
                                    "// !ofs:description Generates a smooth oscillation.\n"
                                    "// !ofs:signal functional\nreturn 50;\n");
    CHECK(h.name == "Sine Wave");
    CHECK(h.description == "Generates a smooth oscillation.");
    CHECK(h.hasSignal); // the metadata directives don't disturb the rest of the header
}

TEST_CASE("parseScriptHeader: name/description default empty, last declaration wins") {
    auto none = ofs::parseScriptHeader("// !ofs:input a\nreturn a;\n");
    CHECK(none.name.empty());
    CHECK(none.description.empty());
    auto dup = ofs::parseScriptHeader("// !ofs:name First\n// !ofs:name Second\n");
    CHECK(dup.name == "Second");
}

TEST_CASE("parseScriptHeader: any amount of space is allowed between // and !ofs") {
    // The canonical form is "// !ofs:" but the parser tolerates no space and extra space alike.
    for (const char *src : {"//!ofs:signal discrete\n",      // no space
                            "// !ofs:signal discrete\n",     // one space (the default)
                            "//   !ofs:signal discrete\n",   // several spaces
                            "//\t!ofs:signal discrete\n"}) { // a tab
        CAPTURE(src);
        auto h = ofs::parseScriptHeader(src);
        CHECK(h.hasSignal);
        CHECK(h.signal == ofs::OfsSignalDiscrete);
    }
}

TEST_CASE("parseScriptHeader: the '!' is required — a plain or double comment is not a directive") {
    // Without the bang it is an ordinary comment, so editor notes never become settings...
    CHECK_FALSE(ofs::parseScriptHeader("//ofs:signal discrete\n").hasSignal);
    CHECK_FALSE(ofs::parseScriptHeader("// ofs:signal discrete\n").hasSignal);
    // ...and a double-commented example (how the stub shows the //!ofs:param syntax) stays inert.
    CHECK(ofs::parseScriptHeader("// // !ofs:param Speed 1.0 0 10\n").params.empty());
}

// ── !ofs:param parsing (pure, no .NET) ────────────────────────────────────────

TEST_CASE("parseScriptHeader: a float param with name/default/min/max") {
    auto h = ofs::parseScriptHeader("// !ofs:param Speed 1.5 0 10\nreturn 50;\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].name == "Speed");
    CHECK(h.params[0].type == ofs::OfsParamFloat);
    CHECK(h.params[0].defaultValue == doctest::Approx(1.5f));
    CHECK(h.params[0].min == doctest::Approx(0.0f));
    CHECK(h.params[0].max == doctest::Approx(10.0f));
}

TEST_CASE("parseScriptHeader: the trailing 'int' flag makes an integer param") {
    auto h = ofs::parseScriptHeader("// !ofs:param Steps 8 1 64 int\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].name == "Steps");
    CHECK(h.params[0].type == ofs::OfsParamInt);
    CHECK(h.params[0].defaultValue == doctest::Approx(8.0f));
    CHECK(h.params[0].min == doctest::Approx(1.0f));
    CHECK(h.params[0].max == doctest::Approx(64.0f));
}

TEST_CASE("parseScriptHeader: default-only param is unbounded (min==max)") {
    auto h = ofs::parseScriptHeader("// !ofs:param X 5\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].defaultValue == doctest::Approx(5.0f));
    CHECK(h.params[0].min == h.params[0].max); // unbounded ⇒ no UI clamp
}

TEST_CASE("parseScriptHeader: a non-identifier name is dropped") {
    CHECK(ofs::parseScriptHeader("// !ofs:param 2bad 1\n").params.empty());
    CHECK(ofs::parseScriptHeader("// !ofs:param has-dash 1\n").params.empty());
    CHECK(ofs::parseScriptHeader("// !ofs:param _ok 1\n").params.size() == 1); // leading underscore is valid
}

TEST_CASE("parseScriptHeader: a missing or non-numeric default drops the param") {
    CHECK(ofs::parseScriptHeader("// !ofs:param Lonely\n").params.empty()); // no default token
    CHECK(ofs::parseScriptHeader("// !ofs:param X abc\n").params.empty());  // default not a number
}

TEST_CASE("parseScriptHeader: a duplicate name keeps the first declaration") {
    auto h = ofs::parseScriptHeader("// !ofs:param Gain 1 0 2\n// !ofs:param Gain 9 0 99\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].defaultValue == doctest::Approx(1.0f)); // first wins
    CHECK(h.params[0].max == doctest::Approx(2.0f));
}

TEST_CASE("parseScriptHeader: params keep declaration order alongside signal/inputs + comments") {
    auto h = ofs::parseScriptHeader("// !ofs:signal functional\n"
                                    "// !ofs:input a\n"
                                    "// !ofs:param Alpha 1 0 10   // first knob\n"
                                    "// !ofs:param Beta 2 0 10\n"
                                    "return a;\n");
    CHECK(h.hasSignal);
    CHECK(h.inputCount() == 1);
    REQUIRE(h.params.size() == 2);
    CHECK(h.params[0].name == "Alpha");               // index 0
    CHECK(h.params[1].name == "Beta");                // index 1
    CHECK(h.params[0].max == doctest::Approx(10.0f)); // trailing "// comment" ignored, not parsed as a number
}

TEST_CASE("parseScriptHeader: the trailing 'bool' flag makes a checkbox param with a {0,1} range") {
    auto h = ofs::parseScriptHeader("// !ofs:param Enabled 1 bool\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].name == "Enabled");
    CHECK(h.params[0].type == ofs::OfsParamBool);
    CHECK(h.params[0].defaultValue == doctest::Approx(1.0f));
    CHECK(h.params[0].min == doctest::Approx(0.0f));
    CHECK(h.params[0].max == doctest::Approx(1.0f)); // implicit range, regardless of any numerics
}

TEST_CASE("parseScriptHeader: an 'enum:' flag captures the labels and an index range [0,count-1]") {
    auto h = ofs::parseScriptHeader("// !ofs:param Mode 0 enum:Sine,Square,Saw\n");
    REQUIRE(h.params.size() == 1);
    CHECK(h.params[0].name == "Mode");
    CHECK(h.params[0].type == ofs::OfsParamEnum);
    CHECK(h.params[0].defaultValue == doctest::Approx(0.0f));
    CHECK(h.params[0].min == doctest::Approx(0.0f));
    CHECK(h.params[0].max == doctest::Approx(2.0f)); // 3 labels ⇒ indices 0..2
    REQUIRE(h.params[0].enumLabels.size() == 3);
    CHECK(h.params[0].enumLabels[0] == "Sine");
    CHECK(h.params[0].enumLabels[1] == "Square");
    CHECK(h.params[0].enumLabels[2] == "Saw");
}

TEST_CASE("parseScriptHeader: an enum with no usable labels is dropped, empty entries are skipped") {
    CHECK(ofs::parseScriptHeader("// !ofs:param Mode 0 enum:\n").params.empty());   // no labels at all
    CHECK(ofs::parseScriptHeader("// !ofs:param Mode 0 enum:,,\n").params.empty()); // only empties
    auto h = ofs::parseScriptHeader("// !ofs:param Mode 0 enum:A,,B,\n");           // empties dropped
    REQUIRE(h.params.size() == 1);
    REQUIRE(h.params[0].enumLabels.size() == 2);
    CHECK(h.params[0].enumLabels[0] == "A");
    CHECK(h.params[0].enumLabels[1] == "B");
    CHECK(h.params[0].max == doctest::Approx(1.0f));
}

// ── reconcileScriptParams (by-index seed/clamp; pure, no .NET) ─────────────────

TEST_CASE("reconcileScriptParams: appended params get defaults, existing values preserved by index") {
    std::vector<float> vals{7.0f};
    std::vector<ofs::ScriptParamDef> defs{{.name = "A", .defaultValue = 1.0f},
                                          {.name = "B", .defaultValue = 2.0f, .min = 0.0f, .max = 5.0f}};
    ofs::reconcileScriptParams(vals, defs);
    REQUIRE(vals.size() == 2);
    CHECK(vals[0] == doctest::Approx(7.0f)); // existing kept
    CHECK(vals[1] == doctest::Approx(2.0f)); // new slot seeded with default
}

TEST_CASE("reconcileScriptParams: shrinking defs drops the trailing values") {
    std::vector<float> vals{1.0f, 2.0f, 3.0f};
    std::vector<ofs::ScriptParamDef> defs{{.name = "A", .defaultValue = 0.0f}};
    ofs::reconcileScriptParams(vals, defs);
    REQUIRE(vals.size() == 1);
    CHECK(vals[0] == doctest::Approx(1.0f));
}

TEST_CASE("reconcileScriptParams: out-of-range values are clamped only when a real range is declared") {
    std::vector<float> vals{99.0f, 99.0f};
    std::vector<ofs::ScriptParamDef> defs{{.name = "Clamped", .defaultValue = 0.0f, .min = 0.0f, .max = 10.0f},
                                          {.name = "Free", .defaultValue = 0.0f, .min = 0.0f, .max = 0.0f}};
    ofs::reconcileScriptParams(vals, defs);
    CHECK(vals[0] == doctest::Approx(10.0f)); // clamped to max
    CHECK(vals[1] == doctest::Approx(99.0f)); // unbounded ⇒ untouched
}

TEST_CASE("reconcileScriptParams: empty defs clear the values") {
    std::vector<float> vals{1.0f, 2.0f};
    ofs::reconcileScriptParams(vals, {});
    CHECK(vals.empty());
}

TEST_CASE("reconcileScriptParams: whole-number kinds round a stray fractional and clamp to range") {
    // A repurposed float slot (1.7) under a bool def rounds to 2 then clamps to {0,1} ⇒ 1; an enum
    // index 2.4 with two labels rounds to 2 then clamps to [0,1] ⇒ 1; an int rounds but stays unbounded.
    std::vector<float> vals{1.7f, 2.4f, 3.6f};
    std::vector<ofs::ScriptParamDef> defs{
        {.name = "B", .type = ofs::OfsParamBool, .defaultValue = 0.0f, .min = 0.0f, .max = 1.0f},
        {.name = "E", .type = ofs::OfsParamEnum, .defaultValue = 0.0f, .min = 0.0f, .max = 1.0f},
        {.name = "I", .type = ofs::OfsParamInt, .defaultValue = 0.0f, .min = 0.0f, .max = 0.0f}};
    ofs::reconcileScriptParams(vals, defs);
    CHECK(vals[0] == doctest::Approx(1.0f)); // round(1.7)=2 → clamp {0,1} → 1
    CHECK(vals[1] == doctest::Approx(1.0f)); // round(2.4)=2 → clamp [0,1] → 1
    CHECK(vals[2] == doctest::Approx(4.0f)); // round(3.6)=4, unbounded
}

// ── Stub-text generation (the "Add Script" dialog's new-file template) ────────

TEST_CASE("scriptStubText: header round-trips through parseScriptHeader for every shape") {
    for (auto signal : {ofs::OfsSignalFunctional, ofs::OfsSignalDiscrete}) {
        for (int inputs = 0; inputs <= 2; ++inputs) {
            CAPTURE(static_cast<int>(signal));
            CAPTURE(inputs);
            const ofs::ScriptHeader h = ofs::parseScriptHeader(ofs::scriptStubText(signal, inputs));
            CHECK(h.hasSignal);
            CHECK(h.signal == signal);
            CHECK(h.hasInputs == (inputs > 0)); // a 0-input generator declares no !ofs:input line
            CHECK(h.inputCount() == inputs);
            CHECK(h.params.empty()); // the // !ofs:param hint is double-commented — not a live param
        }
    }
}

TEST_CASE("scriptStubText: body matches the shape and out-of-range inputs are clamped") {
    // Functional bodies must return a float; discrete bodies write into outp.
    CHECK(ofs::scriptStubText(ofs::OfsSignalFunctional, 1).find("return") != std::string::npos);
    CHECK(ofs::scriptStubText(ofs::OfsSignalDiscrete, 1).find("outp.Add") != std::string::npos);
    // A 0-input discrete generator has no input list to read — it seeds from the region bounds.
    CHECK(ofs::scriptStubText(ofs::OfsSignalDiscrete, 0).find("ctx.RegionStart") != std::string::npos);

    // inputCount is clamped to 0..16, so the header always parses back to a valid count: an in-range
    // count survives, an over-cap one is clamped to 16, and a negative one floors to 0.
    CHECK(ofs::parseScriptHeader(ofs::scriptStubText(ofs::OfsSignalFunctional, 7)).inputCount() == 7);
    CHECK(ofs::parseScriptHeader(ofs::scriptStubText(ofs::OfsSignalFunctional, 99)).inputCount() == 16);
    CHECK(ofs::parseScriptHeader(ofs::scriptStubText(ofs::OfsSignalFunctional, -3)).inputCount() == 0);
}

TEST_CASE("scriptStubText: output count declares pins, leaves a single output implicit, and is clamped") {
    using ofs::OfsSignalDiscrete, ofs::OfsSignalFunctional, ofs::parseScriptHeader, ofs::scriptStubText;

    // A single output is implicit: no "// !ofs:output" directive, parser falls back to one "out" pin,
    // and the body keeps the terse return/outp idiom.
    const std::string singleFn = scriptStubText(OfsSignalFunctional, 1);
    CHECK(singleFn.find("!ofs:output") == std::string::npos);
    CHECK(parseScriptHeader(singleFn).outputCount() == 1);
    CHECK(singleFn.find("return") != std::string::npos);

    // >1 output declares each pin (out0, out1, …) and the body writes every one so it compiles as-is.
    const std::string fnSrc = scriptStubText(OfsSignalFunctional, 1, false, {}, {}, 3);
    const ofs::ScriptHeader fn = parseScriptHeader(fnSrc);
    CHECK(fn.outputCount() == 3);
    CHECK(fn.outputNames[0] == "out0");
    CHECK(fn.outputNames[2] == "out2");
    CHECK(fnSrc.find("out0 = ") != std::string::npos);
    CHECK(fnSrc.find("out2 = ") != std::string::npos);
    CHECK(fnSrc.find("return") == std::string::npos); // multi-output assigns pins, never returns

    const std::string discSrc = scriptStubText(OfsSignalDiscrete, 1, false, {}, {}, 2);
    CHECK(discSrc.find("out0.Add") != std::string::npos);
    CHECK(discSrc.find("out1.Add") != std::string::npos);

    // outputCount is clamped to 1..16: under-min floors to 1 (implicit single output), over-cap → 16.
    CHECK(parseScriptHeader(scriptStubText(OfsSignalFunctional, 0, true, {}, {}, 0)).outputCount() == 1);
    CHECK(parseScriptHeader(scriptStubText(OfsSignalFunctional, 0, true, {}, {}, 99)).outputCount() == 16);
}

TEST_CASE("scriptStubText: supplied name/description become live directives") {
    // With comments on and off, a supplied name/description is written as a live "// !ofs:" directive
    // that round-trips back through the parser (not the inert double-commented hint).
    for (bool comments : {true, false}) {
        CAPTURE(comments);
        const ofs::ScriptHeader h = ofs::parseScriptHeader(
            ofs::scriptStubText(ofs::OfsSignalFunctional, 1, comments, "Sine Wave", "Generates a smooth oscillation."));
        CHECK(h.name == "Sine Wave");
        CHECK(h.description == "Generates a smooth oscillation.");
    }

    // An omitted field stays inert: parseScriptHeader sees no live name/description directive even in
    // comments mode (the hint lines are double-commented).
    const ofs::ScriptHeader blank = ofs::parseScriptHeader(ofs::scriptStubText(ofs::OfsSignalFunctional, 1, true));
    CHECK(blank.name.empty());
    CHECK(blank.description.empty());

    // A name without a description writes only the name directive.
    const ofs::ScriptHeader nameOnly =
        ofs::parseScriptHeader(ofs::scriptStubText(ofs::OfsSignalDiscrete, 0, false, "Pulse", ""));
    CHECK(nameOnly.name == "Pulse");
    CHECK(nameOnly.description.empty());
}

// ── File-watch mtime diff (pure helper behind ScriptSystem::update) ───────────

TEST_CASE("diffWatchedMtimes: first observation seeds without reporting a change") {
    using ft = std::filesystem::file_time_type;
    const ft t0{};
    ofs::WatchMtimeMap lastSeen;
    ofs::WatchMtimeMap current{{"a.cs", t0}, {"b.cs", t0}};

    auto changed = ofs::diffWatchedMtimes(current, lastSeen);
    CHECK(changed.empty());      // baseline seed, nothing recompiles
    CHECK(lastSeen.size() == 2); // both files now tracked
}

TEST_CASE("diffWatchedMtimes: reports only files whose mtime advanced") {
    using ft = std::filesystem::file_time_type;
    const ft t0{};
    const ft t1 = t0 + std::chrono::seconds(1);

    ofs::WatchMtimeMap lastSeen{{"a.cs", t0}, {"b.cs", t0}};
    ofs::WatchMtimeMap current{{"a.cs", t1}, {"b.cs", t0}}; // a changed, b did not

    auto changed = ofs::diffWatchedMtimes(current, lastSeen);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == "a.cs");
    CHECK(lastSeen.at("a.cs") == t1); // baseline advanced, so a steady state won't re-report
    CHECK(ofs::diffWatchedMtimes(current, lastSeen).empty());
}

TEST_CASE("diffWatchedMtimes: unwatching a file prunes it; re-watching re-seeds (no spurious compile)") {
    using ft = std::filesystem::file_time_type;
    const ft t0{};
    const ft t1 = t0 + std::chrono::seconds(1);

    ofs::WatchMtimeMap lastSeen{{"a.cs", t0}};

    // a.cs no longer watched this tick → dropped from the map, nothing reported.
    ofs::WatchMtimeMap none;
    CHECK(ofs::diffWatchedMtimes(none, lastSeen).empty());
    CHECK(lastSeen.empty());

    // Re-watching it (already edited to t1 meanwhile) seeds a fresh baseline rather than recompiling.
    ofs::WatchMtimeMap current{{"a.cs", t1}};
    CHECK(ofs::diffWatchedMtimes(current, lastSeen).empty());
    CHECK(lastSeen.at("a.cs") == t1);
}

TEST_CASE("diffWatchedMtimes: a vanished file (zero mtime) that reappears counts as a change") {
    using ft = std::filesystem::file_time_type;
    const ft real = ft{} + std::chrono::seconds(5);
    const ft missing{}; // ScriptSystem maps a stat failure to a zero mtime

    ofs::WatchMtimeMap lastSeen{{"a.cs", real}};
    // File deleted: now reads as the zero baseline → reported (node should fall back to unresolved).
    ofs::WatchMtimeMap gone{{"a.cs", missing}};
    auto c1 = ofs::diffWatchedMtimes(gone, lastSeen);
    REQUIRE(c1.size() == 1);
    CHECK(c1[0] == "a.cs");
    // Recreated with the old timestamp → changes again.
    ofs::WatchMtimeMap back{{"a.cs", real}};
    auto c2 = ofs::diffWatchedMtimes(back, lastSeen);
    REQUIRE(c2.size() == 1);
    CHECK(c2[0] == "a.cs");
}

// ── Script node evaluation (function-pointer seam — no .NET) ──────────────────

namespace {

// Spin-drain until the axis eval job completes.
bool waitForEval(ofs::EventQueue &eq, const ofs::AxisState &axis, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (axis.pendingEval != nullptr) {
        eq.drain();
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Native callbacks shaped exactly like the array-shaped trampolines a compiled script binds to.
void scriptFuncMod(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx *ctx, float *outs,
                   int /*outCount*/, void * /*ud*/) {
    outs[0] = ins[0] + (ctx->paramCount > 0 ? ctx->params[0] : 0.0f);
}
void scriptFuncGen(double /*t*/, const float * /*ins*/, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/, float *outs,
                   int /*outCount*/, void * /*ud*/) {
    outs[0] = 42.0f;
}
void scriptFuncComb(double /*t*/, const float *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/, float *outs,
                    int /*outCount*/, void * /*ud*/) {
    outs[0] = (ins[0] + ins[1]) / 2.0f;
}
void scriptDiscMod(const ofs::OfsDiscreteInput *const *ins, int /*inCount*/, const ofs::OfsEvalCtx * /*ctx*/,
                   ofs::OfsDiscreteOutput *const *outs, int /*outCount*/, void * /*ud*/) {
    const ofs::OfsDiscreteInput *in = ins[0];
    ofs::OfsDiscreteOutput *out = outs[0];
    for (size_t i = 0; i < in->times.size(); ++i) {
        out->times.push_back(in->times[i]);
        out->positions.push_back(in->positions[i] + 5.0f);
    }
}
void scriptDiscGen(const ofs::OfsDiscreteInput *const * /*ins*/, int /*inCount*/, const ofs::OfsEvalCtx *ctx,
                   ofs::OfsDiscreteOutput *const *outs, int /*outCount*/, void * /*ud*/) {
    ofs::OfsDiscreteOutput *out = outs[0];
    out->times.push_back(ctx->regionStart);
    out->positions.push_back(10.0f);
    out->times.push_back(ctx->regionEnd);
    out->positions.push_back(90.0f);
}

// Seed the registry with a native-backed compiled script under file name `file`.
void seedScript(TestProject &tp, const std::string &file, ofs::OfsGenericFn fn, ofs::OfsSignalKind signal,
                int inputCount) {
    tp.scriptReg.fileToHash[file] = file; // hash == name for the test
    tp.scriptReg.byHash[file] =
        ofs::CompiledScript{.ref = {.fnPtr = fn, .userData = nullptr, .signal = signal, .inputCount = inputCount},
                            .inputCount = inputCount};
}

// Input(L0) → Script(file, inputCount=1) → Output(L0).
ofs::ProcessingNodeGraph scriptModGraph(const std::string &file, ofs::OfsSignalKind signal) {
    ofs::ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptSignal = static_cast<uint8_t>(signal);
    s.scriptInputCount = 1;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Script(file, inputCount=0) → Output(L0). No Input node — a pure generator.
ofs::ProcessingNodeGraph scriptGenGraph(const std::string &file, ofs::OfsSignalKind signal) {
    ofs::ProcessingNodeGraph g;
    const int sId = g.allocId();
    const int outId = g.allocId();
    ofs::ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptSignal = static_cast<uint8_t>(signal);
    s.scriptInputCount = 0;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0) → Script-combiner ← Constant(75); → Output(L0).
ofs::ProcessingNodeGraph scriptCombGraph(const std::string &file) {
    ofs::ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int constId = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    g.nodes.push_back(
        {.id = constId, .type = GraphNodeType::Constant, .constantValue = 75.0f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode s;
    s.id = sId;
    s.type = GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptSignal = static_cast<uint8_t>(ofs::OfsSignalFunctional);
    s.scriptInputCount = 2;
    s.role = StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = constId, .toNode = sId, .toPin = 1});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Drive one region [0,10] over axis L0 with a single input action and return resolved actions.
void runSingleRegion(TestProject &tp, const ofs::ProcessingNodeGraph &graph, ofs::EffectRegistryState &effectReg,
                     ofs::JobSystem &jobSystem, int hz = 30) {
    ofs::ProcessingRegion region;
    region.id = 1;
    region.startTime = 0.0;
    region.endTime = 10.0;
    region.hz = hz;
    region.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    region.nodeGraph = graph;
    tp.project.regions.push_back(region);
    tp.project.sortRegions();

    tp.project.axes[0].showInStrip = true;
    tp.project.mutate(StandardAxis::L0, [](ofs::AxisState &a) { a.actions.insert({1.0, 25}); }, tp.eq);
    tp.eq.drain();
    REQUIRE(waitForEval(tp.eq, tp.project.axes[0]));
}

} // namespace

TEST_CASE("ScriptNode: functional modifier is evaluated") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    seedScript(tp, "mod.cs", reinterpret_cast<ofs::OfsGenericFn>(&scriptFuncMod), ofs::OfsSignalFunctional, 1);

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    auto graph = scriptModGraph("mod.cs", ofs::OfsSignalFunctional);
    // The script node carries one param (offset = 10).
    for (auto &n : graph.nodes)
        if (n.type == GraphNodeType::Script)
            n.effect.params = {10.0f};
    runSingleRegion(tp, graph, effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 35); // 25 + 10
}

TEST_CASE("ScriptNode: discrete modifier is evaluated") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    seedScript(tp, "dmod.cs", reinterpret_cast<ofs::OfsGenericFn>(&scriptDiscMod), ofs::OfsSignalDiscrete, 1);

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptModGraph("dmod.cs", ofs::OfsSignalDiscrete), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 30); // 25 + 5
}

TEST_CASE("ScriptNode: discrete generator emits its own actions") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    seedScript(tp, "gen.cs", reinterpret_cast<ofs::OfsGenericFn>(&scriptDiscGen), ofs::OfsSignalDiscrete, 0);

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptGenGraph("gen.cs", ofs::OfsSignalDiscrete), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 2);
    CHECK(resolved[0].at == doctest::Approx(0.0));
    CHECK(resolved[0].pos == 10);
    CHECK(resolved[1].at == doctest::Approx(10.0));
    CHECK(resolved[1].pos == 90);
}

TEST_CASE("ScriptNode: functional generator is sampled at the region Hz") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    seedScript(tp, "fgen.cs", reinterpret_cast<ofs::OfsGenericFn>(&scriptFuncGen), ofs::OfsSignalFunctional, 0);

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    // 5 Hz over the [0,10] region: ceil(10 / 0.2) = 50 uniform steps plus the endpoint sample = 51.
    runSingleRegion(tp, scriptGenGraph("fgen.cs", ofs::OfsSignalFunctional), effectReg, jobSystem, 5);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    CHECK(resolved.size() == 51); // discretized at the region's Hz, not a per-node rate
    for (const auto &a : resolved)
        CHECK(a.pos == 42);
}

TEST_CASE("ScriptNode: functional combiner averages its two inputs") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    seedScript(tp, "comb.cs", reinterpret_cast<ofs::OfsGenericFn>(&scriptFuncComb), ofs::OfsSignalFunctional, 2);

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptCombGraph("comb.cs"), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 50); // (25 + 75) / 2
}

TEST_CASE("ScriptNode: an unresolved modifier passes its input through") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    // No seeding — "missing.cs" has no compiled artifact.

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptModGraph("missing.cs", ofs::OfsSignalFunctional), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].at == doctest::Approx(1.0));
    CHECK(resolved[0].pos == 25); // unchanged — pass-through
}

TEST_CASE("ScriptNode: an unresolved generator produces no output") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    // No seeding for "missing-gen.cs".

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptGenGraph("missing-gen.cs", ofs::OfsSignalDiscrete), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    // The in-region input is consumed by the (empty) generator; nothing is emitted.
    CHECK(tp.project.axes[0].resolved->actions.empty());
}

TEST_CASE("ScriptNode: an errored artifact is treated as unresolved") {
    TestProject tp;
    ofs::EffectRegistryState effectReg;
    // Compiled entry exists but failed (ref invalid, error text set) — must behave like missing.
    tp.scriptReg.fileToHash["bad.cs"] = "h";
    tp.scriptReg.byHash["h"] = ofs::CompiledScript{.ref = {}, .inputCount = 1, .error = "CS1002: ; expected"};

    ofs::JobSystem jobSystem;
    ofs::ProcessingSystem ps(tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem);
    tp.eq.freeze();
    jobSystem.start();

    runSingleRegion(tp, scriptModGraph("bad.cs", ofs::OfsSignalFunctional), effectReg, jobSystem);

    REQUIRE(tp.project.axes[0].resolved.has_value());
    const auto &resolved = tp.project.axes[0].resolved->actions;
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].pos == 25); // pass-through
}
