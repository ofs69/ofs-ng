#pragma once

namespace ofs {

// The arity-derived bucket a processing node sits under in the add-node menu (and the tag shown on the
// node body). A *closed* set — unlike a plugin's free-form group name — so it is an enum, not a string:
// a node is a Generator (no inputs, synthesizes a signal), a Modifier (reshapes one input), or a
// Combiner (≥2 inputs). The host's built-in effects, the library/user scripts, and a plugin's own nodes
// all map onto these three. Label and icon are presentation, so they live with the renderer (the UI),
// not here; this header is the dependency-free taxonomy the registries key off.
enum class NodeCategory { Generate, Modify, Combine };

// Bucket a node by its declared input count: 0 → Generate, 1 → Modify, ≥2 → Combine.
inline NodeCategory nodeCategoryForInputs(int inputCount) {
    if (inputCount <= 0)
        return NodeCategory::Generate;
    if (inputCount == 1)
        return NodeCategory::Modify;
    return NodeCategory::Combine;
}

} // namespace ofs
