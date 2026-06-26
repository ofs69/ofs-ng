#pragma once

namespace ofs {

// Phases of an audio-waveform extraction, reported by the worker on WaveformProgressEvent and translated
// by WaveformService into the footer task indicator. Probing/Extracting are the in-flight states;
// Done/Failed/Cancelled are terminal. A cache hit never enters Probing — it goes straight to ready.
enum class WaveformPhase { Idle, Probing, Extracting, Done, Failed, Cancelled };

} // namespace ofs
