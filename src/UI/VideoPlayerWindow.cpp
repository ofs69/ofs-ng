#include "VideoPlayerWindow.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/SceneViewEvents.h"
#include "Localization/Translator.h"
#include "Platform/imgui_impl_opengl3.h"
#include "Scenegraph/VrCamera.h"
#include "Scenegraph/VrShader.h"
#include "UI/Icons.h"
#include "Video/VideoPlayer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <numbers>
#include <ranges>

namespace ofs {

// Center a single line of placeholder text within the current window's content region.
static void drawCenteredPlaceholder(const char *text) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 start = ImGui::GetCursorPos();
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(
        {start.x + std::max(0.f, (avail.x - size.x) * 0.5f), start.y + std::max(0.f, (avail.y - size.y) * 0.5f)});
    ImGui::TextUnformatted(text);
}

VideoPlayerWindow::VideoPlayerWindow(EventQueue &eq) {
    vrShader = std::make_unique<VrShader>();
    targetZoomFactor = zoomFactor;
    targetVrZoom = vrZoom;
    // Snap the live camera when the cursor crosses into a scene with remembered framing. Applied
    // next render (pendingFraming), where contentSize is known to denormalize the stored pan. A restore
    // is the newer intent, so it cancels any still-pending reset (and is never treated as one).
    eq.on<RestoreSceneViewEvent>([this](const RestoreSceneViewEvent &e) {
        pendingFraming = e.framing;
        framingResetPending_ = false;
    });
    // "Reset Video View" command / palette / context menu — same path as the middle-double-click gesture.
    eq.on<ResetVideoFramingEvent>([this](const ResetVideoFramingEvent &) { requestFramingReset(); });
}

VideoPlayerWindow::~VideoPlayerWindow() = default;

VideoFraming VideoPlayerWindow::currentFraming(const ImVec2 &contentSize) const {
    const ImVec2 invContent{contentSize.x > 0.f ? 1.f / contentSize.x : 0.f,
                            contentSize.y > 0.f ? 1.f / contentSize.y : 0.f};
    return {.zoomFactor = targetZoomFactor,
            .translation = {translation.x * invContent.x, translation.y * invContent.y},
            .vrRotation = vrRotation,
            .vrZoom = targetVrZoom};
}

