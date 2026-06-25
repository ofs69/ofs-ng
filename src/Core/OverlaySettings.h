#pragma once

#include <algorithm>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace ofs {

enum class ScriptingOverlay : int32_t {
    Frame,
    Tempo,
};

inline constexpr int kTempoSubdivisionCount = 10;

// Beats per grid line for each snap subdivision (4 beats == one 4/4 measure). beatTime =
// (60 / bpm) * kTempoBeatMultiples[i]; a measure (downbeat) line falls every
// (4 / kTempoBeatMultiples[i]) grid lines. Order must match kTempoSubdivisionNames and the
// AppCol_TempoLine0..9 theme slots.
inline constexpr float kTempoBeatMultiples[kTempoSubdivisionCount] = {
    4.f * 1.f,          4.f * (1.f / 2.f),  4.f * (1.f / 4.f),  4.f * (1.f / 8.f),  4.f * (1.f / 12.f),
    4.f * (1.f / 16.f), 4.f * (1.f / 24.f), 4.f * (1.f / 32.f), 4.f * (1.f / 48.f), 4.f * (1.f / 64.f),
};

inline constexpr const char *kTempoSubdivisionNames[kTempoSubdivisionCount] = {
    "1/1 (measure)", "1/2", "1/4", "1/8", "1/12", "1/16", "1/24", "1/32", "1/48", "1/64",
};

struct OverlayState {
    ScriptingOverlay overlay = ScriptingOverlay::Frame;

    // Frame Overlay Settings
    float frameFps = 30.0f;

    // Tempo Overlay Settings
    float tempoBpm = 120.0f;
    float tempoOffsetSeconds = 0.0f; // phase-shift the whole beat grid to align beat 1 with the music
    int tempoMeasureIndex = 2;       // index into kTempoBeatMultiples; default 1/4 (one grid line per beat)
};

// Seconds per tempo grid line, honoring the measure subdivision. The single source of truth for the
// beat duration (the time-nudge step, the Frame navigator channel, and the tempo grid all derive from it).
inline double tempoBeatTime(const OverlayState &s) {
    const int mi = std::clamp(s.tempoMeasureIndex, 0, kTempoSubdivisionCount - 1);
    return (60.0 / static_cast<double>(s.tempoBpm)) * kTempoBeatMultiples[mi];
}

// Seconds one move/step travels under the active overlay: a frame interval, or a tempo beat. This is the
// unit a time-nudge (MoveSelectionTime) advances selected points by. The Frame navigator channel resolves
// its *playhead* step itself (NavigatorRouter::resolveFrameStep) — it snaps to the grid in Tempo mode
// rather than adding a delta, so it shares only tempoBeatTime() with this, not stepTime() as a whole.
inline double stepTime(const OverlayState &s) {
    switch (s.overlay) {
    case ScriptingOverlay::Frame:
        return 1.0 / static_cast<double>(s.frameFps);
    case ScriptingOverlay::Tempo:
        return tempoBeatTime(s);
    }
    return 1.0 / static_cast<double>(s.frameFps); // unreachable (enum is exhaustive); matches Frame default
}

void to_json(nlohmann::json &j, const OverlayState &s);
void from_json(const nlohmann::json &j, OverlayState &s);

} // namespace ofs
