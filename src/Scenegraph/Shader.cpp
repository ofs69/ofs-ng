#include "Shader.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <glad/gl.h>
#include <vector>

namespace ofs {
Shader::Shader(const char *vertexSource, const char *fragmentSource) {
    // Null backend: glad is never loaded, so every gl* entry point is a null pointer. Skip compilation
    // entirely (program stays 0). Shaders are only ever *used* inside dropped ImGui draw callbacks, so
    // a zero program is never bound. SceneShader is a value member of SceneGraph (and so of
    // ScriptSimulator), which is why this ctor runs at startup even with nothing to render.
    if constexpr (ofs::kHeadless)
        return;

    uint32_t vertex = 0, fragment = 0;

    vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vertexSource, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fragmentSource, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    checkCompileErrors(program, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader() {
    if (program != 0) {
        glDeleteProgram(program);
    }
}

void Shader::use() const {
    if (program == 0) // not compiled (headless, or a compile/link failure); only used from draw callbacks
        return;
    glUseProgram(program);
}

void Shader::checkCompileErrors(uint32_t shader, const std::string &type) {
    int32_t success = 0;
    char infoLog[1024];
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            OFS_CORE_ERROR(
                "SHADER_COMPILATION_ERROR of type: {}\n{}\n-- --------------------------------------------------- --",
                type, infoLog);
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            OFS_CORE_ERROR(
                "PROGRAM_LINKING_ERROR of type: {}\n{}\n-- --------------------------------------------------- --",
                type, infoLog);
        }
    }
}

// --- VrShader ---

static const char *vrVertexSource = R"(#version 330 core
        layout (location = 0) in vec2 Position;
        layout (location = 1) in vec2 UV;
        layout (location = 2) in vec4 Color;

        uniform mat4 ProjMtx;

        out vec2 Frag_UV;
        out vec4 Frag_Color;

        void main() {
            Frag_UV = UV;
            Frag_Color = Color;
            gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
        }
    )";

static const char *vrFragBody = R"(
        precision highp float;

        uniform sampler2D Texture;
        uniform vec2 Rotation;
        uniform float Zoom;
        uniform float AspectRatio;
        uniform float VideoAspectRatio;

        in vec2 Frag_UV;
        in vec4 Frag_Color;

        out vec4 Out_Color;

        #define PI 3.1415926535
        #define DEG2RAD 0.01745329251994329576923690768489

        vec3 rotateXY(vec3 p, vec2 angle) {
            vec2 c = cos(angle), s = sin(angle);
            p = vec3(p.x, c.x*p.y + s.x*p.z, -s.x*p.y + c.x*p.z);
            return vec3(c.y*p.x + s.y*p.z, p.y, -s.y*p.x + c.y*p.z);
        }

        float map(float value, float min1, float max1, float min2, float max2) {
            return min2 + (value - min1) * (max2 - min2) / (max1 - min1);
        }

        void main() {
            float inverse_aspect = 1.0 / AspectRatio;
            float hfovRad = hfovDegrees * DEG2RAD;
            float vfovRad = -2.0 * atan(tan(hfovRad/2.0)*inverse_aspect);

            vec2 uv = vec2(Frag_UV.s - 0.5, Frag_UV.t - 0.5);

            vec3 camDir = normalize(vec3(uv.xy * vec2(tan(0.5 * hfovRad), tan(0.5 * vfovRad)), Zoom));
            vec3 camRot = vec3((Rotation - 0.5) * vec2(2.0 * PI, PI), 0.0);

            vec3 rd = normalize(rotateXY(camDir, camRot.yx));

            // acos(rd.y) (not -rd.y): equirect row v=0 is the zenith, so an up-pointing ray must
            // map to the top of the texture. Negating here flips the video vertically.
            vec2 texCoord = vec2(atan(rd.z, rd.x) + PI, acos(rd.y)) / vec2(2.0 * PI, PI);
            if (VideoAspectRatio <= 1.0) {
                texCoord.y = map(texCoord.y, 0.0, 1.0, 0.0, 0.5);
            }
            Out_Color = texture(Texture, texCoord);
        }
    )";

static std::string buildVrFragSource() {
    // {:f} keeps the trailing decimals (e.g. "75.000000") so the value is a valid GLSL float literal.
    return fmt::format("#version 330 core\n        const float hfovDegrees = {:f};\n{}", VrShader::hfovDegrees,
                       vrFragBody);
}

VrShader::VrShader() : Shader(vrVertexSource, buildVrFragSource().c_str()) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    projMtxLoc = glGetUniformLocation(program, "ProjMtx");
    rotationLoc = glGetUniformLocation(program, "Rotation");
    zoomLoc = glGetUniformLocation(program, "Zoom");
    aspectLoc = glGetUniformLocation(program, "AspectRatio");
    videoAspectLoc = glGetUniformLocation(program, "VideoAspectRatio");

    use();
    glUniform1i(glGetUniformLocation(program, "Texture"), 0);
}

void VrShader::setProjMtx(const float *mat4) const {
    glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4);
}

void VrShader::setRotation(float x, float y) const {
    glUniform2f(rotationLoc, x, y);
}

void VrShader::setZoom(float zoom) const {
    glUniform1f(zoomLoc, zoom);
}

void VrShader::setAspectRatio(float aspect) const {
    glUniform1f(aspectLoc, aspect);
}

void VrShader::setVideoAspectRatio(float aspect) const {
    glUniform1f(videoAspectLoc, aspect);
}
} // namespace ofs
