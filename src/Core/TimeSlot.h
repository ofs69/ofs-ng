#pragma once
#include <algorithm>

namespace ofs {

struct TimeSlot {
    double start = 0.0;
    double end = 0.0;

    [[nodiscard]] double length() const { return end - start; }
    [[nodiscard]] bool fits(double minLength) const { return end - start >= minLength; }
};

// First free time slot at or after `requested`, bounded by `upperBound`. Used to place a new
// chapter or region: if `requested` lands inside an existing item, the slot begins where that
// coverage ends (snap forward) rather than failing; the slot ends at the next item's start.
//
// `items` MUST be sorted by start time. Overlaps are allowed — the coverage end is advanced
// greedily, so chained/overlapping items collapse into a single covered span. One forward pass
// suffices: once an item starts beyond the (possibly advanced) slot start, every later item does
// too, so that item's start bounds the gap. `start`/`end` extract the time range from each item.
//
// The returned slot may be empty or shorter than desired (call `fits()` to check) when little or
// no room remains before the next item or `upperBound`.
template <class Range, class StartFn, class EndFn>
[[nodiscard]] TimeSlot firstFreeSlot(const Range &items, double requested, double upperBound, StartFn start,
                                     EndFn end) {
    double slotStart = std::clamp(requested, 0.0, upperBound);
    double slotEnd = upperBound;
    for (const auto &it : items) {
        if (start(it) > slotStart) {
            slotEnd = start(it);
            break;
        }
        if (end(it) > slotStart)
            slotStart = end(it);
    }
    return {slotStart, slotEnd};
}

} // namespace ofs
