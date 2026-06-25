#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include "Localization/TrKey.h"
#include "Services/NodeCategory.h"
#include "Services/PluginApi.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ofs {

// Discrete effect: VectorSet → VectorSet. `cancel` is the worker's cooperative-cancel flag (the same
// &EvalJob::cancelled the plugin/script path polls, or null off-thread): an unbounded effect MUST poll it
// at loop boundaries and bail early when set, so a superseded eval doesn't spin a pool thread forever.
using EffectFn = std::function<VectorSet<ScriptAxisAction>(const VectorSet<ScriptAxisAction> &input, double startTime,
                                                           double endTime, const float *params, int paramCount,
                                                           const volatile int *cancel)>;

// Continuous function over time, t → position [0, 100]
using ActionFn = std::function<float(double t)>;

// Functional effect: ActionFn → ActionFn (composes functions)
using FuncEffectFn = std::function<ActionFn(const ActionFn &input, double startTime, double endTime,
                                            const float *params, int paramCount)>;

enum class EffectKind { Discrete, Functional };

struct EffectParamDef {
    std::string key;      // stable identifier (serialization, ##widget id) — not user-visible
    TrKey displayName{0}; // localized row label; native effects only, so always a catalog key
    enum class Type { Float, Int } type;
    float defaultValue = 0.0f;
    // Optional node-UI clamp range. When max > min the drag is clamped to [min, max]
    // (ImGuiSliderFlags_AlwaysClamp); otherwise the field is unbounded. Use max = FLT_MAX for a
    // lower-bound-only clamp (e.g. a non-negative threshold). Mirrors the script/plugin param path.
    float min = 0.0f;
    float max = 0.0f;
};

struct EffectDefinition {
    EffectKind kind = EffectKind::Discrete;
    std::string type;                             // stable identifier (serialization, registry key) — not user-visible
    TrKey displayName{0};                         // localized menu/node-body label
    NodeCategory category = NodeCategory::Modify; // add-menu grouping bucket
    bool ignoresInput = false;                    // true for pure generators that replace rather than modify the signal
    TrKey description{0};                         // localized tooltip text (index 0 ⇒ none)
    std::vector<EffectParamDef> paramDefs;
    std::variant<EffectFn, FuncEffectFn> fn;
};

struct PluginNodeEntry {
    std::string id;
    std::string displayName;
    std::string category;                  // add-node menu group header (def->group, or the plugin name when unset)
    OfsNodeIcon icon = OfsNodeIconDefault; // author-declared glyph; Default → palette uses the arity icon
    OfsSignalKind signal = OfsSignalFunctional;
    int inputCount = 0;                   // 0..OFS_MAX_NODE_PINS
    int outputCount = 1;                  // 1..OFS_MAX_NODE_PINS
    std::vector<std::string> inputNames;  // pin labels, length == inputCount; copied at registration
    std::vector<std::string> outputNames; // pin labels, length == outputCount
    OfsNodeEvalFn fn = nullptr;           // cast to OfsDiscreteNodeFn / OfsFunctionalNodeFn by `signal`
    void *userData = nullptr;
    OfsNodeUiFn onNodeUi = nullptr; // custom node-body UI callback; null = host-rendered scalar widgets only
    bool hasState = false;          // TState-backed (the node carries JSON state in nodeState; managed owns codec)
    int slot = 0;                   // managed slot index for this registration; passed to the capture codec
};

// Capture/release seam for plugin-node TState. The managed host
// exports these as [UnmanagedCallersOnly] entry points (fetched like CompileScriptNative); a test
// supplies native fakes. capture decodes a node's nodeState JSON into a managed TState for ONE eval
// and returns a handle (≥0) carried in OfsEvalCtx.stateHandle to the worker, or -1 on failure/empty.
// `generation` groups every capture of one axis eval so release drops them in a single call.
typedef int (*OfsCaptureNodeStateFn)(int slot, const char *stateJsonUtf8, int generation);
typedef void (*OfsReleaseNodeStatesFn)(int generation);
typedef void (*OfsClearNodeUpdatesFn)(); // purge deferred Node.Update queues on project load

struct NodeStateCodec {
    OfsCaptureNodeStateFn capture = nullptr; // null → no managed host wired (no plugin TState nodes)
    OfsReleaseNodeStatesFn release = nullptr;
    OfsClearNodeUpdatesFn clearNodeUpdates = nullptr; // purge stale deferred updates on project load
};

struct EffectRegistryState {
    std::unordered_map<std::string, EffectDefinition> effects;
    std::vector<std::string> orderedKeys; // insertion order for the Add Effect list

    // Plugin-registered nodes — keyed by "pluginname.localid"
    std::unordered_map<std::string, PluginNodeEntry> pluginNodes;
    std::vector<std::string> pluginNodeKeys; // insertion order for the Add Node menu

    // TState capture/release codec, wired by PluginManager from the managed host (null until the
    // managed entry points land in phase 4d; native fake in tests). Read on the main thread only.
    NodeStateCodec nodeStateCodec;

    // Node-body custom-UI invoker, wired by PluginManager so the UI layer
    // need not know PluginCtx. Sets the "current node" JSON on the host ctx to `nodeState`, calls
    // entry.onNodeUi on the main thread, and returns true if the managed renderer changed the state
    // (writing the new JSON back into `nodeState`). Null until wired (no managed host / no plugin nodes).
    std::function<bool(const PluginNodeEntry &entry, std::string &nodeState, int regionId, int nodeId)> renderNodeUi;
};

} // namespace ofs
