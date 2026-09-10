#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"

#include <iterator>

namespace ofs {

struct ScriptProject;

class ScriptStatisticsWindow {
  public:
    ScriptStatisticsWindow() = default;
    void render(const ScriptProject &project, bool &open) const;
};

namespace ui {

// Tolerance (seconds) for reading the playhead as sitting *on* an action. Mirrors the navigator's step
// slop, so an action the caret stepped onto is on-point here too even though the seek round-trips
// through the player.
inline constexpr double kOnActionEpsilon = 0.001;

// The actions the Statistics window reports on for one playhead time. `from`/`to` are set as a pair or
// not at all — a lone endpoint bounds no stroke.
struct PlayheadStroke {
    const ScriptAxisAction *prev = nullptr; // last action at or before the playhead
    const ScriptAxisAction *from = nullptr; // stroke start
    const ScriptAxisAction *to = nullptr;   // stroke end
};

[[nodiscard]] inline PlayheadStroke strokeAtPlayhead(const VectorSet<ScriptAxisAction> &actions, double time) {
    PlayheadStroke stroke;
    auto after = actions.upperBound(ScriptAxisAction{time, 0});
    if (after != actions.begin())
        stroke.prev = &*std::prev(after);

    // An action under the caret *ends* the stroke it belongs to rather than starting the next one:
    // stepping onto a peak reports the stroke that reached it, which is the one just watched.
    auto to = actions.lowerBound(ScriptAxisAction{time - kOnActionEpsilon, 0});
    if (to != actions.end() && to != actions.begin()) {
        stroke.to = &*to;
        stroke.from = &*std::prev(to);
    }
    return stroke;
}

} // namespace ui

} // namespace ofs
