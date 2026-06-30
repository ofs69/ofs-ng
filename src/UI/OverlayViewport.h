#pragma once

#include "Video/VideoMode.h"
#include "imgui.h"

namespace ofs {

// Snapshot of the video window's live geometry + camera for one frame, handed to the simulator
// overlay so it can project its content-space anchor (OverlayAnchor) onto the screen. Produced by
// VideoPlayerWindow each frame and consumed by ScriptSimulator::renderOverlay. The overlay also
// keeps the most recent one to convert mouse positions during drag (one-frame stale is fine).
struct OverlayViewport {
    VideoMode mode = VideoMode::Full;
    // Screen-space content region (the whole video panel; the VR image fills exactly this).
    ImVec2 contentMin{};
    ImVec2 contentSize{};
    // Screen-space rect of the displayed 2D image inside the content region (encodes pan + zoom).
    ImVec2 imageMin{};
    ImVec2 imageSize{};
    // Live VR camera (matches VrShader uniforms).
    ImVec2 vrRotation{0.5f, 0.5f};
    float vrZoom = 0.5f;
    bool valid = false; // false in the no-video / audio-only branches: skip overlay projection

    [[nodiscard]] float contentAspect() const { return (contentSize.y > 0.0f) ? contentSize.x / contentSize.y : 1.0f; }
};

// The simulator overlay runs in two passes per frame. The Input pass runs *before* the video player's
// own pan/zoom grab, hit-tests the overlay handles, drives any drag, and reports whether it owns the
// pointer — so the pan can yield in the same frame instead of racing a one-frame-stale latch. The Draw
// pass runs after the image is drawn (so the overlay sits on top) and only renders. See ScriptSimulator.
enum class OverlayPhase { Input, Draw };

} // namespace ofs
