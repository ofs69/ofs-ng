#pragma once

#include "Core/VectorSet.h"

#include <algorithm>
#include <cmath>

namespace ofs {
struct ScriptAxisAction {
    double at = 0.0; // time in seconds
    int pos = 0;     // position 0-100

    ScriptAxisAction() = default;

    ScriptAxisAction(double at, int pos) : at(at), pos(pos) {}

    bool operator<(const ScriptAxisAction &other) const { return at < other.at; }

    bool operator==(const ScriptAxisAction &other) const { return at == other.at && pos == other.pos; }
};

// Action invariants: the timeline starts at t=0, and positions are 0-100. Enforce these O(1) at every
// *input* boundary where an externally-sourced value becomes an action — UI add/move, plugin commits,
// file and funscript import. Internal edits that only relocate already-valid data (axis copies, undo
// restore, paste from the clipboard) don't need to re-clamp, so this never runs as a whole-set sweep.
[[nodiscard]] inline ScriptAxisAction clampedAction(double at, int pos) {
    return {std::max(at, 0.0), std::clamp(pos, 0, 100)};
}

// Whole milliseconds — the finest time a funscript stores (secondsToMs in Format/Funscript.cpp).
inline constexpr double kActionTimeGridHz = 1000.0;
// One grid slot, in seconds: the closest two source actions can legally sit.
inline constexpr double kActionTimeStep = 1.0 / kActionTimeGridHz;

// Snap an authored time onto the export grid. Two source actions inside one grid slot are not
// representable: export rounds both to the same `at`, and a reload merges them (actions are keyed on
// `at` alone). Worse, while the pair exists it shares a single timeline dot, so an edit reaches only one
// of them — the other stays put and kinks the line at a place with no dot to blame. Snapping at the
// input boundary, rather than letting export round later, is what keeps authored points at least one
// slot apart and the on-screen set equal to what a save/reload round-trip yields.
//
// This governs *source* actions only. Processing-node output and AxisState::resolved are computed data,
// not authored data, and keep full double precision.
[[nodiscard]] inline double snapAuthoredTime(double at) {
    return std::max(std::round(at * kActionTimeGridHz) / kActionTimeGridHz, 0.0);
}

// clampedAction for a time the user authored — see snapAuthoredTime.
[[nodiscard]] inline ScriptAxisAction authoredAction(double at, int pos) {
    return {snapAuthoredTime(at), std::clamp(pos, 0, 100)};
}

// The action nearest `time`, or nullptr on an empty set. Resolves a no-selection edit/nudge gesture to
// the single action the playhead is closest to — the edit and intent-router paths must agree on it.
[[nodiscard]] inline const ScriptAxisAction *closestActionByTime(const VectorSet<ScriptAxisAction> &actions,
                                                                 double time) {
    auto it = actions.closest(ScriptAxisAction{time, 0}, &ScriptAxisAction::at);
    return it == actions.end() ? nullptr : &*it;
}
} // namespace ofs
