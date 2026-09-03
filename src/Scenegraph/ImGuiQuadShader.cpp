#include "ImGuiQuadShader.h"

#include "imgui.h"
#include <cstddef>
#include <glad/gl.h>

namespace ofs {

static const char *quadVertexSource = R"(#version 330 core
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

ImGuiQuadShader::ImGuiQuadShader(const char *fragmentSource) : Shader(quadVertexSource, fragmentSource) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    projMtxLoc = glGetUniformLocation(program, "ProjMtx");
}

void ImGuiQuadShader::setProjMtx(const float *mat4) const {
    glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4);
}

void ImGuiQuadShader::useForImGuiDraw() const {
    if (program == 0) // not compiled (headless, or a compile/link failure)
        return;
    glUseProgram(program);

    // Re-point the vertex attributes, do not assume ImGui already did. The backend compiles its own
    // vertex shader without layout qualifiers (the GLSL 130 source it picks for "#version 330"), so the
    // *linker* chooses where Position/UV/Color land, and the VAO handed to this callback is configured
    // for those locations. Ours are pinned to 0/1/2 above. Some drivers assign 0/1/2 in declaration
    // order and the mismatch never surfaces; the Intel Windows driver does not, and the draw then reads
    // each attribute from the wrong offset — no GL error, just nothing on screen. ImGui's VBO is still
    // the bound array buffer here (the backend binds it once per draw list, before walking the
    // commands), and DrawCallback_ResetRenderState restores ImGui's own layout after the quad.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                          reinterpret_cast<void *>(offsetof(ImDrawVert, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                          reinterpret_cast<void *>(offsetof(ImDrawVert, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert),
                          reinterpret_cast<void *>(offsetof(ImDrawVert, col)));

    const ImDrawData *dd = ImGui::GetDrawData();
    const float l = dd->DisplayPos.x;
    const float r = dd->DisplayPos.x + dd->DisplaySize.x;
    const float t = dd->DisplayPos.y;
    const float b = dd->DisplayPos.y + dd->DisplaySize.y;
    const float ortho[4][4] = {
        {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {(r + l) / (l - r), (t + b) / (b - t), 0.0f, 1.0f},
    };
    setProjMtx(&ortho[0][0]);
}

} // namespace ofs
