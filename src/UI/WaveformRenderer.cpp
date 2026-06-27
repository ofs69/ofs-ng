#include "UI/WaveformRenderer.h"
#include "Scenegraph/WaveformShader.h"
#include "Services/WaveformService.h"
#include "UI/Theme.h"
#include <algorithm>
#include <cmath>
#include <glad/gl.h>

namespace ofs {

WaveformRenderer::WaveformRenderer(const WaveformService &service)
    : service_(service), shader_(std::make_unique<WaveformShader>()) {}

WaveformRenderer::~WaveformRenderer() = default;

void WaveformRenderer::glCallback(const ImDrawList * /*parentList*/, const ImDrawCmd *cmd) {
    const auto *d = static_cast<const CallbackData *>(cmd->UserCallbackData);

    // Bind the peak texture to unit 1, not 0: ImGui rebinds the draw command's texture (id 0) to unit 0
    // right before the AddImage quad draws, which would clobber unit 0. The sampler reads unit 1.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, d->textureId);
    glActiveTexture(GL_TEXTURE0);

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
    d->shader->setPeaks(1);
    d->shader->setWindow(d->startBucket, d->endBucket, d->step, d->stride, d->bucketsPerPixel);
    d->shader->setTexDims(d->bucketCount, d->texW, d->texH);
    d->shader->setScale(d->scale);
    d->shader->setColor(d->color[0], d->color[1], d->color[2], d->color[3]);
}

void WaveformRenderer::drawBackground(ImDrawList *drawList, const ImVec2 &pos, const ImVec2 &size, double offsetTime,
                                      double visibleTime) {
    const auto v = service_.gpuView();
    if (!v.ready || v.durationSeconds <= 0.0 || v.bucketCount == 0)
        return;

    if (visibleTime <= 0.0 || size.x < 1.0f)
        return;

    const double toBucket = static_cast<double>(v.bucketCount) / v.durationSeconds;

    // Snap the per-fragment decimation to a power-of-two bucket ladder, anchored to absolute bucket 0.
    // The shader takes the min/max over each ladder group, so this is what keeps the envelope stable:
    // within a zoom step `step` is constant (identical bars) and at a step boundary groups cleanly
    // halve/double. `stride` covers a group within kWaveformMaxScan samples — and, like `step`, depends
    // only on the zoom level, so the sampled set never churns frame-to-frame.
    const double bucketsPerPixel = (visibleTime / static_cast<double>(size.x)) * toBucket;
    double step = 1.0;
    if (bucketsPerPixel > 1.0)
        step = std::exp2(std::ceil(std::log2(bucketsPerPixel)));
    const double stride = std::max(1.0, step / static_cast<double>(kWaveformMaxScan));

    cb_.shader = shader_.get();
    cb_.textureId = v.textureId;
    cb_.startBucket = static_cast<float>(offsetTime * toBucket);
    cb_.endBucket = static_cast<float>((offsetTime + visibleTime) * toBucket);
    cb_.step = static_cast<float>(step);
    cb_.stride = static_cast<float>(stride);
    cb_.bucketsPerPixel = static_cast<float>(bucketsPerPixel);
    cb_.bucketCount = static_cast<float>(v.bucketCount);
    cb_.texW = static_cast<float>(v.texWidth);
    cb_.texH = static_cast<float>(v.texHeight);

    // Read themed color/scale here on the main thread; the deferred GL callback must not touch the theme.
    const ImVec4 &c = ofs::theme::GetStyleColorVec4(AppCol_Waveform);
    cb_.color[0] = c.x;
    cb_.color[1] = c.y;
    cb_.color[2] = c.z;
    cb_.color[3] = c.w;
    cb_.scale = ofs::theme::GetStyleVar(AppVar_WaveformScale);

    drawList->AddCallback(&glCallback, &cb_);
    drawList->AddImage(0, pos, {pos.x + size.x, pos.y + size.y}); // texture handled in the callback
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
}

} // namespace ofs
