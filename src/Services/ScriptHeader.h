#pragma once

#include "Services/PluginApi.h" // OfsSignalKind, OfsParamType, OFS_MAX_NODE_PINS
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace ofs {

// One user-declared script parameter, parsed from a "// !ofs:param" directive. Surfaced as an
// editable widget on the node and delivered into the script as ctx.Param(i) / an injected named
// local. min==max means "unbounded".
struct ScriptParamDef {
    std::string name;                  // a valid C# identifier (validated by the parser)
    OfsParamType type = OfsParamFloat; // Float | Int | Bool | Enum
    float defaultValue = 0.0f;
    float min = 0.0f;
    float max = 0.0f;
    std::vector<std::string> enumLabels; // OfsParamEnum only: the combo entries (index = stored value)
};

// Parsed result of a script file's leading "// !ofs:" directives. A script file is
// self-describing: its header pins the signal kind and its named input/output pins so the node can
// derive its pin layout from the file. Pure data — no .NET involved. Pins are named, one directive
// per pin (the count is the number of directives); the names are injected as locals in the compiled
// body (see ScriptCompiler.cs) and rendered as pin labels.
//
//   // !ofs:signal discrete       // discrete | functional
//   // !ofs:input  speed          // one input pin named "speed" (omit entirely ⇒ a generator)
//   // !ofs:output left           // one output pin named "left"
//   // !ofs:output right          // a second output pin (⇒ a multi-output node)
//   // !ofs:param Speed 1.0 0 10  // <name> <default> [min] [max] [int]
//
// A node with no "!ofs:input" line is a generator (0 inputs); with no "!ofs:output" line it has a
// single implicit output named "out" (so the common single-output script needs no directive).
struct ScriptHeader {
    OfsSignalKind signal = OfsSignalFunctional; // default when the directive is absent
    std::vector<std::string> inputNames;        // input pin names, declaration order (size = input pin count, 0..16)
    std::vector<std::string> outputNames;       // output pin names (size = output pin count, 1..16)
    bool hasSignal = false;                     // a valid !ofs:signal directive was found
    bool hasInputs = false;                     // at least one valid !ofs:input directive was found
    bool hasOutputs = false;                    // at least one valid !ofs:output directive was found
    std::vector<ScriptParamDef> params;         // user params in declaration order (index = param slot)
    std::string name;                           // !ofs:name — add-menu display name (empty ⇒ use file stem)
    std::string description;                    // !ofs:description — add-menu tooltip (empty ⇒ none)
    std::vector<std::string> warnings; // human-readable note per recognized-but-dropped directive (author feedback)

    [[nodiscard]] int inputCount() const { return static_cast<int>(inputNames.size()); }
    [[nodiscard]] int outputCount() const { return static_cast<int>(outputNames.size()); }
};

// Parse the "// !ofs:" directives out of script source. Scans line by line; a directive is a line
// that is "//", then any run of spaces/tabs, then "!ofs:" — the '!' is what distinguishes it from
// an ordinary comment. Recognized keys are "signal" (value discrete|functional), "input"/"output"
// (a single C# identifier naming the pin), "name", "description", and "param". Unknown keys and
// malformed values are ignored; a duplicate pin name (within a direction) keeps the first. At most
// OFS_MAX_NODE_PINS pins per direction are kept. When no "!ofs:output" directive appears the result
// carries a single implicit output named "out". Tolerant by design — the file is externally editable;
// a recognized directive that is dropped for a bad value records a line in `warnings` so the author
// can be told why (the compiler logs them), rather than the value vanishing with no trace.
ScriptHeader parseScriptHeader(std::string_view source);

// Produce the full text of a starter script file for the given signal, input count, and output count:
// a matching "// !ofs:" header (which round-trips through parseScriptHeader) plus a minimal Eval body
// whose in-scope names match the wrapper in plugins/Ofs.ScriptHost/ScriptCompiler.cs. inputCount is
// clamped to 0..16 (named "in0", "in1", …) and outputCount to 1..16. A single output is left implicit
// (named "out", so a functional body `return`s and a discrete one writes `outp`); with >1 output each
// pin is declared "out0", "out1", … and the body assigns/writes every one. When `comments` is false,
// the verbose inline documentation is omitted, leaving just the directives and the starter body. A
// non-empty `name`/`description` is written as a live "// !ofs:name"/"// !ofs:description" directive
// (the add-menu display name and tooltip); an empty one falls back to the inert double-commented hint
// in `comments` mode, or nothing without. Used by the "Add Script" dialog to seed a new .cs.
std::string scriptStubText(OfsSignalKind signal, int inputCount, bool comments = true, std::string_view name = {},
                           std::string_view description = {}, int outputCount = 1);

// Reconcile a node's positional param values against the current defs, by index: keep an existing
// value where the slot still exists, else seed the default; clamp to [min,max] when a real range is
// declared (max>min). Run on every (re)compile and on project load so widgets, stored values, and
// the injected locals all agree on the same ordered slots.
inline void reconcileScriptParams(std::vector<float> &vals, const std::vector<ScriptParamDef> &defs) {
    std::vector<float> next;
    next.reserve(defs.size());
    for (size_t i = 0; i < defs.size(); ++i) {
        float v = (i < vals.size()) ? vals[i] : defs[i].defaultValue;
        // Whole-number kinds (int count, bool 0/1, enum index) store an integer value — round a stray
        // fractional (e.g. a repurposed float slot) before clamping so the widget sees a clean index.
        if (defs[i].type != OfsParamFloat)
            v = std::round(v);
        if (defs[i].max > defs[i].min)
            v = std::clamp(v, defs[i].min, defs[i].max);
        next.push_back(v);
    }
    vals = std::move(next);
}

} // namespace ofs
