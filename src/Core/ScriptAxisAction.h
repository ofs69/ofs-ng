#pragma once

#include <algorithm>

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
} // namespace ofs
