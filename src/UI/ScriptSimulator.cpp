#include "UI/ScriptSimulator.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/SceneViewEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Localization/AxisNames.h"
#include "Localization/Translator.h"
#include "Platform/Headless.h"
#include "Scenegraph/VrCamera.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/Resources.h"
#include "Video/VideoPlayer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iterator>
#include <numbers>

namespace {

float simDistance(const ImVec2 &p1, const ImVec2 &p2) {
    const ImVec2 diff = p1 - p2;
    return std::sqrt(diff.x * diff.x + diff.y * diff.y);
}

ImVec2 simNormalize(const ImVec2 &p) {
    const float mag = simDistance(ImVec2{0.f, 0.f}, p);
    if (mag == 0.f) {
        return {0.f, 0.f};
    }
    return {p.x / mag, p.y / mag};
}

// Perpendicular distance from a point to the nearest point on segment [a, b].
float pointToSegmentDist(const ImVec2 &point, const ImVec2 &segA, const ImVec2 &segB) {
    const ImVec2 ab = segB - segA;
    const float lenSq = ab.x * ab.x + ab.y * ab.y;
    if (lenSq == 0.f) {
        return simDistance(point, segA);
    }
    const ImVec2 ap = point - segA;
    const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / lenSq, 0.f, 1.f);
    return simDistance(point, segA + ab * t);
}

uint32_t applyOpacity(const ImColor &col, float opacity) {
    ImColor c = col;
    c.Value.w *= opacity;
    return c;
}

// Clamp a dragged bar endpoint so it can't cross the opposite end and flip the bar (which would
// invert the 0↔100 mapping). Keeps the component along the *original* bar axis at >= minLen, so the
// endpoint can still move perpendicular (tilt the bar) but never reverse past the fixed end.
ImVec2 clampBarEndpoint(const ImVec2 &moved, const ImVec2 &fixed, const ImVec2 &origMoved, float minLen) {
    const float len0 = simDistance(origMoved, fixed);
    if (len0 < 1e-5f) {
        return moved;
    }
    const ImVec2 u = simNormalize(origMoved - fixed);
    const ImVec2 rel = moved - fixed;
    const float along = rel.x * u.x + rel.y * u.y;
    if (along >= minLen) {
        return moved;
    }
    const ImVec2 perp = rel - u * along; // strip the inverting (negative-axis) component, keep tilt
    return fixed + u * minLen + perp;
}

// Returns a 0..1 normalized position for the given axis role at currentTime.
// Defaults to 0.5 (centre) when the axis has no data.
float getAxisValue(const ofs::ScriptProject &project, ofs::StandardAxis role, double currentTime) {
    const auto idx = static_cast<size_t>(role);
    if (role >= ofs::StandardAxis::Count)
        return 0.5f;
    const auto &axis = project.axes[idx];
    const auto &actions = axis.resolved ? axis.resolved->actions : axis.actions;
    if (actions.empty())
        return 0.5f;
    const ofs::ScriptAxisAction key{currentTime, 0};
    auto it = actions.lowerBound(key);
    if (it == actions.end()) {
        return static_cast<float>(actions.back().pos) / 100.f;
    }
    if (it == actions.begin()) {
        return static_cast<float>(it->pos) / 100.f;
    }
    const auto &next = *it;
    const auto &prev = *std::prev(it);
    const double t = (currentTime - prev.at) / (next.at - prev.at);
    float pos = static_cast<float>(prev.pos) + static_cast<float>(t) * static_cast<float>(next.pos - prev.pos);
    return pos / 100.f;
}

// Degrees the 3D model is deflected to for a rotation DOF, matching render3D's transform mapping
// exactly so a label reads the angle that's actually drawn. `v` is the already-inverted 0..1 value.
float rotationDegrees(ofs::StandardAxis role, float v, const ofs::SimulatorState &s) {
    switch (role) {
    case ofs::StandardAxis::R0:
        return glm::mix(-s.twistRange, s.twistRange, v);
    case ofs::StandardAxis::R1:
        return glm::mix(-s.rollRange, s.rollRange, v);
    case ofs::StandardAxis::R2:
        return glm::mix(s.pitchRange, -s.pitchRange, v);
    default:
        return 0.f;
    }
}

// Returns the center of the Video Player window's content area in screen space,
// falling back to the main viewport center if the window isn't visible.
ImVec2 videoPlayerCenter() {
    const ImGuiWindow *win = ImGui::FindWindowByName("Video Player###video_player");
    if (win && !win->Collapsed && !win->Hidden) {
        return win->InnerRect.GetCenter();
    }
    return ImGui::GetMainViewport()->GetCenter();
}

// ---- Anchor ↔ screen conversion (anchored-follow). 2D anchors are normalized over the displayed
// image rect; VR anchors are sphere directions projected through the live VR camera. ----

ImVec2 normToScreen(const ImVec2 &n, const ofs::OverlayViewport &vp) {
    return {vp.imageMin.x + n.x * vp.imageSize.x, vp.imageMin.y + n.y * vp.imageSize.y};
}
ImVec2 screenToNorm(const ImVec2 &s, const ofs::OverlayViewport &vp) {
    return {vp.imageSize.x > 0.f ? (s.x - vp.imageMin.x) / vp.imageSize.x : 0.5f,
            vp.imageSize.y > 0.f ? (s.y - vp.imageMin.y) / vp.imageSize.y : 0.5f};
}

// Border / line widths are derived from the bar's (content-space, zoom-scaling) thickness instead
// of the old fixed-pixel theme vars, so the whole bar scales as one unit. Lines scale gently and are
// clamped: thick bars must not be dominated by heavy gridlines, thin bars must keep readable lines.
constexpr float kBorderRatio = 0.067f; // border width as a fraction of bar thickness
constexpr float kLineRatio = 0.006f;   // height/indicator line width as a fraction of bar thickness
constexpr float kMinLineWidth = 3.5f;  // lines never thinner than this (readable when zoomed out)
constexpr float kMaxLineWidth = 5.5f;  // ...nor thicker than this (don't dominate when zoomed in)
// In VR, hide the bar once its centre projects this far past the FOV edge (in ndc units). Generous
// so the cull happens well off-screen — the clip rect trims the on-screen overflow without popping.
constexpr float kVrCullMargin = 1.5f;

ImVec2 ndcToScreen(const ImVec2 &ndc, const ofs::OverlayViewport &vp) {
    return {vp.contentMin.x + (ndc.x + 0.5f) * vp.contentSize.x, vp.contentMin.y + (ndc.y + 0.5f) * vp.contentSize.y};
}
ImVec2 screenToNdc(const ImVec2 &s, const ofs::OverlayViewport &vp) {
    return {vp.contentSize.x > 0.f ? (s.x - vp.contentMin.x) / vp.contentSize.x - 0.5f : 0.f,
            vp.contentSize.y > 0.f ? (s.y - vp.contentMin.y) / vp.contentSize.y - 0.5f : 0.f};
}
// The resize cursor whose double-arrow axis best matches `axis` (an undirected screen-space
// direction). The width grip drags along the bar's perpendicular, so the cursor must track the
// bar's orientation rather than assuming a fixed vertical bar. Screen y is down: a vector pointing
// down-right is a '\' (NWSE), down-left is a '/' (NESW).
ImGuiMouseCursor resizeCursorForAxis(const ImVec2 &axis) {
    float deg = std::atan2(axis.y, axis.x) * 180.f / std::numbers::pi_v<float>;
    deg = std::fmod(deg + 180.f, 180.f); // collapse to an undirected line in [0,180)
    if (deg < 22.5f || deg >= 157.5f)
        return ImGuiMouseCursor_ResizeEW;
    if (deg < 67.5f)
        return ImGuiMouseCursor_ResizeNWSE;
    if (deg < 112.5f)
        return ImGuiMouseCursor_ResizeNS;
    return ImGuiMouseCursor_ResizeNESW;
}

// VR: the world sphere direction a screen point samples (drag inverse of vrcam::project).
glm::vec3 vrScreenToDir(const ImVec2 &s, const ofs::OverlayViewport &vp) {
    return ofs::vrcam::unproject(screenToNdc(s, vp), vp.vrRotation, vp.vrZoom, vp.contentAspect());
}

// VR: project a sphere (yaw,pitch) to a screen point; `visible` is false only when behind the
// camera (the 3D-rect overlay still uses this single-direction helper).
bool vrAnchorToScreen(float yaw, float pitch, const ofs::OverlayViewport &vp, ImVec2 &out) {
    const auto p = ofs::vrcam::project(ofs::vrcam::sphereDir(yaw, pitch), vp.vrRotation, vp.vrZoom, vp.contentAspect());
    out = ndcToScreen(p.ndc, vp);
    return p.visible;
}
// VR: inverse of vrAnchorToScreen — the sphere (yaw,pitch) a screen point samples.
void vrScreenToAnchor(const ImVec2 &s, const ofs::OverlayViewport &vp, float &yaw, float &pitch) {
    ofs::vrcam::dirToYawPitch(vrScreenToDir(s, vp), yaw, pitch);
}

// The unit quaternion rotating direction `a` onto direction `b` (used to rigid-rotate the VR bar
// when the user drags its centre). Identity / 180° degenerate cases handled explicitly.
glm::quat rotationBetween(const glm::vec3 &a, const glm::vec3 &b) {
    const glm::vec3 an = glm::normalize(a), bn = glm::normalize(b);
    const float d = std::clamp(glm::dot(an, bn), -1.0f, 1.0f);
    if (d > 0.99999f)
        return {1.f, 0.f, 0.f, 0.f};
    if (d < -0.99999f) {
        const glm::vec3 ref = std::abs(an.x) < 0.9f ? glm::vec3{1.f, 0.f, 0.f} : glm::vec3{0.f, 1.f, 0.f};
        return glm::angleAxis(std::numbers::pi_v<float>, glm::normalize(glm::cross(an, ref)));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(an, bn)));
}

