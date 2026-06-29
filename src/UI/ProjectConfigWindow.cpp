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
            if (ImGui::BeginTabItem(Str::PcfTabMetadata.id("meta_tab"))) {
                ImGui::BeginChild("##meta_scroll", {0.f, 0.f});
                renderMetadataTab(project, eq);
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

void ProjectConfigWindow::renderMetadataTab(const ScriptProject &project, EventQueue &eq) {
    // Re-sync the editing mirror only on an actual document change (value compare, no heap alloc),
    // then bind the widgets to it. By render time this frame's events have already drained, so an
    // in-progress edit pushed last frame is now reflected in project.metadata and compares equal —
    // the mirror is only overwritten by genuine external changes (preset load, reset, file load).
    if (metaBuf != project.metadata)
        metaBuf = project.metadata;
    FunscriptMetadata &meta = metaBuf;

    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // --- Presets ---
    ImGui::SeparatorText(Str::PcfPresets);
    const auto &presets = appSettings.metadataPresets;

    if (selectedPresetIdx >= static_cast<int>(presets.size()))
        selectedPresetIdx = -1;

    // Row 1: preset picker | Load | Delete. The two verb buttons auto-size to their (translated)
    // labels and the combo fills the rest, so a longer translation never clips.
    const char *loadLbl = Str::PcfLoad.id("preset_load");
    const char *deleteLbl = Str::PcfDelete.id("preset_delete");
    const float loadW = buttonW(loadLbl);
    const float deleteW = buttonW(deleteLbl);
    const char *previewLabel =
        selectedPresetIdx >= 0 ? presets[selectedPresetIdx].name.c_str() : Str::PcfSelectPreset.c_str();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - loadW - deleteW - spacing * 2.f - kRightGap);
    if (ImGui::BeginCombo("###preset_combo", previewLabel)) {
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            bool sel = (i == selectedPresetIdx);
            // Stable ###id keyed on the list index so the (user-supplied) preset name doesn't decide
            // the item's widget id.
            if (ImGui::Selectable(fmtScratch("{}###preset_opt{}", presets[i].name, i), sel)) {
                selectedPresetIdx = i;
                newPresetName = presets[i].name;
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedPresetIdx < 0);
    if (ImGui::Button(loadLbl, {loadW, 0.f})) {
        eq.push(ModifyEvent<FunscriptMetadata>{
            [md = presets[selectedPresetIdx].metadata](FunscriptMetadata &m) { m = md; }});
        eq.push(NotifyEvent{.level = NotifyLevel::Success,
                            .message = Str::PcfPresetLoaded.fmt(presets[selectedPresetIdx].name)});
        meta = presets[selectedPresetIdx].metadata;
    }
    ImGui::SameLine();
    if (ImGui::Button(deleteLbl, {deleteW, 0.f})) {
        const int delIdx = selectedPresetIdx;
        confirmAsync(eq,
                     {.title = Str::PcfPresetDeleteConfirmTitle.c_str(),
                      .message = Str::PcfPresetDeleteConfirmBody.fmt(presets[delIdx].name),
                      .buttons = {Str::PcfDelete.c_str(), Str::AppCancel.c_str()},
                      .severity = ofs::ModalSeverity::Warning},
                     [this, eqp = &eq, delIdx](int idx) {
                         if (idx != 0)
                             return;
                         eqp->push(ModifyEvent<AppSettings>{[delIdx](AppSettings &s) {
                             if (delIdx >= 0 && delIdx < static_cast<int>(s.metadataPresets.size()))
                                 s.metadataPresets.erase(s.metadataPresets.begin() + delIdx);
                         }});
                         selectedPresetIdx = -1;
                         newPresetName.clear();
                     });
    }
    ImGui::EndDisabled();

    // Row 2: name input | Save (creates new or overwrites if name matches existing) | Reset (clears metadata)
    const char *saveLbl = Str::PcfSave.id("preset_save");
    const float saveW = buttonW(saveLbl);
    const char *resetLbl = Str::PcfReset.id("reset_meta");
    const float resetW = buttonW(resetLbl);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveW - resetW - 2 * spacing - kRightGap);
    ImGui::InputTextWithHint("###preset_name", Str::PcfPresetNameHint.c_str(), &newPresetName);
    ImGui::SameLine();
    ImGui::BeginDisabled(newPresetName.empty());
    if (ImGui::Button(saveLbl, {saveW, 0.f})) {
        const auto it = std::ranges::find_if(presets, [&](const auto &p) { return p.name == newPresetName; });
        selectedPresetIdx =
            it != presets.end() ? static_cast<int>(it - presets.begin()) : static_cast<int>(presets.size());
        // Overwrite a same-named preset in place, else append. The lambda re-derives the match so it
        // stays correct even if the list changed between this frame and the drain.
        eq.push(ModifyEvent<AppSettings>{[name = newPresetName, md = meta](AppSettings &s) {
            const auto e = std::ranges::find_if(s.metadataPresets, [&](const auto &p) { return p.name == name; });
            if (e != s.metadataPresets.end())
                e->metadata = md;
            else
                s.metadataPresets.push_back({.name = name, .metadata = md});
        }});
        eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PcfPresetSaved.fmt(newPresetName)});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(resetLbl, {resetW, 0.f})) {
        eq.push(ModifyEvent<FunscriptMetadata>{[](FunscriptMetadata &m) { m = FunscriptMetadata{}; }});
        meta = FunscriptMetadata{};
        jsonEdits.clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", Str::PcfClearAllTip.c_str());

    // --- Script Info ---
    ImGui::SeparatorText(Str::PcfScriptInfo);
    if (beginForm("##info_form")) {
        formRow(Str::PcfFieldTitle);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("###title", &meta.title))
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.title](FunscriptMetadata &m) { m.title = v; }});
        formRow(Str::PcfCreator);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##creator", &meta.creator))
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.creator](FunscriptMetadata &m) { m.creator = v; }});
        formRow(Str::PcfScriptUrl);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##surl", &meta.scriptUrl))
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.scriptUrl](FunscriptMetadata &m) { m.scriptUrl = v; }});
        formRow(Str::PcfVideoUrl);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##vurl", &meta.videoUrl))
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.videoUrl](FunscriptMetadata &m) { m.videoUrl = v; }});
        formRow(Str::PcfLicense);
        // Visible labels are localized but each option carries a stable ###id; the *stored* license
        // value stays the English token ("Free"/"Paid"/"") since it is persisted to the funscript.
        const char *licenseOptions[] = {"—###lic_none", Str::PcfLicenseFree.id("lic_free"),
                                        Str::PcfLicensePaid.id("lic_paid")};
        int licenseIdx = 0;
        if (meta.license == "Free")
            licenseIdx = 1;
        else if (meta.license == "Paid")
            licenseIdx = 2;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##license", &licenseIdx, licenseOptions, IM_ARRAYSIZE(licenseOptions))) {
            meta.license = licenseIdx == 1 ? "Free" : (licenseIdx == 2 ? "Paid" : "");
            eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.license](FunscriptMetadata &m) { m.license = v; }});
        }
        endForm();
    }

    // --- Description ---
    ImGui::SeparatorText(Str::PcfDescription);
    if (ImGui::InputTextMultiline("##description", &meta.description,
                                  {ImGui::GetContentRegionAvail().x - kRightGap, ImGui::GetTextLineHeight() * 4.f}))
        eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.description](FunscriptMetadata &m) { m.description = v; }});

    // --- Notes ---
    ImGui::SeparatorText(Str::PcfNotes);
    if (ImGui::InputTextMultiline("##notes", &meta.notes,
                                  {ImGui::GetContentRegionAvail().x - kRightGap, ImGui::GetTextLineHeight() * 4.f}))
        eq.push(ModifyEvent<FunscriptMetadata>{[v = meta.notes](FunscriptMetadata &m) { m.notes = v; }});

    // --- Tags ---
    ImGui::SeparatorText(Str::PcfTags);
    if (!meta.tags.empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
        for (int i = 0; i < static_cast<int>(meta.tags.size()); ++i) {
            // Stable ###tag<i> id so the visible tag text doesn't decide the widget id (test-addressable).
            if (ImGui::SmallButton(fmtScratch("{}###tag{}", meta.tags[i], i))) {
                eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                    if (i >= 0 && i < static_cast<int>(m.tags.size()))
                        m.tags.erase(m.tags.begin() + i);
                }});
                meta.tags.erase(meta.tags.begin() + i);
                --i;
            }
            if (i + 1 < static_cast<int>(meta.tags.size()))
                ImGui::SameLine();
        }
        ImGui::PopStyleVar();
    }
    {
        const float addBtnW = buttonW(ICON_PLUS);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addBtnW - spacing - kRightGap);
        bool enterTag = ImGui::InputText("###new_tag", &newTagInput, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(ICON_PLUS "###add_tag", {addBtnW, 0.f}) || enterTag) && !newTagInput.empty()) {
            eq.push(ModifyEvent<FunscriptMetadata>{[v = newTagInput](FunscriptMetadata &m) { m.tags.push_back(v); }});
            meta.tags.push_back(newTagInput);
            newTagInput.clear();
        }
    }

    // --- Performers ---
    ImGui::SeparatorText(Str::PcfPerformers);
    if (!meta.performers.empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
        for (int i = 0; i < static_cast<int>(meta.performers.size()); ++i) {
            if (ImGui::SmallButton(fmtScratch("{}###perf{}", meta.performers[i], i))) {
                eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                    if (i >= 0 && i < static_cast<int>(m.performers.size()))
                        m.performers.erase(m.performers.begin() + i);
                }});
                meta.performers.erase(meta.performers.begin() + i);
                --i;
            }
            if (i + 1 < static_cast<int>(meta.performers.size()))
                ImGui::SameLine();
        }
        ImGui::PopStyleVar();
    }
    {
        const float addBtnW = buttonW(ICON_PLUS);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addBtnW - spacing - kRightGap);
        bool enterPerformer =
            ImGui::InputText("###new_performer", &newPerformerInput, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(ICON_PLUS "###add_perf", {addBtnW, 0.f}) || enterPerformer) && !newPerformerInput.empty()) {
            eq.push(ModifyEvent<FunscriptMetadata>{
                [v = newPerformerInput](FunscriptMetadata &m) { m.performers.push_back(v); }});
            meta.performers.push_back(newPerformerInput);
            newPerformerInput.clear();
        }
    }

    renderCustomFields(meta, eq);
}

