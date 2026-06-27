#pragma once

#include "Services/ScriptRegistry.h"

#include <string>

namespace ofs {

// ── Script nodes (Roslyn-backed inline C#) ──────────────────────────────────────

// Request a (re)compile of a script file under <prefPath>/scripts. ScriptSystem reads + hashes
// the file on the main thread and, if the content hash is new, submits the Roslyn compile to a
// worker. Pushed on project load (per referenced file) and by the UI (pick / reload).
struct CompileScriptEvent {
    std::string fileName; // file name relative to <prefPath>/scripts
};

// Request a (re)compile of a graph-embedded script (source carried on the node, no file on disk).
// ScriptSystem hashes the source and, if that content hash is new, submits the Roslyn compile.
// Pushed on project load (per distinct embedded source) and when a graph with embedded scripts is
// trusted/applied.
struct CompileEmbeddedScriptEvent {
    std::string source;
};

// Promote a graph-embedded script node to a file node: write its source to <prefPath>/scripts
// under `fileName` and repoint the node at that file. Pushed by the per-node "Save to scripts
// folder" action; the UI guarantees `fileName` is either free or byte-identical to the existing
// file (a differing name clash is refused, so the user must pick another name).
struct SaveEmbeddedScriptEvent {
    int regionId;
    int nodeId;
    std::string fileName;
};

// Result of a script compile, pushed from the JobSystem worker back to the main thread.
// On success `ref` is valid; on failure `error` holds the Roslyn diagnostics and `ref` is empty.
struct ScriptCompiledEvent {
    std::string fileName;
    std::string hash; // content hash the artifact is keyed by
    NodeCallRef ref;
    int inputCount = 1;
    int outputCount = 1;
    std::vector<std::string> inputNames;  // pin labels (header order)
    std::vector<std::string> outputNames; // pin labels (header order)
    bool ok = false;
    std::string error;
};

} // namespace ofs
