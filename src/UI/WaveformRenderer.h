#pragma once

#include "imgui.h"
#include <cstdint>
#include <memory>

namespace ofs {

class WaveformShader;
class WaveformService;

// Draws the audio waveform behind the timeline curves with a custom GLSL shader. Pure renderer: owns
// the shader program and the per-frame callback payload, reads the service's GpuView (a passive data
// accessor), and injects a raw-GL draw into the ImGui draw stream via ImDrawList::AddCallback — the same
// pattern as Heatmap::draw / ScriptSimulator::glCallbackFunc.
class WaveformRenderer {
  public:
    explicit WaveformRenderer(const WaveformService &service);
    ~WaveformRenderer();

    WaveformRenderer(const WaveformRenderer &) = delete;
    WaveformRenderer &operator=(const WaveformRenderer &) = delete;

    // Fills `[pos, pos+size]` with the waveform for the visible window. No-op until the service has a
    // waveform ready. Call inside the timeline's curve-area rendering, before the curves are drawn.
    void drawBackground(ImDrawList *drawList, const ImVec2 &pos, const ImVec2 &size, double offsetTime,
                        double visibleTime);

  private:
    // Lives as a member (not a stack local) because ImGui defers the callback to end-of-frame: the
    // pointer handed to AddCallback must stay valid until the draw list is rendered. One waveform draw
    // per frame, so a single instance suffices.
    struct CallbackData {
        WaveformShader *shader = nullptr;
        uint32_t textureId = 0;
        float startBucket = 0.f;
        float endBucket = 0.f;
        float step = 1.f;            // ladder group size in buckets (power of two)
        float stride = 1.f;          // in-group sample stride
        float bucketsPerPixel = 1.f; // per-pixel footprint for the horizontal supersample
        float bucketCount = 0.f;
        float texW = 0.f;
        float texH = 0.f;
        float scale = 0.9f;
        float color[4] = {1.f, 1.f, 1.f, 1.f};
    };
    static void glCallback(const ImDrawList *parentList, const ImDrawCmd *cmd);

    const WaveformService &service_;
    std::unique_ptr<WaveformShader> shader_;
    CallbackData cb_{};
};

} // namespace ofs
