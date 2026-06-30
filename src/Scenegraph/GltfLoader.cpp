#include "GltfLoader.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <cgltf.h>
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ofs::sg {

namespace {

std::optional<Mesh> buildMesh(const cgltf_primitive *prim) {
    if (prim->type != cgltf_primitive_type_triangles)
        return std::nullopt;

    const cgltf_accessor *posAcc = nullptr;
    const cgltf_accessor *normAcc = nullptr;
    const cgltf_accessor *colorAcc = nullptr;
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        const auto &attr = prim->attributes[i];
        if (attr.type == cgltf_attribute_type_position)
            posAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_normal)
            normAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_color && colorAcc == nullptr)
            colorAcc = attr.data; // COLOR_0; the scene shader multiplies it in (used for baked AO)
    }
    if (posAcc == nullptr)
        return std::nullopt;

    auto vertexCount = static_cast<cgltf_size>(posAcc->count);

    std::vector<float> positions(vertexCount * 3);
    cgltf_accessor_unpack_floats(posAcc, positions.data(), vertexCount * 3);

    std::vector<float> normals(vertexCount * 3, 0.0f);
    if (normAcc != nullptr)
        cgltf_accessor_unpack_floats(normAcc, normals.data(), vertexCount * 3);

    // COLOR_0 may be VEC3 or VEC4; unpack the native width and keep RGB. Absent -> white (no tint).
    std::vector<float> colors(vertexCount * 3, 1.0f);
    if (colorAcc != nullptr) {
        const cgltf_size comps = cgltf_num_components(colorAcc->type);
        std::vector<float> raw(vertexCount * comps);
        cgltf_accessor_unpack_floats(colorAcc, raw.data(), raw.size());
        for (cgltf_size v = 0; v < vertexCount; v++)
            for (cgltf_size c = 0; c < 3 && c < comps; c++)
                colors[v * 3 + c] = raw[v * comps + c];
    }

    std::vector<float> vertices(vertexCount * 9);
    for (cgltf_size v = 0; v < vertexCount; v++) {
        vertices[v * 9 + 0] = positions[v * 3 + 0];
        vertices[v * 9 + 1] = positions[v * 3 + 1];
        vertices[v * 9 + 2] = positions[v * 3 + 2];
        vertices[v * 9 + 3] = normals[v * 3 + 0];
        vertices[v * 9 + 4] = normals[v * 3 + 1];
        vertices[v * 9 + 5] = normals[v * 3 + 2];
        vertices[v * 9 + 6] = colors[v * 3 + 0];
        vertices[v * 9 + 7] = colors[v * 3 + 1];
        vertices[v * 9 + 8] = colors[v * 3 + 2];
    }

    std::vector<uint32_t> indices;
    if (prim->indices != nullptr) {
        cgltf_size indexCount = cgltf_accessor_unpack_indices(prim->indices, nullptr, 4, 0);
        indices.resize(indexCount);
        cgltf_accessor_unpack_indices(prim->indices, indices.data(), 4, indexCount);
    } else {
        indices.resize(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; i++)
            indices[i] = static_cast<uint32_t>(i);
    }

    return Mesh::fromData(vertices, indices);
}

glm::vec4 materialColor(const cgltf_material *mat) {
    if (mat != nullptr && mat->has_pbr_metallic_roughness) {
        const auto *f = mat->pbr_metallic_roughness.base_color_factor;
        return glm::vec4{f[0], f[1], f[2], f[3]};
    }
    return glm::vec4{1.0f};
}

Transform nodeTransform(const cgltf_node *node) {
    Transform t{};
    if (node->has_translation)
        t.position = glm::vec3{node->translation[0], node->translation[1], node->translation[2]};
    if (node->has_rotation) {
        // glTF rotation is (x, y, z, w); glm::quat ctor is (w, x, y, z)
        t.rotation = glm::quat{node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]};
    }
    if (node->has_scale)
        t.scale = glm::vec3{node->scale[0], node->scale[1], node->scale[2]};
    if (node->has_matrix && !node->has_translation && !node->has_rotation && !node->has_scale) {
        // glTF stores a column-major 4x4 matching glm::mat4's layout. Decompose into TRS rather than
        // reading only the translation column, or matrix-authored nodes lose their rotation and scale.
        const glm::mat4 m = glm::make_mat4(node->matrix);
        t.position = glm::vec3{m[3]};
        t.scale = glm::vec3{glm::length(glm::vec3{m[0]}), glm::length(glm::vec3{m[1]}), glm::length(glm::vec3{m[2]})};
        glm::mat3 rot{m};
        if (t.scale.x != 0.0f)
            rot[0] /= t.scale.x;
        if (t.scale.y != 0.0f)
            rot[1] /= t.scale.y;
        if (t.scale.z != 0.0f)
            rot[2] /= t.scale.z;
        t.rotation = glm::quat_cast(rot);
    }
    return t;
}

