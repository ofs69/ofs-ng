#include "Services/SelectIntentRouter.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Services/ModeLifecycle.h"
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Services/SelectionModeRegistry.h"
#include "Util/Log.h"

#include <vector>

namespace ofs {

namespace {
// The axes a selection fans across, and the editable-skip — a two-function mirror of the helpers in
// ProjectManager, duplicated to keep the selection resolver self-contained (as NavigatorRouter mirrors
// its tempo helper). Only the lead-targeted case fans out across the active group; a request naming a
// non-lead axis (a test / a plugin targeting one axis) stays single-axis.
AxisRoles editTargets(const ScriptProject &p, StandardAxis evAxis) {
    if (evAxis == p.state.activeAxis)
        return p.effectiveEditSet();
    AxisRoles r;
    if (evAxis < StandardAxis::Count)
        r.set(static_cast<size_t>(evAxis));
    return r;
}

template <class F> void forEachEditable(ScriptProject &p, AxisRoles roles, F &&fn) {
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!roles.test(i))
            continue;
        auto &ax = p.axes[i];
        if (ax.isLocked)
            continue;
        fn(static_cast<StandardAxis>(i), ax);
    }
}

// The built-in candidate set for one axis under a gesture — what the native mode (and a plugin mode's
// Pass) selects. All takes the whole axis; Box/Point take every action with time in
// [startTime,endTime] (pos ignored). A transposed range yields
// nothing (iterating lowerBound past upperBound is UB; the UI always sends an ordered range, but don't
// trust the caller). Point sends startTime == endTime, so a click never trips the guard.
VectorSet<ScriptAxisAction> nativeCandidates(const AxisState &axis, const SelectRequestEvent &event) {
    if (event.gesture == SelectGesture::All)
        return axis.actions;
    if (event.startTime > event.endTime)
        return {};
    auto itStart = axis.actions.lowerBound(ScriptAxisAction{event.startTime, 0});
    auto itEnd = axis.actions.upperBound(ScriptAxisAction{event.endTime, 0});
    return {itStart, itEnd};
}

// Resolve plugin-emitted {at,pos} pairs back to the real actions on the axis. An emitted action whose
// `at` names no point on the axis is silently ignored (a mode can only select existing actions, never
// invent them — the boundary stays unmisusable). `pos` is not used for lookup since `at` is the
// unique VectorSet key; the host always recovers the canonical {at,pos} from the axis.
VectorSet<ScriptAxisAction> resolveEmittedActions(const AxisState &axis, const std::vector<PluginAction> &actions) {
    VectorSet<ScriptAxisAction> out;
    for (const PluginAction &a : actions)
        if (auto it = axis.actions.find(ScriptAxisAction{a.at, 0}); it != axis.actions.end())
            out.insert(*it);
    return out;
}

// The host-owned sink a selection mode's onSelect emits kept actions into; collected into a vector the
// router resolves after the callback returns (the plugin copies each in during the call).
void collectEmitted(void *sink, PluginAction action) {
    static_cast<std::vector<PluginAction> *>(sink)->push_back(action);
}
} // namespace

SelectIntentRouter::SelectIntentRouter(ScriptProject &project, EventQueue &eq, SelectionModeRegistry &registry)
    : project(project), eq(eq), registry(registry) {
    eq.on<SelectRequestEvent>([this](const SelectRequestEvent &e) { onSelectRequest(e); });
    eq.on<SetActiveSelectionModeEvent>([this](const SetActiveSelectionModeEvent &e) { onSetActiveSelectionMode(e); });
    eq.on<RegisterSelectionModeEvent>([this](const RegisterSelectionModeEvent &e) { onRegisterSelectionMode(e); });
    eq.on<UnregisterSelectionModesEvent>(
        [this](const UnregisterSelectionModesEvent &e) { onUnregisterSelectionModes(e); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &e) { onProjectLoaded(e); });
}

