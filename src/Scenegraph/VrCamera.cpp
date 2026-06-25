#include "Scenegraph/VrCamera.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ofs::vrcam {

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDeg2Rad = kPi / 180.0f;

// Identical to the shader's rotateXY: rotate about X by angle.x, then about Y by angle.y.
glm::vec3 rotateXY(const glm::vec3 &p0, const ImVec2 &angle) {
    const float cx = std::cos(angle.x), sx = std::sin(angle.x);
    const float cy = std::cos(angle.y), sy = std::sin(angle.y);
    const glm::vec3 p{p0.x, cx * p0.y + sx * p0.z, -sx * p0.y + cx * p0.z};
    return {cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z};
}

// Inverse of rotateXY(·, angle): undo the Y rotation, then the X rotation.
glm::vec3 inverseRotateXY(const glm::vec3 &p0, const ImVec2 &angle) {
    const float cy = std::cos(angle.y), sy = std::sin(angle.y);
    const glm::vec3 p{cy * p0.x - sy * p0.z, p0.y, sy * p0.x + cy * p0.z};
    const float cx = std::cos(angle.x), sx = std::sin(angle.x);
    return {p.x, cx * p.y - sx * p.z, sx * p.y + cx * p.z};
}

// (rotateXY angle, tan(hfov/2), tan(vfov/2)) for a given camera — shared by project/unproject so
// they cannot drift from the shader or each other.
struct Frustum {
    ImVec2 angle; // == camRot.yx in the shader (pitch about X, yaw about Y)
    float tanHalfH = 0.f;
    float tanHalfV = 0.f;
};
Frustum frustum(const ImVec2 &vrRotation, float contentAspect) {
    const float hfovRad = kHfovDegrees * kDeg2Rad;
    const float inverseAspect = (contentAspect != 0.0f) ? 1.0f / contentAspect : 1.0f;
    const float vfovRad = -2.0f * std::atan(std::tan(hfovRad / 2.0f) * inverseAspect);
    const ImVec2 camRot{(vrRotation.x - 0.5f) * 2.0f * kPi, (vrRotation.y - 0.5f) * kPi};
    return {.angle = {camRot.y, camRot.x}, .tanHalfH = std::tan(0.5f * hfovRad), .tanHalfV = std::tan(0.5f * vfovRad)};
}
} // namespace

glm::vec3 sphereDir(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return {cp * std::cos(yaw), std::sin(pitch), cp * std::sin(yaw)};
}

void dirToYawPitch(const glm::vec3 &dir, float &yaw, float &pitch) {
    const glm::vec3 d = glm::normalize(dir);
    yaw = std::atan2(d.z, d.x);
    pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
}

Projected project(const glm::vec3 &sphereDirWorld, const ImVec2 &vrRotation, float vrZoom, float contentAspect) {
    const Frustum f = frustum(vrRotation, contentAspect);
    // rd = rotateXY(camDir, angle) ⇒ camDir ∝ inverseRotateXY(rd, angle).
    const glm::vec3 camDir = inverseRotateXY(glm::normalize(sphereDirWorld), f.angle);
    if (camDir.z <= 0.0f)
        return {.ndc = {}, .visible = false};
    // camDir ∝ (uv.x*tanHalfH, uv.y*tanHalfV, Zoom); recover uv from the x/z and y/z ratios.
    const ImVec2 uv{(camDir.x / camDir.z) * (vrZoom / f.tanHalfH), (camDir.y / camDir.z) * (vrZoom / f.tanHalfV)};
    // `visible` means "in front of the camera" only (the camDir.z>0 above). The FOV bound is NOT
    // applied here: the caller draws under a clip rect, so an anchor just past the screen edge
    // slides off smoothly instead of popping the whole overlay out the moment it leaves the FOV.
    return {.ndc = uv, .visible = true};
}

glm::vec3 unproject(const ImVec2 &ndc, const ImVec2 &vrRotation, float vrZoom, float contentAspect) {
    const Frustum f = frustum(vrRotation, contentAspect);
    const glm::vec3 camDir = glm::normalize(glm::vec3{ndc.x * f.tanHalfH, ndc.y * f.tanHalfV, vrZoom});
    return glm::normalize(rotateXY(camDir, f.angle));
}

ImVec2 dragRotation(const glm::vec3 &grabbed, const ImVec2 &cursorNdc, const ImVec2 &currentRotation, float vrZoom,
                    float contentAspect) {
    // tanHalf*/zoom give the camera-space ray the cursor samples (== unproject's pre-rotation camDir);
    // the Frustum angle is unused here since we solve for a new orientation. a.z = vrZoom > 0 always.
    const Frustum f = frustum(currentRotation, contentAspect);
    const glm::vec3 a = glm::normalize(glm::vec3{cursorNdc.x * f.tanHalfH, cursorNdc.y * f.tanHalfV, vrZoom});
    const glm::vec3 g = glm::normalize(grabbed);

    // We need Ry(yaw)·Rx(pitch)·a == g (rotateXY's order). The Y-rotation leaves .y untouched, so
    // pitch alone must satisfy g.y = cos(p)·a.y + sin(p)·a.z = yzMag·cos(p − phi). yzMag ≥ a.z > 0.
    const float yzMag = std::hypot(a.y, a.z);
    const float phi = std::atan2(a.z, a.y);
    const float d = std::acos(std::clamp(g.y / yzMag, -1.0f, 1.0f)); // clamps gracefully past the pole
    // Both p = phi ± d give a.y'==g.y; pick the branch nearest the current pitch to avoid a flip.
    const float curPitch = (currentRotation.y - 0.5f) * kPi;
    const float p1 = phi + d, p2 = phi - d;
    const float pitch = (std::abs(p1 - curPitch) <= std::abs(p2 - curPitch)) ? p1 : p2;

    // After the pitch rotation the xz-magnitudes of a' and g match, so the yaw is the angle between
    // their xz components (Ry here advances atan2(z,x) by −yaw).
    const float cx = std::cos(pitch), sx = std::sin(pitch);
    const glm::vec3 aP{a.x, cx * a.y + sx * a.z, -sx * a.y + cx * a.z};
    const float yaw = std::atan2(aP.z, aP.x) - std::atan2(g.z, g.x);

    ImVec2 rot{yaw / (2.0f * kPi) + 0.5f, pitch / kPi + 0.5f};
    rot.x -= std::floor(rot.x);            // yaw wraps (the equirect sphere is periodic)
    rot.y = std::clamp(rot.y, 0.0f, 1.0f); // pitch bounded to ±90°
    return rot;
}

} // namespace ofs::vrcam
