#include "VideoPlayerControls.h"
#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Core/TimeSlot.h"
#include "Localization/Translator.h"
#include "UI/BandBar.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/Theme.h"
#include "UI/TimelinePreviewPopup.h"
#include "Util/ColorGen.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/TimeUtil.h"
#include "Video/VideoPlayer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stb_image_write.h>
#include <vector>

namespace ofs {

VideoControlsWindow::VideoControlsWindow(EventQueue &eventQueue) : heatmap(std::make_shared<Heatmap>()) {
    eventQueue.on<AxisModifiedEvent>([this](const AxisModifiedEvent &e) { onAxisModified(e); });
    eventQueue.on<AxisSelectedEvent>([this](const AxisSelectedEvent &e) { onAxisSelected(e); });
    eventQueue.on<DurationChangedEvent>([this](const DurationChangedEvent &e) { onDurationChanged(e); });
    eventQueue.on<EvalCompleteEvent>([this](const EvalCompleteEvent &e) { onEvalComplete(e); });
}

void VideoControlsWindow::onAxisModified(const AxisModifiedEvent &) {
    heatmapDirty = true;
}

void VideoControlsWindow::onAxisSelected(const AxisSelectedEvent &) {
    heatmapDirty = true;
}

void VideoControlsWindow::onDurationChanged(const DurationChangedEvent &) {
    heatmapDirty = true;
}

void VideoControlsWindow::onEvalComplete(const EvalCompleteEvent &) {
    // Processing wrote new resolved actions for an axis; the heatmap reads resolved
    // actions for the active axis, so its cached texture must be regenerated.
    heatmapDirty = true;
}

bool VideoControlsWindow::drawTimelineWidget(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                                             const VideoPreview &preview, TimelinePreviewPopup &previewPopup,
                                             const char *label, float *position) {
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiStyle &style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);

    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    // Horizontal inset applied to each side of the widget (see frameBB below).
    const float inset = style.FramePadding.x;
    // Docking this window into a very narrow slot can leave a content width smaller than the two
    // horizontal insets, which would make frameBB's Max.x < Min.x (negative width) and assert in the
    // std::clamp calls below. There's nothing meaningful to draw that small — bail out.
    if (width <= 2.0f * inset)
        return false;
    // Match the chapter/bookmark band bar below so both tracks share a height and scale
    // together with the font, rather than diverging at non-default font sizes. The band bar
    // spans its full height with no vertical inset, so inset only horizontally here for parity.
    float height = ofs::ui::bandBarHeight();
    const ImRect frameBB(cursorPos + ImVec2(inset, 0.0f), cursorPos + ImVec2(width - inset, height));

    ImGui::ItemSize(frameBB, style.FramePadding.y);
    if (!ImGui::ItemAdd(frameBB, id, &frameBB))
        return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(frameBB, id, &hovered, &held);

    if (held) {
        controlsState.dragging = true;
    }
    if (controlsState.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        controlsState.dragging = false;
    }

    if (controlsState.dragging) {
        float mousePosX = ImGui::GetIO().MousePos.x;
        *position = (mousePosX - frameBB.Min.x) / frameBB.GetWidth();
        *position = std::clamp(*position, 0.0f, 1.0f);
        pressed = true;
    }

    float currentMaxSpeed = ofs::theme::getActive().heatmapMaxSpeed;
    if (currentMaxSpeed != lastHeatmapMaxSpeed) {
        lastHeatmapMaxSpeed = currentMaxSpeed;
        heatmapDirty = true;
    }

