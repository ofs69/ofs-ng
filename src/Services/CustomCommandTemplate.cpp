#include "Services/CustomCommandTemplate.h"

#include "Core/CustomCommand.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "Services/BindingSystem.h"

#include <algorithm>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>

namespace ofs {

namespace {

// Symbolic enum names — stable across builds, unlike the integer values (the bindings.json rule, CP
// §16.2). An unknown string leaves `out` at its default (Frame) — the bag round-trips verbatim and only
// degrades to the default at use, never dropping the command.
const char *granularityToString(StepGranularity g) {
    switch (g) {
    case StepGranularity::Frame:
        return "frame";
    case StepGranularity::Action:
        return "action";
    case StepGranularity::ActionAllAxes:
        return "action-all-axes";
    }
    return "frame";
}
bool granularityFromString(const std::string &s, StepGranularity &out) {
    if (s == "frame")
        out = StepGranularity::Frame;
    else if (s == "action")
        out = StepGranularity::Action;
    else if (s == "action-all-axes")
        out = StepGranularity::ActionAllAxes;
    else
        return false;
    return true;
}

// Localized fragment for a direction / granularity value — shared by the editors and the summaries.
const char *directionWord(StepDirection d) {
    return d == StepDirection::Backward ? Str::ScCustomDirBackward.c_str() : Str::ScCustomDirForward.c_str();
}
const char *granularityWord(StepGranularity g) {
    switch (g) {
    case StepGranularity::Action:
        return Str::ScCustomGranAction.c_str();
    case StepGranularity::ActionAllAxes:
        return Str::ScCustomGranActionAll.c_str();
    case StepGranularity::Frame:
        break;
    }
    return Str::ScCustomGranFrame.c_str();
}

// One-line localized descriptions of a param bag. fmtScratch/TrKey::fmt return frame-arena pointers — the
// caller copies them into owned Command strings the same frame.
const char *stepSummary(const nlohmann::json &p) {
    const auto dir = static_cast<StepDirection>(p.value("direction", 1));
    const int reps = std::max(1, p.value("reps", 1));
    StepGranularity gran = StepGranularity::Frame;
    granularityFromString(p.value("granularity", std::string{"frame"}), gran);
    return Str::ScCustomSummaryStep.fmt(directionWord(dir), reps, granularityWord(gran));
}
const char *movePosSummary(const nlohmann::json &p) {
    return Str::ScCustomSummaryMovePos.fmt(p.value("delta", 1));
}
const char *moveTimeSummary(const nlohmann::json &p) {
    const auto dir = static_cast<StepDirection>(p.value("direction", 1));
    const int reps = std::max(1, p.value("reps", 1));
    const char *seek = p.value("seekAfter", false) ? Str::ScCustomSummarySeekSuffix.c_str() : "";
    return Str::ScCustomSummaryMoveTime.fmt(directionWord(dir), reps, seek);
}

// Wrap a per-kind emitter (eq, burst, first) into a holdable Command. The built-in step/move verbs inline
// this same run/tick pairing; a custom command differs only in supplying user-chosen params. `burst` is
// the held-repeat coalesce count (1 on a single press), so the authored amount is the base, scaled by N.
// The cadence is read live each tick (appSettings.holdRepeat), identical to OfsApp::holdRepeatParams(),
// so a Shortcut-window edit applies instantly and every hold binding shares one tunable feel. `title` is
// the resolved row title (user name, or the summary when unnamed); `subtitle` the dimmed canonical action
// (empty when the title already is the summary).
template <class Emit>
Command holdCommand(const std::string &id, const char *title, const char *subtitle, const AppSettings &appSettings,
                    const Emit &step) {
    Command c;
    c.id = id;
    c.group = "Custom";
    c.title = std::string{title}; // owned std::string (TrString) — a user title, never run through the catalog
    c.subtitle = subtitle;
    c.source = CommandSource::Custom;
    c.run = [step](EventQueue &eq) { step(eq, 1, true); };
    c.tick = [step, &appSettings](EventQueue &eq, const HoldTickInfo &info) {
        const HoldRepeatParams hp{.initialDelay = appSettings.holdRepeat.initialDelay,
                                  .interval = appSettings.holdRepeat.interval,
                                  .accel = appSettings.holdRepeat.accel,
                                  .maxRateHz = appSettings.holdRepeat.maxRateHz};
        const int burst = holdRepeats(info.elapsed, info.dt, hp);
        if (burst > 0)
            step(eq, burst, info.first);
    };
    return c;
}

// Direction (Forward/Backward) and Count are shared by the Step and Move-time editors. Both edit the bag
// in place, reading defensively so a missing key falls back to the documented default.
void directionCombo(nlohmann::json &p) {
    ImGui::TextUnformatted(Str::ScCustomDirection);
    const char *items[] = {Str::ScCustomDirForward.id("dir_fwd"), Str::ScCustomDirBackward.id("dir_back")};
    int idx = p.value("direction", 1) == static_cast<int>(StepDirection::Backward) ? 1 : 0;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("###custom_dir", &idx, items, IM_ARRAYSIZE(items)))
        p["direction"] = static_cast<int>(idx == 1 ? StepDirection::Backward : StepDirection::Forward);
}
void countInput(nlohmann::json &p) {
    ImGui::TextUnformatted(Str::ScCustomCount);
    ImGui::SetNextItemWidth(-FLT_MIN);
    int reps = std::max(1, p.value("reps", 1));
    if (ImGui::InputInt("###custom_reps", &reps))
        p["reps"] = std::max(1, reps);
}

// Resolve a definition's row title + subtitle from its name and a summary: an unnamed command shows the
// summary as its title (no subtitle); a named one keeps its name with the summary dimmed beneath.
struct Labels {
    const char *title;
    const char *subtitle;
};
Labels labels(const CustomCommand &def, const char *summary) {
    return def.name.empty() ? Labels{.title = summary, .subtitle = ""}
                            : Labels{.title = def.name.c_str(), .subtitle = summary};
}

} // namespace