SceneNode *loadNode(SceneGraph &graph, GltfScene &scene, const cgltf_node *node, SceneNode *parentNode,
                    const std::unordered_map<const cgltf_mesh *, size_t> &meshMap) {
    SceneNode *sceneNode = graph.createNode(parentNode);
    sceneNode->localTransform = nodeTransform(node);

    if (node->mesh != nullptr) {
        auto it = meshMap.find(node->mesh);
        if (it != meshMap.end()) {
            sceneNode->mesh = scene.meshes[it->second].get();
            sceneNode->color =
                materialColor(node->mesh->primitives_count > 0 ? node->mesh->primitives[0].material : nullptr);
        }
    }

    for (cgltf_size i = 0; i < node->children_count; i++)
        loadNode(graph, scene, node->children[i], sceneNode, meshMap);

    return sceneNode;
}

// External-buffer I/O through our UTF-8 path layer. cgltf concatenates basePath + the (percent-decoded)
// URI into `path` and hands it here; basePath is our UTF-8 string, so fromUtf8 round-trips it losslessly
// — unlike cgltf's default fopen, which decodes the narrow string as ANSI and drops non-ASCII bytes on
// Windows. read allocates the file bytes (via cgltf's allocator when supplied, else malloc); release
// frees them the same way. Harmless for self-contained .glb (no external file is ever read).
cgltf_result utf8FileRead(const cgltf_memory_options *memOpts, const cgltf_file_options *, const char *path,
                          cgltf_size *size, void **data) {
    auto content = ofs::util::readFile(ofs::util::fromUtf8(path));
    if (!content)
        return cgltf_result_file_not_found;
    const cgltf_size n = content->size();
    void *buf = (memOpts && memOpts->alloc_func) ? memOpts->alloc_func(memOpts->user_data, n) : std::malloc(n);
    if (buf == nullptr)
        return cgltf_result_out_of_memory;
    std::memcpy(buf, content->data(), n);
    *size = n;
    *data = buf;
    return cgltf_result_success;
}

void utf8FileRelease(const cgltf_memory_options *memOpts, const cgltf_file_options *, void *data, cgltf_size) {
    if (data == nullptr)
        return;
    if (memOpts && memOpts->free_func)
        memOpts->free_func(memOpts->user_data, data);
    else
        std::free(data);
}

// Shared core: parse already-loaded glTF bytes. `basePath` is passed to cgltf_load_buffers to
// resolve external .bin buffers (file path); pass nullptr for in-memory self-contained .glb data.
std::unique_ptr<GltfScene> loadGltfFromBytes(SceneGraph &graph, const void *bytes, size_t size,
                                             const std::string &label, const char *basePath) {
    cgltf_options opts{};
    // Route external-buffer reads through our UTF-8 file layer (see utf8FileRead) so a non-ASCII asset
    // directory resolves on Windows.
    opts.file.read = &utf8FileRead;
    opts.file.release = &utf8FileRelease;
    cgltf_data *data{};

    if (cgltf_parse(&opts, bytes, size, &data) != cgltf_result_success) {
        OFS_CORE_ERROR("cgltf: failed to parse '{}'", label);
        return nullptr;
    }
    // For self-contained .glb files the buffers are embedded, so this does not open external files.
    if (cgltf_load_buffers(&opts, data, basePath) != cgltf_result_success) {
        OFS_CORE_ERROR("cgltf: failed to load buffers for '{}'", label);
        cgltf_free(data);
        return nullptr;
    }

    auto scene = std::make_unique<GltfScene>();

    std::unordered_map<const cgltf_mesh *, size_t> meshMap;
    for (cgltf_size i = 0; i < data->meshes_count; i++) {
        const cgltf_mesh *cgMesh = &data->meshes[i];
        if (cgMesh->primitives_count == 0)
            continue;
        auto mesh = buildMesh(&cgMesh->primitives[0]);
        if (!mesh)
            continue;
        meshMap[cgMesh] = scene->meshes.size();
        scene->meshes.push_back(std::make_unique<Mesh>(std::move(*mesh)));
    }

    const cgltf_scene *gltfScene =
        (data->scene != nullptr) ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (gltfScene != nullptr) {
        for (cgltf_size i = 0; i < gltfScene->nodes_count; i++)
            scene->roots.push_back(loadNode(graph, *scene, gltfScene->nodes[i], nullptr, meshMap));
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (data->nodes[i].parent == nullptr)
                scene->roots.push_back(loadNode(graph, *scene, &data->nodes[i], nullptr, meshMap));
        }
    }

    OFS_CORE_INFO("cgltf: loaded '{}' ({} meshes, {} root nodes)", label, scene->meshes.size(), scene->roots.size());

    cgltf_free(data);
    return scene;
}

} // namespace

std::unique_ptr<GltfScene> loadGltf(SceneGraph &graph, const std::string &path) {
    // Read the file ourselves through a UTF-8-aware path so non-ASCII paths work;
    // cgltf_parse_file would fopen the narrow string and fail under a non-ASCII path.
    auto content = ofs::util::readFile(ofs::util::fromUtf8(path));
    if (!content) {
        OFS_CORE_ERROR("cgltf: failed to open '{}'", path);
        return nullptr;
    }
    return loadGltfFromBytes(graph, content->data(), content->size(), path, path.c_str());
}

std::unique_ptr<GltfScene> loadGltfFromMemory(SceneGraph &graph, const unsigned char *data, size_t size,
                                              const std::string &label) {
    return loadGltfFromBytes(graph, data, size, label, nullptr);
}

} // namespace ofs::sg
