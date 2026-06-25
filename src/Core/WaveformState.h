#pragma once

namespace ofs {

// Phases of an audio-waveform extraction, surfaced by the progress modal. Probing/Extracting are the
// in-flight states; Done/Failed/Cancelled are terminal. A cache hit never enters Probing — it goes
// straight to ready with no modal.
enum class WaveformPhase { Idle, Probing, Extracting, Done, Failed, Cancelled };

// Transient progress mirror of the running waveform extraction (if any). Driven by the waveform events,
// read by the blocking progress modal. NOT serialized — an extraction never spans a save/load.
struct WaveformState {
    bool active = false; // an extraction is in flight (Probing or Extracting)
    WaveformPhase phase = WaveformPhase::Idle;
    double progress = 0.0;    // 0..1 while Extracting (0 when the source duration is unknown)
    double etaSeconds = -1.0; // estimated seconds remaining (< 0 when not yet known)
};

} // namespace ofs
