#pragma once

#include "Core/ProcessingEffect.h"
#include "Core/StandardAxis.h"
#include "imgui.h"
#include <bitset>
#include <map>
#include <string>
#include <vector>

namespace ofs {

// Set of axis roles — one bit per StandardAxis value
using AxisRoles = std::bitset<kStandardAxisCount>;

// Default region discretization rate (Hz), used wherever the graph turns a functional signal discrete.
inline constexpr int kDefaultRegionHz = 30;

enum class GraphNodeType : uint8_t {
    Input,         // source signal — no inputs, 1 output
    Output,        // sink — 1 input, no outputs
    Effect,        // wraps a ProcessingEffect — 1 input, 1 output
    Add,           // A + B — 2 inputs, 1 output
    Subtract,      // A - B — 2 inputs, 1 output
    Multiply,      // A * B — 2 inputs, 1 output
    Divide,        // A / B — 2 inputs, 1 output (B=0 → 0)
    Constant,      // fixed value — no inputs, 1 output (functional)
    PluginNode,    // plugin-defined — 0/1/2 inputs depending on kind, 1 output
    Script,        // Roslyn-compiled .cs file — 0/1/2 inputs depending on scriptInputCount, 1 output
    Discretize,    // resample any input onto a uniform Hz grid — 1 input, 1 output (always discrete).
                   // effect.params[0] (0/1) keeps the original input action times so peaks survive aliasing;
                   // effect.params[1] is the node's own sampling rate (Hz, 1–120) independent of the region
    Functionalize, // mark any input as a continuous functional signal (linear interp for a discrete input)
                   // — 1 input, 1 output (always functional). The explicit inverse of Discretize: no params,
                   // it just defers discretization downstream to the graph's single Hz point (the Output).
};

struct ProcessingGraphNode {
    int id = 0;
    GraphNodeType type = GraphNodeType::Effect;
    ProcessingEffect effect;     // type == Effect: effect.type = effect id, effect.params = param values
                                 // type == PluginNode: effect.type UNUSED (id moved to pluginNodeId);
                                 //   effect.params = scalar param values (until the TState channel replaces them)
                                 // type == Script: effect.type unused; effect.params = !ofs:param values (by index)
    float constantValue = 50.0f; // valid when type == Constant
    float posX = 0.0f;
    float posY = 0.0f;
    StandardAxis role = StandardAxis::L0; // which axis this Input/Output node represents
    uint8_t pluginInputCount = 1;         // input pin count, valid when type == PluginNode; persisted so a disabled
                                          // plugin still draws the right pins (mirrors scriptInputCount)
    uint8_t pluginOutputCount = 1;        // output pin count, valid when type == PluginNode; persisted alongside
                                          // pluginInputCount so a disabled plugin still draws the right output pins
    uint8_t pluginSignal = 0;             // OfsSignalKind, valid when type == PluginNode
    // type == Script: a reference to a .cs file under <prefPath>/scripts (the source of truth).
    std::string scriptFile;        // file name relative to <prefPath>/scripts — true document state, SERIALIZED
    uint8_t scriptSignal = 1;      // OfsSignalKind cache (default Functional); authoritative source is the file header
    uint8_t scriptInputCount = 1;  // 0..16 cache; authoritative source is the file header. Persisted so a node with a
                                   // missing script file still draws the right pins (mirrors pluginInputCount).
    uint8_t scriptOutputCount = 1; // 1..16 cache; authoritative source is the file header. Persisted alongside
                                   // scriptInputCount so a missing script file still draws the right output pins.
    bool scriptWatch = false;      // type == Script: auto-recompile when the .cs file changes on disk (ScriptSystem
                                   // polls). Transient UI toggle — NOT serialized; resets to off on load.
    // type == Script, embedded variant: the script's source carried inside the graph itself instead
    // of referenced from <prefPath>/scripts. Non-empty ⇒ a *graph-embedded* script node: it compiles
    // and runs from this source in-memory (resolved by content hash, not a file name) and writes
    // nothing to the scripts folder. scriptFile then holds only a display/suggested name. The user
    // converts it to a file node via "Save to scripts folder". Empty ⇒ an ordinary file node.
    std::string scriptEmbeddedSource; // SERIALIZED — the node carries its own code (self-contained project/preset)
    // type == PluginNode: "<plugin>.<id>" naming which registered node this is. The dedicated id carrier
    // (was effect.type), so `effect` is no longer overloaded for plugin nodes. SERIALIZED.
    std::string pluginNodeId;
    // type == PluginNode: the node's encoded TState as raw UTF-8 JSON ("" when empty). A std::string, not a
    // parsed nlohmann::json, because the node is value-copied into every AxisSnapshot and undo ProjectSnapshot —
    // a string is cheap on that hot path and empty for all non-plugin nodes. C++ never interprets it; it is
    // embedded as a nested JSON object in the project file (parse-on-write) so a missing plugin's state
    // round-trips losslessly. SERIALIZED.
    std::string nodeState;
    [[nodiscard]] bool scriptEmbedded() const { return type == GraphNodeType::Script && !scriptEmbeddedSource.empty(); }
    bool operator==(const ProcessingGraphNode &) const = default;
};

struct ProcessingGraphLink {
    int id = 0;
    int fromNode = 0;
    int fromPin = 0; // output slot on fromNode (0 = primary; >0 for multi-output nodes)
    int toNode = 0;
    int toPin = 0; // 0 = primary input, 1 = secondary input (math nodes)
    bool operator==(const ProcessingGraphLink &) const = default;
};

// Canonical per-node pin arity — the one definition every site reads (link validator, editor pin
// drawing, functional-signal analysis). Input/Output/Effect/Discretize/Functionalize have a fixed shape; the math
// nodes take two inputs; plugin and script nodes carry their declared counts, cached on the node so a
// disabled plugin or a missing script file still reports the right pins.
[[nodiscard]] inline int nodeInputPinCount(const ProcessingGraphNode &n) {
    switch (n.type) {
    case GraphNodeType::Output:
    case GraphNodeType::Effect:
    case GraphNodeType::Discretize:
    case GraphNodeType::Functionalize:
        return 1;
    case GraphNodeType::Add:
    case GraphNodeType::Subtract:
    case GraphNodeType::Multiply:
    case GraphNodeType::Divide:
        return 2;
    case GraphNodeType::PluginNode:
        return n.pluginInputCount;
    case GraphNodeType::Script:
        return n.scriptInputCount;
    case GraphNodeType::Input:
    case GraphNodeType::Constant:
        break;
    }
    return 0; // Input / Constant — pure sources, no inputs
}

[[nodiscard]] inline int nodeOutputPinCount(const ProcessingGraphNode &n) {
    switch (n.type) {
    case GraphNodeType::Output:
        return 0; // sink — no outputs
    case GraphNodeType::PluginNode:
        return n.pluginOutputCount;
    case GraphNodeType::Script:
        return n.scriptOutputCount;
    default:
        return 1; // Input / Effect / math / Constant / Discretize — single output
    }
}

// The single 32-bit imnodes id space, shared by node bodies, pins (input/output), static attributes,
// and links. imnodes keys nodes, attributes and links from one int id space, so every id we hand it
// must be globally unique within the editor context. A fixed N*4 stride can't express a variable pin
// count, so ids are produced through this tagged bit layout instead — the one definition of the
// encoding. Every imnodes-facing id is minted and parsed here, never by hand.
//
// Layout, MSB→LSB:
//   bits 31..9  owner  (23 bits — a ProcessingGraphNode/Link id from nextId, ~8.3M distinct)
//   bits  8..6  tag    (3 bits)
//   bits  5..0  slot   (6 bits — pin index 0..63; 0 for body/static/link)
//
// Collision-free by construction: the tag distinguishes a node body from its own pins, a static attr,
// and a link, so a link id drawn from nextId can no longer alias a pin id (the latent hazard the old
// N*4 scheme only avoided because link ids happened to be allocated after all small node ids).
struct GraphId {
    enum class Tag : uint8_t { NodeBody = 0, OutPin = 1, InPin = 2, Static = 3, Link = 4 };

