#include "PlaybackCursor.h"
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>

namespace ofs {

namespace {
// Phase-correction time constant, adaptive in the cursor↔decoder gap. kPhaseTauMax is the gentle
// steady-state value (bridges a low-fps frame interval smoothly, ~41 ms at 24 fps); as the gap grows
// toward kPhaseGapRef the constant tightens to kPhaseTauMin, so a transient rate-estimate error — the
// kind a rapid speed change leaves behind — is pulled back within a few frames instead of drifting into
// a visible run-away. It is self-targeting: tight tracking keeps the gap small, so the firm regime only
// engages during an actual divergence.
constexpr double kPhaseTauMax = 0.1;
constexpr double kPhaseTauMin = 0.03;
constexpr double kPhaseGapRef = 0.06;
// A backward position jump beyond this is a loop-wrap or external reposition, not a phase correction.
constexpr double kLoopJumpThreshold = 0.1;

// Frequency-lock EMA weights. The lock is two-speed: kFastLock acquires a real speed change (the
// measurement jumps far from the estimate) within a couple of frames, so a rapid slow↔fast toggle
// settles before the next flip; kSlowLock gently smooths steady-state dt jitter. kRateJumpBand is the
// relative gap that counts as "a change" rather than jitter, so the fast weight never touches steady
// playback (no added jitter there) — it only speeds up convergence through a deliberate speed change.
constexpr double kFastLock = 0.6;
constexpr double kSlowLock = 0.05;
constexpr double kRateJumpBand = 0.35;
// Sample-spacing bounds (s) for a *usable* frequency measurement. Below the floor the dt is too small
// to divide by without amplifying noise; above the ceiling the gap is a hitch/seek/pause, not playback.
constexpr double kMinMeasureDt = 0.005;
constexpr double kMaxMeasureDt = 0.5;
// Absolute bounds on a believable measured rate. The decoder never sustains faster than the 2.0x speed
// clamp, so a measurement above kMaxPlausibleRate is an artifact — mpv flushing a queued frame on a
// speed change gives a tiny dWall and a measured rate of 4–6x, which would otherwise pull the locked
// estimate far above any real rate and make a later speed-up jump straight to full speed.
constexpr double kMinPlausibleRate = 0.02;
constexpr double kMaxPlausibleRate = 3.0;

// Lead bound: the logical cursor may never sit more than this many decoder frame-steps ahead of the
// last confirmed frame. Between frames the decoder advances ~one frame-step regardless of speed, so
// this caps blind extrapolation across a sparse-sample gap — the run-away when a speed-up command lifts
// the rate estimate before mpv has actually sped up — structurally, whatever the rate estimate does.
constexpr double kLeadFrames = 2.0;
constexpr double kMinLead = 0.04; // floor/ceiling on the bound so a bad frame-step estimate stays sane
constexpr double kMaxLead = 0.12;
// EMA weight for the frame-step estimate, and the largest single advance treated as one frame (bigger
// jumps are log-gap artifacts / catch-up bursts, not a real inter-frame step).
constexpr double kFrameStepLock = 0.1;
constexpr double kMaxFrameStep = 0.1;

double nowSeconds() {
    return static_cast<double>(SDL_GetTicksNS()) * 1e-9;
}
} // namespace

void PlaybackCursor::onPositionSample(double newFramePos, uint64_t signalTimeNs, bool paused, double duration) {
    double signalTime = static_cast<double>(signalTimeNs) * 1e-9;

    // Loop-wrap (or any large backward discontinuity) while playing: reposition immediately. Easing the
    // playhead backward across the whole timeline on every loop would be wrong — this is an allowed
    // jump, not a smoothing snap. Gated on !paused so a paused hold is never moved by a sample.
    if (!paused && newFramePos < framePos - kLoopJumpThreshold) {
        logicalPos = std::clamp(newFramePos, 0.0, duration);
        framePos = newFramePos;
        frameArrivalTime = signalTime;
        return;
    }

    // Re-anchor only when the decoder genuinely advanced. Video feeds this from both the render-update
    // and time-pos paths for the same frame; ignoring the duplicate keeps frameArrivalTime pinned to
    // when the frame first became current, so the extrapolation origin stays consistent.
    if (newFramePos > framePos) {
        // Track the inter-frame media step (used to bound how far the cursor may lead — see advance()).
        // Ignore advances bigger than one plausible frame: a catch-up burst or a logged-sample gap, not
        // a real step. Frame *step* stays clean even when the measured *rate* spikes (same dPos, tiny dt).
        double step = newFramePos - framePos;
        if (step < kMaxFrameStep)
            frameStep += (step - frameStep) * kFrameStepLock;

        // Frequency lock: drive the measured `rate` from the observed advance so the feed-forward tracks
        // mpv's true playback rate. effectiveRate() then clamps it to the commanded setpoint, so a stale
        // measurement through a speed change can't run the cursor away (see the header). Only during real
        // playback, and only for plausible samples (reject a hitch's dt, the across-pause gap, and
        // out-of-range glitches). The two-speed weight acquires a deliberate speed change fast and
        // smooths jitter slow.
        if (!paused && frameArrivalTime > 0.0 && rate > 0.0) {
            double dWall = signalTime - frameArrivalTime;
            if (dWall > kMinMeasureDt && dWall < kMaxMeasureDt) {
                double measured = (newFramePos - framePos) / dWall;
                if (measured > kMinPlausibleRate && measured < kMaxPlausibleRate) {
                    double err = measured - rate;
                    double w = std::abs(err) > kRateJumpBand * rate ? kFastLock : kSlowLock;
                    rate += err * w;
                }
            }
        }
        framePos = newFramePos;
        frameArrivalTime = signalTime;
    }
}

void PlaybackCursor::advance(float dt, bool paused, double duration) {
    // Paused with no pending settle: hold the requested position exactly. The cursor must not chase
    // mpv's frame-boundary correction, or a paused frame-step snaps back when the optimistic step
    // target drifts past a real frame PTS.
    if (paused && !easeWhilePaused)
        return;

    // Extrapolate the true media clock from the last frame anchor. Paused, the decoder is fixed, so the
    // target is simply framePos and the cursor eases toward it (the one-shot play→pause overshoot
    // correction); playing, it advances at effectiveRate() — the measured rate clamped to the setpoint.
    double rateNow = effectiveRate();
    double elapsed = nowSeconds() - frameArrivalTime;
    double target = paused ? framePos : framePos + elapsed * rateNow;

    double prev = logicalPos;
    if (!paused)
        logicalPos += static_cast<double>(dt) * rateNow; // feed-forward: track the ramp with no lag

    // Firm the correction as the cursor diverges from the extrapolated decoder position: gentle when
    // tracking is tight (smooth), tightening to kPhaseTauMin once the gap reaches kPhaseGapRef.
    double slack = std::clamp(std::abs(target - logicalPos) / kPhaseGapRef, 0.0, 1.0);
    double tau = kPhaseTauMax + (kPhaseTauMin - kPhaseTauMax) * slack;
    double alpha = 1.0 - std::exp(-static_cast<double>(dt) / tau); // frame-rate-independent gain
    logicalPos += (target - logicalPos) * alpha;

    // Never reverse the playhead during forward playback — a momentary phase overshoot must stall, not
    // jerk backward. (Paused, backward easing is allowed: that is how an overshoot past the stop point
    // is undone smoothly.)
    if (!paused && rateNow > 0.0)
        logicalPos = std::max(logicalPos, prev);

    // Bound the lead over the last confirmed decoder frame. Across a sparse-sample gap the feed-forward
    // would otherwise extrapolate far ahead at a stale rate — the run-away when a speed-up lifts the rate
    // before mpv has actually sped up. Capping the lead to a couple of frame-steps holds the cursor a
    // hair ahead of the decoder and lets the next frame carry it forward, instead of racing off and
    // waiting. Applied every frame, so framePos (monotonic in forward play) only ever raises the bound —
    // never a backward jerk. Playback-only: a paused settle must keep easing freely toward framePos.
    if (!paused) {
        double maxLead = std::clamp(frameStep * kLeadFrames, kMinLead, kMaxLead);
        logicalPos = std::min(logicalPos, framePos + maxLead);
    }

    logicalPos = std::clamp(logicalPos, 0.0, duration);
}

void PlaybackCursor::reposition(double targetTime, double duration) {
    logicalPos = std::clamp(targetTime, 0.0, duration);
    // Adopt the target as the phase anchor so a paused hold (target == framePos) doesn't pull back
    // toward the pre-seek position while mpv completes the async exact seek.
    framePos = logicalPos;
    frameArrivalTime = nowSeconds();
    // The user repositioned: hold this exact time, don't ease toward the frame boundary mpv reports next.
    easeWhilePaused = false;
}

void PlaybackCursor::resync(uint64_t signalTimeNs) {
    frameArrivalTime = static_cast<double>(signalTimeNs) * 1e-9;
}

} // namespace ofs
