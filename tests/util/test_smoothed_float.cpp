#include "Util/SmoothedFloat.h"
#include <doctest/doctest.h>

using ofs::SmoothedFloat;

TEST_CASE("SmoothedFloat eases toward its target and settles exactly") {
    SmoothedFloat s(0.0f);
    s.setTarget(1.0f);
    CHECK_FALSE(s.settled());

    const float a = s.advance(1.0f / 60.0f, 1.0f / 15.0f);
    CHECK(a > 0.0f);
    CHECK(a < 1.0f); // partway, not a jump
    CHECK(s.value() == doctest::Approx(a));

    // Enough steps drive it onto the target and it latches there (no endless crawl).
    for (int i = 0; i < 240; ++i)
        s.advance(1.0f / 60.0f, 1.0f / 15.0f);
    CHECK(s.settled());
    CHECK(s.value() == doctest::Approx(1.0f));
}

TEST_CASE("SmoothedFloat snap jumps value and target with no animation") {
    SmoothedFloat s(0.0f);
    s.setTarget(5.0f);
    s.snap(3.0f);
    CHECK(s.value() == doctest::Approx(3.0f));
    CHECK(s.target() == doctest::Approx(3.0f));
    CHECK(s.settled());
    CHECK(s.advance(1.0f, 0.1f) == doctest::Approx(3.0f)); // settled: advance is a no-op
}

TEST_CASE("SmoothedFloat is frame-rate independent over equal elapsed time") {
    // One big step vs many small steps covering the same wall-clock should land very close (the
    // exponential low-pass is the frame-rate-correct alpha = 1 - exp(-dt/tau) form).
    SmoothedFloat coarse(0.0f), fine(0.0f);
    coarse.setTarget(1.0f);
    fine.setTarget(1.0f);

    coarse.advance(0.1f, 1.0f / 15.0f);
    for (int i = 0; i < 10; ++i)
        fine.advance(0.01f, 1.0f / 15.0f);

    CHECK(coarse.value() == doctest::Approx(fine.value()).epsilon(0.01));
}