    double duration = videoPlayer.getDuration();
    const auto activeIdx = static_cast<size_t>(project.state.activeAxis);
    if (heatmapDirty && duration > 0.0 && project.state.activeAxis < StandardAxis::Count) {
        const auto &axis = project.axes[activeIdx];
        const auto *resolved = axis.resolved ? &axis.resolved->actions : nullptr;
        heatmap->update(duration, resolved ? *resolved : axis.actions);
        heatmapDirty = false;
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float currentPosX = frameBB.Min.x + frameBB.GetWidth() * (*position);
    const float offsetProgressW = currentPosX - frameBB.Min.x;
    constexpr float kHeatmapOverhang = 4.0f;

    drawList->AddRectFilled(frameBB.Min + ImVec2(0.0f, frameBB.GetHeight()),
                            frameBB.Min + ImVec2(offsetProgressW, frameBB.GetHeight() + kHeatmapOverhang),
                            ofs::theme::GetColorU32(AppCol_VideoTimelineFill));

    heatmap->draw(drawList, frameBB.Min, frameBB.Max);

    // Soft themed outline around the heatmap so it reads as a contained element in both
    // light and dark themes. Drawn here (not in Heatmap.cpp, which stays a pure GL renderer).
    drawList->AddRect(frameBB.Min, frameBB.Max, ofs::theme::GetColorU32(ImGuiCol_Border));

    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##heatmapCtx");
    if (ImGui::BeginPopup("##heatmapCtx")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.f);
        ImGui::DragInt(Str::VpcHeatmapHeight.id("vpc_export_height"), &exportHeight, 1.f, 32, 512);
        ImGui::Checkbox(Str::VpcHeatmapFade.id("vpc_export_fade"), &exportFade);
        if (ImGui::MenuItem(Str::VpcExportPng.iconId(ICON_EXPORT, "vpc_export_png"))) {
            exportHeight = std::clamp(exportHeight, 32, 512);
            // Snapshot the export params + a heatmap handle into the callback; it runs on resume
            // (main thread, so renderToBitmap's GL is safe) and is decoupled from this window's life.
            pickFile(eq,
                     {.kind = FileDialogKind::Save,
                      .key = "heatmap_export",
                      .title = Str::VpcExportHeatmapTitle.c_str(),
                      .defaultName = "heatmap.png",
                      .filterPatterns = {"*.png"},
                      .filterDesc = Str::VpcPngImage.c_str()},
                     [&eq, hm = heatmap, height = exportHeight, fade = exportFade](const std::string &path) {
                         if (path.empty())
                             return;
                         constexpr int16_t kExportWidth = 2048;
                         auto pixels = hm->renderToBitmap(kExportWidth, static_cast<int16_t>(height), fade);
                         if (pixels.empty()) {
                             eq.push(NotifyEvent{.level = NotifyLevel::Warning,
                                                 .message = Str::VpcHeatmapExportFailed.c_str()});
                             return;
                         }
                         SDL_IOStream *f = SDL_IOFromFile(path.c_str(), "wb"); // SDL takes UTF-8
                         if (!f) {
                             OFS_CORE_ERROR("Heatmap export: failed to open '{}'", path);
                             eq.push(NotifyEvent{.level = NotifyLevel::Warning,
                                                 .message = Str::VpcHeatmapExportFailed.c_str()});
                             return;
                         }
                         stbi_write_png_to_func(
                             [](void *ctx, void *data, int sz) {
                                 SDL_WriteIO(static_cast<SDL_IOStream *>(ctx), data, size_t(sz));
                             },
                             f, kExportWidth, height, 4, pixels.data(), kExportWidth * 4);
                         SDL_CloseIO(f);
                         eq.push(NotifyEvent{.level = NotifyLevel::Success,
                                             .message = Str::VpcHeatmapExportDone.fmt(
                                                 ofs::util::toUtf8(ofs::util::fromUtf8(path).filename()))});
                     });
        }
        ImGui::EndPopup();
    }

    const auto &timelineState = project.timelineView;
    if (duration > 0.0 && timelineState.visibleTime > 0.0) {
        auto startPos = static_cast<float>(timelineState.offsetTime / duration);
        auto endPos = static_cast<float>((timelineState.offsetTime + timelineState.visibleTime) / duration);

        float startX = frameBB.Min.x + frameBB.GetWidth() * startPos;
        float endX = frameBB.Min.x + frameBB.GetWidth() * endPos;

        startX = std::clamp(startX, frameBB.Min.x, frameBB.Max.x);
        endX = std::clamp(endX, frameBB.Min.x, frameBB.Max.x);

        if (endX > startX) {
            ImU32 visibleRegionCol = ofs::theme::GetColorU32(AppCol_TimelineVisibleRegionFill);
            drawList->AddRectFilled(ImVec2(startX, frameBB.Min.y), ImVec2(endX, frameBB.Max.y), visibleRegionCol);
            ImU32 visibleRegionBorderCol = ofs::theme::GetColorU32(AppCol_TimelineVisibleRegionBorder);
            drawList->AddRect(ImVec2(startX, frameBB.Min.y), ImVec2(endX, frameBB.Max.y), visibleRegionBorderCol);
        }
    }

    constexpr float timelinePosCursorW = 2.0f;
    ImVec2 p1(currentPosX, frameBB.Min.y);
    ImVec2 p2(currentPosX, frameBB.Max.y);
    drawList->AddLine(p1 + ImVec2(0.0f, frameBB.GetHeight() / 3.0f), p2 + ImVec2(0.0f, frameBB.GetHeight() / 3.0f),
                      ofs::theme::GetColorU32(AppCol_PlayCursor), timelinePosCursorW);

    if (hovered && !controlsState.dragging) {
        float mouseX = ImGui::GetIO().MousePos.x;
        drawList->AddLine(ImVec2(mouseX, frameBB.Min.y), ImVec2(mouseX, frameBB.Max.y),
                          ofs::theme::GetColorU32(AppCol_TimelineCursorOuter), timelinePosCursorW);
        drawList->AddLine(ImVec2(mouseX, frameBB.Min.y), ImVec2(mouseX, frameBB.Max.y),
                          ofs::theme::GetColorU32(AppCol_TimelineCursorInner), timelinePosCursorW / 2.0f);

        float hoverPos = (mouseX - frameBB.Min.x) / frameBB.GetWidth();
        double hoverTime = static_cast<double>(hoverPos) * videoPlayer.getDuration();
        previewPopup.render(project, eq, preview, hoverTime, project.playback.cursorPos,
                            Str::VpcHeatmapExportHint.c_str());
    }

