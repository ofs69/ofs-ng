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

// Reset intents — the command/menu counterpart of the middle-double-click "reset view" gesture, kept
// as one-shot intents (not the resolved value) because the reset target depends on live window state
// the command's run(EventQueue&) can't reach: the video framing reset must snap VideoPlayerWindow's
// transient camera, and the overlay reset needs the current viewport (VR projection) + lock state held
// by ScriptSimulator. Each window subscribes and does its own half, exactly as the gesture does.

// → VideoPlayerWindow: recenter pan/zoom (2D) or VR camera to the default framing.
struct ResetVideoFramingEvent {};

// → ScriptSimulator: recenter the simulator overlay (2D bar or 3D model). A locked overlay ignores it,
// same as the drag path, so a "reset both" doesn't drag a pinned overlay out from under the video.
struct ResetOverlayAnchorEvent {};

} // namespace ofs
