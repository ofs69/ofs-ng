#pragma once

#include <algorithm>
#include <cstdint>

namespace ofs {

// Smooth playback cursor, driven as a virtual clock.
//
// Two quantities define the clock: *phase* (where the decoder actually is) and *frequency* (how fast
// media-time advances per wall-second). Phase comes from the frame samples mpv reports. Frequency comes
// from two sources fused: the nominal playback speed (a setpoint the user controls) and the measured
// advance (frequency-locked from the samples). Neither alone is right — the nominal lags reality through
// a speed change while the decoder applies it, yet the measured rate, learned from frames that arrive at
// the playback rate, converges far too slowly at low speed (a 0.1x frame lands only every ~0.3 s). So
// the cursor advances at effectiveRate() = min(measured, nominal):
//  - Slow-down: min clamps the stale-high measured rate down to the new setpoint at once, with no wait
//    for a sparse confirming sample — the logical cursor can't run ahead of the decoder.
//  - Speed-up: the nominal is high, so min doesn't bind; the (fast-arriving) samples drive the climb,
//    and raising to the nominal early — which would run the cursor ahead — is avoided.
//  - Decode falling behind the setpoint: measured < nominal, min = measured, so the cursor tracks the
//    real (slower) advance.
// Because effectiveRate() is never above the true rate for long, the playhead never runs away and waits.
//
// As a backstop, the cursor is never allowed to lead the last confirmed decoder frame by more than a
// couple of frame-steps. Across a sparse-sample gap (low speed, or the moment after a speed-up before
// mpv has applied it) the feed-forward would otherwise extrapolate far ahead at a stale rate and then
// stall — the classic "runs away, then waits". The lead bound caps that structurally, independent of how
// wrong the rate estimate is, so the cursor stays a hair ahead of the decoder and the next frame carries
// it forward.
//
// advance() is a frame-rate-independent follower: a feed-forward term at effectiveRate() (so a ramp is
// tracked with no steady-state lag) plus an exponential phase correction toward the extrapolated decoder
// position (so jitter and over/undershoot decay smoothly, never as a jump). The correction firms up with
// the size of the gap, so a transient rate error from a rapid speed change is reined in within a few
// frames rather than drifting, while tight tracking stays gentle and smooth. Pausing falls out for free:
// with the decoder fixed the target is just `framePos`, so an overshoot past the stop point eases back
// smoothly. Only two things reposition the cursor discontinuously — an explicit user seek and an
// end→start loop-wrap — both genuine repositions, not smoothing snaps.
struct PlaybackCursor {
    // Record the decoder position of a presented frame (the phase anchor). Video sources this from
    // render-update signals (signalTimeNs = the render-thread present time); audio-only playback
    // renders no frames and sources it from time-pos updates (signalTimeNs = wall clock). A large
    // backward jump is a loop-wrap and repositions immediately; otherwise the anchor moves only when
    // the decoder genuinely advanced to a new frame, and (while playing) that advance frequency-locks
    // the measured `rate` toward reality. `paused` gates the frequency lock — frame-steps aren't playback.
    void onPositionSample(double newFramePos, uint64_t signalTimeNs, bool paused, double duration);

    // Advance the smooth cursor every app frame. Called from MpvVideoPlayer::update().
    void advance(float dt, bool paused, double duration);

    // Explicit user reposition (seek): place the cursor at the target now. This is not a smoothing
    // snap — the user asked to be here, and the edit cursor (cursorPos) must reflect it immediately.
    // Also ends any pause-settle: once the user has repositioned, the cursor holds the requested time
    // exactly and must not drift toward mpv's frame-boundary correction.
    void reposition(double targetTime, double duration);

    // Arm the one-shot pause-settle: while paused the cursor eases to the true stop frame, undoing the
    // overshoot left by the async play→pause. Called only on a genuine play→pause transition — NOT for
    // a paused frame-step, where the cursor must stay on the requested time and ignore the frame
    // boundary mpv reports a moment later.
    void beginPauseSettle() { easeWhilePaused = true; }

    // Seed both the measured and nominal rate before any samples exist (media-sec per wall-sec; equals
    // the speed multiplier). Call on unpause to get the cursor moving immediately at the right speed.
    void setRate(double newRate) {
        rate = newRate;
        nominalRate = newRate;
    }

    // Record a running playback-speed change (the setpoint). The measured `rate` is left to the lock;
    // effectiveRate() = min(rate, nominalRate) then takes a slow-down effect immediately and lets a
    // speed-up be driven by the samples — see the class comment.
    void onSpeedCommand(double newRate) { nominalRate = newRate; }

    // Re-anchor the phase clock to `signalTimeNs` so a resume doesn't extrapolate against a stale
    // pre-pause arrival time (which would read as a huge elapsed and lurch the cursor forward). Call
    // on unpause.
    void resync(uint64_t signalTimeNs);

    // Sub-frame smooth cursor — what all UI should read.
    double getLogicalPosition() const { return logicalPos; }

    // Best known decoder position (the phase anchor); used as the seek-target reference.
    double getActualPosition() const { return framePos; }

  private:
    // Rate the cursor actually advances at: the measured estimate clamped to the commanded setpoint, so
    // a slow-down binds at once but a speed-up (and a decode-bound under-run) is governed by measurement.
    double effectiveRate() const { return std::min(rate, nominalRate); }

    double logicalPos = 0.0;       // smoothed output
    double framePos = 0.0;         // last decoder position (phase anchor position)
    double frameArrivalTime = 0.0; // wall time (s) the current framePos became current
    double rate = 0.0;             // measured media-sec per wall-sec; frequency-locked to reality
    double nominalRate = 1.0;      // commanded playback speed (setpoint) — the upper bound on effectiveRate
    double frameStep = 0.04;       // measured media advance per decoder frame; bounds the cursor's lead
    bool easeWhilePaused = false;  // one-shot: ease to framePos after a play→pause, cleared on reposition
};

} // namespace ofs
