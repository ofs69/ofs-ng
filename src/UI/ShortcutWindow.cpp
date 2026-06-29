#include "ShortcutWindow.h"
#include "Core/CommandEvents.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "Services/BindingEvents.h"
#include "Services/CustomCommandStore.h"
#include "Services/CustomCommandTemplate.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "Util/FrameAllocator.h"
#include "Util/FuzzyMatch.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace ofs {

using ofs::ui::buttonW;

namespace {

// Raise a standard confirm modal: a wrapped message, a confirm button (its own label + stable id), and a
// Cancel button (its own stable id — kept distinct per modal so UI tests can target each). `onConfirm`
// runs when the user confirms; the modal closes on either button. The message is formatted once here into
// an owned string, since the body renders deferred — after the frame arena that .fmt() wrote to is reset.
void confirmModal(EventQueue &eq, const char *title, std::string message, TrKey confirmLabel, const char *confirmId,
                  const char *cancelId, std::function<void()> onConfirm) {
    showCustomModal(eq, {.title = title,
                         .width = 420.0f,
                         .body = [message = std::move(message), confirmLabel, confirmId, cancelId,
                                  onConfirm = std::move(onConfirm)]() -> bool {
                             ImGui::TextWrapped("%s", message.c_str());
                             ImGui::Spacing();
                             if (ImGui::Button(confirmLabel.id(confirmId), {buttonW(confirmLabel), 0.f})) {
                                 onConfirm();
                                 return true;
                             }
                             ImGui::SameLine();
                             if (ImGui::Button(Str::ScCancel.id(cancelId), {buttonW(Str::ScCancel), 0.f}))
                                 return true;
                             return false;
                         }});
}

} // namespace

ShortcutWindow::ShortcutWindow(CommandRegistry &commandRegistry, BindingSystem &bindingSystem,
                               const CustomCommandStore &customStore, const CustomCommandTemplateRegistry &templates)
    : commandRegistry_(commandRegistry), bindingSystem_(bindingSystem), customStore_(customStore),
      templates_(templates) {}

const char *ShortcutWindow::formatKeyChord(const KeyChord &kc) {
    // The trigger-text family below (SDL key/button names + the Ctrl+/Shift+/Alt+/GUI+/[Pad] prefixes)
    // is technical identifier text, shared with the binding filter, and stays in English on purpose —
    // only the "no binding" sentinel is localized (it also surfaces as a standalone label in the table).
    if (kc.key == SDLK_UNKNOWN)
        return Str::ScNone;
    const char *ctrl = (kc.modifiers & SDL_KMOD_CTRL) != 0 ? "Ctrl+" : "";
    const char *shift = (kc.modifiers & SDL_KMOD_SHIFT) != 0 ? "Shift+" : "";
    const char *alt = (kc.modifiers & SDL_KMOD_ALT) != 0 ? "Alt+" : "";
    const char *gui = (kc.modifiers & SDL_KMOD_GUI) != 0 ? "GUI+" : "";
    return fmtScratch("{}{}{}{}{}", ctrl, shift, alt, gui, SDL_GetKeyName(kc.key));
}

static const char *formatTrigger(const Trigger &t) {
    if (const auto *kc = std::get_if<KeyChord>(&t))
        return ShortcutWindow::formatKeyChord(*kc);
    if (const auto *pb = std::get_if<PadButton>(&t)) {
        const char *name = SDL_GetGamepadStringForButton(pb->button);
        return name ? fmtScratch("[Pad] {}", name) : "[Pad] ?";
    }
    if (const auto *pa = std::get_if<PadAxis>(&t)) {
        const char *name = SDL_GetGamepadStringForAxis(pa->axis);
        return name ? fmtScratch("[Pad] {}{}", name, pa->positive ? "+" : "-") : "[Pad] ?";
    }
    return "?";
}

// Short label for a held modifier (no "[Pad]" prefix — it rides next to the trigger as a chip).
static const char *formatModifier(const Modifier &m) {
    if (const auto *pb = std::get_if<PadButton>(&m)) {
        const char *name = SDL_GetGamepadStringForButton(pb->button);
        return name ? name : "?";
    }
    if (const auto *pa = std::get_if<PadAxis>(&m)) {
        const char *name = SDL_GetGamepadStringForAxis(pa->axis);
        return name ? fmtScratch("{}{}", name, pa->positive ? "+" : "-") : "?";
    }
    return "";
}

const Command *ShortcutWindow::conflictingCommand(const Trigger &trigger, const std::string &targetCommandId) const {
    for (const auto &b : bindingSystem_.bindings()) {
        if (b.trigger == trigger && b.commandId != targetCommandId)
            return commandRegistry_.find(b.commandId);
    }
    return nullptr;
}

