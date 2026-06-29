#pragma once

#include "Core/IntentEvents.h"     // StepDirection
#include "Core/ProcessingRegion.h" // AxisRoles
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"

#include <memory>

namespace ofs {

struct EvalJob; // forward declaration for EvalCompleteEvent

struct AxisSelectedEvent {
    StandardAxis role;
};

// Set the multi-axis editing group (timeline strip Ctrl-click / drag). `roles` is the desired group;
// an empty/single-bit `roles` dissolves it back to single-axis editing. The handler keeps activeAxis
// (the lead) coherent with the group. Not undoable — grouping is transient view state.
struct SetAxisGroupingEvent {
    AxisRoles roles;
    StandardAxis lead; // becomes activeAxis
};

struct AxisModifiedEvent {
    StandardAxis role;
};

// Manual recompute request (processing panel "Recompute" button). Forces re-evaluation of the axis
// even while auto-eval is halted; unlike AxisModifiedEvent it carries no document mutation.
struct RequestAxisEvalEvent {
    StandardAxis role;
};

// Toggle automatic re-evaluation on axis edits. Transient session state (ScriptProject::autoEvalEnabled).
struct SetAutoEvalEnabledEvent {
    bool enabled;
};

struct SelectionChangedEvent {
    StandardAxis role;
};

struct UndoEvent {};
struct RedoEvent {};

// Emitted by UndoSystem after an undo/redo *actually* navigated the history (the stack was non-empty and
// the document was restored). A no-op press at the end of a stack emits nothing. Lets observers (the
// audio-feedback cue) react to the outcome instead of the UndoEvent/RedoEvent *request*, which fires even
// when there is nothing to undo. `redo` gives the direction. Derived/informational only — not undoable.
struct HistoryNavigatedEvent {
    bool redo = false;
};

// Emitted by ProjectManager when an axis's panel/strip presence or timeline-row visibility *actually*
// flips. The toggle/preset handlers can no-op (L0 is always in-panel; a bulk show/hide preset may find
// nothing to change; all scratch slots may already be full), so observers (the audio-feedback cue) react
// to this outcome instead of the AddScratchAxis/RemoveAxis/Toggle*/Show* *request*. One per logical
// gesture — a bulk show/hide preset emits a single aggregate change, not one per axis it touches.
// Derived/informational only — not undoable (the originating edit, where there is one, already is).
enum class AxisPresence { AddedToStrip, RemovedFromStrip, Shown, Hidden };
struct AxisPresenceChangedEvent {
    AxisPresence change;
};

// Emitted by ProjectManager when the multi-axis edit group is *actually* formed or changed to a different
// multi-axis set. Re-issuing the same group, or a request that resolves to plain single-axis editing,
// emits nothing — so observers (the audio-feedback cue) chirp only on a real grouping, not on every
// SetAxisGroupingEvent *request* (which also fires to dissolve a group). Transient view state, not undoable.
struct AxisGroupingChangedEvent {};

struct RemoveSelectedActionsEvent {
    StandardAxis axis;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
    bool snapshot = true;   // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct AddActionAtTimeEvent {
    StandardAxis axis;
    double time;
    int pos;
    // Default true ⇒ a normal single add captures its own pre-edit undo snapshot. When an edit mode
    // resolves one gesture into several adds (e.g. Ofs.Core's Shaped Approach), the router sets this
    // true only on the first and false on the rest, so UndoSystem captures once and the whole batch
    // coalesces into a single undo step instead of one per injected point.
    bool snapshot = true;
    // Default true ⇒ an active-axis mutation projects across the active edit group below the seam
    // (ProjectManager fans it via editTargets/effectiveEditSet). The EditIntentRouter sets it false only
    // for a ReplacePerAxis resolution, where it has already consulted the mode once per editable axis and
    // each axis's intents must apply verbatim to that axis alone — no mechanical projection.
    bool fanToGroup = true;
};

struct MoveSelectionPositionEvent {
    StandardAxis axis;
    int delta;
    // A held-repeat burst coalesces into one event; only the first fire of the hold snapshots undo, so
    // the whole hold is a single undo step. The caller pre-scales `delta` by the burst's repeat count
    // (delta is already a signed magnitude). Default true ⇒ every other caller (single tap, menu) snapshots.
    bool snapshot = true;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

struct MoveSelectionTimeEvent {
    StandardAxis axis;
    StepDirection direction; // Forward / Backward
    bool seekAfter;
    // Number of step units to move in one event (a held-repeat burst collapses N fires into reps=N).
    // delta = direction * reps * stepTime — one mutate and one collision check at the final position.
    int reps = 1;
    bool snapshot = true;   // see MoveSelectionPositionEvent::snapshot
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

struct MoveActionToCurrentTimeEvent {
    StandardAxis axis;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

// Copy the current selection of every axis in the active group into ProjectManager's clipboard. No
// mutation (not undoable). Cut = this followed by RemoveSelectedActionsEvent.
struct CopySelectionEvent {};

// Paste ProjectManager's clipboard. A one-axis clipboard is broadcast across the active group; a
// multi-axis clipboard pastes each clip back onto its originating role. exact = at original times.
struct PasteActionsEvent {
    double pasteTime;
    bool exact;
    bool snapshot = true; // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct RemoveActionAtTimeEvent {
    StandardAxis axis;
    double time;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
    bool snapshot = true;   // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct MoveActionEvent {
    StandardAxis axis;
    double fromAt;
    double toAt;
    int toPos;
    bool snapshot = true;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

// Axis management
struct AddScratchAxisEvent {};

struct RemoveAxisEvent {
    StandardAxis axisRole;
};

struct ToggleAxisVisibilityEvent {
    StandardAxis axisRole;
    bool visible;
};

struct ToggleAxisLockEvent {
    StandardAxis axisRole;
    bool locked;
};

struct ToggleAxisPanelVisibilityEvent {
    StandardAxis axisRole;
    bool inPanel;
};

// QOL: make the six main TCode axes (L0–R2) present in one step, so a project that started with only L0
// is ready for multi-axis editing. Not-yet-present axes are created with default flags; already-present
// ones are left untouched. Pushed by the Axes menu "Multi-Axis" item.
struct ShowMultiAxisEvent {};

// QOL counterpart to ShowMultiAxisEvent: collapse the strip back to just L0, hiding every other axis
// from the panel (and dropping them from any edit group). L0 itself is forced visible. Data is never
// cleared: a scratch axis that holds actions still exists() and stays in the Axes menu (re-show via
// "Show in Panel"); an empty scratch axis simply ceases to exist. Pushed by the Axes menu "Show L0
// Only" item.
struct ShowL0OnlyEvent {};

struct EvalCompleteEvent {
    StandardAxis role;
    std::shared_ptr<EvalJob> job; // compared against AxisState::pendingEval for staleness
    VectorSet<ScriptAxisAction> resolvedActions;
    double evalMs = 0.0;
    bool hasResult = false; // false means no regions; resolvedActions is empty
};

struct CommitAxisActionsEvent {
    StandardAxis axis;
    VectorSet<ScriptAxisAction> actions;
};

// Replace an axis's selection wholesale (empty = clear). Deferred like CommitAxisActionsEvent so that a
// plugin's buffered AxisEdit applies atomically: the actions commit drains first, then this selects
// against the freshly-committed set — otherwise a selection of just-created points would be filtered out
// (setSelection drops times not yet in actions).
struct SetAxisSelectionEvent {
    StandardAxis axis;
    VectorSet<ScriptAxisAction> selection;
};

} // namespace ofs
