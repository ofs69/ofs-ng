#pragma once

#include <algorithm>
#include <cmath>

namespace ofs {

// A scalar that eases toward a target with a frame-rate-correct exponential low-pass
// (alpha = 1 - exp(-dt/tau)) — the same smoothing idiom BindingSystem uses for analog sticks. It
// isolates the "ease a value toward its target over time" rule from the UI code that sets the target,
// reads the displayed value, and applies per-step side effects, so rendering and animation stay
// separate. The owner sets a target and calls advance(dt) each frame; the value chases it.
class SmoothedFloat {
  public:
    SmoothedFloat() = default;
    explicit SmoothedFloat(float v) : value_(v), target_(v) {}

    void setTarget(float t) { target_ = t; }
    [[nodiscard]] float target() const { return target_; }
    [[nodiscard]] float value() const { return value_; }
    [[nodiscard]] bool settled() const { return value_ == target_; }

    // Jump value and target to v with no animation (a restore/reset, not an eased change).
    void snap(float v) { value_ = target_ = v; }

    // Step the value toward the target by `dt` seconds. `tau` is the response time constant in seconds
    // (larger = slower, smoother). Snaps to the target once within `epsilon` so it settles exactly
    // instead of crawling. Returns the new value.
    float advance(float dt, float tau, float epsilon = 1e-4f) {
        if (value_ == target_)
            return value_;
        const float a = 1.0f - std::exp(-dt / std::max(tau, 1e-4f));
        value_ += (target_ - value_) * a;
        if (std::abs(target_ - value_) < epsilon)
            value_ = target_;
        return value_;
    }

  private:
    float value_ = 0.0f;
    float target_ = 0.0f;
};

} // namespace ofs
