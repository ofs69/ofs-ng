#include "BandBar.h"
#include "Localization/Translator.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Theme.h"
#include "Util/TimeUtil.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace ofs::ui {

static constexpr float kChPad = kBandBarPad;
static constexpr float kEdgeW = kBandBarEdgeW;
static constexpr float kSnapPx = kBandBarSnapPx;

void drawBandBar(ImDrawList *dl, ImVec2 barMin, ImVec2 barMax, std::span<const BandItem> bands,
                 BandBarDragState &dragState, double playheadTime, double minTime, double maxTime, double minDur,
                 const std::function<float(double)> &toX, const std::function<double(float)> &toTime,
                 const BandBarCallbacks &callbacks, const char *id, bool suppressNewInteraction) {
    const float barW = barMax.x - barMin.x;
    const float barH = barMax.y - barMin.y;
    if (barW <= 0.0f || barH <= 0.0f)
        return;

    // Register the bar's rect as an addressable, non-interactive item so UI tests can
    // anchor to its exact geometry via ItemInfo instead of re-deriving heights/offsets.
    // ItemAdd consumes no input, so the manual hit-testing below is unaffected.
    ImGui::ItemAdd(ImRect(barMin, barMax), ImGui::GetID(id), nullptr, ImGuiItemFlags_NoNav);
    // Occlusion-aware bar hover: IsItemHovered() on the just-added bar item consults
    // g.HoveredWindow (Z-order), so it reports false when another window covers the bar —
    // unlike a raw IsMouseHoveringRect, which is purely geometric and would fire through an
    // overlapping window, leaking the empty-area timestamp tooltip. Captured here while the
    // bar is the last item; Phase 3 adds per-band items that would otherwise shadow it.
    const bool barHovered = ImGui::IsItemHovered();

    const int bandCount = static_cast<int>(bands.size());

    // ── Phase 1: candidate promotion + drag update ───────────────────────────
    dragState.endedThisFrame = false;

    // Promote candidate to drag, or fire onClick on click release
    if (dragState.hasDragCandidate) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            dragState.mode = dragState.candidateMode;
            dragState.dragIdx = dragState.candidateIdx;
            dragState.previewStart = dragState.candidateStart;
            dragState.previewEnd = dragState.candidateEnd;
            dragState.originalStart = dragState.candidateStart;
            dragState.originalEnd = dragState.candidateEnd;
            dragState.dragTimeAtStart = dragState.candidateDragTime;
            dragState.hasDragCandidate = false;
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (callbacks.onClick)
                callbacks.onClick(dragState.candidateIdx);
            dragState.hasDragCandidate = false;
            dragState.candidateIdx = -1;
            dragState.endedThisFrame = true;
        }
    }

    int justEndedIdx = -1;
    double justEndedStart = 0.0;
    double justEndedEnd = 0.0;

    if (dragState.mode != BandBarDragState::Mode::None) {
        const int idx = dragState.dragIdx;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && idx >= 0 && idx < bandCount) {
            const double mouseTime = toTime(ImGui::GetIO().MousePos.x);
            const bool altHeld = ImGui::GetIO().KeyAlt;

            const float playheadX = toX(playheadTime);
            const double snappedTime =
                (std::abs(ImGui::GetIO().MousePos.x - playheadX) < kSnapPx) ? playheadTime : mouseTime;

            const double boundMin = (idx > 0) ? bands[idx - 1].endTime : minTime;
            const double boundMax = (idx < bandCount - 1) ? bands[idx + 1].startTime : maxTime;

            double newStart = dragState.previewStart;
            double newEnd = dragState.previewEnd;

            switch (dragState.mode) {
            case BandBarDragState::Mode::ResizeLeft:
                newStart = altHeld ? dragState.originalStart
                                   : std::clamp(snappedTime, boundMin, dragState.previewEnd - minDur);
                break;
            case BandBarDragState::Mode::ResizeRight:
                newEnd = altHeld ? dragState.originalEnd
                                 : std::clamp(snappedTime, dragState.previewStart + minDur, boundMax);
                break;
            case BandBarDragState::Mode::Move: {
                if (altHeld) {
                    newStart = dragState.originalStart;
                    newEnd = dragState.originalEnd;
                } else {
                    const double delta = mouseTime - dragState.dragTimeAtStart;
                    const double dur = dragState.originalEnd - dragState.originalStart;
                    newStart = std::clamp(dragState.originalStart + delta, boundMin, boundMax - dur);
                    newEnd = newStart + dur;
                }
                break;
            }
            default:
                break;
            }

            dragState.previewStart = newStart;
            dragState.previewEnd = newEnd;

            if (callbacks.onDragUpdate)
                callbacks.onDragUpdate(idx, newStart, newEnd);

            // Cursor feedback
            bool altHeldCursor = ImGui::GetIO().KeyAlt;
            if (dragState.mode == BandBarDragState::Mode::Move)
                ImGui::SetMouseCursor(altHeldCursor ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
            else
                ImGui::SetMouseCursor(altHeldCursor ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
        } else {
            justEndedIdx = dragState.dragIdx;
            justEndedStart = dragState.previewStart;
            justEndedEnd = dragState.previewEnd;
            if (callbacks.onDragEnd)
                callbacks.onDragEnd(dragState.dragIdx, dragState.previewStart, dragState.previewEnd);
            dragState.mode = BandBarDragState::Mode::None;
            dragState.dragIdx = -1;
            dragState.endedThisFrame = true;
        }
    }

    const bool isDragging = dragState.mode != BandBarDragState::Mode::None;

    // ── Phase 2: draw bands ──────────────────────────────────────────────────
    for (int i = 0; i < bandCount; ++i) {
        const double start = (i == justEndedIdx)        ? justEndedStart
                             : (i == dragState.dragIdx) ? dragState.previewStart
                                                        : bands[i].startTime;
        const double end = (i == justEndedIdx)        ? justEndedEnd
                           : (i == dragState.dragIdx) ? dragState.previewEnd
                                                      : bands[i].endTime;

        const float x0 = std::max(toX(start), barMin.x);
        const float x1 = std::min(toX(end), barMax.x);
        if (x1 - x0 < 2.0f)
            continue;

        const ImVec2 cMin = {x0, barMin.y + kChPad};
        const ImVec2 cMax = {x1, barMax.y - kChPad};

        const bool isActive =
            (isDragging && dragState.dragIdx == i) || (dragState.hasDragCandidate && dragState.candidateIdx == i);
        const bool hovered =
            !isDragging && !dragState.hasDragCandidate && barHovered && ImGui::IsMouseHoveringRect(cMin, cMax);

        ImU32 col = bands[i].color;
        if (isActive || hovered)
            col = ofs::ui::brighten(col, 0.15f);
        dl->AddRectFilled(cMin, cMax, col, 3.0f);

        // Diagonal hatch (region bands only): a brighter same-hue stripe pattern clipped to the band,
        // so a colored region reads as distinct from a same-colored solid chapter. The clip rect bounds
        // the work to the band's on-screen rect (already clamped to the bar), and each line is a thin
        // primitive written into the draw list's pre-grown buffers — no per-frame heap allocation.
        if (bands[i].hatched) {
            dl->PushClipRect(cMin, cMax, true);
            const ImU32 hatchCol = ofs::ui::brighten(bands[i].color, 0.22f);
            const float bandH = cMax.y - cMin.y;
            const float step = ImGui::GetFontSize() * 0.8f; // font-relative stripe pitch
            // 45° lines (bottom-left → top-right). Start one band-height left of the rect so the slanted
            // lines still cover the left edge after clipping.
            const float startX = cMin.x - bandH;
            const int lineCount = static_cast<int>((cMax.x - startX) / step) + 1;
            for (int k = 0; k < lineCount; ++k) {
                const float x = startX + static_cast<float>(k) * step;
                dl->AddLine({x, cMax.y}, {x + bandH, cMin.y}, hatchCol, 1.0f);
            }
            dl->PopClipRect();
        }

        if (x1 - x0 > 2.0f * kEdgeW) {
            dl->AddRectFilled(cMin, {x0 + kEdgeW, cMax.y}, ofs::theme::GetColorU32(AppCol_BandBarStripe), 3.0f);
            dl->AddRectFilled({x1 - kEdgeW, cMin.y}, cMax, ofs::theme::GetColorU32(AppCol_BandBarStripe));
        }

        if (!bands[i].name.empty()) {
            const float cy = (barMin.y + barMax.y) * 0.5f;
            const float avail = x1 - x0 - 6.f;
            ImVec2 ts = ImGui::CalcTextSize(bands[i].name.data(), bands[i].name.data() + bands[i].name.size());
            if (ts.x > 0.f && avail > 0.f) {
                // Elide rather than fit-or-drop: a zoomed-out band must still show an identifying prefix
                // (the draw-list "never fit-or-drop" rule). Only allocate the … copy when it overflows.
                std::string_view shown = bands[i].name;
                if (ts.x > avail) {
                    shown = ofs::ui::elide(fmtScratch("{}", bands[i].name), avail);
                    ts = ImGui::CalcTextSize(shown.data(), shown.data() + shown.size());
                }
                ofs::ui::addTextShadow(dl, {(x0 + x1 - ts.x) * 0.5f, cy - ts.y * 0.5f},
                                       ofs::theme::GetColorU32(AppCol_BandBarText), shown);
            }
        }

        // Selection highlight, drawn last so it sits above the fill, hatch and label.
        if (bands[i].selected)
            dl->AddRect(cMin, cMax, ofs::theme::GetColorU32(AppCol_SelectedLine), 3.0f, 2.0f);
    }

    // ── Phase 3: hover + click interaction ───────────────────────────────────
    // Each band is a real ImGui item (InvisibleButton): ImGui owns the hit test and
    // active/hover arbitration, so the bands cooperate with overlapping widgets and are
    // individually test-addressable. Edge-vs-body is a position classification *within*
    // the hovered item (reading the cursor, not a second hit test). The cross-frame drag
    // mechanics still live in the dragState machine (Phases 1–2).
    if (isDragging || dragState.hasDragCandidate || suppressNewInteraction)
        return;

    const float mouseX = ImGui::GetIO().MousePos.x;
    bool anyBandHovered = false;
    bool tooltipShown = false;

    for (int i = 0; i < bandCount; ++i) {
        const float x0 = std::max(toX(bands[i].startTime), barMin.x);
        const float x1 = std::min(toX(bands[i].endTime), barMax.x);
        if (x1 - x0 < 2.0f)
            continue;

        const ImVec2 cMin = {x0, barMin.y + kChPad};
        const ImVec2 cMax = {x1, barMax.y - kChPad};

        // ItemAdd + ButtonBehavior places an interactive item at an absolute rect without
        // touching the layout cursor (InvisibleButton would, and asserts when growing the
        // bar's boundary). ImGui owns the hit test, active/hover arbitration, and a stable
        // per-band ID for tests.
        const ImRect bb(cMin, cMax);
        ImGui::PushID(i);
        const ImGuiID bandId = ImGui::GetID("##band");
        ImGui::PopID();
        ImGui::ItemAdd(bb, bandId);
        bool hovered = false, held = false;
        ImGui::ButtonBehavior(bb, bandId, &hovered, &held,
                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (!hovered)
            continue;

        anyBandHovered = true;

        const bool canEdge = (x1 - x0 > 2.0f * kEdgeW);
        const bool hovEdgeL = canEdge && mouseX <= x0 + kEdgeW;
        const bool hovEdgeR = canEdge && mouseX >= x1 - kEdgeW;

        ImGui::SetMouseCursor((hovEdgeL || hovEdgeR) ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);

        if (!tooltipShown) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted((hovEdgeL || hovEdgeR) ? Str::BandDragResize.c_str() : Str::BandDragMove.c_str());
            ImGui::Text("%s \xe2\x80\x93 %s", ofs::TimeUtil::formatTime(bands[i].startTime, true),
                        ofs::TimeUtil::formatTime(bands[i].endTime, true));
            ImGui::EndTooltip();
            tooltipShown = true;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const BandBarDragState::Mode mode = hovEdgeL   ? BandBarDragState::Mode::ResizeLeft
                                                : hovEdgeR ? BandBarDragState::Mode::ResizeRight
                                                           : BandBarDragState::Mode::Move;
            dragState.hasDragCandidate = true;
            dragState.candidateIdx = i;
            dragState.candidateMode = mode;
            dragState.candidateStart = bands[i].startTime;
            dragState.candidateEnd = bands[i].endTime;
            dragState.candidateDragTime = toTime(ImGui::GetIO().MousePos.x);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            if (callbacks.onRightClick)
                callbacks.onRightClick(i, toTime(ImGui::GetIO().MousePos.x));
        }
        break; // bands don't overlap — at most one is hovered
    }

    // Empty-area fallback (gaps between bands): a background canvas interaction, not a
    // discrete element, so it stays a geometric check gated on "no band hovered".
    if (!anyBandHovered && barHovered) {
        if (!tooltipShown) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", ofs::TimeUtil::formatTime(toTime(ImGui::GetIO().MousePos.x), true));
            ImGui::EndTooltip();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            if (callbacks.onEmptyRightClick)
                callbacks.onEmptyRightClick(toTime(ImGui::GetIO().MousePos.x));
        }
    }
}

} // namespace ofs::ui
