#pragma once
#include "Video/VideoMode.h"

namespace ofs {
struct VideoPlayerState {
    VideoMode activeMode = VideoMode::Full;
    float resolutionScale = 1.0f;
    // When set, the player ignores pan/zoom input (2D and VR) so the framing can't be nudged by accident.
    bool locked = false;
};
} // namespace ofs
