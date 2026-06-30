#include "SceneGraph.h"
#include <glad/gl.h>

namespace ofs::sg {

SceneNode *SceneGraph::createNode(SceneNode *parent) {
    auto node = std::make_unique<SceneNode>();
    SceneNode *ptr = node.get();
    if (parent != nullptr) {
        ptr->parent = parent;
        parent->children.push_back(ptr);
    }
    nodes_.push_back(std::move(node));
    return ptr;
}

void SceneGraph::destroyNode(SceneNode *node) {
    // Recurse over a snapshot: each child's destroyNode erases itself from node->children (through its
    // parent pointer below), which would invalidate a live range-for over the same vector.
    const std::vector<SceneNode *> children = node->children;
    for (SceneNode *child : children)
        destroyNode(child);

    if (node->parent != nullptr) {
        auto &siblings = node->parent->children;
        std::erase(siblings, node);
    }

    std::erase_if(nodes_, [node](const std::unique_ptr<SceneNode> &n) { return n.get() == node; });
}

void SceneGraph::updateRecursive(SceneNode *node, const glm::mat4 &parentWorld) {
    node->worldMatrix = parentWorld * node->localTransform.localMatrix();
    for (SceneNode *child : node->children)
        updateRecursive(child, node->worldMatrix);
}

void SceneGraph::updateTransforms() {
    for (const auto &node : nodes_) {
        if (node->parent == nullptr)
            updateRecursive(node.get(), glm::mat4(1.0f));
    }
}

void SceneGraph::render(const glm::mat4 &view, const glm::mat4 &proj) {
    // ImGui's GL3 backend leaves face culling disabled; enable it for the solid model so back faces
    // don't bleed through and muddy the shading. Save/restore because the host callback that wraps this
    // render only restores viewport/scissor/depth state, not cull state.
    const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
    GLint prevFrontFace = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &prevFrontFace);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW); // glTF winding

    shader.use();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    shader.setEye(eye);
    shader.setRefDist(glm::length(eye)); // all simulator cameras target the origin, so |eye| is eye->focus
    for (const auto &node : nodes_) {
        if (node->mesh == nullptr)
            continue;
        const auto mvp = proj * view * node->worldMatrix;
        shader.setMVP(mvp);
        shader.setModel(node->worldMatrix);
        shader.setColor(node->color);
        node->mesh->draw();
    }

    glFrontFace(static_cast<GLenum>(prevFrontFace));
    if (prevCull == GL_FALSE)
        glDisable(GL_CULL_FACE);
}

} // namespace ofs::sg
