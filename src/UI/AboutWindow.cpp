#include "UI/AboutWindow.h"

#include "Core/EventQueue.h"
#include "Core/UpdateEvents.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/Resources.h"      // ofs::res
#include "Util/Version.h"        // ofs::versionTitle

#include "imgui.h"
#include <OfsBuildInfo.h>  // generated: git identity
#include <SDL3/SDL_misc.h> // SDL_OpenURL (release page)
#include <SDL3/SDL_version.h>
#include <iterator> // std::size

namespace ofs {
namespace {

// One bundled third-party component. name / license / copyright are short identifiers shown in the
// list; `asset` is the archive-relative path (data.pak, via ofs::res) of its verbatim license body.
// This is legal/proper-noun data, intentionally not routed through the localization catalog (see the
// About-window note in strings.toml) — same rationale as the Theme swatch labels.
struct Attribution {
    const char *name;
    const char *license;
    const char *copyright;
    const char *asset;
};

// Every third-party component that ships in the app binary. Test-only deps (doctest, Dear ImGui Test
// Engine) are deliberately excluded — they are not in a distributed build.
constexpr Attribution kAttributions[] = {
    {.name = "Dear ImGui", .license = "MIT", .copyright = "(c) Omar Cornut", .asset = "data/licenses/imgui.txt"},
    {.name = "imnodes", .license = "MIT", .copyright = "(c) Johann Muszynski", .asset = "data/licenses/imnodes.txt"},
    {.name = "SDL3", .license = "Zlib", .copyright = "(c) Sam Lantinga", .asset = "data/licenses/SDL3.txt"},
    {.name = "libmpv",
     .license = "LGPL-2.1+",
     .copyright = "(c) the mpv developers",
     .asset = "data/licenses/libmpv.txt"},
    {.name = "spdlog", .license = "MIT", .copyright = "(c) Gabriel Melman", .asset = "data/licenses/spdlog.txt"},
    {.name = "{fmt}",
     .license = "MIT",
     .copyright = "(c) Victor Zverovich and contributors",
     .asset = "data/licenses/fmt.txt"},
    {.name = "nlohmann/json", .license = "MIT", .copyright = "(c) Niels Lohmann", .asset = "data/licenses/json.txt"},
    {.name = "toml++", .license = "MIT", .copyright = "(c) Mark Gillard", .asset = "data/licenses/tomlplusplus.txt"},
    {.name = "Bitsery", .license = "MIT", .copyright = "(c) Mindaugas Vinkelis", .asset = "data/licenses/bitsery.txt"},
    {.name = "BS::thread_pool",
     .license = "MIT",
     .copyright = "(c) Barak Shoshany",
     .asset = "data/licenses/thread-pool.txt"},
    {.name = "moodycamel::ConcurrentQueue",
     .license = "BSD-2-Clause",
     .copyright = "(c) Cameron Desrochers",
     .asset = "data/licenses/concurrentqueue.txt"},
    {.name = "miniz",
     .license = "MIT",
     .copyright = "(c) Rich Geldreich and contributors",
     .asset = "data/licenses/miniz.txt"},
    {.name = "OpenGL Mathematics (GLM)",
     .license = "MIT",
     .copyright = "(c) G-Truc Creation",
     .asset = "data/licenses/glm.txt"},
    {.name = "glad",
     .license = "CC0-1.0 / Apache-2.0",
     .copyright = "(c) David Herberth; Khronos Group",
     .asset = "data/licenses/glad.txt"},
    {.name = "stb / cgltf / picosha2",
     .license = "MIT / Public Domain",
     .copyright = "(c) Sean Barrett and others",
     .asset = "data/licenses/stb.txt"},
    {.name = ".NET Runtime",
     .license = "MIT",
     .copyright = "(c) .NET Foundation and Contributors",
     .asset = "data/licenses/dotnet.txt"},
    {.name = "JetBrains Mono",
     .license = "OFL-1.1",
     .copyright = "(c) The JetBrains Mono Project Authors",
     .asset = "data/fonts/JetBrainsMono-OFL.txt"},
    {.name = "Lucide",
     .license = "ISC / MIT",
     .copyright = "(c) Lucide Contributors; Feather (c) Cole Bemis",
     .asset = "data/fonts/lucide-LICENSE.txt"},
    {.name = "Noto Sans CJK JP",
     .license = "OFL-1.1",
     .copyright = "(c) 2014-2021 Adobe",
     .asset = "data/fonts/NotoSansCJKjp-LICENSE.txt"},
    {.name = "Kenney UI SFX Set",
     .license = "CC0-1.0",
     .copyright = "(c) Kenney (kenney.nl)",
     .asset = "data/audio/kenney-ui-sfx-set-LICENSE.txt"},
    // CC BY 4.0 requires attribution, so this credit is mandatory, not courtesy.
    {.name = "Universal UI Soundpack",
     .license = "CC-BY-4.0",
     .copyright = "(c) Nathan Gibson (nathangibson.myportfolio.com)",
     .asset = "data/audio/universal-ui-soundpack-LICENSE.txt"},
};

// Compile-time build environment, assembled once. The OS / arch / config are fixed at compile time;
// the toolchain identity is whatever built this TU.
const char *platformLine() {
#if defined(_WIN32)
    constexpr const char *os = "Windows";
#elif defined(__APPLE__)
    constexpr const char *os = "macOS";
#elif defined(__linux__)
    constexpr const char *os = "Linux";
#else
    constexpr const char *os = "Unknown OS";
#endif
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || defined(__ppc64__)
    constexpr const char *arch = "64-bit";
#else
    constexpr const char *arch = "32-bit";
#endif
#ifdef NDEBUG
    constexpr const char *cfg = "Release";
#else
    constexpr const char *cfg = "Debug";
#endif
    return fmtScratch("{} ({}, {})", os, arch, cfg);
}

const char *toolchainLine() {
#if defined(__clang__)
    const char *compiler = fmtScratch("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
    const char *compiler = fmtScratch("MSVC {}", _MSC_VER);
#elif defined(__GNUC__)
    const char *compiler = fmtScratch("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    const char *compiler = "unknown compiler";
#endif
    return fmtScratch("{} \xc2\xb7 Dear ImGui {} \xc2\xb7 SDL {}.{}.{}", compiler, IMGUI_VERSION, SDL_MAJOR_VERSION,
                      SDL_MINOR_VERSION, SDL_MICRO_VERSION);
}

} // namespace

void AboutWindow::selectComponent(int index) {
    selected_ = index;
    const char *asset = kAttributions[index].asset;
    if (auto text = ofs::res::readText(asset))
        licenseText_ = std::move(*text);
    else
        licenseText_ = fmtScratch("Could not load license text: {}", asset);
}

void AboutWindow::render(bool &open, const UpdateChecker::Status &update, EventQueue &eq) {
    if (!open)
        return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({em * 40.0f, em * 32.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({em * 26.0f, em * 18.0f}, {FLT_MAX, FLT_MAX});

    if (!ImGui::Begin(Str::AboutTitle.id("about"), &open,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // ── Identity header ───────────────────────────────────────────────────────────────────────
    ImGui::TextUnformatted(ICON_APP " ofs-ng");
    ImGui::TextDisabled("%s", ofs::versionTitle().c_str());

    // ── Build information ─────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText(Str::AboutBuildInfo);
    if (ofs::ui::beginForm("##about_build")) {
        ofs::ui::formRow(Str::AboutCommit);
        const std::string_view commit = generated::kGitCommitShort;
        ImGui::TextUnformatted(commit.empty() ? "unknown" : fmtScratch("{}", commit));
        if (!commit.empty() && ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(fmtScratch("{}", generated::kGitCommitLong));
            ImGui::EndTooltip();
        }

        ofs::ui::formRow(Str::AboutBuilt);
        ImGui::TextUnformatted(__DATE__ " " __TIME__);

        ofs::ui::formRow(Str::AboutPlatform);
        ImGui::TextUnformatted(platformLine());

        ofs::ui::formRow("");
        ImGui::TextDisabled("%s", toolchainLine());
        ofs::ui::endForm();
    }

    // ── Updates ───────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText(Str::AboutUpdates);
    if (update.state != UpdateChecker::State::Idle)
        ImGui::AlignTextToFramePadding();
    switch (update.state) {
    case UpdateChecker::State::Checking:
        ImGui::TextDisabled("%s", Str::AboutChecking.c_str());
        break;
    case UpdateChecker::State::Available:
        ImGui::TextUnformatted(Str::AboutUpdateAvailable.fmt(update.latestVersion));
        if (!update.releaseUrl.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Str::AboutOpenRelease.id("about_open_release")))
                SDL_OpenURL(update.releaseUrl.c_str());
        }
        break;
    case UpdateChecker::State::UpToDate:
        ImGui::TextDisabled("%s", Str::AboutUpToDate.c_str());
        break;
    case UpdateChecker::State::Failed:
        ImGui::TextDisabled("%s", Str::AboutUpdateFailed.c_str());
        break;
    case UpdateChecker::State::Idle:
        break;
    }
    if (update.state != UpdateChecker::State::Idle)
        ImGui::SameLine();
    ImGui::BeginDisabled(update.state == UpdateChecker::State::Checking);
    if (ImGui::Button(Str::AboutCheckNow.id("about_check_now")))
        eq.push(CheckForUpdatesEvent{.userInitiated = true});
    ImGui::EndDisabled();

    // ── Third-party attributions ──────────────────────────────────────────────────────────────
    ImGui::SeparatorText(Str::AboutThirdParty);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(Str::AboutLicenseNote);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    // Size the license column to the widest string it will actually hold (the SPDX ids plus the
    // translated header) rather than a magic literal, so combos like "CC0-1.0 / Apache-2.0" don't clip.
    float licenseColW = ImGui::CalcTextSize(Str::AboutColLicense.c_str()).x;
    for (const Attribution &e : kAttributions)
        licenseColW = std::max(licenseColW, ImGui::CalcTextSize(e.license).x);
    licenseColW += ImGui::GetStyle().CellPadding.x * 2.0f;
    if (ImGui::BeginTable("##about_thirdparty", 2, tableFlags, {0.0f, em * 9.0f})) {
        ImGui::TableSetupColumn(Str::AboutColComponent, ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Str::AboutColLicense, ImGuiTableColumnFlags_WidthFixed, licenseColW);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(std::size(kAttributions)); ++i) {
            const Attribution &e = kAttributions[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // SpanAllColumns so a click anywhere on the row selects it; the ##id keeps the row's widget
            // identity stable independent of the (proper-noun) label.
            if (ImGui::Selectable(fmtScratch("{}##about_tp{}", e.name, i), selected_ == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                selectComponent(i);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.license);
        }
        ImGui::EndTable();
    }

    // ── Selected license body ─────────────────────────────────────────────────────────────────
    if (selected_ < 0) {
        ImGui::TextDisabled("%s", Str::AboutSelectPrompt.c_str());
    } else {
        const Attribution &e = kAttributions[selected_];
        ImGui::TextUnformatted(fmtScratch("{}  \xe2\x80\x94  {}", e.name, e.license));
        const char *copyLbl = Str::AboutCopyLicense.iconId(ICON_COPY, "about_copy");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ofs::ui::buttonW(copyLbl));
        if (ImGui::Button(copyLbl))
            ImGui::SetClipboardText(licenseText_.c_str());
        ImGui::TextDisabled("%s", e.copyright);

        if (ImGui::BeginChild("##about_license", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::TextUnformatted(licenseText_.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace ofs
