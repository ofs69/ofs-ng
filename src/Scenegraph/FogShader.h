#pragma once

#include "Scenegraph/Shader.h"

#include <cstdint>

namespace ofs {

// Soft drifting-fog background for the welcome screen. A fullscreen quad runs domain-warped fBm value
// noise mapped onto a single themed tint at a low alpha ceiling, so it reads as a faint haze over the
// panel rather than a colored effect. Animation is time-driven (uPhase is advanced by wall-clock
// seconds), so the drift runs at the same speed at any frame rate. Cheap by construction — 4 noise
// octaves, no textures — so it is fine to run at full window resolution. Drawn via
// ImDrawList::AddCallback, the same raw-GL-into-ImGui pattern as WaveformRenderer.
class FogShader : public Shader {
  public:
    FogShader();

    void setProjMtx(const float *mat4) const;
    // Monotonically increasing per-frame counter; drives both translation and evolution of the fog.
    void setPhase(float phase) const;
    // width/height of the quad, so the noise domain stays square regardless of window proportions.
    void setAspect(float aspect) const;
    // Fog tint in rgb (derived from the theme window background on the CPU side) with the alpha ceiling
    // a fully-opaque billow reaches in .a — kept small so text stays readable. The fog pops at the edges
    // and fades toward the center.
    void setColor(float r, float g, float b, float a) const;
    // Soft central vignette over the content area: rgb tint with its peak alpha in .a. Theme-tuned on the
    // CPU side — toward black on dark themes (deepens the panel) and toward white on light themes
    // (brightens it) so the content reads as framed either way.
    void setCenter(float r, float g, float b, float a) const;

  private:
    int32_t projMtxLoc = -1;
    int32_t phaseLoc = -1;
    int32_t aspectLoc = -1;
    int32_t colorLoc = -1;
    int32_t centerLoc = -1;
};
} // namespace ofs
