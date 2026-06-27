#include "ProcessingSystem.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "Services/EffectRegistry.h"
#include "Services/PluginNodeIO.h"
#include "Services/ScriptRegistry.h"
#include "Util/Log.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace ofs {

// ── Discrete effect implementations ──────────────────────────────────────────

static VectorSet<ScriptAxisAction> rdpEffect(const VectorSet<ScriptAxisAction> &input, double /*startTime*/,
                                             double /*endTime*/, const float *params, int /*paramCount*/,
                                             const volatile int *cancel) {
    // epsilon is a distance threshold; a negative value is meaningless and makes `maxDist > epsilon` always
    // true, so clamp it. Combined with the `maxIdx > lo` guard below this keeps the loop terminating for any
    // input (a degenerate sub-segment otherwise re-pushes its own range forever).
    float epsilon = std::max(0.0f, params[0]);
    if (input.size() <= 2)
        return input;

    const int n = static_cast<int>(input.size());
    // Worker-owned buffer: this runs on a JobSystem thread, so it must not touch the main-thread frame arena.
    std::vector<uint8_t> keep(static_cast<size_t>(n), 0);
    keep[0] = keep[n - 1] = 1;

    // Iterative RDP via explicit stack — avoids recursive stack overflow on large inputs
    std::vector<std::pair<int, int>> stack;
    stack.reserve(64);
    stack.emplace_back(0, n - 1);

    size_t poll = 0;
    while (!stack.empty()) {
        // Superseded eval: its result is discarded by the staleness guard, so bail without finishing.
        if (cancel && (++poll & 0x3FF) == 0 && *cancel)
            return {};

        auto [lo, hi] = stack.back();
        stack.pop_back();

        if (hi <= lo + 1)
            continue;

        const auto &p1 = input[lo];
        const auto &p2 = input[hi];
        double dx = p2.at - p1.at;
        double dy = static_cast<double>(p2.pos - p1.pos) / 100.0;
        double lenSq = dx * dx + dy * dy;
        // Hoist 1/sqrt(lenSq) out of the inner loop
        bool degenerate = lenSq < 1e-12;
        double invLen = degenerate ? 0.0 : 1.0 / std::sqrt(lenSq);

        float maxDist = 0.0f;
        int maxIdx = lo;
        for (int i = lo + 1; i < hi; ++i) {
            double tx = input[i].at - p1.at;
            double ty = static_cast<double>(input[i].pos - p1.pos) / 100.0;
            double dist = 0.0;
            if (degenerate) {
                dist = std::sqrt(tx * tx + ty * ty);
            } else {
                double cross = tx * dy - ty * dx;
                dist = std::abs(cross) * invLen;
            }
            if (static_cast<float>(dist) > maxDist) {
                maxDist = static_cast<float>(dist);
                maxIdx = i;
            }
        }

        // maxIdx stays at its `lo` init when every interior point is collinear (maxDist 0); subdividing on
        // that would re-push the identical [lo, hi] range forever, so only recurse when it actually advanced.
        if (maxIdx > lo && maxDist > epsilon) {
            keep[maxIdx] = 1;
            stack.emplace_back(lo, maxIdx);
            stack.emplace_back(maxIdx, hi);
        }
    }

    auto view = std::views::iota(0, n) | std::views::filter([&keep](int i) { return keep[i] != 0; }) |
                std::views::transform([&input](int i) { return input[i]; });
    return {view.begin(), view.end()};
}

// ── Bridge implementations ────────────────────────────────────────────────────

// ── Functional effect implementations ────────────────────────────────────────
//
// The trivial functional generators/modifiers (sine, ramp, triangle, scale, mix, noise) and the
// discrete `invert` were culled — each was a one-liner a user can read and
// fork, so they now ship as library scripts packed in data.pak (see data/scripts/lib/*.cs).
// Only the two genuine algorithms remain native: `smooth` (multi-pass) and `rdp` (Ramer-Douglas-Peucker).

static VectorSet<ScriptAxisAction> smoothEffect(const VectorSet<ScriptAxisAction> &input, double /*startTime*/,
                                                double /*endTime*/, const float *params, int /*paramCount*/,
                                                const volatile int *cancel) {
    int passes = static_cast<int>(params[0]);
    if (input.size() < 3 || passes <= 0)
        return input;

    VectorSet<ScriptAxisAction> buf(input.begin(), input.end());
    VectorSet<ScriptAxisAction> next(input.size());
    for (int p = 0; p < passes; ++p) {
        if (cancel && *cancel) // superseded — result discarded by the staleness guard
            return buf;
        next = buf;
        for (size_t i = 1; i + 1 < buf.size(); ++i)
            next[i].pos = (buf[i - 1].pos + buf[i].pos + buf[i + 1].pos) / 3;
        std::swap(buf, next);
    }

    return buf;
}

// ── Registration ──────────────────────────────────────────────────────────────

void registerNativeEffects(EffectRegistryState &reg) {
    // Only the two non-trivial algorithms remain native. The generators and
    // simple modifiers (sine/ramp/triangle/scale/mix/noise/invert) ship as library scripts instead;
    // see data/scripts/lib/*.cs and the add-menu script catalog (ProcessingPanel).
    {
        EffectDefinition def;
        def.kind = EffectKind::Discrete;
        def.type = "smooth";
        def.displayName = Str::ProcEffectSmooth;
        def.category = NodeCategory::Modify;
        def.description = Str::ProcEffectSmoothDesc;
        def.paramDefs = {{.key = "passes",
                          .displayName = Str::ProcParamPasses,
                          .type = EffectParamDef::Type::Int,
                          .defaultValue = 2.0f}};
        def.fn = EffectFn(smoothEffect);
        reg.orderedKeys.push_back(def.type);
        reg.effects.emplace(def.type, std::move(def));
    }
    {
        EffectDefinition def;
        def.kind = EffectKind::Discrete;
        def.type = "rdp";
        def.displayName = Str::ProcEffectRdp;
        def.category = NodeCategory::Modify;
        def.description = Str::ProcEffectRdpDesc;
        def.paramDefs = {{.key = "epsilon",
                          .displayName = Str::ProcParamEpsilon,
                          .type = EffectParamDef::Type::Float,
                          .defaultValue = 0.05f,
                          .min = 0.0f,
                          .max = FLT_MAX}}; // epsilon is a distance; clamp the UI input non-negative
        def.fn = EffectFn(rdpEffect);
        reg.orderedKeys.push_back(def.type);
        reg.effects.emplace(def.type, std::move(def));
    }
}

