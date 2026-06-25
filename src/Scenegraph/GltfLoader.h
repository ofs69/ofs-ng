#pragma once
#include "Mesh.h"
#include "SceneGraph.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ofs::sg {

struct GltfScene {
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<SceneNode *> roots; // top-level nodes — owned by the SceneGraph, not this struct
};

// Loads a .glb or .gltf file into the SceneGraph.
// Returns nullptr on failure. The caller must keep the returned GltfScene alive for as long as
// the loaded nodes are rendered (meshes are owned here; SceneGraph owns the nodes).
[[nodiscard]] std::unique_ptr<GltfScene> loadGltf(SceneGraph &graph, const std::string &path);

// Loads a self-contained .glb from an in-memory buffer (e.g. the copy baked into the binary).
// External buffers cannot be resolved, so the data must be a fully self-contained .glb. `label` is
// used only for log messages. Returns nullptr on failure.
[[nodiscard]] std::unique_ptr<GltfScene> loadGltfFromMemory(SceneGraph &graph, const unsigned char *data, size_t size,
                                                            const std::string &label);

} // namespace ofs::sg
