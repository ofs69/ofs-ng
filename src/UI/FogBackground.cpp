#include "UI/FogBackground.h"
#include "Scenegraph/FogShader.h"
#include <algorithm>
#include <glad/gl.h>

namespace ofs {

FogBackground::FogBackground() : shader_(std::make_unique<FogShader>()) {}

FogBackground::~FogBackground() = default;

void FogBackground::glCallback(const ImDrawList * /*parentList*/, const ImDrawCmd *cmd) {
    const auto *d = static_cast<const CallbackData *>(cmd->UserCallbackData);

    const ImDrawData *dd = ImGui::GetDrawData();
    const float l = dd->DisplayPos.x;
    const float r = dd->DisplayPos.x + dd->DisplaySize.x;
    const float t = dd->DisplayPos.y;
    const float b = dd->DisplayPos.y + dd->DisplaySize.y;
    const float ortho[4][4] = {
        {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {(r + l) / (l - r), (t + b) / (b - t), 0.0f, 1.0f},
    };

    d->shader->use();
    d->shader->setProjMtx(&ortho[0][0]);
    d->shader->setPhase(d->phase);
    d->shader->setAspect(d->aspect);
    d->shader->setColor(d->color[0], d->color[1], d->color[2], d->color[3]);
    d->shader->setCenter(d->center[0], d->center[1], d->center[2], d->center[3]);
}

void FogBackground::draw(ImDrawList *drawList, const ImVec2 &pos, const ImVec2 &size, const ImVec4 &tint,
                         const ImVec4 &center) {
    if (size.x < 1.0f || size.y < 1.0f)
        return;

    // Time-driven: advance by wall-clock seconds so the drift runs at the same speed regardless of frame
    // rate (identical at 9 fps and 60+). DeltaTime is clamped so a hitch or a paused first frame can't
    // jump the field.
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    phase_ += dt * 0.6f; // ~0.6 phase units/sec — a gentle, unhurried drift

    cb_.shader = shader_.get();
    cb_.phase = phase_;
    cb_.aspect = size.x / size.y;
    cb_.color[0] = tint.x;
    cb_.color[1] = tint.y;
    cb_.color[2] = tint.z;
    cb_.color[3] = tint.w;
    cb_.center[0] = center.x;
    cb_.center[1] = center.y;
    cb_.center[2] = center.z;
    cb_.center[3] = center.w;

    drawList->AddCallback(&glCallback, &cb_);
    drawList->AddImage(0, pos, {pos.x + size.x, pos.y + size.y}); // quad geometry; shader set in callback
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
}

} // namespace ofs
