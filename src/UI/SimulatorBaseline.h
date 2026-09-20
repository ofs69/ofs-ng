#pragma once

#include "Scenegraph/Transform.h"
#include <glm/glm.hpp>

namespace ofs {

struct SimulatorBaselineGeometry {
    glm::vec3 tip{};
    glm::vec3 target{};
    float distance{};
    float percent{};
};

inline SimulatorBaselineGeometry simulatorBaselineGeometry(const sg::Transform &stroker, float strokeRange) {
    constexpr glm::vec3 kTipLocal{0.f, -0.875f, 0.f};
    const glm::vec3 tip = stroker.position + stroker.rotation * (stroker.scale * kTipLocal);
    const glm::vec3 target{0.f, -strokeRange + kTipLocal.y, 0.f};
    const float distance = glm::length(tip - target);
    const float fullTravel = strokeRange * 2.f;
    return {.tip = tip,
            .target = target,
            .distance = distance,
            .percent = fullTravel > 1e-4f ? distance / fullTravel * 100.f : 0.f};
}

} // namespace ofs