// ── Graph evaluation ──────────────────────────────────────────────────────────

// Map from axis role to that axis's source actions within a region time range.
using AxisSourceMap = std::unordered_map<StandardAxis, VectorSet<ScriptAxisAction>>;

namespace {

// Float-precision action used inside the graph; never stored to disk.
struct GraphSample {
    double at;
    float pos;
};

struct NodeSignal {
    std::shared_ptr<const std::vector<GraphSample>> discrete; // shared to avoid copying through the graph
    ActionFn functional;
    bool isFunctional = false;
};

// A node yields one NodeSignal per output pin (slot order). Single-output nodes (Input/Effect/math/…)
// produce a 1-element vector; a multi-output plugin/script node produces one entry per output slot. The
// eval cache holds the whole vector per node, since a node computes all its outputs together.
using NodeOutputs = std::vector<NodeSignal>;

// The value an unconnected node input reads — the neutral mid-position (positions run 0..100). Named
// once so every default-input site agrees: a Divide with an unconnected B therefore divides by 50, not
// 0, so applyMathOp's divide-by-zero guard only ever fires for a *connected* input that evaluates to 0.
constexpr float kUnconnectedInputDefault = 50.0f;

// Expose &EvalJob::cancelled as the plain pollable `const volatile int*` that effects (and, via OfsEvalCtx,
// plugin/script nodes) read directly — C# can't speak std::atomic. atomic<int> is lock-free and
// layout-compatible with int, so the address doubles as a plain int*; the C++ side still mutates it only
// through cancel()/isCancelled(). Null off-thread (no job) → effects simply never poll.
const volatile int *cancelFlagOf(EvalJob *job) {
    static_assert(sizeof(std::atomic<int>) == sizeof(int) && std::atomic<int>::is_always_lock_free,
                  "cancelFlagOf exposes &EvalJob::cancelled to managed/native pollers as a plain int*");
    return job ? reinterpret_cast<const volatile int *>(
                     &job->cancelled) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
               : nullptr;
}

// Sample a functional signal at uniform intervals — no rounding. `job` (the in-flight eval) lets a large
// Hz-driven sweep abort the instant the eval is superseded; the partial result is discarded by the caller.
std::vector<GraphSample> discretizeEffect(const ActionFn &fn, double startTime, double endTime, float hz,
                                          EvalJob *job = nullptr) {
    std::vector<GraphSample> result;
    if (hz <= 0.0f || endTime <= startTime || !fn)
        return result;
    const double dt = 1.0 / static_cast<double>(hz);
    const auto numSteps = static_cast<size_t>(std::ceil((endTime - startTime) / dt));
    result.reserve(numSteps + 2);
    // The one loop whose length is Hz-driven (can reach into the millions), so a superseded sweep must be
    // able to bail. Poll every 1024 samples — negligible next to fn(t), which often crosses into a plugin.
    for (size_t step = 0; step < numSteps; ++step) {
        if (job && step % 1024 == 0 && job->isCancelled())
            return result;
        double t = startTime + static_cast<double>(step) * dt;
        result.push_back({.at = t, .pos = fn(t)});
    }
    result.push_back({.at = endTime, .pos = fn(endTime)});
    return result;
}

// Linearly interpolate a sorted GraphSample vector as a continuous function.
ActionFn graphSampleToFn(const std::vector<GraphSample> &ptsIn) {
    return [pts = ptsIn](double t) -> float {
        if (pts.empty())
            return 0.0f;
        if (t <= pts.front().at)
            return pts.front().pos;
        if (t >= pts.back().at)
            return pts.back().pos;
        auto it = std::ranges::lower_bound(pts, t, {}, &GraphSample::at);
        if (it == pts.end())
            return pts.back().pos;
        if (it->at == t)
            return it->pos;
        const auto &hi = *it;
        const auto &lo = *std::prev(it);
        double frac = (t - lo.at) / (hi.at - lo.at);
        return static_cast<float>(lo.pos + frac * static_cast<double>(hi.pos - lo.pos));
    };
}

float applyMathOp(GraphNodeType op, float va, float vb) {
    switch (op) {
    case GraphNodeType::Add:
        return va + vb;
    case GraphNodeType::Subtract:
        return va - vb;
    case GraphNodeType::Multiply:
        return va * vb;
    case GraphNodeType::Divide:
        // Guards a connected divisor that evaluates to 0 (an unconnected B reads kUnconnectedInputDefault).
        return vb != 0.0f ? va / vb : 0.0f;
    default:
        return va;
    }
}

NodeSignal toFunctionalSig(NodeSignal sig, double /*startTime*/, double /*endTime*/) {
    if (sig.isFunctional)
        return sig;
    if (!sig.discrete)
        return sig;
    sig.functional = graphSampleToFn(*sig.discrete);
    sig.isFunctional = true;
    return sig;
}

NodeOutputs evaluateGraphNode(int nodeId, const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                              const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime,
                              double endTime, float hz, std::unordered_map<int, NodeOutputs> &cache,
                              std::unordered_set<int> &inProgress, EvalJob *job);

// Shared discrete/functional grounding + invocation for any compiled callback node (plugin or
// script). The PluginNode and Script branches both resolve a {signal, fnPtr, userData, in/out counts}
// and hand it here, so the worker-thread eval path is identical for both. `params`/`hz` come from the
// node; `nodeId` lets it pull its own upstream inputs via getInputSignal. Returns one NodeSignal per
// declared output pin.
NodeOutputs evalNodeCallback(const NodeCallRef &cb, float hz, const std::vector<float> &params, int nodeId,
                             const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                             const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime, double endTime,
                             std::unordered_map<int, NodeOutputs> &cache, std::unordered_set<int> &inProgress,
                             EvalJob *job);

// Resolve the signal feeding input pin `toPin` of `toNode`: follow the link, evaluate the source node,
// and pick the source output slot the link draws from (link->fromPin). A missing link or an
// out-of-range source slot yields an empty signal (same as a disabled upstream node).
NodeSignal getInputSignal(int toNode, int toPin, const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                          const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime, double endTime,
                          float hz, std::unordered_map<int, NodeOutputs> &cache, std::unordered_set<int> &inProgress,
                          EvalJob *job) {
    const auto *link = graph.findLinkToPin(toNode, toPin);
    if (!link)
        return {};
    NodeOutputs outs = evaluateGraphNode(link->fromNode, graph, effectReg, nodeRefs, sources, startTime, endTime, hz,
                                         cache, inProgress, job);
    const int slot = link->fromPin;
    if (slot < 0 || slot >= static_cast<int>(outs.size()))
        return {};
    return outs[static_cast<size_t>(slot)];
}

NodeOutputs evaluateGraphNode(int nodeId, const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                              const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime,
                              double endTime, float hz, std::unordered_map<int, NodeOutputs> &cache,
                              std::unordered_set<int> &inProgress, EvalJob *job) {
    auto it = cache.find(nodeId);
    if (it != cache.end())
        return it->second;

    // Cycle guard: if this node is already on the evaluation stack, the graph has a loop.
    if (!inProgress.insert(nodeId).second)
        return {};

    const auto *node = graph.findNode(nodeId);
    if (!node) {
        inProgress.erase(nodeId);
        cache[nodeId] = {};
        return {};
    }

    NodeSignal result;    // the single output of a non-callback node
    NodeOutputs multiOut; // set instead by the callback (plugin/script) cases — one entry per output

    switch (node->type) {
    case GraphNodeType::Input: {
        // Look up source by this node's assigned axis role.
        auto srcIt = sources.find(node->role);
        auto samples = std::make_shared<std::vector<GraphSample>>();
        if (srcIt != sources.end()) {
            samples->reserve(srcIt->second.size());
            for (const auto &a : srcIt->second)
                samples->push_back({.at = a.at, .pos = static_cast<float>(a.pos)});
        }
        result.discrete = std::move(samples);
        result.isFunctional = false;
        break;
    }

    case GraphNodeType::Output: {
        result = getInputSignal(nodeId, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                inProgress, job);
        break;
    }

    case GraphNodeType::Effect: {
        NodeSignal in = getInputSignal(nodeId, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                       inProgress, job);
        if (job)
            job->currentNodeId.store(nodeId);
        auto defIt = effectReg.effects.find(node->effect.type);
        if (defIt == effectReg.effects.end()) {
            result = std::move(in);
            break;
        }
        const auto &def = defIt->second;
        switch (def.kind) {
        case EffectKind::Discrete: {
            // Discrete effects operate on integer actions; round at this boundary only.
            VectorSet<ScriptAxisAction> pts;
            if (in.isFunctional && in.functional) {
                // Functional input with no discrete grid of its own — discretize at the region's Hz.
                auto rawPts = discretizeEffect(in.functional, startTime, endTime, hz, job);
                auto view = rawPts | std::views::transform([](const auto &s) {
                                return ScriptAxisAction{s.at, static_cast<int>(std::round(s.pos))};
                            });
                pts = VectorSet<ScriptAxisAction>(view.begin(), view.end());
            } else if (in.discrete) {
                auto view = *in.discrete | std::views::transform([](const auto &s) {
                    return ScriptAxisAction{s.at, static_cast<int>(std::round(s.pos))};
                });
                pts = VectorSet<ScriptAxisAction>(view.begin(), view.end());
            }
            VectorSet<ScriptAxisAction> processed =
                std::get<EffectFn>(def.fn)(pts, startTime, endTime, node->effect.params.data(),
                                           static_cast<int>(node->effect.params.size()), cancelFlagOf(job));
            auto out = std::make_shared<std::vector<GraphSample>>();
            out->reserve(processed.size());
            for (const auto &a : processed)
                out->push_back({.at = a.at, .pos = static_cast<float>(a.pos)});
            result.discrete = std::move(out);
            result.isFunctional = false;
            break;
        }
        case EffectKind::Functional: {
            const float *ep = node->effect.params.data();
            const int ec = static_cast<int>(node->effect.params.size());
            if (def.ignoresInput) {
                result.functional = std::get<FuncEffectFn>(def.fn)(ActionFn{}, startTime, endTime, ep, ec);
                result.isFunctional = true;
            } else if (!in.isFunctional && in.discrete) {
                // Discrete input: evaluate effect at input timestamps — no Hz needed.
                ActionFn fn = graphSampleToFn(*in.discrete);
                ActionFn effFn = std::get<FuncEffectFn>(def.fn)(fn, startTime, endTime, ep, ec);
                auto out = std::make_shared<std::vector<GraphSample>>();
                out->reserve(in.discrete->size());
                for (const auto &s : *in.discrete)
                    out->push_back({.at = s.at, .pos = effFn(s.at)});
                result.discrete = std::move(out);
                result.isFunctional = false;
            } else {
                // Functional input: compose lazily and stay functional.
                NodeSignal f = toFunctionalSig(std::move(in), startTime, endTime);
                result.functional = std::get<FuncEffectFn>(def.fn)(f.functional, startTime, endTime, ep, ec);
                result.isFunctional = true;
            }
            break;
        }
        default:
            result = std::move(in);
            break;
        }
        break;
    }

    case GraphNodeType::Constant: {
        float v = node->constantValue;
        result.functional = [v](double) { return v; };
        result.isFunctional = true;
        break;
    }

    case GraphNodeType::PluginNode: {
        // Plugin nodes resolve on the main thread into nodeRefs at snapshot build, exactly like
        // scripts — the worker never touches effectReg.pluginNodes, so a plugin unload mid-eval
        // cannot tear the entry out from under this job. A disabled plugin
        // is simply absent from nodeRefs → no output (a plugin node never passes its input through).
        auto refIt = nodeRefs.find(nodeId);
        if (refIt == nodeRefs.end() || !refIt->second.valid())
            break;
        // currentNodeId is stamped inside evalNodeCallback once its inputs resolve — setting it here would
        // be clobbered by that recursive input eval and leave the UI highlighting an upstream node.
        multiOut = evalNodeCallback(refIt->second, hz, node->effect.params, nodeId, graph, effectReg, nodeRefs, sources,
                                    startTime, endTime, cache, inProgress, job);
        break;
    }

    case GraphNodeType::Script: {
        // Scripts are resolved on the main thread and copied into the snapshot (nodeRefs); they
        // never enter effectReg.pluginNodes. An uncompiled/errored/missing script passes the input
        // through for a modifier and produces no output for a generator/combiner — exactly like a
        // disabled plugin node.
        auto refIt = nodeRefs.find(nodeId);
        if (refIt == nodeRefs.end() || !refIt->second.valid()) {
            if (node->scriptInputCount == 1)
                result = getInputSignal(nodeId, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                        inProgress, job);
            break;
        }
        multiOut = evalNodeCallback(refIt->second, hz, node->effect.params, nodeId, graph, effectReg, nodeRefs, sources,
                                    startTime, endTime, cache, inProgress, job);
        break;
    }

    case GraphNodeType::Discretize: {
        NodeSignal in = getInputSignal(nodeId, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                       inProgress, job);
        if (job)
            job->currentNodeId.store(nodeId);
        // Functionalize whatever form the input is in (linear interpolation for a discrete input), then
        // resample onto the region's uniform Hz grid — the explicit, user-placed counterpart to the
        // implicit discretization a downstream discrete node would otherwise apply.
        const bool inputDiscrete = !in.isFunctional && in.discrete;
        ActionFn fn = in.isFunctional ? in.functional : (in.discrete ? graphSampleToFn(*in.discrete) : ActionFn{});
        if (!fn) {
            result.isFunctional = false;
            break;
        }
        // params[1] is this node's own sampling rate, independent of the region default. Clamp to the same
        // 1–120 range the region enforces so a stray value can't spin discretizeEffect's sample loop — the
        // one Hz-driven loop in the graph — into the millions.
        const float nodeHz = std::clamp(node->effect.params.size() > 1 ? node->effect.params[1] : 30.0f, 1.0f, 120.0f);
        std::vector<GraphSample> grid = discretizeEffect(fn, startTime, endTime, nodeHz, job);
        // Anti-aliasing: a sparse action that falls between two grid samples (e.g. a stroke peak) is
        // otherwise rounded off by the uniform resample. Merging the original action times back in
        // pins those exact samples to their input values so no input action is lost. Only meaningful
        // for a discrete input — a purely functional input carries no discrete action times to keep.
        const bool keepActions = !node->effect.params.empty() && node->effect.params[0] != 0.0f;
        if (keepActions && inputDiscrete && !in.discrete->empty()) {
            grid.reserve(grid.size() + in.discrete->size());
            for (const auto &s : *in.discrete)
                if (s.at >= startTime && s.at <= endTime)
                    grid.push_back({.at = s.at, .pos = fn(s.at)});
            std::ranges::sort(grid, {}, &GraphSample::at);
            grid.erase(std::ranges::unique(grid, {}, &GraphSample::at).begin(), grid.end());
        }
        result.discrete = std::make_shared<std::vector<GraphSample>>(std::move(grid));
        result.isFunctional = false;
        break;
    }

    case GraphNodeType::Add:
    case GraphNodeType::Subtract:
    case GraphNodeType::Multiply:
    case GraphNodeType::Divide: {
        auto rawA = getInputSignal(nodeId, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                   inProgress, job);
        auto rawB = getInputSignal(nodeId, 1, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                   inProgress, job);
        if (job)
            job->currentNodeId.store(nodeId);

        // Build ActionFns from whichever form each input is in — no premature rounding.
        ActionFn fnA =
            rawA.isFunctional ? rawA.functional : (rawA.discrete ? graphSampleToFn(*rawA.discrete) : ActionFn{});
        ActionFn fnB =
            rawB.isFunctional ? rawB.functional : (rawB.discrete ? graphSampleToFn(*rawB.discrete) : ActionFn{});

        // Time union from discrete inputs only — purely functional inputs (e.g. Constant)
        // have no natural timestamps and ride along on the discrete side's times.
        std::vector<double> times;
        if (rawA.discrete)
            for (const auto &s : *rawA.discrete)
                times.push_back(s.at);
        if (rawB.discrete)
            for (const auto &s : *rawB.discrete)
                times.push_back(s.at);
        std::ranges::sort(times);
        times.erase(std::ranges::unique(times).begin(), times.end());

        if (!times.empty()) {
            auto out = std::make_shared<std::vector<GraphSample>>();
            out->reserve(times.size());
            for (double t : times) {
                if (job && job->isCancelled())
                    break;
                float va = fnA ? fnA(t) : kUnconnectedInputDefault;
                float vb = fnB ? fnB(t) : kUnconnectedInputDefault;
                out->push_back({.at = t, .pos = applyMathOp(node->type, va, vb)});
            }
            result.discrete = std::move(out);
            result.isFunctional = false;
        } else if (fnA || fnB) {
            // Both inputs are functional (no discrete timestamps) — compose as functional and stay
            // functional; discretization waits for a downstream node that needs a grid.
            ActionFn safeA = fnA ? fnA : [](double) -> float { return kUnconnectedInputDefault; };
            ActionFn safeB = fnB ? fnB : [](double) -> float { return kUnconnectedInputDefault; };
            GraphNodeType op = node->type;
            result.functional = [a = std::move(safeA), b = std::move(safeB), op](double t) {
                return applyMathOp(op, a(t), b(t));
            };
            result.isFunctional = true;
        } else {
            result.isFunctional = false;
        }
        break;
    }
    }

    inProgress.erase(nodeId);
    // A callback node already produced its per-output vector; every other node has the single `result`.
    NodeOutputs outputs = !multiOut.empty() ? std::move(multiOut) : NodeOutputs{std::move(result)};
    cache[nodeId] = outputs;
    return outputs;
}

NodeOutputs evalNodeCallback(const NodeCallRef &cb, float hz, const std::vector<float> &params, int nodeId,
                             const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                             const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime, double endTime,
                             std::unordered_map<int, NodeOutputs> &cache, std::unordered_set<int> &inProgress,
                             EvalJob *job) {
    const int outCount = std::clamp(cb.outputCount, 1, OFS_MAX_NODE_PINS);
    const double regStart = startTime;
    const double regEnd = endTime;
    const int paramCount = static_cast<int>(params.size());
    const int inCount = std::clamp(cb.inputCount, 0, OFS_MAX_NODE_PINS);

    // Gather this node's input signals once (pin slots 0..inCount-1). One array-shaped path serves any
    // shape — the old 0/1/2-input gen/mod/comb ladders collapse into this fold over the inputs.
    std::vector<NodeSignal> raws;
    raws.reserve(static_cast<size_t>(inCount));
    for (int i = 0; i < inCount; ++i)
        raws.push_back(getInputSignal(nodeId, i, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                      inProgress, job));

    if (cb.signal == OfsSignalFunctional) {
        auto fn = reinterpret_cast<OfsFunctionalNodeFn>(cb.fnPtr);

        // One ActionFn per input: a discrete input interpolates, a functional input passes through, a
        // missing/unwired input reads the neutral default.
        std::vector<ActionFn> fns;
        fns.reserve(static_cast<size_t>(inCount));
        for (auto &r : raws) {
            ActionFn f = r.isFunctional ? r.functional : (r.discrete ? graphSampleToFn(*r.discrete) : ActionFn{});
            if (!f)
                f = [](double) { return kUnconnectedInputDefault; };
            fns.push_back(std::move(f));
        }

        // Union of every discrete input's timestamps. Any discrete input grounds the node onto a discrete
        // grid (the N-way generalization of the old A/B timestamp merge); with none, the node stays
        // functional and is sampled lazily downstream.
        std::vector<double> times;
        for (auto &r : raws)
            if (r.discrete)
                for (const auto &s : *r.discrete)
                    times.push_back(s.at);
        std::ranges::sort(times);
        times.erase(std::ranges::unique(times).begin(), times.end());

        // Shared per-input ActionFns + params, captured by every output so the M output closures don't each
        // copy them. `fns` is moved into the shared block; `params` is copied so it outlives this call.
        struct FuncEvalState {
            std::vector<ActionFn> fns;
            std::vector<float> params;
        };
        auto state = std::make_shared<FuncEvalState>(FuncEvalState{.fns = std::move(fns), .params = params});

        // Sample the author fn at one time into a full M-wide output buffer: gather the N input values,
        // call once, copy the M outputs out. The discrete-grounded path calls this once per timestamp and
        // distributes all outputs; the functional path wraps it per output slot (one call per output per
        // sample, §2.6 — deliberately not memoized).
        auto sampleAll = [fn, state, regStart, regEnd, paramCount, ud = cb.userData, sh = cb.stateHandle, inCount,
                          outCount](double t, float *outsBuf) {
            float insBuf[OFS_MAX_NODE_PINS];
            for (int i = 0; i < inCount; ++i)
                insBuf[i] = state->fns[static_cast<size_t>(i)](t);
            OfsEvalCtx ctx{.regionStart = regStart,
                           .regionEnd = regEnd,
                           .params = state->params.data(),
                           .paramCount = paramCount,
                           .stateHandle = sh};
            for (int k = 0; k < outCount; ++k)
                outsBuf[k] = 0.0f;
            fn(t, insBuf, inCount, &ctx, outsBuf, outCount, ud);
        };

        NodeOutputs results(static_cast<size_t>(outCount));
        if (!times.empty()) {
            std::vector<std::shared_ptr<std::vector<GraphSample>>> outs(static_cast<size_t>(outCount));
            for (auto &o : outs) {
                o = std::make_shared<std::vector<GraphSample>>();
                o->reserve(times.size());
            }
            for (double t : times) {
                if (job && job->isCancelled())
                    break;
                float outsBuf[OFS_MAX_NODE_PINS];
                sampleAll(t, outsBuf);
                for (int k = 0; k < outCount; ++k)
                    outs[static_cast<size_t>(k)]->push_back({.at = t, .pos = outsBuf[k]});
            }
            for (int k = 0; k < outCount; ++k) {
                results[static_cast<size_t>(k)].discrete = std::move(outs[static_cast<size_t>(k)]);
                results[static_cast<size_t>(k)].isFunctional = false;
            }
        } else {
            for (int k = 0; k < outCount; ++k) {
                results[static_cast<size_t>(k)].functional = [sampleAll, k, outCount](double t) -> float {
                    float outsBuf[OFS_MAX_NODE_PINS];
                    sampleAll(t, outsBuf);
                    return outsBuf[k];
                };
                results[static_cast<size_t>(k)].isFunctional = true;
            }
        }
        return results;
    }

    // ── Discrete path ────────────────────────────────────────────────────────
    // Only a discrete callback loops over the whole region, so it's the only kind handed the cancel flag;
    // the functional builder above produces one value per call and has nothing to poll.
    OfsEvalCtx ctx{.regionStart = startTime,
                   .regionEnd = endTime,
                   .params = params.data(),
                   .paramCount = paramCount,
                   .stateHandle = cb.stateHandle,
                   .cancelFlag = cancelFlagOf(job)};

    // A discrete input keeps its own grid; a lone functional input (no discrete sibling to borrow
    // timestamps from) falls back to a fixed Hz here.
    auto toInput = [&](const NodeSignal &sig) {
        OfsDiscreteInput inp;
        std::vector<GraphSample> samples;
        if (sig.isFunctional && sig.functional)
            samples = discretizeEffect(sig.functional, startTime, endTime, hz, job);
        else if (sig.discrete)
            samples = *sig.discrete;
        inp.times.reserve(samples.size());
        inp.positions.reserve(samples.size());
        for (const auto &s : samples) {
            inp.times.push_back(s.at);
            inp.positions.push_back(s.pos);
        }
        return inp;
    };

    // Sample a functional signal onto an explicit timestamp grid (the union of discrete inputs) — the
    // functional value rides along on that grid, so no Hz is involved.
    auto sampleOntoTimes = [](const NodeSignal &sig, const std::vector<double> &grid) {
        OfsDiscreteInput inp;
        inp.times.reserve(grid.size());
        inp.positions.reserve(grid.size());
        for (double t : grid) {
            inp.times.push_back(t);
            inp.positions.push_back(sig.functional ? sig.functional(t) : kUnconnectedInputDefault);
        }
        return inp;
    };

    // The union of all discrete inputs' timestamps drives sampling of the functional inputs.
    std::vector<double> unionTimes;
    for (auto &r : raws)
        if (r.discrete)
            for (const auto &s : *r.discrete)
                unionTimes.push_back(s.at);
    std::ranges::sort(unionTimes);
    unionTimes.erase(std::ranges::unique(unionTimes).begin(), unionTimes.end());

    // Build the input array: each discrete input keeps its own grid (independent action lists, as the old
    // combiner handed two), each functional input is sampled onto the union grid (or a fixed Hz when there
    // is no discrete input to borrow timestamps from).
    std::vector<OfsDiscreteInput> inputs;
    inputs.reserve(static_cast<size_t>(inCount));
    for (auto &r : raws) {
        const bool discrete = !r.isFunctional && r.discrete;
        if (!discrete && r.isFunctional && r.functional && !unionTimes.empty())
            inputs.push_back(sampleOntoTimes(r, unionTimes));
        else
            inputs.push_back(toInput(r));
    }
    std::vector<const OfsDiscreteInput *> insPtrs;
    insPtrs.reserve(static_cast<size_t>(inCount));
    for (auto &in : inputs)
        insPtrs.push_back(&in);

    // Inputs are resolved; this node's own (potentially long) callback runs now — attribute it here, not
    // in evaluateGraphNode, where the input recursion above would have clobbered it with an upstream id.
    if (job)
        job->currentNodeId.store(nodeId);

    // One OfsDiscreteOutput per declared output pin; the single callback fills all of them at once.
    std::vector<OfsDiscreteOutput> outBufs(static_cast<size_t>(outCount));
    std::vector<OfsDiscreteOutput *> outsPtrs;
    outsPtrs.reserve(static_cast<size_t>(outCount));
    for (auto &o : outBufs)
        outsPtrs.push_back(&o);
    reinterpret_cast<OfsDiscreteNodeFn>(cb.fnPtr)(insPtrs.data(), inCount, &ctx, outsPtrs.data(), outCount,
                                                  cb.userData);

    // Sort each output's actions by time independently.
    NodeOutputs results(static_cast<size_t>(outCount));
    for (int k = 0; k < outCount; ++k) {
        const OfsDiscreteOutput &outBuf = outBufs[static_cast<size_t>(k)];
        std::vector<size_t> order(outBuf.times.size());
        std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, [&](size_t a, size_t b) { return outBuf.times[a] < outBuf.times[b]; });

        auto out = std::make_shared<std::vector<GraphSample>>();
        out->reserve(outBuf.times.size());
        for (size_t idx : order)
            out->push_back({.at = outBuf.times[idx], .pos = std::clamp(outBuf.positions[idx], 0.0f, 100.0f)});
        results[static_cast<size_t>(k)].discrete = std::move(out);
        results[static_cast<size_t>(k)].isFunctional = false;
    }
    return results;
}

