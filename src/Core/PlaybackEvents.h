#pragma once

#include "Core/OverlaySettings.h"
#include "Core/SimulatorSettings.h"
#include "Video/VideoMode.h"

#include <string>

namespace ofs {

struct PlayPauseEvent {};

struct LoadVideoEvent {
    std::string path;
};

struct CloseVideoEvent {};

struct VolumeChangedEvent {
    float volume;
};

struct SeekEvent {
    double time;
};

// Hover-scrub request for the timeline frame preview. Pushed every hover frame by the seek-bar UI;
// VideoPreview coalesces (latest-wins, settle-gated) so fast flicks drop intermediate targets.
struct PreviewSeekRequestEvent {
    double time;
};

// Enable/disable the hover frame preview. Pushed by OfsApp when AppSettings.showTimelinePreview
// changes (and once at startup). VideoPreview lazily creates its mpv engine on enable and tears it
// down on disable; handled on the main thread during drain (mpv create/destroy is main-thread).
struct SetPreviewEnabledEvent {
    bool enabled;
};

struct PlaybackSpeedEvent {
    float speed;
};

// Toggle the "pause playback on seek" policy. Pushed by OfsApp from AppSettings.pauseOnSeek (once at
// startup and whenever the preference changes); the player caches it and applies it in its seek handler.
struct SetPauseOnSeekEvent {
    bool enabled;
};

struct PlayStateChangedEvent {
    bool playing;
};

struct SpeedChangedEvent {
    float speed;
};

struct MediaChangedEvent {
    std::string path;
};

struct DurationChangedEvent {
    double duration;
};

struct SimulatorPositionChangedEvent {
    SimulatorState state;
};

struct OverlaySettingsChangedEvent {
    OverlayState state;
};

struct VideoModeChangedEvent {
    VideoMode mode;
};

struct VideoResolutionChangedEvent {
    float scale;
};

// Render-target size the video window wants this frame (derived from its content region). The
// player handles it idempotently, so the window pushes only when the requested size changes.
struct SetRenderSizeEvent {
    int width;
    int height;
};

struct ChangeDummyDurationEvent {
    double durationSeconds;
};

struct ChangeMediaPathEvent {
    std::string path; // empty = unload video
};

} // namespace ofs