    static constexpr int kSlotBits = 6;
    static constexpr int kTagBits = 3;
    // 63 — the slot field's encoding ceiling. OFS_MAX_NODE_PINS (16) enforces a lower per-direction cap;
    // a static_assert in ProcessingPanel.cpp keeps that cap from outgrowing this field.
    static constexpr int kMaxSlot = (1 << kSlotBits) - 1;

    int owner = 0; // ProcessingGraphNode::id or ProcessingGraphLink::id (raw, small)
    Tag tag = Tag::NodeBody;
    int slot = 0; // pin index; 0 for body/static/link

    // Pack/unpack through uint32_t: a 23-bit owner can set bit 31, so a signed shift would overflow on
    // encode and sign-extend on decode (recovering a negative owner). The logical shifts keep the full
    // 23-bit range round-tripping — what makes the ~8.3M figure above real rather than ~4.19M.
    constexpr int encode() const {
        const auto u = (static_cast<uint32_t>(owner) << (kTagBits + kSlotBits)) |
                       (static_cast<uint32_t>(tag) << kSlotBits) | static_cast<uint32_t>(slot);
        return static_cast<int>(u);
    }
    static constexpr GraphId decode(int id) {
        const auto u = static_cast<uint32_t>(id);
        return {static_cast<int>(u >> (kTagBits + kSlotBits)),
                static_cast<Tag>((u >> kSlotBits) & ((1u << kTagBits) - 1)), static_cast<int>(u & kMaxSlot)};
    }

