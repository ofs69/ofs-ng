#include "Shader.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <glad/gl.h>

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
    checkCompileErrors(vertex, "VERTEX", false);

    fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fragmentSource, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT", false);

    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    checkCompileErrors(program, "PROGRAM", true);

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

void Shader::checkCompileErrors(uint32_t object, const char *label, bool isProgram) {
    int32_t success = 0;
    if (isProgram)
        glGetProgramiv(object, GL_LINK_STATUS, &success);
    else
        glGetShaderiv(object, GL_COMPILE_STATUS, &success);
    if (success)
        return;

    char infoLog[1024];
    if (isProgram)
        glGetProgramInfoLog(object, sizeof(infoLog), nullptr, infoLog);
    else
        glGetShaderInfoLog(object, sizeof(infoLog), nullptr, infoLog);
    OFS_CORE_ERROR("{} of type: {}\n{}\n-- --------------------------------------------------- --",
                   isProgram ? "PROGRAM_LINKING_ERROR" : "SHADER_COMPILATION_ERROR", label, infoLog);
}
} // namespace ofs
