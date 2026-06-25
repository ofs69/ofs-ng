#include "UI/ProcessingGraphOps.h"
#include "Services/EffectRegistry.h"
#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <vector>

namespace ofs {

bool isNodeOutputFunctional(int nodeId, const ProcessingNodeGraph &graph, const EffectRegistryState &effectReg) {
    const auto *node = graph.findNode(nodeId);
    if (!node)
        return false;
    if (node->type == GraphNodeType::Constant)
        return true;
    if (node->type == GraphNodeType::Input)
        return false;
    if (node->type == GraphNodeType::Discretize)
        return false; // resamples onto the Hz grid — its output is always discrete, whatever the input
    if (node->type == GraphNodeType::Effect) {
        auto it = effectReg.effects.find(node->effect.type);
        if (it == effectReg.effects.end() || it->second.kind != EffectKind::Functional)
            return false;
        if (it->second.ignoresInput)
            return true;
        const auto *link = graph.findLinkToPin(node->id, 0);
        return link && isNodeOutputFunctional(link->fromNode, graph, effectReg);
    }
    // A dynamic node (plugin / script) is functional iff its declared signal is functional, its first input
    // is wired to a functional source, and every other *connected* input is functional too. An unwired
    // secondary input reads a constant (functional), so it doesn't force a discrete grid.
    auto dynamicFunctional = [&](uint8_t signal, int inputCount) {
        if (signal != static_cast<uint8_t>(OfsSignalFunctional))
            return false;
        if (inputCount == 0)
            return true;
        const auto *link0 = graph.findLinkToPin(node->id, 0);
        if (!link0 || !isNodeOutputFunctional(link0->fromNode, graph, effectReg))
            return false;
        for (int i = 1; i < inputCount; ++i) {
            const auto *l = graph.findLinkToPin(node->id, i);
            if (l && !isNodeOutputFunctional(l->fromNode, graph, effectReg))
                return false;
        }
        return true;
    };
    if (node->type == GraphNodeType::PluginNode)
        return dynamicFunctional(node->pluginSignal, node->pluginInputCount);
    if (node->type == GraphNodeType::Script)
        return dynamicFunctional(node->scriptSignal, node->scriptInputCount);
    if (isMathNode(node->type)) {
        const auto *linkA = graph.findLinkToPin(node->id, 0);
        const auto *linkB = graph.findLinkToPin(node->id, 1);
        bool aFunctional = linkA && isNodeOutputFunctional(linkA->fromNode, graph, effectReg);
        bool bFunctional = !linkB || isNodeOutputFunctional(linkB->fromNode, graph, effectReg);
        return aFunctional && bFunctional;
    }
    return false;
}

bool isLinkFunctional(const ProcessingGraphLink &link, const ProcessingNodeGraph &graph,
                      const EffectRegistryState &effectReg) {
    return isNodeOutputFunctional(link.fromNode, graph, effectReg);
}

void autoConnectNewNode(int newNodeId, int droppedPin, bool ignoresInput, ProcessingNodeGraph &graph) {
    auto &links = graph.links;
    const GraphId dropped = GraphId::decode(droppedPin);
    if (dropped.tag == GraphId::Tag::OutPin) {
        // Dropped from an output pin (slot `dropped.slot`): splice the new node in after it. Existing links
        // that drew from that exact output are redirected to draw from the new node's primary output.
        if (ignoresInput)
            return;
        int fromNode = dropped.owner;
        int fromPin = dropped.slot;
        for (auto &l : links)
            if (l.fromNode == fromNode && l.fromPin == fromPin) {
                l.fromNode = newNodeId;
                l.fromPin = 0;
            }
        links.push_back(
            {.id = graph.allocId(), .fromNode = fromNode, .fromPin = fromPin, .toNode = newNodeId, .toPin = 0});
    } else {
        int toNode = dropped.owner;
        int toPin = dropped.slot;
        int oldSource = -1;
        int oldSourcePin = 0;
        for (const auto &l : links)
            if (l.toNode == toNode && l.toPin == toPin) {
                oldSource = l.fromNode;
                oldSourcePin = l.fromPin;
                break;
            }
        links.erase(std::ranges::remove_if(
                        links, [toNode, toPin](const auto &l) { return l.toNode == toNode && l.toPin == toPin; })
                        .begin(),
                    links.end());
        links.push_back({.id = graph.allocId(), .fromNode = newNodeId, .toNode = toNode, .toPin = toPin});
        if (oldSource != -1 && !ignoresInput)
            links.push_back({.id = graph.allocId(),
                             .fromNode = oldSource,
                             .fromPin = oldSourcePin,
                             .toNode = newNodeId,
                             .toPin = 0});
    }
}

bool wouldCreateCycle(int fromNode, int toNode, const ProcessingNodeGraph &graph) {
    std::vector<int> stack = {toNode};
    std::unordered_set<int> visited;
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        if (cur == fromNode)
            return true;
        if (!visited.insert(cur).second)
            continue;
        for (const auto &l : graph.links)
            if (l.fromNode == cur)
                stack.push_back(l.toNode);
    }
    return false;
}

} // namespace ofs