// Screen geometry of the 2D bar for the current anchor + viewport. Full: endpoints normalized over
// the image, thickness a fraction of image height. VR: each endpoint is an independent sphere dir
// (free orientation); thickness is the angular width projected to pixels, so it scales with zoom.
struct BarScreen {
    ImVec2 p1, p2;
    float thickness = 2.f;
    bool visible = false;
};
BarScreen barScreen(const ofs::OverlayAnchor &a, const ofs::OverlayViewport &vp) {
    if (vp.mode == ofs::VideoMode::VrMode) {
        const glm::vec3 d1 = ofs::vrcam::sphereDir(a.vrBarP1.x, a.vrBarP1.y);
        const glm::vec3 d2 = ofs::vrcam::sphereDir(a.vrBarP2.x, a.vrBarP2.y);
        const auto pr1 = ofs::vrcam::project(d1, vp.vrRotation, vp.vrZoom, vp.contentAspect());
        const auto pr2 = ofs::vrcam::project(d2, vp.vrRotation, vp.vrZoom, vp.contentAspect());
        const ImVec2 p1 = ndcToScreen(pr1.ndc, vp);
        const ImVec2 p2 = ndcToScreen(pr2.ndc, vp);
        const ImVec2 cNdc{(pr1.ndc.x + pr2.ndc.x) * 0.5f, (pr1.ndc.y + pr2.ndc.y) * 0.5f};
        const bool visible =
            pr1.visible && pr2.visible && std::abs(cNdc.x) < kVrCullMargin && std::abs(cNdc.y) < kVrCullMargin;
        const float angLen = std::acos(std::clamp(glm::dot(d1, d2), -1.0f, 1.0f));
        const float screenLen = simDistance(p1, p2);
        const float thickness = (angLen > 1e-4f) ? screenLen * (a.vrBarWidthAngle / angLen) : 40.f;
        return {.p1 = p1, .p2 = p2, .thickness = std::max(2.f, thickness), .visible = visible};
    }
    return {.p1 = normToScreen(a.p1Norm, vp),
            .p2 = normToScreen(a.p2Norm, vp),
            .thickness = std::max(2.f, a.widthNorm * vp.imageSize.y),
            .visible = true};
}

// Screen rect (top-left + size, square) of the 3D overlay for the current anchor + viewport. In VR
// the size is the screen distance the angular extent subtends, so it scales with rotation + zoom.
struct RectScreen {
    ImVec2 min, size;
    bool visible = false;
};
RectScreen rect3dScreen(const ofs::OverlayAnchor &a, const ofs::OverlayViewport &vp) {
    if (vp.mode == ofs::VideoMode::VrMode) {
        ImVec2 c, top, bot;
        const bool vis = vrAnchorToScreen(a.vrYaw, a.vrPitch, vp, c);
        vrAnchorToScreen(a.vrYaw, a.vrPitch + a.vrAngularSize * 0.5f, vp, top);
        vrAnchorToScreen(a.vrYaw, a.vrPitch - a.vrAngularSize * 0.5f, vp, bot);
        const float sz = std::max(40.f, simDistance(top, bot));
        return {.min = {c.x - sz * 0.5f, c.y - sz * 0.5f}, .size = {sz, sz}, .visible = vis};
    }
    const float sz = a.size3dNorm * vp.imageSize.y;
    const ImVec2 c = normToScreen(a.center3dNorm, vp);
    return {.min = {c.x - sz * 0.5f, c.y - sz * 0.5f}, .size = {sz, sz}, .visible = true};
}

// Inverse of render3D's per-DOF transform: the displayed 0..1 value the cursor maps to for `role`,
// using the same fixed perspective camera the model is drawn through. Stroke (L0) is the model's
// vertical axis, Sway (L2) its horizontal one; rotations (R0/R1/R2) read the cursor's angle around
// the model centre, mirroring the needle dials in renderOverlay. Surge (L1) runs straight down the
// view axis (no on-screen line) and any non-model axis fall back to a generic vertical slider.
float model3dValueAt(ofs::StandardAxis role, const ImVec2 &mouse, const ImVec2 &rectMin, const ImVec2 &rectSize,
                     const ofs::SimulatorState &s) {
    const auto perspProj = glm::perspective(glm::radians(40.f), 1.0f, 0.1f, 100.f);
    const auto perspView = glm::lookAt(glm::vec3{0.f, 0.f, 6.f}, glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});
    const auto project = [&](glm::vec3 p) -> ImVec2 {
        const glm::vec4 clip = perspProj * perspView * glm::vec4(p, 1.f);
        const float w = (std::abs(clip.w) > 1e-4f) ? clip.w : 1e-4f;
        return {rectMin.x + (clip.x / w * 0.5f + 0.5f) * rectSize.x,
                rectMin.y + (0.5f - clip.y / w * 0.5f) * rectSize.y};
    };
    // Projection parameter of `mouse` onto segment [b (v=0), a (v=1)], clamped to [0,1].
    const auto along = [&](ImVec2 a, ImVec2 b) {
        const ImVec2 ab = a - b;
        const float lenSq = ab.x * ab.x + ab.y * ab.y;
        if (lenSq < 1e-6f)
            return 0.5f;
        return std::clamp(((mouse.x - b.x) * ab.x + (mouse.y - b.y) * ab.y) / lenSq, 0.f, 1.f);
    };
    switch (role) {
    case ofs::StandardAxis::L0: // Stroke: v=1 → +Y (top), v=0 → -Y (bottom)
        return along(project({0.f, s.strokeRange, 0.f}), project({0.f, -s.strokeRange, 0.f}));
    case ofs::StandardAxis::L2: // Sway: v=1 → -X (left), v=0 → +X (right) — matches render3D mix(sway,-sway,v)
        return along(project({-s.swayRange, 0.f, 0.f}), project({s.swayRange, 0.f, 0.f}));
    case ofs::StandardAxis::R0:
    case ofs::StandardAxis::R1:
    case ofs::StandardAxis::R2: {
        const ImVec2 o = project({0.f, 0.f, 0.f});
        // Screen y is down; the dials' reference needle points up (-90°). Convert the cursor's angle
        // off that reference to degrees, then invert rotationDegrees()'s linear mapping per DOF.
        const float refAng = -IM_PI * 0.5f;
        float deg = (std::atan2(mouse.y - o.y, mouse.x - o.x) - refAng) * 180.f / std::numbers::pi_v<float>;
        deg = std::fmod(deg + 540.f, 360.f) - 180.f; // wrap to (-180, 180]
        float v = 0.5f;
        if (role == ofs::StandardAxis::R0 && s.twistRange > 1e-3f)
            v = (deg + s.twistRange) / (2.f * s.twistRange);
        else if (role == ofs::StandardAxis::R1 && s.rollRange > 1e-3f)
            v = (deg + s.rollRange) / (2.f * s.rollRange);
        else if (role == ofs::StandardAxis::R2 && s.pitchRange > 1e-3f)
            v = (s.pitchRange - deg) / (2.f * s.pitchRange);
        return std::clamp(v, 0.f, 1.f);
    }
    default: // Surge (down the view axis) and non-model axes: generic vertical slider (top = 1)
        return std::clamp((rectMin.y + rectSize.y - mouse.y) / std::max(1.f, rectSize.y), 0.f, 1.f);
    }
}

} // namespace

namespace ofs {

// ImGui's GL backend does NOT update glScissor for user callbacks (only for regular draw cmds),
// so the stale scissor from the previous draw command would clip our rendering. Disable it explicitly.
void ScriptSimulator::glCallbackFunc(const ImDrawList * /*parentList*/, const ImDrawCmd *cmd) {
    const auto *d = static_cast<const Sim3DCallbackData *>(cmd->UserCallbackData);
    const ImDrawData *dd = ImGui::GetDrawData();

    // Viewport: maps the overlay rect into GL framebuffer space.
    const float vpX = d->contentMin.x - dd->DisplayPos.x;
    const float vpY = dd->DisplaySize.y - (d->contentMin.y - dd->DisplayPos.y) - d->contentSize.y;
    const float vpW = d->contentSize.x;
    const float vpH = d->contentSize.y;

    // Scissor: use cmd->ClipRect so GL rendering is bounded by the enclosing ImGui window,
    // rather than disabling scissor which would let the 3D content bleed over adjacent windows.
    const ImVec4 &cr = cmd->ClipRect;
    const float scX = cr.x - dd->DisplayPos.x;
    const float scY = dd->DisplaySize.y - (cr.y - dd->DisplayPos.y) - (cr.w - cr.y);
    const float scW = cr.z - cr.x;
    const float scH = cr.w - cr.y;

    GLint prevVp[4];
    GLint prevScissorBox[4];
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
    const GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glViewport(static_cast<GLint>(vpX), static_cast<GLint>(vpY), static_cast<GLsizei>(vpW), static_cast<GLsizei>(vpH));
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(scX), static_cast<GLint>(scY), static_cast<GLsizei>(scW), static_cast<GLsizei>(scH));
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (d->updateTransforms) {
        d->sceneGraph->updateTransforms();
    }
    d->sceneGraph->render(d->viewMatrix, d->projMatrix);

    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    glDepthMask(prevDepthMask);
    glDisable(GL_DEPTH_TEST);
    if (!prevScissorEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }
}

