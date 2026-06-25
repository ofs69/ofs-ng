#include "Core/BookmarkChapterState.h"
#include "Core/TimeSlot.h"
#include <doctest/doctest.h>
#include <limits>
#include <vector>

using ofs::Chapter;
using ofs::firstFreeSlot;
using ofs::TimeSlot;

namespace {
// Minimal interval used to exercise firstFreeSlot independently of any project type.
struct Span {
    double start;
    double end;
};
constexpr auto kInf = std::numeric_limits<double>::max();

TimeSlot slotOf(const std::vector<Span> &items, double requested, double upperBound = kInf) {
    return firstFreeSlot(
        items, requested, upperBound, [](const Span &s) { return s.start; }, [](const Span &s) { return s.end; });
}
} // namespace

TEST_CASE("firstFreeSlot: no items returns the whole range") {
    const auto slot = slotOf({}, 3.0, 20.0);
    CHECK(slot.start == doctest::Approx(3.0));
    CHECK(slot.end == doctest::Approx(20.0));
}

TEST_CASE("firstFreeSlot: request in a free gap is left untouched") {
    const auto slot = slotOf({{0.0, 5.0}, {10.0, 15.0}}, 6.0);
    CHECK(slot.start == doctest::Approx(6.0)); // 6 is free
    CHECK(slot.end == doctest::Approx(10.0));  // bounded by the next item
}

TEST_CASE("firstFreeSlot: request inside an item snaps forward to its end") {
    const auto slot = slotOf({{0.0, 10.0}, {12.0, 15.0}}, 5.0);
    CHECK(slot.start == doctest::Approx(10.0)); // snapped past the covering item
    CHECK(slot.end == doctest::Approx(12.0));   // next item bounds the gap
}

TEST_CASE("firstFreeSlot: snaps across a chain of overlapping items") {
    // Sorted by start; the first three overlap into one covered span [0,10].
    const auto slot = slotOf({{0.0, 5.0}, {3.0, 8.0}, {6.0, 10.0}, {12.0, 15.0}}, 2.0);
    CHECK(slot.start == doctest::Approx(10.0));
    CHECK(slot.end == doctest::Approx(12.0));
}

TEST_CASE("firstFreeSlot: request past the last item runs to the upper bound") {
    const auto slot = slotOf({{0.0, 5.0}}, 8.0, 20.0);
    CHECK(slot.start == doctest::Approx(8.0));
    CHECK(slot.end == doctest::Approx(20.0));
}

TEST_CASE("firstFreeSlot: request is clamped into [0, upperBound]") {
    CHECK(slotOf({}, -4.0, 20.0).start == doctest::Approx(0.0));
    CHECK(slotOf({}, 99.0, 20.0).start == doctest::Approx(20.0));
}

TEST_CASE("firstFreeSlot: an item ending exactly at the request leaves the request free") {
    // Half-open ranges: an item ending at t does not cover t.
    const auto slot = slotOf({{0.0, 5.0}, {8.0, 10.0}}, 5.0);
    CHECK(slot.start == doctest::Approx(5.0));
    CHECK(slot.end == doctest::Approx(8.0));
}

TEST_CASE("TimeSlot::fits reports whether the minimum length is available") {
    CHECK(TimeSlot{5.0, 5.4}.fits(0.5) == false);
    CHECK(TimeSlot{5.0, 5.5}.fits(0.5) == true);
    CHECK(TimeSlot{10.0, 9.0}.fits(0.5) == false); // empty / inverted slot
}

TEST_CASE("firstFreeSlot works on real Chapter intervals (overlaps allowed)") {
    std::vector<Chapter> chapters = {
        {.startTime = 0.0, .endTime = 10.0},
        {.startTime = 5.0, .endTime = 8.0}, // overlaps the first
        {.startTime = 20.0, .endTime = 25.0},
    };
    const auto slot = firstFreeSlot(
        chapters, 3.0, 30.0, [](const Chapter &c) { return c.startTime; }, [](const Chapter &c) { return c.endTime; });
    CHECK(slot.start == doctest::Approx(10.0)); // snapped past the merged [0,10] coverage
    CHECK(slot.end == doctest::Approx(20.0));   // bounded by the next chapter
}
