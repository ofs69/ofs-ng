#pragma once

#include "Core/ProcessingRegion.h"

namespace ofs {

struct EffectRegistryState;

// Pure node-graph operations for the Processing panel's imnodes editor. They touch only
// ProcessingNodeGraph / EffectRegistryState — no ImGui or imnodes state — so they are unit-testable in
// isolation (tests/core/test_processing_graph_ops.cpp) and kept out of the per-frame render TU.

inline bool isMathNode(GraphNodeType t) {
    return t == GraphNodeType::Add || t == GraphNodeType::Subtract || t == GraphNodeType::Multiply ||
           t == GraphNodeType::Divide;
}

// True if adding a fromNode→toNode edge would close a cycle (toNode already reaches fromNode).
bool wouldCreateCycle(int fromNode, int toNode, const ProcessingNodeGraph &graph);

// Splice a freshly-created node into the graph at the pin the user dropped a link on. Dropping on an
// output pin inserts the node downstream (rerouting that output's consumers through the new node);
// dropping on an input pin inserts it there, feeding the previous source (if any) into the new node's
// primary input unless the node ignores input (a pure generator).
void autoConnectNewNode(int newNodeId, int droppedPin, bool ignoresInput, ProcessingNodeGraph &graph);

// Whether a node's output carries a functional (continuous) signal rather than a discrete one,
// propagated recursively through its inputs and the effect/plugin/script signal kinds. Drives the
// functional-vs-discrete link coloring.
bool isNodeOutputFunctional(int nodeId, const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg);
bool isLinkFunctional(const ProcessingGraphLink &link, const ProcessingNodeGraph &graph,
                      const EffectRegistryState &effectReg);

} // namespace ofs
