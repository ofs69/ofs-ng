#include "UI/ProjectConfigWindow.h"

#include "Core/BookmarkChapterState.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/FunscriptMetadata.h"
#include "Core/OverlaySettings.h"
#include "Core/ScriptProject.h"
#include "Core/TranscodeEvents.h"
#include "Format/AppSettings.h"
#include "Format/MediaTypes.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/OverlayControls.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include "Util/TimeUtil.h"
#include "Video/VideoPlayerSettings.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>

namespace ofs {

using ofs::ui::beginForm;
using ofs::ui::buttonW;
using ofs::ui::endForm;
using ofs::ui::formRow;
using ofs::ui::kRightGap;

ProjectConfigWindow::ProjectConfigWindow(const AppSettings &appSettings) : appSettings(appSettings) {}

namespace {

// Shared media-picker descriptor for the two "Browse..." buttons; pushes ChangeMediaPathEvent.
FileDialogSpec mediaPickSpec() {
    return {.kind = FileDialogKind::Open,
            .key = "video",
            .title = Str::PcfOpenMediaTitle.c_str(),
            .filterPatterns = mediaFilterPatterns(),
            .filterDesc = Str::PcfMediaFilesDesc.c_str()};
}
} // namespace

void ProjectConfigWindow::render(const ScriptProject &project, EventQueue &eq, bool &open, double sessionTime,
                                 double mediaDuration, bool dummyPlayerActive) {
    if (!open)
        return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({vp->Size.x * 0.45f, vp->Size.y * 0.7f}, ImGuiCond_FirstUseEver);
    // Min size from font metrics so the form survives a larger font/DPI and a longer translation.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints({em * 24.f, em * 18.f}, {em * 64.f, em * 72.f});

    if (ImGui::Begin(Str::PcfTitle.id("project_config"), &open,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##pi_tabs")) {
            if (ImGui::BeginTabItem(Str::PcfTabSettings.id("settings_tab"))) {
                ImGui::BeginChild("##settings_scroll", {0.f, 0.f});
                renderSettingsTab(project, eq, dummyPlayerActive);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Str::PcfTabChapters.id("markers_tab"))) {
                ImGui::BeginChild("##markers_scroll", {0.f, 0.f});
                renderMarkersTab(project, eq);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Str::PcfTabInfo.id("info_tab"))) {
                ImGui::BeginChild("##info_scroll", {0.f, 0.f});
                renderInfoTab(project, sessionTime, mediaDuration);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void ProjectConfigWindow::renderInfoTab(const ScriptProject &project, double sessionTime, double mediaDuration) {
    const auto &st = project.state;

    // Derive content stats once (cheap: size() reads plus one span scan). Counting resolved actions
    // where present mirrors ScriptStatisticsWindow, so a processed axis reports its output not its source.
    int totalActions = 0;
    int axesScripted = 0;
    double firstAt = std::numeric_limits<double>::max();
    double lastAt = std::numeric_limits<double>::lowest();
    for (const auto &ax : project.axes) {
        const auto &acts = ax.resolved ? ax.resolved->actions : ax.actions;
        if (acts.empty())
            continue;
        ++axesScripted;
        totalActions += static_cast<int>(acts.size());
        firstAt = std::min(firstAt, acts.front().at);
        lastAt = std::max(lastAt, acts.back().at);
    }
    const int sessions = st.editSessionCount > 0 ? st.editSessionCount : 1;
    const double hours = st.totalEditingSeconds / 3600.0;

    // Each section is an [icon | label | value] table; RowBg gives the alternating-row "data table"
    // look, and the auto-fitting label column means a longer translation never clips the value.
    auto beginSection = [](const char *id) {
        if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg))
            return false;
        ImGui::TableSetupColumn("##i", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed); // auto-fits to widest label
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        return true;
    };
    auto row = [](const char *icon, const char *label, const char *value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(icon);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value);
    };
    auto header = [](const char *icon, const char *label) { ImGui::SeparatorText(fmtScratch("{}  {}", icon, label)); };

