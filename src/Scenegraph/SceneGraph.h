#pragma once
#include "Mesh.h"
#include "SceneShader.h"
#include "Transform.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ofs::sg {

struct SceneNode {
    Transform localTransform;
    glm::mat4 worldMatrix{1.0f};
    Mesh *mesh{};
    glm::vec4 color{1.0f};
    SceneNode *parent{};
    std::vector<SceneNode *> children;
};

class SceneGraph {
  public:
    SceneNode *createNode(SceneNode *parent = nullptr);
    void destroyNode(SceneNode *node);

    void updateTransforms();
    void render(const glm::mat4 &view, const glm::mat4 &proj);

  private:
    SceneShader shader;
    std::vector<std::unique_ptr<SceneNode>> nodes_;

    void updateRecursive(SceneNode *node, const glm::mat4 &parentWorld);
};

} // namespace ofs::sg
