#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace ofs {

inline constexpr float kMinSpeedLimit = 10.0f;
inline constexpr float kMaxSpeedLimit = 5000.0f;

// The fastest stroke the user's device can physically play, in position-units per second. Strokes above it
// are flagged on the timeline script line and the seek-bar heatmap, counted in Statistics, and reachable
// through the next/previous over-limit navigation commands. An app preference (it describes the device, not
// the script), off by default.
struct SpeedLimitSettings {
    bool enabled = false;
    float unitsPerSecond = 600.0f;
};

void to_json(nlohmann::json &j, const SpeedLimitSettings &s);
void from_json(const nlohmann::json &j, SpeedLimitSettings &s);

// A stroke with no duration has no defined speed and is never over the limit.
[[nodiscard]] inline bool exceedsSpeedLimit(const ScriptAxisAction &from, const ScriptAxisAction &to,
                                            float unitsPerSecond) {
    const double dt = to.at - from.at;
    return dt > 0.0 && std::abs(to.pos - from.pos) > static_cast<double>(unitsPerSecond) * dt;
}

[[nodiscard]] inline size_t countOverSpeedLimit(const VectorSet<ScriptAxisAction> &actions, float unitsPerSecond) {
    size_t count = 0;
    for (size_t i = 0; i + 1 < actions.size(); ++i)
        if (exceedsSpeedLimit(actions[i], actions[i + 1], unitsPerSecond))
            ++count;
    return count;
}

// Start time of the nearest over-limit stroke that begins strictly after (forward) or strictly before
// (backward) `time`, with a 1 ms slop so a stroke the playhead already sits on is stepped past rather than
// re-found. nullopt when there is none in that direction.
[[nodiscard]] inline std::optional<double> findOverSpeedLimitStroke(const VectorSet<ScriptAxisAction> &actions,
                                                                    double time, bool forward, float unitsPerSecond) {
    constexpr double kSlop = 0.001;
    if (actions.size() < 2)
        return std::nullopt;
    if (forward) {
        for (auto it = actions.upperBound(ScriptAxisAction{time + kSlop, 0}); it != actions.end(); ++it) {
            const auto next = std::next(it);
            if (next == actions.end())
                break;
            if (exceedsSpeedLimit(*it, *next, unitsPerSecond))
                return it->at;
        }
        return std::nullopt;
    }
    auto it = actions.lowerBound(ScriptAxisAction{time - kSlop, 0});
    while (it != actions.begin()) {
        --it;
        const auto next = std::next(it);
        if (next != actions.end() && exceedsSpeedLimit(*it, *next, unitsPerSecond))
            return it->at;
    }
    return std::nullopt;
}

} // namespace ofs