bool ShortcutWindow::renderCaptureModal(EventQueue &eq) {
    RebindState &rs = bindingSystem_.rebindState();

    // A non-empty target keeps the modal alive: while capturing, while a captured result awaits
    // confirmation, and across a Re-capture. Escape during capture clears the target (in
    // BindingSystem::onKeyDown), which drops out here and closes the popup.
    if (rs.targetCommandId.empty())
        return true;

    auto closeCapture = [&rs]() {
        rs.capturing = false;
        rs.hasResult = false;
        rs.targetCommandId.clear();
        rs.captureModifier = false;
        rs.replaceTrigger = false;
    };

    const bool modifierMode = rs.captureModifier;

    if (const Command *cmd = commandRegistry_.find(rs.targetCommandId)) {
        const char *name = cmd->title.c_str();
        if (modifierMode)
            ImGui::TextUnformatted(Str::ScModifierFor.fmt(name, formatTrigger(rs.modifierTarget)));
        else
            ImGui::TextUnformatted(Str::ScCaptureBinding.fmt(name));
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (rs.hasResult) {
        ImGui::TextWrapped("%s", Str::ScCaptured.fmt(formatTrigger(rs.captured)));
        if (!modifierMode) {
            // Trigger capture: warn about a conflict before reassigning it.
            if (const Command *conflict = conflictingCommand(rs.captured, rs.targetCommandId)) {
                const char *conflictName = conflict->title.c_str();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram));
                ImGui::TextWrapped("%s", Str::ScCapturedConflict.fmt(conflictName));
                ImGui::PopStyleColor();
            }
        }
        ImGui::Spacing();
        const bool apply = ImGui::Button(Str::ScApply.id("captureapply"), {buttonW(Str::ScApply), 0.f});
        if (apply) {
            eq.push(ApplyBindingCaptureEvent{.commandId = rs.targetCommandId,
                                             .captured = rs.captured,
                                             .captureModifier = modifierMode,
                                             .modifierTarget = rs.modifierTarget,
                                             .replaceTrigger = rs.replaceTrigger,
                                             .replaceTarget = rs.replaceTarget});
            closeCapture();
            return true;
        }
        ImGui::SameLine();
        if (ImGui::Button(Str::ScRecapture.id("capturerecap"), {buttonW(Str::ScRecapture), 0.f})) {
            rs.hasResult = false; // intentionally does not close
            rs.capturing = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(Str::ScCancel.id("capturecancel"), {buttonW(Str::ScCancel), 0.f})) {
            closeCapture();
            return true;
        }
    } else {
        if (modifierMode)
            ImGui::TextWrapped("%s", Str::ScModifierPrompt.c_str());
        else
            ImGui::TextWrapped("%s", Str::ScCapturePrompt.c_str());
        ImGui::Spacing();
        if (ImGui::Button(Str::ScCancel.id("capturecancelwait"), {buttonW(Str::ScCancel), 0.f})) {
            closeCapture();
            return true;
        }
    }
    return false;
}

void ShortcutWindow::beginCapture(EventQueue &eq, BeginBindingCaptureEvent req) {
    // The kind picks the modal title up front: BindingSystem won't populate rebindState until this event
    // drains next frame, so the title can't come from there.
    const char *title = req.captureModifier  ? Str::ScSetModifierTitle.c_str()
                        : req.replaceTrigger ? Str::ScRebindTitle.c_str()
                                             : Str::ScAddBindingTitle.c_str();
    eq.push(std::move(req));
    showCustomModal(eq, {.title = title, .width = 380.0f, .body = [this, eqp = &eq]() -> bool {
                             return renderCaptureModal(*eqp);
                         }});
}