ScriptSimulator::ScriptSimulator() {
    // The glTF load uploads meshes/shaders to the GPU (Mesh::uploadToGPU, SceneShader ctor). Under the
    // null backend there is no GL context, so skip it: the scene renders only inside the dropped draw
    // callback (glCallbackFunc), and a null gltfScene is already a supported state (asset-missing path).
    if constexpr (!ofs::kHeadless) {
        if (auto glb = ofs::res::read("data/simulator.glb")) {
            gltfScene = sg::loadGltfFromMemory(scene3d, reinterpret_cast<const unsigned char *>(glb->data()),
                                               glb->size(), "data/simulator.glb");
        }
        if (!gltfScene) {
            OFS_CORE_ERROR("Failed to load simulator model 'data/simulator.glb' from assets");
        }
        if (gltfScene && !gltfScene->roots.empty()) {
            strokerNode = gltfScene->roots.front();
        }
    }
}

namespace {
// A centered overlay anchor (used by the Recenter button). 2D bar: a short vertical bar at image
// centre; 3D: centred. VR: pinned to the direction the user is currently looking (screen centre).
ofs::OverlayAnchor centeredAnchor(const ofs::OverlayAnchor &cur, const ofs::OverlayViewport &vp) {
    ofs::OverlayAnchor a = cur;
    a.p1Norm = {0.5f, 0.35f};
    a.p2Norm = {0.5f, 0.65f};
    a.widthNorm = 0.12f;
    a.center3dNorm = {0.5f, 0.5f};
    if (vp.valid && vp.mode == ofs::VideoMode::VrMode) {
        const ImVec2 screenCenter{vp.contentMin.x + vp.contentSize.x * 0.5f, vp.contentMin.y + vp.contentSize.y * 0.5f};
        // Pin the 3D anchor and re-build the 2D bar (default vertical span) around the looked-at dir.
        float yaw = 0.f, pitch = 0.f;
        vrScreenToAnchor(screenCenter, vp, yaw, pitch);
        a.vrYaw = yaw;
        a.vrPitch = pitch;
        constexpr float kHalfSpan = 0.30f; // radians; default bar half-length in pitch (VR FOV is wide,
                                           // so a small span looks tiny — keep it comparable to full mode)
        a.vrBarP1 = {yaw, pitch + kHalfSpan};
        a.vrBarP2 = {yaw, pitch - kHalfSpan};
        // Width as a fraction of length, matching full mode's 0.12/0.30 ratio so the bar isn't too thin.
        a.vrBarWidthAngle = 2.f * kHalfSpan * (0.12f / 0.30f);
    }
    return a;
}
} // namespace

