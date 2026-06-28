#include "UI/WelcomeScreen.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "UI/FogBackground.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"     // buttonW
#include "UI/Theme.h"            // AppCol_Warning, getActive
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/Version.h"        // versionTitle (shared with the OS window title)
#include "imgui.h"
#include <algorithm>
#include <string>

namespace ofs {
namespace {

// Filename portion of a UTF-8 path without allocating. '/' and '\\' are single ASCII bytes that never
// appear inside a UTF-8 multibyte sequence (continuation bytes are all >= 0x80), so a raw byte scan is
// safe for non-ASCII paths — no std::filesystem::path round-trip (which would allocate per frame).
const char *utf8Filename(const std::string &path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path.c_str() : path.c_str() + pos + 1;
}

// Monochrome fog tint derived from the window background: a lighter haze on dark themes, a darker one on
// light themes, so it reads the same way regardless of palette. Alpha is the ceiling a full billow
// reaches; the shader fades it out toward the center so it never competes with the text on top.
ImVec4 fogTint() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool dark = ofs::theme::getActive().isDark;
    const float target = dark ? 1.0f : 0.0f; // toward white on dark, toward black on light
    // A dark haze over a near-white panel needs both a deeper tint and a higher alpha ceiling than a
    // light haze over a dark one to read at all — otherwise light mode just washes out.
    const float k = dark ? 0.6f : 0.8f;
    const float alpha = dark ? 0.13f : 0.28f;
    return {bg.x + (target - bg.x) * k, bg.y + (target - bg.y) * k, bg.z + (target - bg.z) * k, alpha};
}

// Central vignette over the content area. On dark themes it darkens toward black to deepen the panel
// under the text; on light themes it brightens toward white, so the content sits in a clear bright pool
// framed by the darker fog at the edges.
ImVec4 fogCenter() {
    return ofs::theme::getActive().isDark ? ImVec4{0.f, 0.f, 0.f, 0.32f} : ImVec4{1.f, 1.f, 1.f, 0.6f};
}

} // namespace

WelcomeScreen::WelcomeScreen() : fog_(std::make_unique<FogBackground>()) {}

WelcomeScreen::~WelcomeScreen() = default;

void WelcomeScreen::render(EventQueue &eq, const AppSettings &settings) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    // Fill the work area, which the title bar (top) and footer (bottom) have already carved out — the
    // welcome screen spans exactly the region the editor dockspace would.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    // NoBringToFrontOnFocus so it never steals z-order from modals (the New/Open dialog) that render
    // afterwards; NoDocking so it stays a plain overlay and never joins the editor's dock layout.
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
    if (!ImGui::Begin(Str::WelTitle.id("welcome_screen"), nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Animated fog behind everything: emitted first so the window's content draws on top of it.
    fog_->draw(ImGui::GetWindowDrawList(), vp->WorkPos, vp->WorkSize, fogTint(), fogCenter());

    // The two action buttons, composed once so the column can size to the wider (translated) label.
    const char *openLabel = Str::WelOpenProject.iconId(ICON_FOLDER_OPEN, "welcome_open");
    const char *createLabel = Str::WelCreateEmpty.iconId(ICON_FILE_PLUS, "welcome_create_empty");

    // A left-aligned content column, centered horizontally and pushed a little above the vertical
    // center so the recent list has room to grow downward. Width tracks the widest button label (so a
    // longer translation never clips) over a font-relative floor — not a fixed 420 px that ignores DPI.
    const float blockW =
        std::max({ImGui::GetFontSize() * 22.0f, ofs::ui::buttonW(openLabel), ofs::ui::buttonW(createLabel)});
    const float startX = ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - blockW) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y * 0.18f);
    const auto col = [&] { ImGui::SetCursorPosX(startX); };

    col();
    // PushFont(NULL, size) scales off the unscaled FontSizeBase (not GetFontSize, which already has the
    // global DPI factor applied — that would double-apply it). The header is the exact build-identity
    // string used by the OS window title (versionTitle), so the two never drift.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextUnformatted(versionTitle().c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.1f));
    col();
    if (ImGui::Button(openLabel, ImVec2(blockW, 0.0f)))
        eq.push(OpenOrNewProjectRequestEvent{});

    ImGui::Spacing();
    col();
    // Straight to a fresh, media-less project — no file picker, no confirm (unlike Open / New above).
    if (ImGui::Button(createLabel, ImVec2(blockW, 0.0f)))
        eq.push(CreateEmptyProjectEvent{});

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.25f));
    col();
    ImGui::TextDisabled("%s", Str::WelRecentProjects.c_str());
    if (!settings.lastProjectPaths.empty()) {
        // Right-align a small Clear button on the heading row, within the content block.
        const float clearW = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(startX + blockW - clearW);
        if (ImGui::SmallButton(ICON_TRASH "###welcome_clear_recent"))
            eq.push(ClearRecentProjectsEvent{});
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Str::WelClearRecentTip.c_str());
    }
    ImGui::Spacing();

    if (settings.lastProjectPaths.empty()) {
        col();
        ImGui::TextDisabled("%s", Str::WelNoRecent.c_str());
    } else {
        for (size_t i = 0; i < settings.lastProjectPaths.size(); ++i) {
            const std::string &path = settings.lastProjectPaths[i];
            col();
            ImGui::PushID(static_cast<int>(i)); // disambiguate rows that share a filename
            if (ImGui::Selectable(fmtScratch("{}  {}", ICON_FILE, utf8Filename(path)), false, ImGuiSelectableFlags_None,
                                  ImVec2(blockW, 0.0f)))
                eq.push(OpenProjectRequestEvent{path});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());
            if (ImGui::BeginPopupContextItem()) {
                ofs::ui::showInFileBrowserItem(path);
                // Drop just this entry from the list — does not touch the file on disk.
                if (ImGui::MenuItem(Str::WelRemoveRecent.iconId(ICON_TRASH, "welcome_remove_recent")))
                    eq.push(RemoveRecentProjectEvent{path});
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.1f));
    col();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(startX + blockW);
    ImGui::TextWrapped("%s", Str::WelTip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // Security note: .ofp projects can carry embedded scripts that execute on open, so opening one is a
    // trust decision — render it in the full warning color (not dimmed) so it reads as a caution the
    // user shouldn't skim past. Wrapped to the content block.
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.4f));
    col();
    ImGui::PushStyleColor(ImGuiCol_Text, ofs::theme::GetStyleColorVec4(AppCol_Warning));
    ImGui::PushTextWrapPos(startX + blockW);
    ImGui::TextWrapped("%s", Str::WelSecurityNote.icon(ICON_ALERT_TRIANGLE));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::End();
}

} // namespace ofs
