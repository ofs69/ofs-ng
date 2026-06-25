#pragma once

#include "imgui.h"

namespace ofs {

// A SceneView remembers, for one scene, how the video is framed, where the simulator overlay
// is anchored, and whether the simulator is inverted. It is stored as an optional override per Chapter and as
// a project-level fallback (ScriptProject::defaultSceneView), and resolved every frame
// into ScriptProject::activeSceneView. See docs: per-chapter scene memory.

// Live video framing for one scene. The video mode (Full vs VR) is NOT part of this: it is a
// project-level property (ScriptProject::videoPlayer.activeMode), not per-chapter scene memory. Both the
// 2D and VR field sets are always carried; the active mode decides which the player reads. The overlay
// tracks the video because it is anchored in content space (see OverlayAnchor); this struct restores the
// camera itself when a chapter is entered.
struct VideoFraming {
    // 2D pan/zoom of the image inside the content region. `translation` is stored as a
    // fraction of the displayed image size (not raw pixels) so it restores correctly when
    // the window is a different size than at capture time.
    float zoomFactor = 1.0f;
    ImVec2 translation = {0.0f, 0.0f};
    // VR equirect camera: `vrRotation` is normalized [0,1]^2 (x=yaw 2π-period, y=pitch
    // π-period) matching VrShader's Rotation uniform; `vrZoom` matches its Zoom uniform.
    ImVec2 vrRotation = {0.5f, 0.5f};
    float vrZoom = 0.5f;
};

// Overlay placement anchored in *video-content* space so it survives pan/zoom/rotate.
// 2D: positions are normalized [0,1] over the displayed image. VR: the overlay is pinned
// to a sphere direction (yaw,pitch in radians) and projected through the live VR camera.
struct OverlayAnchor {
    // ---- 2D bar, Full mode: endpoints normalized over the displayed image. ----
    ImVec2 p1Norm = {0.5f, 0.4f};
    ImVec2 p2Norm = {0.5f, 0.6f};
    float widthNorm = 0.12f; // bar thickness as a fraction of image height (scales with zoom)

    // ---- 2D bar, VR mode: two independent sphere directions {yaw,pitch} (radians) so the bar
    // tilts freely, plus an angular thickness. Projected through the live VR camera each frame. ----
    ImVec2 vrBarP1 = {0.0f, 0.15f};  // {yaw, pitch}
    ImVec2 vrBarP2 = {0.0f, -0.15f}; // {yaw, pitch}
    float vrBarWidthAngle = 0.06f;   // angular thickness (radians)

    // ---- 3D overlay rect: a single pinned direction + size. ----
    ImVec2 center3dNorm = {0.5f, 0.5f};
    float size3dNorm = 0.3f;
    // VR sphere direction the 3D model is pinned to.
    float vrYaw = 0.0f;         // radians
    float vrPitch = 0.0f;       // radians
    float vrAngularSize = 0.5f; // radians; drives the on-screen scale of the 3D overlay in VR
};

struct SceneView {
    VideoFraming framing;
    OverlayAnchor anchor;
    // Whether the simulator flips axis values (0↔100) for this scene. Per-chapter because a clip
    // may be shot from the opposite side, so the same script reads inverted there.
    bool inverted = false;
};

} // namespace ofs
