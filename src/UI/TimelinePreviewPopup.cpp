#include "TimelinePreviewPopup.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "Scenegraph/VrShader.h"
#include "Util/FrameAllocator.h"
#include "Util/TimeUtil.h"
#include "Video/VideoMode.h"
#include "Video/VideoPreview.h"
#include "imgui.h"
#include <cmath>

namespace ofs {

TimelinePreviewPopup::TimelinePreviewPopup() {
    vrShader = std::make_unique<VrShader>();
}

TimelinePreviewPopup::~TimelinePreviewPopup() = default;

namespace {
// Draw the timestamp + signed offset-from-playhead lines below the frame (or alone, as the
// text-only fallback). Heap-free via fmtScratch; the offset string is localized through TlpDelta.
void drawCaption(double hoverTime, double currentTime) {
    ImGui::TextUnformatted(TimeUtil::formatTime(hoverTime, true));
    double delta = hoverTime - currentTime;
    const char *signedDelta = fmtScratch("{}{}", delta >= 0.0 ? "+" : "-", TimeUtil::formatTime(std::abs(delta), true));
    ImGui::TextUnformatted(Str::TlpDelta.fmt(signedDelta));
}

// Muted action hint at the bottom of the tooltip; no-op when the caller passes none.
void drawFooterHint(const char *footerHint) {
    if (!footerHint)
        return;
    ImGui::Separator();
    ImGui::TextDisabled("%s", footerHint);
}
} // namespace

void TimelinePreviewPopup::render(const ScriptProject &project, EventQueue &eq, const VideoPreview &preview,
                                  double hoverTime, double currentTime, const char *footerHint) {
    // Feature off: preserve the original text-only time tooltip and request nothing.
    if (!preview.isEnabled()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(TimeUtil::formatTime(hoverTime, true));
        drawFooterHint(footerHint);
        ImGui::EndTooltip();
        return;
    }

    eq.push(PreviewSeekRequestEvent{hoverTime}); // every hover frame; the engine coalesces

    // No frame yet (still decoding the first one) — show the caption alone so there's no blank box.
    if (!preview.isReady()) {
        ImGui::BeginTooltip();
        drawCaption(hoverTime, currentTime);
        drawFooterHint(footerHint);
        ImGui::EndTooltip();
        return;
    }

    const bool vr = project.videoPlayer.activeMode == VideoMode::VrMode;
    const float em = ImGui::GetFontSize();
    const uint32_t tex = preview.getFrameTexture();

    ImVec2 imgSize;
    if (vr) {
        // The equirect is projected onto a fixed 16:9 viewport; aspect is the viewport's, not the
        // source frame's.
        imgSize = {em * 16.f, em * 9.f};
    } else {
        const auto vw = static_cast<float>(preview.getWidth());
        const auto vh = static_cast<float>(preview.getHeight());
        imgSize.x = em * 16.f;
        imgSize.y = vh > 0.f && vw > 0.f ? imgSize.x * (vh / vw) : imgSize.x * (9.f / 16.f);
    }

    ImGui::BeginTooltip();
    if (vr) {
        // Project the equirect through the same framing the main player shows. Mirrors
        // VideoPlayerWindow's AddCallback shader sandwich; the snapshot fields feed the deferred
        // callback (it runs when the tooltip draw list is rendered, after this function returns).
        vrRotation = project.activeSceneView.framing.vrRotation;
        vrZoom = project.activeSceneView.framing.vrZoom;
        contentAspect = imgSize.y > 0.f ? imgSize.x / imgSize.y : 1.f;
        const auto vw = static_cast<float>(preview.getWidth());
        const auto vh = static_cast<float>(preview.getHeight());
        videoAspect = vh > 0.f ? vw / vh : 1.f;

        ImGui::GetWindowDrawList()->AddCallback(
            [](const ImDrawList *, const ImDrawCmd *cmd) {
                auto *self = static_cast<TimelinePreviewPopup *>(cmd->UserCallbackData);
                // Recompute ortho from the *current* draw data — the tooltip lives in its own
                // viewport, distinct from the main window's.
                ImDrawData *drawData = ImGui::GetDrawData();
                float left = drawData->DisplayPos.x;
                float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
                float top = drawData->DisplayPos.y;
                float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
                const float orthoProjection[4][4] = {
                    {2.0f / (right - left), 0.0f, 0.0f, 0.0f},
                    {0.0f, 2.0f / (top - bottom), 0.0f, 0.0f},
                    {0.0f, 0.0f, -1.0f, 0.0f},
                    {(right + left) / (left - right), (top + bottom) / (bottom - top), 0.0f, 1.0f},
                };
                self->vrShader->use();
                self->vrShader->setProjMtx(&orthoProjection[0][0]);
                self->vrShader->setRotation(self->vrRotation.x, self->vrRotation.y);
                self->vrShader->setZoom(self->vrZoom);
                self->vrShader->setAspectRatio(self->contentAspect);
                self->vrShader->setVideoAspectRatio(self->videoAspect);
            },
            this);
        ImGui::Image((ImTextureID)(intptr_t)tex, imgSize);
        ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    } else {
        ImGui::Image((ImTextureID)(intptr_t)tex, imgSize);
    }
    drawCaption(hoverTime, currentTime);
    drawFooterHint(footerHint);
    ImGui::EndTooltip();
}

} // namespace ofs
