#pragma once

#include <string>

namespace ofs {

// Lifecycle of an intra-frame transcode, reported by the worker and mirrored into TranscodeState so
// the blocking progress modal can render the right stage. Ordered: a run advances Idle → Probing →
// Encoding → Verifying → Done, or lands in Failed / Cancelled.
enum class TranscodePhase { Idle, Probing, Encoding, Verifying, Done, Failed, Cancelled };

// Transient, main-thread mirror of the running transcode (if any). Updated from TranscodeProgress/
// Complete/Failed events; read by the progress modal. NOT serialized — a transcode never spans a
// save/load.
struct TranscodeState {
    bool active = false;
    TranscodePhase phase = TranscodePhase::Idle;
    double progress = 0.0;   // 0..1, seeded from the source duration
    double etaSeconds = 0.0; // estimated time remaining
    double speed = 0.0;      // encode speed multiple (ffmpeg "speed", e.g. 3.2 = 3.2x realtime)
    std::string sourcePath;  // UTF-8
    std::string outputPath;  // UTF-8
    std::string error;       // populated when phase == Failed
};

} // namespace ofs
