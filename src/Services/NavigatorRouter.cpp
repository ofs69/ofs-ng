#include "Services/NavigatorRouter.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/OverlaySettings.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Services/NavigatorRegistry.h"
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Util/Log.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace ofs {

namespace {
// Time-equality tolerance (seconds) for step resolution: treat the caret as "on" a grid line / action
// within this slop so a step always moves off it rather than snapping back to the current position.
constexpr double kStepEpsilon = 0.001;

// Frame channel: turn a Step into an absolute target by deferring to the current overlay. Always lands
// somewhere, so it returns a value.
double resolveFrameStep(const ScriptProject &project, const StepRequestEvent &event) {
    const auto &modeState = project.overlay;
    const double currentTime = project.playback.cursorPos;
    double nextTime = currentTime;

    const int reps = std::max(1, event.reps);
    if (modeState.overlay == ScriptingOverlay::Frame) {
        const double frameTime = 1.0 / static_cast<double>(modeState.frameFps);
        nextTime = currentTime + static_cast<double>(static_cast<int>(event.direction) * reps) * frameTime;
    } else if (modeState.overlay == ScriptingOverlay::Tempo) {
        const double beatTime = tempoBeatTime(modeState);
        const auto offset = static_cast<double>(modeState.tempoOffsetSeconds);
        // Snap reps grid lines away in the step direction, honoring the beat offset.
        const double idx = (currentTime - offset) / beatTime;
        double beatIdx = event.direction == StepDirection::Forward ? std::floor(idx) + reps : std::ceil(idx) - reps;
        nextTime = beatIdx * beatTime + offset;
        // If we landed exactly where we already are (caret was on a grid line), advance one more.
        if (std::abs(nextTime - currentTime) <= kStepEpsilon)
            nextTime += static_cast<double>(static_cast<int>(event.direction)) * beatTime;
    }
    return std::max(0.0, nextTime);
}

// Action channel: step to the reps-th adjacent action on the active axis. nullopt when there is no such
// action (empty axis / already at the end).
std::optional<double> resolveActionStep(const ScriptProject &project, const StepRequestEvent &event) {
    if (project.state.activeAxis >= StandardAxis::Count)
        return std::nullopt;
    const auto &axis = project.axes[static_cast<size_t>(project.state.activeAxis)];
    if (axis.actions.empty())
        return std::nullopt;

    const int reps = std::max(1, event.reps);
    if (event.direction == StepDirection::Forward) {
        auto it = axis.actions.upperBound(ScriptAxisAction{project.playback.cursorPos, 0});
        if (it == axis.actions.end())
            return std::nullopt;
        for (int k = 1; k < reps && std::next(it) != axis.actions.end(); ++k)
            ++it;
        return it->at;
    }
    auto it = axis.actions.lowerBound(ScriptAxisAction{project.playback.cursorPos - kStepEpsilon, 0});
    if (it == axis.actions.begin())
        return std::nullopt;
    --it;
    for (int k = 1; k < reps && it != axis.actions.begin(); ++k)
        --it;
    return it->at;
}

// ActionAllAxes channel: step to the single nearest adjacent action across every axis (reps ignored — a
// single nearest-across-axes step). Returns the landed time and the axis that owns it, so the caller can
// activate that axis. nullopt when no axis has an action in the step direction.
std::optional<std::pair<double, StandardAxis>> resolveActionStepAllAxes(const ScriptProject &project,
                                                                        const StepRequestEvent &event) {
    const double currentTime = project.playback.cursorPos;
    StandardAxis bestAxis = StandardAxis::Count;
    if (event.direction == StepDirection::Forward) {
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            auto it = project.axes[i].actions.upperBound(ScriptAxisAction{currentTime, 0});
            if (it != project.axes[i].actions.end() && it->at < best) {
                best = it->at;
                bestAxis = static_cast<StandardAxis>(i);
            }
        }
        if (bestAxis == StandardAxis::Count)
            return std::nullopt;
        return std::pair{best, bestAxis};
    }
    double best = -1.0;
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        auto it = project.axes[i].actions.lowerBound(ScriptAxisAction{currentTime - kStepEpsilon, 0});
        if (it != project.axes[i].actions.begin()) {
            --it;
            if (it->at > best) {
                best = it->at;
                bestAxis = static_cast<StandardAxis>(i);
            }
        }
    }
    if (bestAxis == StandardAxis::Count)
        return std::nullopt;
    return std::pair{best, bestAxis};
}

