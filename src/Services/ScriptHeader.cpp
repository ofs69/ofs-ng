#include "ScriptHeader.h"
#include <cctype>
#include <charconv>
#include <spdlog/fmt/fmt.h>

namespace ofs {

namespace {

std::string_view trimLeading(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::string_view trim(std::string_view s) {
    s = trimLeading(s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// First whitespace-delimited token of an already-left-trimmed string.
std::string_view firstToken(std::string_view s) {
    size_t e = 0;
    while (e < s.size() && s[e] != ' ' && s[e] != '\t')
        ++e;
    return s.substr(0, e);
}

// Split into whitespace-delimited tokens, dropping an inline "// comment" tail.
std::vector<std::string_view> tokenize(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= s.size())
            break;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            ++i;
        std::string_view tok = s.substr(start, i - start);
        if (tok.starts_with("//")) // rest of the line is a comment
            break;
        out.push_back(tok);
    }
    return out;
}

// A C# identifier: [A-Za-z_][A-Za-z0-9_]*. Guards the named-local injection in ScriptCompiler.cs.
bool isIdentifier(std::string_view s) {
    auto isFirst = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (s.empty() || !isFirst(s[0]))
        return false;
    return std::ranges::all_of(s, [&](char c) { return isFirst(c) || (c >= '0' && c <= '9'); });
}

bool parseFloat(std::string_view s, float &out) {
    const char *end = s.data() + s.size();
    auto res = std::from_chars(s.data(), end, out);
    return res.ec == std::errc{} && res.ptr == end;
}

} // namespace

ScriptHeader parseScriptHeader(std::string_view source) {
    ScriptHeader header;

    size_t pos = 0;
    while (pos < source.size()) {
        size_t eol = source.find('\n', pos);
        std::string_view line = source.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = (eol == std::string_view::npos) ? source.size() : eol + 1;

        // A directive is "//", then any run of spaces/tabs, then "!ofs:". The "!" is what marks the
        // line as a directive rather than an ordinary comment, so a plain "// ..." line — including a
        // double-commented "// // !ofs:..." example — is never parsed as one.
        std::string_view body = trimLeading(line);
        if (!body.starts_with("//"))
            continue;
        body = trimLeading(body.substr(2)); // skip "//" and any spaces before "!ofs:"
        constexpr std::string_view kPrefix = "!ofs:";
        if (!body.starts_with(kPrefix))
            continue;
        body = body.substr(kPrefix.size());

        // Split into key and the remaining value (first whitespace run separates them).
        size_t sp = 0;
        while (sp < body.size() && body[sp] != ' ' && body[sp] != '\t')
            ++sp;
        std::string_view key = body.substr(0, sp);
        std::string_view rest = trim(body.substr(sp)); // full value tail (trailing CR/space removed)

        if (iequals(key, "signal")) {
            // Single-token value; a trailing "// comment" is ignored.
            std::string_view value = firstToken(rest);
            if (iequals(value, "discrete")) {
                header.signal = OfsSignalDiscrete;
                header.hasSignal = true;
            } else if (iequals(value, "functional")) {
                header.signal = OfsSignalFunctional;
                header.hasSignal = true;
            } else {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:signal '{}': expected 'discrete' or 'functional'", value));
            }
        } else if (iequals(key, "input") || iequals(key, "output")) {
            // One pin per directive; the value is a single C# identifier naming the pin (the injected
            // local in ScriptCompiler.cs). A duplicate name within a direction is dropped (first wins) so
            // the injected locals stay unique. Capped at OFS_MAX_NODE_PINS per direction.
            const bool isInput = iequals(key, "input");
            std::vector<std::string> &names = isInput ? header.inputNames : header.outputNames;
            std::string_view name = firstToken(rest);
            if (!isIdentifier(name)) {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:{} '{}': not a valid C# identifier", isInput ? "input" : "output", name));
                continue;
            }
            if (names.size() >= static_cast<size_t>(OFS_MAX_NODE_PINS)) {
                header.warnings.push_back(fmt::format("ignored !ofs:{} '{}': over the {}-pin limit",
                                                      isInput ? "input" : "output", name, OFS_MAX_NODE_PINS));
                continue;
            }
            bool dup = std::ranges::any_of(names, [&](const std::string &n) { return n == name; });
            if (dup) {
                header.warnings.push_back(fmt::format("ignored duplicate !ofs:{} '{}': the first declaration wins",
                                                      isInput ? "input" : "output", name));
                continue;
            }
            names.emplace_back(name);
            (isInput ? header.hasInputs : header.hasOutputs) = true;
        } else if (iequals(key, "name")) {
            // Free text to end of line: the add-menu display name (last declaration wins).
            header.name = std::string(rest);
        } else if (iequals(key, "description")) {
            // Free text to end of line: the add-menu tooltip (last declaration wins).
            header.description = std::string(rest);
        } else if (iequals(key, "param")) {
            // !ofs:param <name> <default> [min] [max] [int]   (tolerant: malformed lines dropped)
            const std::vector<std::string_view> toks = tokenize(rest);
            if (toks.empty() || !isIdentifier(toks[0])) {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:param '{}': first token must be a valid C# identifier name", rest));
                continue;
            }
            if (toks.size() < 2) {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:param '{}': missing the required numeric <default>", toks[0]));
                continue;
            }
            // Duplicate name → first declaration wins (keeps param indices stable).
            bool dup = false;
            for (const auto &p : header.params)
                if (p.name == toks[0]) {
                    dup = true;
                    break;
                }
            if (dup) {
                header.warnings.push_back(
                    fmt::format("ignored duplicate !ofs:param '{}': the first declaration wins", toks[0]));
                continue;
            }

            ScriptParamDef pd;
            pd.name = std::string(toks[0]);
            if (!parseFloat(toks[1], pd.defaultValue)) {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:param '{}': default '{}' is not a number", pd.name, toks[1]));
                continue;
            }

            // Remaining tokens: up to two numerics (min, max) and an optional type flag
            // ("int" | "bool" | "enum:A,B,C"). The type flag wins over numerics for its implicit range.
            float nums[2] = {0.0f, 0.0f};
            int numCount = 0;
            for (size_t k = 2; k < toks.size(); ++k) {
                if (iequals(toks[k], "int")) {
                    pd.type = OfsParamInt;
                    continue;
                }
                if (iequals(toks[k], "bool")) {
                    pd.type = OfsParamBool;
                    continue;
                }
                constexpr std::string_view kEnum = "enum:";
                if (toks[k].starts_with(kEnum)) {
                    pd.type = OfsParamEnum;
                    // Comma-separated labels, e.g. "enum:Sine,Square,Saw". Empty entries are dropped.
                    std::string_view list = toks[k].substr(kEnum.size());
                    size_t s = 0;
                    while (s <= list.size()) {
                        size_t c = list.find(',', s);
                        std::string_view label = trim(list.substr(s, c == std::string_view::npos ? c : c - s));
                        if (!label.empty())
                            pd.enumLabels.emplace_back(label);
                        if (c == std::string_view::npos)
                            break;
                        s = c + 1;
                    }
                    continue;
                }
                float f = 0.0f;
                if (numCount < 2 && parseFloat(toks[k], f))
                    nums[numCount++] = f;
            }
            // An enum with no usable labels has no widget and no valid index — drop the whole param.
            if (pd.type == OfsParamEnum && pd.enumLabels.empty()) {
                header.warnings.push_back(
                    fmt::format("ignored !ofs:param '{}': enum: has no labels (e.g. enum:A,B,C)", pd.name));
                continue;
            }
            if (pd.type == OfsParamBool) {
                pd.min = 0.0f;
                pd.max = 1.0f;
            } else if (pd.type == OfsParamEnum) {
                pd.min = 0.0f;
                pd.max = static_cast<float>(pd.enumLabels.size() - 1); // index range [0, count-1]
            } else {
                pd.min = (numCount >= 1) ? nums[0] : 0.0f;
                pd.max = (numCount >= 2) ? nums[1] : pd.min; // 0/1 numeric ⇒ min==max ⇒ unbounded
            }
            header.params.push_back(std::move(pd));
        }
    }

    // A script that declares no output pins has a single implicit output named "out" — the common
    // single-output case (a functional `return` / a discrete `outp`) needs no directive.
    if (header.outputNames.empty())
        header.outputNames.emplace_back("out");

    return header;
}

std::string scriptStubText(OfsSignalKind signal, int inputCount, bool comments, std::string_view name,
                           std::string_view description, int outputCount) {
    const int inputs = std::clamp(inputCount, 0, OFS_MAX_NODE_PINS);
    const int outputs = std::clamp(outputCount, 1, OFS_MAX_NODE_PINS);
    const bool discrete = (signal == OfsSignalDiscrete);
    const bool multiOut = outputs > 1;

    // The verbose stub is the only documentation most users will read, so when present it must answer
    // every question a new script raises on its own: what the //!ofs: settings do, what names are in
    // scope in the body, and exactly what `ctx` carries. An experienced user can opt out (`comments`
    // false) for a bare stub that is just the two directives and a starter body. Everything here is
    // plain ASCII (the file is compiled by Roslyn but this .cpp has no /utf-8, and /WX would turn a
    // C4819 into a build break). A trailing "// ..." on a "// !ofs:" line is ignored by the parser.
    std::string s;
    if (comments)
        s = "// ============================================================================\n"
            "//  Script node - inline C# evaluated as one node in this region's graph.\n"
            "//\n"
            "//  A \"// !ofs:\" line is a SETTING (the '!' marks it a directive, not a comment);\n"
            "//  everything below the settings is the BODY you edit. Settings are re-read on\n"
            "//  every save, so changing one reshapes the node (its pins and signal kind)\n"
            "//  immediately - no need to recreate it.\n"
            "// ============================================================================\n"
            "\n";

    s += "// !ofs:signal ";
    s += (discrete ? "discrete" : "functional");
    if (comments)
        s += (discrete ? "   // discrete   = rewrite a list of actions (runs once per region)\n"
                       : "   // functional = return a value for a time t (runs once per sample)\n");
    else
        s += "\n";
    // One named input pin per declared input (in0, in1, …); a generator declares none. A single output
    // is left implicit (named "out"), so a functional body `return`s and a discrete body writes `outp`;
    // with >1 output each pin is declared out0, out1, … and the body assigns/writes every one.
    for (int i = 0; i < inputs; ++i) {
        s += "// !ofs:input in";
        s += std::to_string(i);
        if (comments && i == 0)
            s += "          // an input pin named in0; read it as in0 in the body";
        s += "\n";
    }
    if (multiOut)
        for (int k = 0; k < outputs; ++k) {
            s += "// !ofs:output out";
            s += std::to_string(k);
            if (comments && k == 0)
                s += "       // an output pin named out0; functional: out0 = ...   discrete: out0.Add(...)";
            s += "\n";
        }
    s += "\n";

    // Optional add-menu metadata. A value supplied by the dialog is written as a live directive; an
    // omitted one falls back to the inert double-commented hint (comments mode) so it can be filled in
    // later - delete the leading "// " to activate it (a single "// !ofs:" line IS a directive).
    bool wroteMeta = false;
    if (!name.empty()) {
        s += "// !ofs:name ";
        s += name;
        s += "\n";
        wroteMeta = true;
    }
    if (!description.empty()) {
        s += "// !ofs:description ";
        s += description;
        s += "\n";
        wroteMeta = true;
    }
    if (comments && (name.empty() || description.empty())) {
        s += "// Optional - how this node appears in the Add menu (delete the leading \"// \" to use):\n";
        if (name.empty())
            s += "// // !ofs:name         A friendly node name (default: the file name)\n";
        if (description.empty())
            s += "// // !ofs:description  One line shown as the node's tooltip in the menu\n";
        wroteMeta = true;
    }
    if (wroteMeta)
        s += "\n";

    if (comments) {
        // The example is double-commented ("// // !ofs:param") so it is NOT a live directive: it shows
        // the param syntax without putting a knob on every new script. Deleting the leading "// " leaves
        // "// !ofs:param ...", which IS a real parameter (a slider/field on the node + a same-named local).
        s += "// --- Parameters (optional knobs on the node) --------------------------------\n"
             "// Each \"// !ofs:param\" adds a slider/field on the node and a variable of the\n"
             "// same name you can read in the body.\n"
             "//   Syntax:  // !ofs:param <name> <default> [min] [max] [type]\n"
             "//     name      a valid C# identifier - the variable you read below\n"
             "//     default   starting value\n"
             "//     min max   optional; clamp the node's slider (omit both = unbounded)\n"
             "//     type      optional widget/kind (default float):\n"
             "//                 int              whole-number knob   -> int   variable\n"
             "//                 bool             checkbox            -> bool  variable\n"
             "//                 enum:A,B,C        dropdown of labels  -> int   variable (0-based index)\n"
             "// Delete the leading \"// \" from the next line to enable it, then use Speed below:\n"
             "// // !ofs:param Speed 1.0 0 10\n"
             "\n";

        // Shape-specific reference for the names visible in the body. `ctx` is identical across all
        // shapes; only the inputs/output differ, so describe those per shape and ctx once.
        s += "// --- In scope in the body ---------------------------------------------------\n";
        if (discrete) {
            s += "//   ctx              region info + params (see below)\n";
            if (inputs == 1)
                s += "//   in0              input actions (read-only list; in0[i].At = time, in0[i].Pos = 0..100)\n";
            else if (inputs >= 2) {
                s += "//   in0 .. in";
                s += std::to_string(inputs - 1);
                s += "       the input action lists (each read-only; inK[i].At = time, inK[i].Pos = 0..100)\n";
            }
            if (!multiOut)
                s += "//   outp             output sink: outp.Add(at, pos) appends an action\n"
                     "//                    (the host sorts your output and clamps pos to 0..100)\n";
            else {
                s += "//   out0 .. out";
                s += std::to_string(outputs - 1);
                s += "     output sinks: outK.Add(at, pos) appends an action to pin K\n"
                     "//                    (the host sorts each output and clamps pos to 0..100)\n";
            }
            s += "//   ins / outs       the raw input/output arrays (ins[k], outs[k]) for advanced shapes\n"
                 "//\n"
                 "//   This body runs ONCE PER REGION, so you may loop over the inputs in order\n"
                 "//   and keep local state across actions (integrators, hysteresis, etc.).\n";
        } else {
            s += "//   t                (double) the time being evaluated\n";
            if (inputs == 1)
                s += "//   in0              (float) the upstream value at t, 0..100\n";
            else if (inputs >= 2) {
                s += "//   in0 .. in";
                s += std::to_string(inputs - 1);
                s += "       (float) each input's upstream value at t, 0..100\n";
            }
            s += "//   ctx              region info + params (see below)\n"
                 "//   ins              the raw input array (ins[k]) for advanced shapes\n";
            if (!multiOut)
                s += "//   return ...       a float 0..100 (values outside the range are clamped)\n";
            else {
                s += "//   out0 .. out";
                s += std::to_string(outputs - 1);
                s += "     (float) assign each output pin's value, 0..100 (clamped)\n";
            }
            s += "//\n"
                 "//   This body runs ONCE PER OUTPUT SAMPLE and may run on different threads,\n"
                 "//   so do NOT keep state between calls. For stateful logic use a discrete\n"
                 "//   script (// !ofs:signal discrete) - its body runs once per region.\n";
        }

        // ctx reference - identical for every shape.
        s += "//\n"
             "//   ctx.RegionStart      (double) start time of this region\n"
             "//   ctx.RegionEnd        (double) end time of this region\n"
             "//   ctx.Param(i, dflt)   param value by index (prefer the named variable above)\n"
             "//   ctx.Params           all param values as a ReadOnlySpan<float>\n"
             "// ----------------------------------------------------------------------------\n"
             "\n";
    }

    // Starter body for the chosen shape. The single-output case keeps the terse return/outp idiom; a
    // multi-output stub writes every declared pin so it compiles as-is (each out<k> is a write target).
    if (discrete) {
        if (!multiOut) {
            if (inputs == 0)
                s += "outp.Add(ctx.RegionStart, 50);\n"
                     "outp.Add(ctx.RegionEnd, 50);\n";
            else
                s += "foreach (var p in in0)\n"
                     "    outp.Add(p.At, p.Pos);\n";
        } else {
            for (int k = 0; k < outputs; ++k) {
                const std::string n = std::to_string(k);
                if (inputs == 0) {
                    s += "out";
                    s += n;
                    s += ".Add(ctx.RegionStart, 50);\n";
                    s += "out";
                    s += n;
                    s += ".Add(ctx.RegionEnd, 50);\n";
                } else {
                    s += "foreach (var p in in0)\n    out";
                    s += n;
                    s += ".Add(p.At, p.Pos);\n";
                }
            }
        }
    } else {
        if (!multiOut) {
            s += (inputs == 0 ? "return 50;\n" : "return in0;\n");
        } else {
            for (int k = 0; k < outputs; ++k) {
                s += "out";
                s += std::to_string(k);
                s += (inputs == 0 ? " = 50;\n" : " = in0;\n");
            }
        }
    }
    return s;
}

} // namespace ofs
