#include "SceneShader.h"
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace ofs::sg {

namespace {

constexpr const char *kVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorldPos;
out vec3 vColor;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

// Lighting tuned for legibility over arbitrary video, not photorealism: a hemispheric ambient makes
// surface orientation readable even where no direct light reaches, and a fresnel rim brightens the
// silhouette so the model separates from whatever is drawn behind it.
constexpr const char *kFragSrc = R"(
#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec3 vColor;
uniform vec4 uColor;
uniform vec3 uEyePos;
uniform float uRefDist;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uEyePos - vWorldPos);

    vec3 keyDir = normalize(vec3(1.0, 2.0, 3.0));
    vec3 fillDir = normalize(vec3(-1.5, 0.5, -1.0));
    float key = max(dot(N, keyDir), 0.0) * 0.65;
    float fill = max(dot(N, fillDir), 0.0) * 0.18;

    // Sky above, ground below, blended by the surface's up-facing component: cheap directional ambient.
    vec3 skyCol = vec3(0.42, 0.45, 0.50);
    vec3 groundCol = vec3(0.10, 0.10, 0.13);
    vec3 ambient = mix(groundCol, skyCol, N.y * 0.5 + 0.5);

    vec3 H = normalize(keyDir + V);
    float spec = pow(max(dot(N, H), 0.0), 24.0) * 0.25;
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.5;

    // vColor carries baked grey-scale ambient occlusion; it darkens the diffuse term in contact
    // crevices (where the twist nubs meet the body) but is left out of the additive rim/spec highlights.
    vec3 albedo = uColor.rgb * vColor;
    vec3 lit = albedo * (ambient + key + fill) + vec3(spec) + vec3(rim);

    // Depth cue: darken with distance from the camera's focus point so motion straight down the view
    // axis (surge) reads spatially and the model gains front-to-back volume. uRefDist is the eye->origin
    // distance (every simulator camera targets the origin), so depth is signed about the model centre.
    float depth = length(uEyePos - vWorldPos) - uRefDist;
    lit *= clamp(1.0 - depth * 0.14, 0.72, 1.12);

    fragColor = vec4(lit, uColor.a);
}
)";

} // namespace

SceneShader::SceneShader() : Shader(kVertSrc, kFragSrc) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    mvpLoc = glGetUniformLocation(program, "uMVP");
    modelLoc = glGetUniformLocation(program, "uModel");
    colorLoc = glGetUniformLocation(program, "uColor");
    eyeLoc = glGetUniformLocation(program, "uEyePos");
    refDistLoc = glGetUniformLocation(program, "uRefDist");
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

void SceneShader::setEye(const glm::vec3 &eye) const {
    glUniform3fv(eyeLoc, 1, glm::value_ptr(eye));
}

void SceneShader::setRefDist(float dist) const {
    glUniform1f(refDistLoc, dist);
}

} // namespace ofs::sg
