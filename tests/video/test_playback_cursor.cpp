#include "Video/PlaybackCursor.h"
#include <cmath>
#include <doctest/doctest.h>

using ofs::PlaybackCursor;

namespace {
// onPositionSample takes the sample's wall time explicitly (nanoseconds), so the phase/loop logic is
// fully deterministic in a test — no real clock involved. advance()'s *playing* branch reads the wall
// clock, but its *paused* branch eases toward framePos alone, so the pause-settle is deterministic too.
constexpr uint64_t kNs = 1'000'000'000ull; // 1 second in ns
} // namespace

TEST_CASE("PlaybackCursor: a forward sample re-anchors the decoder position only") {
    PlaybackCursor c;
    c.onPositionSample(3.0, 1 * kNs, /*paused=*/false, /*duration=*/10.0);
    CHECK(c.getActualPosition() == doctest::Approx(3.0));
    // A forward sample moves the phase anchor, not the smoothed output — only advance()/wrap/reposition
    // move logicalPos. (Documents the split between the anchor and the rendered cursor.)
    CHECK(c.getLogicalPosition() == doctest::Approx(0.0));
}

TEST_CASE("PlaybackCursor: a loop-wrap while playing repositions cursor and anchor immediately") {
    PlaybackCursor c;
    c.onPositionSample(9.5, 1 * kNs, false, 10.0);
    // A large backward discontinuity (> kLoopJumpThreshold) is an end→start wrap, not a smoothing snap.
    c.onPositionSample(0.05, 2 * kNs, false, 10.0);
    CHECK(c.getActualPosition() == doctest::Approx(0.05));
    CHECK(c.getLogicalPosition() == doctest::Approx(0.05));
}

TEST_CASE("PlaybackCursor: a backward jump while paused is ignored") {
    PlaybackCursor c;
    c.onPositionSample(5.0, 1 * kNs, false, 10.0);
    // The wrap reposition is gated on !paused — a paused hold must never be moved by an arriving sample.
    c.onPositionSample(0.05, 2 * kNs, /*paused=*/true, 10.0);
    CHECK(c.getActualPosition() == doctest::Approx(5.0));
}

TEST_CASE("PlaybackCursor: a duplicate or sub-threshold-backward sample leaves the anchor pinned") {
    PlaybackCursor c;
    c.onPositionSample(5.0, 1 * kNs, false, 10.0);
    c.onPositionSample(5.0, 2 * kNs, false, 10.0); // exact duplicate (Video feeds the frame twice)
    CHECK(c.getActualPosition() == doctest::Approx(5.0));
    // A small backward wobble (< kLoopJumpThreshold) is neither a wrap nor an advance: ignored, not snapped.
    c.onPositionSample(4.95, 3 * kNs, false, 10.0);
    CHECK(c.getActualPosition() == doctest::Approx(5.0));
}

TEST_CASE("PlaybackCursor: a wrap clamps the smoothed cursor into [0, duration]") {
    PlaybackCursor c;
    c.onPositionSample(5.0, 1 * kNs, false, 10.0);
    c.onPositionSample(-0.5, 2 * kNs, false, 10.0); // wrap below zero ⇒ logical clamps to 0
    CHECK(c.getLogicalPosition() == doctest::Approx(0.0));
}

TEST_CASE("PlaybackCursor: reposition clamps the target and pins the anchor to it") {
    PlaybackCursor c;
    c.reposition(15.0, 10.0); // past the end
    CHECK(c.getLogicalPosition() == doctest::Approx(10.0));
    CHECK(c.getActualPosition() == doctest::Approx(10.0));
    c.reposition(-3.0, 10.0); // before the start
    CHECK(c.getLogicalPosition() == doctest::Approx(0.0));
    CHECK(c.getActualPosition() == doctest::Approx(0.0));
}

TEST_CASE("PlaybackCursor: a paused advance without an armed settle is a no-op") {
    PlaybackCursor c;
    c.onPositionSample(5.0, 1 * kNs, false, 10.0); // anchor at 5, logical still 0
    c.advance(0.016f, /*paused=*/true, 10.0);      // no beginPauseSettle ⇒ early return
    CHECK(c.getLogicalPosition() == doctest::Approx(0.0));
}

TEST_CASE("PlaybackCursor: an armed pause-settle eases the cursor monotonically onto the stop frame") {
    PlaybackCursor c;
    c.onPositionSample(5.0, 1 * kNs, false, 10.0); // anchor (stop frame) at 5, logical at 0
    c.beginPauseSettle();

    double prev = c.getLogicalPosition();
    for (int i = 0; i < 200; ++i) {
        c.advance(0.05f, /*paused=*/true, 10.0);
        const double now = c.getLogicalPosition();
        CHECK(now >= prev - 1e-9); // monotonic toward the target; the paused path may ease but never jerk back
        CHECK(now <= 5.0 + 1e-9);  // never overshoots the stop frame from below
        prev = now;
    }
    CHECK(c.getLogicalPosition() == doctest::Approx(5.0).epsilon(0.001)); // converges onto framePos
}
