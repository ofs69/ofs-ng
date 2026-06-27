#pragma once
#include "UI/Icons.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <string_view>

// Shared ImGui layout helpers used across all UI windows.

namespace ofs::ui {

// Font pointer set once during Application::loadFonts(). Valid from init until shutdown.
// gFontDefault: JetBrainsMono + NotoSansCJKjp + Lucide icons merged
inline ImFont *gFontDefault = nullptr;

// Gap between item right edge and the scrollbar track. Items must stop this
// many pixels short of GetContentRegionAvail().x so they never abut the scrollbar.
inline constexpr float kRightGap = 6.f;
// Label-column width for ProcessingPanel's own node-param table (not the form helpers below,
// which auto-fit). Kept here as the one shared layout constant those node widgets reference.
inline constexpr float kLabelW = 120.f;

// Brighten an RGB color by `delta` per channel (clamped to 1), preserving alpha. Used for hover/active
// highlight tints (axis chips, band bar, node chips) that all lighten a base color the same way.
inline ImVec4 brighten(const ImVec4 &c, float delta) {
    return {std::min(c.x + delta, 1.0f), std::min(c.y + delta, 1.0f), std::min(c.z + delta, 1.0f), c.w};
}
inline ImU32 brighten(ImU32 c, float delta) {
    return ImColor(brighten(ImColor(c).Value, delta));
}

// Content width of a button sized to its label, so verb buttons fit their (translated) text instead
// of a fixed px that clips a longer translation. `true` strips a trailing ###id before measuring, so
// the label may be passed with or without its id. When reserving a right-pinned gap, add ItemSpacing.x.
inline float buttonW(const char *label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.f;
}

// Opens a two-column (label | widget) form table. Table width is capped at
// GetContentRegionAvail().x - kRightGap so widgets never abut the scrollbar. The label column is
// WidthFixed with no width, so it auto-fits to the widest label in this table — a longer translated
// label can never clip or overrun the widget column (no hardcoded width that survives only English).
// Returns false (and must NOT call endForm) if the table could not be created.
inline bool beginForm(const char *id) {
    float w = ImGui::GetContentRegionAvail().x - kRightGap;
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None, {w, 0.f}))
        return false;
    ImGui::TableSetupColumn("##L", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##R", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// Advances to a new row, writes a left-aligned label in the auto-fitting label column, then moves
// the cursor to the widget column. Follow with SetNextItemWidth(-FLT_MIN) and the desired widget.
// Left-aligned (not right-aligned into a fixed column) so the column can size to content — see
// beginForm; mirrors PluginManager's pluginFormRow.
inline void formRow(const char *label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

// Like beginForm, but with a leading fixed-width icon column so glyphs line up in
// their own column independent of label width: [icon | label | widget].
inline bool beginIconForm(const char *id) {
    float w = ImGui::GetContentRegionAvail().x - kRightGap;
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_None, {w, 0.f}))
        return false;
    ImGui::TableSetupColumn("##I", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x);
    ImGui::TableSetupColumn("##L", ImGuiTableColumnFlags_WidthFixed); // auto-fits to widest label
    ImGui::TableSetupColumn("##R", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// Row for beginIconForm: left-aligned icon (may be nullptr to leave the column blank), left-aligned
// label in the auto-fitting label column, then the cursor parks in the widget column. Follow with
// SetNextItemWidth(-FLT_MIN) and the widget, as with formRow.
inline void formRowIcon(const char *icon, const char *label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (icon) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(icon);
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

inline void endForm() {
    ImGui::EndTable();
}

// Dimmed help glyph that reveals `desc` as a wrapped tooltip on hover — for attaching an explanatory
// note to a setting without spending a row on inline text. Mirrors imgui_demo.cpp's HelpMarker.
// AlignTextToFramePadding so the icon lines up when placed after a checkbox or in a form label cell.
inline void helpMarker(const char *desc) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", ICON_CIRCLE_HELP);
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Ctrl+F search activation. Returns true on the frame Ctrl+F is pressed while the current
// window (or one of its children) holds focus; follow it with SetKeyboardFocusHere() just
// before a filter/search InputText to jump the caret into it. Routed through ImGui::Shortcut
// (default ImGuiInputFlags_RouteFocused), so only the focused window reacts — the command
// palette, which lives in its own popup on Ctrl+Shift+P, is never caught by this.
inline bool shortcutFocusSearch() {
    return ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F);
}

// Draw an animated arc spinner directly onto a draw list at a given centre point.
inline void spinnerAt(ImDrawList *dl, ImVec2 centre, float radius, float thickness, ImU32 color) {
    constexpr float kPi = 3.14159265358979323846f;
    const float t = static_cast<float>(ImGui::GetTime());
    constexpr int kSegments = 30;
    const int start = static_cast<int>(std::abs(std::sin(t * 1.8f) * (kSegments - 5)));
    const float aMin = kPi * 2.f * static_cast<float>(start) / kSegments;
    const float aMax = kPi * 2.f * static_cast<float>(kSegments - 3) / kSegments;
    dl->PathClear();
    for (int i = 0; i < kSegments; ++i) {
        const float a = aMin + (static_cast<float>(i) / kSegments) * (aMax - aMin);
        dl->PathLineTo({centre.x + std::cos(a + t * 8.f) * radius, centre.y + std::sin(a + t * 8.f) * radius});
    }
    dl->PathStroke(color, thickness);
}

// Animated arc spinner. Reserves space via Dummy() and draws into the window draw list.
// radius and thickness are in pixels.
inline void spinner(const char *label, float radius, float thickness, ImU32 color) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({radius * 2.f, (radius + style.FramePadding.y) * 2.f});
    if (ImGui::IsItemVisible()) {
        const ImVec2 centre(pos.x + radius, pos.y + radius + style.FramePadding.y);
        spinnerAt(ImGui::GetWindowDrawList(), centre, radius, thickness, color);
    }
}

// Truncate `s` to fit `maxW` pixels, appending an ellipsis when it would overflow; returns the
// original pointer when it already fits, otherwise a frame-arena string ending in "…". Draw-list
// text (unlike a widget) has no auto-clip or auto-elision, so any caption painted via AddText into a
// bounded area must be elided by hand or it overruns its neighbour.
inline const char *elide(const char *s, float maxW) {
    if (maxW <= 0.f || ImGui::CalcTextSize(s).x <= maxW)
        return s;
    const char *ellipsis = "\xE2\x80\xA6"; // …
    const float avail = std::max(0.f, maxW - ImGui::CalcTextSize(ellipsis).x);
    int fit = 0;
    for (int i = 0; s[i] != '\0';) {
        int next = i + 1;
        while ((static_cast<unsigned char>(s[next]) & 0xC0U) == 0x80U) // span a whole UTF-8 codepoint
            ++next;
        if (ImGui::CalcTextSize(s, s + next).x > avail)
            break;
        fit = next;
        i = next;
    }
    return fmtScratch("{}{}", std::string_view(s, static_cast<size_t>(fit)), ellipsis);
}

// Inside an already-open popup, renders a "Show in File Explorer" item that reveals `utf8Path` (a
// UTF-8 file path) in the OS file browser — selected in Explorer on Windows, its folder opened
// elsewhere. Disabled when the path is empty. Defined in ImGuiHelpers.cpp to keep <filesystem> and
// PathUtil out of this widely-included header.
void showInFileBrowserItem(std::string_view utf8Path);

inline void addTextShadow(ImDrawList *drawList, const ImVec2 &pos, ImU32 color, std::string_view text) {
    auto alpha = static_cast<uint8_t>((color >> 24) & 0xFFU);
    ImU32 shadowColor = (ofs::theme::GetColorU32(AppCol_TextShadow) & 0x00FFFFFFU) | (static_cast<ImU32>(alpha) << 24);
    drawList->AddText(pos + ImVec2(1, 1), shadowColor, text.data(), text.data() + text.size());
    drawList->AddText(pos, color, text.data(), text.data() + text.size());
}

} // namespace ofs::ui
