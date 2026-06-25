#pragma once

#include "imgui.h"
#include <bitset>
#include <cstdint>

namespace ofs {

struct SimulatorState {
    ImVec2 p1 = {600.f, 300.f};
    ImVec2 p2 = {600.f, 700.f};
    int32_t extraLinesCount = 0;
    bool lockedPosition = false;
    bool use3dSimulator = false;
    ImVec2 sim3dPos = {-1.f, -1.f}; // negative sentinel → centered on first render
    float sim3dSize = 300.f;
    // Feature toggles (behavior, not appearance — moved here out of the theme).
    bool enableIndicators = true;
    bool enablePosition = false;
    bool enableHeightLines = true;
    // 3D mapping ranges: a 0–100 axis value maps symmetrically across ±range.
    // Linear ranges are in model-space units, rotation ranges in degrees.
    // Defaults reproduce the original hardcoded mapping (Godot's Simulator3D.cs).
    float swayRange = 1.0f;   // L2 position
    float strokeRange = 1.0f; // L0 position
    float surgeRange = 1.0f;  // L1 position
    float pitchRange = 30.f;  // R2 rotation (degrees)
    float rollRange = 30.f;   // R1 rotation (degrees)
    float twistRange = 180.f; // R0 rotation (degrees)
    // 3D overlay data labels: which DOF gizmos/readouts are drawn, one bit per axis indexed by
    // StandardAxis (L0..R2 == 0..5). Default: all six on. A label only shows if its bit is set *and*
    // the axis exists (AxisState::exists()).
    static constexpr size_t kSim3dDofCount = 6;
    std::bitset<kSim3dDofCount> labels3dMask{0x3F};
    // Rotation DOFs (R0/R1/R2) read out the mapped deflection in degrees when true, percent when false.
    // Linear DOFs (L0/L1/L2) are always percent — degrees have no meaning for a translation.
    bool labels3dInDegrees = true;
};

} // namespace ofs
