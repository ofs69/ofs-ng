#include "UI/TitleBar.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"     // elide — draw-list search label has no auto-clip
#include "UI/Theme.h"            // ofs::theme::GetColorU32 — themed color lookups
#include "Util/FrameAllocator.h" // fmtScratch, allocArray — palette builds frame-scratch strings/arrays
#include "Util/FuzzyMatch.h"     // shared UTF-8 fuzzy matcher (replaces the old ASCII-only local one)
#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar, ImFloor — not part of the public imgui.h API.
#include "imgui_stdlib.h"   // std::string-bound InputTextWithHint

#include <algorithm>
#include <cfloat>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep std::max/std::min usable; windows.h otherwise defines max/min macros.
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

namespace ofs::ui {
namespace {

// Height of the native window title bar, expressed in ImGui coordinates (logical points). Windows
// reports caption metrics in physical pixels; io.DisplayFramebufferScale converts pixels -> points,
// which is the space SetNextWindowSize/BeginViewportSideBar operate in (DisplaySize comes from
// SDL_GetWindowSize, i.e. logical points).
float nativeTitleBarHeight() {
#ifdef _WIN32
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    if (vp && vp->PlatformHandleRaw) {
        auto *const hwnd = static_cast<HWND>(vp->PlatformHandleRaw);
        const UINT dpi = GetDpiForWindow(hwnd);
        const int captionPx = GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                              GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const float scaleY = ImGui::GetIO().DisplayFramebufferScale.y;
        if (captionPx > 0 && scaleY > 0.f)
            return static_cast<float>(captionPx) / scaleY;
    }
#endif
    // Fallback (non-Windows or missing handle): a touch taller than the menu bar so it still reads
    // as a title bar.
    return ImGui::GetFrameHeight() * 1.4f;
}

enum class Glyph { Minimize, Maximize, Restore, Close };

// Draws one Windows 11-style caption button at an absolute screen rect using draw-list primitives,
// with hover/press backgrounds. `danger` (the close button) turns red on hover with a white glyph.
// Returns true on click (press + release inside the button).
bool captionButton(const char *id, ImVec2 topLeft, ImVec2 size, Glyph glyph, bool danger) {
    ImGui::SetCursorScreenPos(topLeft);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 br = topLeft + size;

    // Background: Win11 uses a subtle light overlay for min/max and a solid red for close.
    ImU32 bg = 0;
    if (danger) {
        if (held)
            bg = IM_COL32(0xB4, 0x1E, 0x12, 0xFF);
        else if (hovered)
            bg = IM_COL32(0xC4, 0x2B, 0x1C, 0xFF);
    } else {
        if (held)
            bg = IM_COL32(0xFF, 0xFF, 0xFF, 0x18);
        else if (hovered)
            bg = IM_COL32(0xFF, 0xFF, 0xFF, 0x10);
    }
    if (bg)
        dl->AddRectFilled(topLeft, br, bg);

    const ImU32 fg = (danger && (hovered || held)) ? IM_COL32_WHITE : ofs::theme::GetColorU32(ImGuiCol_Text);
    const ImVec2 c = topLeft + size * 0.5f;
    const float g = ImFloor(size.y * 0.16f); // glyph half-extent (~5px for a 32px bar)
    const float t = 1.0f;                    // 1 logical-point stroke, like Win11

    switch (glyph) {
    case Glyph::Minimize:
        dl->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), fg, t);
        break;
    case Glyph::Maximize:
        dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, 0.0f, t);
        break;
    case Glyph::Restore: {
        const float o = ImFloor(g * 0.6f); // offset between the two squares
        // Back square (upper-right), then front square (lower-left) drawn over it.
        dl->AddRect(ImVec2(c.x - g + o, c.y - g), ImVec2(c.x + g, c.y + g - o), fg, 0.0f, t);
        dl->AddRectFilled(ImVec2(c.x - g, c.y - g + o), ImVec2(c.x + g - o, c.y + g),
                          ofs::theme::GetColorU32(ImGuiCol_TitleBgActive));
        dl->AddRect(ImVec2(c.x - g, c.y - g + o), ImVec2(c.x + g - o, c.y + g), fg, 0.0f, t);
        break;
    }
    case Glyph::Close:
        dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, t);
        dl->AddLine(ImVec2(c.x - g, c.y + g), ImVec2(c.x + g, c.y - g), fg, t);
        break;
    }
    return clicked;
}

