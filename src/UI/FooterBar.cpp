#include "UI/FooterBar.h"
#include "Core/EventQueue.h"
#include "Core/IntentEvents.h" // SetActiveEditModeEvent / SetActiveNavigatorEvent / SetActiveSelectionModeEvent (selector write path)
#include "Core/StandardAxis.h"      // standardAxisShortName / standardAxisColor (active-axis zone lookups)
#include "Core/TaskEvents.h"        // CancelTaskEvent (task abort button)
#include "Core/TranscodeEvents.h"   // OpenTranscodeDialogEvent (original-source badge click → optimize dialog)
#include "Localization/AxisNames.h" // localizedAxisName (axis hover tooltip)
#include "Localization/Translator.h"
#include "UI/AxisColors.h" // standardAxisColor active-axis zone lookups
#include "UI/Icons.h"
#include "UI/Theme.h"            // ofs::theme::GetColorU32 + AppCol semantic status slots
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/TimeUtil.h"
#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar — not part of the public imgui.h API.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace ofs::ui {
namespace {

// A crisp vertical hairline between zones, consuming a 1px-wide item and keeping the line flowing.
void zoneSep(float h) {
    ImGui::SameLine(0.0f, 8.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = h * 0.18f;
    dl->AddLine(ImVec2(p.x, p.y + pad), ImVec2(p.x, p.y + h - pad), ofs::theme::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, 8.0f);
}

// Filled status dot (active-axis tint), consuming a square item of side `d`.
void dot(float d, ImU32 col) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + d * 0.5f, p.y + d * 0.5f), d * 0.28f, col);
    ImGui::Dummy(ImVec2(d, d));
}

