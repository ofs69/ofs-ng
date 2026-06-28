#pragma once

#include "imgui.h"
#include <memory>

namespace ofs {

class FogShader;

// Draws the welcome screen's animated fog backdrop with a custom GLSL shader. Pure renderer: owns the
// shader program, the time-driven phase counter, and the per-frame callback payload; injects a raw-GL
// draw into the ImGui draw stream via ImDrawList::AddCallback — the same pattern as WaveformRenderer.
// The fog tint is derived from the active theme on the main thread and passed in, so light and dark
// themes both work and the deferred GL callback never touches the theme.
class FogBackground {
  public:
    FogBackground();
    ~FogBackground();

    FogBackground(const FogBackground &) = delete;
    FogBackground &operator=(const FogBackground &) = delete;

    // Fills [pos, pos+size] with one frame of fog and advances the animation by one step. `tint` is the
    // fog color (rgb) and the alpha ceiling a full billow reaches (a); `center` is the central vignette
    // tint (rgb) and its peak alpha (a). Call before the window's content.
    void draw(ImDrawList *drawList, const ImVec2 &pos, const ImVec2 &size, const ImVec4 &tint, const ImVec4 &center);

  private:
    // Lives as a member (not a stack local) because ImGui defers the callback to end-of-frame: the
    // pointer handed to AddCallback must stay valid until the draw list is rendered.
    struct CallbackData {
        FogShader *shader = nullptr;
        float phase = 0.f;
        float aspect = 1.f;
        float color[4] = {1.f, 1.f, 1.f, 1.f};
        float center[4] = {0.f, 0.f, 0.f, 0.f};
    };
    static void glCallback(const ImDrawList *parentList, const ImDrawCmd *cmd);

    std::unique_ptr<FogShader> shader_;
    CallbackData cb_{};
    float phase_ = 0.f; // time-driven: advanced by wall-clock seconds in draw(), frame-rate independent
};

} // namespace ofs