// Convert a NodeSignal to integer ScriptAxisActions. A still-functional signal is discretized here,
// at the output, using the Output node's Hz — the graph's single discretization point.
VectorSet<ScriptAxisAction> nodeSignalToActions(NodeSignal &result, double startTime, double endTime, float outHz,
                                                EvalJob *job) {
    auto toIntActions = [](const std::vector<GraphSample> &samples) -> VectorSet<ScriptAxisAction> {
        auto view = samples | std::views::transform([](const GraphSample &s) {
                        return ScriptAxisAction{s.at, std::clamp(static_cast<int>(std::round(s.pos)), 0, 100)};
                    });
        return {view.begin(), view.end()};
    };

    if (result.isFunctional) {
        if (!result.functional)
            return {};
        return toIntActions(discretizeEffect(result.functional, startTime, endTime, outHz, job));
    }
    if (!result.discrete)
        return {};
    return toIntActions(*result.discrete);
}

// Evaluate a graph with per-axis sources; returns one result per Output node (keyed by Output.role).
// If no Output nodes exist, returns the sources unchanged.
std::unordered_map<StandardAxis, VectorSet<ScriptAxisAction>>
evaluateMultiAxisGraph(const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                       const NodeRefMap &nodeRefs, const AxisSourceMap &sources, double startTime, double endTime,
                       float hz, std::unordered_map<int, NodeOutputs> &cache, EvalJob *job) {
    std::unordered_map<StandardAxis, VectorSet<ScriptAxisAction>> results;
    cache.clear();
    std::unordered_set<int> inProgress;

    for (const auto &node : graph.nodes) {
        if (node.type != GraphNodeType::Output)
            continue;
        inProgress.clear(); // each Output evaluation starts fresh (cache is retained)
        // An Output node has a single input pin; getInputSignal already selected the source output slot.
        NodeSignal sig = getInputSignal(node.id, 0, graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache,
                                        inProgress, job);
        results[node.role] = nodeSignalToActions(sig, startTime, endTime, hz, job);
    }

    if (results.empty()) {
        for (const auto &[axis, src] : sources)
            results[axis] = src;
    }
    return results;
}

