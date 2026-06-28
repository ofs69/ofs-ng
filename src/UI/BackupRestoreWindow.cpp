#include "UI/BackupRestoreWindow.h"

#include "Core/EventQueue.h"
#include "Core/ProjectLifecycleEvents.h" // RestoreBackupRequestEvent
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/PathUtil.h"

#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <string_view>

namespace ofs {
namespace {

// The backup directory key for a project — its filename stem, or "_unnamed_" for a never-saved project.
// Must match the write side in ProjectManager so the list shows the files it actually produced.
std::string backupStem(const ScriptProject &project) {
    return project.state.filePath.empty() ? "_unnamed_"
                                          : ofs::util::toUtf8(ofs::util::fromUtf8(project.state.filePath).stem());
}

} // namespace

void BackupRestoreWindow::refresh(const ScriptProject &project) {
    entries_.clear();
    selected_ = -1;
    scannedStem_ = backupStem(project);
    loaded_ = true;

    constexpr std::string_view kPrefix = "backup-";
    auto dir = ofs::util::getPrefPath() / "backup" / ofs::util::fromUtf8(scannedStem_);
    std::error_code ec;
    for (const auto &dirEntry : std::filesystem::directory_iterator(dir, ec)) {
        if (!dirEntry.is_regular_file())
            continue;
        const auto &p = dirEntry.path();
        std::string fn = ofs::util::toUtf8(p.filename());
        if (p.extension() != ".ofp" || !fn.starts_with(kPrefix))
            continue;

        // "backup-YYYY-MM-DD_HH-MM-SS.ofp" → "YYYY-MM-DD HH:MM:SS". Fall back to the raw stem if the
        // 19-char core doesn't match (a hand-renamed file).
        std::string core = fn.substr(kPrefix.size(), fn.size() - kPrefix.size() - 4);
        std::string display = core;
        if (display.size() == 19) {
            display[10] = ' ';
            display[13] = ':';
            display[16] = ':';
        }
        entries_.push_back(
            {.path = ofs::util::toUtf8(p), .display = std::move(display), .bytes = dirEntry.file_size(ec)});
    }
    // Sortable timestamp ⇒ lexicographic path order is chronological; descending puts newest first.
    std::ranges::sort(entries_, [](const Entry &a, const Entry &b) { return a.path > b.path; });
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

    if (!loaded_ || backupStem(project) != scannedStem_)
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
            const Entry &e = entries_[i];
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
        eq.push(RestoreBackupRequestEvent{entries_[selected_].path});
        open = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Str::BackupRestoreRefresh.id("backup_refresh")))
        refresh(project);
    ImGui::SameLine();
    if (ImGui::Button(Str::PrefOpenBackupFolder.iconId(ICON_FOLDER_OPEN, "backup_open_folder")))
        ofs::util::openInFileBrowser(ofs::util::getPrefPath() / "backup" / ofs::util::fromUtf8(scannedStem_));

    ImGui::End();
}

} // namespace ofs
