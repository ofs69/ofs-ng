#include <doctest/doctest.h>

#include "Util/FaultThrottle.h"

#include <chrono>

using ofs::util::FaultThrottle;
using namespace std::chrono_literals;

// A fixed, nonzero base so the injected times are deterministic (FaultThrottle never reads the real
// clock here). It must be past the epoch: FaultThrottle treats a zero time_since_epoch as the
// never-emitted sentinel, which the real steady_clock never returns.
static const std::chrono::steady_clock::time_point kBase = std::chrono::steady_clock::time_point{} + 100s;

TEST_CASE("first fault for a key emits with zero suppressed") {
    FaultThrottle ft(3s);
    auto r = ft.onFault("node", kBase + 1s);
    REQUIRE(r.has_value());
    CHECK(*r == 0);
}

TEST_CASE("faults inside the window are suppressed and counted") {
    FaultThrottle ft(3s);
    CHECK(ft.onFault("node", kBase) == 0); // first emit
    CHECK_FALSE(ft.onFault("node", kBase + 500ms).has_value());
    CHECK_FALSE(ft.onFault("node", kBase + 1s).has_value());
    CHECK_FALSE(ft.onFault("node", kBase + 2999ms).has_value());

    // Past the window: emit again, carrying the three suppressed faults, then reset.
    auto r = ft.onFault("node", kBase + 3s);
    REQUIRE(r.has_value());
    CHECK(*r == 3);

    // A fault right after the emit is inside the new window again.
    CHECK_FALSE(ft.onFault("node", kBase + 3s + 1s).has_value());
}

TEST_CASE("window boundary is exclusive on the low side, inclusive at the edge") {
    FaultThrottle ft(3s);
    CHECK(ft.onFault("k", kBase) == 0);
    // now - lastEmit == window is NOT < window, so exactly at the edge it emits.
    auto r = ft.onFault("k", kBase + 3s);
    REQUIRE(r.has_value());
    CHECK(*r == 0); // nothing was suppressed between the two emits
}

TEST_CASE("keys are throttled independently") {
    FaultThrottle ft(3s);
    CHECK(ft.onFault("a", kBase) == 0);
    CHECK(ft.onFault("b", kBase) == 0); // different key emits on its own
    CHECK_FALSE(ft.onFault("a", kBase + 1s).has_value());
    CHECK_FALSE(ft.onFault("b", kBase + 1s).has_value());

    auto ra = ft.onFault("a", kBase + 4s);
    auto rb = ft.onFault("b", kBase + 4s);
    REQUIRE(ra.has_value());
    REQUIRE(rb.has_value());
    CHECK(*ra == 1);
    CHECK(*rb == 1);
}

TEST_CASE("a zero-length window emits every fault") {
    FaultThrottle ft(0s);
    CHECK(ft.onFault("k", kBase) == 0);
    CHECK(ft.onFault("k", kBase) == 0); // now - lastEmit (0) is not < 0, so it emits
    CHECK(ft.onFault("k", kBase + 1s) == 0);
}
