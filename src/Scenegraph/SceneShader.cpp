#include "SceneShader.h"
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace ofs::sg {

namespace {

constexpr const char *kVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
void main() {
    vNormal = mat3(uModel) * aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

constexpr const char *kFragSrc = R"(
#version 330 core
in vec3 vNormal;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    vec3 lightDir = normalize(vec3(1.0, 2.0, 3.0));
    float diffuse = max(dot(normalize(vNormal), lightDir), 0.0);
    float light = 0.3 + 0.7 * diffuse;
    fragColor = vec4(uColor.rgb * light, uColor.a);
}
)";

} // namespace

SceneShader::SceneShader() : Shader(kVertSrc, kFragSrc) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    mvpLoc = glGetUniformLocation(program, "uMVP");
    modelLoc = glGetUniformLocation(program, "uModel");
    colorLoc = glGetUniformLocation(program, "uColor");
}

void SceneShader::setMVP(const glm::mat4 &mvp) const {
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
}

void SceneShader::setModel(const glm::mat4 &model) const {
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
}

void SceneShader::setColor(const glm::vec4 &color) const {
    glUniform4fv(colorLoc, 1, glm::value_ptr(color));
}

} // namespace ofs::sg