// The native navigator (follow-overlay): dispatch a Step to its channel's built-in resolution. Returns
// the landed time and the axis to activate. Frame/Action stepping keeps the active axis; ActionAllAxes
// lands on whichever axis owns the nearest action and reports it, so the caller can switch to it.
std::optional<std::pair<double, StandardAxis>> resolveNativeStep(const ScriptProject &project,
                                                                 const StepRequestEvent &event) {
    const StandardAxis active = project.state.activeAxis;
    switch (event.granularity) {
    case StepGranularity::Frame:
        return std::pair{resolveFrameStep(project, event), active};
    case StepGranularity::Action:
        if (const auto t = resolveActionStep(project, event))
            return std::pair{*t, active};
        return std::nullopt;
    case StepGranularity::ActionAllAxes:
        return resolveActionStepAllAxes(project, event); // already (time, owning axis)
    }
    return std::nullopt;
}

// Push the resolved Seek, and — when the step switches axes — an AxisSelectedEvent. Shared by the native
// resolution and a plugin navigator's Seek so both express "seek (and maybe activate an axis)" through
// one path. A multi-axis step lands on the axis owning the nearest action and activates it, so the user
// keeps editing the script they just stepped onto. onAxisSelected ignores an off-panel/out-of-range axis
// defensively, but skip the redundant event when the step stays on the active axis.
void pushSeek(EventQueue &eq, StandardAxis activeAxis, double time, StandardAxis axis) {
    eq.push(SeekEvent{std::max(0.0, time)});
    if (axis != activeAxis && axis < StandardAxis::Count)
        eq.push(AxisSelectedEvent{axis});
}
} // namespace

NavigatorRouter::NavigatorRouter(ScriptProject &project, EventQueue &eq, NavigatorRegistry &registry)
    : project(project), eq(eq), registry(registry) {
    eq.on<StepRequestEvent>([this](const StepRequestEvent &e) { onStepRequest(e); });
    eq.on<SetActiveNavigatorEvent>([this](const SetActiveNavigatorEvent &e) { onSetActiveNavigator(e); });
    eq.on<RegisterNavigatorEvent>([this](const RegisterNavigatorEvent &e) { onRegisterNavigator(e); });
    eq.on<UnregisterNavigatorsEvent>([this](const UnregisterNavigatorsEvent &e) { onUnregisterNavigators(e); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &e) { onProjectLoaded(e); });
}

void NavigatorRouter::pushNativeStep(const StepRequestEvent &event) {
    if (const auto r = resolveNativeStep(project, event))
        pushSeek(eq, project.state.activeAxis, r->first, r->second);
}