    const char *coverage = "—";
    if (mediaDuration > 0.0 && totalActions > 0) {
        // Scripted extent (first → last action) over the media length — the span, not a gap-free
        // measure: it overstates when the script has interior holes.
        const double frac = std::clamp((lastAt - firstAt) / mediaDuration, 0.0, 1.0);
        coverage = fmtScratch("{}%", std::lround(frac * 100.0));
    }

    // Two side-by-side panels balance the height (~7 rows each) and use the horizontal space the old
    // single column wasted; the inner vertical border separates them.
    if (ImGui::BeginTable("##info_grid", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();

        // -- Left: Work Time + Structure --
        ImGui::TableNextColumn();
        header(ICON_TIMER, Str::PcfWorkTime);
        if (beginSection("##worktime")) {
            row(ICON_CLOCK_3, Str::PcfSession, ofs::TimeUtil::formatTime(sessionTime, true));
            row(ICON_HOURGLASS, Str::PcfTotal, ofs::TimeUtil::formatTime(st.totalEditingSeconds, true));
            row(ICON_REPEAT, Str::PcfSessions, fmtScratch("{}", st.editSessionCount));
            row(ICON_GAUGE, Str::PcfAvgSession, ofs::TimeUtil::formatTime(st.totalEditingSeconds / sessions, false));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        header(ICON_LAYOUT_LIST, Str::PcfStructure);
        if (beginSection("##structure")) {
            row(ICON_CHAPTER, Str::PcfChapters, fmtScratch("{}", project.bookmarks.chapters.size()));
            row(ICON_BOX_SELECT, Str::PcfRegions, fmtScratch("{}", project.regions.size()));
            row(ICON_BOOKMARK, Str::PcfBookmarks, fmtScratch("{}", project.bookmarks.bookmarks.size()));
            ImGui::EndTable();
        }

        // -- Right: Content + History --
        ImGui::TableNextColumn();
        header(ICON_ACTIVITY, Str::PcfContent);
        if (beginSection("##content")) {
            row(ICON_LIST, Str::PcfActions, fmtScratch("{}", totalActions));
            // Net change since the session baseline — signed, since undo/deletes can take it negative.
            row(ICON_TRENDING_UP, Str::PcfThisSession, fmtScratch("{:+}", totalActions - st.sessionBaselineActions));
            row(ICON_GAUGE, Str::PcfActionsPerHour,
                hours > 0.0 ? fmtScratch("{}", std::lround(totalActions / hours)) : "—");
            row(ICON_SLIDERS_HORIZONTAL, Str::PcfAxesScripted, fmtScratch("{}", axesScripted));
            row(ICON_PERCENT, Str::PcfCoverage, coverage);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        header(ICON_HISTORY, Str::PcfHistory);
        if (beginSection("##history")) {
            row(ICON_CALENDAR_PLUS, Str::PcfCreated,
                st.createdAtUnix > 0 ? ofs::TimeUtil::formatDate(st.createdAtUnix) : Str::PcfNotSaved.c_str());
            row(ICON_CALENDAR_CHECK, Str::PcfModified,
                st.modifiedAtUnix > 0 ? ofs::TimeUtil::formatDate(st.modifiedAtUnix) : Str::PcfNotSaved.c_str());
            ImGui::EndTable();
        }
        ImGui::EndTable();
    }
}

void ProjectConfigWindow::renderMarkersTab(const ScriptProject &project, EventQueue &eq) {
    const auto &bc = project.bookmarks;
    const float seekW = buttonW(ICON_SEEK);
    constexpr auto kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                 ImGuiTableFlags_SizingFixedFit;

    // Re-sync the name edit mirror to the document, allocating only when an entry's text actually changed
    // (size change or external edit/undo) — never every frame. See chNameBuf_/bmNameBuf_ in the header.
    auto syncNames = [](std::vector<std::string> &buf, const auto &items, auto getName) {
        if (buf.size() != items.size())
            buf.resize(items.size());
        for (size_t i = 0; i < items.size(); ++i)
            if (buf[i] != getName(items[i]))
                buf[i] = getName(items[i]);
    };
    syncNames(chNameBuf_, bc.chapters, [](const Chapter &c) -> const std::string & { return c.name; });
    syncNames(bmNameBuf_, bc.bookmarks, [](const Bookmark &b) -> const std::string & { return b.name; });

    // snapshot=true only on the first change of a (kind,row) gesture; subsequent frames coalesce.
    auto firstChange = [this](MarkerEdit kind, int idx) {
        const bool first = !(markerEdit_ == kind && markerEditIdx_ == idx);
        markerEdit_ = kind;
        markerEditIdx_ = idx;
        return first;
    };

    // --- Chapters ---
    ImGui::SeparatorText(fmtScratch("{}  {}", ICON_CHAPTER, Str::PcfChapters.sv()));
    if (bc.chapters.empty()) {
        ImGui::TextDisabled("%s", Str::PcfNoChapters.c_str());
    } else if (ImGui::BeginTable("##chapters_tbl", 6, kTableFlags)) {
        ImGui::TableSetupColumn("##ch_color", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn(Str::PcfMarkerName.id("ch_col_name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Str::PcfMarkerStart.id("ch_col_start"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(Str::PcfMarkerEnd.id("ch_col_end"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(Str::PcfMarkerDuration.id("ch_col_dur"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##ch_seek", ImGuiTableColumnFlags_WidthFixed, seekW);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(bc.chapters.size()); ++i) {
            const auto &ch = bc.chapters[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            // Color: ColorEdit3 swatch button opens a picker; alpha is preserved from the stored color.
            ImGui::TableNextColumn();
            ImColor colVec(ch.color);
            float col[3] = {colVec.Value.x, colVec.Value.y, colVec.Value.z};
            if (ImGui::ColorEdit3("##chcolor", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                eq.push(ModifyBookmarkChapterEvent{
                    .apply =
                        [i, c = static_cast<ImU32>(ImColor(col[0], col[1], col[2], colVec.Value.w))](
                            BookmarkChapterState &s) {
                            if (i >= 0 && i < static_cast<int>(s.chapters.size()))
                                s.chapters[i].color = c;
                        },
                    .snapshot = firstChange(MarkerEdit::ChapterColor, i)});
            }
            if (ImGui::IsItemDeactivated())
                markerEdit_ = MarkerEdit::None;

            // Name: edits push live each frame (one undo step per typing gesture).
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##chname", &chNameBuf_[i])) {
                eq.push(ModifyBookmarkChapterEvent{.apply =
                                                       [i, name = chNameBuf_[i]](BookmarkChapterState &s) {
                                                           if (i >= 0 && i < static_cast<int>(s.chapters.size()))
                                                               s.chapters[i].name = name;
                                                       },
                                                   .snapshot = firstChange(MarkerEdit::ChapterName, i)});
            }
            if (ImGui::IsItemDeactivated())
                markerEdit_ = MarkerEdit::None;

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TimeUtil::formatTime(ch.startTime, true));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TimeUtil::formatTime(ch.endTime, true));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TimeUtil::formatTime(std::max(0.0, ch.endTime - ch.startTime), true));

            ImGui::TableNextColumn();
            if (ImGui::Button(ICON_SEEK "###chseek", {seekW, 0.f}))
                eq.push(SeekEvent{ch.startTime});

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // --- Bookmarks ---
    ImGui::SeparatorText(fmtScratch("{}  {}", ICON_BOOKMARK, Str::PcfBookmarks.sv()));
    if (bc.bookmarks.empty()) {
        ImGui::TextDisabled("%s", Str::PcfNoBookmarks.c_str());
    } else if (ImGui::BeginTable("##bookmarks_tbl", 3, kTableFlags)) {
        ImGui::TableSetupColumn(Str::PcfMarkerName.id("bm_col_name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Str::PcfMarkerTime.id("bm_col_time"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##bm_seek", ImGuiTableColumnFlags_WidthFixed, seekW);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(bc.bookmarks.size()); ++i) {
            const auto &bm = bc.bookmarks[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##bmname", &bmNameBuf_[i])) {
                eq.push(ModifyBookmarkChapterEvent{.apply =
                                                       [i, name = bmNameBuf_[i]](BookmarkChapterState &s) {
                                                           if (i >= 0 && i < static_cast<int>(s.bookmarks.size()))
                                                               s.bookmarks[i].name = name;
                                                       },
                                                   .snapshot = firstChange(MarkerEdit::BookmarkName, i)});
            }
            if (ImGui::IsItemDeactivated())
                markerEdit_ = MarkerEdit::None;

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TimeUtil::formatTime(bm.time, true));

            ImGui::TableNextColumn();
            if (ImGui::Button(ICON_SEEK "###bmseek", {seekW, 0.f}))
                eq.push(SeekEvent{bm.time});

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void ProjectConfigWindow::renderSettingsTab(const ScriptProject &project, EventQueue &eq, bool dummyPlayerActive) {
    const auto &state = project.state;

    // --- Media ---
    ImGui::SeparatorText(Str::PcfMedia);
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const bool hasVideo = !state.mediaPath.empty();
        const char *browseLbl = Str::PcfBrowse;
        const float browseW = buttonW(browseLbl);

        if (hasVideo) {
            const char *unloadLbl = Str::PcfUnload;
            const float unloadW = buttonW(unloadLbl);
            // Optimize shares the media row (icon + short label); measure it so the path input reserves
            // its width too. The gate mirrors the command in OfsApp; a visible-but-disabled button keeps
            // the prerequisite discoverable, with the reason spelled out below the row.
            const char *optLbl = Str::PcfOptimize.iconId(ICON_GAUGE, "optimize_intra");
            const float optW = buttonW(optLbl);
            const bool haveTools = ofs::util::toolAvailable("ffmpeg") && ofs::util::toolAvailable("ffprobe");
            const bool canOptimize =
                haveTools && project.activeSource == MediaSource::Original && !project.transcode.active;

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - unloadW - optW - spacing * 3.f -
                                    kRightGap);
            ImGui::InputText("##media_path", const_cast<std::string *>(&state.mediaPath), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::BeginPopupContextItem("##media_path_ctx")) {
                ofs::ui::showInFileBrowserItem(state.mediaPath);
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(browseLbl, {browseW, 0.f})) {
                pickFile(eq, mediaPickSpec(), [&eq](std::string file) {
                    if (!file.empty())
                        eq.push(ChangeMediaPathEvent{std::move(file)});
                });
            }
            ImGui::SameLine();
            if (ImGui::Button(unloadLbl, {unloadW, 0.f}))
                eq.push(ChangeMediaPathEvent{""});
            ImGui::SameLine();
            ImGui::BeginDisabled(!canOptimize);
            if (ImGui::Button(optLbl, {optW, 0.f}))
                eq.push(OpenTranscodeDialogEvent{});
            ImGui::EndDisabled();
            if (!haveTools)
                ImGui::TextDisabled("%s", Str::CmdOptimizeIntraNoFfmpeg.c_str());

            // --- Active source toggle (Original ↔ optimized intra copy) ---
            // Only meaningful once an intra copy has been generated for this project. Switching is a
            // plain ChangeMediaPathEvent to the resolved path; ProjectManager keeps the two-path model
            // and the playback position in sync.
            if (!state.intraMediaPath.empty()) {
                const bool intraExists = std::filesystem::exists(ofs::util::fromUtf8(state.intraMediaPath));
                int src = project.activeSource == MediaSource::Intra ? 1 : 0;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Str::PcfSource);
                ImGui::SameLine();
                if (ImGui::RadioButton(Str::PcfSourceOriginal.id("src_original"), &src, 0) &&
                    project.activeSource != MediaSource::Original)
                    eq.push(ChangeMediaPathEvent{state.originalMediaPath});
                ImGui::SameLine();
                // Intra is only selectable while the file is present; a missing copy falls back to
                // Original (resolveActiveMedia), so the radio would otherwise lie.
                ImGui::BeginDisabled(!intraExists);
                if (ImGui::RadioButton(Str::PcfSourceOptimized.id("src_optimized"), &src, 1) &&
                    project.activeSource != MediaSource::Intra)
                    eq.push(ChangeMediaPathEvent{state.intraMediaPath});
                ImGui::EndDisabled();
            }
        } else {
            ImGui::TextDisabled("%s", Str::PcfNoVideo.c_str());
            ImGui::SameLine();
            if (ImGui::Button(browseLbl, {browseW, 0.f})) {
                pickFile(eq, mediaPickSpec(), [&eq](std::string file) {
                    if (!file.empty())
                        eq.push(ChangeMediaPathEvent{std::move(file)});
                });
            }

            ImGui::Spacing();
            if (!dummyPlayerActive) {
                if (ImGui::Button(Str::PcfStartWithoutVideo.id("start_no_video")))
                    eq.push(ChangeDummyDurationEvent{300.0});
            } else {
                ImGui::TextUnformatted(Str::PcfDurationLabel);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
                ImGui::InputText("##dummy_dur", &dummyDuration_);
                ImGui::SameLine();
                if (ImGui::Button(Str::PcfApply.id("dur_apply"))) {
                    // Accept "H:M:S", "M:S", or a plain seconds value. Each colon shifts the
                    // running total up by 60, so one accumulator covers all three forms and the
                    // final token may be fractional. Stream-based rather than sscanf_s, which is
                    // MSVC-only (bare sscanf trips C4996 under /WX).
                    std::istringstream ss(dummyDuration_);
                    double parsed = 0.0;
                    int count = 0;
                    for (std::string tok; std::getline(ss, tok, ':'); ++count) {
                        char *end = nullptr;
                        double v = std::strtod(tok.c_str(), &end);
                        if (end == tok.c_str() || *end != '\0') {
                            count = -1;
                            break;
                        }
                        parsed = parsed * 60.0 + v;
                    }
                    if (count < 1 || parsed <= 0.0)
                        parsed = -1.0;
                    if (parsed > 0.0) {
                        dummyDurationError = false;
                        eq.push(ChangeDummyDurationEvent{parsed});
                    } else {
                        dummyDurationError = true;
                    }
                }
                if (dummyDurationError) {
                    ImGui::SameLine();
                    ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s", Str::PcfInvalid.c_str());
                }
            }
        }
    }

    // --- Video Player ---
    ImGui::SeparatorText(Str::PcfVideoPlayer);
    auto vpState = project.videoPlayer;
    bool vpChanged = false;

    if (beginForm("##vp_form")) {
        formRow(Str::PcfMode);
        // Each option's visible label is localized but carries a stable ###id; the stored value is the
        // enum index, so translating the labels never changes what's persisted.
        const char *modes[] = {Str::PcfModeFull.id("mode_full"), Str::PcfModeVr.id("mode_vr")};
        int currentMode = static_cast<int>(vpState.activeMode);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
            vpState.activeMode = static_cast<VideoMode>(currentMode);
            vpChanged = true;
        }

        // Resolution percentages are numeric/symbolic — not translatable, so they stay literal.
        static constexpr float kScales[] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 0.125f};
        static const char *kScaleNames[] = {"100%", "50%", "33%", "25%", "12.5%"};
        int currentScale = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kScales); ++i) {
            if (std::abs(vpState.resolutionScale - kScales[i]) < 0.001f) {
                currentScale = i;
                break;
            }
        }
        formRow(Str::PcfResolution);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##res", &currentScale, kScaleNames, IM_ARRAYSIZE(kScaleNames))) {
            vpState.resolutionScale = kScales[currentScale];
            vpChanged = true;
        }
        endForm();
    }

    if (vpChanged)
        eq.push(ModifyEvent<VideoPlayerState>{
            [mode = vpState.activeMode, scale = vpState.resolutionScale](VideoPlayerState &v) {
                v.activeMode = mode;
                v.resolutionScale = scale;
            }});

    // --- Overlay ---
    ImGui::SeparatorText(Str::PcfOverlay);
    auto overlayState = project.overlay;
    bool overlayChanged = false;

    if (beginForm("##overlay_form")) {
        overlayChanged = ofs::ui::renderOverlayControls(overlayState, Str::PcfType, [](TrKey label) {
            formRow(label);
            ImGui::SetNextItemWidth(-FLT_MIN);
        });
        endForm();
    }

    if (overlayChanged)
        eq.push(OverlaySettingsChangedEvent{overlayState});
}

} // namespace ofs
