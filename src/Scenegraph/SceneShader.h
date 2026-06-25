#pragma once
#include "Scenegraph/Shader.h"
#include <glm/glm.hpp>

namespace ofs::sg {

class SceneShader : public ofs::Shader {
    int32_t mvpLoc{-1};
    int32_t modelLoc{-1};
    int32_t colorLoc{-1};

  public:
    SceneShader();

    void setMVP(const glm::mat4 &mvp) const;
    void setModel(const glm::mat4 &model) const;
    void setColor(const glm::vec4 &color) const;
};

} // namespace ofs::sg
