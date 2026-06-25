#pragma once

#include "Core/SceneView.h"

namespace ofs {

// ProjectManager → VideoPlayerWindow: the active scene changed (the cursor crossed into a
// different chapter, or a capture happened); snap the live video camera to this framing.
// The overlay-anchor half is applied via ScriptProject::activeSceneView, read directly
// during render, so it is not carried here.
struct RestoreSceneViewEvent {
    VideoFraming framing;
};

// Capture-on-adjust is split into two half-events because the overlay anchor and the video
// framing live in different windows (ScriptSimulator vs VideoPlayerWindow). Each updates only
// its half of the SceneView for the chapter containing the cursor (or the project-level
// fallback); ProjectManager merges, preserving the other half. A single full-snapshot event
// would let whichever window pushed it clobber the half it doesn't own.

// ScriptSimulator → ProjectManager: the user moved/resized the simulator overlay.
struct CaptureOverlayAnchorEvent {
    OverlayAnchor anchor;
};

// VideoPlayerWindow → ProjectManager: the user panned/zoomed/rotated the video.
struct CaptureVideoFramingEvent {
    VideoFraming framing;
};

// ScriptSimulator → ProjectManager: the user toggled the simulator's Invert checkbox. A third
// per-scene half (like anchor/framing) so it restores per chapter and doesn't clobber the others.
struct CaptureSimInvertedEvent {
    bool inverted;
};

} // namespace ofs