    drawList->AddLine(p1, p2, ofs::theme::GetColorU32(AppCol_TimelineCursorOuter), timelinePosCursorW);
    drawList->AddLine(p1, p2, ofs::theme::GetColorU32(AppCol_TimelineCursorInner), timelinePosCursorW / 2.0f);

    return pressed;
}

void VideoControlsWindow::drawBookmarkBar(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer) {
    if (!videoPlayer.isVideoLoaded())
        return;

    const double duration = videoPlayer.getDuration();
    if (duration <= 0.0)
        return;

    const auto &bcState = project.bookmarks;

    const ImVec2 barPos = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    const float inset = ImGui::GetStyle().FramePadding.x;
    const float effectiveW = barW - 2.0f * inset;
    const float kBarH = ofs::ui::bandBarHeight(); // tracks font size so chapter labels never clip
    constexpr float kBmRadius = 5.0f;
    constexpr float kBmHitPad = 2.0f;
    constexpr float kBmChPad = 2.0f;
    constexpr double kMinDur = 0.5;
    const ImVec2 barMin = {barPos.x + inset, barPos.y};
    const ImVec2 barMax = {barMin.x + effectiveW, barPos.y + kBarH};

    auto toX = [&](double t) -> float { return barMin.x + static_cast<float>(t / duration) * effectiveW; };
    auto toTime = [&](float x) -> double {
        return std::clamp(static_cast<double>((x - barMin.x) / effectiveW) * duration, 0.0, duration);
    };

    // Reserve screen space, then register the bar rect as an addressable, non-interactive
    // item so UI tests can anchor to the bookmark strip via ItemInfo. ItemAdd consumes no
    // input, so the manual bookmark hit-testing below is unaffected. (The chapter band
    // registers its own "##chapterbar" item inside drawBandBar over the same rect.)
    ImGui::Dummy({barW, kBarH});
    ImGui::ItemAdd(ImRect(barMin, barMax), ImGui::GetID("##bookmarkbar"), nullptr, ImGuiItemFlags_NoNav);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Background + soft outline so the bookmark/chapter strip reads as its own track,
    // distinct from the seek bar above, even when no bookmarks or chapters are placed.
    dl->AddRectFilled(barMin, barMax, ofs::theme::GetColorU32(AppCol_HudBg));
    dl->AddRect(barMin, barMax, ofs::theme::GetColorU32(ImGuiCol_Border));

    // With nothing placed the strip is an empty track and the right-click that adds a chapter/bookmark
    // is otherwise undiscoverable; prompt it (elided so a narrow bar can't overrun), mirroring the
    // timeline's region bar.
    if (bcState.chapters.empty() && bcState.bookmarks.empty()) {
        const char *hint = ofs::ui::elide(Str::VpcBarHint.c_str(), (barMax.x - barMin.x) - ImGui::GetFontSize());
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({(barMin.x + barMax.x - ts.x) * 0.5f, (barMin.y + barMax.y - ts.y) * 0.5f},
                    ofs::theme::GetColorU32(ImGuiCol_TextDisabled), hint);
    }

    const int bandCount = static_cast<int>(bcState.chapters.size());
    auto *bandData = ofs::FrameAllocator::instance().allocArray<ofs::ui::BandItem>(bandCount);
    for (int i = 0; i < bandCount; ++i) {
        const auto &ch = bcState.chapters[i];
        bandData[i] = {.startTime = ch.startTime, .endTime = ch.endTime, .color = ch.color, .name = ch.name};
    }
    std::span<const ofs::ui::BandItem> bands(bandData, bandCount);

    const double playheadTime = project.playback.cursorPos;
    // Occlusion-aware: IsItemHovered() on the bar item registered above consults g.HoveredWindow
    // (Z-order), so the bookmark hover/tooltip stays suppressed when another window covers the
    // strip — a raw IsMouseHoveringRect is geometric only and fires through an overlapping window.
    const bool barHovered = ImGui::IsItemHovered();
    const bool bandDragging = bandDragState.mode != ofs::ui::BandBarDragState::Mode::None;
    auto &bmDrag = barState.bmDrag;

    // ── Determine if mouse is over a bookmark (for chapter interaction priority) ──
    // Bookmarks take priority over chapters: if the cursor is on a bookmark,
    // suppress the chapter band's Phase 3 so only the bookmark gets the click.
    bool overBookmark = false;
    if (!bandDragging && !bmDrag.isDragging && !bmDrag.hasDragCandidate && barHovered) {
        for (const auto &bm : bcState.bookmarks) {
            const float bx = toX(bm.time);
            const ImVec2 hitMin = {bx - kBmRadius - kBmHitPad, barMin.y};
            const ImVec2 hitMax = {bx + kBmRadius + kBmHitPad, barMin.y + kBmRadius * 2.0f + kBmChPad * 2.0f};
            if (ImGui::IsMouseHoveringRect(hitMin, hitMax)) {
                overBookmark = true;
                break;
            }
        }
    }

    // ── Chapters (BandBar) ───────────────────────────────────────────────────
    ofs::ui::BandBarCallbacks callbacks;

    // Commit once on drag-release, not every frame: the band previews locally during the drag (BandBar
    // draws from its own dragState), so a single end-of-gesture mutation gives one undo step per move/resize
    // — matching the region band — instead of one per frame.
    callbacks.onDragEnd = [&](int idx, double finalStart, double finalEnd) {
        if (idx < 0 || idx >= static_cast<int>(bcState.chapters.size()))
            return;
        const auto &ch = bcState.chapters[idx];
        // Preview-only during the drag, so ch still holds the pre-drag span. An Alt snap-back or a
        // released-in-place drag is a no-op; emitting the event anyway would record an empty undo step.
        if (finalStart == ch.startTime && finalEnd == ch.endTime)
            return;
        eq.push(ModifyBookmarkChapterEvent{.apply = [idx, finalStart, finalEnd](BookmarkChapterState &s) {
            if (idx >= 0 && idx < static_cast<int>(s.chapters.size())) {
                s.chapters[idx].startTime = finalStart;
                s.chapters[idx].endTime = finalEnd;
            }
        }});
    };

    callbacks.onClick = [&](int idx) { eq.push(SeekEvent{bcState.chapters[idx].startTime}); };

    callbacks.onRightClick = [&](int idx, double t) {
        barState.activeCtx = BookmarkBarState::CtxTarget::Chapter;
        barState.ctxIdx = idx;
        barState.editName = bcState.chapters[idx].name;
        barState.ctxTime = t;
        barState.activeEdit = BookmarkBarState::ActiveEdit::None; // each menu-open starts fresh gestures
        ImGui::OpenPopup("##bar_ctx");
    };

    callbacks.onEmptyRightClick = [&](double t) {
        barState.ctxTime = t;
        barState.activeCtx = BookmarkBarState::CtxTarget::Empty;
        barState.editName.clear();
        barState.activeEdit = BookmarkBarState::ActiveEdit::None;
        ImGui::OpenPopup("##bar_ctx");
    };

    ofs::ui::drawBandBar(dl, barMin, barMax, bands, bandDragState, playheadTime, 0.0, duration, kMinDur, toX, toTime,
                         callbacks, "##chapterbar", overBookmark || bmDrag.isDragging || bmDrag.hasDragCandidate);

    // ── Bookmark drag state update ───────────────────────────────────────────
    int justEndedBmIdx = -1;
    double justEndedBmTime = 0.0;

    if (bmDrag.isDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            bool altHeld = ImGui::GetIO().KeyAlt;
            // Snap to the playhead when the cursor is within the shared band snap distance of it, matching
            // the chapter band's edge/move snap so a bookmark can be parked exactly on the current frame.
            const float mouseX = ImGui::GetIO().MousePos.x;
            const bool nearPlayhead = std::abs(mouseX - toX(playheadTime)) < ofs::ui::bandBarSnapPx();
            const double droppedTime = nearPlayhead ? playheadTime : toTime(mouseX);
            bmDrag.previewTime = altHeld ? bmDrag.originalTime : droppedTime;
            ImGui::SetMouseCursor(altHeld ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
        } else {
            justEndedBmIdx = bmDrag.idx;
            justEndedBmTime = bmDrag.previewTime;
            eq.push(ModifyBookmarkChapterEvent{
                .apply = [idx = bmDrag.idx, t = bmDrag.previewTime](BookmarkChapterState &s) {
                    if (idx >= 0 && idx < static_cast<int>(s.bookmarks.size()))
                        s.bookmarks[idx].time = t;
                }});
            bmDrag.isDragging = false;
            bmDrag.idx = -1;
        }
    }

    if (bmDrag.hasDragCandidate) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            bmDrag.isDragging = true;
            bmDrag.previewTime = bmDrag.originalTime;
            bmDrag.hasDragCandidate = false;
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            eq.push(SeekEvent{bcState.bookmarks[bmDrag.idx].time});
            bmDrag.hasDragCandidate = false;
            bmDrag.idx = -1;
        }
    }

    // ── Draw bookmarks ───────────────────────────────────────────────────────
    for (int i = 0; i < static_cast<int>(bcState.bookmarks.size()); ++i) {
        const auto &bm = bcState.bookmarks[i];
        bool isDragTarget = (bmDrag.isDragging && bmDrag.idx == i) || (bmDrag.hasDragCandidate && bmDrag.idx == i);
        double displayTime = isDragTarget ? bmDrag.previewTime : (justEndedBmIdx == i) ? justEndedBmTime : bm.time;
        const float bx = toX(displayTime);
        if (bx < barMin.x || bx > barMax.x)
            continue;

        const ImVec2 center = {bx, barMin.y + kBmRadius + kBmChPad};
        const ImVec2 hitMin = {bx - kBmRadius - kBmHitPad, barMin.y};
        const ImVec2 hitMax = {bx + kBmRadius + kBmHitPad, barMin.y + kBmRadius * 2.0f + kBmChPad * 2.0f};
        const bool hov = !bandDragging && !bmDrag.isDragging && !bmDrag.hasDragCandidate && barHovered &&
                         ImGui::IsMouseHoveringRect(hitMin, hitMax);

        dl->AddCircleFilled(center, kBmRadius,
                            (hov || isDragTarget) ? ofs::theme::GetColorU32(AppCol_BookmarkDotHovered)
                                                  : ofs::theme::GetColorU32(AppCol_BookmarkDot));
        dl->AddCircle(center, kBmRadius, ofs::theme::GetColorU32(AppCol_BookmarkOutline), 0, 1.5f);
    }

    // ── Bookmark interaction (highest priority) ──────────────────────────────
    if (!bandDragging && !bmDrag.isDragging && !bmDrag.hasDragCandidate) {
        bool interacted = false;
        bool tooltipShown = false;

        for (int i = 0; i < static_cast<int>(bcState.bookmarks.size()); ++i) {
            const auto &bm = bcState.bookmarks[i];
            const float bx = toX(bm.time);
            if (bx < barMin.x || bx > barMax.x)
                continue;

            const ImVec2 hitMin = {bx - kBmRadius - kBmHitPad, barMin.y};
            const ImVec2 hitMax = {bx + kBmRadius + kBmHitPad, barMin.y + kBmRadius * 2.0f + kBmChPad * 2.0f};
            if (!barHovered || !ImGui::IsMouseHoveringRect(hitMin, hitMax))
                continue;

            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (!tooltipShown) {
                const char *bmName = bm.name.empty() ? Str::VpcUnnamed.c_str() : bm.name.c_str();
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(Str::VpcBookmarkTip.fmt(bmName));
                ImGui::Text("%s", TimeUtil::formatTime(bm.time, true));
                ImGui::EndTooltip();
                tooltipShown = true;
            }

            if (!interacted) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    bmDrag.hasDragCandidate = true;
                    bmDrag.idx = i;
                    bmDrag.originalTime = bm.time;
                    bmDrag.previewTime = bm.time;
                    interacted = true;
                } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    barState.activeCtx = BookmarkBarState::CtxTarget::Bookmark;
                    barState.ctxIdx = i;
                    barState.editName = bm.name;
                    barState.activeEdit = BookmarkBarState::ActiveEdit::None;
                    ImGui::OpenPopup("##bar_ctx");
                    interacted = true;
                }
            }
        }
    }

    // --- Context popup ---
    if (ImGui::BeginPopup("##bar_ctx")) {
        const double playheadTime2 = project.playback.cursorPos;

        // Shared helper: create a chapter anchored at `requested`. If that instant is already
        // covered by a chapter, the new chapter begins at the first free moment after it (snap
        // forward). Only fails — with a toast, never silently — when no gap of at least kMinDur
        // remains before the next chapter or the video's end.
        auto addChapterAt = [&](double requested) {
            const TimeSlot slot = firstFreeSlot(
                bcState.chapters, requested, duration, [](const Chapter &c) { return c.startTime; },
                [](const Chapter &c) { return c.endTime; });
            if (!slot.fits(kMinDur)) {
                eq.push(NotifyEvent{.level = NotifyLevel::Warning, .message = Str::VpcChapterNoRoom.c_str()});
                return;
            }
            const double chEnd = slot.start + std::min(duration * 0.1, slot.length());
            eq.push(ModifyBookmarkChapterEvent{
                .apply = [start = slot.start, chEnd,
                          color = ofs::util::goldenRatioColor(static_cast<size_t>(project.state.autoNameSeed) +
                                                              bcState.chapters.size())](BookmarkChapterState &s) {
                    s.chapters.push_back({.startTime = start, .endTime = chEnd, .name = "", .color = color});
                }});
        };

        switch (barState.activeCtx) {

        case BookmarkBarState::CtxTarget::Empty:
            ImGui::TextDisabled("%s", TimeUtil::formatTime(barState.ctxTime, true));
            ImGui::Separator();
            if (ImGui::MenuItem(Str::VpcAddBookmarkPlayhead.iconId(ICON_BOOKMARK, "add_bookmark_playhead"))) {
                eq.push(ModifyBookmarkChapterEvent{.apply = [t = playheadTime2](BookmarkChapterState &s) {
                    s.bookmarks.push_back({.time = t, .name = ""});
                }});
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem(Str::VpcAddChapterPlayhead.iconId(ICON_CHAPTER, "add_chapter_playhead"))) {
                addChapterAt(playheadTime2);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Str::VpcAddBookmarkHere.iconId(ICON_BOOKMARK, "add_bookmark_here"))) {
                eq.push(ModifyBookmarkChapterEvent{.apply = [t = barState.ctxTime](BookmarkChapterState &s) {
                    s.bookmarks.push_back({.time = t, .name = ""});
                }});
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem(Str::VpcAddChapterHere.iconId(ICON_CHAPTER, "add_chapter_here"))) {
                addChapterAt(barState.ctxTime);
                ImGui::CloseCurrentPopup();
            }
            break;

        case BookmarkBarState::CtxTarget::Bookmark:
            if (barState.ctxIdx >= 0 && barState.ctxIdx < static_cast<int>(bcState.bookmarks.size())) {
                const auto &bm = bcState.bookmarks[barState.ctxIdx];
                ImGui::TextDisabled("%s", TimeUtil::formatTime(bm.time, true));
                ImGui::Separator();
                ImGui::SetNextItemWidth(-FLT_MIN);
                // Push live for band preview, but snapshot undo only on the first keystroke so the whole
                // rename is one undo step (see the chapter color edit below for the shared rationale).
                if (ImGui::InputText("##bmname", &barState.editName)) {
                    eq.push(ModifyBookmarkChapterEvent{
                        .apply =
                            [idx = barState.ctxIdx, name = barState.editName](BookmarkChapterState &s) {
                                if (idx >= 0 && idx < static_cast<int>(s.bookmarks.size()))
                                    s.bookmarks[idx].name = name;
                            },
                        .snapshot = barState.activeEdit != BookmarkBarState::ActiveEdit::BookmarkName});
                    barState.activeEdit = BookmarkBarState::ActiveEdit::BookmarkName;
                }
                if (ImGui::IsItemDeactivated())
                    barState.activeEdit = BookmarkBarState::ActiveEdit::None;
                ImGui::Separator();
                if (ImGui::MenuItem(Str::VpcSeekHere.iconId(ICON_SEEK, "vpc_seek_here"))) {
                    eq.push(SeekEvent{bm.time});
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(Str::VpcDelete.iconId(ICON_TRASH, "vpc_bm_delete"))) {
                    eq.push(ModifyBookmarkChapterEvent{.apply = [idx = barState.ctxIdx](BookmarkChapterState &s) {
                        if (idx >= 0 && idx < static_cast<int>(s.bookmarks.size()))
                            s.bookmarks.erase(s.bookmarks.begin() + idx);
                    }});
                    ImGui::CloseCurrentPopup();
                }
            }
            break;

        case BookmarkBarState::CtxTarget::Chapter:
            if (barState.ctxIdx >= 0 && barState.ctxIdx < static_cast<int>(bcState.chapters.size())) {
                const auto &ch = bcState.chapters[barState.ctxIdx];
                ImGui::TextDisabled("%s - %s", TimeUtil::formatTime(ch.startTime, true),
                                    TimeUtil::formatTime(ch.endTime, true));
                ImGui::Separator();
                ImGui::SetNextItemWidth(-FLT_MIN);
                // One undo step per rename, not per keystroke (see the color edit below for the rationale).
                if (ImGui::InputText("##chname", &barState.editName)) {
                    eq.push(ModifyBookmarkChapterEvent{
                        .apply =
                            [idx = barState.ctxIdx, name = barState.editName](BookmarkChapterState &s) {
                                if (idx >= 0 && idx < static_cast<int>(s.chapters.size()))
                                    s.chapters[idx].name = name;
                            },
                        .snapshot = barState.activeEdit != BookmarkBarState::ActiveEdit::ChapterName});
                    barState.activeEdit = BookmarkBarState::ActiveEdit::ChapterName;
                }
                if (ImGui::IsItemDeactivated())
                    barState.activeEdit = BookmarkBarState::ActiveEdit::None;
                {
                    // Per-chapter band color, same gesture handling as the region band (ScriptTimeline):
                    // ColorEdit3 fires every frame of a picker drag, so push the live mutation each frame
                    // for real-state preview but snapshot undo only on the first change — one undo step per
                    // gesture instead of one per frame. IsItemDeactivatedAfterEdit never fires when the
                    // picker is dismissed by clicking outside (which also closes this menu), so the
                    // first-change check is the reliable gesture boundary.
                    ImColor colVec(ch.color);
                    std::array<float, 3> col = {colVec.Value.x, colVec.Value.y, colVec.Value.z};
                    if (ImGui::ColorEdit3(Str::VpcColor.id("ch_color"), col.data(), ImGuiColorEditFlags_NoInputs)) {
                        eq.push(ModifyBookmarkChapterEvent{
                            .apply =
                                [idx = barState.ctxIdx,
                                 color = static_cast<ImU32>(ImColor(col[0], col[1], col[2], colVec.Value.w))](
                                    BookmarkChapterState &s) {
                                    if (idx >= 0 && idx < static_cast<int>(s.chapters.size()))
                                        s.chapters[idx].color = color;
                                },
                            .snapshot = barState.activeEdit != BookmarkBarState::ActiveEdit::ChapterColor});
                        barState.activeEdit = BookmarkBarState::ActiveEdit::ChapterColor;
                    }
                    if (ImGui::IsItemDeactivated())
                        barState.activeEdit = BookmarkBarState::ActiveEdit::None; // next change = fresh undo step
                }
                ImGui::Separator();
                if (ImGui::MenuItem(Str::VpcAddBookmarkPlayhead.iconId(ICON_BOOKMARK, "add_bookmark_playhead"))) {
                    eq.push(ModifyBookmarkChapterEvent{.apply = [t = playheadTime2](BookmarkChapterState &s) {
                        s.bookmarks.push_back({.time = t, .name = ""});
                    }});
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(Str::VpcAddBookmarkHere.iconId(ICON_BOOKMARK, "add_bookmark_here"))) {
                    eq.push(ModifyBookmarkChapterEvent{.apply = [t = barState.ctxTime](BookmarkChapterState &s) {
                        s.bookmarks.push_back({.time = t, .name = ""});
                    }});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(Str::VpcSeekToStart.iconId(ICON_SEEK, "vpc_seek_start"))) {
                    eq.push(SeekEvent{ch.startTime});
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(Str::VpcSeekToEnd.iconId(ICON_SEEK, "vpc_seek_end"))) {
                    eq.push(SeekEvent{ch.endTime});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                // Split at the playhead into two chapters — only when the playhead sits inside the chapter
                // far enough from each edge that both halves clear the minimum. The right half keeps the
                // name but takes a new color so the two read as distinct.
                const bool canSplit = playheadTime2 > ch.startTime + kMinDur && playheadTime2 < ch.endTime - kMinDur;
                ImGui::BeginDisabled(!canSplit);
                if (ImGui::MenuItem(Str::TlSplitAtPlayhead.iconId(ICON_SPLIT, "vpc_ch_split")) && canSplit) {
                    eq.push(ModifyBookmarkChapterEvent{.apply = [idx = barState.ctxIdx, splitT = playheadTime2,
                                                                 seed = project.state.autoNameSeed](
                                                                    BookmarkChapterState &s) {
                        if (idx < 0 || idx >= static_cast<int>(s.chapters.size()))
                            return;
                        if (splitT - s.chapters[idx].startTime < kMinDur || s.chapters[idx].endTime - splitT < kMinDur)
                            return;
                        Chapter right = s.chapters[idx];
                        right.startTime = splitT;
                        right.color = ofs::util::goldenRatioColor(static_cast<size_t>(seed) + s.chapters.size());
                        s.chapters[idx].endTime = splitT;
                        s.chapters.push_back(std::move(right));
                    }});
                    ImGui::CloseCurrentPopup();
                }
                if (!canSplit &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s", Str::TlSplitReq.c_str());
                ImGui::EndDisabled();
                if (ImGui::MenuItem(Str::VpcDelete.iconId(ICON_TRASH, "vpc_ch_delete"))) {
                    eq.push(ModifyBookmarkChapterEvent{.apply = [idx = barState.ctxIdx](BookmarkChapterState &s) {
                        if (idx >= 0 && idx < static_cast<int>(s.chapters.size()))
                            s.chapters.erase(s.chapters.begin() + idx);
                    }});
                    ImGui::CloseCurrentPopup();
                }
            }
            break;

        default:
            break;
        }
        ImGui::EndPopup();
    }
}