    // Named constructors — the only way the rest of the code mints an imnodes id.
    static constexpr int nodeBody(int n) { return GraphId{n, Tag::NodeBody, 0}.encode(); }
    static constexpr int outPin(int n, int s = 0) { return GraphId{n, Tag::OutPin, s}.encode(); }
    static constexpr int inPin(int n, int s = 0) { return GraphId{n, Tag::InPin, s}.encode(); }
    static constexpr int staticAttr(int n) { return GraphId{n, Tag::Static, 0}.encode(); }
    static constexpr int link(int l) { return GraphId{l, Tag::Link, 0}.encode(); }
};

struct ProcessingNodeGraph {
    std::vector<ProcessingGraphNode> nodes;
    std::vector<ProcessingGraphLink> links;
    int nextId = 1;

    bool operator==(const ProcessingNodeGraph &) const = default;

    int allocId() { return nextId++; }

    ProcessingGraphNode *findNode(int nodeId) {
        for (auto &n : nodes)
            if (n.id == nodeId)
                return &n;
        return nullptr;
    }
    const ProcessingGraphNode *findNode(int nodeId) const {
        for (const auto &n : nodes)
            if (n.id == nodeId)
                return &n;
        return nullptr;
    }
    const ProcessingGraphLink *findLinkToPin(int toNode, int toPin) const {
        for (const auto &l : links)
            if (l.toNode == toNode && l.toPin == toPin)
                return &l;
        return nullptr;
    }
};

struct ProcessingRegion {
    int id = 0;
    double startTime = 0.0;
    double endTime = 0.0;
    std::string name;
    // Band color on the timeline region bar. Auto-assigned from the golden-ratio generator on
    // creation (ofs::util::goldenRatioColor) and user-overridable via the region context menu.
    ImU32 color = IM_COL32(70, 130, 180, 220);
    ProcessingNodeGraph nodeGraph;
    int hz = kDefaultRegionHz; // discretization rate (1–120) for the whole region; used wherever the graph
                               // turns a functional signal into discrete actions
    bool showSourceActions = true;
    AxisRoles axisRoles;                   // runtime bitset — kept in sync with axisRoleTags
    std::vector<std::string> axisRoleTags; // serialized axis tags; rebuilt from axisRoles on save
    bool operator==(const ProcessingRegion &) const = default;
};

// Build a graph with one Input→Output pair per assigned axis.
// Pairs are stacked vertically (150px apart). Each node's `role` is set to its axis.
inline ProcessingNodeGraph buildDefaultGraph(AxisRoles axisRoles) {
    ProcessingNodeGraph g;
    int pairIndex = 0;
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!axisRoles.test(i))
            continue;
        const auto axis = static_cast<StandardAxis>(i);
        const float y = 100.0f + static_cast<float>(pairIndex) * 150.0f;
        int inputId = g.allocId();
        int outputId = g.allocId();
        g.nodes.push_back({.id = inputId, .type = GraphNodeType::Input, .posX = 50.0f, .posY = y, .role = axis});
        g.nodes.push_back({.id = outputId, .type = GraphNodeType::Output, .posX = 400.0f, .posY = y, .role = axis});
        int linkId = g.allocId();
        g.links.push_back({.id = linkId, .fromNode = inputId, .toNode = outputId, .toPin = 0});
        ++pairIndex;
    }
    return g;
}

// Single-axis convenience overload (backwards compat)
inline ProcessingNodeGraph buildDefaultGraph(StandardAxis axis = StandardAxis::L0) {
    AxisRoles roles;
    roles.set(static_cast<size_t>(axis));
    return buildDefaultGraph(roles);
}

} // namespace ofs