namespace {
constexpr int kCustomTypeCount = 6;

// Localized display name for a custom-field JSON type index (0..5).
const char *customTypeName(int t) {
    switch (t) {
    case 1:
        return Str::PcfTypeBool;
    case 2:
        return Str::PcfTypeNumber;
    case 3:
        return Str::PcfTypeString;
    case 4:
        return Str::PcfTypeArray;
    case 5:
        return Str::PcfTypeObject;
    default:
        return Str::PcfTypeNull;
    }
}

// Combo item label: the localized type name plus a stable ###cf_opt<t> id, so the popup items are
// addressable by id (not the translated label) and survive label/order changes — see suite_metadata.cpp.
const char *customTypeItem(int t) {
    return fmtScratch("{}###cf_opt{}", customTypeName(t), t);
}

// Type dropdown rendered via BeginCombo so each option gets an explicit id. Returns true and writes
// *type when the selection changes.
bool customTypeCombo(const char *id, int *type) {
    bool changed = false;
    if (ImGui::BeginCombo(id, customTypeName(*type))) {
        for (int t = 0; t < kCustomTypeCount; ++t) {
            const bool sel = (*type == t);
            if (ImGui::Selectable(customTypeItem(t), sel)) {
                *type = t;
                changed = true;
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Combo index for a json value's type (all number_* collapse to "Number").
int customTypeIndex(const nlohmann::json &v) {
    if (v.is_boolean())
        return 1;
    if (v.is_number())
        return 2;
    if (v.is_string())
        return 3;
    if (v.is_array())
        return 4;
    if (v.is_object())
        return 5;
    return 0; // null
}

nlohmann::json customDefaultForType(int type) {
    switch (type) {
    case 1:
        return false;
    case 2:
        return 0;
    case 3:
        return std::string{};
    case 4:
        return nlohmann::json::array();
    case 5:
        return nlohmann::json::object();
    default:
        return nlohmann::json{}; // null
    }
}
} // namespace

void ProjectConfigWindow::renderCustomFields(FunscriptMetadata &meta, EventQueue &eq) {
    ImGui::SeparatorText(Str::PcfCustomFields);

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // Width the trash button to the glyph plus symmetric padding so the icon sits centered (a bare
    // frame-height square is narrower than the glyph needs and clips it left-of-center).
    const float delBtnW = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.f;
    // Font-relative key column (scales with font/DPI); the type column is sized to the widest *localized*
    // type name so a longer translation never clips and the value column always starts past it.
    const float keyW = ImGui::GetFontSize() * 10.f;
    float typeW = 0.f;
    for (int t = 0; t < kCustomTypeCount; ++t)
        typeW = ImMax(typeW, ImGui::CalcTextSize(customTypeName(t)).x);
    typeW += ImGui::GetStyle().FramePadding.x * 2.f;

    for (int i = 0; i < static_cast<int>(meta.customFields.size()); ++i) {
        auto &cf = meta.customFields[i];
        ImGui::PushID(i);

        // Row 1: key, type, scalar value (if scalar), delete. Both the key and the type are fixed
        // once created — the key is the field's identity (it keys customJsonText and the on-disk JSON
        // object) and the type is derived from the stored value, so neither can be edited in place
        // without orphaning state or risking a value/type mismatch. To change either, delete and
        // re-add. Both are shown as plain labels, frame-aligned to sit level with the value editor.
        const float typeColX = keyW + spacing;
        const float valColX = typeColX + typeW + spacing;
        const int type = customTypeIndex(cf.value);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cf.key.c_str());
        ImGui::SameLine(typeColX);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(customTypeName(type));

        const bool scalar = type <= 3;
        if (scalar) {
            ImGui::SameLine(valColX);
            const float valW = ImGui::GetContentRegionAvail().x - delBtnW - spacing - kRightGap;
            switch (type) {
            case 1: { // bool
                bool b = cf.value.get<bool>();
                if (ImGui::Checkbox("###cf_bool", &b)) {
                    cf.value = b; // keep the mirror in sync so no full resync copy is needed next frame
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, b](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = b;
                    }});
                }
                break;
            }
            case 2: { // number
                double d = cf.value.get<double>();
                ImGui::SetNextItemWidth(valW);
                if (ImGui::InputDouble("###cf_num", &d)) {
                    cf.value = d;
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, d](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = d;
                    }});
                }
                break;
            }
            case 3: { // string
                // Bind straight to the json's internal string (no per-frame copy); the edit lands in
                // the mirror immediately and is forwarded to the document via the event.
                std::string *s = cf.value.get_ptr<std::string *>();
                ImGui::SetNextItemWidth(valW);
                if (s && ImGui::InputText("###cf_str", s))
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, v = *s](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = v;
                    }});
                break;
            }
            default: // null — no value editor
                ImGui::TextDisabled("%s", Str::PcfNullValue.c_str());
                break;
            }
        }

        ImGui::SameLine();
        bool deleted = false;
        if (ImGui::Button(ICON_TRASH "###cf_del", {delBtnW, 0.f})) {
            jsonEdits.erase(cf.key);
            eq.push(ModifyEvent<FunscriptMetadata>{[i](FunscriptMetadata &m) {
                if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                    m.customFields.erase(m.customFields.begin() + i);
            }});
            meta.customFields.erase(meta.customFields.begin() + i);
            --i;
            deleted = true;
        }

        // Row 2: array/object edited as raw JSON text, validated on commit. Parsing only happens on
        // an actual edit; the per-frame render path does no json parse or dump.
        if (!deleted && type >= 4) {
            auto it = jsonEdits.find(cf.key);
            if (it == jsonEdits.end())
                it = jsonEdits.emplace(cf.key, JsonEditState{.text = cf.value.dump(4), .lastValue = cf.value}).first;
            JsonEditState &st = it->second;

            if (!st.valid) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                ImGui::PushStyleColor(ImGuiCol_Border, ofs::theme::GetStyleColorVec4(AppCol_Error));
            }
            const float boxW = ImGui::GetContentRegionAvail().x - kRightGap;
            bool changed = ImGui::InputTextMultiline("###cf_json", &st.text, {boxW, ImGui::GetTextLineHeight() * 4.f});
            const bool editing = ImGui::IsItemActive();
            if (!st.valid) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s", Str::PcfInvalidJson.c_str());
            }

            if (changed) {
                // Re-parse only the keystroke that changed the buffer; cache validity for the next
                // frame's red border so the render path never parses.
                try {
                    nlohmann::json edited = nlohmann::json::parse(st.text);
                    st.valid = true;
                    cf.value = edited; // keep the mirror in sync so no full resync copy is needed next frame
                    eq.push(ModifyEvent<FunscriptMetadata>{[i, nv = std::move(edited)](FunscriptMetadata &m) {
                        if (i >= 0 && i < static_cast<int>(m.customFields.size()))
                            m.customFields[i].value = nv;
                    }});
                } catch (const std::exception &) {
                    st.valid = false; // mid-edit invalid JSON — keep the committed value until it parses
                }
            }
            // While the box isn't being edited, reflow it to pretty-printed (4-space) JSON whenever the
            // committed value has changed since we last synced — picks up external changes (preset load
            // / reset) and normalizes the user's own input once they finish. The value compare is
            // allocation-free; the dump runs only on a real change, not every frame.
            else if (!editing && cf.value != st.lastValue) {
                st.text = cf.value.dump(4);
                st.lastValue = cf.value;
                st.valid = true;
            }
        }

        ImGui::PopID();
    }

    // Add-new-field row.
    {
        ImGui::SetNextItemWidth(keyW);
        ImGui::InputTextWithHint("###cf_new_key", Str::PcfFieldNameHint.c_str(), &newCustomFieldKey);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(typeW);
        customTypeCombo("###cf_new_type", &newCustomFieldType);
        ImGui::SameLine();
        if (ImGui::Button(ICON_PLUS "###cf_add", {buttonW(ICON_PLUS), 0.f}) && !newCustomFieldKey.empty()) {
            eq.push(ModifyEvent<FunscriptMetadata>{
                [k = newCustomFieldKey, nv = customDefaultForType(newCustomFieldType)](FunscriptMetadata &m) {
                    m.customFields.push_back({.key = k, .value = nv});
                }});
            meta.customFields.push_back({.key = newCustomFieldKey, .value = customDefaultForType(newCustomFieldType)});
            newCustomFieldKey.clear();
        }
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
        formRow(Str::PcfType);
        const char *overlayNames[] = {Str::PcfOverlayFrame.id("overlay_frame"),
                                      Str::PcfOverlayTempo.id("overlay_tempo")};
        int currentOverlay = static_cast<int>(overlayState.overlay);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##overlay_type", &currentOverlay, overlayNames, IM_ARRAYSIZE(overlayNames))) {
            overlayState.overlay = static_cast<ScriptingOverlay>(currentOverlay);
            overlayChanged = true;
        }

        if (overlayState.overlay == ScriptingOverlay::Frame) {
            formRow(Str::PcfFps);
            ImGui::SetNextItemWidth(-FLT_MIN);
            overlayChanged |= ImGui::DragFloat("##fps", &overlayState.frameFps, 0.1f, 1.f, 240.f);
        } else {
            formRow(Str::PcfBpm);
            ImGui::SetNextItemWidth(-FLT_MIN);
            overlayChanged |= ImGui::DragFloat("##bpm", &overlayState.tempoBpm, 0.1f, 10.f, 500.f);

            formRow(Str::PcfOffset);
            ImGui::SetNextItemWidth(-FLT_MIN);
            overlayChanged |=
                ImGui::DragFloat("##tempo_offset", &overlayState.tempoOffsetSeconds, 0.001f, -10.f, 10.f, "%.3f");

            // Localize only the "measure" word in kTempoSubdivisionNames index 0 ("1/1 (measure)");
            // the rest are numeric fractions. Mirrors the identical Snap combo in ScriptTimeline.
            auto **snapNames = ofs::FrameAllocator::instance().allocArray<const char *>(kTempoSubdivisionCount);
            for (int i = 0; i < kTempoSubdivisionCount; ++i)
                snapNames[i] = kTempoSubdivisionNames[i];
            snapNames[0] = fmtScratch("1/1 ({})", Str::TlMeasure.sv());

            formRow(Str::PcfSnap);
            ImGui::SetNextItemWidth(-FLT_MIN);
            overlayChanged |=
                ImGui::Combo("##tempo_snap", &overlayState.tempoMeasureIndex, snapNames, kTempoSubdivisionCount);
        }
        endForm();
    }

    if (overlayChanged)
        eq.push(OverlaySettingsChangedEvent{overlayState});
}

} // namespace ofs