void ScriptSimulator::render3D(const ScriptProject &project, EventQueue &eq, double currentTime,
                               const SimulatorState &state, const OverlayAnchor &anchor, bool vpHovered) {
    if (strokerNode == nullptr) {
        ImGui::TextDisabled("%s", Str::SimModelLoadFailed.c_str());
        return;
    }

    // Shift-hover preview: while Shift is held and the cursor is inside the model's overlay rect, the
    // active axis is previewed at the value the cursor's on-screen direction maps to (see
    // model3dValueAt). Computed before the transform so the model deflects to the previewed value; a
    // left-click in the interaction block below commits it. Not gated by lockedPosition — locking
    // only freezes the overlay's placement, not scripting.
    const auto activeRole = project.state.activeAxis;
    const bool activePresent = activeRole < StandardAxis::Count;
    bool shiftPlace3d = false;
    if (ImGui::GetIO().KeyShift && activePresent && vpHovered && overlayVp_.valid && !isMoving3d && !isResizing3d) {
        const RectScreen r = rect3dScreen(anchor, overlayVp_);
        const ImVec2 m = ImGui::GetMousePos();
        const ImVec2 rMax{r.min.x + r.size.x, r.min.y + r.size.y};
        if (r.visible && m.x >= r.min.x && m.x <= rMax.x && m.y >= r.min.y && m.y <= rMax.y) {
            shiftPlace3d = true;
            preview_.active = true;
            preview_.axis = activeRole;
            preview_.displayedValue = model3dValueAt(activeRole, m, r.min, r.size, state);
        }
    }

    // Apply transforms — matches Godot's Simulator3D.cs exactly. axisVal returns the *displayed*
    // deflection (per-scene inversion applied); during a Shift-hover preview the active axis reads
    // the previewed value instead, so the model deflects to it.
    const auto axisVal = [&](StandardAxis role) {
        if (preview_.active && role == preview_.axis)
            return preview_.displayedValue;
        const float v = getAxisValue(project, role, currentTime);
        return project.activeSceneView.inverted ? 1.0f - v : v;
    };
    auto &t = strokerNode->localTransform;
    t.position = glm::vec3{
        glm::mix(state.swayRange, -state.swayRange, axisVal(StandardAxis::L2)),
        glm::mix(-state.strokeRange, state.strokeRange, axisVal(StandardAxis::L0)),
        glm::mix(state.surgeRange, -state.surgeRange, axisVal(StandardAxis::L1)),
    };
    const float pitchAngle =
        glm::mix(glm::radians(state.pitchRange), glm::radians(-state.pitchRange), axisVal(StandardAxis::R2));
    const float rollAngle =
        glm::mix(glm::radians(-state.rollRange), glm::radians(state.rollRange), axisVal(StandardAxis::R1));
    const float twistAngle =
        glm::mix(glm::radians(-state.twistRange), glm::radians(state.twistRange), axisVal(StandardAxis::R0));
    t.rotation = glm::angleAxis(-rollAngle, glm::vec3{0, 0, 1}) * glm::angleAxis(pitchAngle, glm::vec3{1, 0, 0}) *
                 glm::angleAxis(twistAngle, glm::vec3{0, 1, 0});

    // ---- Orthographic views inside the Simulator window ----

    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    // Reserve the full content area so the window scroll region is correct
    ImGui::Dummy(contentSize);

    const float halfW = contentSize.x / 2.f;
    const ImVec2 topMin = contentMin;
    const ImVec2 topMax = {contentMin.x + halfW, contentMin.y + contentSize.y};
    const ImVec2 sideMin = {contentMin.x + halfW, contentMin.y};
    const ImVec2 sideMax = contentMin + contentSize;

    const float orthoR = 2.0f;
    const float vpAspect = (contentSize.y > 0.f) ? halfW / contentSize.y : 1.f;
    const auto orthoProj = glm::ortho(-orthoR * vpAspect, orthoR * vpAspect, -orthoR, orthoR, 0.1f, 100.f);
    // Top view: camera directly above, looking down -Y, forward along -Z
    const auto topView = glm::lookAt(glm::vec3{0.f, 8.f, 0.f}, glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 0.f, -1.f});
    // Side view: camera along X axis, looking toward origin
    const auto sideView = glm::lookAt(glm::vec3{8.f, 0.3f, 0.f}, glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});

    // updateTransforms only needs to run once per frame — set flag on first callback
    callbackDataOrtho[0] = {.sceneGraph = &scene3d,
                            .contentMin = topMin,
                            .contentSize = {halfW, contentSize.y},
                            .viewMatrix = topView,
                            .projMatrix = orthoProj,
                            .updateTransforms = true};
    callbackDataOrtho[1] = {.sceneGraph = &scene3d,
                            .contentMin = sideMin,
                            .contentSize = {halfW, contentSize.y},
                            .viewMatrix = sideView,
                            .projMatrix = orthoProj,
                            .updateTransforms = false};

    ImDrawList *wdl = ImGui::GetWindowDrawList();

    // Top view panel
    wdl->AddRectFilled(topMin, topMax, ofs::theme::GetColorU32(AppCol_SimViewBg));
    wdl->AddCallback(&ScriptSimulator::glCallbackFunc, &callbackDataOrtho[0]);
    wdl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    // Draw-list captions have no auto-clip — elide so a longer translation can't run past the panel.
    wdl->AddText({topMin.x + 4.f, topMin.y + 2.f}, ofs::theme::GetColorU32(AppCol_SimTintTop, 200.f / 255.f),
                 ofs::ui::elide(Str::SimViewTop, halfW - 8.f));

    // Side view panel
    wdl->AddRectFilled(sideMin, sideMax, ofs::theme::GetColorU32(AppCol_SimViewBg));
    wdl->AddCallback(&ScriptSimulator::glCallbackFunc, &callbackDataOrtho[1]);
    wdl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    wdl->AddText({sideMin.x + 4.f, sideMin.y + 2.f}, ofs::theme::GetColorU32(AppCol_SimTintSide, 200.f / 255.f),
                 ofs::ui::elide(Str::SimViewSide, halfW - 8.f));

    // Divider between the two panels
    wdl->AddLine({sideMin.x, topMin.y}, {sideMin.x, topMax.y}, ofs::theme::GetColorU32(AppCol_SimDivider), 1.f);

    // ---- 2D overlays: crosshair, axis labels, rotation arcs ----
    {
        const ImVec2 vpSz{halfW, contentSize.y};

        // Project a world-space point into screen coords for an ortho view.
        const auto projectPt = [&](const glm::vec3 &p, const glm::mat4 &vMat, const ImVec2 &vpMin) -> ImVec2 {
            const glm::vec4 clip = orthoProj * vMat * glm::vec4(p, 1.f);
            return {vpMin.x + (clip.x / clip.w * 0.5f + 0.5f) * vpSz.x,
                    vpMin.y + (0.5f - clip.y / clip.w * 0.5f) * vpSz.y};
        };

        const ImVec2 topOrig = projectPt({0.f, 0.f, 0.f}, topView, topMin);
        const ImVec2 sideOrig = projectPt({0.f, 0.f, 0.f}, sideView, sideMin);

        // Crosshairs at the world origin (all axes at 50% = neutral reference)
        constexpr float kCrossR = 10.f;
        const ImU32 crossCol = ofs::theme::GetColorU32(AppCol_SimCrosshair);
        wdl->AddLine({topOrig.x - kCrossR, topOrig.y}, {topOrig.x + kCrossR, topOrig.y}, crossCol, 1.f);
        wdl->AddLine({topOrig.x, topOrig.y - kCrossR}, {topOrig.x, topOrig.y + kCrossR}, crossCol, 1.f);
        wdl->AddLine({sideOrig.x - kCrossR, sideOrig.y}, {sideOrig.x + kCrossR, sideOrig.y}, crossCol, 1.f);
        wdl->AddLine({sideOrig.x, sideOrig.y - kCrossR}, {sideOrig.x, sideOrig.y + kCrossR}, crossCol, 1.f);

        // Axis dimension labels at panel edges
        // Top view: horizontal = Sway (L2), vertical = Surge (L1)
        // Side view: vertical = Stroke (L0), horizontal = Surge (L1)
        const ImU32 topLabelCol = ofs::theme::GetColorU32(AppCol_SimTintTop, 180.f / 255.f);
        const ImU32 topTagCol = ofs::theme::GetColorU32(AppCol_SimTintTop, 110.f / 255.f);
        const ImU32 sideLabelCol = ofs::theme::GetColorU32(AppCol_SimTintSide, 180.f / 255.f);
        const ImU32 sideTagCol = ofs::theme::GetColorU32(AppCol_SimTintSide, 110.f / 255.f);
        constexpr float kTagGap = 4.f;

        // Draw "Name  TAG" where TAG is in a dimmer shade of the same hue.
        const auto drawTagged = [&](ImVec2 pos, const char *label, const char *tag, ImU32 lCol, ImU32 tCol) {
            wdl->AddText(pos, lCol, label);
            wdl->AddText({pos.x + ImGui::CalcTextSize(label).x + kTagGap, pos.y}, tCol, tag);
        };
        const auto taggedW = [&](const char *label, const char *tag) {
            return ImGui::CalcTextSize(label).x + kTagGap + ImGui::CalcTextSize(tag).x;
        };

        // DOF words are translatable; the axis-id tag (L0/L1/L2) stays literal like other id symbols.
        const char *sway = Str::SimDofSway.c_str();
        const char *surge = Str::SimDofSurge.c_str();
        const char *stroke = Str::SimDofStroke.c_str();
        // Sway L2 — right edge of top view, mid-height at origin
        drawTagged({topMax.x - taggedW(sway, "L2") - 4.f, topOrig.y - ImGui::GetTextLineHeight() * 0.5f}, sway, "L2",
                   topLabelCol, topTagCol);
        // Surge L1 — top edge of top view, centered on origin X
        drawTagged({topOrig.x - taggedW(surge, "L1") * 0.5f, topMin.y + ImGui::CalcTextSize(surge).y + 2.f}, surge,
                   "L1", topLabelCol, topTagCol);
        // Stroke L0 — top edge of side view, centered on origin X
        drawTagged({sideOrig.x - taggedW(stroke, "L0") * 0.5f, sideMin.y + ImGui::CalcTextSize(stroke).y + 2.f}, stroke,
                   "L0", sideLabelCol, sideTagCol);
        // Surge L1 — right edge of side view, mid-height at origin
        drawTagged({sideMax.x - taggedW(surge, "L1") - 4.f, sideOrig.y - ImGui::GetTextLineHeight() * 0.5f}, surge,
                   "L1", sideLabelCol, sideTagCol);

        // Rotation arc indicators — faint reference needle + colored current needle + arc sweep
        const float twistRad = glm::mix(-IM_PI, IM_PI, axisVal(StandardAxis::R0));
        const float pitchRad = glm::mix(glm::radians(30.f), glm::radians(-30.f), axisVal(StandardAxis::R2));

        constexpr float kArcR = 28.f;
        const ImU32 arcRefCol = ofs::theme::GetColorU32(AppCol_SimArcRef);
        const ImU32 arcCol = ofs::theme::GetColorU32(AppCol_SimArc);
        const ImU32 arcFillCol = ofs::theme::GetColorU32(AppCol_SimArc);

        const auto drawArc = [&](ImVec2 origin, float ref, float cur) {
            wdl->AddLine(origin, {origin.x + kArcR * std::cos(ref), origin.y + kArcR * std::sin(ref)}, arcRefCol, 1.f);
            wdl->AddLine(origin, {origin.x + kArcR * std::cos(cur), origin.y + kArcR * std::sin(cur)}, arcCol, 2.f);
            if (std::abs(cur - ref) > 0.01f) {
                wdl->PathArcTo(origin, kArcR, std::min(ref, cur), std::max(ref, cur), 32);
                wdl->PathStroke(arcFillCol, 2.f, ImDrawFlags_None);
            }
        };

        // Twist arc: top view — rotation around world Y appears as screen rotation.
        // Positive twist rotates CCW from above → CW on screen → needle sweeps left of the ref (screen-up).
        drawArc(topOrig, -IM_PI * 0.5f, -IM_PI * 0.5f - twistRad);
        // Pitch arc: side view — positive pitch tilts the device top toward screen-right.
        drawArc(sideOrig, -IM_PI * 0.5f, -IM_PI * 0.5f + pitchRad);
    }

    // ---- Perspective overlay: setup + interaction (drawing happens in renderOverlay) ----
    // The model renders through a fixed perspective camera; its screen rect comes from the anchor
    // projected through the live video viewport (anchored-follow). renderOverlay overrides the rect
    // again with the current frame's viewport; here we use the last frame's for hit-testing.

    RectScreen rect;
    if (overlayVp_.valid) {
        rect = rect3dScreen(anchor, overlayVp_);
    } else {
        const float sz = 300.f;
        const ImVec2 c = videoPlayerCenter();
        rect = {.min = {c.x - sz * 0.5f, c.y - sz * 0.5f}, .size = {sz, sz}, .visible = true};
    }
    const ImVec2 perspMin = rect.min;
    const ImVec2 perspMax = {perspMin.x + rect.size.x, perspMin.y + rect.size.y};

    const auto perspProj = glm::perspective(glm::radians(40.f), 1.0f, 0.1f, 100.f);
    const auto perspView = glm::lookAt(glm::vec3{0.f, 0.f, 6.f}, glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});

    // Perspective callback runs after the ortho ones, so transforms are already up to date
    callbackDataPerspective = {.sceneGraph = &scene3d,
                               .contentMin = perspMin,
                               .contentSize = rect.size,
                               .viewMatrix = perspView,
                               .projMatrix = perspProj,
                               .updateTransforms = false};

    const bool vr = overlayVp_.mode == VideoMode::VrMode;
    OverlayAnchor a = anchor; // working copy; pushed as a capture on change

    // Move/resize interaction
    constexpr float kGripSize = 14.f;
    if (shiftPlace3d) {
        // Shift-hover place: a left-click commits an action on the active axis at the previewed value.
        // Consume the click (and claim hover) so the Video Player doesn't pan/rotate or seek under it.
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetHoveredID(ImGui::GetCurrentWindowRead()->ID);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const bool inv = project.activeSceneView.inverted;
            const float stored = inv ? 1.f - preview_.displayedValue : preview_.displayedValue;
            eq.push(
                EditRequestEvent{.intent = {.kind = EditIntentKind::AddPoint,
                                            .axis = activeRole,
                                            .time = currentTime,
                                            .pos = std::clamp(static_cast<int>(std::lround(stored * 100.f)), 0, 100)}});
            ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
        }
        isMoving3d = false;
        isResizing3d = false;
    } else if (!state.lockedPosition && rect.visible) {
        ImGuiWindow *simWindow = ImGui::GetCurrentWindowRead();
        const ImGuiID moveId = ImGui::GetID("Sim3DMove");
        const ImGuiID resizeId = ImGui::GetID("Sim3DResize");
        const ImVec2 mouse = ImGui::GetMousePos();

        // Register the overlay and its resize grip as addressable, non-interactive items
        // so UI tests can anchor to them via ItemInfo. ItemAdd consumes no input; the
        // manual hit-testing below (which already claims these same IDs via SetActiveID)
        // is unaffected.
        ImGui::ItemAdd(ImRect(perspMin, perspMax), moveId, nullptr, ImGuiItemFlags_NoNav);
        ImGui::ItemAdd(ImRect({perspMax.x - kGripSize, perspMax.y - kGripSize}, perspMax), resizeId, nullptr,
                       ImGuiItemFlags_NoNav);

        const bool mouseInOverlay =
            mouse.x >= perspMin.x && mouse.x <= perspMax.x && mouse.y >= perspMin.y && mouse.y <= perspMax.y;
        // Mouse is in the bottom-right grip triangle (approximate with a square corner)
        const bool mouseOnGrip = mouse.x >= perspMax.x - kGripSize && mouse.x <= perspMax.x &&
                                 mouse.y >= perspMax.y - kGripSize && mouse.y <= perspMax.y;

        if (isResizing3d) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            ImGui::SetActiveID(resizeId, simWindow);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                const float d = (delta.x + delta.y) * 0.5f;
                if (vr)
                    a.vrAngularSize = std::max(0.05f, startResize3dSize + d * 0.003f);
                else if (overlayVp_.imageSize.y > 0.f)
                    a.size3dNorm = std::max(0.05f, startResize3dSize + d / overlayVp_.imageSize.y);
                eq.push(CaptureOverlayAnchorEvent{a});
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                isResizing3d = false;
                ImGui::ClearActiveID();
            }
        } else if (isMoving3d) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetActiveID(moveId, simWindow);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (vr) {
                    vrScreenToAnchor(mouse, overlayVp_, a.vrYaw, a.vrPitch);
                } else {
                    const ImVec2 cur = screenToNorm(mouse, overlayVp_);
                    a.center3dNorm = dragStart3dAnchor_.center3dNorm + (cur - dragStart3dMouseNorm_);
                }
                eq.push(CaptureOverlayAnchorEvent{a});
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                isMoving3d = false;
                ImGui::ClearActiveID();
            }
        } else if (mouseOnGrip && vpHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            ImGui::SetHoveredID(simWindow->ID);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImGui::SetActiveID(resizeId, simWindow);
                ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
                startResize3dSize = vr ? anchor.vrAngularSize : anchor.size3dNorm;
                isResizing3d = true;
            }
        } else if (mouseInOverlay && vpHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetHoveredID(simWindow->ID);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImGui::SetActiveID(moveId, simWindow);
                ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
                dragStart3dAnchor_ = anchor;
                dragStart3dMouseNorm_ = screenToNorm(mouse, overlayVp_);
                isMoving3d = true;
            }
        }
    } else {
        isMoving3d = false;
        isResizing3d = false;
    }

    // Right-click anywhere on the overlay opens a quick-access menu for the label settings. Kept
    // separate from the move/resize block above so it works even when the overlay is locked.
    if (rect.visible && vpHovered) {
        const ImVec2 m = ImGui::GetMousePos();
        if (m.x >= perspMin.x && m.x <= perspMax.x && m.y >= perspMin.y && m.y <= perspMax.y &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("Sim3DContext");
    }
    if (ImGui::BeginPopup("Sim3DContext")) {
        // Menu items push events like every other UI write — never mutate ScriptProject directly.
        ImGui::SeparatorText(Str::Sim3dLabels);
        constexpr StandardAxis dofs[] = {StandardAxis::L0, StandardAxis::L1, StandardAxis::L2,
                                         StandardAxis::R0, StandardAxis::R1, StandardAxis::R2};
        for (StandardAxis role : dofs) {
            const auto idx = static_cast<size_t>(role);
            const bool on = state.labels3dMask.test(idx);
            // Visible label is translated; the language-independent axis index is the stable widget id.
            if (ImGui::MenuItem(fmtScratch("{}###sim_axis_label_{}", ofs::loc::localizedAxisName(role), idx), nullptr,
                                on))
                eq.push(ModifyEvent<SimulatorState>{[idx, on](SimulatorState &s) { s.labels3dMask.set(idx, !on); }});
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Str::SimShowAll.id("sim_show_all")))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.labels3dMask.set(); }});
        if (ImGui::MenuItem(Str::SimHideAll.id("sim_hide_all")))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.labels3dMask.reset(); }});
        ImGui::SeparatorText(Str::SimRotationUnits);
        if (ImGui::MenuItem(Str::SimDegrees.id("sim_degrees"), nullptr, state.labels3dInDegrees))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.labels3dInDegrees = true; }});
        if (ImGui::MenuItem(Str::SimPercent.id("sim_percent"), nullptr, !state.labels3dInDegrees))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.labels3dInDegrees = false; }});
        ImGui::Separator();
        if (ImGui::MenuItem(Str::SimLocked.iconId(state.lockedPosition ? ICON_LOCK : ICON_LOCK_OPEN, "sim_locked_3d"),
                            nullptr, state.lockedPosition))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.lockedPosition = !s.lockedPosition; }});
        ImGui::EndPopup();
    }
}

