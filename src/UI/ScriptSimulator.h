#pragma once

#include "Core/SceneView.h"
#include "Core/SimulatorSettings.h"
#include "Core/StandardAxis.h"
#include "Scenegraph/GltfLoader.h"
#include "Scenegraph/SceneGraph.h"
#include "UI/OverlayViewport.h"
#include "imgui.h"
#include <glm/glm.hpp>
#include <memory>

namespace ofs {

struct ScriptProject;
class EventQueue;

class ScriptSimulator {
  public:
    // Takes the EventQueue to register its ResetOverlayAnchorEvent handler (must be before freeze()).
    explicit ScriptSimulator(EventQueue &eq);

    void render(const ScriptProject &project, EventQueue &eq, bool &open, bool vpHovered);
    // The simulator overlay over the video, run in two passes (see OverlayPhase). The Input pass hit-tests
    // the 2D bar / 3D model, drives any drag, and returns true when the cursor is over the overlay (or
    // actively dragging it) so the Video Player yields its pan-grab and right-click menu there. The Draw
    // pass renders the bar/model on top of the image and always returns false. The interaction lives here,
    // not in render(), so the 2D bar stays interactive while the Simulator window is hidden in 2D mode.
    bool renderOverlay(ImDrawList *dl, const ScriptProject &project, EventQueue &eq, const OverlayViewport &vp,
                       bool vpHovered, OverlayPhase phase);

  private:
    void render3D(const ScriptProject &project, EventQueue &eq, double currentTime, const SimulatorState &state,
                  const OverlayAnchor &anchor, bool vpHovered);
    static void glCallbackFunc(const ImDrawList *, const ImDrawCmd *);

    // Most recent video-content viewport, captured in renderOverlay so render()'s interaction (which
    // runs earlier in the frame, in the Simulator window) can project/unproject the anchor. One frame
    // stale is fine — the video transform moves smoothly.
    OverlayViewport overlayVp_{};

    // Set by the ResetOverlayAnchorEvent handler (command / menu); consumed in renderOverlay where the
    // live viewport + current anchor are known to compute the centered anchor, exactly as the
    // middle-double-click gesture does. The gesture sets nothing here — it is read inline.
    bool overlayResetPending_ = false;

    // 2D bar drag state. The anchor + mouse position (normalized) at gesture start let endpoint and
    // center drags apply a delta in content space, preserving the grab offset. In VR, center drags
    // rigid-rotate both endpoint dirs by the rotation from dragStartMouseDir_ to the live cursor dir.
    OverlayAnchor dragStartAnchor_{};
    ImVec2 dragStartMouseNorm_{};
    glm::vec3 dragStartMouseDir_{0.f, 0.f, 1.f}; // VR: cursor sphere dir at gesture start
    enum class DragTarget { None, P1, P2, Width };
    DragTarget dragTarget = DragTarget::None;
    bool isMovingSimulator = false;

    struct Sim3DCallbackData {
        sg::SceneGraph *sceneGraph{};
        ImVec2 contentMin{};
        ImVec2 contentSize{};
        glm::mat4 viewMatrix{1.f};
        glm::mat4 projMatrix{1.f};
        bool updateTransforms{false};
    };

    // scene3d must be declared before gltfScene so nodes outlive mesh pointers
    sg::SceneGraph scene3d;
    std::unique_ptr<sg::GltfScene> gltfScene;
    sg::SceneNode *strokerNode{};
    Sim3DCallbackData callbackDataOrtho[2]{};    // [0]=top view, [1]=side view (inside window)
    Sim3DCallbackData callbackDataPerspective{}; // perspective overlay (over video)

    // 3D perspective overlay drag/resize state (anchor + size at gesture start, plus the start mouse
    // position in normalized content space for the move delta). In VR the move rigid-rotates the model
    // by the cursor's spherical displacement, so the start cursor dir is captured too (like the 2D bar).
    bool isMoving3d = false;
    OverlayAnchor dragStart3dAnchor_{};
    ImVec2 dragStart3dMouseNorm_{};
    glm::vec3 dragStart3dMouseDir_{0.f, 0.f, 1.f}; // VR: cursor sphere dir at gesture start
    bool isResizing3d = false;
    float startResize3dSize = 0.f;

    // Shift-hover "preview & place". While Shift is held and the cursor is over the overlay, the
    // active axis is shown at the value the cursor maps to — 2D: position along the bar; 3D: the
    // active DOF's on-screen direction (vertical stroke, horizontal sway, angular rotations) — and a
    // left-click commits an action there. Computed in render()/render3D() (which run before
    // renderOverlay() in the frame) and consumed by renderOverlay() so the bar fill / model / labels
    // reflect the previewed value the same frame. displayedValue is already in display space (any
    // per-scene inversion applied), so storing an action un-inverts it.
    struct ScrubPreview {
        bool active = false;
        StandardAxis axis = StandardAxis::L0;
        float displayedValue = 0.5f; // 0..1
    };
    ScrubPreview preview_{};
};

} // namespace ofs