// Backwards-compat single-source wrapper: maps one source to the Output node's role.
VectorSet<ScriptAxisAction> evaluateGraph(const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg,
                                          const NodeRefMap &nodeRefs, const VectorSet<ScriptAxisAction> &source,
                                          double startTime, double endTime, float hz,
                                          std::unordered_map<int, NodeOutputs> &cache, EvalJob *job) {
    // Find the role of the (last) Output node to use as the source key.
    StandardAxis outputRole = StandardAxis::L0;
    for (const auto &n : graph.nodes)
        if (n.type == GraphNodeType::Output)
            outputRole = n.role;

    AxisSourceMap sources;
    sources[outputRole] = source;

    auto results = evaluateMultiAxisGraph(graph, effectReg, nodeRefs, sources, startTime, endTime, hz, cache, job);
    auto it = results.find(outputRole);
    if (it != results.end())
        return std::move(it->second);
    return source;
}

// Worker-thread body of an axis evaluation. Runs on a JobSystem thread over a value-copied AxisSnapshot
// (never ScriptProject); evaluates each region driving the axis and pushes the merged result back through
// the event queue. Cancellation is cooperative — checked at every region boundary — and a superseded
// job returns without pushing, leaving its captures for abandonEval to release.
void runAxisEval(const std::shared_ptr<EvalJob> &job, AxisSnapshot snap, EventQueue &eq,
                 const EffectRegistryState &effectReg) {
    const auto roleIdx = static_cast<size_t>(snap.role);
    const auto &actions = snap.axes[roleIdx].actions;

    const auto t0 = std::chrono::steady_clock::now();

    std::unordered_map<int, NodeOutputs> evalCache;
    VectorSet<ScriptAxisAction> resolved;
    resolved.reserve(actions.size());

    auto actionIt = actions.begin();
    const auto actionEnd = actions.end();

    for (const auto &region : snap.regions) {
        if (job->isCancelled())
            return;

        if (!region.axisRoles.test(roleIdx))
            continue;

        auto beforeStart = actionIt;
        actionIt = std::ranges::lower_bound(actionIt, actionEnd, region.startTime, {}, &ScriptAxisAction::at);
        for (const auto &action : std::ranges::subrange(beforeStart, actionIt))
            resolved.insert(action);

        auto inStart = actionIt;
        actionIt = std::ranges::upper_bound(actionIt, actionEnd, region.endTime, {}, &ScriptAxisAction::at);
        VectorSet<ScriptAxisAction> input(inStart, actionIt);

        // Script call info for this region (nodeId -> ref), resolved on the main thread.
        static const NodeRefMap kEmptyRefs;
        auto refsIt = snap.nodeRefs.find(region.id);
        const NodeRefMap &nodeRefs = (refsIt != snap.nodeRefs.end()) ? refsIt->second : kEmptyRefs;

        VectorSet<ScriptAxisAction> output;
        if (region.axisRoles.count() > 1) {
            AxisSourceMap sources;
            for (size_t i = 0; i < kStandardAxisCount; ++i) {
                if (!region.axisRoles.test(i))
                    continue;
                const auto axisRole = static_cast<StandardAxis>(i);
                if (axisRole == snap.role) {
                    sources[axisRole] = input;
                } else {
                    const auto &otherActions = snap.axes[i].actions;
                    auto lo = std::ranges::lower_bound(otherActions, region.startTime, {}, &ScriptAxisAction::at);
                    auto hi = std::ranges::upper_bound(otherActions, region.endTime, {}, &ScriptAxisAction::at);
                    sources[axisRole] = VectorSet<ScriptAxisAction>(lo, hi);
                }
            }
            auto results = evaluateMultiAxisGraph(region.nodeGraph, effectReg, nodeRefs, sources, region.startTime,
                                                  region.endTime, static_cast<float>(region.hz), evalCache, job.get());
            auto it = results.find(snap.role);
            if (it != results.end())
                output = std::move(it->second);
            else
                output = input;
        } else {
            output = evaluateGraph(region.nodeGraph, effectReg, nodeRefs, input, region.startTime, region.endTime,
                                   static_cast<float>(region.hz), evalCache, job.get());
        }

        for (const auto &action : output)
            resolved.insert(action);
    }

    if (job->isCancelled())
        return;

    for (const auto &action : std::ranges::subrange(actionIt, actionEnd))
        resolved.insert(action);

    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    job->currentNodeId.store(-1);
    eq.push(EvalCompleteEvent{
        .role = snap.role,
        .job = job,
        .resolvedActions = std::move(resolved),
        .evalMs = ms,
        .hasResult = true,
    });
}

} // anonymous namespace

