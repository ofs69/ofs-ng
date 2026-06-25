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

namespace ofs {

namespace {

// Symbolic enum names — stable across builds, unlike the integer values (the bindings.json rule, CP
// §16.2). An unknown string makes readParams skip the whole entry (forward-tolerant).
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

// Wrap a per-kind emitter (eq, burst, first) into a holdable Command. The built-in step/move verbs inline
// this same run/tick pairing; a custom command differs only in supplying user-chosen params. `burst` is
// the held-repeat coalesce count (1 on a single press), so the authored amount is the base, scaled by N.
// The cadence is read live each tick (appSettings.holdRepeat), identical to OfsApp::holdRepeatParams(),
// so a Shortcut-window edit applies instantly and every hold binding shares one tunable feel.
template <class Emit> Command holdCommand(const CustomCommand &def, const AppSettings &appSettings, const Emit &step) {
    Command c;
    c.id = def.id;
    c.group = "Custom";
    c.title = def.name; // owned std::string (TrString) — a user title, never run through the catalog
    c.source = CommandSource::Custom;
    c.run = [step](EventQueue &eq) { step(eq, 1, true); };
    c.tick = [step, &appSettings](EventQueue &eq, const HoldTickInfo &info) {
        const HoldRepeatParams hp{.initialDelay = appSettings.holdRepeat.initialDelay,
                                  .interval = appSettings.holdRepeat.interval,
                                  .accel = appSettings.holdRepeat.accel};
        const int burst = holdRepeats(info.elapsed, info.dt, hp);
        if (burst > 0)
            step(eq, burst, info.first);
    };
    return c;
}

// Direction (Forward/Backward) and Count are shared by the Step and Move-time editors.
void directionCombo(CustomCommand &d) {
    ImGui::TextUnformatted(Str::ScCustomDirection);
    const char *items[] = {Str::ScCustomDirForward.id("dir_fwd"), Str::ScCustomDirBackward.id("dir_back")};
    int idx = d.direction == StepDirection::Backward ? 1 : 0;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("###custom_dir", &idx, items, IM_ARRAYSIZE(items)))
        d.direction = idx == 1 ? StepDirection::Backward : StepDirection::Forward;
}
void countInput(CustomCommand &d) {
    ImGui::TextUnformatted(Str::ScCustomCount);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputInt("###custom_reps", &d.reps))
        d.reps = std::max(1, d.reps);
}

} // namespace

void registerBuiltinCommandTemplates(CustomCommandTemplateRegistry &registry, ScriptProject &project,
                                     const AppSettings &appSettings) {
    // ── Step ── navigation only; no undo, no gesture. The navigator ignores reps for ActionAllAxes.
    registry.add(CustomCommandTemplate{
        .key = "step",
        .displayName = Str::ScCustomKindStep,
        .renderEditor =
            [](CustomCommand &d) {
                directionCombo(d);
                countInput(d);
                ImGui::TextUnformatted(Str::ScCustomGranularity);
                const char *gran[] = {Str::ScCustomGranFrame.id("gran_frame"),
                                      Str::ScCustomGranAction.id("gran_action"),
                                      Str::ScCustomGranActionAll.id("gran_actionall")};
                int gi = static_cast<int>(d.granularity);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("###custom_gran", &gi, gran, IM_ARRAYSIZE(gran)))
                    d.granularity = static_cast<StepGranularity>(gi);
            },
        .build =
            [&appSettings](const CustomCommand &def) { // Step reads no project state
                return holdCommand(def, appSettings, [def](EventQueue &eq, int burst, bool /*first*/) {
                    eq.push(StepRequestEvent{
                        .direction = def.direction, .reps = def.reps * burst, .granularity = def.granularity});
                });
            },
        .writeParams =
            [](const CustomCommand &c, nlohmann::json &e) {
                e["direction"] = static_cast<int>(c.direction);
                e["reps"] = c.reps;
                e["granularity"] = granularityToString(c.granularity);
            },
        .readParams =
            [](const nlohmann::json &e, CustomCommand &c) {
                c.direction = static_cast<StepDirection>(e.value("direction", 1));
                c.reps = std::max(1, e.value("reps", 1));
                if (e.contains("granularity") &&
                    !granularityFromString(e["granularity"].get<std::string>(), c.granularity))
                    return false; // present but unknown granularity — skip
                return true;
            },
    });

    // ── Move-position ── signed nudge of the active axis selection; the sign is the direction.
    registry.add(CustomCommandTemplate{
        .key = "move-position",
        .displayName = Str::ScCustomKindMovePos,
        .renderEditor =
            [](CustomCommand &d) {
                ImGui::TextUnformatted(Str::ScCustomAmount);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputInt("###custom_delta", &d.delta);
            },
        .build =
            [&project, &appSettings](const CustomCommand &def) {
                return holdCommand(def, appSettings, [def, &project](EventQueue &eq, int burst, bool first) {
                    const auto role = project.state.activeAxis;
                    if (role >= StandardAxis::Count)
                        return;
                    // first → gesture boundary (snapshot); a later hold fire continues the same undo step.
                    eq.push(EditRequestEvent{
                        .intent = {.kind = EditIntentKind::MoveSelection, .axis = role, .pos = def.delta * burst},
                        .gesture = first ? GesturePhase::Begin : GesturePhase::Continue});
                });
            },
        .writeParams = [](const CustomCommand &c, nlohmann::json &e) { e["delta"] = c.delta; },
        .readParams =
            [](const nlohmann::json &e, CustomCommand &c) {
                c.delta = e.value("delta", 1);
                return true;
            },
    });

    // ── Move-time ── shift the active axis selection in time; optionally seek the playhead after.
    registry.add(CustomCommandTemplate{
        .key = "move-time",
        .displayName = Str::ScCustomKindMoveTime,
        .renderEditor =
            [](CustomCommand &d) {
                directionCombo(d);
                countInput(d);
                ImGui::Checkbox(Str::ScCustomSeekAfter.id("custom_seek"), &d.seekAfter);
            },
        .build =
            [&project, &appSettings](const CustomCommand &def) {
                return holdCommand(def, appSettings, [def, &project](EventQueue &eq, int burst, bool first) {
                    const auto role = project.state.activeAxis;
                    if (role >= StandardAxis::Count)
                        return;
                    eq.push(EditRequestEvent{.intent = {.kind = EditIntentKind::MoveSelection,
                                                        .axis = role,
                                                        .direction = def.direction,
                                                        .reps = def.reps * burst,
                                                        .seekAfter = def.seekAfter},
                                             .gesture = first ? GesturePhase::Begin : GesturePhase::Continue});
                });
            },
        .writeParams =
            [](const CustomCommand &c, nlohmann::json &e) {
                e["direction"] = static_cast<int>(c.direction);
                e["reps"] = c.reps;
                e["seekAfter"] = c.seekAfter;
            },
        .readParams =
            [](const nlohmann::json &e, CustomCommand &c) {
                c.direction = static_cast<StepDirection>(e.value("direction", 1));
                c.reps = std::max(1, e.value("reps", 1));
                c.seekAfter = e.value("seekAfter", false);
                return true;
            },
    });
}

} // namespace ofs
