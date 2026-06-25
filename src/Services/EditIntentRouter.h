#pragma once

#include <vector>

namespace ofs {

class EventQueue;
class EditModeRegistry;
struct ScriptProject;
struct EditIntent;
struct EditModeEntry;
struct EditRequestEvent;
struct SetActiveEditModeEvent;
struct RegisterEditModeEvent;
struct UnregisterEditModesEvent;
struct LoadProjectEvent;
enum class GesturePhase;

// Sole subscriber to EditRequestEvent — the editing interception seam. It consults the active edit
// mode and emits the existing mutation events the intent resolves to (AddActionAtTimeEvent,
// MoveActionEvent, …), which ProjectManager and UndoSystem still consume unchanged. The active mode
// (ScriptProject::activeEditMode) selects the resolver; the built-in `native` resolver applies each
// intent verbatim — equivalent to pushing the mutation event directly.
//
// When a plugin edit mode is active, the router marshals the intent across the C ABI, calls the mode's
// onEditIntent, and resolves the result: Pass → resolve the original natively; Drop → nothing; Replace
// → resolve each emitted intent natively (projected across the active group below the seam);
// ReplacePerAxis → apply the lead's emitted intents to the lead alone, then re-consult the mode once per
// remaining editable axis and apply each verbatim (fan-out above the seam). A native mode (or an unknown
// active id) resolves directly.
//
// The router owns the per-gesture snapshot latch: it stamps snapshot=true on the first mutation of a
// Begin/OneShot gesture and false on every later mutation of the same gesture, so a drag (or a held
// repeat) collapses into one undo step regardless of how many mutations it produced — including the
// several a single Replace emits. The effective id is a weak reference: when its plugin is absent
// (foreign file / uninstalled on load, or disabled/unloaded/reloaded/crashed at runtime) it falls back
// to native, while the stored authored id (ScriptProject::storedEditMode) is preserved for a re-save.
class EditIntentRouter {
  public:
    EditIntentRouter(ScriptProject &project, EventQueue &eq, EditModeRegistry &registry);

  private:
    void onEditRequest(const EditRequestEvent &event);
    void onSetActiveEditMode(const SetActiveEditModeEvent &event);
    void onRegisterEditMode(const RegisterEditModeEvent &event);
    void onUnregisterEditModes(const UnregisterEditModesEvent &event);
    void onProjectLoaded(const LoadProjectEvent &event);

    // Consult a plugin mode with `consult` and resolve the result; a Pass applies `nativeFallback` (the two
    // differ only for a single-action MoveSelection, where the mode sees a MovePoint but Pass resolves the
    // original nudge so native behavior is byte-identical).
    void consultMode(const EditModeEntry &mode, const EditIntent &consult, const EditIntent &nativeFallback);

    // True when a MoveSelection nudge moves exactly one action on its lead axis (one selected, or — with no
    // selection — the action nearest the playhead). Such a nudge is presented to the mode as a MovePoint.
    bool isSingleActionNudge(const EditIntent &intent) const;

    // Build the MovePoint a single-action MoveSelection resolves to (the selected/nearest action, shifted by
    // the host-owned step for a time nudge or clamped for a position nudge).
    EditIntent synthesizeSingleMove(const EditIntent &intent) const;

    // Native resolve: emit the mutation event(s) the intent maps to. AddPointAtPlayhead decomposes
    // here (compute the cursor time, emit AddActionAtTimeEvent) rather than via a second request.
    // `fanToGroup` is stamped onto the emitted mutation; the router passes false only on the
    // ReplacePerAxis path, where it has already fanned across the group itself (see onEditRequest).
    void resolveNative(const EditIntent &intent, bool fanToGroup = true);

    // ReplacePerAxis fan-out (above the seam): apply the lead's already-resolved intents to the lead axis
    // alone, then re-consult the mode once per remaining editable axis (the request retargeted) and apply
    // each verbatim. Every mutation is single-axis (the router has fanned), so ProjectManager does not
    // re-project. ABI marshaling stays in the .cpp; this takes host intents only.
    void resolvePerAxis(const EditModeEntry &mode, const EditIntent &leadIntent,
                        const std::vector<EditIntent> &leadEmitted);

    // Read-and-clear the snapshot latch armed at the current gesture's boundary.
    bool consumeSnapshot();

    ScriptProject &project;
    EventQueue &eq;
    EditModeRegistry &registry;

    // True between a gesture's boundary (Begin/OneShot) and the first mutation it yields; that mutation
    // takes snapshot=true and clears the latch, so the rest of the gesture coalesces.
    bool snapshotPending_ = false;
};

} // namespace ofs
