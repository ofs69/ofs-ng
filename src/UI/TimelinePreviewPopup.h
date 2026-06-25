#pragma once

#include "imgui.h"
#include <memory>

namespace ofs {

struct ScriptProject;
class EventQueue;
class VideoPreview;
class VrShader;

// Renders the hover frame preview as a tooltip near the cursor. Owns a VrShader so the VR/360 case
// can be projected to match the main player's view. Site-agnostic: callers pass the hovered time
// and the current playhead time, so the same helper can serve any timeline-like widget.
class TimelinePreviewPopup {
  public:
    TimelinePreviewPopup();
    ~TimelinePreviewPopup();

    // Pushes a PreviewSeekRequestEvent for `hoverTime`, then (if a frame is ready) draws the preview
    // tooltip — a 2D image, or the equirect projected through the current VR framing — plus the
    // hovered timestamp and signed offset from the playhead. Falls back to a text-only time tooltip
    // until the engine has a frame (feature off, or first frame not yet decoded).
    void render(const ScriptProject &project, EventQueue &eq, const VideoPreview &preview, double hoverTime,
                double currentTime);

  private:
    std::unique_ptr<VrShader> vrShader;
    // Snapshot for the deferred VR draw callback (it runs after render() returns).
    ImVec2 vrRotation = {0.5f, 0.5f};
    float vrZoom = 0.5f;
    float contentAspect = 1.0f;
    float videoAspect = 1.0f;
};

} // namespace ofs
