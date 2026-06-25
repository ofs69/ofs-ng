#include "UI/ScriptStatistics.h"

#include "Core/ScriptAxisAction.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include "imgui.h"
#include <cmath>

namespace ofs {

void ScriptStatisticsWindow::render(const ScriptProject &project, bool &open) const {
    if (!open)
        return;
    if (!ImGui::Begin(Str::StatTitle.id("statistics"), &open, ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        return;
    }

    const auto activeIdx = static_cast<size_t>(project.state.activeAxis);
    if (project.state.activeAxis >= StandardAxis::Count) {
        ImGui::TextDisabled("%s", Str::StatNoAxis.c_str());
        ImGui::End();
        return;
    }

    const auto &axis = project.axes[activeIdx];
    const auto &actions = axis.resolved ? axis.resolved->actions : axis.actions;

    if (actions.empty()) {
        ImGui::TextDisabled("%s", Str::StatNoActions.c_str());
        ImGui::End();
        return;
    }

    const double currentTime = project.playback.cursorPos;

    // Find the actions immediately before and after the playhead.
    // upperBound gives the first action strictly after currentTime.
    auto it = actions.upperBound(ScriptAxisAction{currentTime, 0});
    const ScriptAxisAction *front = (it != actions.end()) ? &*it : nullptr;
    const ScriptAxisAction *behind = nullptr;
    if (it != actions.begin()) {
        --it;
        behind = &*it;
    }

    // A 2-column table (icon+label | value) auto-fits the label column to the widest *translated*
    // label, replacing the old hand-padded spaces. The values are pre-formatted fixed-width (%6.2f →
    // "{:6.2f}") so the value column never reflows as the playhead moves between actions — the catalog
    // can't carry an fmt width spec, so the width is applied here and the unit comes from the key.
    auto valRow = [](const char *icon, const char *label, const char *value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(fmtScratch("{} {}", icon, label));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value);
    };

    // All playhead rows render every frame regardless of where the cursor sits — a row whose value
    // needs a neighbouring action that isn't there shows a dash, so the table keeps a constant height
    // and the window doesn't jump as the playhead crosses actions. Interval needs only the previous
    // action; Speed/Duration/Delta need both surrounding actions.
    const char *kNoValue = "—";
    const bool haveSpan = behind != nullptr && front != nullptr;
    const double duration = haveSpan ? front->at - behind->at : 0.0;
    const int delta = haveSpan ? front->pos - behind->pos : 0;

    if (ImGui::BeginTable("##statplayhead", 2, ImGuiTableFlags_SizingFixedFit)) {
        const char *intervalVal = (behind != nullptr)
                                      ? Str::StatMs.fmt(fmtScratch("{:6.2f}", (currentTime - behind->at) * 1000.0))
                                      : kNoValue;
        valRow(ICON_CLOCK_3, Str::StatInterval, intervalVal);

        const char *speedVal =
            haveSpan ? Str::StatUnitsPerSec.fmt(fmtScratch("{:6.2f}", std::abs(delta) / duration)) : kNoValue;
        const char *durVal = haveSpan ? Str::StatMs.fmt(fmtScratch("{:6.2f}", duration * 1000.0)) : kNoValue;
        valRow(ICON_GAUGE, Str::StatSpeed, speedVal);
        valRow(ICON_TIMER, Str::StatDuration, durVal);

        // The row icon carries the direction (neutral up-down when there's no span); the value is the
        // magnitude and from→to diagram. {:3d} keeps each field fixed-width (pos 0-100, |delta| <= 100).
        const char *deltaIcon = !haveSpan ? ICON_ARROW_UP_DOWN : (delta >= 0 ? ICON_ARROW_UP : ICON_ARROW_DOWN);
        const char *deltaVal =
            haveSpan ? fmtScratch("{:3d}  ({:3d} " ICON_ARROW_RIGHT " {:3d})", std::abs(delta), behind->pos, front->pos)
                     : kNoValue;
        valRow(deltaIcon, Str::StatDelta, deltaVal);
        ImGui::EndTable();
    }

    size_t totalAllAxes = 0;
    for (const auto &ax : project.axes) {
        totalAllAxes += ax.resolved ? ax.resolved->actions.size() : ax.actions.size();
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##stataction", 2, ImGuiTableFlags_SizingFixedFit)) {
        valRow(ICON_LIST, Str::StatActionsAxis, fmtScratch("{}", actions.size()));
        valRow(ICON_LAYERS_2, Str::StatActionsAll, fmtScratch("{}", totalAllAxes));
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace ofs
