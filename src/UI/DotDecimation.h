#pragma once

#include <cmath>
#include <cstdint>

// Source-dot decimation for the timeline's script line. The line itself is always stroked at full
// resolution; only the dots are thinned, because a dot has a fixed pixel radius and a dense stretch
// would otherwise draw thousands of overlapping circles per frame.
//
// Pure and free of ImGui/ScriptProject so the renderer and the unit tests share one implementation —
// the same reason TimelineLayout.h exists. The hit-test runs through the same enumeration as the
// renderer, so a clickable dot is always a drawn dot.

namespace ofs::ui {

// Time width of one decimation bucket: dots closer than ~2*dotRadiusPx are collapsed into one bucket.
// The bucket grid is anchored to absolute time 0, so panning never re-shuffles which dots in a cluster
// win — that is what keeps decimation steady during playback. To also stay steady across *zoom*, the
// bucket size is snapped to a fixed power-of-two ladder rather than varying continuously with
// visibleTime. Within a ladder step the drawn set is bit-for-bit identical; at a step boundary the
// buckets cleanly halve/double, so the visible set is a strict superset/subset of its neighbour — dots
// reveal or merge in place instead of a different cluster member popping in.
inline double dotBucketDuration(float dotRadiusPx, double visibleTime, float width) {
    double minBucket = static_cast<double>(dotRadiusPx) * 2.0 * visibleTime / static_cast<double>(width);
    if (!(minBucket > 0.0))
        return 1.0; // degenerate (zero-width view); any positive value keeps the bucket math finite
    constexpr double kLadderUnit = 0.001; // 1 ms — the absolute-time anchor the ladder is built on
    double step = std::ceil(std::log2(minBucket / kLadderUnit));
    return kLadderUnit * std::exp2(step);
}

// First/last time a scan must cover to enumerate every bucket that overlaps [startTime, endTime] in
// full. Backing the range out to bucket boundaries — rather than to the first action outside the
// window — is what makes a bucket's chosen representatives independent of where the view starts: a
// bucket clipped by the scan range would pick its extremes from whichever part happened to be in
// range, and they would change as the view pans.
inline double bucketScanStart(double startTime, double bucketDuration) {
    return std::floor(startTime / bucketDuration) * bucketDuration;
}
inline double bucketScanEnd(double endTime, double bucketDuration) {
    return (std::floor(endTime / bucketDuration) + 1.0) * bucketDuration;
}

// Emit the dots standing in for the actions in [first, last), in time order, at most two per bucket:
// the lowest- and highest-position action the bucket holds.
//
// Taking the extremes rather than the first is what makes a collapsed cluster legible. Two actions a
// fraction of a millisecond apart at a stroke reversal differ in *position*, so they render as two
// dots the user can see and click — instead of one dot that hides the fact an edit reached only half
// of the pair and left the other behind as a kink with nothing to grab. A cluster that is flat in
// position still collapses to a single dot, which is right: it produces no kink to explain.
//
// `visible(action)` filters before bucketing, so a suppressed action (one inside a region that hides
// source points) can never win a bucket and hide a shown neighbour.
//
// [first, last) must cover whole buckets — see bucketScanStart/bucketScanEnd.
template <class It, class VisibleFn, class OutFn>
void forEachBucketDot(It first, It last, double bucketDuration, VisibleFn visible, OutFn out) {
    It lo = last, hi = last; // lowest / highest position seen in the bucket being accumulated
    int64_t curBucket = 0;

    auto flush = [&] {
        if (lo == last)
            return;
        if (lo == hi) {
            out(*lo);
        } else if (hi->at < lo->at) {
            out(*hi);
            out(*lo);
        } else {
            out(*lo);
            out(*hi);
        }
    };

    for (It it = first; it != last; ++it) {
        if (!visible(*it))
            continue;
        auto bucket = static_cast<int64_t>(std::floor(it->at / bucketDuration));
        if (lo == last || bucket != curBucket) {
            flush();
            curBucket = bucket;
            lo = hi = it;
            continue;
        }
        if (it->pos < lo->pos)
            lo = it;
        if (it->pos > hi->pos)
            hi = it;
    }
    flush();
}

} // namespace ofs::ui
