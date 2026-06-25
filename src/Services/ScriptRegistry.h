#pragma once

#include "Services/PluginApi.h"    // OfsSignalKind, OfsGenericFn-compatible eval fns
#include "Services/ScriptHeader.h" // ScriptParamDef
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ofs {

// Type-erased trampoline pointer. A generic *function* pointer, not void*, so every cast on the
// path (test fn -> here, here -> Ofs*Fn at call time) is function-pointer-to-function-pointer —
// well-defined and warning-clean. A void* would trip MSVC C4054/C4055 (function/data-pointer
// cast) under /WX. reinterpret_cast back to the matching Ofs*Fn by (kind, signal) before calling.
using OfsGenericFn = void (*)();

// Resolved, process-lifetime-stable call info for one dynamic node — a script *or* a plugin
// node (both kinds resolve into this one kind-neutral ref). This is
// the function-pointer seam: production fills fnPtr from the
// managed Roslyn compile (scripts) or the plugin entry (plugin nodes); a test supplies a native
// C++ function directly. Copied by value into AxisSnapshot at snapshot-build time so the worker
// thread reads a structure it solely owns — fnPtr/userData are stable, so neither a concurrent
// recompile (scripts) nor a plugin unload (plugin nodes) can invalidate an in-flight evaluation.
struct NodeCallRef {
    OfsGenericFn fnPtr = nullptr; // reinterpret_cast to OfsDiscreteNodeFn / OfsFunctionalNodeFn by `signal`
    void *userData = nullptr;     // slot index encoded as a pointer (matches the plugin trampolines)
    OfsSignalKind signal = OfsSignalFunctional;
    int inputCount = 0;   // 0..OFS_MAX_NODE_PINS — how many input pins the worker gathers for this node
    int outputCount = 1;  // 1..OFS_MAX_NODE_PINS
    int stateHandle = -1; // ≥0 → index into a managed TState capture for this eval; -1 = none (script node).
                          // Copied into OfsEvalCtx::stateHandle so the eval callback can reach its TState.

    bool valid() const { return fnPtr != nullptr; }
};

// Per-graph map nodeId -> resolved ref, sliced out of the snapshot for one region's evaluation.
// Holds both kinds (script + plugin node), keyed by node id.
using NodeRefMap = std::unordered_map<int, NodeCallRef>;

// One compiled (or failed) script, keyed by file-content hash. inputCount/outputCount mirror the
// file header so the node can resolve its pin layout; the pin names drive the node's pin labels (not
// serialized — re-derived from the file each load). error holds Roslyn diagnostics (empty on
// success). ref.valid() is false for an errored or not-yet-compiled entry.
struct CompiledScript {
    NodeCallRef ref;
    int inputCount = 1;
    int outputCount = 1;
    std::vector<std::string> inputNames;  // pin labels (header order); index label fallback when empty
    std::vector<std::string> outputNames; // pin labels (header order)
    std::string error;
    std::vector<ScriptParamDef> params; // user-declared params (file order); drives node widgets
};

// 64-bit FNV-1a over the bytes, lowercase hex. Shared by ScriptSystem (compile dedupe) and
// ProcessingSystem / the UI (resolving a graph-embedded script node by its source). Both sides MUST
// hash identically, so the one definition lives here.
inline std::string scriptContentHash(const std::string &content) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : content) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i)
        buf[i] = hex[(h >> ((15 - i) * 4)) & 0xF];
    buf[16] = '\0';
    return {buf};
}

// Main-thread registry: file-content hash -> compiled artifact, plus the current hash for each
// referenced file name. Owned by OfsApp and filled by the ScriptSystem service (Roslyn). Read by
// ProcessingSystem at snapshot-build time. Injectable by reference exactly like
// EffectRegistryState, so tests pre-seed a native-backed script without any .NET runtime.
struct ScriptRegistryState {
    std::unordered_map<std::string, CompiledScript> byHash;  // content hash -> artifact
    std::unordered_map<std::string, std::string> fileToHash; // script file name -> current content hash

    // Resolve a node's scriptFile to its current compiled artifact, or nullptr if the file is
    // unknown or its hash has no entry yet (uncompiled). The artifact may itself be errored
    // (ref.valid() == false) — callers treat that the same as a disabled plugin node.
    const CompiledScript *find(const std::string &fileName) const {
        auto h = fileToHash.find(fileName);
        if (h == fileToHash.end())
            return nullptr;
        auto c = byHash.find(h->second);
        return c == byHash.end() ? nullptr : &c->second;
    }

    // Resolve a graph-embedded script node directly by its source's content hash (it has no file
    // name in fileToHash). nullptr until the embedded source has compiled.
    const CompiledScript *findByHash(const std::string &hash) const {
        auto c = byHash.find(hash);
        return c == byHash.end() ? nullptr : &c->second;
    }
};

} // namespace ofs
