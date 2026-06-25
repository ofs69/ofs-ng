#pragma once

#include "Core/WaveformState.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ofs {

// A finished audio-waveform peak summary, posted from a JobSystem worker back to the main thread.
// `peaks` is a shared_ptr so the (potentially multi-MB) buffer rides the event queue without a copy;
// the WaveformService uploads it to a GL texture on the main thread. `mediaPath` is the UTF-8 source
// the peaks were extracted from — the handler drops the result if a newer media has since loaded.
struct WaveformReadyEvent {
    std::string mediaPath;
    uint32_t bucketCount = 0;     // number of min/max buckets; `peaks` holds 2*bucketCount floats
    double durationSeconds = 0.0; // total audio duration the buckets span
    std::shared_ptr<std::vector<float>> peaks;
};

// In-flight progress, pushed from the worker (coalesced) so the modal can show a bar. The first one
// (phase Probing) is also what flips ScriptProject::waveform.active true and raises the modal.
struct WaveformProgressEvent {
    std::string mediaPath;
    double progress = 0.0;    // 0..1 (0 while Probing / when the duration is unknown)
    double etaSeconds = -1.0; // estimated seconds remaining (< 0 when not yet known)
    WaveformPhase phase = WaveformPhase::Extracting;
};

// Extraction could not produce a waveform: `cancelled` true means the user aborted it; false means a
// genuine failure (no audio stream, ffmpeg missing). The handler clears the on-screen waveform for
// `mediaPath`; it never auto-retries until the media changes.
struct WaveformFailedEvent {
    std::string mediaPath;
    bool cancelled = false;
};

// User aborted the extraction from the progress modal. The service flips its cancel flag; the worker
// then kills ffmpeg and reports WaveformFailedEvent{cancelled=true}.
struct CancelWaveformEvent {};

} // namespace ofs
