#include "UI/LogWindow.h"

#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include <array>

namespace ofs {
namespace {

// Combo index -> minimum level shown. Order mirrors the LogLevel* string list below.
constexpr std::array<spdlog::level::level_enum, 4> kLevelThresholds = {spdlog::level::trace, spdlog::level::info,
                                                                       spdlog::level::warn, spdlog::level::err};

// Severity tint for a log line. trace/debug are dimmed, warn/err/critical stand out; info renders in
// the default text colour. Colours are not user-visible strings, so the literals stay inline.
bool levelColor(spdlog::level::level_enum level, ImVec4 &out) {
    switch (level) {
    case spdlog::level::critical:
    case spdlog::level::err:
        out = ImVec4(1.0f, 0.40f, 0.40f, 1.0f);
        return true;
    case spdlog::level::warn:
        out = ImVec4(1.0f, 0.80f, 0.30f, 1.0f);
        return true;
    case spdlog::level::trace:
    case spdlog::level::debug:
        out = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        return true;
    default:
        return false;
    }
}

const char *levelLabel(int idx) {
    switch (idx) {
    case 1:
        return Str::LogLevelInfo;
    case 2:
        return Str::LogLevelWarn;
    case 3:
        return Str::LogLevelError;
    default:
        return Str::LogLevelAll;
    }
}

} // namespace

void LogWindow::rebuildFiltered() {
    filtered_.clear();
    filtered_.reserve(entries_.size());
    const spdlog::level::level_enum threshold = kLevelThresholds[static_cast<size_t>(minLevel_)];
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const LogEntry &e = entries_[static_cast<size_t>(i)];
        if (e.level < threshold)
            continue;
        if (!textFilter_.PassFilter(e.text.c_str()))
            continue;
        filtered_.push_back(i);
    }
}

void LogWindow::render(bool &open) {
    if (!open)
        return;

    // Font-relative so the window grows with the UI font / DPI. The toolbar (clear/copy/level/filter)
    // sets the minimum width; the starting size gives a usable multi-line view on first open.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize({em * 42.0f, em * 22.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({em * 24.0f, em * 8.0f}, {FLT_MAX, FLT_MAX});

    if (!ImGui::Begin(Str::LogTitle.id("log"), &open, ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        return;
    }

    // Refresh the cached snapshot only when the ring actually changed; the filter is then rebuilt
    // when its inputs change or new lines arrived.
    const uint64_t gen = Log::logGeneration();
    if (gen != cachedGeneration_) {
        Log::snapshotLog(entries_);
        cachedGeneration_ = gen;
        filterDirty_ = true;
    }

    if (ImGui::Button(Str::LogClear.iconId(ICON_TRASH, "log_clear"))) {
        Log::clearLog();
        entries_.clear();
        filterDirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Str::LogCopy.iconId(ICON_COPY, "log_copy"))) {
        std::string all;
        for (int idx : filtered_) {
            all += entries_[static_cast<size_t>(idx)].text;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox(Str::LogAutoScroll.id("log_autoscroll"), &autoScroll_);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::BeginCombo("##loglevel", levelLabel(minLevel_))) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(levelLabel(i), minLevel_ == i)) {
                minLevel_ = i;
                filterDirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (textFilter_.Draw(Str::LogFilter.id("logfilter"), ImGui::GetFontSize() * 12.0f))
        filterDirty_ = true;

    if (filterDirty_) {
        rebuildFiltered();
        filterDirty_ = false;
    }

    ImGui::Separator();

    if (ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (filtered_.empty()) {
            ImGui::TextDisabled("%s", Str::LogEmpty.c_str());
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const LogEntry &e = entries_[static_cast<size_t>(filtered_[static_cast<size_t>(row)])];
                    ImVec4 col;
                    const bool colored = levelColor(e.level, col);
                    if (colored)
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(e.text.c_str());
                    if (colored)
                        ImGui::PopStyleColor();
                }
            }
            clipper.End();
        }

        // Stick to the bottom only while the user is already there, so manual scroll-up isn't yanked
        // back down by incoming lines.
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace ofs