// ── ProcessingSystem ──────────────────────────────────────────────────────────

ProcessingSystem::ProcessingSystem(ScriptProject &project, const EffectRegistryState &effectReg,
                                   const ScriptRegistryState &scriptReg, EventQueue &eq, JobSystem &jobSystem)
    : project(project), effectReg(effectReg), scriptReg(scriptReg), eq(eq), jobSystem(jobSystem) {
    eq.on<AxisModifiedEvent>([this](const AxisModifiedEvent &event) { onAxisModified(event); });
    eq.on<RequestAxisEvalEvent>([this](const RequestAxisEvalEvent &event) { onRequestEval(event); });
    eq.on<SetAutoEvalEnabledEvent>([this](const SetAutoEvalEnabledEvent &event) { onSetAutoEvalEnabled(event); });
    eq.on<EvalCompleteEvent>([this](const EvalCompleteEvent &event) { onEvalComplete(event); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &event) { onProjectLoaded(event); });
}

void ProcessingSystem::onProjectLoaded(const LoadProjectEvent &) {
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        for (const auto &r : project.regions) {
            if (r.axisRoles.test(i)) {
                eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
                break;
            }
        }
    }
}

void ProcessingSystem::onAxisModified(const AxisModifiedEvent &event) {
    // While auto-eval is halted, edits don't recompute; resolved was cleared when the halt engaged
    // (see onSetAutoEvalEnabled), so the timeline shows raw actions until a manual Recompute.
    if (!project.autoEvalEnabled)
        return;
    evaluateAxis(event.role);
}