// Rotating-arc spinner centered at `c`. Open arc (flags default to 0, not Closed) so the gap reads as
// motion. Uses ImGui time (not wall-clock) so the renderer stays free of any platform clock.
void drawSpinner(ImDrawList *dl, ImVec2 c, float radius, float thickness, ImU32 col) {
    const float a0 = static_cast<float>(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(c, radius, a0, a0 + IM_PI * 1.5f, 16);
    dl->PathStroke(col, thickness);
}

// Spinner consuming a square layout item of side `h`.
void spinner(float h, ImU32 col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    drawSpinner(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.30f, 1.6f, col);
    ImGui::Dummy(ImVec2(h, h));
}

// Per-level glyph + tint for a notification row and the bell badge.
struct LevelStyle {
    const char *icon;
    ImU32 color;
};
LevelStyle levelStyle(ofs::NotifyLevel level) {
    switch (level) {
    case ofs::NotifyLevel::Success:
        return {.icon = ICON_CHECK, .color = ofs::theme::GetColorU32(AppCol_Success)};
    case ofs::NotifyLevel::Warning:
        return {.icon = ICON_ALERT_TRIANGLE, .color = ofs::theme::GetColorU32(AppCol_Warning)};
    case ofs::NotifyLevel::Error:
        return {.icon = ICON_ALERT_CIRCLE, .color = ofs::theme::GetColorU32(AppCol_Error)};
    case ofs::NotifyLevel::Info:
    default:
        return {.icon = ICON_BADGE_INFO, .color = ofs::theme::GetColorU32(ImGuiCol_Text)};
    }
}

// Right-aligned app-status cluster, parked just left of the bell and mirroring renderBell's right-edge
// math so the two never overlap. Ordered left→right: background-eval spinner, eval-worker count, managed
// (.NET) heap, then the loop-status icon (moon = idle-throttled, bolt = full rate) + UI frame rate. Each
// leading readout is hidden when it has nothing to show (no eval in flight / no workers / no CLR loaded).
void renderAppStatus(const FooterBarInfo &info) {
    const char *icon = info.idle ? ICON_MOON : ICON_ZAP;
    // Space-pad the rate to 3 digits so the cluster width holds steady as it crosses 2↔3 digits.
    const char *fpsTxt = fmtScratch("{:3.0f} fps", info.appFps);
    const float gap = 6.0f;
    const float lineH = ImGui::GetTextLineHeight(); // spinner side

    // Background evaluation: spinner + "Evaluating L0 (+N)" — leads the cluster. Measure the actual
    // (translated) label; the frame-arena pointer stays valid through render. Bakes a trailing gap.
    const bool hasEval = info.evaluatingCount > 0;
    const char *evalTxt = "";
    float evalW = 0.0f;
    if (hasEval) {
        evalTxt = info.evaluatingCount > 1
                      ? Str::FtEvaluatingMore.fmt(info.evaluatingAxisName, info.evaluatingCount - 1)
                      : Str::FtEvaluating.fmt(info.evaluatingAxisName);
        evalW = lineH + gap + ImGui::CalcTextSize(evalTxt).x + 12.0f;
    }

    // Background eval workers: cpu icon + count while any run; a stuck worker (oldest past the threshold)
    // appends a warning triangle. Like the heap read-out, it bakes a trailing gap into its width.
    constexpr double kStuckWorkerSeconds = 60.0;
    const bool hasWorkers = info.runningWorkers > 0;
    const bool workerStuck = info.oldestWorkerSeconds > kStuckWorkerSeconds;
    const char *workerTxt = "";
    float workerW = 0.0f;
    if (hasWorkers) {
        workerTxt = workerStuck ? fmtScratch("{} {} {}", ICON_CPU, info.runningWorkers, ICON_ALERT_TRIANGLE)
                                : fmtScratch("{} {}", ICON_CPU, info.runningWorkers);
        workerW = ImGui::CalcTextSize(workerTxt).x + 12.0f;
    }

    // Managed (.NET) heap: pure icon + "N MB" (unit literal, px-rule), the tooltip carries the meaning.
    const bool hasHeap = info.managedHeapBytes > 0;
    const char *heapTxt = "";
    float heapW = 0.0f;
    if (hasHeap) {
        constexpr double kMib = 1024.0 * 1024.0;
        heapTxt = fmtScratch("{} {:.1f} MB", ICON_MEMORY_STICK, static_cast<double>(info.managedHeapBytes) / kMib);
        heapW = ImGui::CalcTextSize(heapTxt).x + 12.0f;
    }

    const float clusterW = evalW + workerW + heapW + ImGui::CalcTextSize(icon).x + gap + ImGui::CalcTextSize(fpsTxt).x;

    // Bell zone width per renderBell (glyph + 4px pad each side); leave a gap before it.
    const float bellZoneW = ImGui::CalcTextSize(ICON_BELL).x + 8.0f;
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(std::max(0.0f, rightEdge - bellZoneW - 12.0f - clusterW));

    if (hasEval) {
        spinner(lineH, ofs::theme::GetColorU32(ImGuiCol_CheckMark));
        ImGui::SameLine(0.0f, gap);
        ImGui::TextUnformatted(evalTxt);
        ImGui::SameLine(0.0f, 12.0f);
    }

    if (hasWorkers) {
        ImGui::TextDisabled("%s", fmtScratch("{} {}", ICON_CPU, info.runningWorkers));
        ImGui::SetItemTooltip("%s", Str::FtWorkersTip.c_str());
        if (workerStuck) {
            ImGui::SameLine(0.0f, gap);
            ImGui::PushStyleColor(ImGuiCol_Text, ofs::theme::GetColorU32(AppCol_Warning));
            ImGui::TextUnformatted(ICON_ALERT_TRIANGLE);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", Str::FtWorkerStuckTip.c_str());
        }
        ImGui::SameLine(0.0f, 12.0f);
    }

    if (hasHeap) {
        ImGui::TextDisabled("%s", heapTxt);
        ImGui::SetItemTooltip("%s", Str::FtManagedHeapTip.c_str());
        ImGui::SameLine(0.0f, 12.0f);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, info.idle ? ofs::theme::GetColorU32(ImGuiCol_TextDisabled)
                                                   : ofs::theme::GetColorU32(ImGuiCol_CheckMark));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", info.idle ? Str::FtIdleTip.c_str() : Str::FtActiveTip.c_str());
    ImGui::SameLine(0.0f, gap);
    ImGui::TextDisabled("%s", fpsTxt);
}

// Compact footer dropdown: a dim leading `icon` plus a "current ▾" SmallButton that opens a popup of
// options. A SmallButton (frame-padding.y == 0) is exactly one text line tall, so it fits the
// single-line bar where a full Combo (frame-height tall) would overflow. The icon stands in for a text
// label to keep the bar dense; `tooltip` (the localized name) names it on hover so the meaning stays
// discoverable and localized. Returns the id the user just picked (different from `activeId`), else
// nullptr; the caller turns that into the set-active event. `idtag` gives the button/popup a stable,
// language-independent ###id; each option's stable registry id backs its Selectable id, so a
// translated label never changes widget identity (test-stable).
const char *footerSelect(const char *idtag, const char *icon, const char *tooltip, const FooterSelectOption *opts,
                         int count, const char *activeId, EventQueue &eq, ToolOptionTarget optionsTarget) {
    if (count <= 0)
        return nullptr;

    const char *activeLabel = opts[0].label; // fall back to the first if the active id is unknown
    bool activeHasUi = opts[0].hasUi;
    for (int i = 0; i < count; ++i)
        if (std::strcmp(opts[i].id, activeId) == 0) {
            activeLabel = opts[i].label;
            activeHasUi = opts[i].hasUi;
        }

    ImGui::TextDisabled("%s", icon);
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(fmtScratch("{} {}###{}", activeLabel, ICON_CHEVRON_DOWN, idtag)))
        ImGui::OpenPopup(fmtScratch("##{}_popup", idtag));

    // The active mode carries options → a sliders glyph beside it that opens this mode's options as a
    // centered click-away modal (the host owns the surface; a footer-anchored popover clipped and jumped
    // against the bar's bottom edge). This is the link from the activation point to the options.
    if (activeHasUi) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton(fmtScratch("{}###{}_opts", ICON_SLIDERS_HORIZONTAL, idtag)))
            eq.push(OpenToolOptionsEvent{.target = optionsTarget});
        ImGui::SetItemTooltip("%s", Str::FtToolOptions.c_str());
    }

    const char *picked = nullptr;
    if (ImGui::BeginPopup(fmtScratch("##{}_popup", idtag))) {
        for (int i = 0; i < count; ++i) {
            const bool isActive = std::strcmp(opts[i].id, activeId) == 0;
            if (ImGui::Selectable(fmtScratch("{}###{}", opts[i].label, opts[i].id), isActive) && !isActive)
                picked = opts[i].id;
            // Attribute a plugin-provided entry: its owning plugin's name, dim, on the same row. A
            // default Selectable advances the layout cursor by the label width (its full-width highlight
            // is visual only), so SameLine lands the source right after the label, inside the row.
            if (opts[i].source[0] != '\0') {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextDisabled("%s", opts[i].source);
            }
        }
        ImGui::EndPopup();
    }
    return picked;
}

