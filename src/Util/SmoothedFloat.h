#pragma once

#include <algorithm>
#include <cmath>

namespace ofs {

// A scalar that eases toward a target each frame — the shared smoothing idiom behind the video zoom
// glide, the BindingSystem analog-stick low-pass, and the timeline axis-emphasis highlight. It isolates
// the "ease a value toward its target over time" rule from the owning code that sets the target, reads
// the displayed value, and applies per-step side effects, so rendering and animation stay separate. The
// owner sets a target and calls advance(dt, …) each frame; the value chases it.
//
// Two response shapes, selected by the damping ratio `zeta` passed to advance():
//   • zeta >= 1 (default): a frame-rate-correct exponential low-pass (alpha = 1 - exp(-dt/tau)).
//     Monotonic — it never crosses the target. This is the original behavior; callers that omit zeta
//     (zoom, analog stick) get it unchanged.
//   • zeta < 1: an under-damped spring that overshoots the target and settles back ("bounce back",
//     the easeOutBack feel). Re-targetable mid-flight — a new setTarget just re-aims the spring, no
//     clock to restart — which is what makes it right for a retargetable UI highlight.
class SmoothedFloat {
  public:
    SmoothedFloat() = default;
    explicit SmoothedFloat(float v) : value_(v), target_(v) {}

    void setTarget(float t) { target_ = t; }
    [[nodiscard]] float target() const { return target_; }
    [[nodiscard]] float value() const { return value_; }
    [[nodiscard]] bool settled() const { return value_ == target_ && velocity_ == 0.0f; }

    // Jump value and target to v with no animation (a restore/reset, not an eased change).
    void snap(float v) {
        value_ = target_ = v;
        velocity_ = 0.0f;
    }

    // Step the value toward the target by `dt` seconds. `tau` is the response time constant in seconds
    // (larger = slower, smoother). `zeta` is the damping ratio: >= 1 gives the monotonic exponential
    // low-pass, < 1 an over-shooting spring (smaller = more overshoot). Snaps to the target once within
    // `epsilon` (and at rest) so it settles exactly instead of crawling. Returns the new value.
    float advance(float dt, float tau, float zeta = 1.0f, float epsilon = 1e-4f) {
        if (value_ == target_ && velocity_ == 0.0f)
            return value_;
        tau = std::max(tau, 1e-4f);

        if (zeta >= 1.0f) {
            // Critically/over-damped: first-order exponential low-pass, monotonic (no overshoot).
            value_ += (target_ - value_) * (1.0f - std::exp(-dt / tau));
            velocity_ = 0.0f;
        } else {
            // Under-damped spring (overshoots, then settles). Semi-implicit (symplectic) Euler, which is
            // only conditionally stable, so sub-step when omega*dt is large (a very short response time)
            // to keep the integration from blowing up.
            const float omega = 1.0f / tau;
            const float k = omega * omega;       // stiffness
            const float c = 2.0f * zeta * omega; // damping
            const int steps = std::max(1, static_cast<int>(std::ceil(omega * dt * 2.0f)));
            const float h = dt / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                velocity_ += (-(value_ - target_) * k - velocity_ * c) * h;
                value_ += velocity_ * h;
            }
        }

        if (std::abs(target_ - value_) < epsilon && std::abs(velocity_) < epsilon) {
            value_ = target_;
            velocity_ = 0.0f;
        }
        return value_;
    }

  private:
    float value_ = 0.0f;
    float target_ = 0.0f;
    float velocity_ = 0.0f; // spring velocity; unused (stays 0) on the exponential-low-pass path
};

} // namespace ofs