void VideoControlsWindow::render(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                                 const VideoPreview &preview, TimelinePreviewPopup &previewPopup) {
    if (!ImGui::Begin(Str::VpcTitle.id("video_controls"), nullptr,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        return;
    }

    if (!videoPlayer.isVideoLoaded()) {
        ImGui::End();
        return;
    }

    // Speed calculation
    {
        double currentTime = ImGui::GetTime();
        double dt = currentTime - controlsState.lastTime;
        controlsState.lastTime = currentTime;

        constexpr float speedCalcUpdateFrequency = 1.0f;
        controlsState.speedCalcAccumulator += dt;

        if (!videoPlayer.isPaused()) {
            if (controlsState.speedCalcAccumulator >= speedCalcUpdateFrequency) {
                auto dur = static_cast<float>(videoPlayer.getDuration());
                float pos = dur > 0.0f ? static_cast<float>(project.playback.cursorPos / dur) : 0.0f;
                float expectedStep = speedCalcUpdateFrequency / dur;
                float actualStep = std::abs(pos - controlsState.lastPlayerPosition);
                controlsState.actualPlaybackSpeed = actualStep / expectedStep;
                controlsState.lastPlayerPosition = pos;
                controlsState.speedCalcAccumulator = 0.0;
            }
        } else {
            auto dur = static_cast<float>(videoPlayer.getDuration());
            controlsState.lastPlayerPosition = dur > 0.0f ? static_cast<float>(project.playback.cursorPos / dur) : 0.0f;
            controlsState.speedCalcAccumulator = 0.0;
        }
    }

    auto duration = static_cast<float>(videoPlayer.getDuration());
    float position = duration > 0.0f ? static_cast<float>(project.playback.cursorPos / duration) : 0.0f;

    // The volume/speed sliders sit in the same content-sized columns as the transport buttons and time
    // readout. A fill (-FLT_MIN) slider there would ratchet: the column measures its own given width as
    // content, so a larger font pushes the column wider (via the siblings) but the slider then re-fills
    // and never lets it shrink back. Size each slider from its sibling's deterministic width instead.
    const float btnW = ImGui::GetFrameHeight() * 1.6f;
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float transportGroupW = btnW * 5.0f + itemSpacing * 4.0f;
    const float minSliderW = ImGui::GetFrameHeight() * 2.0f;
    float timeAnchorW = 0.0f;

    if (ImGui::BeginTable("##controls", 3, 0)) {
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c3", ImGuiTableColumnFlags_WidthFixed);

        // ── Row 1: Transport | Seek bar | Time ───────────────────────────────
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        constexpr float seekTime = 3.0f;
        {
            // Frame-step back/forward through the navigator seam (Frame granularity = one overlay-grid
            // step), the same resolution the prev/next-step keys use.
            if (ImGui::Button(ICON_STEP_BACKWARD, {btnW, 0.f}))
                eq.push(StepRequestEvent{.direction = StepDirection::Backward, .granularity = StepGranularity::Frame});
            ImGui::SetItemTooltip("%s", Str::VpcStepBack.c_str());
            ImGui::SameLine();
            if (ImGui::Button(ICON_BACKWARD, {btnW, 0.f}))
                eq.push(SeekEvent{project.playback.cursorPos - seekTime});
            ImGui::SetItemTooltip("%s", Str::VpcSeekBack.fmt(static_cast<int>(seekTime)));
            ImGui::SameLine();
            bool paused = videoPlayer.isPaused();
            if (ImGui::Button(paused ? ICON_PLAY : ICON_PAUSE, {btnW, 0.f}))
                eq.push(PlayPauseEvent{});
            ImGui::SetItemTooltip("%s", paused ? Str::VpcPlay.c_str() : Str::VpcPause.c_str());
            ImGui::SameLine();
            if (ImGui::Button(ICON_FORWARD, {btnW, 0.f}))
                eq.push(SeekEvent{project.playback.cursorPos + seekTime});
            ImGui::SetItemTooltip("%s", Str::VpcSeekForward.fmt(static_cast<int>(seekTime)));
            ImGui::SameLine();
            if (ImGui::Button(ICON_STEP_FORWARD, {btnW, 0.f}))
                eq.push(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Frame});
            ImGui::SetItemTooltip("%s", Str::VpcStepForward.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        if (drawTimelineWidget(project, eq, videoPlayer, preview, previewPopup, "###TimelineWidget", &position))
            eq.push(SeekEvent{static_cast<double>(position * duration)});

        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        const char *timeStr = fmtScratch("{} / {}", TimeUtil::formatTime(project.playback.cursorPos, true),
                                         TimeUtil::formatTime(videoPlayer.getDuration(), true));
        ImGui::TextUnformatted(timeStr);
        timeAnchorW = ImGui::CalcTextSize(timeStr).x;

        // ── Row 2: Volume | Bookmark bar | Speed ─────────────────────────────
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        {
            float volume = videoPlayer.getVolume();
            bool muted = (volume == 0.0f);
            if (ImGui::Checkbox(muted ? ICON_VOLUME_OFF "##mute" : ICON_VOLUME_UP "##mute", &muted)) {
                if (muted) {
                    controlsState.preMuteVolume = volume > 0.0f ? volume : 1.0f;
                    eq.push(VolumeChangedEvent{0.0f});
                } else {
                    eq.push(VolumeChangedEvent{controlsState.preMuteVolume});
                }
            }
            const float muteW = ImGui::GetItemRectSize().x;
            ImGui::SetItemTooltip("%s", muted ? Str::VpcUnmute.c_str() : Str::VpcMute.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(minSliderW, transportGroupW - muteW - itemSpacing));
            // Show the gain as a percent (0–130%) so the >100% boost reads as a boost, not a bare "1.3".
            float volPct = volume * 100.0f;
            if (ImGui::SliderFloat("##Volume", &volPct, 0.0f, 130.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                volume = std::round(volPct / 10.0f) * 10.0f / 100.0f; // snap to 10 % steps (the old 0.1 grid)
                eq.push(VolumeChangedEvent{volume});
            }
        }

        ImGui::TableSetColumnIndex(1);
        drawBookmarkBar(project, eq, videoPlayer);

        ImGui::TableSetColumnIndex(2);
        const float speedRowStartX = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Str::VpcSpeed);
        ImGui::SameLine();
        float currentSpeed = videoPlayer.getPlaybackSpeed();
        if (ImGui::Button(ICON_CHEVRON_UP "##speed_presets"))
            ImGui::OpenPopup("##speed_ctx");
        ImGui::SetItemTooltip("%s", Str::VpcSpeedPresets.c_str());
        // Controls live at the bottom of the screen, so anchor the popup's lower-left to the
        // button's top-left (pivot y=1) to force it to grow upward.
        const ImVec2 btnTopLeft = ImGui::GetItemRectMin();
        ImGui::SetNextWindowPos(btnTopLeft, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        if (ImGui::BeginPopup("##speed_ctx")) {
            static constexpr float kSpeedPresets[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f};
            for (float preset : kSpeedPresets) {
                if (ImGui::Selectable(fmtScratch("{:g}x", preset), currentSpeed == preset))
                    eq.push(PlaybackSpeedEvent{preset});
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        const float speedUsedW = ImGui::GetCursorPosX() - speedRowStartX;
        ImGui::SetNextItemWidth(std::max(minSliderW, timeAnchorW - speedUsedW));
        if (ImGui::SliderFloat("##Speed", &currentSpeed, 0.1f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp))
            eq.push(PlaybackSpeedEvent{currentSpeed});

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace ofs