void ProcessingSystem::onRequestEval(const RequestAxisEvalEvent &event) {
    evaluateAxis(event.role);
}

void ProcessingSystem::onSetAutoEvalEnabled(const SetAutoEvalEnabledEvent &event) {
    if (project.autoEvalEnabled == event.enabled)
        return;
    project.autoEvalEnabled = event.enabled;

    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        auto &axis = project.axes[i];
        bool drivenByRegion = false;
        for (const auto &r : project.regions)
            if (r.axisRoles.test(i)) {
                drivenByRegion = true;
                break;
            }
        if (!drivenByRegion)
            continue;

        if (event.enabled) {
            // Re-enabling: recompute so the processed result reflects edits made while halted.
            evaluateAxis(static_cast<StandardAxis>(i));
        } else {
            // Halting: drop the processed result; the timeline falls back to raw actions until recompute.
            abandonEval(axis);
            axis.resolved = std::nullopt;
        }
    }
}

void ProcessingSystem::evaluateAxis(StandardAxis role) {
    const auto roleIdx = static_cast<size_t>(role);
    auto &axis = project.axes[roleIdx];

    abandonEval(axis);

    bool hasRegions = false;
    for (const auto &r : project.regions)
        if (r.axisRoles.test(roleIdx)) {
            hasRegions = true;
            break;
        }

    if (!hasRegions) {
        axis.resolved = std::nullopt;
        return;
    }

    AxisSnapshot snap;
    snap.role = role;
    snap.regions = project.regions;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        snap.axes[i].actions = project.axes[i].actions;

    const int captureGeneration = buildNodeRefs(snap);

    auto job = std::make_shared<EvalJob>();
    job->captureGeneration = captureGeneration;
    axis.pendingEval = job;

    jobSystem.submit(job, std::move(snap), [job, &eq = this->eq, &effectReg = this->effectReg](AxisSnapshot snap) {
        runAxisEval(job, std::move(snap), eq, effectReg);
    });
}