// ---------------------------------------------------------------------------------------------------
// Command palette (VSCode-style). State is owned by the caller (OfsApp::paletteState) and passed
// in as CommandPaletteState& — satisfies the no-global-UI-state rule.
// ---------------------------------------------------------------------------------------------------

const char *kPalettePopupId = "##cmdpalette";

void openPalette(CommandPaletteState &st) {
    st.justOpened = true;
    st.query.clear();
    st.selected = 0;
    ImGui::OpenPopup(kPalettePopupId);
}

// Renders the palette popup if open, anchored at (anchorX, anchorY) with the given width. On invoke
// (Enter or click) sets result.invokedCommand to the chosen command's original index and closes.
void renderCommandPalette(TitleBarResult &result, std::span<const TitleBarCommand> commands, float anchorX,
                          float anchorY, float width, CommandPaletteState &st) {
    const int commandCount = static_cast<int>(commands.size());
    ImGui::SetNextWindowPos(ImVec2(anchorX, anchorY));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    // NoNavInputs: the palette runs its own Up/Down/Enter navigation over st.selected (below) so the
    // search box keeps focus while the result highlight moves. With ImGuiConfigFlags_NavEnableKeyboard
    // on, ImGui's built-in nav would otherwise also consume Up/Down/Enter and pull focus onto the
    // Selectables — a double-drive. Escape-to-close still works (handled globally, not via this flag).
    const bool open = ImGui::BeginPopup(kPalettePopupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    if (!open)
        return;

    if (st.justOpened) {
        ImGui::SetKeyboardFocusHere();
        st.justOpened = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered =
        ImGui::InputTextWithHint("##q", Str::TbTypeCommand.c_str(), &st.query, ImGuiInputTextFlags_EnterReturnsTrue);

    // Filter + score into frame-scratch (no std::vector in a per-frame path — hot-path rule).
    struct Hit {
        int index;
        int score;
        uint64_t frecency;
    };
    const int cap = commandCount > 0 ? commandCount : 1;
    Hit *hits = FrameAllocator::instance().allocArray<Hit>(cap);
    int n = 0;
    for (int i = 0; i < commandCount; ++i) {
        const char *hay = fmtScratch("{} {} {}", commands[i].group, commands[i].title, commands[i].keywords);
        const ofs::util::FuzzyResult r = ofs::util::fuzzyMatch(st.query, hay);
        if (r.matched)
            hits[n++] = {.index = i, .score = r.score, .frecency = commands[i].frecency};
    }
    const bool hasQuery = !st.query.empty();
    // With a query: best textual score first, frecency breaks ties so a familiar command wins among
    // equal matches. Empty query: pure frecency (frequently/recently used first), stable so unused
    // commands keep registry order.
    if (hasQuery)
        std::stable_sort(hits, hits + n, [](const Hit &a, const Hit &b) {
            return a.score != b.score ? a.score > b.score : a.frecency > b.frecency;
        });
    else
        std::stable_sort(hits, hits + n, [](const Hit &a, const Hit &b) { return a.frecency > b.frecency; });

    // Keyboard navigation (single-line InputText leaves Up/Down/Enter free for us to consume).
    const bool navDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    const bool navUp = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    if (n > 0) {
        if (navDown)
            st.selected = (st.selected + 1) % n;
        if (navUp)
            st.selected = (st.selected + n - 1) % n;
        st.selected = std::clamp(st.selected, 0, n - 1);
    } else {
        st.selected = 0;
    }

    ImGui::Spacing();
    ImGui::BeginChild("##results", ImVec2(0.0f, ImGui::GetFontSize() * 20.0f));
    for (int r = 0; r < n; ++r) {
        const TitleBarCommand &c = commands[hits[r].index];
        const bool sel = (r == st.selected);
        if (ImGui::Selectable(fmtScratch("{} {}: {}##{}", c.icon, c.group, c.title, hits[r].index), sel)) {
            result.invokedCommand = hits[r].index;
            ImGui::CloseCurrentPopup();
        }
        if (sel && (ImGui::IsWindowAppearing() || navUp || navDown))
            ImGui::SetScrollHereY();
        if (c.shortcut && c.shortcut[0] != '\0') {
            ImGui::SameLine();
            const float w = ImGui::CalcTextSize(c.shortcut).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
            ImGui::TextDisabled("%s", c.shortcut);
        }
    }
    ImGui::EndChild();

    if (n == 0) {
        ImGui::TextDisabled("%s", Str::TbNoMatch.c_str());
    }

    if (entered && n > 0) {
        result.invokedCommand = hits[st.selected].index;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

TitleBarResult renderTitleBar(const char *appName, const char *projectTitle, bool isMaximized,
                              std::span<const TitleBarCommand> commands, bool requestOpen,
                              CommandPaletteState &paletteState) {
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    // Never smaller than one frame height, so the text always fits even if the OS caption is tiny.
    const float barHeight = std::max(nativeTitleBarHeight(), ImGui::GetFrameHeight());

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ofs::theme::GetColorU32(ImGuiCol_TitleBgActive));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    const bool open = ImGui::BeginViewportSideBar("##AppTitleBar", viewport, ImGuiDir_Up, barHeight, flags);
    ImGui::PopStyleVar();

    TitleBarResult result;
    result.height = barHeight;

    if (open) {
        const float barW = ImGui::GetWindowWidth();
        // Win11 caption buttons are 46x32 for a 32px-tall bar (ratio ~1.44).
        const ImVec2 btnSize(ImFloor(barHeight * 1.44f), barHeight);
        const float buttonsZoneW = btnSize.x * 3.0f;
        result.buttonsLeftX = barW - buttonsZoneW;
        const ImVec2 barPos = ImGui::GetWindowPos();

        // Vertically center a single text line within the (possibly taller-than-frame) bar.
        const float padY = std::max(0.0f, (ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::SetCursorPosY(padY);
        // App mark before the title, tinted with the accent so it reads as a logo.
        ImGui::PushStyleColor(ImGuiCol_Text, ofs::theme::GetColorU32(ImGuiCol_CheckMark));
        ImGui::TextUnformatted(ICON_APP);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 6.0f);
        const float appNameLeft = ImGui::GetCursorPosX();
        ImGui::TextUnformatted(appName);
        const float appNameRight = appNameLeft + ImGui::CalcTextSize(appName).x;

        // Centered clickable box: shows the active project's name and opens the command palette, like
        // the VSCode title-bar quick-open. Drawn with primitives (matching the caption buttons) so it
        // needs no extra widgets, and reported back as a hit-test carve-out so the OS caption doesn't
        // swallow the click.
        const float boxPadX = 12.0f;
        const float nameWidth = ImGui::CalcTextSize(projectTitle).x;
        const float magW = ImGui::GetTextLineHeight() * 0.9f;
        const float boxH = std::min(ImGui::GetTextLineHeight() + 6.0f, barHeight - 6.0f);
        // Widen to fit the project name (plus a little trailing slack), capped at ~half the bar so a
        // long name can't crowd the caption buttons; past the cap the name is elided below.
        const float boxW = std::min(barW * 0.52f, magW + nameWidth + boxPadX * 2.0f + ImGui::GetFontSize() * 1.5f);
        // Always center on the full bar width so the box sits at the true middle of the title bar.
        // The clamps only engage on a narrow window, keeping the box clear of the app name and the
        // caption buttons rather than overlapping them.
        float boxX = barW * 0.5f - boxW * 0.5f;
        boxX = std::max(boxX, appNameRight + 12.0f);
        boxX = std::min(boxX, barW - buttonsZoneW - boxW - 8.0f);
        const float boxY = (ImGui::GetWindowHeight() - boxH) * 0.5f;

        ImGui::SetCursorPos(ImVec2(boxX, boxY));
        const ImVec2 boxTL = ImGui::GetCursorScreenPos();
        const bool boxClicked = ImGui::InvisibleButton("##searchbox", ImVec2(boxW, boxH));
        const bool boxHovered = ImGui::IsItemHovered();
        ImGui::SetMouseCursor(boxHovered ? ImGuiMouseCursor_Hand : ImGui::GetMouseCursor());
        if (boxHovered)
            ImGui::SetTooltip("%s", projectTitle);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 boxBR = boxTL + ImVec2(boxW, boxH);
        dl->AddRectFilled(boxTL, boxBR, IM_COL32(0xFF, 0xFF, 0xFF, boxHovered ? 0x22 : 0x12), 4.0f);
        dl->AddRect(boxTL, boxBR, IM_COL32(0xFF, 0xFF, 0xFF, 0x22), 4.0f);
        // Magnifier glyph (circle + handle).
        const ImU32 gcol = ofs::theme::GetColorU32(ImGuiCol_TextDisabled);
        const float gr = boxH * 0.16f;
        const ImVec2 gc(boxTL.x + boxPadX, boxTL.y + boxH * 0.5f);
        dl->AddCircle(gc, gr, gcol, 0, 1.2f);
        dl->AddLine(gc + ImVec2(gr * 0.7f, gr * 0.7f), gc + ImVec2(gr * 1.6f, gr * 1.6f), gcol, 1.2f);
        // Draw-list text has no auto-clip; elide so a long project name on a narrow window (where the
        // barW*0.52 cap shrinks the box below the name width) stops at the box edge instead of overrunning.
        // The name uses full text color (it's content); only the magnifier stays muted.
        const float textLeft = gc.x + gr + 12.0f;
        dl->AddText(ImVec2(textLeft, boxTL.y + (boxH - ImGui::GetTextLineHeight()) * 0.5f),
                    ofs::theme::GetColorU32(ImGuiCol_Text), ofs::ui::elide(projectTitle, boxBR.x - textLeft - boxPadX));

        // Carve the box out of the draggable strip (one-frame latency, same as buttonsLeftX).
        result.searchLeftX = boxX;
        result.searchRightX = boxX + boxW;

        // Open triggers: click the box, or `requestOpen` (from the rebindable "open palette" command).
        if ((boxClicked || requestOpen) && !ImGui::IsPopupOpen(kPalettePopupId))
            openPalette(paletteState);

        const float palW = std::max(boxW, ImGui::GetFontSize() * 29.0f);
        // Center the popup on the search box, then clamp to window bounds.
        float palX = boxTL.x + boxW * 0.5f - palW * 0.5f;
        palX = std::max(palX, barPos.x + 8.0f);
        palX = std::min(palX, barPos.x + barW - palW - 8.0f);
        renderCommandPalette(result, commands, palX, barPos.y + barHeight + 2.0f, palW, paletteState);

        const float top = barPos.y;
        const float rightX = barPos.x + barW;
        const ImVec2 minTL(rightX - btnSize.x * 3.0f, top);
        const ImVec2 maxTL(rightX - btnSize.x * 2.0f, top);
        const ImVec2 closeTL(rightX - btnSize.x, top);

        if (captionButton("##min", minTL, btnSize, Glyph::Minimize, false))
            result.action = TitleBarAction::Minimize;
        if (captionButton("##max", maxTL, btnSize, isMaximized ? Glyph::Restore : Glyph::Maximize, false))
            result.action = TitleBarAction::ToggleMaximize;
        if (captionButton("##close", closeTL, btnSize, Glyph::Close, true))
            result.action = TitleBarAction::Close;
    }
    ImGui::End();
    ImGui::PopStyleColor();

    return result;
}

} // namespace ofs::ui
