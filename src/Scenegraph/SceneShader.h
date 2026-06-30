#pragma once
#include "Scenegraph/Shader.h"
#include <glm/glm.hpp>

namespace ofs::sg {

class SceneShader : public ofs::Shader {
    int32_t mvpLoc{-1};
    int32_t modelLoc{-1};
    int32_t colorLoc{-1};
    int32_t eyeLoc{-1};
    int32_t refDistLoc{-1};

  public:
    SceneShader();

    void setMVP(const glm::mat4 &mvp) const;
    void setModel(const glm::mat4 &model) const;
    void setColor(const glm::vec4 &color) const;
    void setEye(const glm::vec3 &eye) const;
    void setRefDist(float dist) const;
};

} // namespace ofs::sg