int ProcessingSystem::buildNodeRefs(AxisSnapshot &snap) {
    // Resolve every dynamic node — script AND plugin — to its call info HERE, on the main thread, and
    // copy the refs into the snapshot. The worker then reads a structure it solely owns; the fnPtr is
    // process-lifetime-stable, so neither a concurrent recompile (scripts) nor a plugin unload (plugin
    // nodes) can invalidate an in-flight eval. Unresolved nodes — an uncompiled/errored/missing script
    // or a disabled plugin — are simply omitted; the worker treats a missing ref as a disabled node.
    // (Kicking off a compile for an unresolved script is the ScriptSystem's job, wired in a later phase.)
    // A plugin node with TState additionally has its nodeState JSON value-captured here (main thread) into a
    // managed TState; the worker reads only the int handle. captureGeneration is allocated lazily on the first
    // capture so an eval with no state nodes never touches the codec, and groups this eval's captures for a
    // single release on completion/cancel.
    int captureGeneration = -1;
    for (const auto &region : snap.regions) {
        for (const auto &node : region.nodeGraph.nodes) {
            if (node.type == GraphNodeType::Script) {
                // A graph-embedded node resolves by its source's content hash (no file name); a file
                // node resolves through fileToHash as before.
                const CompiledScript *cs = node.scriptEmbeddedSource.empty()
                                               ? scriptReg.find(node.scriptFile)
                                               : scriptReg.findByHash(scriptContentHash(node.scriptEmbeddedSource));
                if (cs && cs->ref.valid())
                    snap.nodeRefs[region.id][node.id] = cs->ref; // stateHandle stays -1
            } else if (node.type == GraphNodeType::PluginNode) {
                // Resolve the plugin entry on the main thread — the worker no longer reads
                // effectReg.pluginNodes (closes the unload-mid-eval race). Absent → disabled.
                auto it = effectReg.pluginNodes.find(node.pluginNodeId);
                if (it == effectReg.pluginNodes.end())
                    continue;
                const auto &entry = it->second;
                NodeCallRef ref{.fnPtr = entry.fn,
                                .userData = entry.userData,
                                .signal = entry.signal,
                                .inputCount = entry.inputCount,
                                .outputCount = entry.outputCount};
                // Capture every state-bearing node, even one whose nodeState is still empty (default params,
                // never edited). The worker needs a real stateHandle for a `prepare` factory to memoize its
                // artifact once per region eval; with no handle it would rebuild it per sample. Empty JSON
                // decodes to a default TState, so the capture is well-defined.
                if (effectReg.nodeStateCodec.capture && entry.hasState) {
                    if (captureGeneration < 0)
                        captureGeneration = nextCaptureGeneration_++;
                    ref.stateHandle =
                        effectReg.nodeStateCodec.capture(entry.slot, node.nodeState.c_str(), captureGeneration);
                }
                snap.nodeRefs[region.id][node.id] = ref;
            }
        }
    }
    return captureGeneration;
}