void ScriptSimulator::render(const ScriptProject &project, EventQueue &eq, bool &open, bool vpHovered) {
    if (!open) {
        return;
    }
    auto state = project.simulator;
    const double currentTime = project.playback.cursorPos;

    // Cleared each frame; render3D() or the 2D path below re-arms it while Shift-hovering the overlay.
    preview_.active = false;

    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints({fs * 22.f, fs * 18.f}, {FLT_MAX, FLT_MAX});
    // NoNavInputs so the editor's unmodified arrow/Space shortcuts keep working while this panel has
    // focus, instead of being claimed by ImGui keyboard nav (see Application.cpp).
    ImGui::Begin(Str::SimTitle.id("Simulator"), &open,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);

    bool headerVisualsChanged = false;

    // Header labels are composed once so the Recenter button reserve measures the *actual* (translated)
    // checkbox text, and the checkboxes render the same string — never an English literal.
    const char *lockLabel = Str::SimLock.icon(state.lockedPosition ? ICON_LOCK : ICON_LOCK_OPEN);
    const char *recenterLabel = Str::SimRecenter.icon(ICON_RECENTER);

    {
        const ImGuiStyle &style = ImGui::GetStyle();
        const float frameH = ImGui::GetFrameHeight();
        const float lockW = frameH + style.ItemInnerSpacing.x + ImGui::CalcTextSize(lockLabel).x;
        const float threeDW = frameH + style.ItemInnerSpacing.x + ImGui::CalcTextSize(Str::Sim3D).x;
        const float invertW = frameH + style.ItemInnerSpacing.x + ImGui::CalcTextSize(Str::SimInvert).x;
        const float checkboxesTotal = lockW + style.ItemSpacing.x + threeDW + style.ItemSpacing.x + invertW;
        const float btnWidth = ImGui::GetContentRegionAvail().x - checkboxesTotal - style.ItemSpacing.x;

        // ###Recenter pins the ID so the icon prefix doesn't break ItemClick(".../Recenter") in tests.
        if (ImGui::Button(
                fmtScratch("{}###Recenter", recenterLabel),
                {std::max(btnWidth, style.FramePadding.x * 2.f + ImGui::CalcTextSize(recenterLabel).x), 0.f})) {
            eq.push(CaptureOverlayAnchorEvent{centeredAnchor(project.activeSceneView.anchor, overlayVp_)});
        }
    }

    ImGui::SameLine();
    ImGui::Checkbox(fmtScratch("{}###sim_lock", lockLabel), &state.lockedPosition);
    headerVisualsChanged |= ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    if (ImGui::Checkbox(Str::Sim3D.id("sim_3d"), &state.use3dSimulator)) {
        headerVisualsChanged = true;
    }

    ImGui::SameLine();
    // Invert is stored per-scene (in the active SceneView), not in the app-global SimulatorState,
    // so it restores per chapter. Capture it on its own half-event rather than via SimulatorPositionChanged.
    bool inverted = project.activeSceneView.inverted;
    if (ImGui::Checkbox(Str::SimInvert.id("sim_invert"), &inverted)) {
        eq.push(CaptureSimInvertedEvent{inverted});
    }

    if (headerVisualsChanged) {
        // project.simulator is the live truth (updated via this event); AppSettings is mirrored
        // from it on exit, so there is no settings write here.
        eq.push(SimulatorPositionChangedEvent{state});
    }

    // Don't run interaction or draw overlays while a modal popup is active.
    if (ImGui::GetTopMostPopupModal() != nullptr) {
        ImGui::End();
        return;
    }

    const OverlayAnchor &anchor = project.activeSceneView.anchor;

    if (state.use3dSimulator) {
        render3D(project, eq, currentTime, state, anchor, vpHovered);
        ImGui::End();
        return;
    }

    // ---- 2D bar interaction ----
    // The bar's screen endpoints come from the anchor projected through the last frame's video
    // viewport (anchored-follow). Drags update the anchor (content space) and push a capture. With
    // no video to anchor to, skip interaction.

    const ImGuiID simId = ImGui::GetID("ActualSimulator");
    ImGui::KeepAliveID(simId);

    const bool vr = overlayVp_.mode == VideoMode::VrMode;
    const BarScreen bar = barScreen(anchor, overlayVp_);

    // Bar geometry — used for drag-handle hit-testing. Thickness is content-space (scales with zoom).
    const float barWidth = bar.thickness;
    const float borderWidth = barWidth * kBorderRatio;
    const ImVec2 direction = simNormalize(bar.p1 - bar.p2);
    const ImVec2 perp{-direction.y, direction.x};
    const ImVec2 barP1 = bar.p1 - (direction * (borderWidth / 2.f));
    const ImVec2 barP2 = bar.p2 + (direction * (borderWidth / 2.f));
    const float barLen = simDistance(barP1, barP2);
    const ImVec2 barCenter = barP2 + (direction * (barLen / 2.f));
    // Width grip sits just outside the bar edge, perpendicular to it.
    const float gripOffset = barWidth * 0.5f + 10.f;
    const ImVec2 widthGrip = barCenter + perp * gripOffset;
    // VR bar's angular length + on-screen length, used to convert a perpendicular pixel drag of the
    // width grip into an angular width (keeps thickness ↔ widthAngle consistent with barScreen()).
    const float vrAngLen = vr ? std::acos(std::clamp(glm::dot(vrcam::sphereDir(anchor.vrBarP1.x, anchor.vrBarP1.y),
                                                              vrcam::sphereDir(anchor.vrBarP2.x, anchor.vrBarP2.y)),
                                                     -1.0f, 1.0f))
                              : 0.f;
    const float vrScreenLen = simDistance(bar.p1, bar.p2);

    OverlayAnchor a = anchor; // working copy; pushed as a capture on change

    // Apply the live drag for `target` to the working anchor `a`. VR endpoints re-pin to the cursor
    // direction; VR center rigid-rotates both endpoints by the cursor's spherical displacement; the
    // width grip maps the cursor's perpendicular distance from the bar axis to the bar thickness.
    const auto applyDrag = [&](DragTarget target, bool moveCenter) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float perpPx = std::abs((mouse.x - barCenter.x) * perp.x + (mouse.y - barCenter.y) * perp.y);
        if (vr) {
            if (target == DragTarget::Width) {
                a.vrBarWidthAngle =
                    std::max(0.01f, (vrScreenLen > 1.f) ? 2.f * perpPx * (vrAngLen / vrScreenLen) : 0.06f);
            } else if (target == DragTarget::P1) {
                vrcam::dirToYawPitch(vrScreenToDir(mouse, overlayVp_), a.vrBarP1.x, a.vrBarP1.y);
            } else if (target == DragTarget::P2) {
                vrcam::dirToYawPitch(vrScreenToDir(mouse, overlayVp_), a.vrBarP2.x, a.vrBarP2.y);
            } else if (moveCenter) {
                const glm::quat q = rotationBetween(dragStartMouseDir_, vrScreenToDir(mouse, overlayVp_));
                const glm::vec3 n1 = q * vrcam::sphereDir(dragStartAnchor_.vrBarP1.x, dragStartAnchor_.vrBarP1.y);
                const glm::vec3 n2 = q * vrcam::sphereDir(dragStartAnchor_.vrBarP2.x, dragStartAnchor_.vrBarP2.y);
                vrcam::dirToYawPitch(n1, a.vrBarP1.x, a.vrBarP1.y);
                vrcam::dirToYawPitch(n2, a.vrBarP2.x, a.vrBarP2.y);
            }
            return;
        }
        if (target == DragTarget::Width) {
            a.widthNorm = std::max(0.01f, 2.f * perpPx / std::max(1.f, overlayVp_.imageSize.y));
            return;
        }
        const ImVec2 deltaNorm = screenToNorm(mouse, overlayVp_) - dragStartMouseNorm_;
        constexpr float kMinBarLenNorm = 0.05f; // min length so an endpoint can't cross the other and invert
        if (moveCenter) {
            a.p1Norm = dragStartAnchor_.p1Norm + deltaNorm;
            a.p2Norm = dragStartAnchor_.p2Norm + deltaNorm;
        } else if (target == DragTarget::P1) {
            a.p1Norm = clampBarEndpoint(dragStartAnchor_.p1Norm + deltaNorm, a.p2Norm, dragStartAnchor_.p1Norm,
                                        kMinBarLenNorm);
        } else if (target == DragTarget::P2) {
            a.p2Norm = clampBarEndpoint(dragStartAnchor_.p2Norm + deltaNorm, a.p1Norm, dragStartAnchor_.p2Norm,
                                        kMinBarLenNorm);
        }
    };

    // Shift-hover preview & place: project the cursor onto the bar axis to a 0..1 displayed value,
    // show the bar at it (renderOverlay reads preview_), and place an action on the active axis on a
    // left-click. Works even when locked; takes precedence over move/resize and is skipped mid-drag so
    // pressing Shift during a drag can't hijack it.
    const auto activeRole = project.state.activeAxis;
    const bool activePresent = activeRole < StandardAxis::Count;
    bool shiftPlace = false;
    if (ImGui::GetIO().KeyShift && activePresent && overlayVp_.valid && bar.visible && vpHovered &&
        dragTarget == DragTarget::None && !isMovingSimulator) {
        const ImVec2 mouse = ImGui::GetMousePos();
        // Generous grab band around the (thin) bar so the cursor needn't sit exactly on it.
        if (pointToSegmentDist(mouse, barP1, barP2) <= std::max(barWidth, ImGui::GetFontSize() * 2.f)) {
            shiftPlace = true;
            const float t = std::clamp(((mouse.x - barP2.x) * direction.x + (mouse.y - barP2.y) * direction.y) /
                                           std::max(1.f, barLen),
                                       0.f, 1.f);
            preview_.active = true;
            preview_.axis = activeRole;
            preview_.displayedValue = t;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetHoveredID(ImGui::GetCurrentWindowRead()->ID);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const bool inv = project.activeSceneView.inverted;
                const float stored = inv ? 1.f - t : t;
                eq.push(EditRequestEvent{
                    .intent = {.kind = EditIntentKind::AddPoint,
                               .axis = activeRole,
                               .time = currentTime,
                               .pos = std::clamp(static_cast<int>(std::lround(stored * 100.f)), 0, 100)}});
                ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
            }
        }
    }

    if (shiftPlace) {
        dragTarget = DragTarget::None;
        isMovingSimulator = false;
    } else if (!state.lockedPosition && overlayVp_.valid && bar.visible) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float p1Dist = simDistance(mouse, barP1);
        const float p2Dist = simDistance(mouse, barP2);
        const float centerDist = simDistance(mouse, barCenter);
        const float widthDist = simDistance(mouse, widthGrip);
        const float handleRadius = std::max(8.f, barWidth / 2.f);

        ImGuiWindow *const simWindow = ImGui::GetCurrentWindowRead();

        // Register the handles as addressable, non-interactive items so UI tests can anchor to them
        // via ItemInfo. The actual hit-testing stays distance-based below; ItemAdd consumes no input.
        const ImVec2 handleExtent{handleRadius, handleRadius};
        ImGui::ItemAdd(ImRect(barP1 - handleExtent, barP1 + handleExtent), ImGui::GetID("##sim_p1"), nullptr,
                       ImGuiItemFlags_NoNav);
        ImGui::ItemAdd(ImRect(barP2 - handleExtent, barP2 + handleExtent), ImGui::GetID("##sim_p2"), nullptr,
                       ImGuiItemFlags_NoNav);
        ImGui::ItemAdd(ImRect(barCenter - handleExtent, barCenter + handleExtent), ImGui::GetID("##sim_center"),
                       nullptr, ImGuiItemFlags_NoNav);
        ImGui::ItemAdd(ImRect(widthGrip - handleExtent, widthGrip + handleExtent), ImGui::GetID("##sim_width"), nullptr,
                       ImGuiItemFlags_NoNav);

        if (dragTarget == DragTarget::Width) {
            ImGui::SetMouseCursor(resizeCursorForAxis(perp));
            ImGui::SetActiveID(simId, simWindow);
        } else if (dragTarget != DragTarget::None) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetActiveID(simId, simWindow);
        } else if (isMovingSimulator) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetActiveID(simId, simWindow);
        }

        if (dragTarget == DragTarget::None && !isMovingSimulator) {
            const auto beginDrag = [&]() {
                ImGui::SetActiveID(simId, simWindow);
                ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
                dragStartAnchor_ = anchor;
                dragStartMouseNorm_ = screenToNorm(mouse, overlayVp_);
                dragStartMouseDir_ = vrScreenToDir(mouse, overlayVp_);
            };
            // Width grip first — it sits just outside the bar so it shouldn't be shadowed by it.
            if (vpHovered && widthDist <= handleRadius) {
                ImGui::SetMouseCursor(resizeCursorForAxis(perp));
                ImGui::SetHoveredID(simWindow->ID);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    beginDrag();
                    dragTarget = DragTarget::Width;
                }
            } else if (vpHovered && p1Dist <= handleRadius) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetHoveredID(simWindow->ID);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    beginDrag();
                    dragTarget = DragTarget::P1;
                }
            } else if (vpHovered && p2Dist <= handleRadius) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetHoveredID(simWindow->ID);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    beginDrag();
                    dragTarget = DragTarget::P2;
                }
            } else if (vpHovered && centerDist <= handleRadius) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                ImGui::SetHoveredID(simWindow->ID);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    beginDrag();
                    isMovingSimulator = true;
                }
            } else {
                if (vpHovered && pointToSegmentDist(mouse, barP1, barP2) <= handleRadius &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] = false;
                }
                if (ImGui::GetActiveID() == simId) {
                    ImGui::ClearActiveID();
                }
            }
        }

        if (dragTarget != DragTarget::None) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                applyDrag(dragTarget, false);
                eq.push(CaptureOverlayAnchorEvent{a});
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                dragTarget = DragTarget::None;
                ImGui::ClearActiveID();
            }
        } else if (isMovingSimulator) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                applyDrag(DragTarget::None, true);
                eq.push(CaptureOverlayAnchorEvent{a});
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                isMovingSimulator = false;
                ImGui::ClearActiveID();
            }
        }
    } else {
        dragTarget = DragTarget::None;
        isMovingSimulator = false;
        if (ImGui::GetActiveID() == simId) {
            ImGui::ClearActiveID();
        }
    }

    // Right-click context menu for quick access to the 2D bar settings. The Video Player suppresses
    // its own menu over the bar (renderOverlay reports the hover), so this is the only menu there.
    // Kept outside the move/resize block above so it works even when the overlay is locked.
    if (overlayVp_.valid && bar.visible && vpHovered) {
        const ImVec2 m = ImGui::GetMousePos();
        if (pointToSegmentDist(m, barP1, barP2) <= std::max(8.f, barWidth / 2.f) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("Sim2DContext");
    }
    if (ImGui::BeginPopup("Sim2DContext")) {
        // Menu items push events like every other UI write — never mutate ScriptProject directly.
        if (ImGui::BeginMenu(Str::SimDisplay.id("sim_display"))) {
            if (ImGui::MenuItem(Str::SimIndicators.id("sim_indicators"), nullptr, state.enableIndicators))
                eq.push(
                    ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.enableIndicators = !s.enableIndicators; }});
            if (ImGui::MenuItem(Str::SimPosition.id("sim_position"), nullptr, state.enablePosition))
                eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.enablePosition = !s.enablePosition; }});
            if (ImGui::MenuItem(Str::SimHeightLines.id("sim_height_lines"), nullptr, state.enableHeightLines))
                eq.push(
                    ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.enableHeightLines = !s.enableHeightLines; }});
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Str::SimExtraLines.id("sim_extra_lines"))) {
            int extra = state.extraLinesCount;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
            if (ImGui::SliderInt("##extralines", &extra, 0, 20))
                eq.push(ModifyEvent<SimulatorState>{[extra](SimulatorState &s) { s.extraLinesCount = extra; }});
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Str::SimLocked.iconId(state.lockedPosition ? ICON_LOCK : ICON_LOCK_OPEN, "sim_locked"),
                            nullptr, state.lockedPosition))
            eq.push(ModifyEvent<SimulatorState>{[](SimulatorState &s) { s.lockedPosition = !s.lockedPosition; }});
        const bool inverted = project.activeSceneView.inverted;
        if (ImGui::MenuItem(Str::SimInvert.id("sim_invert_menu"), nullptr, inverted))
            eq.push(CaptureSimInvertedEvent{!inverted});
        ImGui::EndPopup();
    }

    ImGui::End();
}

