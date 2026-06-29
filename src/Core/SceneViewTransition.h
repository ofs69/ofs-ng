#pragma once

#include "Core/SceneView.h"

#include <algorithm>

namespace ofs {

// Drives a timed, eased glide between two SceneViews. The split it embodies: the owner (ProjectManager)
// decides *which* view is active for the cursor — that's functionality; this decides *how* the active
// view eases from the old one to the new — that's animation. It is pure and headless (no project, no
// event queue), so the easing/clamping is unit-tested without the service.
class SceneViewTransition {
  public:
    explicit SceneViewTransition(float seconds = 0.25f) : seconds_(seconds) {}

    // Begin gliding from `from` to `to`. Re-callable mid-glide to retarget (pass the current view as
    // `from` so it continues without a pop).
    void start(const SceneView &from, const SceneView &to) {
        from_ = from;
        to_ = to;
        t_ = 0.0f;
    }

    // Jump straight to `to`, no glide (initial load / reset).
    void snap(const SceneView &to) {
        from_ = to_ = to;
        t_ = 1.0f;
    }

    // Drop any in-flight glide, e.g. when a user edit takes over the scene view.
    void cancel() { t_ = 1.0f; }

    [[nodiscard]] bool active() const { return t_ < 1.0f; }

    // Advance the clock by `dt` and return the eased current view. Call only while active().
    SceneView advance(float dt) {
        t_ = seconds_ > 0.0f ? std::min(1.0f, t_ + dt / seconds_) : 1.0f;
        const float e = t_ * t_ * (3.0f - 2.0f * t_); // smoothstep
        return lerp(from_, to_, e);
    }

    [[nodiscard]] const SceneView &target() const { return to_; }

  private:
    SceneView from_;
    SceneView to_;
    float t_ = 1.0f; // 1 = settled
    float seconds_;
};

} // namespace ofs