void ProcessingSystem::releaseCaptures(int generation) const {
    if (generation >= 0 && effectReg.nodeStateCodec.release)
        effectReg.nodeStateCodec.release(generation);
}

void ProcessingSystem::abandonEval(AxisState &axis) const {
    if (!axis.pendingEval)
        return;
    axis.pendingEval->cancel();
    // A superseded/halted job never pushes EvalCompleteEvent, so release its captures here rather than leak
    // them. Cancellation is cooperative (checked only at region boundaries), so a worker may still be mid-
    // region and read a handle we just freed. That is safe for two independent reasons, BOTH of which must
    // hold: (1) capture handles are monotonic and never reused, and a managed read of a missing handle
    // yields a default value, never corrupt memory; (2) a superseded job's result is discarded by the
    // staleness guard in onEvalComplete regardless of what it computed. The captured value is NOT kept alive
    // by the worker's closure — the snapshot carries only the int handle — so do not weaken either reason.
    releaseCaptures(axis.pendingEval->captureGeneration);
    axis.pendingEval = nullptr;
}

void ProcessingSystem::onEvalComplete(const EvalCompleteEvent &event) {
    auto &axis = project.axes[static_cast<size_t>(event.role)];
    if (axis.pendingEval.get() != event.job.get())
        return; // stale result from a superseded job (its captures were released when it was superseded)

    if (event.hasResult)
        axis.resolved = ResolvedActions{.actions = event.resolvedActions, .evalMs = event.evalMs};
    else
        axis.resolved = std::nullopt;

    releaseCaptures(event.job->captureGeneration);
    axis.pendingEval.reset();
}

} // namespace ofs
