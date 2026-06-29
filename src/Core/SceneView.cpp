#include "Core/SceneView.h"

#include <cmath>
#include <numbers>

namespace ofs {

static float lerp1(float a, float b, float t) {
    return a + (b - a) * t;
}

static ImVec2 lerp2(const ImVec2 &a, const ImVec2 &b, float t) {
    return {lerp1(a.x, b.x, t), lerp1(a.y, b.y, t)};
}

// Interpolate an angle that wraps with `period` along the shortest arc. The signed delta is folded
// into [-period/2, period/2] so a yaw spanning the seam takes the short way around.
static float lerpAngle(float a, float b, float t, float period) {
    float d = b - a;
    d -= period * std::round(d / period);
    return a + d * t;
}

VideoFraming lerp(const VideoFraming &a, const VideoFraming &b, float t) {
    VideoFraming r;
    r.zoomFactor = lerp1(a.zoomFactor, b.zoomFactor, t);
    r.translation = lerp2(a.translation, b.translation, t);
    // vrRotation.x is normalized yaw with a 2π period mapped to 1.0; pitch (y) is bounded, not wrapped.
    r.vrRotation.x = lerpAngle(a.vrRotation.x, b.vrRotation.x, t, 1.0f);
    r.vrRotation.y = lerp1(a.vrRotation.y, b.vrRotation.y, t);
    r.vrZoom = lerp1(a.vrZoom, b.vrZoom, t);
    return r;
}

OverlayAnchor lerp(const OverlayAnchor &a, const OverlayAnchor &b, float t) {
    constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
    OverlayAnchor r;
    r.p1Norm = lerp2(a.p1Norm, b.p1Norm, t);
    r.p2Norm = lerp2(a.p2Norm, b.p2Norm, t);
    r.widthNorm = lerp1(a.widthNorm, b.widthNorm, t);
    // VR bar endpoints are {yaw, pitch} in radians: yaw wraps (2π), pitch does not.
    r.vrBarP1 = {lerpAngle(a.vrBarP1.x, b.vrBarP1.x, t, twoPi), lerp1(a.vrBarP1.y, b.vrBarP1.y, t)};
    r.vrBarP2 = {lerpAngle(a.vrBarP2.x, b.vrBarP2.x, t, twoPi), lerp1(a.vrBarP2.y, b.vrBarP2.y, t)};
    r.vrBarWidthAngle = lerp1(a.vrBarWidthAngle, b.vrBarWidthAngle, t);
    r.center3dNorm = lerp2(a.center3dNorm, b.center3dNorm, t);
    r.size3dNorm = lerp1(a.size3dNorm, b.size3dNorm, t);
    r.vrYaw = lerpAngle(a.vrYaw, b.vrYaw, t, twoPi);
    r.vrPitch = lerp1(a.vrPitch, b.vrPitch, t);
    r.vrAngularSize = lerp1(a.vrAngularSize, b.vrAngularSize, t);
    return r;
}

SceneView lerp(const SceneView &a, const SceneView &b, float t) {
    return {.framing = lerp(a.framing, b.framing, t),
            .anchor = lerp(a.anchor, b.anchor, t),
            .inverted = t < 0.5f ? a.inverted : b.inverted};
}

} // namespace ofs
