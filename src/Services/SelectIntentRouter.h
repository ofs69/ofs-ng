#pragma once

namespace ofs {

class EventQueue;
class SelectionModeRegistry;
struct SelectionModeEntry;
struct ScriptProject;
struct SelectRequestEvent;
struct SetActiveSelectionModeEvent;
struct RegisterSelectionModeEvent;
struct UnregisterSelectionModesEvent;
struct LoadProjectEvent;

// Sole subscriber to SelectRequestEvent — the selection interception seam. It resolves a selection
// gesture (marquee / Ctrl+A / click) into a per-axis selection and applies it through
// project.setSelection(). The active selection mode (ScriptProject::activeSelectionMode) selects the
// resolver; the built-in `native` resolver selects every action the gesture covers — Box/Point select
// every action in the time range (pos ignored), All selects the whole axis. The per-axis group fan-out
// (editTargets / forEachEditable) and the host-owned `additive` combine live here, below the seam.
//
// Selection group fan-out is *always* per-axis (a box over a group is independently meaningful on each
// axis's own actions; a selection has no sensible lead-projection), so unlike the edit seam there is no
// disposition to choose. A plugin selection mode is consulted once per editable axis: it enumerates its
// own candidates and emits the action times it keeps (Pass → native candidates, Drop → none, Replace →
// the emitted ones), which the host resolves back to actions and applies through setSelection().
//
// The effective id is a weak reference: when its plugin is absent (foreign file / uninstalled on load,
// or disabled/unloaded/reloaded/crashed at runtime) it falls back to native, while the stored authored
// id (ScriptProject::storedSelectionMode) is preserved for a re-save.
class SelectIntentRouter {
  public:
    SelectIntentRouter(ScriptProject &project, EventQueue &eq, SelectionModeRegistry &registry);

  private:
    void onSelectRequest(const SelectRequestEvent &event);
    void onSetActiveSelectionMode(const SetActiveSelectionModeEvent &event);
    void onRegisterSelectionMode(const RegisterSelectionModeEvent &event);
    void onUnregisterSelectionModes(const UnregisterSelectionModesEvent &event);
    void onProjectLoaded(const LoadProjectEvent &event);

    // Per-axis resolve: native built-in candidate set, or the active plugin mode's onSelect, applied
    // through the host-owned additive combine. `onSelect` is null for the native mode.
    void resolve(const SelectRequestEvent &event, const SelectionModeEntry *mode);

    ScriptProject &project;
    EventQueue &eq;
    SelectionModeRegistry &registry;
};

} // namespace ofs
