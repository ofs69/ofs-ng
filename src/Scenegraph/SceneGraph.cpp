#include "SceneGraph.h"

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
    shader.use();
    for (const auto &node : nodes_) {
        if (node->mesh == nullptr)
            continue;
        const auto mvp = proj * view * node->worldMatrix;
        shader.setMVP(mvp);
        shader.setModel(node->worldMatrix);
        shader.setColor(node->color);
        node->mesh->draw();
    }
}

} // namespace ofs::sg
