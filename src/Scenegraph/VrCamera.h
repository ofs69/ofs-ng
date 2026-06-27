#pragma once

#include "Scenegraph/Shader.h" // VrShader::hfovDegrees — the single source for the equirect HFOV
#include "imgui.h"
#include <glm/glm.hpp>

// Forward/inverse projection for the VR equirect view, kept byte-for-byte consistent with the
// VrShader fragment shader (Shader.cpp). The shader maps a screen pixel → view ray → equirect
// texture; for the simulator overlay we need the opposite: pin the overlay to a fixed direction on
// the sphere (rotation-invariant, so it tracks the video as the user rotates) and find where that
// direction lands on screen. `project` is the analytic inverse of the shader; `unproject` mirrors
// the shader exactly so a drag round-trips. See Shader.cpp vrFragBody for the reference math.
namespace ofs::vrcam {

inline constexpr float kHfovDegrees = VrShader::hfovDegrees; // single source: the shader's HFOV

// World ray direction for an equirect (yaw,pitch): yaw = longitude = atan2(z,x), pitch = asin(y).
glm::vec3 sphereDir(float yaw, float pitch);

// Inverse of sphereDir: recover (yaw,pitch) from a (not necessarily unit) direction.
void dirToYawPitch(const glm::vec3 &dir, float &yaw, float &pitch);

// Project a world sphere direction to centered screen NDC in [-0.5,0.5]^2 (the shader's `uv`
// space, where 0 is the content-region centre). `visible` is false only when the direction is
// behind the camera; it stays true outside the FOV (|ndc|>0.5) so the caller can draw and let a
// clip rect trim the overflow — that avoids the overlay popping out at the screen edge.
struct Projected {
    ImVec2 ndc;
    bool visible = false;
};
Projected project(const glm::vec3 &sphereDirWorld, const ImVec2 &vrRotation, float vrZoom, float contentAspect);

// Inverse of project: a centered screen NDC back to the world sphere direction it samples.
glm::vec3 unproject(const ImVec2 &ndc, const ImVec2 &vrRotation, float vrZoom, float contentAspect);

// Grab-to-pan: return the vrRotation that pins the world direction `grabbed` to the cursor's
// centered screen NDC. Grab `grabbed = unproject(cursorNdc, …)` on mouse-down, then call this each
// frame with the live cursor to keep that point exactly under the pointer (the connected feel a flat
// `delta * sensitivity` can't give — the perspective is non-linear toward the edges). `currentRotation`
// only selects the continuous pitch branch and is the graceful fallback near the poles, where the
// cursor ray's latitude cone can't reach `grabbed`. Uses the same shared Frustum as project/unproject.
ImVec2 dragRotation(const glm::vec3 &grabbed, const ImVec2 &cursorNdc, const ImVec2 &currentRotation, float vrZoom,
                    float contentAspect);

} // namespace ofs::vrcam