// One task's progress bar. Empty overlay both ways: the percentage is noise here, and a negative fraction
// (indeterminate) would otherwise format a nonsensical value — < 0 makes ImGui animate an indeterminate bar.
void taskProgressBar(const TaskItem &item, float height) {
    if (item.progress < 0.0f)
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, height), "");
    else
        ImGui::ProgressBar(item.progress, ImVec2(-FLT_MIN, height), "");
}

const char *kNotifPopupId = "##notifications";

// Renders the bell glyph in the far-right of the bar plus its unread badge and a running-task spinner,
// and the popup (opening upward from the bell). Mutates `state`: clicking the bell opens the popup and
// clears `unread`; "Clear all" empties the log; the popup's task rows toggle TaskItem::hidden and push
// CancelTaskEvent. Returns nothing — the bell owns its own popup, like the palette.
void renderBell(NotificationState &state, EventQueue &eq, float lineH) {
    const char *bell = ICON_BELL;
    const float pad = 4.0f;
    const float fs = ImGui::GetFontSize();

    // Count tasks minimized into the bell: they drive a spinner cue (so the user can find them) and a
    // section in the popup. Visible (floating) tasks render their own panel and don't appear here.
    int hiddenTasks = 0;
    for (const TaskItem &t : state.tasks)
        if (t.hidden)
            ++hiddenTasks;

    const float bellW = ImGui::CalcTextSize(bell).x;
    const float zoneW = bellW + pad * 2.0f;
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;

    ImGui::SameLine(std::max(0.0f, rightEdge - zoneW));
    const ImVec2 tl = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##bell", ImVec2(zoneW, lineH));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float bellX = tl.x + pad;
    const bool active = state.unread > 0 || hiddenTasks > 0 || hovered;
    const ImU32 bellCol =
        active ? ofs::theme::GetColorU32(ImGuiCol_Text) : ofs::theme::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddText(ImVec2(bellX, tl.y), bellCol, bell);

    // Running-task cue: an animated spinner ringing the bell whenever work is minimized into it.
    if (hiddenTasks > 0)
        drawSpinner(dl, ImVec2(bellX + bellW * 0.5f, tl.y + lineH * 0.5f), fs * 0.62f, fs * 0.11f,
                    ofs::theme::GetColorU32(ImGuiCol_CheckMark));

    // Unread badge: a small accent dot at the bell glyph's top-right, VSCode-style, capped at "9+".
    // Every metric scales off GetFontSize() (font/DPI-safe). The disc is a FIXED size regardless of
    // the count — sized off the glyph cap height, never the text width — so "9+" matches a single digit.
    if (state.unread > 0) {
        const char *count = state.unread > 9 ? "9+" : fmtScratch("{}", state.unread);
        const float badgeFont = fs * 0.74f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(badgeFont, FLT_MAX, 0.0f, count);
        const float r = badgeFont * 0.62f; // fixed radius; cap height + a little pad
        // Hug the bell glyph's top-right corner.
        const ImVec2 c(bellX + bellW - r * 0.2f, tl.y + r * 0.35f);
        // WindowBg text on the accent fill: accents are designed to pop on the window, so this pair
        // stays legible in both light and dark schemes without a hardcoded on-accent color.
        dl->AddCircleFilled(c, r, ofs::theme::GetColorU32(ImGuiCol_CheckMark));
        dl->AddText(ImGui::GetFont(), badgeFont, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    ofs::theme::GetColorU32(ImGuiCol_WindowBg), count);
    }

    if (clicked) {
        ImGui::OpenPopup(kNotifPopupId);
        state.unread = 0;
    }

    // Publish the bell's top-right corner so renderToasts() (a later pass) can stack above it.
    state.bellAnchorX = tl.x + zoneW;
    state.bellAnchorY = tl.y;

    // Anchor the popup so its bottom-right corner sits at the bell's top-right: it grows up and left
    // out of the bar instead of off the bottom of the screen.
    ImGui::SetNextWindowPos(ImVec2(tl.x + zoneW, tl.y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 18.75f, 0.0f), ImVec2(fs * 26.25f, fs * 22.5f));
    if (!ImGui::BeginPopup(kNotifPopupId))
        return;

    state.panelOpen = true; // suppress toasts while the full list is open
    state.unread = 0;       // stay cleared while the panel is open

    // Minimized running tasks sit at the top of the popup: each shows its progress, a Show button to pop
    // it back out as a floating panel, and a Cancel button. They live above the log, not in it.
    if (hiddenTasks > 0) {
        ImGui::SeparatorText(Str::FtRunningTasks);
        const float padX = ImGui::GetStyle().FramePadding.x;
        for (TaskItem &t : state.tasks) {
            if (!t.hidden)
                continue;
            ImGui::PushID(static_cast<int>(t.id));
            ImGui::TextUnformatted(t.label.c_str());
            const float showW = ImGui::CalcTextSize(ICON_CHEVRON_UP).x + padX * 2.0f;
            const float cancelW = t.cancellable ? ImGui::CalcTextSize(ICON_CIRCLE_STOP).x + padX * 2.0f : 0.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - showW -
                                 (t.cancellable ? spacing + cancelW : 0.0f));
            if (ImGui::SmallButton(ICON_CHEVRON_UP "###taskshow")) {
                t.hidden = false;
                ImGui::CloseCurrentPopup(); // the task reappears as a floating panel; dismiss the bell
            }
            ImGui::SetItemTooltip("%s", Str::TaskShow.c_str());
            if (t.cancellable) {
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_CIRCLE_STOP "###taskcancel"))
                    eq.push(CancelTaskEvent{.id = t.id});
                ImGui::SetItemTooltip("%s", Str::TaskCancel.c_str());
            }
            taskProgressBar(t, ImGui::GetTextLineHeight() * 0.5f);
            if (!t.detail.empty())
                ImGui::TextDisabled("%s", t.detail.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    ImGui::TextUnformatted(Str::FtNotifications);
    ImGui::SameLine();
    // Reserve the *translated* button label width (not the English literal) so the right-aligned
    // "Clear all" never overruns the panel's right edge.
    const char *clearLabel = Str::FtClearAll;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(clearLabel).x -
                         ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::BeginDisabled(state.log.empty());
    if (ImGui::SmallButton(Str::FtClearAll.id("clearall")))
        state.clear();
    ImGui::EndDisabled();
    ImGui::Separator();

    const size_t n = state.log.size();
    if (n == 0) {
        ImGui::TextDisabled("%s", Str::FtNoNotifications.c_str());
    } else {
        ImGui::BeginChild("##notiflist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        for (size_t k = n; k-- > 0;) { // newest first
            const NotificationItem &item = state.log[k];
            const LevelStyle ls = levelStyle(item.level);
            ImGui::PushStyleColor(ImGuiCol_Text, ls.color);
            ImGui::TextUnformatted(ls.icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextWrapped("%s", item.text.c_str());
        }
        ImGui::EndChild();
    }
    ImGui::EndPopup();
}

// Toast tuning (seconds). A toast fades in, holds, then fades out; hovering pauses it; clicking
// dismisses it. Errors hold longer since they matter more and are easy to miss.
constexpr double kToastFade = 0.3;      // fade-in / fade-out, carved out of the visible window
constexpr double kToastLife = 3.0;      // total seconds a toast is visible
constexpr double kToastLifeError = 6.0; // errors linger longer — more important and easier to miss
constexpr int kMaxToasts = 3;

double toastLife(ofs::NotifyLevel level) {
    return level == ofs::NotifyLevel::Error ? kToastLifeError : kToastLife;
}

// One toast panel: accent bar + level glyph + wrapped text, faded by `alpha`. Returns true while
// hovered so the caller can pause its timer.
bool renderOneToast(ToastItem &item, float alpha) {
    const LevelStyle ls = levelStyle(item.level);
    // Font-relative width so the toast tracks DPI; the wrap column and side indent follow from it.
    const float toastWidth = ImGui::GetFontSize() * 20.0f;
    const float sideIndent = ImGui::GetFontSize() * 0.6f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    // Tight vertical inner padding so a one-line toast is a compact bar rather than a tall block; keep the
    // theme's horizontal padding. This replaces the old Dummy(0,1) top/bottom spacers.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetFontSize() * 0.2f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ofs::theme::GetColorU32(ImGuiCol_PopupBg));
    ImGui::BeginChild(fmtScratch("##toast{}", static_cast<const void *>(&item)), ImVec2(toastWidth, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 mn = ImGui::GetWindowPos();
    const ImVec2 mx = mn + ImGui::GetWindowSize();
    dl->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y), ls.color); // left accent stripe

    ImGui::Indent(sideIndent);
    ImGui::PushStyleColor(ImGuiCol_Text, ls.color);
    ImGui::TextUnformatted(ls.icon);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushTextWrapPos(toastWidth - ImGui::GetFontSize() * 1.1f);
    ImGui::TextUnformatted(item.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Unindent(sideIndent);

    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return hovered;
}

} // namespace

void renderToasts(NotificationState &state) {
    const double now = ImGui::GetTime();

    // Stamp every pending toast on first sight so its timer starts, then drop the expired ones. This
    // bounds state.toasts to whatever was pushed within the last toastLife() seconds.
    for (auto &t : state.toasts)
        if (t.shownAt < 0.0)
            t.shownAt = now;
    std::erase_if(state.toasts, [&](const ToastItem &t) { return now - t.shownAt >= toastLife(t.level); });

    if (state.toasts.empty() || state.panelOpen)
        return;

    // Draw the newest kMaxToasts (a burst's overflow simply ages out without a toast — it isn't lost
    // from the bell, which keeps Warning/Error independently).
    const int n = static_cast<int>(state.toasts.size());
    size_t shown[kMaxToasts];
    float alpha[kMaxToasts];
    int shownN = 0;
    for (int k = n - 1; k >= 0 && shownN < kMaxToasts; --k) {
        const ToastItem &item = state.toasts[static_cast<size_t>(k)];
        const double age = now - item.shownAt;
        const double life = toastLife(item.level);
        float a = 1.0f;
        if (age < kToastFade)
            a = static_cast<float>(age / kToastFade);
        else if (age > life - kToastFade)
            a = static_cast<float>((life - age) / kToastFade);
        shown[shownN] = static_cast<size_t>(k);
        alpha[shownN] = a;
        ++shownN;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    // Bottom-right pinned just above the bell (and above the task stack, if any); grows up as toasts stack.
    ImGui::SetNextWindowPos(ImVec2(state.bellAnchorX, state.bellAnchorY - state.taskStackHeight - 6.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    // Transparent, border-less shell — each toast paints its own panel + border; without the border
    // override the theme's WindowBorderSize would draw a second box around the whole stack.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##toaststack", nullptr, flags)) {
        // Render oldest-first so the newest sits at the bottom, nearest the bell.
        for (int i = shownN - 1; i >= 0; --i) {
            ToastItem &item = state.toasts[shown[i]];
            if (renderOneToast(item, alpha[i])) {
                item.shownAt = now; // pause while hovered
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    item.shownAt = now - toastLife(item.level); // click dismisses
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void renderTasks(NotificationState &state, EventQueue &eq) {
    int visible = 0;
    for (const TaskItem &t : state.tasks)
        if (!t.hidden)
            ++visible;
    if (visible == 0) { // all tasks minimized into the bell (or none) → no floating stack
        state.taskStackHeight = 0.0f;
        return;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    // Pinned bottom-right just above the bell; the window grows upward as tasks stack. renderToasts then
    // floats its own stack above this one using the taskStackHeight published below.
    ImGui::SetNextWindowPos(ImVec2(state.bellAnchorX, state.bellAnchorY - 6.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    // Transparent, border-less shell — each task paints its own panel + border; the border override stops
    // the theme's WindowBorderSize from drawing a second box around the whole stack.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##taskstack", nullptr, flags)) {
        const float panelW = ImGui::GetFontSize() * 20.0f;
        const float sideIndent = ImGui::GetFontSize() * 0.6f;
        const float barH = ImGui::GetTextLineHeight() * 0.6f;
        const float padX = ImGui::GetStyle().FramePadding.x;
        bool first = true;
        for (TaskItem &item : state.tasks) {
            if (item.hidden)
                continue; // minimized into the bell — rendered there, not as a floating panel
            if (!first)
                ImGui::Spacing();
            first = false;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ofs::theme::GetColorU32(ImGuiCol_PopupBg));
            // Symmetric inner padding via WindowPadding (child has Borders, so it's honored) — a left-only
            // Indent would leave the right edge flush against the border.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(sideIndent, ImGui::GetStyle().FramePadding.y + 2.0f));
            ImGui::BeginChild(fmtScratch("##task{}", item.id), ImVec2(panelW, 0.0f),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 mn = ImGui::GetWindowPos();
            const ImVec2 mx = mn + ImGui::GetWindowSize();
            dl->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y), ofs::theme::GetColorU32(ImGuiCol_CheckMark)); // accent

            ImGui::TextUnformatted(item.label.c_str());

            // Right-aligned icon controls: minimize (collapses into the bell, work keeps running) and a
            // stop button to abort. A stop glyph, not an ✕, since ✕ reads as "dismiss the notice".
            const float hideW = ImGui::CalcTextSize(ICON_CHEVRON_DOWN).x + padX * 2.0f;
            const float cancelW = item.cancellable ? ImGui::CalcTextSize(ICON_CIRCLE_STOP).x + padX * 2.0f : 0.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - hideW -
                                 (item.cancellable ? spacing + cancelW : 0.0f));
            if (ImGui::SmallButton(fmtScratch("{}###taskhide{}", ICON_CHEVRON_DOWN, item.id)))
                item.hidden = true;
            ImGui::SetItemTooltip("%s", Str::TaskHide.c_str());
            if (item.cancellable) {
                ImGui::SameLine();
                if (ImGui::SmallButton(fmtScratch("{}###taskcancel{}", ICON_CIRCLE_STOP, item.id)))
                    eq.push(CancelTaskEvent{.id = item.id});
                ImGui::SetItemTooltip("%s", Str::TaskCancel.c_str());
            }

            taskProgressBar(item, barH);
            if (!item.detail.empty())
                ImGui::TextDisabled("%s", item.detail.c_str());

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        // +6 leaves a gap so renderToasts' stack clears the task window's own gap above the bell.
        state.taskStackHeight = ImGui::GetWindowSize().y + 6.0f;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

float renderFooterBar(const FooterBarInfo &info, NotificationState &notifications, EventQueue &eq) {
    notifications.panelOpen = false; // re-latched by renderBell when the popup is open this frame
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float barHeight = ImGui::GetFrameHeight();
    const float lineH = ImGui::GetTextLineHeight();
    // Center the single text line in the bar (== FramePadding.y here, but compute it so the bar
    // stays centered if the height is ever decoupled from the frame height).
    const float padY = std::max(0.0f, (barHeight - lineH) * 0.5f);

    // NoNavInputs: the footer is mouse-driven status chrome. Without it, keyboard/gamepad directional
    // nav can step into the footer's selectors and bell, swallowing the arrow keys the timeline needs.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ofs::theme::GetColorU32(ImGuiCol_TitleBgActive));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, padY));
    const bool open = ImGui::BeginViewportSideBar("##AppFooterBar", viewport, ImGuiDir_Down, barHeight, flags);
    ImGui::PopStyleVar();

    if (open && info.idleMessage[0] != '\0') {
        // No project: a single status line stands in for the transport/axis/eval zones; app status + bell follow.
        ImGui::TextDisabled("%s", info.idleMessage);
        renderAppStatus(info);
        renderBell(notifications, eq, lineH);
    } else if (open) {
        const ImU32 dimCol = ofs::theme::GetColorU32(ImGuiCol_TextDisabled);

        // Emit a zone separator only when something already precedes it, so the first visible zone
        // never has a leading divider (e.g. when there is no media and the fps zone is hidden).
        bool any = false;
        auto lead = [&]() {
            if (any)
                zoneSep(lineH);
            any = true;
        };

        // ── Active media source badge (optimized intra copy vs original) ───────────────────────────
        if (info.mediaSource >= 0) {
            lead();
            const bool intra = info.mediaSource == 1;
            if (!intra && info.canOptimize) {
                // The original badge is a click target that opens the optimize dialog. Rendered as an
                // InvisibleButton + draw-list glyph (like the bell) so it carries a stable ###id and reads
                // as interactive — dim at rest, brightening on hover.
                const char *glyph = ICON_FILM;
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("###ft_source_optimize", ImGui::CalcTextSize(glyph)))
                    eq.push(OpenTranscodeDialogEvent{});
                const bool hovered = ImGui::IsItemHovered();
                if (hovered)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::GetWindowDrawList()->AddText(pos, hovered ? ofs::theme::GetColorU32(ImGuiCol_Text) : dimCol,
                                                    glyph);
                ImGui::SetItemTooltip("%s", Str::FtSourceOriginalClickTip.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, intra ? ofs::theme::GetColorU32(AppCol_Success) : dimCol);
                // Icon only — the source is conveyed by the glyph (and the tooltip), no inline label.
                ImGui::TextUnformatted(intra ? ICON_GAUGE : ICON_FILM);
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s",
                                      intra ? Str::FtSourceOptimizedTip.c_str() : Str::FtSourceOriginalTip.c_str());
            }
        }

        // ── Transport stats (fps + speed) ────────────────────────────────────────────────────────
        if (info.fps > 0.0) {
            lead();
            ImGui::TextDisabled("%s", fmtScratch("{:.0f} fps", info.fps));
            if (std::fabs(info.playbackSpeed - 1.0f) > 0.001f) {
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextDisabled("%s", fmtScratch("{:.2f}x", info.playbackSpeed));
            }
        }

        // ── Active axis + selection ──────────────────────────────────────────────────────────────
        lead();
        const ImU32 axisCol = ofs::standardAxisColor(info.activeAxis);
        dot(lineH, axisCol != 0 ? axisCol : dimCol);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextUnformatted(
            Str::FtAxisPoints.fmt(ofs::standardAxisShortName(info.activeAxis), info.activeActionCount));
        ImGui::SetItemTooltip("%s", ofs::loc::localizedAxisName(info.activeAxis)); // localized full name on hover
        if (info.selectionCount > 0) {
            ImGui::SameLine(0.0f, 8.0f);
            if (info.selectionCount >= 2 && info.selectionSpan > 0.0) {
                // formatTimeShort returns frame-arena storage; bind it so .fmt() gets a named lvalue.
                const char *span = TimeUtil::formatTimeShort(info.selectionSpan);
                ImGui::TextDisabled("%s", Str::FtSelectionSpan.fmt(info.selectionCount, span));
            } else {
                ImGui::TextDisabled("%s", Str::FtSelection.fmt(info.selectionCount));
            }
        }

        // ── View window (timeline zoom span) ─────────────────────────────────────────────────────
        lead();
        // Icon + duration only — the eye glyph stands in for the "view" word; its tooltip names the zone.
        const char *viewSpan = fmtScratch("{:.1f}s", info.visibleTime);
        ImGui::TextDisabled("%s", ICON_EYE);
        ImGui::SetItemTooltip("%s", Str::FtView.c_str());
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("%s", viewSpan);
        // Pad the trailing gap to the widest value's width — the zoom tops out at 300.0s — so the zones
        // after this one stay put as the digit count changes ("5.0s" → "300.0s"). Off the text, not a px
        // literal.
        const float viewPad = ImGui::CalcTextSize(fmtScratch("{:.1f}s", 300.0)).x - ImGui::CalcTextSize(viewSpan).x;
        if (viewPad > 0.0f) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::Dummy(ImVec2(viewPad, lineH));
        }

        // ── Interaction extension points: edit mode + step navigator + selection selectors ───────
        if (info.editModeCount > 0 || info.navigatorCount > 0 || info.selectionModeCount > 0) {
            lead();
            bool prev = false; // emit an inter-selector gap only after the previous one rendered
            auto gap = [&]() {
                if (prev)
                    ImGui::SameLine(0.0f, 12.0f);
                prev = true;
            };
            if (info.editModeCount > 0) {
                gap();
                if (const char *pick =
                        footerSelect("ft_editmode", ICON_SPARKLE, Str::FtEditMode, info.editModes, info.editModeCount,
                                     info.activeEditModeId, eq, ToolOptionTarget::Edit))
                    eq.push(SetActiveEditModeEvent{.id = pick});
            }
            if (info.navigatorCount > 0) {
                gap();
                if (const char *pick =
                        footerSelect("ft_step", ICON_FOOTPRINTS, Str::FtStep, info.navigators, info.navigatorCount,
                                     info.activeNavigatorId, eq, ToolOptionTarget::Navigator))
                    eq.push(SetActiveNavigatorEvent{.id = pick});
            }
            if (info.selectionModeCount > 0) {
                gap();
                if (const char *pick = footerSelect("ft_select", ICON_BOX_SELECT, Str::FtSelect, info.selectionModes,
                                                    info.selectionModeCount, info.activeSelectionModeId, eq,
                                                    ToolOptionTarget::Selection))
                    eq.push(SetActiveSelectionModeEvent{.id = pick});
            }
        }

        // ── Undo history (stack depth) ───────────────────────────────────────────────────────────
        // Icons + counts only — no words; the undo/redo glyphs carry the meaning. The history memory
        // (used / limit) lives in the hover tooltip rather than cluttering the bar. The tooltip is pure
        // icon + numbers + unit literal (px-rule), so it needs no translatable text.
        if (info.undoMaxBytes > 0) {
            lead();
            constexpr double kMib = 1024.0 * 1024.0;
            const double usedMib = static_cast<double>(info.undoUsedBytes) / kMib;
            const double maxMib = static_cast<double>(info.undoMaxBytes) / kMib;
            ImGui::TextDisabled("%s", fmtScratch("{} {} {} {}", ICON_UNDO, info.undoSteps, ICON_REDO, info.redoSteps));
            ImGui::SetItemTooltip("%s", fmtScratch("{} {:.1f} / {:.0f} MB", ICON_DATABASE, usedMib, maxMib));
        }

        // ── Right zone: eval spinner + worker count + managed heap + loop status (idle icon + UI fps),
        // then the notification bell ─
        renderAppStatus(info);
        renderBell(notifications, eq, lineH);
    }
    ImGui::End();
    ImGui::PopStyleColor();

    return barHeight;
}

} // namespace ofs::ui