void registerBuiltinCommandTemplates(CustomCommandTemplateRegistry &registry, ScriptProject &project,
                                     const AppSettings &appSettings) {
    // ── Step ── navigation only; no undo, no gesture. The navigator ignores reps for ActionAllAxes.
    registry.add(CustomCommandTemplate{
        .key = "step",
        .displayName = Str::ScCustomKindStep,
        .renderEditor =
            [](nlohmann::json &p) {
                directionCombo(p);
                countInput(p);
                ImGui::TextUnformatted(Str::ScCustomGranularity);
                const char *gran[] = {Str::ScCustomGranFrame.id("gran_frame"),
                                      Str::ScCustomGranAction.id("gran_action"),
                                      Str::ScCustomGranActionAll.id("gran_actionall")};
                StepGranularity g = StepGranularity::Frame;
                granularityFromString(p.value("granularity", std::string{"frame"}), g);
                int gi = static_cast<int>(g);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("###custom_gran", &gi, gran, IM_ARRAYSIZE(gran)))
                    p["granularity"] = granularityToString(static_cast<StepGranularity>(gi));
            },
        .build =
            [&appSettings](const CustomCommand &def) { // Step reads no project state
                const auto dir = static_cast<StepDirection>(def.params.value("direction", 1));
                const int reps = std::max(1, def.params.value("reps", 1));
                StepGranularity gran = StepGranularity::Frame;
                granularityFromString(def.params.value("granularity", std::string{"frame"}), gran);
                const Labels l = labels(def, stepSummary(def.params));
                return holdCommand(
                    def.id, l.title, l.subtitle, appSettings,
                    [dir, reps, gran](EventQueue &eq, int burst, bool /*first*/) {
                        eq.push(StepRequestEvent{.direction = dir, .reps = reps * burst, .granularity = gran});
                    });
            },
        .summary = stepSummary,
    });

    // ── Move-position ── signed nudge of the active axis selection; the sign is the direction.
    registry.add(CustomCommandTemplate{
        .key = "move-position",
        .displayName = Str::ScCustomKindMovePos,
        .renderEditor =
            [](nlohmann::json &p) {
                ImGui::TextUnformatted(Str::ScCustomAmount);
                ImGui::SetNextItemWidth(-FLT_MIN);
                int delta = p.value("delta", 1);
                if (ImGui::InputInt("###custom_delta", &delta))
                    p["delta"] = delta;
            },
        .build =
            [&project, &appSettings](const CustomCommand &def) {
                const int delta = def.params.value("delta", 1);
                const Labels l = labels(def, movePosSummary(def.params));
                return holdCommand(
                    def.id, l.title, l.subtitle, appSettings, [delta, &project](EventQueue &eq, int burst, bool first) {
                        const auto role = project.state.activeAxis;
                        if (role >= StandardAxis::Count)
                            return;
                        // first → gesture boundary (snapshot); a later hold fire continues
                        // the same undo step.
                        eq.push(EditRequestEvent{
                            .intent = {.kind = EditIntentKind::MoveSelection, .axis = role, .pos = delta * burst},
                            .gesture = first ? GesturePhase::Begin : GesturePhase::Continue});
                    });
            },
        .summary = movePosSummary,
    });

    // ── Move-time ── shift the active axis selection in time; optionally seek the playhead after.
    registry.add(CustomCommandTemplate{
        .key = "move-time",
        .displayName = Str::ScCustomKindMoveTime,
        .renderEditor =
            [](nlohmann::json &p) {
                directionCombo(p);
                countInput(p);
                bool seek = p.value("seekAfter", false);
                if (ImGui::Checkbox(Str::ScCustomSeekAfter.id("custom_seek"), &seek))
                    p["seekAfter"] = seek;
            },
        .build =
            [&project, &appSettings](const CustomCommand &def) {
                const auto dir = static_cast<StepDirection>(def.params.value("direction", 1));
                const int reps = std::max(1, def.params.value("reps", 1));
                const bool seekAfter = def.params.value("seekAfter", false);
                const Labels l = labels(def, moveTimeSummary(def.params));
                return holdCommand(
                    def.id, l.title, l.subtitle, appSettings,
                    [dir, reps, seekAfter, &project](EventQueue &eq, int burst, bool first) {
                        const auto role = project.state.activeAxis;
                        if (role >= StandardAxis::Count)
                            return;
                        eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                                            .axis = role,
                                                            .direction = dir,
                                                            .reps = reps * burst,
                                                            .seekAfter = seekAfter},
                                                 .gesture = first ? GesturePhase::Begin : GesturePhase::Continue});
                    });
            },
        .summary = moveTimeSummary,
    });
}

} // namespace ofs