bool ScriptSimulator::renderOverlay(ImDrawList *dl, const ScriptProject &project, const OverlayViewport &vp) {
    // Stash the viewport for render()'s interaction (which runs earlier in the frame, in the
    // Simulator window, and has no other way to reach the live video geometry).
    overlayVp_ = vp;

    if (ImGui::GetTopMostPopupModal() != nullptr)
        return false;
    // Anchored to video content: with no video to project onto, there's nothing to draw.
    if (!vp.valid)
        return false;

    const auto &state = project.simulator;
    const OverlayAnchor &anchor = project.activeSceneView.anchor;

    if (state.use3dSimulator) {
        if (strokerNode == nullptr)
            return false;
        const RectScreen r = rect3dScreen(anchor, vp);
        if (!r.visible) // anchor rotated out of view (VR) — hide the overlay
            return false;
        // Override the rect with this frame's projection (render3D set it from the previous frame).
        callbackDataPerspective.contentMin = r.min;
        callbackDataPerspective.contentSize = r.size;
        const ImVec2 perspMin = r.min;
        const ImVec2 perspMax = {perspMin.x + r.size.x, perspMin.y + r.size.y};

        // Clip the model + outline to the video content region so they don't spill over window chrome
        // and slide off smoothly at the edge instead of popping.
        dl->PushClipRect(vp.contentMin, vp.contentMin + vp.contentSize, true);
        dl->AddCallback(&ScriptSimulator::glCallbackFunc, &callbackDataPerspective);
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        dl->AddRect(perspMin, perspMax, ofs::theme::GetColorU32(AppCol_SimCrosshair), 0.f, 1.5f, ImDrawFlags_None);

        // Data labels: an axis gizmo anchored on the model so each value reads as a position/angle on
        // the model itself. Stroke (world Y) and Sway (world X) project to clean screen lines with a
        // neutral tick at origin and a marker at the live value; rotations are concentric needle-dials
        // centred on the model; Surge (world Z, straight down the view axis) can't show as a line, so it
        // reads out as a depth label. All strings use fmtScratch (no per-frame heap allocation).
        if (state.labels3dMask.any()) {
            const ofs::theme::Theme &theme = ofs::theme::getActive();
            const float opacity = ofs::theme::GetStyleVar(AppVar_SimGlobalOpacity);
            const float lineH = ImGui::GetTextLineHeight();

            const auto axisVal = [&](StandardAxis role) {
                if (preview_.active && role == preview_.axis)
                    return preview_.displayedValue;
                const float v = getAxisValue(project, role, project.playback.cursorPos);
                return project.activeSceneView.inverted ? 1.0f - v : v;
            };
            // A label shows only when its bit is set and the axis carries data.
            const auto shown = [&](StandardAxis role) {
                const auto idx = static_cast<size_t>(role);
                return state.labels3dMask.test(idx);
            };
            const auto col = [&](StandardAxis role) { return applyOpacity(ImColor(standardAxisColor(role)), opacity); };
            const auto dim = [&](StandardAxis role) {
                return applyOpacity(ImColor(standardAxisColorDim(role)), opacity);
            };
            const auto dofWord = [](StandardAxis role) -> const char * {
                switch (role) {
                case StandardAxis::L0:
                    return Str::SimDofStroke.c_str();
                case StandardAxis::L1:
                    return Str::SimDofSurge.c_str();
                case StandardAxis::L2:
                    return Str::SimDofSway.c_str();
                case StandardAxis::R0:
                    return Str::SimDofTwist.c_str();
                case StandardAxis::R1:
                    return Str::SimDofRoll.c_str();
                case StandardAxis::R2:
                    return Str::SimDofPitch.c_str();
                default:
                    return "";
                }
            };
            // Drop-shadowed text — the gizmo has no backing panel, so a 1px shadow keeps labels legible
            // over arbitrary video.
            const ImU32 shadow = applyOpacity(ImColor(ofs::theme::GetColorU32(AppCol_TextShadow)), opacity * 0.8f);
            const auto text = [&](ImVec2 p, ImU32 c, const char *s) {
                dl->AddText({p.x + 1.f, p.y + 1.f}, shadow, s);
                dl->AddText(p, c, s);
            };

            // Same fixed perspective camera the model is rendered through (see render3D). Recomputed
            // here from constants so a world point can be projected into the model's square screen rect.
            const auto perspProj = glm::perspective(glm::radians(40.f), 1.0f, 0.1f, 100.f);
            const auto perspView =
                glm::lookAt(glm::vec3{0.f, 0.f, 6.f}, glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});
            const ImVec2 sz = r.size;
            const auto project = [&](glm::vec3 p) -> ImVec2 {
                const glm::vec4 clip = perspProj * perspView * glm::vec4(p, 1.f);
                const float w = (std::abs(clip.w) > 1e-4f) ? clip.w : 1e-4f;
                return {perspMin.x + (clip.x / w * 0.5f + 0.5f) * sz.x, perspMin.y + (0.5f - clip.y / w * 0.5f) * sz.y};
            };

            const ImVec2 origin = project({0.f, 0.f, 0.f});

            // Linear marker: filled axis-coloured dot with a thin light outline for contrast.
            const auto marker = [&](ImVec2 p, ImU32 c) {
                dl->AddCircleFilled(p, 4.f, c);
                dl->AddCircle(p, 4.f, shadow, 0, 1.f);
            };

            // Stroke L0 — vertical axis (world Y).
            if (shown(StandardAxis::L0)) {
                const float v = axisVal(StandardAxis::L0);
                const ImVec2 top = project({0.f, state.strokeRange, 0.f});
                const ImVec2 bot = project({0.f, -state.strokeRange, 0.f});
                dl->AddLine(bot, top, dim(StandardAxis::L0), 1.5f);
                dl->AddLine({origin.x - 5.f, origin.y}, {origin.x + 5.f, origin.y}, dim(StandardAxis::L0), 1.5f);
                marker(project({0.f, glm::mix(-state.strokeRange, state.strokeRange, v), 0.f}), col(StandardAxis::L0));
                const char *lbl = fmtScratch("{} {} {:.0f}%", standardAxisShortName(StandardAxis::L0),
                                             dofWord(StandardAxis::L0), v * 100.f);
                // Pinned to the top edge of the rect, mirroring how Surge (L1) reads out at the bottom.
                text({origin.x - ImGui::CalcTextSize(lbl).x * 0.5f, perspMin.y + 4.f}, col(StandardAxis::L0), lbl);
            }

            // Sway L2 — horizontal axis (world X). Mapping mirrors render3D: v=0 → +X (screen right).
            if (shown(StandardAxis::L2)) {
                const float v = axisVal(StandardAxis::L2);
                const ImVec2 right = project({state.swayRange, 0.f, 0.f});
                const ImVec2 left = project({-state.swayRange, 0.f, 0.f});
                dl->AddLine(left, right, dim(StandardAxis::L2), 1.5f);
                dl->AddLine({origin.x, origin.y - 5.f}, {origin.x, origin.y + 5.f}, dim(StandardAxis::L2), 1.5f);
                marker(project({glm::mix(state.swayRange, -state.swayRange, v), 0.f, 0.f}), col(StandardAxis::L2));
                const char *lbl = fmtScratch("{} {} {:.0f}%", standardAxisShortName(StandardAxis::L2),
                                             dofWord(StandardAxis::L2), v * 100.f);
                text({right.x + 6.f, right.y - lineH * 0.5f}, col(StandardAxis::L2), lbl);
            }

            // Surge L1 — world Z runs straight down the view axis, so there's no on-screen line; read it
            // out as a depth label centred under the model.
            if (shown(StandardAxis::L1)) {
                const float v = axisVal(StandardAxis::L1);
                const char *lbl = fmtScratch("{} {} {:.0f}% {}", standardAxisShortName(StandardAxis::L1),
                                             dofWord(StandardAxis::L1), v * 100.f, Str::SimDepth.c_str());
                text({origin.x - ImGui::CalcTextSize(lbl).x * 0.5f, perspMax.y - lineH - 4.f}, col(StandardAxis::L1),
                     lbl);
            }

            // Rotation dials — concentric needle gauges centred on the model. Reference needle points up
            // (neutral); the coloured needle rotates by the mapped degrees (1:1 on screen; exact for Roll
            // about the view axis, a readable proxy for Twist/Pitch). The numeric label honours the
            // degrees/percent choice.
            constexpr StandardAxis kRot[] = {StandardAxis::R0, StandardAxis::R1, StandardAxis::R2};
            int ring = 0;
            for (StandardAxis role : kRot) {
                if (!shown(role))
                    continue;
                const float v = axisVal(role);
                const float radius = 24.f + static_cast<float>(ring) * 15.f;
                ++ring;
                const float refAng = -IM_PI * 0.5f;
                const float curAng = refAng + glm::radians(std::clamp(rotationDegrees(role, v, state), -179.f, 179.f));
                dl->AddCircle(origin, radius, dim(role), 0, 1.f);
                dl->AddLine(origin, {origin.x + radius * std::cos(refAng), origin.y + radius * std::sin(refAng)},
                            dim(role), 1.f);
                const ImVec2 tip = {origin.x + radius * std::cos(curAng), origin.y + radius * std::sin(curAng)};
                dl->AddLine(origin, tip, col(role), 2.f);
                if (std::abs(curAng - refAng) > 0.01f) {
                    dl->PathArcTo(origin, radius, std::min(refAng, curAng), std::max(refAng, curAng), 24);
                    dl->PathStroke(col(role), 2.f);
                }
                const char *lbl =
                    state.labels3dInDegrees
                        ? fmtScratch("{} {:.0f}\xc2\xb0", standardAxisShortName(role), rotationDegrees(role, v, state))
                        : fmtScratch("{} {:.0f}%", standardAxisShortName(role), v * 100.f);
                text({tip.x + 4.f, tip.y - lineH * 0.5f}, col(role), lbl);
            }
        }

        // Shift-hover place: a value readout pinned to the cursor so the placement value is always
        // visible, even with the model's data labels turned off.
        if (preview_.active) {
            const ImVec2 cur = ImGui::GetMousePos();
            const char *lbl = fmtScratch("{:.0f}%", preview_.displayedValue * 100.f);
            dl->AddText({cur.x + 13.f, cur.y + 1.f}, ofs::theme::GetColorU32(AppCol_TextShadow), lbl);
            dl->AddText({cur.x + 12.f, cur.y}, ofs::theme::GetColorU32(AppCol_Sim2DText), lbl);
        }

        dl->PopClipRect();
        // Report hover so the Video Player suppresses its context menu over the model.
        return ImGui::IsMouseHoveringRect(perspMin, perspMax);
    }

    // ---- 2D bar ----

    const BarScreen bar = barScreen(anchor, vp);
    if (!bar.visible) // bar anchored off-screen (VR) — hide it
        return false;

    const ofs::theme::Theme &theme = ofs::theme::getActive();
    // Thickness is content-space (scales with zoom); border/line widths derive from it.
    const float barWidth = bar.thickness;
    const float borderWidth = barWidth * kBorderRatio;
    const float globalOpacity = ofs::theme::GetStyleVar(AppVar_SimGlobalOpacity);
    const float lineWidth = std::clamp(barWidth * kLineRatio, kMinLineWidth, kMaxLineWidth);
    const float extraLineWidth = lineWidth;

    const double currentTime = project.playback.cursorPos;
    float currentPos = 0.f;
    const VectorSet<ScriptAxisAction> *sampledActions = nullptr;
    const auto activeIdx = static_cast<size_t>(project.state.activeAxis);
    if (project.state.activeAxis < StandardAxis::Count) {
        const auto &axis = project.axes[activeIdx];
        currentPos = getAxisValue(project, axis.role, currentTime) * 100.f;
        if (project.activeSceneView.inverted) {
            currentPos = 100.f - currentPos;
        }
        sampledActions = axis.resolved ? &axis.resolved->actions : &axis.actions;
    }

    // Shift-hover preview (set in render()) overrides the live fill with the value the cursor maps to.
    if (preview_.active && preview_.axis == project.state.activeAxis) {
        currentPos = preview_.displayedValue * 100.f;
    }

    const ImVec2 direction = simNormalize(bar.p1 - bar.p2);
    const ImVec2 barP1 = bar.p1 - (direction * (borderWidth / 2.f));
    const ImVec2 barP2 = bar.p2 + (direction * (borderWidth / 2.f));
    const float barLen = simDistance(barP1, barP2);
    ImVec2 perp = simNormalize(bar.p1 - bar.p2);
    perp = ImVec2{-perp.y, perp.x};

    // Clip to the video content region so the bar slides smoothly off the edge (VR rotation / 2D
    // pan) instead of popping out or spilling over the window chrome.
    dl->PushClipRect(vp.contentMin, vp.contentMin + vp.contentSize, true);

    // Background fill.
    dl->AddLine(barP1 + direction, barP2 - direction, applyOpacity(theme.colors[AppCol_Sim2DBack], globalOpacity),
                barWidth - borderWidth + 1.f);

    // Front fill (position-driven).
    const float percent = currentPos / 100.f;
    dl->AddLine(barP2 + (direction * barLen * percent), barP2,
                applyOpacity(theme.colors[AppCol_Sim2DFront], globalOpacity), barWidth - borderWidth + 1.f);

    // Border quad.
    if (borderWidth > 0.f) {
        const ImVec2 halfWidth = perp * (barWidth / 2.f);
        dl->AddQuad(bar.p1 - halfWidth, bar.p1 + halfWidth, bar.p2 + halfWidth, bar.p2 - halfWidth,
                    applyOpacity(theme.colors[AppCol_Sim2DBorder], globalOpacity), borderWidth);
    }

    // Height lines at 10% intervals within 0-100.
    if (state.enableHeightLines) {
        for (int i = 1; i < 10; ++i) {
            const float pos = static_cast<float>(i) * 10.f;
            const ImVec2 lp1 =
                barP2 + (direction * barLen * (pos / 100.f)) - (perp * (barWidth / 2.f)) + (perp * (borderWidth / 2.f));
            const ImVec2 lp2 =
                barP2 + (direction * barLen * (pos / 100.f)) + (perp * (barWidth / 2.f)) - (perp * (borderWidth / 2.f));
            dl->AddLine(lp1, lp2, applyOpacity(theme.colors[AppCol_Sim2DLines], globalOpacity), lineWidth);
        }
    }

    // Extra lines beyond the 0-100 range.
    if (state.extraLinesCount > 0) {
        for (int i = -state.extraLinesCount; i < 1; ++i) {
            const float pos = static_cast<float>(i) * 10.f;
            const ImVec2 lp1 =
                barP2 + (direction * barLen * (pos / 100.f)) - (perp * (barWidth / 2.f)) + (perp * (borderWidth / 2.f));
            const ImVec2 lp2 =
                barP2 + (direction * barLen * (pos / 100.f)) + (perp * (barWidth / 2.f)) - (perp * (borderWidth / 2.f));
            dl->AddLine(lp1, lp2, applyOpacity(theme.colors[AppCol_Sim2DLines], globalOpacity), extraLineWidth);
        }
        for (int i = 10; i < (11 + state.extraLinesCount); ++i) {
            const float pos = static_cast<float>(i) * 10.f;
            const ImVec2 lp1 =
                barP2 + (direction * barLen * (pos / 100.f)) - (perp * (barWidth / 2.f)) + (perp * (borderWidth / 2.f));
            const ImVec2 lp2 =
                barP2 + (direction * barLen * (pos / 100.f)) + (perp * (barWidth / 2.f)) - (perp * (borderWidth / 2.f));
            dl->AddLine(lp1, lp2, applyOpacity(theme.colors[AppCol_Sim2DLines], globalOpacity), extraLineWidth);
        }
    }

    // Indicator lines for the previous and next script actions.
    if (state.enableIndicators && sampledActions != nullptr && !sampledActions->empty()) {
        const auto &actions = *sampledActions;
        const ScriptAxisAction *prevAction = nullptr;
        const ScriptAxisAction *nextAction = nullptr;

        const ScriptAxisAction key{currentTime, 0};
        auto it = actions.lowerBound(key);

        if (it != actions.end() && std::abs(it->at - currentTime) <= 0.02) {
            prevAction = &*it;
            const auto nit = std::next(it);
            if (nit != actions.end()) {
                nextAction = &*nit;
            }
        } else {
            if (it != actions.begin()) {
                prevAction = &*std::prev(it);
            }
            if (it != actions.end()) {
                nextAction = &*it;
            }
        }

        const auto drawIndicator = [&](const ScriptAxisAction *action) {
            if (action == nullptr) {
                return;
            }
            if (action->pos <= 0 || action->pos >= 100) {
                return;
            }
            const int displayPos = project.activeSceneView.inverted ? 100 - action->pos : action->pos;
            const auto fpos = static_cast<float>(displayPos);
            const ImVec2 lp1 = barP2 + (direction * barLen * (fpos / 100.f)) - (perp * (barWidth / 2.f)) +
                               (perp * (borderWidth / 2.f));
            const ImVec2 lp2 = barP2 + (direction * barLen * (fpos / 100.f)) + (perp * (barWidth / 2.f)) -
                               (perp * (borderWidth / 2.f));
            const ImVec2 center = barP2 + (direction * barLen * (fpos / 100.f));
            dl->AddLine(lp1, lp2, applyOpacity(theme.colors[AppCol_Sim2DIndicator], globalOpacity), lineWidth);
            const char *tmp = fmtScratch("{}", displayPos);
            const ImVec2 halfText = ImGui::CalcTextSize(tmp) / 2.f;
            dl->AddText(center - halfText, applyOpacity(theme.colors[AppCol_Sim2DText], globalOpacity), tmp);
        };

        drawIndicator(prevAction);
        drawIndicator(nextAction);
    }

    // Current position label at bar center.
    if (state.enablePosition) {
        const char *tmp = fmtScratch("{:.0f}", currentPos);
        const ImVec2 halfText = ImGui::CalcTextSize(tmp) / 2.f;
        dl->AddText(barP2 + direction * barLen * 0.5f - halfText,
                    applyOpacity(theme.colors[AppCol_Sim2DText], globalOpacity), tmp);
    }

    // No visible move/resize handles — they cluttered the overlay. Hit-testing is distance-based
    // (see render()), and the resize cursor on hover signals the grip without drawing anything.

    // Shift-hover place: a value readout pinned to the cursor signals where a click would land.
    if (preview_.active && preview_.axis == project.state.activeAxis) {
        const ImVec2 cur = ImGui::GetMousePos();
        const char *lbl = fmtScratch("{:.0f}%", currentPos);
        dl->AddText({cur.x + 13.f, cur.y + 1.f}, ofs::theme::GetColorU32(AppCol_TextShadow), lbl);
        dl->AddText({cur.x + 12.f, cur.y}, ofs::theme::GetColorU32(AppCol_Sim2DText), lbl);
    }

    dl->PopClipRect();
    // Report hover so the Video Player suppresses its context menu over the bar.
    return pointToSegmentDist(ImGui::GetMousePos(), barP1, barP2) <= std::max(8.f, barWidth / 2.f);
}

} // namespace ofs
