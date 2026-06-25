#pragma once

#include "Core/Events.h"
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

  private:
    void onAxisModified(const AxisModifiedEvent &event);
    void onRequestEval(const RequestAxisEvalEvent &event);
    void onSetAutoEvalEnabled(const SetAutoEvalEnabledEvent &event);
    void onEvalComplete(const EvalCompleteEvent &event);
    void onProjectLoaded(const LoadProjectEvent &event);

    // Cancel any in-flight job for the axis and submit a fresh evaluation. Shared by the auto-eval
    // (AxisModifiedEvent) and manual (RequestAxisEvalEvent) paths.
    void evaluateAxis(StandardAxis role);

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

    // Monotonic id handed to each eval's TState captures so they can be released as a group. Never
    // reused; -1 is reserved for "no captures". Main-thread only.
    int nextCaptureGeneration_ = 1;
};

void registerNativeEffects(EffectRegistryState &reg);

} // namespace ofs