void SelectIntentRouter::onSelectRequest(const SelectRequestEvent &event) {
    // The native selection mode (and an unknown active id — a weak reference whose plugin has gone)
    // resolves through the built-in per-axis candidate set. A plugin mode with an onSelect resolver is
    // consulted once per editable axis; null onSelect (native) falls through to the built-in candidates.
    const SelectionModeEntry *mode = registry.find(project.activeSelectionMode);
    resolve(event, mode);
}

void SelectIntentRouter::resolve(const SelectRequestEvent &event, const SelectionModeEntry *mode) {
    // A transposed range (startTime > endTime) names no valid region: leave the selection untouched
    // entirely (don't consult the mode, don't clear). Box/Point only — All has no range. (Point sends
    // startTime == endTime, so a click never trips this.)
    if (event.gesture != SelectGesture::All && event.startTime > event.endTime)
        return;

    // Apply the gesture's candidate set to one axis: replace the selection, or — when Ctrl is held —
    // toggle the candidates into the current selection. Host-owned combine, below the seam, so a mode
    // never reasons about it.
    auto apply = [&](StandardAxis role, AxisState &axis, const VectorSet<ScriptAxisAction> &candidates) {
        if (!event.additive) {
            project.setSelection(role, candidates, eq);
            return;
        }
        VectorSet<ScriptAxisAction> newSel = axis.selection;
        for (const auto &c : candidates) {
            if (newSel.contains(c))
                newSel.erase(c);
            else
                newSel.insert(c);
        }
        project.setSelection(role, newSel, eq);
    };

    const OfsSelectFn onSelect = mode ? mode->onSelect : nullptr;
    forEachEditable(project, editTargets(project, event.axis), [&](StandardAxis role, AxisState &axis) {
        if (onSelect == nullptr) { // native mode (or a null-resolver entry) → built-in candidates
            apply(role, axis, nativeCandidates(axis, event));
            return;
        }
        // Per-axis plugin consult: the mode enumerates its own candidates (the same read surface a script
        // node has) and emits the times it keeps. The request carries this axis as the lead so the mode
        // sees one axis at a time; the additive combine is the host's (applied in apply()), not exposed.
        const OfsSelectRequest in{.gesture = static_cast<int>(event.gesture),
                                  .axis = static_cast<int>(role),
                                  .startTime = event.startTime,
                                  .endTime = event.endTime,
                                  .pos = event.pos};
        std::vector<PluginAction> emitted;
        switch (mode->onSelect(mode->userData, &in, &collectEmitted, &emitted)) {
        case OfsSelectDrop:
            apply(role, axis, {}); // select nothing on this axis
            break;
        case OfsSelectReplace:
            apply(role, axis, resolveEmittedActions(axis, emitted)); // exactly the named actions
            break;
        case OfsSelectPass:
        default:
            apply(role, axis, nativeCandidates(axis, event)); // native candidates (any unknown → Pass)
            break;
        }
    });
}

void SelectIntentRouter::onSetActiveSelectionMode(const SetActiveSelectionModeEvent &event) {
    // The footer selector is the only writer of activeSelectionMode/storedSelectionMode (ModeLifecycle.h).
    mode_lifecycle::setActive(registry, project.activeSelectionMode, project.storedSelectionMode, event.id);
}

void SelectIntentRouter::onRegisterSelectionMode(const RegisterSelectionModeEvent &event) {
    // Sole owner of the registry's plugin entries: publish the mode so the footer lists it and a later
    // user selection can activate it. Registration never activates it (no plugin-callable setter).
    registry.add(event.entry);
}

void SelectIntentRouter::onUnregisterSelectionModes(const UnregisterSelectionModesEvent &event) {
    mode_lifecycle::unregisterPlugin(registry, project.activeSelectionMode, event.pluginName, kNativeSelectionModeId);
}

void SelectIntentRouter::onProjectLoaded(const LoadProjectEvent &) {
    mode_lifecycle::onProjectLoaded(registry, project.activeSelectionMode, project.storedSelectionMode,
                                    kNativeSelectionModeId, "Selection mode");
}

} // namespace ofs
