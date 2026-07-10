#include "ConfigurationWindow.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Core/SimulatorSettings.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "UI/Glyphs.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "Util/FuzzyMatch.h"
#include "Util/PathUtil.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <SDL3/SDL_video.h>
#include <cmath>
#include <cstring>
#include <span>

namespace ofs {

using ofs::ui::beginForm;
using ofs::ui::beginIconForm;
using ofs::ui::comboItem;
using ofs::ui::endForm;
using ofs::ui::formRow;
using ofs::ui::formRowHelp;
using ofs::ui::formRowIcon;
using ofs::ui::labeledCheckbox;
using ofs::ui::resetButton;
using ofs::ui::scrollbarGap;

ConfigurationWindow::ConfigurationWindow(const AppSettings &appSettings) : appSettings(appSettings) {}

void ConfigurationWindow::render(const ScriptProject &project, EventQueue &eq, bool &open) {
    if (prevOpen && !open) {
        // Font size applies live (layout refits on change); only the video hardware-decode toggle needs
        // a restart, since it's bound when the mpv player is created.
        if (appSettings.hwdecEnabled != openHwdec)
            showInfo(eq, Str::PrefRestartTitle.c_str(), Str::PrefRestartBody.c_str());
    }

    if (open && !prevOpen) {
        availableLanguages = ofs::loc::Translator::instance().available();
        availableThemes = ofs::theme::list();
        openHwdec = appSettings.hwdecEnabled;
        fontSizeEdit_ = static_cast<int>(appSettings.fontSizeBase);
    }
    prevOpen = open;

    if (!open)
        return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    // Min constraint is font-relative so it grows with the UI font / DPI instead of clipping content.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({vp->Size.x * 0.45f, vp->Size.y * 0.7f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({em * 30.f, em * 16.f}, {em * 66.f, em * 66.f});

    if (ImGui::Begin(Str::PrefTitle.id("preferences"), &open,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##config_tabs")) {
            if (ImGui::BeginTabItem(Str::PrefTabApplication.id("app_tab"))) {
                ImGui::BeginChild("##app_scroll", {0.f, 0.f}, ImGuiChildFlags_None);
                renderApplicationTab(eq);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Str::PrefSimulator.id("sim_tab"))) {
                ImGui::BeginChild("##sim_scroll", {0.f, 0.f}, ImGuiChildFlags_None);
                renderSimulatorTab(project, eq);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(Str::PrefTheme.id("theme_tab"))) {
                ImGui::BeginChild("##theme_scroll", {0.f, 0.f}, ImGuiChildFlags_None);
                renderThemeTab(project, eq);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void ConfigurationWindow::renderSimulatorTab(const ScriptProject &project, EventQueue &eq) {
    // Edit a local copy of the live simulator state; project.simulator is the source of truth and
    // is persisted to AppSettings on exit. Only the settings fields below are pushed, so a
    // concurrent transform edit from the simulator window is never clobbered.
    SimulatorState sim = project.simulator;
    bool changed = false;

    ImGui::SeparatorText(Str::PrefDisplay);
    if (beginIconForm("##sim_display")) {
        formRowIcon(ICON_EYE, Str::PrefIndicators);
        changed |= ImGui::Checkbox("##simindicators", &sim.enableIndicators);
        formRowIcon(ICON_SEEK, Str::PrefShowPosition);
        changed |= ImGui::Checkbox("##simshowpos", &sim.enablePosition);
        formRowIcon(ICON_LIST, Str::PrefHeightLines);
        changed |= ImGui::Checkbox("##simheightlines", &sim.enableHeightLines);
        formRowIcon(nullptr, Str::PrefExtraLines);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragInt("##extracount", &sim.extraLinesCount, 1.f, 0, 20);
        formRowIcon(ICON_EYE, Str::Pref3dLabels);
        {
            // One toggle per DOF, indexed into labels3dMask by StandardAxis (L0..R2 == 0..5).
            constexpr StandardAxis dofs[] = {StandardAxis::L0, StandardAxis::L1, StandardAxis::L2,
                                             StandardAxis::R0, StandardAxis::R1, StandardAxis::R2};
            for (size_t i = 0; i < std::size(dofs); ++i) {
                if (i != 0)
                    ImGui::SameLine();
                const auto bit = static_cast<size_t>(dofs[i]);
                bool on = sim.labels3dMask.test(bit);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Checkbox(fmtScratch("{}", standardAxisShortName(dofs[i])), &on)) {
                    sim.labels3dMask.set(bit, on);
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        formRowIcon(nullptr, Str::Pref3dLabelUnits);
        ImGui::SetNextItemWidth(-FLT_MIN);
        {
            int unit = sim.labels3dInDegrees ? 0 : 1;
            const char *units[] = {Str::PrefDegrees, Str::PrefPercent};
            if (ImGui::Combo("##sim3dunits", &unit, units, 2)) {
                sim.labels3dInDegrees = (unit == 0);
                changed = true;
            }
        }
        endForm();
    }

    ImGui::SeparatorText(Str::PrefSim3dRanges);
    // A 0–100 axis value maps symmetrically across ±range. Linear ranges are in
    // model-space units; rotation ranges are the maximum deflection in degrees.
    if (beginIconForm("##sim3d_ranges")) {
        formRowIcon(ICON_MOVE_VERTICAL, Str::PrefStrokeL0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("##simstroke", &sim.strokeRange, 0.01f, 0.f, 4.f, "%.2f");
        formRowIcon(ICON_MOVE_3D, Str::PrefSurgeL1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("##simsurge", &sim.surgeRange, 0.01f, 0.f, 4.f, "%.2f");
        formRowIcon(ICON_MOVE_HORIZONTAL, Str::PrefSwayL2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("##simsway", &sim.swayRange, 0.01f, 0.f, 4.f, "%.2f");
        formRowIcon(ICON_ROTATE_3D, Str::PrefTwistR0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##simtwist", &sim.twistRange, 0.f, 180.f, "%.0f" GLYPH_DEGREE);
        formRowIcon(ICON_AXIS_3D, Str::PrefRollR1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##simroll", &sim.rollRange, 0.f, 90.f, "%.0f" GLYPH_DEGREE);
        formRowIcon(ICON_ORBIT, Str::PrefPitchR2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##simpitch", &sim.pitchRange, 0.f, 90.f, "%.0f" GLYPH_DEGREE);
        endForm();
    }

    if (changed) {
        eq.push(ModifyEvent<SimulatorState>{[sim](SimulatorState &s) {
            s.extraLinesCount = sim.extraLinesCount;
            s.enableIndicators = sim.enableIndicators;
            s.enablePosition = sim.enablePosition;
            s.enableHeightLines = sim.enableHeightLines;
            s.swayRange = sim.swayRange;
            s.strokeRange = sim.strokeRange;
            s.surgeRange = sim.surgeRange;
            s.pitchRange = sim.pitchRange;
            s.rollRange = sim.rollRange;
            s.twistRange = sim.twistRange;
            s.labels3dMask = sim.labels3dMask;
            s.labels3dInDegrees = sim.labels3dInDegrees;
        }});
    }
}

void ConfigurationWindow::renderApplicationTab(EventQueue &eq) {
    // --- Performance ---
    ImGui::SeparatorText(Str::PrefPerformance);
    {
        // Refresh-aware FPS cap. The presets are exact integer divisors of the display refresh so the
        // cap is realized tear-free as a swap interval (Application::updateSwapInterval). The runtime
        // path uses the window's actual display; the preset list reads the primary display, which is
        // the right monitor in the common single-display case.
        float refresh = 0.f;
        if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()))
            refresh = mode->refresh_rate;
        if (refresh <= 1.f)
            refresh = 60.f;
        // Only offer divisors at/above this floor — capping below ~60 FPS is never the intent here.
        constexpr float kFpsFloor = 58.f;
        const int curMax = appSettings.maxFps; // 0 = unlimited

        auto matchDisplayLabel = [&] { return Str::PrefMaxFpsMatchDisplay.fmt(fmtScratch("{:.0f}", refresh)); };
        const char *preview = curMax <= 0 ? matchDisplayLabel() : Str::PrefMaxFpsValue.fmt(fmtScratch("{}", curMax));

        if (beginForm("##perf_form")) {
            formRowHelp(Str::PrefMaxFps, Str::PrefMaxFpsHint.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##max_fps", preview)) {
                if (comboItem(fmtScratch("{}###maxfps_match", matchDisplayLabel()), curMax <= 0))
                    eq.push(ModifyEvent<AppSettings>{[](AppSettings &s) { s.maxFps = 0; }});
                for (int n = 2; refresh / static_cast<float>(n) >= kFpsFloor; ++n) {
                    const int fps = static_cast<int>(std::lround(refresh / static_cast<float>(n)));
                    const char *visible = Str::PrefMaxFpsValue.fmt(fmtScratch("{}", fps));
                    if (comboItem(fmtScratch("{}###maxfps_{}", visible, n), curMax == fps))
                        eq.push(ModifyEvent<AppSettings>{[fps](AppSettings &s) { s.maxFps = fps; }});
                }
                ImGui::EndCombo();
            }

            // Undo history memory budget (MB). Presets only — exact byte tuning isn't useful here.
            formRowHelp(Str::PrefUndoMemory, Str::PrefUndoMemoryHint.c_str());
            const int curUndo = appSettings.undoMemoryLimitMb;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##undo_mem", Str::PrefUndoMemoryValue.fmt(fmtScratch("{}", curUndo)))) {
                for (const int mb : {64, 128, 256, 512, 1024, 2048}) {
                    if (comboItem(fmtScratch("{}###undomem_{}", Str::PrefUndoMemoryValue.fmt(fmtScratch("{}", mb)), mb),
                                  curUndo == mb))
                        eq.push(ModifyEvent<AppSettings>{[mb](AppSettings &s) { s.undoMemoryLimitMb = mb; }});
                }
                ImGui::EndCombo();
            }
            endForm();
        }
    }
    ImGui::Spacing();

    // --- Video ---
    ImGui::SeparatorText(Str::PrefVideo);
    // HW Decoding and Timeline Preview share one row (label-then-toggle, repeated). Checkbox ids stay
    // "##…" (no visible label) so each toggle's label sits to its left like the form rows elsewhere.
    {
        bool hwdec = appSettings.hwdecEnabled;
        if (labeledCheckbox(Str::PrefHwDecoding, "##hwdec", &hwdec))
            eq.push(ModifyEvent<AppSettings>{[hwdec](AppSettings &s) { s.hwdecEnabled = hwdec; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(hwdec ? Str::PrefHardwareDecodingHint.c_str() : Str::PrefSoftwareDecodingHint.c_str());
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
        bool timelinePreview = appSettings.showTimelinePreview;
        if (labeledCheckbox(Str::PrefTimelinePreview, "##timeline_preview", &timelinePreview))
            eq.push(ModifyEvent<AppSettings>{
                [timelinePreview](AppSettings &s) { s.showTimelinePreview = timelinePreview; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefTimelinePreviewHint.c_str());
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
        bool pauseOnSeek = appSettings.pauseOnSeek;
        if (labeledCheckbox(Str::PrefPauseOnSeek, "##pause_on_seek", &pauseOnSeek))
            eq.push(ModifyEvent<AppSettings>{[pauseOnSeek](AppSettings &s) { s.pauseOnSeek = pauseOnSeek; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefPauseOnSeekHint.c_str());
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Str::PrefRestartHint.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // --- Sound (UI feedback SFX) ---
    ImGui::SeparatorText(Str::PrefSound);
    {
        bool uiSounds = appSettings.uiSoundsEnabled;
        if (labeledCheckbox(Str::PrefUiSounds, "##ui_sounds", &uiSounds))
            eq.push(ModifyEvent<AppSettings>{[uiSounds](AppSettings &s) { s.uiSoundsEnabled = uiSounds; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefUiSoundsHint.c_str());
        if (uiSounds) {
            ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
            ImGui::TextUnformatted(Str::PrefUiSoundVolume);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            float volPct = appSettings.uiSoundVolume * 100.0f;
            if (ImGui::SliderFloat("##ui_sound_volume", &volPct, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
                eq.push(ModifyEvent<AppSettings>{[volPct](AppSettings &s) { s.uiSoundVolume = volPct / 100.0f; }});
        }
    }
    ImGui::Spacing();

    // --- Optimized Video (intra-frame transcoding) ---
    // The shared destination for intra-optimized videos. Mandatory and without a default: the
    // optimize command stays disabled until this is set. Cleanup is the
    // user's responsibility, so there's no app-managed pruning here — only a shortcut to open it.
    ImGui::SeparatorText(Str::PrefIntraSection);
    {
        const std::string &dir = appSettings.intraOutputDir;
        const bool isSet = !dir.empty();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;

        // Action buttons follow the path on one row; measure them so the input fills the rest. iconId
        // returns a frame-arena pointer valid for the whole frame, so it can be measured then reused.
        const char *chooseLbl = Str::PrefIntraChooseFolder.iconId(ICON_FOLDER_OPEN, "intra_choose");
        // Reserve the choose button plus the trailing help marker (icon + its leading SameLine gap).
        float reserved = ofs::ui::buttonW(chooseLbl) + spacing + ImGui::CalcTextSize(ICON_CIRCLE_HELP).x + spacing;
        const char *openLbl = nullptr;
        const char *clearLbl = nullptr;
        if (isSet) {
            openLbl = Str::PrefIntraOpenFolder.iconId(ICON_FOLDER_OPEN, "intra_open");
            clearLbl = Str::PrefIntraClear.iconId(ICON_TRASH, "intra_clear");
            reserved += ofs::ui::buttonW(openLbl) + ofs::ui::buttonW(clearLbl) + spacing * 2.f;
        }

        // Read-only input: shows the full (possibly non-ASCII) path and lets the user select/copy it,
        // and its hint placeholder covers the unset case. Fills the row minus the action buttons.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reserved - scrollbarGap());
        ImGui::InputTextWithHint("###intra_dir", Str::PrefIntraOutputDirNotSet.c_str(), const_cast<std::string *>(&dir),
                                 ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(chooseLbl)) {
            pickFile(eq,
                     {.kind = FileDialogKind::SelectFolder,
                      .key = "intra_output",
                      .title = Str::PrefIntraChooseFolderTitle.c_str()},
                     [&eq](std::string path) {
                         if (!path.empty())
                             eq.push(ModifyEvent<AppSettings>{
                                 [path = std::move(path)](AppSettings &s) { s.intraOutputDir = path; }});
                     });
        }
        if (isSet) {
            ImGui::SameLine();
            if (ImGui::Button(openLbl))
                ofs::util::openInFileBrowser(ofs::util::fromUtf8(dir));
            ImGui::SameLine();
            if (ImGui::Button(clearLbl))
                confirmAsync(eq,
                             {.title = Str::PrefIntraClearConfirmTitle.c_str(),
                              .message = Str::PrefIntraClearConfirmBody.c_str(),
                              .buttons = {Str::PrefIntraClear.c_str(), Str::AppCancel.c_str()},
                              .severity = ofs::ModalSeverity::Warning},
                             [eqp = &eq](int idx) {
                                 if (idx == 0)
                                     eqp->push(
                                         ModifyEvent<AppSettings>{[](AppSettings &s) { s.intraOutputDir.clear(); }});
                             });
        }
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefIntraOutputDirHint.c_str());

        // Storage guidance: all-intra copies are large and seek-heavy — local SSD, not a network share.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Warning), "%s", Str::PrefIntraStorageHint.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();

    // --- Export ---
    ImGui::SeparatorText(Str::PrefExportSection);
    {
        auto modeLabel = [](ExportDirMode m) -> TrKey {
            switch (m) {
            case ExportDirMode::VideoFolder:
                return Str::PrefExportDirVideoFolder;
            case ExportDirMode::Custom:
                return Str::PrefExportDirCustom;
            case ExportDirMode::LastUsed:
                break;
            }
            return Str::PrefExportDirLastUsed;
        };
        const ExportDirMode mode = appSettings.exportDirMode;
        if (beginForm("##export_form")) {
            formRowHelp(Str::PrefExportDir, Str::PrefExportDirHint.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##export_dir_mode", modeLabel(mode))) {
                for (const ExportDirMode m :
                     {ExportDirMode::VideoFolder, ExportDirMode::LastUsed, ExportDirMode::Custom})
                    if (comboItem(fmtScratch("{}###exportmode_{}", modeLabel(m).sv(), static_cast<int>(m)), m == mode))
                        eq.push(ModifyEvent<AppSettings>{[m](AppSettings &s) { s.exportDirMode = m; }});
                ImGui::EndCombo();
            }
            // Custom mode: the fixed folder, with a picker. A read-only input shows the full (possibly
            // non-ASCII) path; its hint covers the unset case (which falls back to the last-used folder).
            if (mode == ExportDirMode::Custom) {
                formRow(Str::PrefExportDirCustom);
                const char *chooseLbl = Str::PrefIntraChooseFolder.iconId(ICON_FOLDER_OPEN, "export_dir_choose");
                const float reserved = ofs::ui::buttonW(chooseLbl) + ImGui::GetStyle().ItemSpacing.x;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reserved - scrollbarGap());
                ImGui::InputTextWithHint("###export_dir_path", Str::PrefExportDirNotSet.c_str(),
                                         const_cast<std::string *>(&appSettings.exportDir),
                                         ImGuiInputTextFlags_ReadOnly);
                ImGui::SameLine();
                if (ImGui::Button(chooseLbl))
                    pickFile(eq,
                             {.kind = FileDialogKind::SelectFolder,
                              .key = "export_dir",
                              .title = Str::PrefExportDirChooseTitle.c_str()},
                             [&eq](std::string path) {
                                 if (!path.empty())
                                     eq.push(ModifyEvent<AppSettings>{
                                         [path = std::move(path)](AppSettings &s) { s.exportDir = path; }});
                             });
            }
            endForm();
        }
    }
    ImGui::Spacing();

    // --- Project ---
    ImGui::SeparatorText(Str::PrefProjectSection);
    {
        bool openOnLoad = appSettings.openProjectConfigOnOpen;
        if (labeledCheckbox(Str::PrefOpenProjectConfig, "###open_project_config", &openOnLoad))
            eq.push(ModifyEvent<AppSettings>{[openOnLoad](AppSettings &s) { s.openProjectConfigOnOpen = openOnLoad; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefOpenProjectConfigHint.c_str());
    }
    ImGui::Spacing();

    // --- View ---
    ImGui::SeparatorText(Str::PrefView);
    if (beginForm("##view_form")) {
        formRow(Str::PrefFontSize);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Commit on edit-end (Enter / focus-loss / step button) rather than per-keystroke, so typing a
        // multi-digit size doesn't momentarily clamp through an out-of-range prefix and thrash the layout.
        ImGui::InputInt("##font_size", &fontSizeEdit_);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            fontSizeEdit_ = ImClamp(fontSizeEdit_, 10, 40);
            eq.push(ModifyEvent<AppSettings>{
                [v = fontSizeEdit_](AppSettings &s) { s.fontSizeBase = static_cast<float>(v); }});
        }
        endForm();
    }

    // --- Localization ---
    ImGui::SeparatorText(Str::PrefLocalization);
    {
        // const char* (not std::string) to avoid a per-frame heap allocation in this render path;
        // appSettings.language outlives the frame, so .c_str() stays valid.
        const char *current = appSettings.language.empty() ? "en" : appSettings.language.c_str();
        // The built-in entry's name is an endonym — the language's own name for itself — and must not
        // be translated: it stays "English (built-in)" whatever the active UI language. Other entries
        // show their raw language id (also an endonym), so this literal is consistent, not an omission.
        auto displayName = [](const char *id) -> const char * {
            return std::strcmp(id, "en") == 0 ? "English (built-in)" : id;
        };
        if (beginForm("##loc_form")) {
            formRowHelp(Str::PrefLanguage, Str::PrefLanguagePluginReloadHint.c_str());
            // Combo, Export and Live Reload share the widget column. Reserve room on the right for the
            // Export button (icon + label) and the checkbox (frame box + inner spacing + visible label),
            // then let the combo fill the rest. file-output reads as "export to file" — download (the
            // old glyph) read as the opposite.
            const ImGuiStyle &st = ImGui::GetStyle();
            const char *exportLabel = Str::PrefExportCatalog.iconId(ICON_FILE_OUTPUT, "export_catalog");
            const float btnW = ImGui::CalcTextSize(exportLabel, nullptr, true).x + st.FramePadding.x * 2.f;
            // Label-left: label + SameLine gap + box (see labeledCheckbox), so reserve in that order.
            const float cbW =
                ImGui::CalcTextSize(Str::PrefLiveReload.c_str()).x + st.ItemSpacing.x + ImGui::GetFrameHeight();
            const float hmW = ImGui::CalcTextSize(ICON_CIRCLE_HELP).x;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - cbW - hmW - st.ItemSpacing.x * 3.f);
            const bool comboOpen = ImGui::BeginCombo("##language", displayName(current));
            // Rescan <pref>/lang on the open edge so a just-exported or edited override shows up without
            // reopening Preferences. One scan per open (not per frame): cheap and off the hot path.
            if (comboOpen && !langComboOpen_)
                availableLanguages = ofs::loc::Translator::instance().available();
            langComboOpen_ = comboOpen;
            if (comboOpen) {
                for (const auto &id : availableLanguages) {
                    if (comboItem(displayName(id.c_str()), id == current))
                        eq.push(SetLanguageEvent{.languageId = id});
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            // Export the active language for hand-editing. The catalog lands in <pref>/lang — the only
            // dir load() reads overrides from and the one the combo rescans — so a saved/edited file
            // shows up as a selectable language on its own.
            if (ImGui::Button(exportLabel)) {
                pickFile(eq,
                         {.kind = FileDialogKind::Save,
                          .title = Str::PrefExportCatalogTitle.c_str(),
                          .defaultName = fmt::format("{}.toml", current),
                          .filterPatterns = {"*.toml"},
                          .filterDesc = "TOML",
                          .forceDir = ofs::util::toUtf8(ofs::util::getPrefPath() / "lang")},
                         [&eq](std::string path) {
                             if (!path.empty())
                                 eq.push(ExportCatalogEvent{.path = std::move(path)});
                         });
            }
            ImGui::SameLine();
            bool liveReload = appSettings.liveReloadTranslations;
            if (labeledCheckbox(Str::PrefLiveReload, "###liveReload", &liveReload))
                eq.push(
                    ModifyEvent<AppSettings>{[liveReload](AppSettings &s) { s.liveReloadTranslations = liveReload; }});
            ImGui::SameLine();
            ofs::ui::helpMarker(Str::PrefLiveReloadHint.c_str());
            endForm();
        }
    }
    ImGui::Spacing();

    // --- Backup ---
    ImGui::SeparatorText(Str::PrefBackup);
    {
        // Auto-backup toggle, the retention-count slider, and both folder shortcuts share one row: the
        // checkbox and slider auto-size, the two buttons split the remaining width.
        bool autoBackup = appSettings.autoBackupEnabled;
        if (labeledCheckbox(Str::PrefAutoBackup, "###auto_backup", &autoBackup))
            eq.push(ModifyEvent<AppSettings>{[autoBackup](AppSettings &s) { s.autoBackupEnabled = autoBackup; }});

        // How many timestamped backups to retain per project before the oldest is pruned. No visible
        // label — the help marker explains it.
        ImGui::SameLine();
        ImGui::BeginDisabled(!appSettings.autoBackupEnabled);
        int keepCount = appSettings.backupKeepCount;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::SliderInt("##backup_keep_count", &keepCount, 5, 20))
            eq.push(ModifyEvent<AppSettings>{[keepCount](AppSettings &s) { s.backupKeepCount = keepCount; }});
        ImGui::SameLine();
        ofs::ui::helpMarker(Str::PrefBackupKeepCountHint.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        const float btnW = (ImGui::GetContentRegionAvail().x - scrollbarGap() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(Str::PrefOpenAppDataFolder.icon(ICON_FOLDER_OPEN), {btnW, 0.f}))
            ofs::util::openInFileBrowser(ofs::util::getPrefPath());
        ImGui::SameLine();
        if (ImGui::Button(Str::PrefOpenBackupFolder.icon(ICON_FOLDER_OPEN), {btnW, 0.f}))
            ofs::util::openInFileBrowser(ofs::util::getPrefPath() / "backup");
    }

    ImGui::Spacing();

    // --- Updates ---
    ImGui::SeparatorText(Str::AboutUpdates);
    {
        bool checkUpdates = appSettings.checkForUpdatesOnStartup;
        if (labeledCheckbox(Str::PrefCheckUpdates, "###check_updates", &checkUpdates))
            eq.push(ModifyEvent<AppSettings>{
                [checkUpdates](AppSettings &s) { s.checkForUpdatesOnStartup = checkUpdates; }});
    }
}

namespace {

// Draw a horizontal gradient bar from sorted marks into [min, max].
void drawHGradientBar(ImDrawList *dl, ImVec2 min, ImVec2 max, const std::vector<ofs::ImGradientMark> &marks) {
    float w = max.x - min.x;
    for (size_t i = 0; i + 1 < marks.size(); ++i) {
        const auto &a = marks[i];
        const auto &b = marks[i + 1];
        ImVec2 p0{min.x + a.position * w, min.y};
        ImVec2 p1{min.x + b.position * w, max.y};
        ImU32 ca = ImGui::ColorConvertFloat4ToU32({a.color[0], a.color[1], a.color[2], a.color[3]});
        ImU32 cb = ImGui::ColorConvertFloat4ToU32({b.color[0], b.color[1], b.color[2], b.color[3]});
        dl->AddRectFilledMultiColor(p0, p1, ca, cb, cb, ca);
    }
    dl->AddRect(min, max, ofs::theme::GetColorU32(ImGuiCol_Border));
}

// Data-driven section descriptors. Each section's rows live in a table here rather than a hand-
// written sequence of pairRow() calls so the search filter can pack matching rows and the whole
// section can be hidden when nothing matches. Order is preserved from the original layout.
// Constructors (rather than aggregates) keep the dense tables below free of per-field designators.
struct ColorItem {
    int slot; // AppCol_* / ImGuiCol_*
    const char *label;
    constexpr ColorItem(int s, const char *l) : slot(s), label(l) {}
};
// Which widget a theme-var row renders. All kinds read/write t.vars[slot] (scalar in .x; Vec2 uses .x/.y).
enum class VarWidget { Drag, Vec2, DragInt, Slider };
struct VarItem {
    int slot; // ImGuiStyleVar_* / AppVar_*
    const char *label;
    VarWidget kind;
    float speed, mn, mx; // Drag/DragInt tunables (speed unused by Slider)
    const char *fmt;     // value format ("%.1f" / "%d"); nullptr for Vec2 (uses "%.0f")
    constexpr VarItem(int s, const char *l, VarWidget k, float sp, float lo, float hi, const char *f)
        : slot(s), label(l), kind(k), speed(sp), mn(lo), mx(hi), fmt(f) {}
};
struct NodeColorItem {
    int slot; // ImNodesCol_*
    const char *label;
    constexpr NodeColorItem(int s, const char *l) : slot(s), label(l) {}
};

constexpr ColorItem kAxisColors[] = {
    {AppCol_AxisL0, "L0 Stroke"}, {AppCol_AxisL1, "L1 Surge"}, {AppCol_AxisL2, "L2 Sway"}, {AppCol_AxisR0, "R0 Twist"},
    {AppCol_AxisR1, "R1 Roll"},   {AppCol_AxisR2, "R2 Pitch"}, {AppCol_AxisV0, "V0 Vibe"}, {AppCol_AxisV1, "V1 Vibe2"},
    {AppCol_AxisA0, "A0 Air"},    {AppCol_AxisA1, "A1 Air2"},  {AppCol_AxisS0, "S0"},      {AppCol_AxisS1, "S1"},
    {AppCol_AxisS2, "S2"},        {AppCol_AxisS3, "S3"},       {AppCol_AxisS4, "S4"},      {AppCol_AxisS5, "S5"},
    {AppCol_AxisS6, "S6"},        {AppCol_AxisS7, "S7"},       {AppCol_AxisS8, "S8"},      {AppCol_AxisS9, "S9"},
};

constexpr ColorItem kHeatmapBase[] = {{AppCol_HeatmapBase, "Base Color"}};

constexpr ColorItem kTimelineColors[] = {
    {AppCol_ScriptLineBgTop, "Script Line BG Top"},
    {AppCol_ScriptLineBgBottom, "Script Line BG Bottom"},
    {AppCol_ScriptLineHoverBg, "Script Line Hover"},
    {AppCol_Waveform, "Waveform"},
    {AppCol_GridLine, "Grid Line"},
    {AppCol_GridLineMid, "Grid Line Mid"},
    {AppCol_StripBg, "Strip BG"},
    {AppCol_StripActiveBg, "Strip Active"},
    {AppCol_StripHoverBg, "Strip Hover"},
    {AppCol_StripSeparator, "Strip Sep."},
    {AppCol_StripDivider, "Strip Divider"},
    {AppCol_LockIndicator, "Lock Indicator"},
    {AppCol_SelectedLine, "Selected Line"},
    {AppCol_DragPreview, "Drag Preview"},
    {AppCol_DragPreviewOutline, "Drag Outline"},
    {AppCol_SelectionBox, "Selection Box"},
    {AppCol_SelectionBoxFill, "Selection Fill"},
    {AppCol_TimelineOutline, "Point Outline"},
    {AppCol_TimelinePoint, "Point"},
    {AppCol_TimelinePointSelected, "Point Sel."},
    {AppCol_OverlayLineMajor, "Overlay Major"},
    {AppCol_OverlayLineMinor, "Overlay Minor"},
    {AppCol_ScriptSeekCursor, "Seek Cursor"},
    {AppCol_ScriptPlayCursor, "Play Cursor"},
    {AppCol_TempoMeasureLine, "Tempo Measure"},
};

constexpr ColorItem kVideoColors[] = {
    {AppCol_PlayCursor, "Play Cursor"},
    {AppCol_TimelineCursorOuter, "Cursor Outer"},
    {AppCol_TimelineCursorInner, "Cursor Inner"},
    {AppCol_HudBg, "HUD BG"},
    {AppCol_VideoTimelineFill, "Seek Fill"},
    {AppCol_TimelineVisibleRegionFill, "Visible Fill"},
    {AppCol_TimelineVisibleRegionBorder, "Visible Border"},
};

constexpr ColorItem kSim3dColors[] = {
    {AppCol_SimViewBg, "View BG"},  {AppCol_SimTintTop, "Top Tint"},    {AppCol_SimTintSide, "Side Tint"},
    {AppCol_SimDivider, "Divider"}, {AppCol_SimCrosshair, "Crosshair"}, {AppCol_SimArcRef, "Arc Ref."},
    {AppCol_SimArc, "Arc Active"},
};

constexpr ColorItem kSim2dColors[] = {
    {AppCol_Sim2DFront, "Front"}, {AppCol_Sim2DBack, "Back"},   {AppCol_Sim2DBorder, "Border"},
    {AppCol_Sim2DText, "Text"},   {AppCol_Sim2DLines, "Lines"}, {AppCol_Sim2DIndicator, "Indicator"},
};

constexpr ColorItem kBookmarkColors[] = {
    {AppCol_Bookmark, "Bookmark"},
    {AppCol_BookmarkDot, "Bookmark Dot"},
    {AppCol_BookmarkDotHovered, "Dot Hovered"},
    {AppCol_BookmarkOutline, "Outline"},
    {AppCol_BandBarStripe, "Band Stripe"},
    {AppCol_BandBarText, "Band Text"},
    {AppCol_TextShadow, "Text Shadow"},
    {AppCol_UnsavedIndicator, "Unsaved Indicator"},
    {AppCol_Success, "Success"},
    {AppCol_Warning, "Warning"},
    {AppCol_Error, "Error"},
};

constexpr ColorItem kProcessingColors[] = {
    {AppCol_ProcessingPanelBg, "Panel BG"},
    {AppCol_NodeIO, "Node: I/O"},
    {AppCol_NodeMath, "Node: Math"},
    {AppCol_NodeDiscrete, "Node: Discrete"},
    {AppCol_NodeFunctional, "Node: Functional"},
    {AppCol_LinkDiscrete, "Link: Discrete"},
    {AppCol_LinkFunctional, "Link: Functional"},
};

constexpr VarItem kGeometryVars[] = {
    {ImGuiStyleVar_WindowRounding, "Window Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_ChildRounding, "Child Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_FrameRounding, "Frame Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_PopupRounding, "Popup Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_ScrollbarRounding, "Scrollbar Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_GrabRounding, "Grab Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_TabRounding, "Tab Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {ImGuiStyleVar_WindowBorderSize, "Window Border", VarWidget::Drag, 0.05f, 0.f, 3.f, "%.2f"},
    {ImGuiStyleVar_FrameBorderSize, "Frame Border", VarWidget::Drag, 0.05f, 0.f, 3.f, "%.2f"},
    {ImGuiStyleVar_WindowPadding, "Window Padding", VarWidget::Vec2, 0.2f, 0.f, 30.f, nullptr},
    {ImGuiStyleVar_FramePadding, "Frame Padding", VarWidget::Vec2, 0.2f, 0.f, 30.f, nullptr},
    {ImGuiStyleVar_ItemSpacing, "Item Spacing", VarWidget::Vec2, 0.2f, 0.f, 30.f, nullptr},
    {ImGuiStyleVar_CellPadding, "Cell Padding", VarWidget::Vec2, 0.2f, 0.f, 30.f, nullptr},
};

constexpr VarItem kNodeGeomVars[] = {
    {AppVar_NodeGridSpacing, "Grid Spacing", VarWidget::Drag, 0.2f, 4.f, 64.f, "%.0f"},
    {AppVar_NodeCornerRounding, "Corner Rounding", VarWidget::Drag, 0.1f, 0.f, 16.f, "%.1f"},
    {AppVar_NodeBorderThickness, "Border Thickness", VarWidget::Drag, 0.05f, 0.f, 5.f, "%.2f"},
    {AppVar_NodeLinkThickness, "Link Thickness", VarWidget::Drag, 0.05f, 0.5f, 8.f, "%.2f"},
    {AppVar_NodePinRadius, "Pin Radius", VarWidget::Drag, 0.05f, 1.f, 12.f, "%.2f"},
    {AppVar_NodePadding, "Node Padding", VarWidget::Vec2, 0.2f, 0.f, 30.f, nullptr},
};

// Timeline stroke/width metrics — same t.vars[] backing store as the geometry sections, rendered
// through the same two-per-row var grid so they gain reset buttons and a consistent layout.
constexpr VarItem kTimelineVars[] = {
    {AppVar_ScriptSeekCursorWidth, "Seek Cursor Width", VarWidget::DragInt, 1.f, 1.f, 4.f, "%d"},
    {AppVar_ScriptPlayCursorWidth, "Play Cursor Width", VarWidget::DragInt, 1.f, 1.f, 4.f, "%d"},
    {AppVar_GridLineMidWidth, "Grid Mid Width", VarWidget::Drag, 0.1f, 0.5f, 4.f, "%.1f"},
    {AppVar_OverlayLineMajorWidth, "Overlay Major Width", VarWidget::Drag, 0.1f, 0.5f, 4.f, "%.1f"},
    {AppVar_TimelineLineWidth, "Line Width", VarWidget::Drag, 0.1f, 1.f, 6.f, "%.1f"},
    {AppVar_WaveformScale, "Waveform Height", VarWidget::Slider, 0.f, 0.1f, 1.f, "%.2f"},
};

constexpr NodeColorItem kNodeColors[] = {
    {ImNodesCol_GridBackground, "Grid Background"},
    {ImNodesCol_GridLine, "Grid Line"},
    {ImNodesCol_GridLinePrimary, "Grid Line Primary"},
    {ImNodesCol_NodeBackground, "Node Background"},
    {ImNodesCol_NodeOutline, "Node Outline"},
    {ImNodesCol_BoxSelector, "Box Selector"},
    {ImNodesCol_BoxSelectorOutline, "Box Selector Outline"},
    {ImNodesCol_Pin, "Pin"},
};

} // namespace

void ConfigurationWindow::renderThemeTab(const ScriptProject &project, EventQueue &eq) {
    ofs::theme::Theme &t = ofs::theme::getActive();
    const float avail = ImGui::GetContentRegionAvail().x - scrollbarGap();

    const ofs::theme::Theme &defaults = t.isDark ? ofs::theme::defaultDark() : ofs::theme::defaultLight();

    auto applyAndSave = [&] {
        ofs::theme::apply(t);
        ofs::theme::save();
    };

    constexpr ImGuiColorEditFlags kColFlags =
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
    // Sized from live frame metrics, not constants: the color swatch is a square of one
    // frame-height and the "R" reset SmallButton is text + 2x FramePadding.x. Hardcoded
    // widths clipped the rightmost reset column once FramePadding grew (square-corner theme).
    const float kColorW = ImGui::GetFrameHeight();
    const float kResetW = ImGui::CalcTextSize(ICON_RESET).x + ImGui::GetStyle().FramePadding.x * 2.f;

    // Begin the shared 6-column pair table (label|color|reset|label|color|reset).
    auto beginPairTable = [&](const char *id) -> bool {
        if (!ImGui::BeginTable(id, 6, ImGuiTableFlags_SizingFixedFit, {avail, 0}))
            return false;
        ImGui::TableSetupColumn("##l1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthFixed, kColorW);
        ImGui::TableSetupColumn("##r1", ImGuiTableColumnFlags_WidthFixed, kResetW);
        ImGui::TableSetupColumn("##l2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthFixed, kColorW);
        ImGui::TableSetupColumn("##r2", ImGuiTableColumnFlags_WidthFixed, kResetW);
        return true;
    };

    // One color entry (half of a pair row). side=0 → left columns, side=1 → right columns.
    auto colorEntry = [&](int side, int slot, const char *label) -> bool {
        bool changed = false;
        ImGui::TableSetColumnIndex(side * 3 + 0);
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(side * 3 + 1);
        {
            ImColor &c = t.colors[slot];
            float v[4] = {c.Value.x, c.Value.y, c.Value.z, c.Value.w};
            // Stable ###col<slot> / ###colreset<slot> ids bake the AppCol slot in, so each swatch and
            // its reset button is uniquely test-addressable regardless of the visible label text.
            if (ImGui::ColorEdit4(fmtScratch("###col{}", slot), v, kColFlags)) {
                c = ImColor(v[0], v[1], v[2], v[3]);
                changed = true;
            }
        }
        ImGui::TableSetColumnIndex(side * 3 + 2);
        if (resetButton(fmtScratch("###colreset{}", slot), Str::PrefResetToDefaultTip.c_str())) {
            t.colors[slot] = defaults.colors[slot];
            changed = true;
        }
        return changed;
    };

    // Live search predicate. fuzzyMatch returns matched=true for an empty query, so an inactive search
    // shows every row; !empty() gates the force-open / hide-empty-section logic.
    auto pass = [&](const char *label) { return ofs::util::fuzzyMatch(themeFilter_, label).matched; };
    auto countMatch = [&](auto items) {
        int n = 0;
        for (const auto &it : items)
            if (pass(it.label))
                ++n;
        return n;
    };

    // Set whenever a section is shown because it has a filter hit; drives the "no matches" note.
    bool sawMatch = false;

    // Collapsing header that auto-opens while searching and is skipped entirely when its section
    // has no matching rows, mirroring the shortcut window's per-group filtering.
    auto beginSection = [&](const char *header, bool anyMatch, bool defaultOpen) -> bool {
        const bool active = !themeFilter_.empty();
        if (active && !anyMatch)
            return false;
        if (active) {
            sawMatch = true;
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        return ImGui::CollapsingHeader(header, (defaultOpen && !active) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    };

    // Render `items` (filtered) as a flowing two-per-row swatch grid: matches pack left-to-right so
    // hidden rows leave no gaps. Returns true if any swatch changed.
    auto colorGrid = [&](const char *tableId, std::span<const ColorItem> items) -> bool {
        auto *m = ofs::FrameAllocator::instance().allocArray<ColorItem>(items.size());
        int n = 0;
        for (const auto &it : items)
            if (pass(it.label))
                m[n++] = it;
        if (n == 0 || !beginPairTable(tableId))
            return false;
        bool changed = false;
        for (int i = 0; i < n; i += 2) {
            ImGui::TableNextRow();
            changed |= colorEntry(0, m[i].slot, m[i].label);
            if (i + 1 < n)
                changed |= colorEntry(1, m[i + 1].slot, m[i + 1].label);
        }
        ImGui::EndTable();
        return changed;
    };

    // A whole pure-color section: header + flowing grid + persist on change.
    auto colorSection = [&](const char *header, const char *tableId, std::span<const ColorItem> items,
                            bool defaultOpen) {
        if (!beginSection(header, countMatch(items) > 0, defaultOpen))
            return;
        if (colorGrid(tableId, items))
            applyAndSave();
    };

    // Begin the shared 6-column var pair table (label|value|reset | label|value|reset). Mirrors the
    // color pair table, but here the *value* column stretches (drag/slider) and the label auto-fits.
    auto beginVarPairTable = [&](const char *id) -> bool {
        if (!ImGui::BeginTable(id, 6, ImGuiTableFlags_SizingFixedFit, {avail, 0}))
            return false;
        ImGui::TableSetupColumn("##l1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##v1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##r1", ImGuiTableColumnFlags_WidthFixed, kResetW);
        ImGui::TableSetupColumn("##l2", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##v2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##r2", ImGuiTableColumnFlags_WidthFixed, kResetW);
        return true;
    };

    // One variable entry (half of a pair row). side=0 → left columns, side=1 → right columns. The value
    // widget fills its column and the reset lives in its own fixed column (like colorEntry), so a
    // SameLine-placed button can never overrun into the scrollbar. Stable ###var<slot> / ###varreset<slot>
    // ids bake the vars[] slot in (unique across every var table), keeping rows test-addressable.
    auto varEntry = [&](int side, const VarItem &it) -> bool {
        const int slot = it.slot;
        ImGui::TableSetColumnIndex(side * 3 + 0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(it.label);
        ImGui::TableSetColumnIndex(side * 3 + 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool changed = false;
        const char *id = fmtScratch("###var{}", slot);
        switch (it.kind) {
        case VarWidget::Vec2:
            changed = ImGui::DragFloat2(id, &t.vars[slot].x, it.speed, it.mn, it.mx, "%.0f");
            break;
        case VarWidget::DragInt: {
            int iv = static_cast<int>(std::lround(t.vars[slot].x));
            if (ImGui::DragInt(id, &iv, it.speed, static_cast<int>(it.mn), static_cast<int>(it.mx), it.fmt)) {
                t.vars[slot].x = static_cast<float>(iv);
                changed = true;
            }
            break;
        }
        case VarWidget::Slider:
            changed = ImGui::SliderFloat(id, &t.vars[slot].x, it.mn, it.mx, it.fmt);
            break;
        case VarWidget::Drag:
            changed = ImGui::DragFloat(id, &t.vars[slot].x, it.speed, it.mn, it.mx, it.fmt);
            break;
        }
        ImGui::TableSetColumnIndex(side * 3 + 2);
        if (resetButton(fmtScratch("###varreset{}", slot), Str::PrefResetToDefaultTip.c_str())) {
            t.vars[slot] = defaults.vars[slot];
            changed = true;
        }
        return changed;
    };

    // Render `items` (filtered) as a flowing two-per-row var grid, mirroring colorGrid: matches pack
    // left-to-right so hidden rows leave no gaps. Returns true if any row changed.
    auto varGrid = [&](const char *tableId, std::span<const VarItem> items) -> bool {
        auto *m = ofs::FrameAllocator::instance().allocArray<VarItem>(items.size());
        int n = 0;
        for (const auto &it : items)
            if (pass(it.label))
                m[n++] = it;
        if (n == 0 || !beginVarPairTable(tableId))
            return false;
        bool changed = false;
        for (int i = 0; i < n; i += 2) {
            ImGui::TableNextRow();
            changed |= varEntry(0, m[i]);
            if (i + 1 < n)
                changed |= varEntry(1, m[i + 1]);
        }
        ImGui::EndTable();
        return changed;
    };

    // A lone full-width 0..1 slider + reset for a single metric that doesn't pair into the two-per-row
    // grid (e.g. an opacity). Same label|value|reset column shape as varEntry, so the reset sits in its
    // own fixed column and can't crowd the scrollbar. `valueId`/`resetId` carry the stable ### suffixes.
    auto floatRow = [&](const char *tableId, const char *label, const char *valueId, const char *resetId, float *val,
                        float defVal) -> bool {
        if (!ImGui::BeginTable(tableId, 3, ImGuiTableFlags_SizingFixedFit, {avail, 0}))
            return false;
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##r", ImGuiTableColumnFlags_WidthFixed, kResetW);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool changed = ImGui::SliderFloat(valueId, val, 0.f, 1.f, "%.2f");
        ImGui::TableSetColumnIndex(2);
        if (resetButton(resetId, Str::PrefResetToDefaultTip.c_str())) {
            *val = defVal;
            changed = true;
        }
        ImGui::EndTable();
        return changed;
    };

    // One imnodes structural-color entry (half of a pair row); mirrors colorEntry but reads/writes
    // the packed-u32 t.nodes.Colors[] rather than t.colors[]. Stable ###ncol<slot> ids.
    auto nodeColorEntry = [&](int side, int slot, const char *label) -> bool {
        bool changed = false;
        ImGui::TableSetColumnIndex(side * 3 + 0);
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(side * 3 + 1);
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(t.nodes.Colors[slot]);
        if (ImGui::ColorEdit4(fmtScratch("###ncol{}", slot), &v.x, kColFlags)) {
            t.nodes.Colors[slot] = ImGui::ColorConvertFloat4ToU32(v);
            changed = true;
        }
        ImGui::TableSetColumnIndex(side * 3 + 2);
        if (resetButton(fmtScratch("###ncolreset{}", slot), Str::PrefResetToDefaultTip.c_str())) {
            t.nodes.Colors[slot] = defaults.nodes.Colors[slot];
            changed = true;
        }
        return changed;
    };

    // Filtered imnodes structural colors as a flowing two-per-row swatch grid (matches the other
    // color sections). Colors are packed u32 in t.nodes.Colors[].
    auto nodeColorGrid = [&](const char *tableId, std::span<const NodeColorItem> items) -> bool {
        auto *m = ofs::FrameAllocator::instance().allocArray<NodeColorItem>(items.size());
        int n = 0;
        for (const auto &it : items)
            if (pass(it.label))
                m[n++] = it;
        if (n == 0 || !beginPairTable(tableId))
            return false;
        bool changed = false;
        for (int i = 0; i < n; i += 2) {
            ImGui::TableNextRow();
            changed |= nodeColorEntry(0, m[i].slot, m[i].label);
            if (i + 1 < n)
                changed |= nodeColorEntry(1, m[i + 1].slot, m[i + 1].label);
        }
        ImGui::EndTable();
        return changed;
    };

    // Gradient editor: preview bar + stops laid out left-to-right and wrapped, so they fill the
    // available width instead of stacking one stop per row. Returns true when changed.
    auto gradientEditor = [&](const char *id, ofs::ImGradient &gradient) -> bool {
        bool changed = false;
        auto &marks = gradient.getMarks();
        {
            const float kBarH = ImGui::GetFontSize();
            ImVec2 barMin = ImGui::GetCursorScreenPos();
            drawHGradientBar(ImGui::GetWindowDrawList(), barMin, {barMin.x + avail, barMin.y + kBarH}, marks);
            ImGui::Dummy({avail, kBarH});
        }
        ImGui::PushID(id);
        const ImGuiStyle &style = ImGui::GetStyle();
        // Position field holds a "%.3f" in [0,1] — size it to that sample so it scales with font/DPI
        // instead of a fixed pixel width. ("0.000" is a numeric format sample, not translatable text.)
        const float kPosW = ImGui::CalcTextSize("0.000").x + style.FramePadding.x * 2.f;
        // Each stop is a fixed-width [swatch | position | remove] group; wrap to a new line when the
        // next group would overflow the panel's right edge.
        const float groupW = ImGui::GetFrameHeight() + kPosW + ImGui::CalcTextSize("-").x + style.FramePadding.x * 2.f +
                             style.ItemSpacing.x * 2.f;
        const float rightX = ImGui::GetCursorScreenPos().x + avail;
        for (int i = 0; i < static_cast<int>(marks.size()); ++i) {
            ImGui::PushID(i);
            ImGui::BeginGroup();
            float col[4] = {marks[i].color[0], marks[i].color[1], marks[i].color[2], marks[i].color[3]};
            if (ImGui::ColorEdit4("##c", col, kColFlags)) {
                marks[i].color[0] = col[0];
                marks[i].color[1] = col[1];
                marks[i].color[2] = col[2];
                marks[i].color[3] = col[3];
                changed = true;
            }
            ImGui::SameLine();
            float pos = marks[i].position;
            ImGui::SetNextItemWidth(kPosW);
            if (ImGui::DragFloat("##p", &pos, 0.002f, 0.f, 1.f, "%.3f")) {
                marks[i].position = ImClamp(pos, 0.f, 1.f);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(static_cast<int>(marks.size()) <= 2);
            if (ImGui::SmallButton("-")) {
                marks.erase(marks.begin() + i);
                --i;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::EndGroup();
            const float nextX = ImGui::GetItemRectMax().x + style.ItemSpacing.x + groupW;
            if (i + 1 < static_cast<int>(marks.size()) && nextX < rightX)
                ImGui::SameLine();
            ImGui::PopID();
        }
        if (static_cast<int>(marks.size()) < 8 && ImGui::SmallButton("+")) {
            // New stops always append at the back (position 1.0, the fast end of the gradient).
            marks.emplace_back(0.5f, 0.5f, 0.5f, 1.f, 1.0f);
            changed = true;
        }
        ImGui::PopID();
        if (changed)
            gradient.refreshCache();
        return changed;
    };

    // --- Theme browser ---
    ImGui::SeparatorText(Str::PrefTheme);
    {
        bool activeShipped = false;
        for (const auto &info : availableThemes)
            if (info.name == appSettings.activeTheme)
                activeShipped = info.shipped;

        // Switch the active theme: load from disk/shipped, apply, persist the choice.
        auto switchTo = [&](const std::string &name) {
            ofs::theme::Theme loaded;
            if (ofs::theme::load(name, &loaded)) {
                eq.push(ModifyEvent<AppSettings>{[name](AppSettings &s) { s.activeTheme = name; }});
                ofs::theme::apply(loaded); // post-apply hook re-captures DPI base
            }
        };

        // Selector fills the row, with the two trailing actions reserved at its right. Reserve their
        // *visible* width (CalcTextSize hides text after "##", so the ###id doesn't inflate the measure)
        // so the combo grows/shrinks with the panel and translated button labels instead of a fixed px.
        const ImGuiStyle &style = ImGui::GetStyle();
        const char *revealLbl = Str::PrefRevealInFolder.iconId(ICON_FOLDER_OPEN, "theme_reveal");
        const char *resetLbl = Str::PrefResetToDefaults.iconId(ICON_RESET, "theme_reset_all");
        auto buttonW = [&](const char *l) {
            return ImGui::CalcTextSize(l, nullptr, true).x + style.FramePadding.x * 2.f;
        };
        float reserved = buttonW(revealLbl) + buttonW(resetLbl) + style.ItemSpacing.x * 2.f;
        // A shipped theme grows a trailing help marker explaining its read-only behaviour; reserve its
        // width only then so a user theme's selector still fills the whole row.
        if (activeShipped)
            reserved += ImGui::CalcTextSize(ICON_CIRCLE_HELP).x + style.ItemSpacing.x;
        ImGui::SetNextItemWidth(ImMax(ImGui::GetFontSize() * 8.f, avail - reserved));
        if (ImGui::BeginCombo("###theme_select", appSettings.activeTheme.c_str())) {
            for (const auto &info : availableThemes) {
                const bool selected = info.name == appSettings.activeTheme;
                const char *visible = info.shipped ? Str::PrefThemeShippedSuffix.fmt(info.name) : info.name.c_str();
                // Stable ###id keyed on the (untranslated) theme name: the "(shipped)" suffix is
                // localized, so without this the item's ImGui id would shift per language.
                const char *label = fmtScratch("{}###theme_opt_{}", visible, info.name);
                if (comboItem(label, selected))
                    switchTo(info.name);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(revealLbl))
            ofs::util::openInFileBrowser(ofs::util::getPrefPath() / "themes");
        ImGui::SameLine();
        if (ImGui::Button(resetLbl)) {
            confirmAsync(eq,
                         {.title = Str::PrefThemeResetConfirmTitle.c_str(),
                          .message = Str::PrefThemeResetConfirmBody.c_str(),
                          .buttons = {Str::PrefResetToDefaults.c_str(), Str::AppCancel.c_str()},
                          .severity = ofs::ModalSeverity::Warning},
                         [](int idx) {
                             if (idx != 0)
                                 return;
                             ofs::theme::Theme &at = ofs::theme::getActive();
                             at = at.isDark ? ofs::theme::defaultDark() : ofs::theme::defaultLight();
                             ofs::theme::apply(at);
                             ofs::theme::save();
                         });
        }
        if (activeShipped) {
            ImGui::SameLine();
            ofs::ui::helpMarker(Str::PrefShippedReadOnlyHint.c_str());
        }

        // New-theme name on its own full-width row, then two equal-width action rows that each fill the
        // panel width and align as a 2×2 grid: [Save As | Delete] over [Export | Import]. Width is `avail`
        // (not -FLT_MIN) so the input's right edge lines up with the button rows, which stop at `avail`;
        // -FLT_MIN would run it kRightGap past them.
        ImGui::SetNextItemWidth(avail);
        ImGui::InputTextWithHint("###theme_name", Str::PrefNewThemeNameHint.c_str(), &themeName_);

        const float halfW = (avail - style.ItemSpacing.x) * 0.5f;

        ImGui::BeginDisabled(themeName_.empty());
        if (ImGui::Button(Str::PrefSaveAs.iconId(ICON_SAVE, "theme_saveas"), {halfW, 0.f})) {
            if (ofs::theme::saveAs(t, themeName_)) {
                t.name = themeName_;
                eq.push(ModifyEvent<AppSettings>{[name = themeName_](AppSettings &s) { s.activeTheme = name; }});
                ofs::theme::apply(t);
                availableThemes = ofs::theme::list();
                eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PrefThemeSaved.fmt(t.name)});
                themeName_.clear();
            } else {
                eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PrefThemeSaveFailed.fmt(themeName_)});
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(activeShipped);
        if (ImGui::Button(Str::PrefDelete.iconId(ICON_TRASH, "theme_delete"), {halfW, 0.f})) {
            confirmAsync(eq,
                         {.title = Str::PrefThemeDeleteConfirmTitle.c_str(),
                          .message = Str::PrefThemeDeleteConfirmBody.fmt(appSettings.activeTheme),
                          .buttons = {Str::PrefDelete.c_str(), Str::AppCancel.c_str()},
                          .severity = ofs::ModalSeverity::Warning},
                         [this, eqp = &eq](int idx) {
                             if (idx != 0)
                                 return;
                             ofs::theme::remove(appSettings.activeTheme);
                             availableThemes = ofs::theme::list();
                             ofs::theme::Theme loaded;
                             if (ofs::theme::load("Dark", &loaded)) {
                                 eqp->push(ModifyEvent<AppSettings>{[](AppSettings &s) { s.activeTheme = "Dark"; }});
                                 ofs::theme::apply(loaded);
                             }
                         });
        }
        ImGui::EndDisabled();

        // Share path: export the current (possibly edited) theme to a chosen file, or
        // pull an external .json into <pref>/themes/ so it joins the dropdown.
        if (ImGui::Button(Str::PrefExport.iconId(ICON_EXPORT, "theme_export"), {halfW, 0.f})) {
            std::string defName = fmtScratch("{}.json", appSettings.activeTheme);
            // Re-fetch the active theme in the callback: it is a stable global, and exporting what
            // is active when the dialog resolves matches the old blocking behavior.
            pickFile(eq,
                     {.kind = FileDialogKind::Save,
                      .key = "theme",
                      .title = Str::PrefExportThemeTitle.c_str(),
                      .defaultName = std::move(defName),
                      .filterPatterns = {"*.json"},
                      .filterDesc = "Theme JSON"},
                     [&eq](const std::string &dest) {
                         if (dest.empty())
                             return;
                         const bool ok = ofs::theme::exportToFile(ofs::theme::getActive(), ofs::util::fromUtf8(dest));
                         eq.push(NotifyEvent{.level = ok ? NotifyLevel::Success : NotifyLevel::Error,
                                             .message = ok ? Str::PrefThemeExported.c_str()
                                                           : Str::PrefThemeExportFailed.c_str()});
                     });
        }
        ImGui::SameLine();
        if (ImGui::Button(Str::PrefImport.iconId(ICON_FOLDER_OPEN, "theme_import"), {halfW, 0.f})) {
            // Inlines switchTo()'s body (a frame-local lambda above): this callback runs in a later
            // frame, after renderThemeTab has returned, so it may touch only members, not locals.
            pickFile(eq,
                     {.kind = FileDialogKind::Open,
                      .key = "theme",
                      .title = Str::PrefImportThemeTitle.c_str(),
                      .filterPatterns = {"*.json"},
                      .filterDesc = "Theme JSON"},
                     [this, &eq](const std::string &src) {
                         if (src.empty())
                             return;
                         auto name = ofs::theme::importFromFile(ofs::util::fromUtf8(src));
                         if (!name) {
                             eq.push(NotifyEvent{.level = NotifyLevel::Error,
                                                 .message = Str::PrefThemeImportFailed.c_str()});
                             return;
                         }
                         availableThemes = ofs::theme::list();
                         ofs::theme::Theme loaded;
                         if (ofs::theme::load(*name, &loaded)) {
                             eq.push(ModifyEvent<AppSettings>{[n = *name](AppSettings &s) { s.activeTheme = n; }});
                             ofs::theme::apply(loaded); // post-apply hook re-captures DPI base
                             eq.push(NotifyEvent{.level = NotifyLevel::Success,
                                                 .message = Str::PrefThemeImported.fmt(name->c_str())});
                         }
                     });
        }
    }
    ImGui::Spacing();

    // --- Search ---
    // Mirrors the shortcut window: an InputTextWithHint driving the fuzzy search, focusable with
    // Ctrl+F. Stable "###theme_filter" id. Sections below pack matching rows and hide when empty.
    if (ofs::ui::shortcutFocusSearch())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(avail);
    ImGui::InputTextWithHint("###theme_filter", fmtScratch("{}  {}", ICON_SEARCH, Str::PrefFilterHint.sv()),
                             &themeFilter_);
    ImGui::Spacing();

    // --- Axis Colors ---
    {
        // "BG Axes" opacity lives with the axis colors it governs: it sets how strongly the
        // non-active axes are drawn behind the timeline. It's a theme value, so it saves with the theme.
        const bool bgAxes = pass(Str::PrefBgAxes);
        if (beginSection(Str::PrefSecAxisColors.id("sec_axis"), countMatch(std::span(kAxisColors)) > 0 || bgAxes,
                         true)) {
            if (colorGrid("##axis", kAxisColors))
                applyAndSave();
            if (bgAxes && floatRow("##bg_axes_form", Str::PrefBgAxes, "##bg_axes", "###bgaxesreset",
                                   &t.backgroundAxisOpacity, defaults.backgroundAxisOpacity))
                applyAndSave();
        }
    }

    // --- Heatmap (gradient + max speed + base color) ---
    {
        // Synthetic keyword labels so a search for "heatmap", "gradient", "speed", etc. reveals the
        // matching sub-block even though those words aren't a visible row label.
        const bool grad = pass("Heatmap Colors gradient");
        const bool speed = pass("Heatmap Max speed");
        if (beginSection(Str::PrefSecHeatmap.id("sec_heatmap"),
                         grad || speed || countMatch(std::span(kHeatmapBase)) > 0, true)) {
            if (grad) {
                ImGui::TextDisabled("%s", Str::PrefHeatmapColorsCaption.c_str());
                ImGui::SameLine();
                if (resetButton("##hmColors", Str::PrefResetGradientTip.c_str())) {
                    t.heatmapColors = defaults.heatmapColors;
                    applyAndSave();
                }
                if (gradientEditor("##hmColors", t.heatmapColors))
                    applyAndSave();
            }
            // Max speed and base color share one row — each is a single control that wastes a whole
            // row on its own. Max-speed label sits in front of a compact drag; the base swatch follows.
            const bool base = countMatch(std::span(kHeatmapBase)) > 0;
            if (speed) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Str::PrefHeatmapMaxSpeedCaption);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.f);
                if (ImGui::DragFloat("##hmMaxSpeed", &t.heatmapMaxSpeed, 5.f, 50.f, 4000.f, "%.0f"))
                    applyAndSave();
                ImGui::SameLine();
                if (resetButton("##hmMaxSpeed", Str::PrefResetMaxSpeedTip.c_str())) {
                    t.heatmapMaxSpeed = defaults.heatmapMaxSpeed;
                    applyAndSave();
                }
            }
            if (base) {
                // Zero-speed base color (blends into the timeline background at low speed). Inline
                // swatch rather than a one-cell grid so it can share the max-speed row.
                if (speed)
                    ImGui::SameLine(0.f, ImGui::GetFontSize() * 1.5f);
                const int slot = kHeatmapBase[0].slot;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(kHeatmapBase[0].label);
                ImGui::SameLine();
                ImColor &c = t.colors[slot];
                float v[4] = {c.Value.x, c.Value.y, c.Value.z, c.Value.w};
                if (ImGui::ColorEdit4(fmtScratch("###col{}", slot), v, kColFlags)) {
                    c = ImColor(v[0], v[1], v[2], v[3]);
                    applyAndSave();
                }
                ImGui::SameLine();
                if (resetButton(fmtScratch("###colreset{}", slot), Str::PrefResetToDefaultTip.c_str())) {
                    t.colors[slot] = defaults.colors[slot];
                    applyAndSave();
                }
            }
        }
    }

    // --- Timeline ---
    {
        if (beginSection(Str::PrefSecTimeline.id("sec_timeline"),
                         countMatch(std::span(kTimelineColors)) > 0 || countMatch(std::span(kTimelineVars)) > 0,
                         true)) {
            if (colorGrid("##tl", kTimelineColors))
                applyAndSave();
            if (varGrid("##tlwidths", kTimelineVars))
                applyAndSave();
            // Script-line BG gradient preview (vertical, top → bottom). Decorative — hidden while searching.
            if (themeFilter_.empty()) {
                constexpr float kBarH = 18.f;
                ImVec2 barMin = ImGui::GetCursorScreenPos();
                ImVec2 barMax{barMin.x + avail, barMin.y + kBarH};
                ImU32 ct = ImGui::ColorConvertFloat4ToU32(t.colors[AppCol_ScriptLineBgTop].Value);
                ImU32 cb = ImGui::ColorConvertFloat4ToU32(t.colors[AppCol_ScriptLineBgBottom].Value);
                ImGui::GetWindowDrawList()->AddRectFilledMultiColor(barMin, barMax, ct, ct, cb, cb);
                ImGui::GetWindowDrawList()->AddRect(barMin, barMax, ofs::theme::GetColorU32(ImGuiCol_Border));
                ImGui::Dummy({avail, kBarH});
            }
        }
    }

    // --- Video Controls ---
    colorSection(Str::PrefSecVideoControls.id("sec_video"), "##vc", kVideoColors, true);

    // --- Simulator ---
    {
        const int n3d = countMatch(std::span(kSim3dColors));
        const int n2d = countMatch(std::span(kSim2dColors));
        const bool opacity = pass("Opacity");
        if (beginSection(Str::PrefSimulator.id("sec_sim"), n3d > 0 || n2d > 0 || opacity, true)) {
            if (n3d > 0) {
                ImGui::TextDisabled("%s", Str::PrefSim3dViews.c_str());
                if (colorGrid("##sim3d", kSim3dColors))
                    applyAndSave();
            }
            if (n2d > 0) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", Str::PrefSim2dColors.c_str());
                if (colorGrid("##sim2d", kSim2dColors))
                    applyAndSave();
            }
            if (opacity) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", Str::PrefSim2dMetrics.c_str());
                // Bar/border/line widths are now content-space (OverlayAnchor::widthNorm, resized in the
                // simulator via the width grip) so they scale with zoom; only opacity stays a theme var.
                if (floatRow("##sim2d_metrics", "Opacity", "##simopacity", "###simopacityreset",
                             &t.vars[AppVar_SimGlobalOpacity].x, defaults.vars[AppVar_SimGlobalOpacity].x))
                    applyAndSave();
            }
        }
    }

    // --- Bookmarks & Misc ---
    colorSection(Str::PrefSecBookmarksMisc.id("sec_bm"), "##bm", kBookmarkColors, false);

    // --- Processing ---
    colorSection(Str::PrefSecProcessing.id("sec_proc"), "##proc", kProcessingColors, false);

    // --- Geometry (ImGui style vars that carry identity) ---
    if (beginSection(Str::PrefSecGeometry.id("sec_geom"), countMatch(std::span(kGeometryVars)) > 0, false))
        if (varGrid("##geom", kGeometryVars))
            applyAndSave();

    // --- Node Editor (imnodes structural colors + geometry) ---
    {
        const int nColors = countMatch(std::span(kNodeColors));
        const int nGeom = countMatch(std::span(kNodeGeomVars));
        if (beginSection(Str::PrefSecNodeEditor.id("sec_node"), nColors > 0 || nGeom > 0, false)) {
            if (nColors > 0 && nodeColorGrid("##nodecolors", kNodeColors))
                applyAndSave();
            if (nGeom > 0) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", Str::PrefSecGeometry.c_str());
                if (varGrid("##nodegeom", kNodeGeomVars))
                    applyAndSave();
            }
        }
    }

    // --- ImGui Colors ---
    {
        // Names are generated at runtime; build a frame-arena ColorItem span for the shared section.
        auto *items = ofs::FrameAllocator::instance().allocArray<ColorItem>(ImGuiCol_COUNT);
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
            items[i] = {i, ImGui::GetStyleColorName(i)};
        colorSection(Str::PrefSecImguiColors.id("sec_imgui"), "##imgui", {items, static_cast<size_t>(ImGuiCol_COUNT)},
                     false);
    }

    if (!themeFilter_.empty() && !sawMatch)
        ImGui::TextDisabled("%s", Str::PrefNoThemeMatch.fmt(themeFilter_));
}

} // namespace ofs
