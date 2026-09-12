// Source-dot decimation. The rule that matters for editing is that a bucket reports its position
// extremes, not an arbitrary member: two actions one grid slot apart at a stroke reversal differ in
// position, so they must stay two dots even when they collapse into one bucket. A single dot there
// hides the fact that an edit reached only one of the pair, and — because the hit-test enumerates the
// same set — leaves the other unclickable.

#include "Core/ScriptAxisAction.h"
#include "UI/DotDecimation.h"

#include <doctest/doctest.h>

#include <vector>

using ofs::ScriptAxisAction;
using ofs::ui::bucketScanEnd;
using ofs::ui::bucketScanStart;
using ofs::ui::dotBucketDuration;
using ofs::ui::forEachBucketDot;

namespace {

constexpr float kDotRadius = 8.1f; // the ≈8 px dot at the 18 px default font

// Every dot the decimator emits for `actions`, in emission order.
std::vector<ScriptAxisAction> dots(const std::vector<ScriptAxisAction> &actions, double bucketDuration) {
    std::vector<ScriptAxisAction> out;
    forEachBucketDot(
        actions.begin(), actions.end(), bucketDuration, [](const ScriptAxisAction &) { return true; },
        [&](const ScriptAxisAction &a) { out.push_back(a); });
    return out;
}

} // namespace

TEST_CASE("A bucket reports its position extremes, so a reversal pair stays two dots") {
    // Both inside one 0.1 s bucket, straddling a reversal.
    const std::vector<ScriptAxisAction> actions{{1.000, 20}, {1.001, 80}};
    const auto out = dots(actions, 0.1);

    REQUIRE(out.size() == 2);
    CHECK(out[0].pos == 20);
    CHECK(out[1].pos == 80);
}

TEST_CASE("A position-flat cluster still collapses to one dot") {
    // Nothing to explain here — no kink is possible — so the decimation stays as aggressive as before.
    const std::vector<ScriptAxisAction> actions{{1.000, 50}, {1.001, 50}, {1.002, 50}, {1.003, 50}};
    CHECK(dots(actions, 0.1).size() == 1);
}

TEST_CASE("Bucket dots come out in time order whichever extreme is earlier") {
    const std::vector<ScriptAxisAction> descending{{1.000, 90}, {1.001, 40}, {1.002, 10}};
    const auto out = dots(descending, 0.1);
    REQUIRE(out.size() == 2);
    CHECK(out[0].at == doctest::Approx(1.000)); // the high extreme happens to be first here
    CHECK(out[1].at == doctest::Approx(1.002));
    CHECK(out[0].at < out[1].at);
}

TEST_CASE("A bucket never emits the same action twice") {
    const std::vector<ScriptAxisAction> lone{{1.0, 50}};
    CHECK(dots(lone, 0.1).size() == 1);
}

TEST_CASE("Each bucket is decimated independently") {
    const std::vector<ScriptAxisAction> actions{{0.00, 10}, {0.01, 90}, {5.00, 20}, {5.01, 80}};
    CHECK(dots(actions, 0.1).size() == 4); // two buckets, two extremes each
}

TEST_CASE("A suppressed action cannot win a bucket and hide a shown neighbour") {
    // Hidden-region points are filtered before bucketing. If they were filtered after, the hidden pair
    // would take both extremes and the visible point would vanish with them.
    const std::vector<ScriptAxisAction> actions{{1.000, 0}, {1.001, 50}, {1.002, 100}};
    std::vector<ScriptAxisAction> out;
    forEachBucketDot(
        actions.begin(), actions.end(), 0.1,
        [](const ScriptAxisAction &a) { return a.pos == 50; }, // only the middle one is shown
        [&](const ScriptAxisAction &a) { out.push_back(a); });

    REQUIRE(out.size() == 1);
    CHECK(out[0].pos == 50);
}

TEST_CASE("Full zoom-in resolves adjacent grid slots into separate dots") {
    // The zoom floor exists to guarantee this: at the tightest window a bucket must be strictly narrower
    // than one millisecond, or two authored points one slot apart can never be told apart however far the
    // user zooms. Exactly 1 ms does not do — the boundary division is inexact and lands them together —
    // so the floor has to reach the ladder's 0.5 ms step. 400 px is a narrow Lanes row, the worst case
    // that still has to work.
    constexpr double kZoomFloor = 0.01;
    CHECK(dotBucketDuration(kDotRadius, kZoomFloor, 400.0f) < 0.001);
    CHECK(dotBucketDuration(kDotRadius, kZoomFloor, 1600.0f) < 0.001);

    const std::vector<ScriptAxisAction> pair{{1.000, 20}, {1.001, 20}}; // flat, so only the bucket splits them
    CHECK(dots(pair, dotBucketDuration(kDotRadius, kZoomFloor, 400.0f)).size() == 2);
}

TEST_CASE("The bucket ladder is stable across a zoom range and anchored to absolute time") {
    // Within one ladder step the bucket size must not budge, so the drawn set cannot shimmer as the
    // smooth-zoom animation runs.
    const double a = dotBucketDuration(kDotRadius, 10.0, 1600.0f);
    CHECK(dotBucketDuration(kDotRadius, 10.5, 1600.0f) == doctest::Approx(a));

    // Doubling the window at most doubles the bucket — neighbouring steps stay a clean 2x apart, which is
    // what makes the visible set a superset/subset of its neighbour rather than a different cluster member.
    const double wide = dotBucketDuration(kDotRadius, 20.0, 1600.0f);
    CHECK(wide == doctest::Approx(a * 2.0));
}

TEST_CASE("The scan range covers whole buckets so panning cannot change a bucket's extremes") {
    constexpr double kBucket = 0.1;
    // A window starting mid-bucket must scan back to that bucket's start, else the bucket's extremes
    // would be picked from just the part in view and would shift as the user pans.
    CHECK(bucketScanStart(1.07, kBucket) == doctest::Approx(1.0));
    CHECK(bucketScanEnd(1.07, kBucket) == doctest::Approx(1.1));
    CHECK(bucketScanStart(1.0, kBucket) == doctest::Approx(1.0));

    const std::vector<ScriptAxisAction> actions{{1.00, 10}, {1.05, 90}};
    // Scanning the whole bucket yields the same two dots wherever inside it the window begins.
    for (const double viewStart : {1.00, 1.02, 1.04, 1.06}) {
        auto begin = actions.begin();
        while (begin != actions.end() && begin->at < bucketScanStart(viewStart, kBucket))
            ++begin;
        std::vector<ScriptAxisAction> out;
        forEachBucketDot(
            begin, actions.end(), kBucket, [](const ScriptAxisAction &) { return true; },
            [&](const ScriptAxisAction &a) { out.push_back(a); });
        REQUIRE(out.size() == 2);
        CHECK(out[0].pos == 10);
        CHECK(out[1].pos == 90);
    }
}
