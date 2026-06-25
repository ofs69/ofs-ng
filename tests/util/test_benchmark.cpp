#include "Util/Benchmark.h"
#include <doctest/doctest.h>

// The Stopwatch wraps SDL's performance counter. Wall-clock timing can't be asserted to an exact value
// without flakiness, so these pin only the invariants that must hold on any machine: elapsed time is
// non-negative, monotonically non-decreasing across reads, and restart() rewinds it toward zero. The
// busy-loop (not a sleep) keeps the test fast and CI-stable while still advancing the counter.

using ofs::util::Stopwatch;

namespace {
// A trivial spin that the optimizer can't elide, used to let the performance counter advance a little.
uint64_t burn() {
    uint64_t acc = 0;
    for (int i = 0; i < 2'000'000; ++i)
        acc += static_cast<uint64_t>(i) ^ acc;
    return acc;
}
} // namespace

TEST_CASE("Stopwatch: elapsed time is non-negative and non-decreasing") {
    const Stopwatch sw;
    const double a = sw.elapsedMs();
    CHECK(a >= 0.0);
    volatile uint64_t sink = burn();
    (void)sink;
    const double b = sw.elapsedMs();
    CHECK(b >= a); // a later read never reports less elapsed time
}

TEST_CASE("Stopwatch: us and ms readings agree") {
    const Stopwatch sw;
    volatile uint64_t sink = burn();
    (void)sink;
    const double ms = sw.elapsedMs();
    const double us = sw.elapsedUs();
    CHECK(us >= ms * 1000.0 * 0.5); // same clock, consistent scale (loose bound — us read strictly later)
    CHECK(ms >= 0.0);
}

TEST_CASE("Stopwatch: restart rewinds the elapsed time") {
    Stopwatch sw;
    volatile uint64_t sink = burn();
    (void)sink;
    const double before = sw.elapsedMs();
    sw.restart();
    const double after = sw.elapsedMs();
    CHECK(after <= before); // restart re-zeros, so the fresh reading can't exceed the accumulated one
    CHECK(after >= 0.0);
}