void VideoPlayerWindow::onImGuiRender(const ScriptProject &project, EventQueue &eq, VideoPlayer &player) {
    const auto &state = project.videoPlayer;

    if (!open)
        return;

    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 12.f, fs * 9.f), ImVec2(FLT_MAX, FLT_MAX));
    // "###video_player" is a language-independent id slug. The ProcessingPanel shares this exact id
    // (Processing###video_player) so the two render into one dock node; the slug must match there and
    // in DockLayout.cpp. The visible title is translated; the slug after ### is what keys the dock node.
    // NoNavInputs so the editor's unmodified arrow/Space shortcuts keep working while this panel has
    // focus, instead of being claimed by ImGui keyboard nav (see Application.cpp).
    // Zero window padding so the video fills the pane edge-to-edge; the framing math centers the image
    // within the full content region, so the default padding only showed as a dead margin around it.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible =
        ImGui::Begin(Str::VpwTitle.id("video_player"), &open,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    windowHovered = ImGui::IsWindowHovered();

    // hasMedia(), not isVideoLoaded(): the dummy "scripting without video" timeline reports
    // isVideoLoaded() == true but has no media, and would otherwise fall through to the
    // audio-only branch below (width/height == 0) and show the wrong placeholder.
    if (!player.hasMedia()) {
        drawCenteredPlaceholder(Str::VpwNoVideo);
        if (overlayCallback)
            overlayCallback(ImGui::GetWindowDrawList(), OverlayViewport{}, windowHovered);
        ImGui::End();
        return;
    }

    // Audio-only media has no video track, so getWidth()/getHeight() stay 0. Show a placeholder
    // instead of the previous file's lingering last frame (and avoid the div-by-zero aspect math).
    if (player.getWidth() <= 0 || player.getHeight() <= 0) {
        drawCenteredPlaceholder(Str::VpwAudioOnly);
        if (overlayCallback)
            overlayCallback(ImGui::GetWindowDrawList(), OverlayViewport{}, windowHovered);
        ImGui::End();
        return;
    }

    const uint32_t textureId = player.getFrameTexture();
    if (textureId == 0) {
        if (overlayCallback)
            overlayCallback(ImGui::GetWindowDrawList(), OverlayViewport{}, windowHovered);
        ImGui::End();
        return;
    }

    const ImVec2 contentScreenMin = ImGui::GetCursorScreenPos(); // content-region top-left, pre-SetCursorPos
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    const float videoAspect = (float)player.getWidth() / (float)player.getHeight();
    // Screen-space rect of the displayed image; defaults (whole content) are correct for VR, the 2D
    // branch overrides them with the panned/zoomed image rect. Fed to the overlay each frame.
    ImVec2 imageMin = contentScreenMin;
    ImVec2 imageSize = contentSize;

    // Snap the camera to a freshly-restored scene framing (translation was stored normalized). A
    // pending view-reset rides the same path with the defaults, then persists the recentered framing.
    // A locked view ignores the reset (gesture, command, or "reset both"), mirroring the overlay's
    // locked-position behavior — but a chapter-crossing *restore* still applies, locked or not.
    if (pendingFraming) {
        if (framingResetPending_ && state.locked) {
            framingResetPending_ = false;
            pendingFraming.reset();
        } else {
            const VideoFraming &f = *pendingFraming;
            zoomFactor = targetZoomFactor = f.zoomFactor;
            vrZoom = targetVrZoom = f.vrZoom;
            vrRotation = f.vrRotation;
            translation = {f.translation.x * contentSize.x, f.translation.y * contentSize.y};
            pendingFraming.reset();
            if (framingResetPending_) {
                framingResetPending_ = false;
                eq.push(CaptureVideoFramingEvent{currentFraming(contentSize)});
            }
        }
    }

    // Middle double-click resets the view (2D pan/zoom or VR camera). Routed through requestFramingReset
    // so the gesture, the command, and the context menu share one path (lock enforced at apply time
    // above). The default-framing snap applies next render — one-frame latency, as for a restore.
    if (windowHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle))
        requestFramingReset();

    float baseScaleFactor =
        std::min(contentSize.x / (float)player.getWidth(), contentSize.y / (float)player.getHeight());

    ImVec2 baseSize;
    baseSize.x = (float)player.getWidth() * baseScaleFactor;
    baseSize.y = (float)player.getHeight() * baseScaleFactor;

    // Calculate multiplier based on mode. Use the *target* zoom (not the animating value) so the
    // render resolution jumps once to its final size instead of requesting a new texture every
    // frame of the lerp — that per-frame resize churn reallocated the FBO and disturbed playback.
    // The displayed quad still scales with the live zoomFactor below, so the visual zoom stays smooth.
    float multiplier = 1.0f;
    if (state.activeMode == VideoMode::VrMode) {
        const float pi = std::numbers::pi_v<float>;
        const float hfovRad = VrShader::hfovDegrees * (pi / 180.0f);
        const float k = 0.5f * std::tan(hfovRad * 0.5f);
        multiplier = pi / std::atan(k / targetVrZoom);

        if (contentSize.y > 0.0f) {
            float windowAspect = contentSize.x / contentSize.y;
            multiplier *= std::max(1.0f, windowAspect / videoAspect);
        }
    } else {
        multiplier = targetZoomFactor;
    }

    int reqWidth = std::max(1, std::min((int)(baseSize.x * multiplier * state.resolutionScale), player.getWidth()));
    int reqHeight = std::max(1, std::min((int)(baseSize.y * multiplier * state.resolutionScale), player.getHeight()));

    if (reqWidth != lastReqWidth || reqHeight != lastReqHeight) {
        lastReqWidth = reqWidth;
        lastReqHeight = reqHeight;
        eq.push(SetRenderSizeEvent{.width = reqWidth, .height = reqHeight});
    }

    if (state.activeMode == VideoMode::VrMode) {
        bool framingChanged = false;
        if (!state.locked) {
            // Handle VR zoom
            if (hovered && ImGui::GetIO().MouseWheel != 0) {
                targetVrZoom *= (1.0f + ImGui::GetIO().MouseWheel * 0.1f);
                targetVrZoom = std::clamp(targetVrZoom, 0.05f, 2.0f);
                framingChanged = true;
            }

            if (vrZoom != targetVrZoom) {
                float lerpFactor = std::min(15.0f * ImGui::GetIO().DeltaTime, 1.0f);
                vrZoom += (targetVrZoom - vrZoom) * lerpFactor;
                if (std::abs(vrZoom - targetVrZoom) < 0.0001f) {
                    vrZoom = targetVrZoom;
                }
            }

            // Handle VR rotation. Grab-to-pan: pin the world point under the cursor and keep it under
            // the pointer for the whole drag, so the view tracks the mouse exactly (a flat
            // delta*sensitivity slides the grabbed point away, especially toward the edges where the
            // perspective is non-linear). cursorNdc is the shader's `uv` space: content-region
            // fraction minus 0.5, so 0 is the centre. The image fills the content region in VR.
            const float contentAspect = contentSize.y > 0.0f ? contentSize.x / contentSize.y : 1.0f;
            auto cursorNdc = [&] {
                const ImVec2 m = ImGui::GetMousePos();
                return ImVec2{(m.x - contentScreenMin.x) / contentSize.x - 0.5f,
                              (m.y - contentScreenMin.y) / contentSize.y - 0.5f};
            };
            if (hovered && !overlayHoveredPrev && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                ImGui::GetActiveID() == 0) {
                dragging = true;
                vrGrabDir = ofs::vrcam::unproject(cursorNdc(), vrRotation, vrZoom, contentAspect);
            }
            if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                vrRotation = ofs::vrcam::dragRotation(vrGrabDir, cursorNdc(), vrRotation, vrZoom, contentAspect);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && dragging) {
                dragging = false;
                framingChanged = true;
            }
        } else {
            dragging = false;
        }
        if (framingChanged)
            eq.push(CaptureVideoFramingEvent{currentFraming(contentSize)});

        lastContentWidth = contentSize.x;
        lastContentHeight = contentSize.y;
        lastVideoAspect = videoAspect;

        ImGui::GetWindowDrawList()->AddCallback(
            [](const ImDrawList *parentList, const ImDrawCmd *cmd) {
                auto *self = static_cast<VideoPlayerWindow *>(cmd->UserCallbackData);

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
                self->vrShader->setAspectRatio(self->lastContentWidth / self->lastContentHeight);
                self->vrShader->setVideoAspectRatio(self->lastVideoAspect);
            },
            this);

        ImGui::Image((ImTextureID)(intptr_t)textureId, contentSize);

        ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    } else {
        bool framingChanged = false;
        if (!state.locked) {
            // Handle 2D zoom
            if (hovered && ImGui::GetIO().MouseWheel != 0) {
                targetZoomFactor *= (1.0f + ImGui::GetIO().MouseWheel * 0.1f);
                targetZoomFactor = std::clamp(targetZoomFactor, 0.1f, 10.0f);
                framingChanged = true;
            }

            if (zoomFactor != targetZoomFactor) {
                float oldZoom = zoomFactor;
                float lerpFactor = std::min(15.0f * ImGui::GetIO().DeltaTime, 1.0f);
                zoomFactor += (targetZoomFactor - zoomFactor) * lerpFactor;

                if (std::abs(zoomFactor - targetZoomFactor) < 0.0001f) {
                    zoomFactor = targetZoomFactor;
                }

                // Zoom towards mouse
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 contentStartPos = ImGui::GetWindowPos() + ImGui::GetCursorPos();
                // The center of the video is the center of the content region, plus the current pan translation
                ImVec2 videoCenter = contentStartPos + contentSize * 0.5f + translation;
                ImVec2 mouseOffsetFromCenter = mousePos - videoCenter;

                ImVec2 currentVideoSize = baseSize * oldZoom;
                float zoomPointX = mouseOffsetFromCenter.x / currentVideoSize.x;
                float zoomPointY = mouseOffsetFromCenter.y / currentVideoSize.y;

                float scaleChange = (zoomFactor - oldZoom);
                translation.x -= zoomPointX * scaleChange * baseSize.x;
                translation.y -= zoomPointY * scaleChange * baseSize.y;
            }

            // Handle 2D panning
            if (hovered && !overlayHoveredPrev && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                ImGui::GetActiveID() == 0) {
                dragging = true;
            }
            if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                translation += ImGui::GetIO().MouseDelta;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && dragging) {
                dragging = false;
                framingChanged = true;
            }
        } else {
            dragging = false;
        }
        if (framingChanged)
            eq.push(CaptureVideoFramingEvent{currentFraming(contentSize)});

        ImVec2 displaySize = baseSize * zoomFactor;
        ImVec2 pos = (contentSize - displaySize) * 0.5f + translation;
        imageMin = contentScreenMin + pos;
        imageSize = displaySize;

        // Seed the cursor in screen space, not window-local. SetCursorPos is relative to the window's
        // top-left (DC.CursorPos = window->Pos - Scroll + local), so it ignores the content-region
        // inset (window padding + the docked tab bar). Feeding the content-relative centering offset to
        // it shifted the image up by that inset — the centered video left dead space at the bottom and
        // drew offset from the overlay, which already anchors to imageMin.
        ImGui::SetCursorScreenPos(imageMin);
        ImGui::Image((ImTextureID)(intptr_t)textureId, displaySize);
    }

    hovered = ImGui::IsItemHovered() && windowHovered;

    bool overlayHovered = false;
    if (overlayCallback) {
        OverlayViewport vp{.mode = state.activeMode,
                           .contentMin = contentScreenMin,
                           .contentSize = contentSize,
                           .imageMin = imageMin,
                           .imageSize = imageSize,
                           .vrRotation = vrRotation,
                           .vrZoom = vrZoom,
                           .valid = true};
        overlayHovered = overlayCallback(ImGui::GetWindowDrawList(), vp, windowHovered);
    }
    overlayHoveredPrev = overlayHovered;

    // Don't open the window's context menu over the simulator overlay — it shows its own menu there.
    // Gate only the *open* request; an already-open popup still renders via BeginPopup. Open on a
    // right-click anywhere in the pane (windowHovered), not just over the image, so the menu stays
    // reachable when the video has been panned out of view.
    if (!overlayHovered && windowHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("VideoPlayerPopup");
    if (ImGui::BeginPopup("VideoPlayerPopup")) {
        if (ImGui::MenuItem(Str::VpwFullMode.id("vpw_full_mode"), nullptr, state.activeMode == VideoMode::Full))
            eq.push(VideoModeChangedEvent{VideoMode::Full});
        if (ImGui::MenuItem(Str::VpwVrMode.id("vpw_vr_mode"), nullptr, state.activeMode == VideoMode::VrMode))
            eq.push(VideoModeChangedEvent{VideoMode::VrMode});
        ImGui::Separator();
        if (ImGui::BeginMenu(Str::VpwResolution.id("vpw_resolution"))) {
            ImGui::SetItemTooltip("%s", Str::VpwResolutionTip.c_str());
            if (ImGui::MenuItem(Str::VpwResFull.id("vpw_res_full"), nullptr, state.resolutionScale == 1.0f))
                eq.push(VideoResolutionChangedEvent{1.0f});
            if (ImGui::MenuItem(Str::VpwResHalf.id("vpw_res_half"), nullptr, state.resolutionScale == 0.5f))
                eq.push(VideoResolutionChangedEvent{0.5f});
            if (ImGui::MenuItem(Str::VpwResThird.id("vpw_res_third"), nullptr, state.resolutionScale == 1.0f / 3.0f))
                eq.push(VideoResolutionChangedEvent{1.0f / 3.0f});
            if (ImGui::MenuItem(Str::VpwResQuarter.id("vpw_res_quarter"), nullptr, state.resolutionScale == 0.25f))
                eq.push(VideoResolutionChangedEvent{0.25f});
            if (ImGui::MenuItem(Str::VpwResEighth.id("vpw_res_eighth"), nullptr, state.resolutionScale == 0.125f))
                eq.push(VideoResolutionChangedEvent{0.125f});
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(Str::VpwLocked.iconId(state.locked ? ICON_LOCK : ICON_LOCK_OPEN, "vpw_locked"), nullptr,
                            state.locked))
            eq.push(ModifyEvent<VideoPlayerState>{[](VideoPlayerState &s) { s.locked = !s.locked; }});
        ImGui::Separator();
        // Disabled while locked: a reset is ignored for a locked view (matches the apply-time guard).
        if (ImGui::MenuItem(Str::CmdResetVideoView.id("vpw_reset_view"), nullptr, false, !state.locked))
            eq.push(ResetVideoFramingEvent{});
        ImGui::EndPopup();
    }

    ImGui::End();
}
} // namespace ofs
