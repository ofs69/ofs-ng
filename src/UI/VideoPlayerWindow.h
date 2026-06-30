#pragma once

#include "Core/EventQueue.h"
#include "Core/SceneView.h"
#include "Core/ScriptProject.h"
#include "UI/OverlayViewport.h"
#include "Util/SmoothedFloat.h"
#include "Video/VideoPlayerSettings.h"
#include "imgui.h"
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <optional>

namespace ofs {
class VideoPlayer;
class VrShader;

class VideoPlayerWindow {
  public:
    // Takes the EventQueue to register its RestoreSceneViewEvent handler (must be before freeze()).
    explicit VideoPlayerWindow(EventQueue &eq);

    ~VideoPlayerWindow();

    void onImGuiRender(const ScriptProject &project, EventQueue &eq, VideoPlayer &player);
    // The callback runs the overlay in two passes (OverlayPhase). The Input pass runs before this
    // window's pan/zoom grab and returns true when the cursor is over the overlay (or actively dragging
    // it), so the pan and the right-click context menu yield there in the same frame; the Draw pass runs
    // after the image and renders on top (its return is ignored).
    void
    setOverlayCallback(std::function<bool(ImDrawList *, const OverlayViewport &, bool vpHovered, OverlayPhase)> cb) {
        overlayCallback = std::move(cb);
    }
    bool isWindowHovered() const { return windowHovered; }

  private:
    bool open = true;

    // 2D Pan/Zoom. The zoom eases toward its target via SmoothedFloat (animation rule), driven by
    // wheel input and chapter-cross glides; the window owns only the target and the per-step side
    // effects (zoom-toward-cursor pan, render-resolution sizing).
    SmoothedFloat zoom_{1.0f};
    ImVec2 translation = {0.0f, 0.0f};
    bool dragging = false;
    ImVec2 dragStartTranslation_ = {0.0f, 0.0f}; // pan at drag start; release persists only if it moved

    // VR State
    std::unique_ptr<VrShader> vrShader;
    // Neutral camera: yaw centered, pitch 0 (forward, +Z). y stays in [0,1] (pitch ±90°).
    ImVec2 vrRotation = {0.5f, 0.5f};
    SmoothedFloat vrZoom_{0.5f};
    // Response time constant (s) for the eased zoom chase; ~1/15 keeps the prior wheel-zoom feel.
    static constexpr float kZoomTau = 1.0f / 15.0f;
    // World sphere direction grabbed under the cursor at drag-start; kept pinned to the pointer for
    // the whole drag (see ofs::vrcam::dragRotation). Only meaningful while `dragging`.
    glm::vec3 vrGrabDir = {0.0f, 0.0f, 1.0f};

    // Temporary rendering state for VR callback
    float lastContentWidth = 0.0f;
    float lastContentHeight = 0.0f;
    float lastVideoAspect = 1.0f;

    bool hovered = false;
    bool windowHovered = false;

    // Last render size pushed to the player; lets us push SetRenderSizeEvent only on change
    // instead of every frame.
    int lastReqWidth = 0;
    int lastReqHeight = 0;

    // Set by the RestoreSceneViewEvent handler; applied to the live camera transients on the next
    // render (where contentSize is known to denormalize the pan). Cleared once applied.
    std::optional<VideoFraming> pendingFraming;
    // The pending restore's *final* framing (== pendingFraming on a snap) and whether it is a glide
    // step. While gliding, the render resolution keys off `pendingSettle` (the constant final zoom) so
    // the FBO resizes once, and the live zoom lerp is suppressed (see `gliding`) so it doesn't fight
    // the per-frame interpolation ProjectManager drives.
    VideoFraming pendingSettle;
    bool pendingAnimating_ = false;
    // True while a chapter-cross glide is being driven frame-by-frame from ProjectManager. Set when an
    // animating restore is applied, cleared on a snap/final frame or a frame with no restore — so if a
    // glide is cancelled mid-flight (e.g. an overlay drag), the live zoom lerp resumes and settles.
    bool gliding = false;
    // True while a view-reset (middle-double-click gesture or the "Reset Video View" command) is
    // pending: pendingFraming is set to the defaults, and once applied next render we also push a
    // CaptureVideoFramingEvent to persist it. Distinguishes the reset path from a RestoreSceneView
    // (which sets pendingFraming too, but restores *from* storage and must not re-persist).
    bool framingResetPending_ = false;
    // Snap the live camera to the default framing and mark it for persistence. Shared by the gesture
    // and the ResetVideoFramingEvent handler so both reset through one path.
    void requestFramingReset() {
        pendingFraming = VideoFraming{};
        pendingSettle = VideoFraming{};
        pendingAnimating_ = false;
        framingResetPending_ = true;
    }
    // Build a VideoFraming snapshot of the current camera (pan stored as a fraction of contentSize,
    // zoom as the lerp target = the user's intent) to push as a CaptureVideoFramingEvent. Video mode is
    // project-level, not part of the framing, so it is not a parameter here.
    [[nodiscard]] VideoFraming currentFraming(const ImVec2 &contentSize) const;

    std::function<bool(ImDrawList *, const OverlayViewport &, bool vpHovered, OverlayPhase)> overlayCallback;
};
} // namespace ofs