void NavigatorRouter::onStepRequest(const StepRequestEvent &event) {
    // The native follow-overlay navigator (and an unknown active id — a weak reference whose plugin has
    // gone) resolves through the built-in channel resolution. A plugin navigator resolves the Step
    // itself, or Passes a granularity it doesn't redefine back to the native resolution.
    const NavigatorEntry *nav = registry.find(project.activeNavigator);
    if (nav == nullptr || nav->onStep == nullptr) {
        pushNativeStep(event);
        return;
    }

    // Pre-seed the result axis with the current active axis so a navigator that only seeks (never touches
    // axis) leaves the active axis as-is; a navigator overwrites axis only to *switch* axes.
    const StandardAxis activeAxis = project.state.activeAxis;
    const OfsNavIntent step{.direction = static_cast<int>(event.direction),
                            .reps = std::max(1, event.reps),
                            .time = 0.0,
                            .granularity = static_cast<int>(event.granularity),
                            .axis = static_cast<int>(activeAxis)};
    OfsNavIntent out{.axis = static_cast<int>(activeAxis)};
    switch (nav->onStep(nav->userData, &step, &out)) {
    case OfsNavResultSeek:
        // Nav.Seek(time) → seek; Nav.Seek(time, axis) → also switch axes, reproducing the native
        // ActionAllAxes step through the same path.
        pushSeek(eq, activeAxis, out.time, static_cast<StandardAxis>(out.axis));
        break;
    case OfsNavResultPass:
        // The navigator declined this granularity; fall back to the native resolution for it.
        pushNativeStep(event);
        break;
    case OfsNavResultNone:
    default:
        break; // Nav.None → no movement
    }
}

void NavigatorRouter::onSetActiveNavigator(const SetActiveNavigatorEvent &event) {
    // The footer selector is the only writer of activeNavigator. Ignore an unknown id defensively (a
    // stale UI option after a plugin unloaded mid-frame) so stepping always resolves. A no-op
    // re-selection of the current navigator is harmless.
    if (event.id == project.activeNavigator || !registry.has(event.id))
        return;

    // Run the outgoing navigator's onExit then the incoming one's onEnter (a user switch, not the
    // plugin's initiative). The native follow-overlay entry has no callbacks. Order matters: exit-old
    // before enter-new.
    if (const NavigatorEntry *prev = registry.find(project.activeNavigator); prev && prev->onExit)
        prev->onExit(prev->userData);
    // A user pick updates both the effective and the stored (authored) id, so a save persists the new
    // selection rather than a previously fallen-back id.
    project.activeNavigator = event.id;
    project.storedNavigator = event.id;
    if (const NavigatorEntry *next = registry.find(event.id); next && next->onEnter)
        next->onEnter(next->userData);
}

void NavigatorRouter::onRegisterNavigator(const RegisterNavigatorEvent &event) {
    // Sole owner of the registry's plugin entries: publish the navigator so the footer lists it.
    // Registration never activates it (no plugin-callable setter).
    registry.add(event.entry);
}

void NavigatorRouter::onUnregisterNavigators(const UnregisterNavigatorsEvent &event) {
    // The plugin is unloading (disabled, unloaded, hot-reloaded, crashed). If its navigator is the active
    // one, fall the *effective* selection back to follow-overlay and run its onExit best-effort first;
    // leave the stored (authored) id intact so a re-save preserves it and a reload re-publishes the
    // navigator without silently re-activating it. onExit is a safe no-op once the managed slot is released
    // (the guard turns a post-teardown/crash callback into a fallback).
    if (const NavigatorEntry *active = registry.find(project.activeNavigator);
        active && active->owningPlugin == event.pluginName) {
        if (active->onExit)
            active->onExit(active->userData);
        project.activeNavigator = kFollowOverlayNavigatorId; // storedNavigator untouched — authored id preserved
    }
    registry.removeByPlugin(event.pluginName);
}

void NavigatorRouter::onProjectLoaded(const LoadProjectEvent &) {
    // Weak reference: a loaded project may name a navigator no loaded plugin registers (uninstalled
    // plugin, foreign file). Fall the *effective* id back to follow-overlay so stepping always resolves;
    // leave storedNavigator pointing at the authored id so a re-save preserves it and re-opening with the
    // plugin present restores it. The effective id is re-derived from the stored id each load.
    project.activeNavigator = project.storedNavigator;
    if (!registry.has(project.activeNavigator)) {
        OFS_CORE_INFO("Navigator '{}' is not registered; falling back to '{}' (authored id preserved).",
                      project.storedNavigator, kFollowOverlayNavigatorId);
        project.activeNavigator = kFollowOverlayNavigatorId;
    }
}

} // namespace ofs
