#include "UI/BackupRestoreWindow.h"

#include "Core/EventQueue.h"
#include "Core/ProjectLifecycleEvents.h" // RestoreBackupRequestEvent
#include "Core/ScriptProject.h"
#include "Format/BackupArchive.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/PathUtil.h"

#include "imgui.h"

namespace ofs {

void BackupRestoreWindow::refresh(const ScriptProject &project) {
    scannedFor_ = project.state.filePath;
    loaded_ = true;
    entries_ = backup::list(backup::dirForProject(ofs::util::fromUtf8(project.state.filePath)));
    // Preselect the newest backup so Restore is immediately actionable.
    selected_ = entries_.empty() ? -1 : 0;
}

void BackupRestoreWindow::render(bool &open, const ScriptProject &project, EventQueue &eq) {
    if (!open) {
        loaded_ = false; // force a fresh scan next time it opens
        return;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({em * 30.0f, em * 26.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({em * 22.0f, em * 16.0f}, {FLT_MAX, FLT_MAX});

    if (!ImGui::Begin(Str::BackupRestoreTitle.id("backup_restore"), &open,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!loaded_ || project.state.filePath != scannedFor_)
        refresh(project);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(Str::BackupRestoreHint);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // Reserve the footer button row beneath the (scrolling) list.
    const float footer = ImGui::GetFrameHeightWithSpacing();
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##backup_list", 2, tableFlags, {0.0f, -footer})) {
        ImGui::TableSetupColumn(Str::BackupRestoreColDate, ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Str::BackupRestoreColSize, ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const backup::BackupFile &e = entries_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(fmtScratch("{}##backup_row{}", e.display, i), selected_ == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                selected_ = i;
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", fmtScratch("{} KB", (e.bytes + 1023) / 1024));
        }

        if (entries_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", Str::BackupRestoreEmpty.c_str());
        }
        ImGui::EndTable();
    }

    const bool hasSel = selected_ >= 0 && selected_ < static_cast<int>(entries_.size());
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button(Str::BackupRestoreRestore.id("backup_do_restore"))) {
        eq.push(RestoreBackupRequestEvent{ofs::util::toUtf8(entries_[selected_].path)});
        open = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Str::BackupRestoreRefresh.id("backup_refresh")))
        refresh(project);
    ImGui::SameLine();
    if (ImGui::Button(Str::PrefOpenBackupFolder.iconId(ICON_FOLDER_OPEN, "backup_open_folder")))
        ofs::util::openInFileBrowser(backup::dirForProject(ofs::util::fromUtf8(scannedFor_)));

    ImGui::End();
}

} // namespace ofs
