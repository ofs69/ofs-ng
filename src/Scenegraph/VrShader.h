#pragma once

#include "Scenegraph/Shader.h"

namespace ofs {

// Equirectangular VR projection shader: maps a screen pixel → view ray → equirect texture coordinate so
// a 180/360 video sphere can be looked around. Drawn from a dropped ImGui draw callback (VideoPlayerWindow
// / TimelinePreviewPopup). hfovDegrees is the single source of the horizontal field of view, shared with
// VrCamera.
class VrShader : public Shader {
  public:
    static constexpr float hfovDegrees = 75.0f;

    VrShader();

    void setProjMtx(const float *mat4) const;

    void setRotation(float x, float y) const;

    void setZoom(float zoom) const;

    void setAspectRatio(float aspect) const;

    void setVideoAspectRatio(float aspect) const;

  private:
    int32_t projMtxLoc = -1;
    int32_t rotationLoc = -1;
    int32_t zoomLoc = -1;
    int32_t aspectLoc = -1;
    int32_t videoAspectLoc = -1;
};
} // namespace ofs
