#pragma once

#include "Scenegraph/Shader.h"

#include <cstdint>

namespace ofs {

// Base for the shaders that draw *through* ImGui: an ImDrawList::AddCallback swaps the program in, and
// the AddImage/Image quad that follows is what actually issues the draw. The geometry is therefore
// ImGui's own ImDrawVert stream, so every such shader shares one vertex stage (Position/UV/Color →
// Frag_UV/Frag_Color, projected by ProjMtx) and a derived class supplies only a fragment shader.
class ImGuiQuadShader : public Shader {
  public:
    // Bind the program for a draw over ImGui's vertex stream, and upload the projection ImGui is
    // rendering this frame's draw data with. Call it first in the draw callback — the other uniform
    // setters need the program bound.
    void useForImGuiDraw() const;

    // Projection for a draw the caller sets up itself (an off-screen quad with its own VAO). The
    // ImGui path takes its projection from useForImGuiDraw().
    void setProjMtx(const float *mat4) const;

  protected:
    explicit ImGuiQuadShader(const char *fragmentSource);

  private:
    int32_t projMtxLoc = -1;
};

} // namespace ofs
