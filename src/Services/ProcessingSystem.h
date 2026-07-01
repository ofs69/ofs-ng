#pragma once

#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/StandardAxis.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/ScriptRegistry.h"

namespace ofs {

class EventQueue;
struct ScriptProject;
struct AxisState;

class ProcessingSystem {
  public:
    ProcessingSystem(ScriptProject &project, const EffectRegistryState &effectReg, const ScriptRegistryState &scriptReg,
                     EventQueue &eq, JobSystem &jobSystem);

    // Flush the axes marked for evaluation during this frame's drain — one snapshot + job submit per axis,
    // however many AxisModifiedEvents (or a manual RequestAxisEval) touched it. Call once per frame, after
    // drain(). The debounce collapses a multi-mutation gesture / multi-axis edit / project-load fan-out
    // into a single eval per axis instead of one per event. An axis whose previous eval is still in flight
    // is left marked and re-submitted only after that job completes — a throttle that lets periodic results
    // through during a sustained edit stream rather than cancelling and resubmitting every frame.
    void update();

  private:
    void onAxisModified(const AxisModifiedEvent &event);
    void onRequestEval(const RequestAxisEvalEvent &event);
    void onSetAutoEvalEnabled(const SetAutoEvalEnabledEvent &event);
    void onEvalComplete(const EvalCompleteEvent &event);
    void onProjectLoaded(const LoadProjectEvent &event);

    // Cancel any in-flight job for the axis and submit a fresh evaluation. Shared by the auto-eval
    // (AxisModifiedEvent) and manual (RequestAxisEvalEvent) paths.
    void evaluateAxis(StandardAxis role);

    // Resolve every dynamic node (script + plugin) in the snapshot's regions to its call ref on the main
    // thread, populating snap.nodeRefs and value-capturing each state-bearing plugin node's TState.
    // Returns the capture generation grouping this eval's captures (-1 if none), for later release.
    int buildNodeRefs(AxisSnapshot &snap);

    // Cancel + release the captures of an axis's in-flight job and clear its pendingEval. Used by the
    // supersede (evaluateAxis) and halt (onSetAutoEvalEnabled) paths so a job that never completes
    // does not leak its TState captures.
    void abandonEval(AxisState &axis) const;
    // Drop the TState captures named by `generation` (no-op for -1 or when no managed codec is wired).
    void releaseCaptures(int generation) const;

    ScriptProject &project;
    const EffectRegistryState &effectReg;
    const ScriptRegistryState &scriptReg;
    EventQueue &eq;
    JobSystem &jobSystem;

    // Axes marked for re-evaluation, flushed by update(). Coalesces the eval so several modifications to
    // the same axis in one frame build only one snapshot; an axis whose prior eval is still in flight stays
    // set across frames (the throttle) until that job completes. Main-thread only.
    AxisRoles pendingEvalAxes_;

    // Monotonic id handed to each eval's TState captures so they can be released as a group. Never
    // reused; -1 is reserved for "no captures". Main-thread only.
    int nextCaptureGeneration_ = 1;
};

void registerNativeEffects(EffectRegistryState &reg);

} // namespace ofs
