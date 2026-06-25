#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include "imgui.h"
#include <cstdint>
#include <vector>

namespace ofs {
class Heatmap {
  public:
    Heatmap();

    ~Heatmap();

    static void init();

    void draw(ImDrawList *drawList, const ImVec2 &min, const ImVec2 &max);

    void update(double totalDuration, const VectorSet<ScriptAxisAction> &actions) const;

    // Width of the speed texture / number of time bins spanning [0, totalDuration].
    static constexpr int kSpeedResolution = 2048;

    // Bins per-stroke speeds into a normalized [0,1] heatmap buffer of kSpeedResolution
    // samples (index 0 = t0 … last = totalDuration). Each occupied bin holds the mean of
    // the speeds (|Δpos|/Δt) of the strokes covering it, divided by maxSpeed and clamped
    // to [0,1]; unoccupied bins stay 0. Returns empty when totalDuration <= 0. Pure (no
    // GL) so the binning logic is unit-testable; update() uploads the result.
    static std::vector<float> computeSpeedBuffer(double totalDuration, const VectorSet<ScriptAxisAction> &actions,
                                                 float maxSpeed);

    static void rebuildColorLut();

    // Returns width*height RGBA8 pixels (left=t0, right=t1), always fully opaque. When
    // fade is true the on-screen black→transparent top-to-bottom fade is baked into the
    // RGB (top fades to black) so the export matches the UI; otherwise the gradient is
    // flat. Requires a current GL context and an up-to-date speed texture (call update()
    // first). Returns empty on FBO failure.
    std::vector<uint8_t> renderToBitmap(int16_t width, int16_t height, bool fade) const;

  private:
    uint32_t m_speedTexture = 0;
};
} // namespace ofs