void ShortcutWindow::render(bool &open, EventQueue &eq, const AppSettings &appSettings) {
    if (!open) {
        m_presetsLoaded = false; // refresh the cached list the next time the window opens
        m_onlyBoundPrev = false; // so reopening with "Only bound" still checked re-expands the groups
        return;
    }
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({vp->Size.x * 0.45f, vp->Size.y * 0.7f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Str::ScTitle.id("shortcut_bindings"), &open,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        return;
    }

    if (!m_presetsLoaded) {
        m_presets = bindingSystem_.listPresets();
        m_presetsLoaded = true;
    }

    // Top row: the "Add command…" action then the filter on the left, with the "Only bound" toggle and
    // "Reset to Defaults" pinned right.
    const ImGuiStyle &style = ImGui::GetStyle();
    const char *resetLabel = Str::ScResetToDefaults.icon(ICON_RESET);
    const float resetW = ImGui::CalcTextSize(resetLabel).x + style.FramePadding.x * 2.f + style.ItemSpacing.x;
    auto checkboxW = [&style](const char *label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + style.ItemSpacing.x;
    };
    const float onlyW = checkboxW(Str::ScOnlyBound);

    // One entry point for everything beyond the default rebind list: create a user-defined custom command
    // (Step / Move position / Move time), or assign a key to a command not listed by default (provider/opener
    // commands). Both used to be separate top-row affordances — a button and a checkbox; the picker unifies
    // them. The popup anchors to this button, so BeginPopup stays adjacent. Stable "###addcommand" id
    // (display stays the icon+label) so tests target it by id, not the glyph.
    if (ImGui::Button(fmtScratch("{}###addcommand", Str::ScAddCommand.icon(ICON_PLUS))))
        ImGui::OpenPopup("##addpopup");
    ImGui::SetItemTooltip("%s", Str::ScAddCommandTip.c_str());
    if (ImGui::BeginPopup("##addpopup")) {
        renderAddCommandPicker();
        ImGui::EndPopup();
    }
    ImGui::SameLine();

    // InputTextWithHint (not a plain box) so the hint can advertise the Ctrl+F shortcut — without it the
    // focus-search feature is invisible. The "###scfilter" id stays stable (UI tests target it). The filter
    // fills the gap between the Add button and the right-pinned controls (avail already excludes the button).
    if (ofs::ui::shortcutFocusSearch())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ofs::ui::kRightGap - onlyW - resetW);
    ImGui::InputTextWithHint("###scfilter", fmtScratch("{}  {}", ICON_SEARCH, Str::ScFilterHint.c_str()), &filter_);
    ImGui::SameLine();
    ImGui::Checkbox(Str::ScOnlyBound.id("onlybound"), &m_onlyBound);
    ImGui::SetItemTooltip("%s", Str::ScOnlyBoundTip.c_str());
    ImGui::SameLine();
    // Stable "###screset" id (display stays the icon+label) so tests target it by id, not the glyph.
    if (ImGui::Button(fmtScratch("{}###screset", resetLabel)))
        m_openResetModal = true;
    ImGui::SetItemTooltip("%s", Str::ScResetTip.c_str());

    renderPresetBar();

    ImGui::Spacing();
    ImGui::BeginChild("##shortcut_list", {0.f, 0.f});

    // Gamepad / analog tunables. Sliders push a
    // ModifyEvent<AppSettings> on release so the value persists and applies live (UI pushes events only — no direct
    // mutation). Collapsed by default. Rendered inside the list child so its header shares the same horizontal
    // padding as the per-group headers below it.
    // Stable "###gamepad_analog" id: the visible label carries a '/', which the UI-test path parser
    // treats as a separator, so tests target the header (and its sliders) through the id instead.
    if (ImGui::CollapsingHeader(Str::ScGamepadAnalog.id("gamepad_analog"))) {
        float deadzone = appSettings.input.deadzone;
        float smoothing = appSettings.input.smoothing;
        bool changed = false;
        // Stable "###" ids so UI tests target the sliders by id, not the visible label.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScDeadzone.id("deadzone"), &deadzone, 0.0f, 0.5f, "%.2f"))
            changed = true;
        ImGui::SetItemTooltip("%s", Str::ScDeadzoneTip.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScSmoothing.id("smoothing"), &smoothing, 0.0f, 1.0f, "%.2f s"))
            changed = true;
        ImGui::SetItemTooltip("%s", Str::ScSmoothingTip.c_str());
        if (changed)
            eq.push(ofs::ModifyEvent<ofs::AppSettings>{[deadzone, smoothing](ofs::AppSettings &s) {
                s.input.deadzone = deadzone;
                s.input.smoothing = smoothing;
            }});
    }

    // Global cadence for every "fire while held" binding (frame-step, move, volume, …). Read live each
    // tick by OfsApp::holdRepeatParams(), so edits here apply instantly.
    if (ImGui::CollapsingHeader(Str::ScHoldRepeat.id("hold_repeat"))) {
        float delay = appSettings.holdRepeat.initialDelay;
        float interval = appSettings.holdRepeat.interval;
        float accel = appSettings.holdRepeat.accel;
        float maxRate = appSettings.holdRepeat.maxRateHz;
        bool changed = false;

        // Min 0 allows "no delay" — a held key starts repeating immediately. "%.2f s" shows 0.00 s there.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScInitialDelay.id("hold_delay"), &delay, 0.00f, 1.00f, "%.2f s"))
            changed = true;
        ImGui::SetItemTooltip("%s", Str::ScInitialDelayTip.c_str());

        // Present the inter-fire gap as a starting rate (repeats/sec) — users think in speed, not gap.
        float rate = interval > 0.0f ? 1.0f / interval : 0.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScRepeatSpeed.id("hold_speed"), &rate, 1.0f, 100.0f, "%.0f /s")) {
            interval = 1.0f / rate;
            changed = true;
        }
        ImGui::SetItemTooltip("%s", Str::ScRepeatSpeedTip.c_str());

        // Acceleration as an intuitive 0–100 % (0 = steady, 100 = strongest ramp). The stored factor is
        // inverted — 1.00 is no acceleration and kAccelMin is the strongest — so map both ways here.
        constexpr float kAccelMin = 0.85f;
        float accelPct = (1.0f - accel) / (1.0f - kAccelMin) * 100.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScAcceleration.id("hold_accel"), &accelPct, 0.0f, 100.0f, "%.0f %%")) {
            accel = 1.0f - (accelPct / 100.0f) * (1.0f - kAccelMin);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", Str::ScAccelerationTip.c_str());

        // The accel target, clamped to the hard burst-safety ceiling (kHoldRepeatMaxRateHz).
        const auto maxRateCap = static_cast<float>(ofs::kHoldRepeatMaxRateHz);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat(Str::ScMaxSpeed.id("hold_maxspeed"), &maxRate, 5.0f, maxRateCap, "%.0f /s")) {
            maxRate = std::min(maxRate, maxRateCap);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", Str::ScMaxSpeedTip.c_str());

        // Live one-line preview of the resulting feel. With no acceleration the cadence is constant;
        // otherwise it ramps from the starting rate to the configured maximum, reached after `ramp` seconds.
        const float effMaxRate = std::min(maxRate, maxRateCap);
        const float floorGap = 1.0f / effMaxRate;
        const int startHz = static_cast<int>(std::lround(interval > 0.0f ? 1.0f / interval : 0.0f));
        const auto topHz = static_cast<int>(std::lround(effMaxRate));
        // No ramp when steady, or when the starting rate already meets/exceeds the ceiling. The sustained
        // cadence is then the smaller of the two (holdRepeats clamps the gap to the floor).
        if (accel >= 1.0f || interval <= floorGap) {
            ImGui::TextDisabled("%s", Str::ScHoldRepeatPreviewSteady.fmt(std::min(startHz, topHz)));
        } else {
            // Gaps until interval·accel^m reaches the floor, then summed: delay + interval·(1−accel^m)/(1−accel).
            const float m = std::log(floorGap / interval) / std::log(accel);
            const float ramp = delay + interval * (1.0f - std::pow(accel, m)) / (1.0f - accel);
            const char *secs = fmtScratch("{:.1f}", ramp);
            ImGui::TextDisabled("%s", Str::ScHoldRepeatPreview.fmt(startHz, topHz, secs));
        }

        if (changed)
            eq.push(ofs::ModifyEvent<ofs::AppSettings>{[delay, interval, accel, maxRate](ofs::AppSettings &s) {
                s.holdRepeat.initialDelay = delay;
                s.holdRepeat.interval = interval;
                s.holdRepeat.accel = accel;
                s.holdRepeat.maxRateHz = maxRate;
            }});
    }

    // Group bindable commands by group; a command matches if the filter hits its title, group,
    // id, or any of its bound triggers (ORed). An empty filter passes everything, so the trigger
    // loop below is skipped in the common case.
    // Built into the per-frame arena (no std::map / std::vector churn): collect matches into a
    // flat array, then sort by group so equal-group runs are contiguous. The sort key carries the
    // registry index so ties keep registry order — reproducing the old std::map's alphabetical
    // groups with insertion-ordered rows.
    const auto &allCommands = commandRegistry_.all();
    struct Entry {
        const Command *cmd;
        int order;
    };
    auto *matched = ofs::FrameAllocator::instance().allocArray<Entry>(allCommands.empty() ? 1 : allCommands.size());
    int matchCount = 0;
    for (const auto &cmd : allCommands) {
        const bool hasBinding = hasValidBinding(cmd.id);
        // inRebindList=false commands (providers, window openers) aren't listed by default — they appear once
        // the user has bound one (added through the "Add command…" picker). A listed command always shows.
        // They remain assignable either way (§3 Amendment A).
        if (!cmd.inRebindList && !hasBinding)
            continue;
        // "Only bound" hides commands with no (valid) binding — the fastest way to shrink a long list
        // down to just what the user has actually customized.
        if (m_onlyBound && !hasBinding)
            continue;
        // Match the localized title and localized group name only — fuzzy search runs against the
        // active language with no English fallback (mirrors the command palette). cmd.id stays in the
        // haystack: it's a technical handle (e.g. "core.save"), not translatable display text, and
        // plugin authors search bindings by id.
        bool match =
            ofs::util::fuzzyMatchAny(filter_, {cmd.title.c_str(), commandRegistry_.groupDisplayName(cmd.group), cmd.id})
                .matched;
        for (const auto &b : bindingSystem_.bindings()) {
            if (match)
                break;
            if (b.commandId != cmd.id)
                continue;
            if (const auto *kc = std::get_if<KeyChord>(&b.trigger); kc && kc->key == SDLK_UNKNOWN)
                continue;
            match = ofs::util::fuzzyMatch(filter_, formatTrigger(b.trigger)).matched;
        }
        if (!match)
            continue;
        matched[matchCount] = {.cmd = &cmd, .order = matchCount};
        ++matchCount;
    }
    std::ranges::sort(matched, matched + matchCount, [](const Entry &a, const Entry &b) {
        const int c = a.cmd->group.compare(b.cmd->group);
        return c < 0 || (c == 0 && a.order < b.order);
    });

    if (matchCount == 0)
        ImGui::TextDisabled("%s", Str::ScNoMatch.c_str());

    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;
    const float tableW = ImGui::GetContentRegionAvail().x - ofs::ui::kRightGap;

    // Unfiltered, groups start collapsed so the window opens as a short list of group headers (the full
    // command set is long). A narrowed set is worth showing expanded — but how it's forced open differs:
    //  • An active filter forces open *every frame* (ImGuiCond_Always): while searching you want all hits
    //    visible, and the state is transient, so you rarely want to collapse mid-search.
    //  • "Only bound" is a persistent browse mode, so it expands only on its *rising edge* (the frame it
    //    turns on). Forcing every frame would also block the user from collapsing a group while it's on —
    //    the reported bug. A one-shot open expands-by-default yet leaves each header collapsible.
    // m_onlyBoundPrev resets on window close (the early-out above), so reopening with it still checked
    // re-expands.
    const bool onlyBoundRising = m_onlyBound && !m_onlyBoundPrev;
    m_onlyBoundPrev = m_onlyBound;
    const bool forceOpen = !filter_.empty() || onlyBoundRising;

    for (int gi = 0; gi < matchCount;) {
        // matched is sorted by group, so this group's rows are the contiguous run [gi, groupEnd).
        const std::string &groupName = matched[gi].cmd->group;
        int groupEnd = gi;
        while (groupEnd < matchCount && matched[groupEnd].cmd->group == groupName)
            ++groupEnd;

        // Collapsing headers let the user fold away whole groups. The count rides in the visible label;
        // the localized group name is the display text, while the "###grp_<English>" id stays the
        // canonical name so the header keeps its identity across languages and count changes.
        if (forceOpen)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (!ImGui::CollapsingHeader(fmtScratch("{} ({})###grp_{}", commandRegistry_.groupDisplayName(groupName),
                                                groupEnd - gi, groupName.c_str()))) {
            gi = groupEnd;
            continue;
        }
        ImGui::PushID(groupName.c_str());

        // Wide enough for the Press/Hold combo (longest of the *translated* labels + the dropdown arrow
        // + paddings). Measuring the actual rendered strings — not an English literal — keeps the column
        // from clipping when a translation runs longer than "Press"/"Hold".
        const float labelW = std::max(ImGui::CalcTextSize(Str::ScPress).x, ImGui::CalcTextSize(Str::ScHold).x);
        const float modeColW = labelW + ImGui::GetFrameHeight() + style.FramePadding.x * 4.f;
        // Add column sized to the "+" icon button (glyph + frame padding), font/DPI-relative — a
        // hardcoded literal would be too narrow at larger fonts, clipping the glyph and pinning it left
        // (ImGui drops the center alignment once the text overflows its button).
        const float addColW = ImGui::CalcTextSize(ICON_PLUS).x + style.FramePadding.x * 2.f;
        if (ImGui::BeginTable("##bindings", 4, kTableFlags, {tableW, 0.f})) {
            ImGui::TableSetupColumn(Str::ScColAction.id("col_action"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(Str::ScColBinding.id("col_binding"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
            ImGui::TableSetupColumn(Str::ScColMode.id("col_mode"), ImGuiTableColumnFlags_WidthFixed, modeColW);
            ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, addColW);
            ImGui::TableHeadersRow();

            for (int ri = gi; ri < groupEnd; ++ri) {
                const Command *cmd = matched[ri].cmd;
                ImGui::TableNextRow();
                ImGui::PushID(cmd->id.c_str());

                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(cmd->title.c_str());
                // Right-aligned edit + delete cluster for custom commands (the only rows the user can edit
                // or remove here). One offset for the pair keeps the trailing trash button on the cell edge
                // regardless of title length.
                if (cmd->source == CommandSource::Custom) {
                    ImGui::SameLine();
                    const float editW = ImGui::CalcTextSize(ICON_EDIT).x + style.FramePadding.x * 2.f;
                    const float trashW = ImGui::CalcTextSize(ICON_TRASH).x + style.FramePadding.x * 2.f;
                    const float clusterW = editW + trashW + style.ItemSpacing.x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - clusterW);
                    if (ImGui::Button(fmtScratch("{}###cmdedit", ICON_EDIT))) {
                        // Reopen the editor on the stored definition (the registry Command can't carry
                        // the params back — see customStore_).
                        const auto &defs = customStore_.commands();
                        if (auto it =
                                std::ranges::find_if(defs, [cmd](const CustomCommand &c) { return c.id == cmd->id; });
                            it != defs.end()) {
                            m_customDraft = *it;
                            m_openCustomModal = true;
                        }
                    }
                    ImGui::SetItemTooltip("%s", Str::ScCustomEditTip.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(fmtScratch("{}###cmddelete", ICON_TRASH))) {
                        m_pendingDeleteCustomId = cmd->id;
                        m_pendingDeleteCustomName = cmd->title.c_str();
                    }
                    ImGui::SetItemTooltip("%s", Str::ScCustomDeleteTip.c_str());
                }
                // A custom command the user named carries its canonical action as a dimmed subtitle, so the
                // row still says what it does. Empty for native/plugin/dynamic rows and for an unnamed
                // custom (whose title already is that action).
                if (!cmd->subtitle.empty())
                    ImGui::TextDisabled("%s", cmd->subtitle.c_str());

                ImGui::TableSetColumnIndex(1);
                // Render each binding for this command on its own line: trigger + remove button.
                bool first = true;
                for (const auto &b : bindingSystem_.bindings()) {
                    if (b.commandId != cmd->id)
                        continue;

                    // Skip invalid/empty triggers.
                    if (const auto *kc = std::get_if<KeyChord>(&b.trigger)) {
                        if (kc->key == SDLK_UNKNOWN)
                            continue;
                    }

                    first = false;

                    // Unique per-trigger ImGui id.
                    int uid = 0;
                    if (const auto *kc = std::get_if<KeyChord>(&b.trigger))
                        uid = static_cast<int>(kc->key) ^ (static_cast<int>(kc->modifiers) << 16);
                    else if (const auto *pb = std::get_if<PadButton>(&b.trigger))
                        uid = 0x10000 + static_cast<int>(pb->button);
                    else if (const auto *pa = std::get_if<PadAxis>(&b.trigger))
                        uid = 0x20000 + static_cast<int>(pa->axis) * 2 + (pa->positive ? 1 : 0);

                    ImGui::PushID(uid);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(formatTrigger(b.trigger));

                    // Held-modifier affordance — gamepad triggers only. A modified binding fires only
                    // while its modifier (a held pad button/trigger/stick) is held, shadowing an
                    // unmodified binding on the same trigger. Keyboard chords carry their own
                    // Ctrl/Shift/Alt, so they have no held-gate affordance.
                    if (!std::holds_alternative<KeyChord>(b.trigger)) {
                        const bool hasMod = !std::holds_alternative<std::monostate>(b.modifier);
                        const char *icon = ICON_GAMEPAD;
                        auto startModifierCapture = [&]() {
                            beginCapture(eq,
                                         {.commandId = cmd->id, .captureModifier = true, .modifierTarget = b.trigger});
                        };
                        ImGui::SameLine();
                        if (hasMod) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                            if (ImGui::SmallButton(fmtScratch("{} {}###modchip", icon, formatModifier(b.modifier))))
                                ImGui::OpenPopup("##modmenu");
                            ImGui::PopStyleColor();
                            ImGui::SetItemTooltip("%s", Str::ScModChipTip.fmt(formatModifier(b.modifier)));
                            if (ImGui::BeginPopup("##modmenu")) {
                                if (ImGui::MenuItem(Str::ScChangeModifier))
                                    startModifierCapture();
                                if (ImGui::MenuItem(Str::ScClearModifier))
                                    eq.push(SetBindingModifierEvent{
                                        .trigger = b.trigger, .commandId = cmd->id, .modifier = std::monostate{}});
                                ImGui::EndPopup();
                            }
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                            const bool clicked = ImGui::SmallButton(fmtScratch("{}+###modadd", icon));
                            ImGui::PopStyleColor();
                            ImGui::SetItemTooltip("%s", Str::ScAddModifierTip.c_str());
                            if (clicked)
                                startModifierCapture();
                        }
                    }

                    // Right-align the rebind + delete cluster to the cell edge so the buttons line up
                    // in a column regardless of trigger-text length. One offset for the pair keeps the
                    // trailing trash button on the cell edge.
                    ImGui::SameLine();
                    const float rebindW = ImGui::CalcTextSize(ICON_PENCIL).x + style.FramePadding.x * 2.f;
                    const float trashW = ImGui::CalcTextSize(ICON_TRASH).x + style.FramePadding.x * 2.f;
                    const float clusterW = rebindW + trashW + style.ItemSpacing.x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - clusterW);
                    // Re-capture: listen for a new trigger and overwrite this binding in place (the old
                    // trigger is removed and the new one inherits this binding's mode/modifier). Stable
                    // "###bindrebind" id (display stays the glyph) so tests target it by id.
                    if (ImGui::Button(fmtScratch("{}###bindrebind", ICON_PENCIL)))
                        beginCapture(eq, {.commandId = cmd->id, .replaceTrigger = true, .replaceTarget = b.trigger});
                    ImGui::SetItemTooltip("%s", Str::ScRebindBindingTip.c_str());
                    ImGui::SameLine();
                    // Stable "###bindremove" id (display stays the glyph) so tests target it by id.
                    if (ImGui::Button(fmtScratch("{}###bindremove", ICON_TRASH)))
                        eq.push(RemoveBindingEvent{.trigger = b.trigger, .commandId = cmd->id});
                    ImGui::SetItemTooltip("%s", Str::ScRemoveBindingTip.c_str());
                    ImGui::PopID();
                }
                if (first) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", Str::ScNone.c_str());
                }

                // Mode column: one Press/Hold selector per trigger line, in the same order and with the
                // same skip rules as the Binding cell above so the rows align vertically. Hold ticks the
                // command every frame while held (replacing OS key-repeat); it is only meaningful for a
                // holdable command (one with a tick), so the selector is disabled otherwise — kept visible
                // to preserve alignment.
                ImGui::TableSetColumnIndex(2);
                for (const auto &b : bindingSystem_.bindings()) {
                    if (b.commandId != cmd->id)
                        continue;
                    const auto *kc = std::get_if<KeyChord>(&b.trigger);
                    if (kc && kc->key == SDLK_UNKNOWN)
                        continue;

                    int uid = 0;
                    if (kc)
                        uid = static_cast<int>(kc->key) ^ (static_cast<int>(kc->modifiers) << 16);
                    else if (const auto *pb = std::get_if<PadButton>(&b.trigger))
                        uid = 0x10000 + static_cast<int>(pb->button);
                    else if (const auto *pa = std::get_if<PadAxis>(&b.trigger))
                        uid = 0x20000 + static_cast<int>(pa->axis) * 2 + (pa->positive ? 1 : 0);

                    ImGui::PushID(uid);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    // Hold is meaningful for any holdable command (one with a tick), on the keyboard, on
                    // gamepad buttons (up-tracked), and on analog axes (which are always continuous holds).
                    const bool selectable = cmd->holdable();
                    ImGui::BeginDisabled(!selectable);
                    int modeIdx = (b.mode == ActivationMode::Hold) ? 1 : 0;
                    // Array form (not the "Press\0Hold\0" packed string) so each option carries its own
                    // translation and a stable ###id; the visible label hides after ###, tests target the id.
                    const char *modeItems[] = {Str::ScPress.id("mode_press"), Str::ScHold.id("mode_hold")};
                    if (ImGui::Combo("##mode", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
                        eq.push(
                            SetBindingModeEvent{.trigger = b.trigger,
                                                .commandId = cmd->id,
                                                .mode = modeIdx == 1 ? ActivationMode::Hold : ActivationMode::Press});
                    ImGui::EndDisabled();
                    if (!selectable)
                        ImGui::SetItemTooltip("%s", Str::ScModeDisabledTip.c_str());
                    else
                        ImGui::SetItemTooltip("%s", Str::ScModeTip.c_str());
                    ImGui::PopID();
                }

                ImGui::TableSetColumnIndex(3);
                // Stable "###bindadd" id (display stays the glyph) so tests target it by id.
                if (ImGui::Button(fmtScratch("{}###bindadd", ICON_PLUS), {-FLT_MIN, 0.f}))
                    beginCapture(eq, {.commandId = cmd->id});
                ImGui::SetItemTooltip("%s", Str::ScAddBindingTip.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
        gi = groupEnd;
    }

    ImGui::EndChild();

    // Reset-to-defaults confirmation (raised once from the top-row button). The modal body runs in later
    // frames, so it closes over the long-lived EventQueue by pointer (not the render() reference param).
    if (m_openResetModal) {
        m_openResetModal = false;
        confirmModal(eq, Str::ScResetTitle.c_str(), Str::ScResetBody.c_str(), Str::ScReset, "resetconfirm",
                     "resetcancel", [eqp = &eq]() {
                         eqp->push(ResetBindingsEvent{});
                         // Global input tunables live in AppSettings — reset them through the same
                         // ModifyEvent path the sliders use, so OfsApp reapplies them live.
                         eqp->push(ofs::ModifyEvent<ofs::AppSettings>{[](ofs::AppSettings &s) {
                             s.input = ofs::InputSettings{};
                             s.holdRepeat = ofs::HoldRepeatSettings{};
                         }});
                     });
    }

    // Provider-category target modal, raised after its picker category was chosen. Picking a target inside
    // calls beginCapture(), which raises the capture modal on its own (the capture modal is not latched here).
    if (!m_addProviderGroup.empty()) {
        const std::string group = m_addProviderGroup;
        m_addProviderGroup.clear();
        showCustomModal(
            eq, {.title = commandRegistry_.groupDisplayName(group),
                 .width = 380.0f,
                 .body = [this, group, eqp = &eq]() -> bool { return renderProviderTargetModal(*eqp, group); }});
    }

    // ── Preset modals (latched by renderPresetBar; raised here, outside the table/child) ──

    if (m_openSaveAsModal) {
        m_openSaveAsModal = false;
        showCustomModal(
            eq, {.title = Str::ScSavePresetTitle.c_str(), .width = 380.0f, .body = [this, eqp = &eq]() -> bool {
                     ImGui::TextUnformatted(Str::ScPresetNameLabel);
                     ImGui::SetNextItemWidth(-FLT_MIN);
                     const bool enter =
                         ImGui::InputText("###presetname", &m_presetNameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
                     ImGui::Spacing();
                     const bool hasName = !m_presetNameBuf.empty();
                     ImGui::BeginDisabled(!hasName);
                     const bool save =
                         ImGui::Button(Str::ScSave.id("presetsaveconfirm"), {buttonW(Str::ScSave), 0.f}) ||
                         (enter && hasName);
                     ImGui::EndDisabled();
                     if (save) {
                         eqp->push(SaveBindingPresetEvent{.name = m_presetNameBuf});
                         m_presetsLoaded = false; // re-fetch the list next render, after the save drains
                         return true;
                     }
                     ImGui::SameLine();
                     if (ImGui::Button(Str::ScCancel.id("presetsavecancel"), {buttonW(Str::ScCancel), 0.f}))
                         return true;
                     return false;
                 }});
    }

    if (!m_pendingLoadSlug.empty()) {
        const std::string slug = m_pendingLoadSlug;
        const char *body = Str::ScLoadBody.fmt(m_pendingLoadName);
        m_pendingLoadSlug.clear();
        confirmModal(eq, Str::ScLoadTitle.c_str(), body, Str::ScLoad, "loadconfirm", "loadcancel",
                     [this, slug, name = m_pendingLoadName, eqp = &eq]() {
                         // The handler loads the preset and emits the (possibly partial) load notification.
                         eqp->push(LoadBindingPresetEvent{.slug = slug, .name = name});
                         m_selectedPresetSlug = slug;
                     });
    }

    if (!m_pendingDeleteSlug.empty()) {
        const std::string slug = m_pendingDeleteSlug;
        const char *body = Str::ScDeleteBody.fmt(m_pendingDeleteName);
        m_pendingDeleteSlug.clear();
        confirmModal(eq, Str::ScDeleteTitle.c_str(), body, Str::ScDelete, "deleteconfirm", "deletecancel",
                     [this, slug, eqp = &eq]() {
                         eqp->push(DeleteBindingPresetEvent{.slug = slug});
                         m_presetsLoaded = false; // re-fetch the list next render, after the delete drains
                         if (m_selectedPresetSlug == slug)
                             m_selectedPresetSlug.clear();
                     });
    }

    // Custom-command editor (new or edit). Raised once; the body runs deferred, editing m_customDraft.
    if (m_openCustomModal) {
        m_openCustomModal = false;
        showCustomModal(
            eq, {.title = m_customDraft.id.empty() ? Str::ScCustomNewTitle.c_str() : Str::ScCustomEditTitle.c_str(),
                 .width = 420.0f,
                 .body = [this, eqp = &eq]() -> bool { return renderCustomEditor(*eqp); }});
    }

    // Custom-command delete confirmation. RemoveCustomCommandEvent is handled independently by the store
    // (drops the def) and BindingSystem (prunes its bindings).
    if (!m_pendingDeleteCustomId.empty()) {
        const std::string id = m_pendingDeleteCustomId;
        const char *body = Str::ScCustomDeleteBody.fmt(m_pendingDeleteCustomName);
        m_pendingDeleteCustomId.clear();
        confirmModal(eq, Str::ScCustomDeleteTitle.c_str(), body, Str::ScDelete, "customdeleteconfirm",
                     "customdeletecancel", [id, eqp = &eq]() { eqp->push(ofs::RemoveCustomCommandEvent{id}); });
    }

    ImGui::End();
}

bool ShortcutWindow::renderCustomEditor(EventQueue &eq) {
    // The kind is chosen upstream — the Add-command picker entry (create) or the existing command (edit) — so
    // the editor shows it as a header, not a picker. One less control, and an edit can't swap a command's
    // kind out from under its params/bindings.
    const CustomCommandTemplate *t = templates_.find(m_customDraft.templateKey);
    if (t)
        ImGui::SeparatorText(t->displayName.c_str());

    // Name is optional — left blank, the command takes the template's live summary as its title. The hint
    // shows that summary so the placeholder previews the auto-name. std::string-bound InputText so a CJK
    // title isn't truncated at the byte cap.
    ImGui::TextUnformatted(Str::ScCustomName);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char *nameHint = t && t->summary ? t->summary(m_customDraft.params) : Str::ScCustomNameHint.c_str();
    ImGui::InputTextWithHint("###custom_name", nameHint, &m_customDraft.name);

    // The template draws its own parameter widgets, editing its opaque param bag — the Shortcut window
    // knows nothing about any specific kind's fields ("the UI is rendered from a function on the provider").
    if (t && t->renderEditor)
        t->renderEditor(m_customDraft.params);

    ImGui::Separator();
    ImGui::Spacing();

    bool close = false;
    if (ImGui::Button(Str::ScSave.id("customsaveconfirm"), {buttonW(Str::ScSave), 0.f})) {
        // An existing draft carries its "custom.N" id (Update keeps it); a fresh one has an empty id the
        // store assigns on Add.
        if (m_customDraft.id.empty())
            eq.push(ofs::AddCustomCommandEvent{m_customDraft});
        else
            eq.push(ofs::UpdateCustomCommandEvent{m_customDraft});
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Str::ScCancel.id("customcancel"), {buttonW(Str::ScCancel), 0.f}))
        close = true;
    return close;
}

void ShortcutWindow::renderAddCommandPicker() {
    // Custom-command kinds: one entry per registered template. Picking one opens the editor scoped to that
    // kind — the list entry *is* the kind choice, so the editor needs no in-modal kind picker. The "###<key>"
    // id is stable (the template key) and language-independent.
    for (const auto &t : templates_.templates()) {
        if (ImGui::Selectable(t.displayName.iconId(ICON_WAND_2, t.key.c_str()))) {
            m_customDraft = CustomCommand{};
            m_customDraft.templateKey = t.key;
            m_openCustomModal = true;
            ImGui::CloseCurrentPopup();
        }
    }

    // Provider categories: the distinct groups among registry commands not listed by default (inRebindList=false)
    // and not yet bound. One entry per group — so "Select Axis" is a single "Select Axis…" row, not twenty —
    // and picking it opens the target modal to choose the concrete command. Collected into the frame arena
    // (no per-render std::vector); groups are deduped by comparing the group name.
    const auto &all = commandRegistry_.all();
    auto **groups = ofs::FrameAllocator::instance().allocArray<const std::string *>(all.empty() ? 1 : all.size());
    int groupCount = 0;
    for (const auto &cmd : all) {
        if (cmd.inRebindList || hasValidBinding(cmd.id))
            continue;
        if (std::none_of(groups, groups + groupCount, [&cmd](const std::string *g) { return *g == cmd.group; }))
            groups[groupCount++] = &cmd.group;
    }
    if (groupCount > 0)
        ImGui::Separator();
    for (int i = 0; i < groupCount; ++i) {
        const std::string &g = *groups[i];
        const char *icon = commandRegistry_.groupIcon(g);
        // "<Group>…" — the ellipsis signals it opens a chooser. Stable "###addcat_<group>" id for tests.
        const char *label =
            icon[0] != '\0' ? fmtScratch("{} {}…###addcat_{}", icon, commandRegistry_.groupDisplayName(g), g.c_str())
                            : fmtScratch("{}…###addcat_{}", commandRegistry_.groupDisplayName(g), g.c_str());
        if (ImGui::Selectable(label)) {
            m_addProviderGroup = g; // latches the target modal, raised after the popup closes
            ImGui::CloseCurrentPopup();
        }
    }
}

bool ShortcutWindow::renderProviderTargetModal(EventQueue &eq, const std::string &group) {
    ImGui::TextWrapped("%s", Str::ScAddProviderPrompt.c_str());
    ImGui::Spacing();
    // One selectable per unbound command in this group (e.g. each axis under "Select Axis"). Picking one
    // sets up rebindState and latches the capture modal — the same flow the table "+" uses. Contextual no-op
    // targets (isEnabled=false, e.g. the active axis) are still offered: a binding to them survives and the
    // table shows it once bound.
    ImGui::BeginChild("##provtargets", {0.f, ImGui::GetFontSize() * 12.f});
    bool close = false;
    for (const auto &cmd : commandRegistry_.all()) {
        if (cmd.inRebindList || cmd.group != group || hasValidBinding(cmd.id))
            continue;
        if (ImGui::Selectable(fmtScratch("{}###provpick_{}", cmd.title.c_str(), cmd.id.c_str()))) {
            beginCapture(eq, {.commandId = cmd.id});
            close = true;
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button(Str::ScCancel.id("provcancel"), {buttonW(Str::ScCancel), 0.f}))
        close = true;
    return close;
}

bool ShortcutWindow::hasValidBinding(const std::string &id) const {
    return std::ranges::any_of(bindingSystem_.bindings(), [&id](const Binding &b) {
        if (b.commandId != id)
            return false;
        const auto *kc = std::get_if<KeyChord>(&b.trigger);
        return !(kc && kc->key == SDLK_UNKNOWN);
    });
}

void ShortcutWindow::renderPresetBar() {
    // Resolve the current selection against the cached list.
    const char *preview = Str::ScSelectPreset;
    int selIdx = -1;
    for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
        if (m_presets[i].slug == m_selectedPresetSlug) {
            preview = m_presets[i].name.c_str();
            selIdx = i;
        }
    }

    // Stretch the combo to fill the row, with the three buttons pinned right — matching the top row's
    // full-width feel. Each button's reserved width includes its leading ItemSpacing gap; sizing to the
    // translated label keeps the combo from over/under-reserving in any language.
    const ImGuiStyle &style = ImGui::GetStyle();
    const float buttonsW = ofs::ui::buttonW(Str::ScLoad) + ofs::ui::buttonW(Str::ScSaveAs) +
                           ofs::ui::buttonW(Str::ScDelete) + style.ItemSpacing.x * 3.f;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ofs::ui::kRightGap - buttonsW);
    if (ImGui::BeginCombo("###presetcombo", preview)) {
        for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
            const PresetInfo &p = m_presets[i];
            // The preset name is user data (untranslated); only the "(built-in)" suffix is localized.
            // A stable "###preset_opt_<slug>" id keeps each option addressable by id in any language.
            const char *visible = p.builtin ? Str::ScBuiltinSuffix.fmt(p.name) : p.name.c_str();
            if (ImGui::Selectable(fmtScratch("{}###preset_opt_{}", visible, p.slug.c_str()), i == selIdx))
                m_selectedPresetSlug = p.slug;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(selIdx < 0);
    if (ImGui::Button(Str::ScLoad.id("presetload"))) {
        m_pendingLoadSlug = m_presets[selIdx].slug;
        m_pendingLoadName = m_presets[selIdx].name;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", Str::ScPresetLoadTip.c_str());

    ImGui::SameLine();
    if (ImGui::Button(Str::ScSaveAs.id("presetsaveas"))) {
        m_presetNameBuf.clear();
        m_openSaveAsModal = true;
    }
    ImGui::SetItemTooltip("%s", Str::ScPresetSaveAsTip.c_str());

    ImGui::SameLine();
    const bool canDelete = selIdx >= 0 && !m_presets[selIdx].builtin;
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button(Str::ScDelete.id("presetdelete"))) {
        m_pendingDeleteSlug = m_presets[selIdx].slug;
        m_pendingDeleteName = m_presets[selIdx].name;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", (canDelete ? Str::ScPresetDeleteTip : Str::ScPresetDeleteDisabledTip).c_str());
}

} // namespace ofs
